#include "Bytecode.h"

#include <cinttypes>
#include <cstdio>
#include <map>

#include "../data/DataWin.h"

namespace gm {

const char* opName(Op op) {
    switch (op) {
        case Op::Conv: return "conv";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::Rem: return "rem";
        case Op::Mod: return "mod";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::And: return "and";
        case Op::Or: return "or";
        case Op::Xor: return "xor";
        case Op::Neg: return "neg";
        case Op::Not: return "not";
        case Op::Shl: return "shl";
        case Op::Shr: return "shr";
        case Op::Cmp: return "cmp";
        case Op::Pop: return "pop";
        case Op::PushI: return "pushi";
        case Op::Dup: return "dup";
        case Op::CallV: return "callv";
        case Op::Ret: return "ret";
        case Op::Exit: return "exit";
        case Op::Popz: return "popz";
        case Op::B: return "b";
        case Op::Bt: return "bt";
        case Op::Bf: return "bf";
        case Op::PushEnv: return "pushenv";
        case Op::PopEnv: return "popenv";
        case Op::Push: return "push";
        case Op::PushLoc: return "pushloc";
        case Op::PushGlb: return "pushglb";
        case Op::PushBltn: return "pushbltn";
        case Op::Call: return "call";
        case Op::Break: return "break";
    }
    return "???";
}

bool isKnownOp(uint8_t op) {
    switch (static_cast<Op>(op)) {
        case Op::Conv: case Op::Mul: case Op::Div: case Op::Rem: case Op::Mod: case Op::Add:
        case Op::Sub: case Op::And: case Op::Or: case Op::Xor: case Op::Neg: case Op::Not:
        case Op::Shl: case Op::Shr: case Op::Cmp: case Op::Pop: case Op::PushI: case Op::Dup:
        case Op::CallV: case Op::Ret: case Op::Exit: case Op::Popz: case Op::B: case Op::Bt:
        case Op::Bf: case Op::PushEnv: case Op::PopEnv: case Op::Push: case Op::PushLoc:
        case Op::PushGlb: case Op::PushBltn: case Op::Call: case Op::Break:
            return true;
    }
    return false;
}

char typeChar(DataType t) {
    switch (t) {
        case DataType::Double: return 'd';
        case DataType::Float: return 'f';
        case DataType::Int32: return 'i';
        case DataType::Int64: return 'l';
        case DataType::Boolean: return 'b';
        case DataType::Variable: return 'v';
        case DataType::String: return 's';
        case DataType::Instance: return 'n';
        case DataType::Delete: return 'x';
        case DataType::Undefined: return 'u';
        case DataType::UnsignedInt: return 'w';
        case DataType::Int16: return 'e';
    }
    return '?';
}

static VarKind varKindOf(uint32_t refWord) {
    switch ((refWord >> 24) & 0xF8) {
        case 0x00: return VarKind::Array;
        case 0x80: return VarKind::StackTop;
        case 0xA0: return VarKind::Normal;
        case 0xE0: return VarKind::Instance;
        case 0x10: return VarKind::MultiPush;
        case 0x90: return VarKind::MultiPushPop;
        default: return VarKind::Other;
    }
}

Instruction decode(const DataWin& dw, uint32_t address) {
    const Reader& r = dw.reader();
    uint32_t w = r.u32(address);
    Instruction in;
    in.address = address;
    uint8_t op = static_cast<uint8_t>(w >> 24);
    if (!isKnownOp(op)) throw FormatError("unknown opcode 0x" + Reader::hex(op) + " at 0x" + Reader::hex(address));
    in.op = static_cast<Op>(op);
    in.type1 = static_cast<DataType>((w >> 16) & 0xF);
    in.type2 = static_cast<DataType>((w >> 20) & 0xF);
    in.operand = static_cast<int16_t>(w & 0xFFFF);

    auto varRef = [&]() {
        uint32_t ref = r.u32(address + 4);
        in.varKind = varKindOf(ref);
        auto it = dw.varRefs.find(address);
        in.ref = it == dw.varRefs.end() ? -1 : it->second;
        in.words = 2;
    };

    switch (in.op) {
        case Op::Cmp:
            in.cmp = static_cast<CmpKind>((w >> 8) & 0xFF);
            break;
        case Op::B: case Op::Bt: case Op::Bf: case Op::PushEnv: case Op::PopEnv: {
            int32_t off = static_cast<int32_t>(w << 9) >> 9;  // sign-extend 23 bits
            if (in.op == Op::PopEnv && off == -0x100000) in.popenvExit = true;
            in.jump = off;
            break;
        }
        case Op::Pop:
            if (in.type1 == DataType::Int16) break;  // GMS2.3 swap form, no reference
            varRef();
            break;
        case Op::Push: case Op::PushLoc: case Op::PushGlb: case Op::PushBltn:
            switch (in.type1) {
                case DataType::Int16: in.imm.i32 = in.operand; break;
                case DataType::Int32: case DataType::Boolean: case DataType::UnsignedInt:
                    in.imm.i32 = r.i32(address + 4);
                    in.words = 2;
                    break;
                case DataType::Float:
                    in.imm.f = r.f32(address + 4);
                    in.words = 2;
                    break;
                case DataType::Double:
                    in.imm.d = r.f64(address + 4);
                    in.words = 3;
                    break;
                case DataType::Int64:
                    in.imm.i64 = static_cast<int64_t>(r.u64(address + 4));
                    in.words = 3;
                    break;
                case DataType::String:
                    in.ref = r.i32(address + 4);
                    in.words = 2;
                    break;
                case DataType::Variable:
                    varRef();
                    break;
                default:
                    throw FormatError("bad push type at 0x" + Reader::hex(address));
            }
            break;
        case Op::PushI:
            in.imm.i32 = in.operand;
            break;
        case Op::Break:
            // GMS 2.3+ extended ops with an Int32 type carry an extra word (pushref's asset).
            if (in.type1 == DataType::Int32) {
                in.imm.i32 = r.i32(address + 4);
                in.words = 2;
            }
            break;
        case Op::Call: {
            auto it = dw.funcRefs.find(address);
            in.ref = it == dw.funcRefs.end() ? -1 : it->second;
            in.words = 2;
            break;
        }
        default:
            break;
    }
    return in;
}

std::vector<Instruction> decodeEntry(const DataWin& dw, const CodeEntry& entry) {
    std::vector<Instruction> out;
    uint32_t end = entry.bytecode + entry.length;
    for (uint32_t a = entry.bytecode; a < end;) {
        out.push_back(decode(dw, a));
        a += out.back().words * 4;
    }
    return out;
}

static const char* instanceName(int32_t t) {
    switch (t) {
        case INST_SELF: return "self";
        case INST_OTHER: return "other";
        case INST_ALL: return "all";
        case INST_NOONE: return "noone";
        case INST_GLOBAL: return "global";
        case INST_BUILTIN: return "builtin";
        case INST_LOCAL: return "local";
        case INST_STACKTOP: return "stacktop";
        case INST_ARG: return "arg";
        case INST_STATIC: return "static";
    }
    return nullptr;
}

static std::string varText(const DataWin& dw, const Instruction& in) {
    std::string s;
    if (in.varKind == VarKind::Array) s += "[array]";
    else if (in.varKind == VarKind::StackTop) s += "[stacktop]";
    else if (in.varKind == VarKind::Instance) s += "[instance]";
    else if (in.varKind == VarKind::MultiPush) s += "[multipush]";
    else if (in.varKind == VarKind::MultiPushPop) s += "[multipushpop]";
    if (in.varKind == VarKind::Array || in.varKind == VarKind::StackTop || in.varKind == VarKind::MultiPush ||
        in.varKind == VarKind::MultiPushPop) {
        // The target instance comes from the stack; the operand field is unused.
        s += "stack";
    } else if (const char* n = instanceName(in.operand)) {
        s += n;
    } else if (in.operand >= 0 && static_cast<size_t>(in.operand) < dw.objects.size()) {
        s += dw.objects[in.operand].name;
    } else {
        s += std::to_string(in.operand);
    }
    s += '.';
    if (in.ref >= 0) s += dw.variables[in.ref].name;
    else s += "<unresolved>";
    return s;
}

static std::string quote(std::string_view v) {
    std::string s = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') { s += '\\'; s += c; }
        else if (c == '\n') s += "\\n";
        else if (c == '\r') s += "\\r";
        else s += c;
    }
    return s + '"';
}

