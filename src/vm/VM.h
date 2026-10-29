// Bytecode interpreter for GameMaker bytecode 15/16.
#pragma once

#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Bytecode.h"
#include "Value.h"

namespace gm {

// Tolerance for numeric ==, <, > etc. (math_set_epsilon; GameMaker defaults to 0.00001).
extern double gCompareEpsilon;

class Runner;
struct Instance;

// Thrown by built-ins and the VM for GML runtime errors. Aborts the current event.
class GMLError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// `throw value` in GML: a catch block receives the value itself.
class GMLThrow : public GMLError {
public:
    GMLThrow(Value v, const std::string& what) : GMLError(what), value(std::move(v)) {}
    Value value;
};

using BuiltinFn = Value (*)(Runner& rt, Instance* self, Instance* other, int argc, Value* args);

// Pre-decoded instruction with branch targets turned into instruction indices.
struct CInstr {
    Op op;
    DataType t1, t2;
    uint8_t aux;      // CmpKind for cmp, VarKind for variable access
    int16_t operand;  // instance target / argc / dup count
    bool swap;        // pop.i.v form: value is on top of the instance/index
    bool popenvExit;
    int32_t target;   // branch target (instruction index)
    int32_t ref;      // variable / function / string index
    int32_t slot;     // local variable slot (frame-relative), -1 if not a local
    bool funcRef;     // push.i of a function reference (GMS 2.3)
    uint32_t offset;  // byte offset in the code entry (diagnostics)
    union {
        double d;
        int64_t i;
    } imm;
};

struct CompiledCode {
    bool ready = false;
    bool failed = false;
    uint32_t entry = 0;  // first instruction index (entries can share a blob)
    int32_t localCount = 0;
    std::vector<CInstr> ins;
};

enum class VarScope : uint8_t { Instance, Global, Local, Builtin };

struct VarInfo {
    std::string_view name;
    int32_t id = 0;        // instance/global variable id, or local slot
    int16_t builtin = -1;  // BuiltinVar id when this is a built-in variable
    int32_t memberId = -1; // per-instance built-ins: the plain variable used when the target is a struct
};

struct FuncInfo {
    std::string_view name;
    BuiltinFn fn = nullptr;
    int32_t code = -1;    // script code entry
    int32_t script = -1;  // script asset index
};

class VM {
public:
    explicit VM(Runner& rt);
    void init();

    void registerBuiltin(const char* name, BuiltinFn fn) { builtins_[name] = fn; }
    bool hasBuiltin(std::string_view name) const { return builtins_.count(std::string(name)) != 0; }
    BuiltinFn findBuiltin(std::string_view name) const {
        auto it = builtins_.find(std::string(name));
        return it == builtins_.end() ? nullptr : it->second;
    }

    // Runs a code entry (event, script, creation code). Errors are logged and swallowed.
    Value run(int32_t code, Instance* self, Instance* other, int argc = 0, Value* args = nullptr);
    // Like run(), but propagates GMLError (used for script calls from built-ins).
    Value call(int32_t code, Instance* self, Instance* other, int argc, Value* args);
    // Calls a script or built-in by FUNC-style name; used by script_execute.
    Value callByName(std::string_view name, Instance* self, Instance* other, int argc, Value* args);
    int32_t scriptCode(int32_t scriptIndex) const;

    // GMS 2.3 function values.
    Value functionValue(int32_t funcIndex);
    Value builtinFunctionValue(std::string_view name);  // a built-in as a function value, or undefined           // FUNC entry -> unbound method value
    Value scriptValue(int32_t scriptIndex);           // script asset -> unbound method value
    Value bindMethod(const Value& self, const Value& fn, Instance* caller);
    Value callValue(const Value& fn, Instance* self, Instance* other, int argc, Value* args);
    Value selfValue(Instance* self) const;            // struct, instance id or global
    Instance* targetOf(const Value& v, Instance* self, Instance* other);
    Value newStruct(Instance* creator);
    Instance* staticScope(int32_t code);  // the static scope of a function (by code entry)
    int32_t staticOwner(const Instance* scope) const;  // the code entry whose static scope this is, or -1
    int32_t currentCode() const { return frames_.empty() ? -1 : frames_.back()->codeIndex; }
    int32_t codeOf(const Value& fn);      // code entry of a function value or script index, or -1

    // Variable access by name (variable_instance_get etc.) and by id.
    int32_t varIdForName(std::string_view name);  // instance/global share one id space
    Value& globalVar(int32_t id);
    Value* findGlobal(int32_t id);
    Value getInstanceVar(Instance* inst, int32_t id);
    // A member by variable id as variable_instance_get / set / exists see it: on an instance
    // (not a struct) the per-instance built-ins (x, image_alpha, alarm, ...) are the built-ins.
    Value memberGet(Instance* inst, int32_t id);
    void memberSet(Instance* inst, int32_t id, Value v);
    bool memberExists(Instance* inst, int32_t id);
    void setInstanceVar(Instance* inst, int32_t id, Value v);

