#include "VM.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "data/DataWin.h"
#include "runtime/BuiltinVars.h"
#include "runtime/Instance.h"
#include "runtime/Runner.h"

namespace gm {

static constexpr size_t kStackSize = 1 << 16;
static constexpr size_t kLocalPoolSize = 1 << 18;
static constexpr int kMaxDepth = 512;
double gCompareEpsilon = 0.00001;  // math_set_epsilon

VM::VM(Runner& rt) : rt_(rt) {}

static VM* gStringVM = nullptr;  // variable names for struct-to-string conversion

void VM::init() {
    const DataWin& dw = rt_.dw;
    gStringVM = this;
    gVarNameForString = [](int32_t id) { return gStringVM->varName(id); };
    gGms2Strings = dw.gen.gms23;  // booleans became their own type in GMS 2.3
    stack_.reset(new Value[kStackSize]);
    sizes_.reset(new uint8_t[kStackSize]);
    localPool_.reset(new Value[kLocalPoolSize]);
    compiled_.assign(dw.code.size(), CompiledCode());

    strings_.reserve(dw.strings.size());
    for (auto s : dw.strings) strings_.emplace_back(s);

    int32_t maxId = 0;
    vars_.reserve(dw.variables.size());
    for (auto& v : dw.variables) {
        VarInfo vi;
        vi.name = v.name;
        vi.id = v.varId;
        // Built-ins: varId -6, or (GMS2, e.g. argument0-3) instance type -6.
        if (v.varId == -6 || (v.instanceType == INST_BUILTIN && builtinVarId(v.name) >= 0)) {
            vi.builtin = builtinVarId(v.name);
            if (vi.builtin < 0 && !builtins_.count(std::string(v.name)) && v.name.rfind("time_source_", 0) != 0)
                warnOnce("bv:" + std::string(v.name), "unknown built-in variable " + std::string(v.name));
        } else if (v.instanceType != INST_LOCAL && v.varId >= 0) {
            maxId = std::max(maxId, v.varId);
            varIdByName_.emplace(std::string(v.name), v.varId);
        }
        vars_.push_back(vi);
    }
    varNameById_.assign(maxId + 1, std::string_view());
    for (auto& v : dw.variables)
        if (v.varId >= 0 && v.instanceType != INST_LOCAL) varNameById_[v.varId] = v.name;
    globals_.resize(maxId + 1);
    // Structs have no built-in variables: `s.x` is an ordinary member there.
    for (auto& vi : vars_)
        if (vi.builtin >= 0 && (vi.builtin <= BV_timeline_loop || vi.builtin == BV_layer)) vi.memberId = varIdForName(vi.name);

    for (size_t i = 0; i < dw.scripts.size(); i++) {
        scriptByName_[std::string(dw.scripts[i].name)] = dw.scripts[i].code;
        scriptIndexByName_[std::string(dw.scripts[i].name)] = static_cast<int32_t>(i);
    }
    funcs_.reserve(dw.functions.size() + 1024);  // room for built-ins looked up by name later
    for (auto& f : dw.functions) {
        FuncInfo fi;
        fi.name = f.name;
        auto sc = scriptByName_.find(std::string(f.name));
        if (sc != scriptByName_.end()) {
            fi.code = sc->second;
            fi.script = scriptIndexByName_[std::string(f.name)];
        } else if (int32_t ci = dw.codeIndex(f.name); ci >= 0) {
            fi.code = ci;  // GMS 2.3 functions are referenced by their code entry name
        } else {
            auto b = builtins_.find(std::string(f.name));
            if (b != builtins_.end()) fi.fn = b->second;
        }
        funcs_.push_back(fi);
    }
}

std::vector<std::string> VM::unknownFunctions() const {
    std::vector<std::string> out;
    for (auto& f : funcs_)
        if (!f.fn && f.code < 0) out.emplace_back(f.name);
    return out;
}

void VM::warnOnce(const std::string& key, const std::string& msg) {
    if (!warned_.insert(key).second) return;
    rt_.report(key.compare(0, 3, "fn:") == 0 ? Runner::DiagKind::Unimplemented : Runner::DiagKind::Warning, msg);
}

std::string VM::currentCodeName() const {
    if (frames_.empty()) return "<none>";
    return std::string(rt_.dw.code[frames_.back()->codeIndex].name);
}

std::string VM::callStack() const {
    std::string out;
    for (const Frame* f : frames_) {
        if (!out.empty()) out += " > ";
        out += std::string(rt_.dw.code[f->codeIndex].name);
        out += "(self " + std::to_string(f->self ? f->self->id : 0) + ", other " +
               std::to_string(f->other ? f->other->id : 0) + ")";
    }
    return out;
}

int32_t VM::scriptCode(int32_t scriptIndex) const {
    if (scriptIndex < 0 || static_cast<size_t>(scriptIndex) >= rt_.dw.scripts.size()) return -1;
    return rt_.dw.scripts[scriptIndex].code;
}

int32_t VM::varIdForName(std::string_view name) {
    auto it = varIdByName_.find(std::string(name));
    if (it != varIdByName_.end()) return it->second;
    // A variable the compiler never saw (created through variable_*_set): give it a fresh id.
    int32_t id = static_cast<int32_t>(varNameById_.size());
    auto ins = varIdByName_.emplace(std::string(name), id);
    varNameById_.push_back(ins.first->first);
    globals_.resize(varNameById_.size());
    return id;
}

std::string_view VM::varName(int32_t id) const {
    if (id < 0 || static_cast<size_t>(id) >= varNameById_.size()) return "?";
    return varNameById_[id];
}

Value& VM::globalVar(int32_t id) {
    if (static_cast<size_t>(id) >= globals_.size()) globals_.resize(id + 1);
    return globals_[id];
}

Value* VM::findGlobal(int32_t id) {
    if (id < 0 || static_cast<size_t>(id) >= globals_.size()) return nullptr;
    return &globals_[id];
}

Value VM::getInstanceVar(Instance* inst, int32_t id) {
    if (Value* v = inst->vars.find(id)) return *v;
    return Value();
}

void VM::setInstanceVar(Instance* inst, int32_t id, Value v) { inst->vars.getOrCreate(id) = std::move(v); }

static int16_t instanceBuiltin(std::string_view name) {
    int16_t b = builtinVarId(name);
    return b >= 0 && (b <= BV_timeline_loop || b == BV_layer) ? b : -1;
}
Value VM::memberGet(Instance* inst, int32_t id) {
    if (!inst->structObj)
        if (int16_t b = instanceBuiltin(varName(id)); b >= 0) return getBuiltinVar(rt_, inst, b, 0, nullptr, 0);
    return getInstanceVar(inst, id);
}
void VM::memberSet(Instance* inst, int32_t id, Value v) {
    if (!inst->structObj)
        if (int16_t b = instanceBuiltin(varName(id)); b >= 0) {
            setBuiltinVar(rt_, inst, b, 0, std::move(v), nullptr, 0);
            return;
        }
    setInstanceVar(inst, id, std::move(v));
}
bool VM::memberExists(Instance* inst, int32_t id) {
    if (!inst->structObj && instanceBuiltin(varName(id)) >= 0) return true;
    return inst->vars.find(id) != nullptr;
}

// ---------------------------------------------------------------------------------------------
// Compilation

CompiledCode& VM::compiled(int32_t code) {
    CompiledCode& c = compiled_[code];
    if (!c.ready) {
        c.ready = true;
        try {
            compile(code, c);
        } catch (const std::exception& e) {
            c.failed = true;
            rt_.report(Runner::DiagKind::Compile,
                       "cannot compile " + std::string(rt_.dw.code[code].name) + ": " + e.what());
        }
    }
    return c;
}

void VM::compile(int32_t code, CompiledCode& out) {
    const CodeEntry& e = rt_.dw.code[code];
    std::vector<Instruction> raw = decodeEntry(rt_.dw, e);
    std::unordered_map<uint32_t, int32_t> index;
    for (size_t i = 0; i < raw.size(); i++) index[raw[i].address] = static_cast<int32_t>(i);
    index[e.bytecode + e.length] = static_cast<int32_t>(raw.size());

    out.ins.resize(raw.size());
    // Keyed by name: a local can be reached through different VARI entries (the `[array]stack.x`
    // form with a local target from the stack may reference the instance variable's entry).
    // Slots are only used when the access really targets the local scope at run time.
    std::unordered_map<std::string_view, int32_t> localSlots;
    // Variables used as locals: marked local in VARI, or accessed with the local scope (GMS 2.3
    // function locals can have VARI entries marked as instance variables).
    std::unordered_set<std::string_view> localVars;
    for (const Instruction& r : raw)
        if (r.ref >= 0 && static_cast<size_t>(r.ref) < rt_.dw.variables.size() &&
            ((r.op == Op::Pop && r.type1 != DataType::Int16) ||
             ((r.op == Op::Push || r.op == Op::PushLoc) && r.type1 == DataType::Variable)) &&
            (r.operand == INST_LOCAL || rt_.dw.variables[r.ref].instanceType == INST_LOCAL))
            localVars.insert(rt_.dw.variables[r.ref].name);
    for (size_t i = 0; i < raw.size(); i++) {
        const Instruction& r = raw[i];
        CInstr& c = out.ins[i];
        c.op = r.op;
        c.t1 = r.type1;
        c.t2 = r.type2;
        c.aux = r.op == Op::Cmp ? static_cast<uint8_t>(r.cmp) : static_cast<uint8_t>(r.varKind);
        c.operand = r.operand;
        c.swap = r.op == Op::Pop && r.type1 == DataType::Int32;
        c.popenvExit = r.popenvExit;
        c.target = -1;
        c.ref = r.ref;
        c.slot = -1;
        c.funcRef = false;
        c.offset = r.address - e.bytecode;
        c.imm.i = 0;
        if (r.isBranch() && !r.popenvExit) {
            auto it = index.find(r.address + r.jump * 4);
            if (it == index.end()) throw GMLError("bad branch target");
            c.target = it->second;
        }
        if (r.op == Op::Push || r.op == Op::PushLoc || r.op == Op::PushGlb || r.op == Op::PushBltn ||
            r.op == Op::PushI) {
            switch (r.type1) {
                case DataType::Double: c.imm.d = r.imm.d; break;
                case DataType::Float: c.imm.d = r.imm.f; break;
                case DataType::Int64: c.imm.i = r.imm.i64; break;
                case DataType::Int16: case DataType::Int32: case DataType::Boolean: case DataType::UnsignedInt:
                    c.imm.i = r.imm.i32;
                    break;
                default: break;
            }
        }
        bool needsVar = (r.op == Op::Pop && r.type1 != DataType::Int16) ||
                        ((r.op == Op::Push || r.op == Op::PushLoc || r.op == Op::PushGlb || r.op == Op::PushBltn) &&
                         r.type1 == DataType::Variable);
        if (needsVar && (c.ref < 0 || static_cast<size_t>(c.ref) >= vars_.size()))
            throw GMLError("unresolved variable reference");
        // Locals get dense per-entry slots (GMS 2.3 local ids are not small indices).
        if (needsVar && localVars.count(vars_[c.ref].name)) {
            std::string_view name = vars_[c.ref].name;
            auto it = localSlots.find(name);
            if (it == localSlots.end()) it = localSlots.emplace(name, static_cast<int32_t>(localSlots.size())).first;
            c.slot = it->second;
        }
        // GMS 2.3+: break.i carries an operand (pushref's asset), which may be a FUNC reference.
        if (r.op == Op::Break && r.words == 2) {
            c.imm.i = r.imm.i32;
            auto fr = rt_.dw.funcRefs.find(r.address);
            if (fr != rt_.dw.funcRefs.end()) {
                c.funcRef = true;
                c.ref = fr->second;
            }
        }
        // GMS 2.3: push.i whose operand is a FUNC reference pushes a function value.
        if (r.op == Op::Push && r.type1 == DataType::Int32) {
            auto fr = rt_.dw.funcRefs.find(r.address);
            if (fr != rt_.dw.funcRefs.end()) {
                c.funcRef = true;
                c.ref = fr->second;
            }
            // 2023+: a push.i on a VARI chain is a variable hash (struct_get_from_hash etc.);
            // ours is the variable's id.
            auto vr = rt_.dw.varRefs.find(r.address);
            if (vr != rt_.dw.varRefs.end() && !c.funcRef)
                c.imm.i = varIdForName(rt_.dw.variables[vr->second].name);
        }
    }
    out.localCount = static_cast<int32_t>(localSlots.size());
    auto it = index.find(e.bytecode + e.offset);
    out.entry = it == index.end() ? 0 : static_cast<uint32_t>(it->second);
}

// ---------------------------------------------------------------------------------------------
// Value operations

static bool bothStrings(const Value& a, const Value& b) { return a.isString() && b.isString(); }

static Value arith(Op op, const Value& a, const Value& b) {
    if (op == Op::Add && bothStrings(a, b)) return Value(a.str() + b.str());
    if (a.isString() || b.isString()) {
        // GameMaker raises an error here; keep going with a best-effort result.
        if (op == Op::Add) return Value(a.toString() + b.toString());
        throw GMLError(std::string("illegal string operation in ") + opName(op));
    }
    bool integral = a.isIntegral() && b.isIntegral();
    if (integral) {
        int64_t x = a.rawInt(), y = b.rawInt();
        bool bools = a.type() == VT::Bool && b.type() == VT::Bool;
        switch (op) {
            case Op::Add: return Value::i64(x + y);
            case Op::Sub: return Value::i64(x - y);
            case Op::Mul: return Value::i64(x * y);
            // `/` is a real division and by zero gives infinity or NaN as IEEE floats do, in
            // GMS 1.4 as well (8 / (lives + 1) with -1 lives, as in a game tested); only `div` and `mod`
            // by zero are errors.
            case Op::Div: return Value(static_cast<double>(x) / static_cast<double>(y));
            case Op::Rem:
                if (y == 0) throw GMLError("division by zero (div)");
                return Value::i64(x / y);
            case Op::Mod:
                if (y == 0) throw GMLError("division by zero (mod)");
                return Value::i64(x % y);
            case Op::And: return bools ? Value::boolean(x & y) : Value::i64(x & y);
            case Op::Or: return bools ? Value::boolean(x | y) : Value::i64(x | y);
            case Op::Xor: return bools ? Value::boolean(x ^ y) : Value::i64(x ^ y);
            case Op::Shl: return Value::i64(static_cast<int64_t>(static_cast<uint64_t>(x) << (y & 63)));
            case Op::Shr: return Value::i64(x >> (y & 63));
            default: break;
        }
    }
    double x = a.num(), y = b.num();
    switch (op) {
        case Op::Add: return Value(x + y);
        case Op::Sub: return Value(x - y);
        case Op::Mul: return Value(x * y);
        case Op::Div: return Value(x / y);
        case Op::Rem:
            if (y == 0) throw GMLError("division by zero (div)");
            return Value(std::trunc(x / y));
        case Op::Mod:
            if (y == 0) throw GMLError("division by zero (mod)");
            return Value(std::fmod(x, y));
        case Op::And: return Value::i64(a.toInt64() & b.toInt64());
        case Op::Or: return Value::i64(a.toInt64() | b.toInt64());
        case Op::Xor: return Value::i64(a.toInt64() ^ b.toInt64());
        case Op::Shl: return Value::i64(static_cast<int64_t>(static_cast<uint64_t>(a.toInt64()) << (b.toInt64() & 63)));
        case Op::Shr: return Value::i64(a.toInt64() >> (b.toInt64() & 63));
        default: break;
    }
    throw GMLError("bad arithmetic op");
}

// A string that is entirely a number ("2", " -1.5"), for GMS2's mixed comparisons.
static bool numericString(const std::string& s, double& out) {
    const char* p = s.c_str();
    char* end = nullptr;
    out = strtod(p, &end);
    if (end == p) return false;
    while (*end == ' ' || *end == '\t') end++;
    return *end == 0;
}

static bool compare(const Value& a, const Value& b, CmpKind k, bool gms2) {
    int c;  // -1, 0, 1
    if (bothStrings(a, b)) {
        int r = a.str().compare(b.str());
        c = r < 0 ? -1 : (r > 0 ? 1 : 0);
    } else if (a.isString() || b.isString()) {
        // GMS2 compares a numeric string with a number by value ("2" == 2); 1.4 never matches.
        double x, y;
        bool ok = gms2 && (a.isString() ? numericString(a.str(), x) && b.isNumeric() && ((y = b.num()), true)
                                        : numericString(b.str(), y) && a.isNumeric() && ((x = a.num()), true));
        if (!ok) return k == CmpKind::NE;
        c = std::fabs(x - y) <= gCompareEpsilon ? 0 : (x < y ? -1 : 1);
    } else if (a.isArray() || b.isArray() || a.isObject() || b.isObject() || a.isMethod() || b.isMethod()) {
        bool eq = a.type() == b.type() &&
                  (a.isArray() ? a.arr() == b.arr()
                               : a.isObject() ? a.obj() == b.obj()
                                              : a.method() && b.method() && a.method()->code == b.method()->code &&
                                                    a.method()->builtin == b.method()->builtin);
        return k == CmpKind::EQ ? eq : (k == CmpKind::NE ? !eq : false);
    } else if (a.isUndefined() || b.isUndefined()) {
        bool eq = a.isUndefined() && b.isUndefined();
        if (k == CmpKind::EQ) return eq;
        if (k == CmpKind::NE) return !eq;
        double x = a.num(), y = b.num();
        c = x < y ? -1 : (x > y ? 1 : 0);
    } else if (a.type() == VT::Ptr || b.type() == VT::Ptr) {
        c = a.rawPtr() == b.rawPtr() ? 0 : (a.rawPtr() < b.rawPtr() ? -1 : 1);
    } else {
        double x = a.num(), y = b.num();
        if (std::fabs(x - y) <= gCompareEpsilon) c = 0;
        else c = x < y ? -1 : 1;
    }
    switch (k) {
        case CmpKind::LT: return c < 0;
        case CmpKind::LE: return c <= 0;
        case CmpKind::EQ: return c == 0;
        case CmpKind::NE: return c != 0;
        case CmpKind::GE: return c >= 0;
        case CmpKind::GT: return c > 0;
        default: return false;
    }
}

// Size of a stack item of the given type in YoYo's runner, which `dup` counts in.
static uint8_t typeSize(DataType t) {
    switch (t) {
        case DataType::Double: case DataType::Int64: return 8;
        case DataType::Variable: case DataType::String: return 16;
        default: return 4;
    }
}

static uint8_t resultSize(DataType a, DataType b) {
    if (a == DataType::Variable || b == DataType::Variable) return 16;
    return std::max(typeSize(a), typeSize(b));
}

static Value convert(Value v, DataType to, DataType from) {
    // A bool-typed operand (true/false literals are pushed as pushi.e 1/0) becomes a real bool.
    if (from == DataType::Boolean && to == DataType::Variable && v.type() != VT::Bool) return Value::boolean(v.truthy());
    switch (to) {
        case DataType::Boolean:
            if (v.type() == VT::Bool) return v;
            return Value::boolean(v.truthy());
        case DataType::Int32:
            if (v.isNumeric()) return Value::i32(v.toInt());
            return v;
        case DataType::Int64:
            if (v.isNumeric()) return Value::i64(v.toInt64());
            return v;
        case DataType::Double: case DataType::Float:
            if (v.isNumeric()) return Value(v.num());
            return v;
        default:
            return v;
    }
}

// ---------------------------------------------------------------------------------------------
// Variable access

Instance* VM::resolveOne(Frame& f, int32_t target) {
    switch (target) {
        case INST_SELF: case INST_BUILTIN: case INST_ARG: return f.self;
        case INST_OTHER: return f.other;
        case INST_NOONE: return nullptr;
        case INST_ALL: return rt_.firstOf(-1);
        case INST_STATIC: return staticScope(f.codeIndex);
        default: break;
    }
    if (target >= 100000) return rt_.findForAccess(target);
    // GameMaker still resolves obj.var while the object's instances are deactivated (hit-stop
    // effects rely on it), so fall back to inactive instances rather than failing.
    if (target >= 0 && static_cast<size_t>(target) < rt_.dw.objects.size()) return rt_.firstOfIncludingInactive(target);
    return nullptr;
}

// Instance variable storage; the global scope (GMS 2.3 global scripts run with self = global)
// maps onto the globals.
Value* VM::instanceSlot(Instance* inst, int32_t id, bool create) {
    if (inst == &rt_.globalSelf) return create ? &globalVar(id) : findGlobal(id);
    if (create) return &inst->vars.getOrCreate(id);
    // Structs made by a constructor read missing variables from its static scope (and up the
    // constructor chain); writes always land on the struct itself.
    for (Instance* i = inst; i; i = i->staticParent)
        if (Value* v = i->vars.find(id)) return v;
    return nullptr;
}

Value* VM::lvalueSlot(Frame& f, int32_t target, Instance* direct, const VarInfo& v, int32_t slotIdx) {
    if (v.builtin >= 0) return nullptr;
    if (direct) return instanceSlot(direct, v.id, true);
    if (target == INST_LOCAL) {
        if (slotIdx >= 0) return &f.locals[slotIdx];
        if (!f.extraLocals) f.extraLocals = std::make_unique<std::unordered_map<int32_t, Value>>();
        return &(*f.extraLocals)[static_cast<int32_t>(&v - vars_.data())];
    }
    if (target == INST_GLOBAL) return &globalVar(v.id);
    Instance* inst = resolveOne(f, target);
    return inst ? instanceSlot(inst, v.id, true) : nullptr;
}

int32_t VM::codeOf(const Value& fn) {
    if (GMMethod* m = fn.method()) return m->code;
    if (fn.isNumeric()) return scriptCode(fn.toInt());
    return -1;
}

int32_t VM::staticOwner(const Instance* scope) const {
    for (auto& [code, v] : staticScopes_)
        if (v.obj() && v.obj()->inst == scope) return code;
    return -1;
}

Instance* VM::staticScope(int32_t code) {
    auto it = staticScopes_.find(code);
    if (it == staticScopes_.end()) it = staticScopes_.emplace(code, newStruct(nullptr)).first;
    return it->second.obj()->inst;
}

// Is `name` in the comma-separated list `list` (GMR_TRACE_CALL)?
static bool traced(const char* list, std::string_view name) {
    std::string_view l(list);
    while (!l.empty()) {
        size_t c = l.find(',');
        if (l.substr(0, c) == name) return true;
        if (c == std::string_view::npos) break;
        l.remove_prefix(c + 1);
    }
    return false;
}

// The index readVar/writeVar get for a plain variable access (a[-1] is an index like any other).
constexpr int64_t kWholeVar = INT64_MIN;

// argumentN / argument[i] as an argument number, or -1 for other built-ins.
static int argNumber(int16_t builtin, int64_t index) {
    if (builtin >= BV_argument0 && builtin <= BV_argument15) return builtin - BV_argument0;
    if (builtin == BV_argument && index >= 0 && index < 4096) return static_cast<int>(index);
    return -1;
}

// An element read with a[i] in GML: past the end it is a runtime error, as in GameMaker (a
// catch block receives it); built-in functions keep GMArray::get's undefined.
static Value arrayElement(const GMArray& a, int64_t index) {
    size_t row = index >= 0 ? static_cast<size_t>(index / kArrayRowStride) : 0;
    size_t col = index >= 0 ? static_cast<size_t>(index % kArrayRowStride) : 0;
    if (index < 0 || row >= a.rows.size() || col >= a.rows[row].size())
        throw GMLError("array index " + std::to_string(index) + " out of range (" +
                       std::to_string(row < a.rows.size() ? a.rows[row].size() : 0) + ")");
    return a.rows[row][col];
}

static int64_t checkedIndex(int64_t index) {
    if (index < 0) throw GMLError("array index " + std::to_string(index) + " is negative");
    return index;
}

Value VM::readVar(Frame& f, int32_t target, Instance* direct, const VarInfo& v, int64_t index, int32_t slotIdx) {
    if (v.builtin >= 0) {
        // Scripts may assign arguments that were not passed (`if (argument0 == undefined) argument0 = 0`).
        if (int n = argNumber(v.builtin, index); n >= f.argc && f.extraArgs) {
            auto it = f.extraArgs->find(n);
            if (it == f.extraArgs->end()) return Value();
            // argumentN[i] is an element of argument N's array
            if (v.builtin != BV_argument && index != kWholeVar && it->second.arr()) return it->second.arr()->get(index);
            return it->second;
        }
        Instance* inst = direct ? direct : (target == INST_GLOBAL ? f.self : resolveOne(f, target));
        if (!inst) {
            warnOnce("bvnull:" + std::string(v.name), "built-in " + std::string(v.name) + " read on missing instance " +
                                                          std::to_string(target) + " in " + currentCodeName());
            return Value(0.0);
        }
        if (inst->structObj && v.memberId >= 0) {
            const Value* m = instanceSlot(inst, v.memberId, false);
            if (!m) return Value();
            return index != kWholeVar && m->arr() ? m->arr()->get(index) : *m;
        }
        return getBuiltinVar(rt_, inst, v.builtin, index, f.args, f.argc);
    }
    const Value* slot = nullptr;
    if (!direct && target == INST_LOCAL) {
        if (slotIdx >= 0) {
            slot = &f.locals[slotIdx];
        } else {
            if (!f.extraLocals) return Value();
            auto it = f.extraLocals->find(static_cast<int32_t>(&v - vars_.data()));
            if (it == f.extraLocals->end()) return Value();
            slot = &it->second;
        }
    } else if (!direct && target == INST_GLOBAL) {
        slot = &globalVar(v.id);
    } else {
        Instance* inst = direct ? direct : resolveOne(f, target);
        if (!inst) {
            warnOnce("vnull:" + std::string(v.name) + std::to_string(target),
                     "read of " + std::string(v.name) + " on missing instance/object " + std::to_string(target) +
                         " in " + currentCodeName());
            return Value(0.0);
        }
        // A weak reference's `ref`: its target while that is alive, else undefined.
        if (inst->structObj && inst->structObj->weakRef && v.name == "ref") {
            const GMWeakRef& w = *inst->structObj->weakRef;
            if (w.targetAlive) return *w.targetAlive ? Value::fromObject(w.target) : Value();
            auto it = rt_.byId.find(w.instanceId);
            return it != rt_.byId.end() && !it->second->destroyed ? Value(static_cast<double>(w.instanceId)) : Value();
        }
        slot = instanceSlot(inst, v.id, false);
        // GMS 2.3 "builtin" scope for user variables: self first, then global (method variables).
        if ((!slot || slot->isUndefined()) && !direct && target == INST_BUILTIN) {
            if (Value* g = findGlobal(v.id); g && !g->isUndefined()) slot = g;
        }
        // Globals are pre-allocated, so "unset" there is an undefined slot.
        if ((!slot || slot->isUndefined()) && rt_.dw.gen.gms2) {
            if (Value r; runtimeName(v.name, r)) return r;
        }
        if (!slot) {
            if (rt_.dw.options.flags & OPT_UNINIT_VARS_ZERO) return Value(0.0);
            warnOnce("unset:" + std::string(v.name),
                     "variable " + std::string(v.name) + " read before being set in " + currentCodeName());
            return rt_.dw.gen.gms2 ? Value() : Value(0.0);
        }
    }
    if (index == kWholeVar) return *slot;
    if (GMArray* a = slot->arr()) return arrayElement(*a, index);
    if (index == 0 && !rt_.dw.gen.gms2) return *slot;  // 1.4 lets scalars be read as var[0]
    return Value();
}

void VM::writeVar(Frame& f, int32_t target, Instance* direct, const VarInfo& v, int64_t index, int32_t slotIdx,
                  Value val) {
    auto store = [&](Value& slot) {
        if (index == kWholeVar) slot = std::move(val);
        else slot.writableArray()->set(checkedIndex(index), std::move(val));
    };
    if (v.builtin >= 0) {
        if (int n = argNumber(v.builtin, index); n >= f.argc) {
            if (!f.extraArgs) f.extraArgs = std::make_unique<std::unordered_map<int32_t, Value>>();
            Value& slot = (*f.extraArgs)[n];
            if (v.builtin != BV_argument && index != kWholeVar) {
                if (!slot.isArray()) slot = Value::newArray();
                slot.arr()->set(index, std::move(val));
            } else {
                slot = std::move(val);
            }
            return;
        }
        auto setOn = [&](Instance* inst, Value value) {
            if (inst->structObj && v.memberId >= 0) {  // a struct member, not a built-in
                Value& m = inst->vars.getOrCreate(v.memberId);
                if (index == kWholeVar) m = std::move(value);
                else m.writableArray()->set(checkedIndex(index), std::move(value));
                return;
            }
            setBuiltinVar(rt_, inst, v.builtin, index, std::move(value), f.args, f.argc);
        };
        if (direct) {
            setOn(direct, std::move(val));
            return;
        }
        if (target == INST_SELF || target == INST_BUILTIN || target == INST_GLOBAL || target == INST_OTHER ||
            target == INST_ARG || target >= 100000) {
            Instance* inst = target == INST_GLOBAL ? f.self : resolveOne(f, target);
            if (inst) setOn(inst, std::move(val));
            return;
        }
        std::vector<Instance*> list;
        rt_.collectTargets(target, f.self, f.other, list);
        for (Instance* inst : list) setBuiltinVar(rt_, inst, v.builtin, index, val, f.args, f.argc);
        return;
    }
    if (direct) return store(*instanceSlot(direct, v.id, true));
    if (target == INST_LOCAL) {
        if (slotIdx >= 0) return store(f.locals[slotIdx]);
        if (!f.extraLocals) f.extraLocals = std::make_unique<std::unordered_map<int32_t, Value>>();
        return store((*f.extraLocals)[static_cast<int32_t>(&v - vars_.data())]);
    }
    if (target == INST_GLOBAL) {
        // GMR_WATCH_GLOBAL also logs each write with the code that made it.
        static const char* watch = getenv("GMR_WATCH_GLOBAL");
        if (watch && v.name == watch)
            rt_.log("[watch] step %llu global.%s := %s in %s", static_cast<unsigned long long>(rt_.stepCount), watch,
                    val.repr().c_str(), currentCodeName().c_str());
        return store(globalVar(v.id));
    }
    if (target == INST_SELF || target == INST_OTHER || target == INST_BUILTIN || target == INST_ARG ||
        target == INST_STATIC || target >= 100000) {
        Instance* inst = resolveOne(f, target);
        if (!inst) {
            warnOnce("wnull:" + std::string(v.name), "write of " + std::string(v.name) + " to missing instance in " +
                                                         currentCodeName());
            return;
        }
        return store(*instanceSlot(inst, v.id, true));
    }
    // obj.var = value assigns to every instance of the object.
    std::vector<Instance*> list;
    rt_.collectTargets(target, f.self, f.other, list);
    if (list.empty())
        warnOnce("wobj:" + std::string(v.name), "write of " + std::string(v.name) + " to object " +
                                                    std::to_string(target) + " with no instances in " + currentCodeName());
    for (size_t i = 0; i < list.size(); i++) {
        Value& slot = *instanceSlot(list[i], v.id, true);
        Value copy = i + 1 < list.size() ? val : std::move(val);
        if (index == kWholeVar) slot = std::move(copy);
        else slot.writableArray()->set(checkedIndex(index), std::move(copy));
    }
}

// ---------------------------------------------------------------------------------------------
// GMS 2.3 function values and structs

Value VM::functionValue(int32_t funcIndex) {
    auto* m = new GMMethod();
    if (funcIndex >= 0 && static_cast<size_t>(funcIndex) < funcs_.size()) {
        const FuncInfo& fi = funcs_[funcIndex];
        m->code = fi.code;
        m->script = fi.script;
        if (fi.code < 0) m->builtin = funcIndex;
    }
    return Value::fromMethod(m);
}

Value VM::builtinFunctionValue(std::string_view name) {
    for (size_t i = 0; i < funcs_.size(); i++)
        if (funcs_[i].name == name && funcs_[i].fn) return functionValue(static_cast<int32_t>(i));
    auto it = builtins_.find(std::string(name));
    if (it == builtins_.end() || funcs_.size() >= funcs_.capacity()) return Value();
    extraFuncNames_.emplace_back(name);
    FuncInfo fi;
    fi.name = extraFuncNames_.back();
    fi.fn = it->second;
    funcs_.push_back(fi);
    return functionValue(static_cast<int32_t>(funcs_.size() - 1));
}

bool VM::runtimeName(std::string_view name, Value& out) {
    static const std::unordered_map<std::string_view, double> constants = {
        {"time_source_global", 0}, {"time_source_game", 1}, {"time_source_units_seconds", 0},
        {"time_source_units_frames", 1}, {"time_source_expire_nearest", 0}, {"time_source_expire_after", 1},
        {"time_source_state_initial", 0}, {"time_source_state_active", 1}, {"time_source_state_paused", 2},
        {"time_source_state_stopped", 3},
    };
    auto c = constants.find(name);
    if (c != constants.end()) {
        out = Value(c->second);
        return true;
    }
    if (builtins_.count(std::string(name))) {
        out = builtinFunctionValue(name);
        return !out.isUndefined();
    }
    return false;
}

Value VM::scriptValue(int32_t scriptIndex) {
    auto* m = new GMMethod();
    m->script = scriptIndex;
    m->code = scriptCode(scriptIndex);
    return Value::fromMethod(m);
}

Value VM::selfValue(Instance* self) const {
    if (!self) return Value(-4.0);
    if (self->structObj) return Value::fromObject(self->structObj);
    if (self == &rt_.globalSelf) return Value(static_cast<double>(INST_GLOBAL));
    return Value(static_cast<double>(self->id));
}

Instance* VM::targetOf(const Value& v, Instance* self, Instance* other) {
    if (GMObject* o = v.obj()) return o->inst;
    int32_t t = v.toInt();
    switch (t) {
        case INST_SELF: return self;
        case INST_OTHER: return other;
        case INST_GLOBAL: return &rt_.globalSelf;
        default: break;
    }
    if (t >= 100000) return rt_.findForAccess(t);
    if (t >= 0 && static_cast<size_t>(t) < rt_.dw.objects.size()) return rt_.firstOfIncludingInactive(t);
    return nullptr;
}

Value VM::bindMethod(const Value& selfArg, const Value& fn, Instance* caller) {
    auto* m = new GMMethod();
    if (GMMethod* src = fn.method()) {
        m->code = src->code;
        m->builtin = src->builtin;
        m->script = src->script;
    } else if (fn.isNumeric()) {
        m->script = fn.toInt();
        m->code = scriptCode(m->script);
    }
    // Functions declared at the top of a script are bound to global; calling them keeps the
    // caller's self, so treat that (and undefined) as unbound.
    if (selfArg.isObject()) {
        m->self = selfArg;
    } else if (!selfArg.isUndefined()) {
        Instance* t = targetOf(selfArg, caller, caller);
        if (t && t != &rt_.globalSelf) m->self = selfValue(t);
    }
    return Value::fromMethod(m);
}

Value VM::callValue(const Value& fn, Instance* self, Instance* other, int argc, Value* args) {
    GMMethod* m = fn.method();
    if (!m) {
        if (fn.isNumeric()) {
            int32_t code = scriptCode(fn.toInt());
            if (code >= 0) return call(code, self, other, argc, args);
        }
        throw GMLError("calling a value that is not a function: " + fn.repr());
    }
    Instance* s = self;
    if (!m->self.isUndefined()) {
        if (Instance* bound = targetOf(m->self, self, other)) s = bound;
    }
    if (m->code >= 0) return call(m->code, s, other, argc, args);
    if (m->builtin >= 0 && funcs_[m->builtin].fn) return funcs_[m->builtin].fn(rt_, s, other, argc, args);
    throw GMLError("calling an empty function value");
}

Value VM::newStruct(Instance* creator) {
    auto* o = new GMObject();
    o->inst = new Instance();
    o->inst->id = 0;
    o->inst->object = -1;
    o->inst->structObj = o;
    o->inst->visible = false;
    (void)creator;
    return Value::fromObject(o);
}

// ---------------------------------------------------------------------------------------------
// Execution

Value VM::run(int32_t code, Instance* self, Instance* other, int argc, Value* args) {
    try {
        return call(code, self, other, argc, args);
    } catch (const GMLError& e) {
        rt_.report(Runner::DiagKind::Error, "error in " + std::string(rt_.dw.code[code].name) + ": " + e.what());
    } catch (const std::exception& e) {
        rt_.report(Runner::DiagKind::Error,
                   "internal error in " + std::string(rt_.dw.code[code].name) + ": " + e.what());
    }
    return Value();
}

Value VM::call(int32_t code, Instance* self, Instance* other, int argc, Value* args) {
    if (code < 0 || static_cast<size_t>(code) >= compiled_.size()) return Value(0.0);
    CompiledCode& cc = compiled(code);
    if (cc.failed) return Value(0.0);
    if (depth_ >= kMaxDepth) throw GMLError("recursion limit reached");

    size_t nlocals = static_cast<size_t>(std::max(1, cc.localCount));
    if (localTop_ + nlocals > kLocalPoolSize) throw GMLError("local variable pool exhausted");
    Frame f{&cc, code, self, other, args, argc, &localPool_[localTop_], env_.size(), nullptr, nullptr, {}, nullptr};
    localTop_ += nlocals;
    size_t sp0 = sp_;
    depth_++;
    frames_.push_back(&f);

    struct Cleanup {
        VM& vm;
        Frame& f;
        size_t sp0, nlocals;
        ~Cleanup() {
            while (vm.sp_ > sp0) vm.stack_[--vm.sp_] = Value();
            for (size_t i = 0; i < nlocals; i++) f.locals[i] = Value();
            vm.localTop_ -= nlocals;
            vm.env_.resize(f.envBase);
            vm.frames_.pop_back();
            vm.depth_--;
        }
    } cleanup{*this, f, sp0, nlocals};

    return execute(f);
}

Value VM::callByName(std::string_view name, Instance* self, Instance* other, int argc, Value* args) {
    auto sc = scriptByName_.find(std::string(name));
    if (sc != scriptByName_.end()) return call(sc->second, self, other, argc, args);
    auto b = builtins_.find(std::string(name));
    if (b != builtins_.end()) return b->second(rt_, self, other, argc, args);
    warnOnce("fn:" + std::string(name), "call to unknown function " + std::string(name));
    return Value(0.0);
}

Value VM::execute(Frame& f) {
    const std::vector<CInstr>& ins = f.code->ins;
    const size_t n = ins.size();
    const size_t base = sp_;
    Value* stk = stack_.get();

    uint8_t* sizes = sizes_.get();
    auto push = [&](Value v, uint8_t size = 16) {
        if (sp_ >= kStackSize) throw GMLError("stack overflow");
        sizes[sp_] = size;
        stk[sp_++] = std::move(v);
    };
    auto pop = [&]() -> Value {
        if (sp_ <= base) throw GMLError("stack underflow");
        return std::move(stk[--sp_]);
    };

    size_t pc = f.code->entry;
    for (;;) {
    try {
    while (pc < n) {
        const CInstr& c = ins[pc];
        switch (c.op) {
            case Op::PushI:
                push(Value::i32(static_cast<int32_t>(c.imm.i)), 4);
                break;
            case Op::Push: case Op::PushLoc: case Op::PushGlb: case Op::PushBltn:
                switch (c.t1) {
                    case DataType::Int16: case DataType::Int32: case DataType::UnsignedInt:
                        if (c.funcRef) push(functionValue(c.ref), 4);
                        else push(Value::i32(static_cast<int32_t>(c.imm.i)), 4);
                        break;
                    case DataType::Int64: push(Value::i64(c.imm.i), 8); break;
                    case DataType::Double: push(Value(c.imm.d), 8); break;
                    case DataType::Float: push(Value(c.imm.d), 4); break;
                    case DataType::Boolean: push(Value::boolean(c.imm.i != 0), 4); break;
                    case DataType::String:
                        push(c.ref >= 0 && static_cast<size_t>(c.ref) < strings_.size() ? strings_[c.ref] : Value(""));
                        break;
                    case DataType::Variable: {
                        const VarInfo& v = vars_[c.ref];
                        VarKind kind = static_cast<VarKind>(c.aux);
                        if (kind == VarKind::Array) {
                            int64_t idx = pop().toInt64();
                            Value tv = pop();
                            if (tv.isNumeric() && tv.toInt() == -9) tv = pop();  // 2.3: target below
                            Instance* direct = tv.isObject() ? tv.obj()->inst : nullptr;
                            push(readVar(f, direct ? INST_SELF : tv.toInt(), direct, v, idx, c.slot));
                        } else if (kind == VarKind::StackTop) {
                            Value tv = pop();
                            if (tv.isNumeric() && tv.toInt() == -9) tv = pop();  // 2.3: target below
                            Instance* direct = tv.isObject() ? tv.obj()->inst : nullptr;
                            push(readVar(f, direct ? INST_SELF : tv.toInt(), direct, v, kWholeVar, c.slot));
                        } else if (kind == VarKind::MultiPush || kind == VarKind::MultiPushPop) {
                            // a[i][j]: push a[i] as the container for pushaf/popaf/pushac. The write
                            // side (multipushpop) makes a[i] a unique array, creating it if needed.
                            int64_t idx = pop().toInt64();
                            Value tv = pop();
                            if (tv.isNumeric() && tv.toInt() == -9) tv = pop();  // 2.3: target below
                            Instance* direct = tv.isObject() ? tv.obj()->inst : nullptr;
                            int32_t target = direct ? INST_SELF : tv.toInt();
                            if (kind == VarKind::MultiPush) {
                                Value whole = readVar(f, target, direct, v, kWholeVar, c.slot);
                                push(whole.arr() ? whole.arr()->get(idx) : Value(), 16);
                            } else {
                                GMArray* outer = nullptr;
                                Value tmp;
                                if (Value* root = lvalueSlot(f, target, direct, v, c.slot)) {
                                    outer = root->writableArray();
                                } else {  // built-in (e.g. argumentN): arrays are shared, edit in place
                                    tmp = readVar(f, target, direct, v, kWholeVar, c.slot);
                                    outer = tmp.arr();
                                    if (!outer) throw GMLError("array write into a value that is not an array");
                                }
                                Value& el = outer->at(idx);
                                push(Value::fromArray(el.writableArray()), 16);
                            }
                        } else if (c.operand == INST_STACKTOP) {
                            // push.v stacktop.x: the target instance/struct is on the stack.
                            Value tv = pop();
                            Instance* direct = tv.isObject() ? tv.obj()->inst : nullptr;
                            push(readVar(f, direct ? INST_SELF : tv.toInt(), direct, v, kWholeVar, c.slot));
                        } else {
                            push(readVar(f, c.operand, nullptr, v, kWholeVar, c.slot));
                        }
                        break;
                    }
                    default:
                        throw GMLError("bad push type");
                }
                break;
            case Op::Pop: {
                if (c.t1 == DataType::Int16) throw GMLError("pop.e swap is not supported");
                const VarInfo& v = vars_[c.ref];
                VarKind kind = static_cast<VarKind>(c.aux);
                // `x = true` pops a bool-typed 1 (pop.v.b): store a real bool.
                auto typed = [&](Value x) {
                    return c.t2 == DataType::Boolean && x.type() != VT::Bool ? Value::boolean(x.truthy()) : x;
                };
                if (kind == VarKind::Array) {
                    Value val, tv;
                    int64_t idx;
                    // A target of -9 means "the value below on the stack" (2.3: `global.arr[i] = v`).
                    if (c.swap) {
                        val = pop();
                        idx = pop().toInt64();
                        tv = pop();
                        if (tv.isNumeric() && tv.toInt() == -9) tv = pop();
                    } else {
                        idx = pop().toInt64();
                        tv = pop();
                        if (tv.isNumeric() && tv.toInt() == -9) tv = pop();
                        val = pop();
                    }
                    Instance* direct = tv.isObject() ? tv.obj()->inst : nullptr;
                    writeVar(f, direct ? INST_SELF : tv.toInt(), direct, v, idx, c.slot, typed(std::move(val)));
                } else if (kind == VarKind::StackTop) {
                    Value val, tv;
                    if (c.swap) {
                        val = pop();
                        tv = pop();
                        if (tv.isNumeric() && tv.toInt() == -9) tv = pop();
                    } else {
                        tv = pop();
                        if (tv.isNumeric() && tv.toInt() == -9) tv = pop();
                        val = pop();
                    }
                    Instance* direct = tv.isObject() ? tv.obj()->inst : nullptr;
                    writeVar(f, direct ? INST_SELF : tv.toInt(), direct, v, kWholeVar, c.slot, typed(std::move(val)));
                } else if (c.operand == INST_STACKTOP) {  // pop.v.v stacktop.x: target on the stack
                    Value tv = pop();
                    Value val = pop();
                    Instance* direct = tv.isObject() ? tv.obj()->inst : nullptr;
                    writeVar(f, direct ? INST_SELF : tv.toInt(), direct, v, kWholeVar, c.slot, typed(std::move(val)));
                } else {
                    writeVar(f, c.operand, nullptr, v, kWholeVar, c.slot, typed(pop()));
                }
                break;
            }
            case Op::Dup: {
                // Newer compilers' swap form (second byte set): dup.T a b moves the top a * sizeof(T)
                // bytes below the ((b & 0x7F) >> 3) * sizeof(T) bytes under them (`obj.f(args)` brings
                // obj above its arguments; `a[i++]` keeps the old value under the target).
                if (int extra = (c.operand >> 8) & 0xFF) {
                    size_t moveBytes = static_cast<size_t>(c.operand & 0xFF) * typeSize(c.t1);
                    size_t overBytes = static_cast<size_t>((extra & 0x7F) >> 3) * typeSize(c.t1);
                    size_t moveCount = 0, covered = 0;
                    while (covered < moveBytes) {
                        if (sp_ < base + moveCount + 1) throw GMLError("dup swap underflow");
                        covered += sizes[sp_ - 1 - moveCount];
                        moveCount++;
                    }
                    size_t overCount = 0;
                    covered = 0;
                    while (covered < overBytes) {
                        if (sp_ < base + moveCount + overCount + 1) throw GMLError("dup swap underflow");
                        covered += sizes[sp_ - 1 - moveCount - overCount];
                        overCount++;
                    }
                    size_t first = sp_ - moveCount - overCount;
                    std::rotate(stk + first, stk + first + overCount, stk + sp_);
                    std::rotate(sizes + first, sizes + first + overCount, sizes + sp_);
                    break;
                }
                // dup.T n duplicates (n + 1) * sizeof(T) bytes of stack, which may span several items.
                size_t bytes = (static_cast<size_t>(c.operand & 0xFF) + 1) * typeSize(c.t1);
                size_t count = 0, covered = 0;
                while (covered < bytes) {
                    if (sp_ < base + count + 1) throw GMLError("dup underflow");
                    covered += sizes[sp_ - 1 - count];
                    count++;
                }
                size_t start = sp_ - count;
                for (size_t i = 0; i < count; i++) push(Value(stk[start + i]), sizes[start + i]);
                break;
            }
            case Op::Popz:
                pop();
                break;
            case Op::Conv: {
                // In place on the stack top (most conversions leave the value as it is).
                if (sp_ <= base) throw GMLError("stack underflow");
                Value& top = stk[sp_ - 1];
                if (c.t2 != DataType::Variable || c.t1 == DataType::Boolean) top = convert(std::move(top), c.t2, c.t1);
                sizes[sp_ - 1] = typeSize(c.t2);
                break;
            }
            case Op::Mul: case Op::Div: case Op::Rem: case Op::Mod: case Op::Add: case Op::Sub:
            case Op::And: case Op::Or: case Op::Xor: case Op::Shl: case Op::Shr: {
                // Fast path, same results as arith(): two reals added, subtracted, multiplied or
                // divided (by non-zero), in place.
                if (sp_ >= base + 2) {
                    Value& a = stk[sp_ - 2];
                    const Value& b = stk[sp_ - 1];
                    if (a.type() == VT::Real && b.type() == VT::Real) {
                        double x = a.num(), y = b.num(), r = 0;
                        bool fast = true;
                        switch (c.op) {
                            case Op::Add: r = x + y; break;
                            case Op::Sub: r = x - y; break;
                            case Op::Mul: r = x * y; break;
                            case Op::Div: fast = y != 0; r = fast ? x / y : 0; break;
                            default: fast = false; break;
                        }
                        if (fast) {
                            a = Value(r);
                            sp_--;
                            sizes[sp_ - 1] = resultSize(c.t1, c.t2);
                            break;
                        }
                    }
                }
                Value b = pop();
                Value a = pop();
                push(arith(c.op, a, b), resultSize(c.t1, c.t2));
                break;
            }
            case Op::Cmp: {
                // Fast path, as compare() does for two numbers (reals, ints, bools): within epsilon is equal.
                if (sp_ >= base + 2) {
                    Value& a = stk[sp_ - 2];
                    const Value& b = stk[sp_ - 1];
                    if (a.isNumeric() && b.isNumeric()) {
                        double x = a.num(), y = b.num();
                        int cmp = std::fabs(x - y) <= gCompareEpsilon ? 0 : (x < y ? -1 : 1);
                        bool r;
                        switch (static_cast<CmpKind>(c.aux)) {
                            case CmpKind::LT: r = cmp < 0; break;
                            case CmpKind::LE: r = cmp <= 0; break;
                            case CmpKind::EQ: r = cmp == 0; break;
                            case CmpKind::NE: r = cmp != 0; break;
                            case CmpKind::GE: r = cmp >= 0; break;
                            case CmpKind::GT: r = cmp > 0; break;
                            default: r = false; break;
                        }
                        a = Value::boolean(r);
                        sp_--;
                        sizes[sp_ - 1] = 4;
                        break;
                    }
                }
                Value b = pop();
                Value a = pop();
                push(Value::boolean(compare(a, b, static_cast<CmpKind>(c.aux), rt_.dw.gen.gms2)), 4);
                break;
            }
            case Op::Neg: {
                Value a = pop();
                if (a.isIntegral()) push(Value::i64(-a.rawInt()), typeSize(c.t1));
                else push(Value(-a.num()), typeSize(c.t1));
                break;
            }
            case Op::Not: {
                Value a = pop();
                if (c.t1 == DataType::Boolean) push(Value::boolean(!a.truthy()), 4);
                else push(Value::i64(~a.toInt64()), typeSize(c.t1));
                break;
            }
            case Op::B:
                if (static_cast<size_t>(c.target) <= pc) checkLoop();
                pc = c.target;
                continue;
            case Op::Bt:
                if (pop().truthy()) {
                    if (static_cast<size_t>(c.target) <= pc) checkLoop();
                    pc = c.target;
                    continue;
                }
                break;
            case Op::Bf:
                if (!pop().truthy()) {
                    if (static_cast<size_t>(c.target) <= pc) checkLoop();
                    pc = c.target;
                    continue;
                }
                break;
            case Op::Ret:
                return pop();
            case Op::Exit:
                return Value(0.0);
            case Op::PushEnv: {
                Value tv = pop();
                // GMS 2.3: -9 ("stacktop") means the real target is the value below it.
                if (tv.isNumeric() && tv.toInt() == -9) tv = pop();
                EnvEntry e;
                if (GMObject* o = tv.obj()) {
                    e.list.push_back(o->inst);
                    e.keep = tv;
                } else {
                    rt_.collectTargets(tv.toInt(), f.self, f.other, e.list);
                }
                e.index = 0;
                e.savedSelf = f.self;
                e.savedOther = f.other;
                if (e.list.empty()) {
                    // pushenv jumps to its popenv, which pops this placeholder and falls through.
                    env_.push_back(std::move(e));
                    pc = c.target;
                    continue;
                }
                f.other = f.self;
                f.self = e.list[0];
                env_.push_back(std::move(e));
                break;
            }
            case Op::PopEnv: {
                if (env_.size() <= f.envBase) throw GMLError("popenv without pushenv");
                EnvEntry& e = env_.back();
                if (!c.popenvExit) {
                    while (++e.index < e.list.size()) {
                        Instance* next = e.list[e.index];
                        if (!next->destroyed && next->active) {
                            f.self = next;
                            pc = c.target;
                            break;
                        }
                    }
                    if (e.index < e.list.size()) continue;
                }
                f.self = e.savedSelf;
                f.other = e.savedOther;
                env_.pop_back();
                break;
            }
            case Op::Call: {
                int argc = c.operand;
                if (sp_ < base + static_cast<size_t>(argc)) throw GMLError("call underflow");
                Value* args = &stk[sp_ - argc];
                // Arguments are pushed last-first; reverse so args[0] is argument0.
                for (int i = 0, j = argc - 1; i < j; i++, j--) std::swap(args[i], args[j]);
                const FuncInfo& fn = funcs_[c.ref];
                Value result;
                if (fn.code >= 0) {
                    result = call(fn.code, f.self, f.other, argc, args);
                } else if (fn.fn) {
                    // GMR_TRACE_CALL=name: log each call to that built-in with its arguments and result.
                    static const char* traceCall = getenv("GMR_TRACE_CALL");
                    result = fn.fn(rt_, f.self, f.other, argc, args);
                    if (traceCall && traced(traceCall, fn.name)) {
                        std::string a;
                        for (int i = 0; i < argc; i++) a += (i ? ", " : "") + args[i].repr();
                        std::string_view from = f.codeIndex >= 0 ? rt_.dw.code[f.codeIndex].name : "?";
                        rt_.log("[call] step %llu %.*s(%s) = %s  (from %.*s)", static_cast<unsigned long long>(rt_.stepCount),
                                int(fn.name.size()), fn.name.data(), a.c_str(), result.repr().c_str(),
                                int(from.size()), from.data());
                    }
                } else {
                    unknownCalls_[std::string(fn.name)]++;
                    warnOnce("fn:" + std::string(fn.name), "unimplemented function " + std::string(fn.name));
                    result = Value(0.0);
                }
                for (int i = 0; i < argc; i++) stk[--sp_] = Value();
                push(std::move(result));
                break;
            }
            case Op::Break:
                switch (c.operand) {
                    case -1:  // chkindex
                        break;
                    case -2: {  // pushaf: container, index -> value
                        int64_t idx = pop().toInt64();
                        Value a = pop();
                        push(a.arr() ? arrayElement(*a.arr(), idx) : Value(), 16);
                        break;
                    }
                    case -3: {  // popaf: value, container, index -> container[index] = value
                        int64_t idx = pop().toInt64();
                        Value a = pop();
                        Value val = pop();
                        if (GMArray* arr = a.arr()) arr->set(idx, std::move(val));
                        else throw GMLError("array write into a value that is not an array");
                        break;
                    }
                    case -4: {  // pushac: container, index -> inner container (unique, created if needed)
                        int64_t idx = pop().toInt64();
                        Value a = pop();
                        if (GMArray* arr = a.arr()) push(Value::fromArray(arr->at(idx).writableArray()), 16);
                        else push(Value(), 16);
                        break;
                    }
                    case -5:  // setowner: the code that now owns the arrays it writes (copy-on-write)
                        ArrayOwnership::current = pop().toInt64();
                        break;
                    case -6:  // isstaticok: have this function's static initialisers run?
                        push(Value::boolean(staticInit_.count(f.codeIndex) != 0), 4);
                        break;
                    case -7:  // setstatic
                        staticInit_.insert(f.codeIndex);
                        break;
                    // savearef / restorearef bracket a compound assignment to a multi-dimensional
                    // array element (a[i][j] += n): dup, savearef, pushaf, <op>, restorearef, a dup
                    // swap, popaf. They leave the stack as it is; the dup swap puts the new value
                    // under the container and index for popaf.
                    case -8:  // savearef
                    case -9:  // restorearef
                        break;
                    case -11: {  // pushref: asset reference (type in the top byte) or function
                        if (c.funcRef) {
                            push(functionValue(c.ref), 16);
                            break;
                        }
                        int32_t type = static_cast<int32_t>((static_cast<uint32_t>(c.imm.i) >> 24) & 0xFF);
                        int32_t index = static_cast<int32_t>(c.imm.i & 0xFFFFFF);
                        if (type == 5) push(scriptValue(index), 16);  // script
                        else push(Value(static_cast<double>(index)), 16);
                        break;
                    }
                    case -10:  // chknullish: is the top of the stack undefined?
                        if (sp_ <= base) throw GMLError("chknullish underflow");
                        push(Value::boolean(stk[sp_ - 1].isUndefined()), 4);
                        break;
                    default:
                        if (c.operand < 0) throw GMLError("unsupported break " + std::to_string(c.operand));
                        break;  // debugger hooks
                }
                break;
            case Op::CallV: {
                // Stack: args (last-first), then the `self` to call with, then the function value.
                int argc = c.operand;
                Value fn = pop();
                Value tv = pop();
                if (sp_ < base + static_cast<size_t>(argc)) throw GMLError("callv underflow");
                Value* args = &stk[sp_ - argc];
                for (int i = 0, j = argc - 1; i < j; i++, j--) std::swap(args[i], args[j]);
                Instance* target = targetOf(tv, f.self, f.other);
                Value result = callValue(fn, target ? target : f.self, f.self, argc, args);
                for (int i = 0; i < argc; i++) stk[--sp_] = Value();
                push(std::move(result));
                break;
            }
        }
        pc++;
    }
    return Value(0.0);
    } catch (const GMLError& e) {
        if (enterCatch(f, e, pc)) continue;  // a GML try covers it: on with the catch block
        if (std::string(e.what()).find(" @") != std::string::npos) throw;
        char buf[48];
        snprintf(buf, sizeof buf, " @%05x ", pc < n ? ins[pc].offset : 0u);
        std::string where = buf;
        if (pc < n) where += opName(ins[pc].op);
        if (f.codeIndex >= 0 && static_cast<size_t>(f.codeIndex) < rt_.dw.code.size())
            where += " in " + std::string(rt_.dw.code[f.codeIndex].name);
        if (auto* t = dynamic_cast<const GMLThrow*>(&e)) throw GMLThrow(t->value, std::string(e.what()) + where);
        throw GMLError(std::string(e.what()) + where);
    }
    }
}

void VM::tryHook(int argc, const Value* args) {
    if (frames_.empty() || argc < 1) return;
    Frame& f = *frames_.back();
    // The call's own arguments are still on the stack; the handler goes back to below them.
    int64_t end = argc > 1 ? args[1].toInt64() : -1;
    f.tries.push_back({args[0].toInt64(), end, sp_ - static_cast<size_t>(argc), env_.size(), f.self, f.other});
}

// The try statement's code unhooks its handler itself: at the start of the catch block and at the
// end block, which normal completion of the try body branches to.
void VM::tryUnhook() {
    if (!frames_.empty() && !frames_.back()->tries.empty()) frames_.back()->tries.pop_back();
}

void VM::finishFinally() {
    if (frames_.empty() || !frames_.back()->pending) return;
    std::unique_ptr<Frame::Pending> p = std::move(frames_.back()->pending);
    if (p->thrown) throw GMLThrow(p->value, p->message);
    throw GMLError(p->message);
}

bool VM::enterCatch(Frame& f, const GMLError& e, size_t& pc) {
    if (f.tries.empty()) return false;
    // The handler stays registered: the catch block (or the end block) unhooks it.
    Frame::TryHandler h = f.tries.back();
    const std::vector<CInstr>& ins = f.code->ins;
    auto at = [&](int64_t offset) {
        for (size_t i = 0; offset >= 0 && i < ins.size(); i++)
            if (ins[i].offset == static_cast<uint32_t>(offset)) return i;
        return ins.size();
    };
    size_t catchPc = at(h.catchAt), endPc = at(h.endAt);
    if (catchPc == ins.size() && endPc == ins.size()) {
        f.tries.pop_back();
        return false;
    }
    if (catchPc == ins.size()) {  // try / finally: through the finally block, then on outwards
        if (rt_.debugTrace) rt_.log("[gml] through finally in %s: %s", f.codeIndex >= 0 ? std::string(rt_.dw.code[f.codeIndex].name).c_str() : "?", e.what());
        while (sp_ > h.sp) stack_[--sp_] = Value();
        env_.resize(h.env);
        f.self = h.self;
        f.other = h.other;
        f.pending = std::make_unique<Frame::Pending>();
        if (auto* t = dynamic_cast<const GMLThrow*>(&e)) f.pending->thrown = true, f.pending->value = t->value;
        f.pending->message = e.what();
        pc = endPc;
        return true;
    }
    size_t target = catchPc;
    f.pending.reset();  // a new exception replaces one still on its way out
    if (rt_.debugTrace) rt_.log("[gml] caught by try in %s: %s", f.codeIndex >= 0 ? std::string(rt_.dw.code[f.codeIndex].name).c_str() : "?", e.what());
    while (sp_ > h.sp) stack_[--sp_] = Value();
    env_.resize(h.env);
    f.self = h.self;
    f.other = h.other;
    Value exception;
    if (auto* t = dynamic_cast<const GMLThrow*>(&e)) {
        exception = t->value;
    } else {
        // A runtime error: the struct GameMaker passes (message, longMessage, script, stacktrace).
        std::string message = e.what();
        if (size_t at = message.find(" @"); at != std::string::npos) message.resize(at);
        std::string script = f.codeIndex >= 0 ? std::string(rt_.dw.code[f.codeIndex].name) : "";
        exception = newStruct(f.self);
        Instance* s = exception.obj()->inst;
        memberSet(s, varIdForName("message"), Value(message));
        memberSet(s, varIdForName("longMessage"), Value("ERROR in action number 1\n" + message));
        memberSet(s, varIdForName("script"), Value(script));
        Value trace = Value::newArray();
        trace.writableArray()->rows.assign(1, {Value(script)});
        memberSet(s, varIdForName("stacktrace"), trace);
    }
    sizes_[sp_] = 16;
    stack_[sp_++] = std::move(exception);
    pc = target;
    return true;
}

}  // namespace gm
