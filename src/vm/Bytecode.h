// GameMaker bytecode (versions 15+) instruction set and decoder.
//
// Each instruction is one little-endian 32-bit word:
//   bits 24-31 opcode, bits 16-19 type1, bits 20-23 type2, bits 0-15 operand
// followed by 0-2 extra words (immediates or a variable/function reference).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gm {

class DataWin;
struct CodeEntry;

enum class Op : uint8_t {
    Conv = 0x07, Mul = 0x08, Div = 0x09, Rem = 0x0A, Mod = 0x0B, Add = 0x0C, Sub = 0x0D,
    And = 0x0E, Or = 0x0F, Xor = 0x10, Neg = 0x11, Not = 0x12, Shl = 0x13, Shr = 0x14,
    Cmp = 0x15, Pop = 0x45, PushI = 0x84, Dup = 0x86, CallV = 0x99, Ret = 0x9C, Exit = 0x9D,
    Popz = 0x9E, B = 0xB6, Bt = 0xB7, Bf = 0xB8, PushEnv = 0xBA, PopEnv = 0xBB,
    Push = 0xC0, PushLoc = 0xC1, PushGlb = 0xC2, PushBltn = 0xC3, Call = 0xD9, Break = 0xFF,
};

enum class DataType : uint8_t {
    Double = 0, Float = 1, Int32 = 2, Int64 = 3, Boolean = 4, Variable = 5, String = 6,
    Instance = 7, Delete = 8, Undefined = 9, UnsignedInt = 10, Int16 = 15,
};

enum class CmpKind : uint8_t { None = 0, LT = 1, LE = 2, EQ = 3, NE = 4, GE = 5, GT = 6 };

// Special instance targets encoded in the operand of variable instructions.
enum InstanceTarget : int32_t {
    INST_SELF = -1, INST_OTHER = -2, INST_ALL = -3, INST_NOONE = -4, INST_GLOBAL = -5,
    INST_BUILTIN = -6, INST_LOCAL = -7, INST_STACKTOP = -9, INST_ARG = -15, INST_STATIC = -16,
};

// How a variable reference is accessed (top bits of the reference word).
// Variable access kinds (high byte of the reference word). MultiPush/MultiPushPop (GMS 2.3) push
// an array element as a container for multi-dimensional access (a[i][j]), read or write side.
enum class VarKind : uint8_t {
    Array = 0x00, StackTop = 0x80, Normal = 0xA0, Instance = 0xE0, MultiPush = 0x10, MultiPushPop = 0x90,
    Other = 0xFF
};

struct Instruction {
    uint32_t address = 0;  // absolute file offset
    Op op = Op::Break;
    DataType type1 = DataType::Double, type2 = DataType::Double;
    CmpKind cmp = CmpKind::None;
    int16_t operand = 0;       // instance target, argc, dup count or int16 immediate
    int32_t jump = 0;          // branch offset in words
    bool popenvExit = false;
    VarKind varKind = VarKind::Normal;
    int32_t ref = -1;          // VARI / FUNC index, or STRG index for string pushes
    union {
        double d;
        float f;
        int32_t i32;
        int64_t i64;
    } imm{};
    uint8_t words = 1;

    bool isBranch() const {
        return op == Op::B || op == Op::Bt || op == Op::Bf || op == Op::PushEnv || op == Op::PopEnv;
    }
};

const char* opName(Op op);
char typeChar(DataType t);
bool isKnownOp(uint8_t op);

// Decodes the instruction at an absolute file offset. Throws FormatError on garbage.
Instruction decode(const DataWin& dw, uint32_t address);

// Decodes a whole code entry, starting at its blob (not its entry offset).
std::vector<Instruction> decodeEntry(const DataWin& dw, const CodeEntry& entry);

// Human-readable listing, one instruction per line, with branch targets resolved to labels.
std::string disassemble(const DataWin& dw, const CodeEntry& entry);
std::string formatInstruction(const DataWin& dw, const Instruction& in);

}  // namespace gm