    std::string_view varName(int32_t id) const;
    std::vector<std::string> unknownFunctions() const;
    void warnOnce(const std::string& key, const std::string& msg);

    // Optional guard against runaway loops (--smoke): at most loopLimit backward jumps per
    // game step, then the running event is aborted with an error.
    int64_t loopLimit = 0;
    void resetLoopCounter() { loopCount_ = 0; }

    int depth() const { return depth_; }
    std::string currentCodeName() const;
    std::string callStack() const;  // "outer > ... > inner" with self/other ids
    // try / catch / finally (GMS 2.3): @@try_hook@@(catch, end), @@try_unhook@@ and
    // @@finish_finally@@ in the running code.
    void tryHook(int argc, const Value* args);
    void tryUnhook();
    void finishFinally();  // an exception that went to the finally block (no catch) goes on from here

private:
    struct Frame {
        const CompiledCode* code;
        int32_t codeIndex;
        Instance* self;
        Instance* other;
        Value* args;
        int argc;
        Value* locals;
        size_t envBase;
        std::unique_ptr<std::unordered_map<int32_t, Value>> extraLocals;  // locals only known at run time
        std::unique_ptr<std::unordered_map<int32_t, Value>> extraArgs;    // argumentN written past argc
        struct TryHandler {
            int64_t catchAt;   // byte offset of the catch block, -1 without one
            int64_t endAt;     // the try statement's end (try_unhook, then the finally block)
            size_t sp, env;    // the stack and `with` depth to go back to
            Instance* self;
            Instance* other;
        };
        std::vector<TryHandler> tries;  // innermost last
        // An exception on its way through a finally block (try without catch): rethrown by
        // @@finish_finally@@ once the block has run.
        struct Pending {
            bool thrown = false;  // `throw value` rather than a runtime error
            Value value;
            std::string message;
        };
        std::unique_ptr<Pending> pending;
    };
    // The catch block for an error in `f`, with the stack unwound and the exception pushed;
    // false when no try in `f` covers it.
    bool enterCatch(Frame& f, const GMLError& e, size_t& pc);
    struct EnvEntry {
        Value keep;  // `with (struct)`: holds the struct while the block runs (it may be a temporary)
        std::vector<Instance*> list;
        size_t index;
        Instance* savedSelf;
        Instance* savedOther;
    };

    CompiledCode& compiled(int32_t code);
    void compile(int32_t code, CompiledCode& out);
    Value execute(Frame& f);

    Value readVar(Frame& f, int32_t target, Instance* direct, const VarInfo& v, int64_t index, int32_t slot);
    void writeVar(Frame& f, int32_t target, Instance* direct, const VarInfo& v, int64_t index, int32_t slot,
                  Value val);
    Value* instanceSlot(Instance* inst, int32_t id, bool create);
    // Storage of a user variable for writing (created if missing), or null for built-ins.
    Value* lvalueSlot(Frame& f, int32_t target, Instance* direct, const VarInfo& v, int32_t slotIdx);
    Instance* resolveOne(Frame& f, int32_t target);
    Value builtinCall(const CInstr& in, Frame& f, Value* args);

    Runner& rt_;
    std::vector<CompiledCode> compiled_;
    std::vector<VarInfo> vars_;
    std::vector<FuncInfo> funcs_;
    std::deque<std::string> extraFuncNames_;  // names of built-ins added to funcs_ at run time
    // Names the compiler left as variable reads but the runner provides: built-in functions (as
    // function values) and run-time constants such as time_source_global.
    bool runtimeName(std::string_view name, Value& out);
    std::vector<Value> strings_;
    std::vector<Value> globals_;
    std::unordered_map<std::string, BuiltinFn> builtins_;
    std::unordered_map<std::string, int32_t> varIdByName_;
    std::vector<std::string_view> varNameById_;
    std::unordered_map<std::string, int32_t> scriptByName_;
    std::unordered_map<std::string, int32_t> scriptIndexByName_;
    std::unordered_set<std::string> warned_;
    // GMS 2.3 static variables: one static scope (a struct) per function's code entry, and
    // whether its `static` initialisers have run.
    std::unordered_map<int32_t, Value> staticScopes_;
    std::unordered_set<int32_t> staticInit_;
    std::unordered_map<std::string, int> unknownCalls_;
    int64_t loopCount_ = 0;
    void checkLoop() {
        if (loopLimit > 0 && ++loopCount_ > loopLimit) {
            loopCount_ = 0;
            throw GMLError("probable infinite loop (loop limit reached)");
        }
    }

    std::unique_ptr<Value[]> stack_;
    std::unique_ptr<uint8_t[]> sizes_;  // byte size of each stack item as the bytecode sees it (for dup)
    size_t sp_ = 0;
    std::unique_ptr<Value[]> localPool_;
    size_t localTop_ = 0;
    std::vector<EnvEntry> env_;
    std::vector<Frame*> frames_;
    int depth_ = 0;
};

}  // namespace gm
