// GCN "Sea Islands" (GFX7) instruction model. This is the ISA the PS4's GPU
// runs; the offline translator decodes each shader with it. Field layouts
// follow AMD's public "Sea Islands Series ISA" reference.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gcn {

enum class Enc : std::uint8_t {
    SOP2,
    SOPK,
    SOP1,
    SOPC,
    SOPP,
    SMRD,
    VOP2,
    VOP1,
    VOPC,
    VOP3,
    VINTRP,
    DS,
    MUBUF,
    MTBUF,
    MIMG,
    EXP,
    Unknown,
};

// 9-bit scalar/vector source operand codes.
enum Operand : std::uint16_t {
    kSgpr0 = 0,          // 0..103
    kFlatScratchLo = 102,
    kFlatScratchHi = 103,
    kVccLo = 106,
    kVccHi = 107,
    kTbaLo = 108,
    kTbaHi = 109,
    kTmaLo = 110,
    kTmaHi = 111,
    kTtmp0 = 112,        // 112..123
    kM0 = 124,
    kExecLo = 126,
    kExecHi = 127,
    kConst0 = 128,       // 128 = 0, 129..192 = 1..64, 193..208 = -1..-16
    kConstHalf = 240,    // 0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0
    kVccz = 251,
    kExecz = 252,
    kScc = 253,
    kLdsDirect = 254,
    kLiteral = 255,
    kVgpr0 = 256,        // 256..511
};

struct Inst {
    Enc enc = Enc::Unknown;
    std::uint16_t op = 0;       // encoding-local opcode
    std::uint32_t words[3] = {0, 0, 0};
    std::uint8_t size = 0;      // dwords consumed (1..3)
    std::uint32_t offset = 0;   // byte offset in the program
    bool has_literal = false;
    std::uint32_t literal = 0;

    // Common operand fields; which ones matter depends on `enc`.
    std::uint16_t src0 = 0, src1 = 0, src2 = 0;  // 9-bit operand codes (VOP*: src1/2 are VGPRs)
    std::uint16_t dst = 0;                       // SGPR/VGPR index, or SDST operand code for scalar
    std::uint16_t sdst = 0;                      // VOP3b carry-out / SMRD sdst
    std::int32_t imm = 0;                        // SOPK/SOPP simm16 (sign-extended), SMRD offset
    // VOP3 modifiers
    std::uint8_t abs = 0, neg = 0, omod = 0;
    bool clamp = false;
    // Memory encodings
    std::uint16_t vaddr = 0, vdata = 0, srsrc = 0, ssamp = 0, soffset = 0;
    std::uint16_t offset12 = 0;   // MUBUF/MTBUF 12-bit offset
    std::uint8_t offset0 = 0, offset1 = 0;  // DS
    bool offen = false, idxen = false, glc = false, slc = false, tfe = false, lds = false, addr64 = false;
    bool gds = false, unorm = false, da = false, r128 = false, lwe = false;
    std::uint8_t dmask = 0;       // MIMG / EXP enable
    std::uint8_t dfmt = 0, nfmt = 0;  // MTBUF
    bool imm_flag = false;        // SMRD IMM
    // VINTRP
    std::uint8_t attr = 0, attr_chan = 0;
    // EXP
    std::uint8_t tgt = 0;
    bool compr = false, done = false, vm = false;
    std::uint8_t vsrc[4] = {0, 0, 0, 0};
};

struct DecodeError {
    std::uint32_t offset;
    std::string what;
};

struct Program {
    std::vector<Inst> insts;
    std::vector<DecodeError> errors;
    // Byte offset just past the last decoded instruction; trailing padding follows.
    std::uint32_t end = 0;
};

// Decode one instruction at `words[i]`; `n` dwords available. Returns false
// on an unknown encoding (size still set to 1).
bool decode_one(const std::uint32_t* words, std::size_t n, std::size_t i, Inst& out);

// Decode a whole program linearly. Stops after the last instruction; a run of
// s_nop/zero padding at the end is not reported.
Program decode(const std::uint32_t* words, std::size_t n);

// Mnemonic for an instruction, or nullptr when the opcode is undefined in the
// Sea Islands ISA.
const char* mnemonic(const Inst& in);
const char* enc_name(Enc e);

// LLVM-style textual disassembly of one instruction.
std::string format(const Inst& in);

// Helpers for operand codes.
bool is_vgpr(std::uint16_t code);
bool is_sgpr(std::uint16_t code);
std::string operand_name(std::uint16_t code, std::uint32_t literal = 0, int dwords = 1);

}  // namespace gcn