std::string formatInstruction(const DataWin& dw, const Instruction& in) {
    char buf[64];
    std::string s = opName(in.op);
    switch (in.op) {
        case Op::Conv: case Op::Mul: case Op::Div: case Op::Rem: case Op::Mod: case Op::Add:
        case Op::Sub: case Op::And: case Op::Or: case Op::Xor: case Op::Shl: case Op::Shr:
        case Op::Pop:
            s += '.'; s += typeChar(in.type1); s += '.'; s += typeChar(in.type2);
            if (in.op == Op::Pop) s += ' ' + varText(dw, in);
            break;
        case Op::Cmp: {
            static const char* k[] = {"?", "LT", "LE", "EQ", "NE", "GE", "GT"};
            s += '.'; s += typeChar(in.type1); s += '.'; s += typeChar(in.type2);
            s += ' ';
            s += static_cast<uint8_t>(in.cmp) <= 6 ? k[static_cast<uint8_t>(in.cmp)] : "?";
            break;
        }
        case Op::Neg: case Op::Not: case Op::Ret: case Op::Exit: case Op::Popz:
            s += '.'; s += typeChar(in.type1);
            break;
        case Op::Dup:
            s += '.'; s += typeChar(in.type1); s += ' ' + std::to_string(in.operand & 0xFF);
            if ((in.operand >> 8) & 0xFF) s += ' ' + std::to_string((in.operand >> 8) & 0xFF);  // swap form
            break;
        case Op::Break:
            // Negative operands are the extended ops (chkindex from GMS 1.4, the rest GMS 2.3+).
            switch (in.operand) {
                case -1: s = "chkindex"; break;
                case -2: s = "pushaf"; break;
                case -3: s = "popaf"; break;
                case -4: s = "pushac"; break;
                case -5: s = "setowner"; break;
                case -6: s = "isstaticok"; break;
                case -7: s = "setstatic"; break;
                case -8: s = "savearef"; break;
                case -9: s = "restorearef"; break;
                case -10: s = "chknullish"; break;
                case -11: s = "pushref"; break;
                default: s += ' ' + std::to_string(in.operand); break;
            }
            if (in.words == 2) s += ' ' + std::to_string(in.imm.i32);
            break;
        case Op::Push: case Op::PushLoc: case Op::PushGlb: case Op::PushBltn: case Op::PushI:
            s += '.'; s += typeChar(in.type1); s += ' ';
            switch (in.type1) {
                case DataType::Int16: case DataType::Int32: case DataType::Boolean: case DataType::UnsignedInt:
                    s += std::to_string(in.imm.i32);
                    break;
                case DataType::Float: snprintf(buf, sizeof buf, "%g", in.imm.f); s += buf; break;
                case DataType::Double: snprintf(buf, sizeof buf, "%.17g", in.imm.d); s += buf; break;
                case DataType::Int64: s += std::to_string(in.imm.i64); break;
                case DataType::String:
                    if (in.ref >= 0 && static_cast<size_t>(in.ref) < dw.strings.size()) s += quote(dw.strings[in.ref]);
                    else s += "<bad string " + std::to_string(in.ref) + ">";
                    break;
                case DataType::Variable: s += varText(dw, in); break;
                default: s += '?';
            }
            break;
        case Op::Call:
            s += '.'; s += typeChar(in.type1); s += ' ';
            s += in.ref >= 0 ? std::string(dw.functions[in.ref].name) : "<unresolved>";
            s += "(argc=" + std::to_string(in.operand) + ")";
            break;
        default:
            break;
    }
    return s;
}

std::string disassemble(const DataWin& dw, const CodeEntry& entry) {
    auto code = decodeEntry(dw, entry);
    std::map<uint32_t, int> labels;
    for (auto& in : code)
        if (in.isBranch() && !in.popenvExit) labels.emplace(in.address + in.jump * 4, 0);
    int n = 0;
    for (auto& [addr, id] : labels) id = n++;

    std::string out;
    char buf[32];
    for (auto& in : code) {
        auto it = labels.find(in.address);
        if (it != labels.end()) out += "L" + std::to_string(it->second) + ":\n";
        snprintf(buf, sizeof buf, "  %05x  ", in.address - entry.bytecode);
        out += buf;
        out += formatInstruction(dw, in);
        if (in.isBranch()) {
            if (in.popenvExit) out += " <exit>";
            else out += " L" + std::to_string(labels[in.address + in.jump * 4]);
        }
        out += '\n';
    }
    auto it = labels.find(entry.bytecode + entry.length);
    if (it != labels.end()) out += "L" + std::to_string(it->second) + ":  (end)\n";
    return out;
}

}  // namespace gm
