//===- AArch64_NativeDecodeParityTests.cpp - native decode vs capstone ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Differential test locking the two safety invariants of the Capstone-free
/// operand decoder in neverd/decode/AArch64NativeDecode.h, with Capstone as
/// the oracle:
///
///   1. Strict subset — for every word tryDecode() accepts, Capstone also
///      decodes it (so the fast path never turns a Capstone reject into an
///      accept, the property function-entry verification relies on).
///   2. Lift parity  — lifting the native cs_insn produces LowIR byte-identical
///      to lifting the Capstone cs_insn for the same word.
///
/// Being a correctness test, its verdict is independent of machine load (unlike
/// the throughput numbers the decode benchmark reports).  It exercises
/// hand-picked encodings, exhaustive sweeps over each class's selector fields,
/// a large deterministic random sweep, and an optional real-binary walk
/// (NEVERD_PARITY_BIN).
///
//===----------------------------------------------------------------------===//

#include "neverd/decode/AArch64NativeDecode.h"
#include "neverd/lift/AArch64Lifter.h"

#include <capstone/capstone.h>

#include "gtest/gtest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace neverd;

namespace {

// LowOp equality restricted to the fields that define the lifted semantics:
// opcode, output vnode, and the input vnodes.  (Addr is set from the insn
// address, which both paths share; Seq is positional and checked implicitly by
// comparing the vectors in order.)
bool lowOpEq(const LowOp &A, const LowOp &B) {
  if (A.Opcode != B.Opcode || A.NumInputs != B.NumInputs ||
      A.Output != B.Output || A.Addr != B.Addr)
    return false;
  for (unsigned I = 0; I < A.NumInputs; ++I)
    if (A.Inputs[I] != B.Inputs[I])
      return false;
  return true;
}

std::string vnStr(const NdVar &V) {
  const char *Sp = V.isReg()    ? "reg"
                   : V.isTemp() ? "tmp"
                   : V.isRam()  ? "ram"
                                : "cst";
  char Buf[64];
  std::snprintf(Buf, sizeof(Buf), "%s:0x%llx/%u", Sp,
                (unsigned long long)V.Offset, V.Size);
  return Buf;
}

std::string opsStr(const std::vector<LowOp> &Ops) {
  std::string S;
  char Buf[96];
  for (const auto &O : Ops) {
    std::snprintf(Buf, sizeof(Buf), "  op#%d out=%s <-", (int)O.Opcode,
                  vnStr(O.Output).c_str());
    S += Buf;
    for (unsigned I = 0; I < O.NumInputs; ++I)
      S += " " + vnStr(O.Inputs[I]);
    S += "\n";
  }
  return S;
}

class Oracle {
public:
  Oracle() {
    if (cs_open(CS_ARCH_AARCH64, CS_MODE_ARM, &H) != CS_ERR_OK)
      H = 0;
    else {
      cs_option(H, CS_OPT_DETAIL, CS_OPT_ON);
      Insn = cs_malloc(H);
    }
  }
  ~Oracle() {
    if (Insn)
      cs_free(Insn, 1);
    if (H)
      cs_close(&H);
  }
  bool ok() const { return H != 0 && Insn != nullptr; }

  // Decode+lift with capstone; returns false if capstone rejects the word.
  bool lift(uint32_t Word, va_t Addr, std::vector<LowOp> &Ops) const {
    uint8_t Code[4] = {static_cast<uint8_t>(Word),
                       static_cast<uint8_t>(Word >> 8),
                       static_cast<uint8_t>(Word >> 16),
                       static_cast<uint8_t>(Word >> 24)};
    const uint8_t *P = Code;
    size_t Sz = 4;
    uint64_t A = Addr;
    if (!cs_disasm_iter(H, &P, &Sz, &A, Insn))
      return false;
    AArch64Lifter::fixupDecodedInsn(Insn);
    AArch64Lifter L(Arch::AArch64);
    L.setStrict(false);
    Ops.clear();
    L.lift(Insn, Ops);
    return true;
  }

private:
  csh H = 0;
  cs_insn *Insn = nullptr;
};

// Lift a native-decoded word; caller has already confirmed tryDecode accepted.
void nativeLift(uint32_t Word, va_t Addr, const cs_insn &NI,
                std::vector<LowOp> &Ops) {
  AArch64Lifter L(Arch::AArch64);
  L.setStrict(false);
  Ops.clear();
  L.lift(&NI, Ops);
  (void)Word;
  (void)Addr;
}

// Check one word.  Returns "" on success, else a human-readable diagnostic.
// On any word the native decoder accepts, capstone must also decode it, and
// the two lifts must be identical.
std::string checkOne(const Oracle &O, uint32_t Word, va_t Addr) {
  cs_insn NI{};
  cs_detail ND{};
  if (!a64native::tryDecode(Word, Addr, NI, ND))
    return ""; // declined -> nothing to prove (capstone handles it)

  std::vector<LowOp> CsOps;
  if (!O.lift(Word, Addr, CsOps)) {
    char Buf[96];
    std::snprintf(Buf, sizeof(Buf),
                  "STRICT-SUBSET VIOLATION: native accepted w=0x%08X but "
                  "capstone rejected",
                  Word);
    return Buf;
  }
  std::vector<LowOp> NatOps;
  nativeLift(Word, Addr, NI, NatOps);

  if (NatOps.size() != CsOps.size() ||
      !std::equal(NatOps.begin(), NatOps.end(), CsOps.begin(), lowOpEq)) {
    char Buf[128];
    std::snprintf(Buf, sizeof(Buf),
                  "LIFT MISMATCH w=0x%08X va=0x%llx id=%u\n--- capstone ---\n",
                  Word, (unsigned long long)Addr, NI.id);
    return std::string(Buf) + opsStr(CsOps) + "--- native ---\n" +
           opsStr(NatOps);
  }
  return "";
}

// Fill every operand slot of a reused buffer with a poison pattern the lifter
// would visibly mis-lift if it ever read a slot the current decode did not
// write (a real register with a shift, plus writeback/cc set).  Used to prove
// begin() need not bulk-zero operands[]: since it only resets the header and
// each decode fully writes operands[0, op_count), a stale poisoned slot beyond
// op_count must never reach the lift.
void poison(cs_insn &NI, cs_detail &ND) {
  NI.detail = &ND;
  ND.writeback = true;
  cs_aarch64 &A = ND.aarch64;
  A.cc = AArch64CC_LT;
  A.update_flags = true;
  A.post_index = true;
  A.op_count = 16;
  for (int I = 0; I < 16; ++I) {
    cs_aarch64_op &Op = A.operands[I];
    Op = cs_aarch64_op{};
    Op.type = AARCH64_OP_REG;
    Op.reg = AARCH64_REG_X7;
    Op.vector_index = 3;
    Op.vas = AARCH64LAYOUT_VL_2D;
    Op.shift.type = AARCH64_SFT_LSL;
    Op.shift.value = 7;
    Op.ext = AARCH64_EXT_SXTX;
    Op.access = CS_AC_READ_WRITE;
    Op.imm = 0x7BADBADBAD;
  }
}

// Like checkOne, but decodes into the caller's REUSED, pre-poisoned buffer
// (mirroring Decoder::InsnBuf, which persists across decodes) instead of a
// fresh zeroed one.  A regression that let a stale operand leak past op_count
// surfaces here as a lift mismatch even though checkOne (fresh buffer) passes.
std::string checkOneReuse(const Oracle &O, cs_insn &NI, cs_detail &ND,
                          uint32_t Word, va_t Addr) {
  poison(NI, ND);
  if (!a64native::tryDecode(Word, Addr, NI, ND))
    return "";
  std::vector<LowOp> CsOps;
  if (!O.lift(Word, Addr, CsOps)) {
    char Buf[96];
    std::snprintf(Buf, sizeof(Buf),
                  "STRICT-SUBSET VIOLATION (reuse): native accepted w=0x%08X "
                  "but capstone rejected",
                  Word);
    return Buf;
  }
  std::vector<LowOp> NatOps;
  nativeLift(Word, Addr, NI, NatOps);
  if (NatOps.size() != CsOps.size() ||
      !std::equal(NatOps.begin(), NatOps.end(), CsOps.begin(), lowOpEq)) {
    char Buf[128];
    std::snprintf(Buf, sizeof(Buf),
                  "REUSED-BUFFER STALE-OPERAND LEAK w=0x%08X id=%u\n"
                  "--- capstone ---\n",
                  Word, NI.id);
    return std::string(Buf) + opsStr(CsOps) + "--- native ---\n" +
           opsStr(NatOps);
  }
  return "";
}

} // namespace

class A64NativeParity : public ::testing::Test {
protected:
  Oracle O;
  void SetUp() override {
    if (!O.ok())
      GTEST_SKIP() << "capstone AArch64 unavailable";
  }
};

TEST_F(A64NativeParity, EdgeEncodings) {
  const va_t A = 0x100000;
  const uint32_t Words[] = {
      0x10000000u, // adr x0, .
      0x10000060u, // adr x0, .+12
      0x70000123u, // adr x3, (immlo=11)
      0x90000000u, // adrp x0, .
      0x90000123u, // adrp x3
      0xB0000123u, // adrp x3 (immlo set)
      0x1000001Fu, // adr xzr
      0xD2800000u, // movz x0, #0
      0xD2800020u, // movz x0, #1
      0xD2A00020u, // movz x0, #1, lsl 16
      0xD2C00020u, // movz x0, #1, lsl 32
      0xD2E00020u, // movz x0, #1, lsl 48
      0x52800020u, // movz w0, #1
      0x52A00020u, // movz w0, #1, lsl 16
      0x92800000u, // movn x0, #0  (-> -1)
      0x92800020u, // movn x0, #1
      0x12800020u, // movn w0, #1
      0xF2800020u, // movk x0, #1
      0xF2A00020u, // movk x0, #1, lsl 16
      0x72800020u, // movk w0, #1
      0x7280001Fu, // movk wzr, #0
      0x529FFFE0u, // movz w0, #0xffff
      0xD29FFFE0u, // movz x0, #0xffff
      0xD503201Fu, // nop
      0x92400400u, // and x0, x0, #3
      0x12000000u, // and w0, w0, #1
      0xF240001Fu, // tst x0, #1  (ands zr alias)
      0x92000000u, // and x0, x0, #0x100000001
  };
  for (uint32_t W : Words) {
    std::string D = checkOne(O, W, A);
    EXPECT_TRUE(D.empty()) << D;
  }
}

TEST_F(A64NativeParity, AdrAdrpSweep) {
  const va_t A = 0x210000; // deliberately not page-aligned
  const uint32_t ImmHiSamples[] = {0u,     1u,      2u,      0x3FFFEu,
                                   0x3FFFFu, 0x40000u, 0x7FFFEu, 0x7FFFFu};
  for (uint32_t Op = 0; Op < 2; ++Op)
    for (uint32_t Rd = 0; Rd < 32; ++Rd)
      for (uint32_t ImmLo = 0; ImmLo < 4; ++ImmLo)
        for (uint32_t ImmHi : ImmHiSamples) {
          uint32_t W = (Op << 31) | (ImmLo << 29) | 0x10000000u |
                       ((ImmHi & 0x7FFFF) << 5) | Rd;
          std::string D = checkOne(O, W, A);
          EXPECT_TRUE(D.empty()) << D;
        }
}

// Exhaustive over the wide-move selector fields (opc, sf, hw, Rd) crossed with
// imm16 samples — covers MOVN/MOVZ/MOVK including the UNALLOCATED 32-bit hw>=2
// forms native must decline.
TEST_F(A64NativeParity, MoveWideSweep) {
  const va_t A = 0x300000;
  const uint32_t Imm16Samples[] = {0u,      1u,      2u,      0x1234u,
                                   0x7FFFu, 0x8000u, 0xABCDu, 0xFFFFu};
  for (uint32_t Opc = 0; Opc < 4; ++Opc)      // 01 must be declined
    for (uint32_t Sf = 0; Sf < 2; ++Sf)
      for (uint32_t Hw = 0; Hw < 4; ++Hw)     // hw>=2 w/ sf=0 must be declined
        for (uint32_t Rd = 0; Rd < 32; ++Rd)
          for (uint32_t Imm : Imm16Samples) {
            uint32_t W = (Sf << 31) | (Opc << 29) | (0x25u << 23) |
                         (Hw << 21) | ((Imm & 0xFFFF) << 5) | Rd;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}

// Add/subtract immediate, incl. the MOV-to/from-SP, CMP and CMN alias
// boundaries (Rd/Rn==31, imm==0, S/sh bits).
TEST_F(A64NativeParity, AddSubImmSweep) {
  const va_t A = 0x500000;
  const uint32_t Imm12Samples[] = {0u, 1u, 0x30u, 0x7FFu, 0xFFFu};
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)     // add / sub
      for (uint32_t S = 0; S < 2; ++S)      // flag-setting
        for (uint32_t Sh = 0; Sh < 2; ++Sh) // lsl #12
          for (uint32_t Rd : {0u, 1u, 29u, 30u, 31u})
            for (uint32_t Rn : {0u, 5u, 31u})
              for (uint32_t Imm : Imm12Samples) {
                uint32_t W = (Sf << 31) | (Op << 30) | (S << 29) |
                             (0x11u << 24) | (Sh << 22) | ((Imm & 0xFFF) << 10) |
                             (Rn << 5) | Rd;
                std::string D = checkOne(O, W, A);
                EXPECT_TRUE(D.empty()) << D;
              }
}

// Add/subtract shifted register + logical shifted register, incl. the CMP/CMN,
// NEG/NEGS, MOV/MVN/TST alias boundaries and the ROR/oversized-shift declines.
TEST_F(A64NativeParity, DpRegSweep) {
  const va_t A = 0x600000;
  const uint32_t Imm6Samples[] = {0u, 1u, 4u, 31u, 32u, 63u};
  // Add/sub shifted register: op0=0b01011, bit21=0.
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t S = 0; S < 2; ++S)
        for (uint32_t Sh = 0; Sh < 4; ++Sh) // 3 (ROR) must be declined
          for (uint32_t Rd : {0u, 1u, 31u})
            for (uint32_t Rn : {0u, 31u})
              for (uint32_t Rm : {1u, 2u})
                for (uint32_t Imm6 : Imm6Samples) {
                  uint32_t W = (Sf << 31) | (Op << 30) | (S << 29) |
                               (0x0Bu << 24) | (Sh << 22) | (Rm << 16) |
                               ((Imm6 & 0x3F) << 10) | (Rn << 5) | Rd;
                  std::string D = checkOne(O, W, A);
                  EXPECT_TRUE(D.empty()) << D;
                }
  // Logical shifted register: op0=0b01010, all shift types + N bit.
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t Sh = 0; Sh < 4; ++Sh)
        for (uint32_t N = 0; N < 2; ++N)
          for (uint32_t Rd : {0u, 1u, 31u})
            for (uint32_t Rn : {0u, 31u})
              for (uint32_t Rm : {1u, 2u})
                for (uint32_t Imm6 : Imm6Samples) {
                  uint32_t W = (Sf << 31) | (Opc << 29) | (0x0Au << 24) |
                               (Sh << 22) | (N << 21) | (Rm << 16) |
                               ((Imm6 & 0x3F) << 10) | (Rn << 5) | Rd;
                  std::string D = checkOne(O, W, A);
                  EXPECT_TRUE(D.empty()) << D;
                }
}

// Load/store register (unsigned immediate offset): all size x opc combinations
// (covering the byte/half/word/dword widths, the sign-extending loads, and the
// declined PRFM/UNALLOCATED slots), crossed with base/transfer register and
// scaled-displacement samples.  V==1 (FP/SIMD) is not generated here since it
// must be declined; the random sweep exercises that path.
TEST_F(A64NativeParity, LoadStoreUImmSweep) {
  const va_t A = 0x700000;
  const uint32_t Imm12Samples[] = {0u, 1u, 2u, 8u, 0x555u, 0xFFFu};
  for (uint32_t Size = 0; Size < 4; ++Size)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t Rt : {0u, 1u, 30u, 31u})
        for (uint32_t Rn : {0u, 9u, 31u})
          for (uint32_t Imm : Imm12Samples) {
            uint32_t W = (Size << 30) | 0x39000000u | (Opc << 22) |
                         ((Imm & 0xFFF) << 10) | (Rn << 5) | Rt;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}

// Control flow: B/BL immediate range, B.cond over every condition (+ the BC.cond
// decline), CBZ/CBNZ, TBZ/TBNZ over the bit-position range, and RET/BR/BLR over
// the register field.
TEST_F(A64NativeParity, ControlFlowSweep) {
  const va_t A = 0x800000;
  const uint32_t Imm26[] = {0u, 1u, 0x21u, 0x1FFFFFFu, 0x2000000u, 0x3FFFFFFu};
  for (uint32_t Imm : Imm26) {
    EXPECT_TRUE(checkOne(O, 0x14000000u | (Imm & 0x3FFFFFF), A).empty());
    EXPECT_TRUE(checkOne(O, 0x94000000u | (Imm & 0x3FFFFFF), A).empty());
  }
  // B.cond / BC.cond across cond (bits[3:0]) and o0 (bit4).
  for (uint32_t Cond = 0; Cond < 16; ++Cond)
    for (uint32_t O0 = 0; O0 < 2; ++O0)
      for (uint32_t Imm19 : {0u, 1u, 0x3FFFFu, 0x7FFFFu}) {
        uint32_t W = 0x54000000u | ((Imm19 & 0x7FFFF) << 5) | (O0 << 4) | Cond;
        EXPECT_TRUE(checkOne(O, W, A).empty());
      }
  // CBZ/CBNZ (sf, op) x register x imm19 samples.
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t Rt : {0u, 1u, 31u})
        for (uint32_t Imm19 : {0u, 1u, 0x7FFFFu}) {
          uint32_t W = (Sf << 31) | 0x34000000u | (Op << 24) |
                       ((Imm19 & 0x7FFFF) << 5) | Rt;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
  // TBZ/TBNZ (b5, op) x bit-low x register x imm14 samples.
  for (uint32_t B5 = 0; B5 < 2; ++B5)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t B40 = 0; B40 < 32; B40 += 7)
        for (uint32_t Rt : {0u, 31u})
          for (uint32_t Imm14 : {0u, 1u, 0x3FFFu}) {
            uint32_t W = (B5 << 31) | 0x36000000u | (Op << 24) | (B40 << 19) |
                         ((Imm14 & 0x3FFF) << 5) | Rt;
            EXPECT_TRUE(checkOne(O, W, A).empty());
          }
  // RET/BR/BLR over Rn.
  for (uint32_t Rn = 0; Rn < 32; ++Rn) {
    EXPECT_TRUE(checkOne(O, 0xD65F0000u | (Rn << 5), A).empty());
    EXPECT_TRUE(checkOne(O, 0xD61F0000u | (Rn << 5), A).empty());
    EXPECT_TRUE(checkOne(O, 0xD63F0000u | (Rn << 5), A).empty());
  }
}

// Load/store pair: opc (32/64/declined) x mode (post/offset/pre/declined) x
// load-store x register x signed imm7 samples.
TEST_F(A64NativeParity, LoadStorePairSweep) {
  const va_t A = 0x900000;
  const uint32_t Imm7[] = {0u, 1u, 2u, 0x3Fu, 0x40u, 0x7Eu, 0x7Fu};
  for (uint32_t Opc = 0; Opc < 4; ++Opc)
    for (uint32_t Mode = 0; Mode < 4; ++Mode) // 0=STNP(declined),1,2,3
      for (uint32_t L = 0; L < 2; ++L)
        for (uint32_t Rt : {0u, 1u, 31u})
          for (uint32_t Rt2 : {2u, 30u})
            for (uint32_t Rn : {0u, 31u})
              for (uint32_t Imm : Imm7) {
                uint32_t W = (Opc << 30) | (0x5u << 27) | (Mode << 23) |
                             (L << 22) | ((Imm & 0x7F) << 15) | (Rt2 << 10) |
                             (Rn << 5) | Rt;
                std::string D = checkOne(O, W, A);
                EXPECT_TRUE(D.empty()) << D;
              }
}

// Logical immediate (AND/ORR/EOR/ANDS + TST alias): sweep sf/opc/N/immr/imms
// exhaustively over the small field space so every valid bitmask encoding (and
// every UNDEFINED one, which must be declined) is exercised, across a couple of
// Rd/Rn choices that select the plain / ZR / SP register forms.
TEST_F(A64NativeParity, LogicalImmSweep) {
  const va_t A = 0xA00000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t N = 0; N < 2; ++N)
        for (uint32_t Immr = 0; Immr < 64; Immr += 5)
          for (uint32_t Imms = 0; Imms < 64; Imms += 3)
            for (uint32_t Regs : {0u, 0x3E0u /*Rn=31*/, 0x1Fu /*Rd=31*/}) {
              uint32_t W = (Sf << 31) | (Opc << 29) | (0x24u << 23) |
                           (N << 22) | (Immr << 16) | (Imms << 10) | Regs;
              std::string D = checkOne(O, W, A);
              EXPECT_TRUE(D.empty()) << D;
            }
}

// Conditional select + its cset/csetm/cinc/cinv/cneg aliases across all
// conditions and register combinations, plus MADD/MSUB/MUL/MNEG, SMULH/UMULH,
// and SDIV/UDIV.
TEST_F(A64NativeParity, CondSelectAndMulDivSweep) {
  const va_t A = 0xB00000;
  // Conditional select: op(bit30) x op2(bit10) x cond x (Rm,Rn) selecting the
  // canonical / cset / cinc / cneg forms.
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t Op2 = 0; Op2 < 2; ++Op2)
        for (uint32_t Cond = 0; Cond < 16; ++Cond)
          for (auto RmRn : {std::pair<uint32_t, uint32_t>{2u, 3u},
                            {31u, 31u}, {5u, 5u}, {31u, 7u}}) {
            uint32_t W = (Sf << 31) | (Op << 30) | (0xD4u << 21) |
                         (RmRn.first << 16) | (Cond << 12) | (Op2 << 10) |
                         (RmRn.second << 5) | 1u;
            EXPECT_TRUE(checkOne(O, W, A).empty());
          }
  // Multiply-accumulate (3-source) and the mul/mneg aliases; smulh/umulh.
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op31 = 0; Op31 < 8; ++Op31)
      for (uint32_t O0 = 0; O0 < 2; ++O0)
        for (uint32_t Ra : {0u, 4u, 31u}) {
          uint32_t W = (Sf << 31) | (0x1Bu << 24) | (Op31 << 21) | (2u << 16) |
                       (O0 << 15) | (Ra << 10) | (1u << 5) | 0u;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
  // Data-processing (2-source): sweep opcode to hit SDIV/UDIV (taken) and the
  // shift-variable / CRC forms (declined).
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op2 = 0; Op2 < 24; ++Op2) {
      uint32_t W = (Sf << 31) | (0x0D6u << 21) | (2u << 16) | (Op2 << 10) |
                   (1u << 5) | 0u;
      EXPECT_TRUE(checkOne(O, W, A).empty());
    }
}

// Bitfield move (SBFM/BFM/UBFM) exhaustively over opc x sf x immr x imms, for
// both a real source register and Rn==31, so every preferred-alias boundary
// (lsl/lsr/asr, uxtb/uxth, sxtb/sxth/sxtw, ubfx/ubfiz, sbfx/sbfiz, bfi/bfxil)
// and every declined case (Rn==31 BFM, 32-bit oversized field) is lifted both
// ways.  This is the class where the lifter reads the alias mnemonic/id, so
// only this lift-parity check (not the field probe) proves it.
TEST_F(A64NativeParity, BitfieldSweep) {
  const va_t A = 0xC00000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf) {
    const uint32_t Hi = Sf ? 64u : 32u;
    for (uint32_t Opc = 0; Opc < 3; ++Opc)
      for (uint32_t Rn : {1u, 31u})
        for (uint32_t Immr = 0; Immr < Hi; ++Immr)
          for (uint32_t Imms = 0; Imms < Hi; ++Imms) {
            uint32_t W = (Sf << 31) | (Opc << 29) | (0x26u << 23) |
                         (Sf << 22) | (Immr << 16) | (Imms << 10) | (Rn << 5) |
                         2u;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
  }
}

// FP/SIMD load/store (unsigned immediate offset): size x opc (covering the
// B/H/S/D scalar widths, the Q form, load/store, and the declined size!=0 Q
// slots) x transfer register x displacement.
TEST_F(A64NativeParity, FpLoadStoreSweep) {
  const va_t A = 0xD00000;
  const uint32_t Imm12Samples[] = {0u, 1u, 2u, 8u, 0xFFFu};
  for (uint32_t Size = 0; Size < 4; ++Size)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t Rt : {0u, 1u, 31u})
        for (uint32_t Rn : {0u, 9u, 31u})
          for (uint32_t Imm : Imm12Samples) {
            uint32_t W = (Size << 30) | 0x3D000000u | (Opc << 22) |
                         ((Imm & 0xFFF) << 10) | (Rn << 5) | Rt;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}

// Variable shifts (LSLV/LSRV/ASRV/RORV, via the 2-source opcodes) and EXTR
// (incl. the ROR-immediate alias when Rn==Rm).
TEST_F(A64NativeParity, VarShiftAndExtrSweep) {
  const va_t A = 0xE00000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf) {
    for (uint32_t Op2 = 8; Op2 <= 11; ++Op2)
      for (uint32_t Rm : {0u, 2u, 31u})
        for (uint32_t Rn : {1u, 31u}) {
          uint32_t W = (Sf << 31) | (0x0D6u << 21) | (Rm << 16) | (Op2 << 10) |
                       (Rn << 5) | 3u;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
    // EXTR: N==sf, o0==0; sweep imms and Rm (Rn==Rm gives the ror alias).
    for (uint32_t Rm : {1u, 2u})
      for (uint32_t Rn : {1u, 5u})
        for (uint32_t Imms : {0u, 1u, 3u, 31u, 63u}) {
          uint32_t W = (Sf << 31) | (0x27u << 23) | (Sf << 22) | (Rm << 16) |
                       ((Imms & 0x3F) << 10) | (Rn << 5) | 0u;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
  }
}

// Add/subtract extended register (bit21==1): every option x imm3(amount) x sf
// x op x S, across register choices that select the plain / SP / ZR forms and
// the CMP/CMN and natural-LSL (dropped-extend) alias boundaries.
TEST_F(A64NativeParity, AddSubExtendedRegSweep) {
  const va_t A = 0x640000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t S = 0; S < 2; ++S)
        for (uint32_t Option = 0; Option < 8; ++Option)
          for (uint32_t Imm3 = 0; Imm3 < 8; ++Imm3) // 5..7 must be declined
            for (uint32_t Rd : {0u, 2u, 31u})
              for (uint32_t Rn : {0u, 31u})
                for (uint32_t Rm : {1u, 3u, 31u}) {
                  uint32_t W = (Sf << 31) | (Op << 30) | (S << 29) |
                               (0x0Bu << 24) | (1u << 21) | (Rm << 16) |
                               (Option << 13) | (Imm3 << 10) | (Rn << 5) | Rd;
                  std::string D = checkOne(O, W, A);
                  EXPECT_TRUE(D.empty()) << D;
                }
}

// Widening multiply-accumulate long (SMADDL/UMADDL/SMSUBL/UMSUBL) and their
// SMULL/UMULL/SMNEGL/UMNEGL (Ra==31) aliases, over sf (sf==0 declined), the
// op31/o0 selector, and the accumulator register.
TEST_F(A64NativeParity, WideningMulAddSweep) {
  const va_t A = 0xB80000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)      // sf==0 must be declined
    for (uint32_t Op31 = 0; Op31 < 8; ++Op31)
      for (uint32_t O0 = 0; O0 < 2; ++O0)
        for (uint32_t Rm : {0u, 2u, 31u})
          for (uint32_t Ra : {0u, 4u, 31u})
            for (uint32_t Rn : {1u, 31u}) {
              uint32_t W = (Sf << 31) | (0x1Bu << 24) | (Op31 << 21) |
                           (Rm << 16) | (O0 << 15) | (Ra << 10) | (Rn << 5) | 5u;
              std::string D = checkOne(O, W, A);
              EXPECT_TRUE(D.empty()) << D;
            }
}

// FMOV scalar: register (Vd,Vn), general (GPR<->FP), and the 8-bit scalar
// immediate, over ptype (S/D/H + the UNALLOCATED 10), the rmode/opcode selector
// (top-half rmode!=0 declined) and every imm8 for the immediate form.
TEST_F(A64NativeParity, FmovScalarSweep) {
  const va_t A = 0xF00000;
  // Register + general: sweep M(sf), ptype, rmode, opcode, and reg fields.
  for (uint32_t M = 0; M < 2; ++M)
    for (uint32_t Ptype = 0; Ptype < 4; ++Ptype)
      for (uint32_t Rmode = 0; Rmode < 4; ++Rmode)
        for (uint32_t Opcode = 0; Opcode < 8; ++Opcode)
          for (uint32_t Rn : {0u, 1u, 31u})
            for (uint32_t Rd : {0u, 2u, 31u}) {
              uint32_t W = (M << 31) | (0x1Eu << 24) | (Ptype << 22) |
                           (1u << 21) | (Rmode << 19) | (Opcode << 16) |
                           (Rn << 5) | Rd;
              std::string D = checkOne(O, W, A);
              EXPECT_TRUE(D.empty()) << D;
            }
  // FP data-processing 1-source (opcode(20:15)==0, bits[14:10]==0b10000) hits
  // the FMOV-register form; sweep ptype and registers.
  for (uint32_t Ptype = 0; Ptype < 4; ++Ptype)
    for (uint32_t Rn = 0; Rn < 32; ++Rn)
      for (uint32_t Rd : {0u, 31u}) {
        uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) | (0x10u << 10) |
                     (Rn << 5) | Rd;
        std::string D = checkOne(O, W, A);
        EXPECT_TRUE(D.empty()) << D;
      }
  // Scalar immediate: ptype x every imm8 x a couple of Rd (+ a nonzero imm5
  // field, which must be declined).
  for (uint32_t Ptype = 0; Ptype < 4; ++Ptype)
    for (uint32_t Imm8 = 0; Imm8 < 256; ++Imm8)
      for (uint32_t Imm5 : {0u, 1u})
        for (uint32_t Rd : {0u, 5u, 31u}) {
          uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) |
                       (Imm8 << 13) | (0x4u << 10) | (Imm5 << 5) | Rd;
          std::string D = checkOne(O, W, A);
          EXPECT_TRUE(D.empty()) << D;
        }
}

// ORR (immediate) MOV-bitmask alias: sweep sf/immr/imms/N with Rn==31 (the
// MOV/ORR fork) plus a couple of Rd (SP-form) choices, so every bitmask value
// crosses the MoveWidePreferred boundary that decides mov vs orr rendering.
TEST_F(A64NativeParity, OrrMovBitmaskSweep) {
  const va_t A = 0xA80000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t N = 0; N < 2; ++N)
      for (uint32_t Immr = 0; Immr < 64; ++Immr)
        for (uint32_t Imms = 0; Imms < 64; ++Imms)
          for (uint32_t Rd : {0u, 5u, 31u}) {
            uint32_t W = (Sf << 31) | (0x01u << 29) | (0x24u << 23) |
                         (N << 22) | (Immr << 16) | (Imms << 10) | (31u << 5) |
                         Rd;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}

// Data-processing (1 source) integer: RBIT/REV16/REV32/REV/CLZ/CLS over sf and
// opcode (incl. opcode2!=0 and opcode>=6, which must be declined).
TEST_F(A64NativeParity, DataProc1SourceSweep) {
  const va_t A = 0xCC0000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Opcode2 = 0; Opcode2 < 2; ++Opcode2) // 1 -> reserved
      for (uint32_t Opcode = 0; Opcode < 8; ++Opcode)  // 6,7 -> declined
        for (uint32_t Rn : {0u, 31u})
          for (uint32_t Rd : {0u, 31u}) {
            uint32_t W = (Sf << 31) | (0x2D6u << 21) | (Opcode2 << 16) |
                         (Opcode << 10) | (Rn << 5) | Rd;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}

// Scalar FP data-processing: 1-source (FABS/FNEG/FSQRT/FCVT), 2-source
// (FMUL/FDIV/FADD/FSUB/FMAX/FMIN/FMAXNM/FMINNM/FNMUL), and FCMP/FCMPE (register
// and zero forms) across ptype (S/D/H + the UNALLOCATED 10) and the opcode
// fields, so every taken/declined boundary is lifted both ways.
TEST_F(A64NativeParity, FpScalarDataProcSweep) {
  const va_t A = 0xF80000;
  for (uint32_t Ptype = 0; Ptype < 4; ++Ptype) {
    // 1-source: opcode(20:15) 0..0x0F covers FMOV/FABS/FNEG/FSQRT + the FCVT
    // targets (0001TT) and some FRINT declines.
    for (uint32_t Opcode = 0; Opcode < 0x10; ++Opcode)
      for (uint32_t Rn : {0u, 31u})
        for (uint32_t Rd : {0u, 31u}) {
          uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) |
                       (Opcode << 15) | (0x10u << 10) | (Rn << 5) | Rd;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
    // 2-source: opcode(15:12) 0..0x0F (9..15 declined).
    for (uint32_t Opcode = 0; Opcode < 0x10; ++Opcode)
      for (uint32_t Rm : {1u, 31u})
        for (uint32_t Rn : {2u, 31u}) {
          uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) | (Rm << 16) |
                       (Opcode << 12) | (2u << 10) | (Rn << 5) | 0u;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
    // FP compare: op(15:14) x opcode2(4:0) x Rm x Rn.
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t Opcode2 = 0; Opcode2 < 0x20; ++Opcode2)
        for (uint32_t Rm : {0u, 2u})
          for (uint32_t Rn : {0u, 3u}) {
            uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) |
                         (Rm << 16) | (Op << 14) | (0x8u << 10) | (Rn << 5) |
                         Opcode2;
            EXPECT_TRUE(checkOne(O, W, A).empty());
          }
  }
}

// FP/SIMD load/store register, non-unsigned-offset forms: register-offset,
// unscaled (LDUR/STUR), and pre/post-index, over size x opc (B/H/S/D/Q widths,
// the Q-with-size!=0 declines) x sub-form.
TEST_F(A64NativeParity, FpLoadStoreMiscSweep) {
  const va_t A = 0xE80000;
  // Register offset (bit21==1, [11:10]==10): sweep size, opc, option, S.
  for (uint32_t Size = 0; Size < 4; ++Size)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t Option = 0; Option < 8; ++Option)
        for (uint32_t S = 0; S < 2; ++S)
          for (uint32_t Rt : {0u, 31u}) {
            uint32_t W = (Size << 30) | (0x1Cu << 24) | (Opc << 22) |
                         (1u << 21) | (2u << 16) | (Option << 13) | (S << 12) |
                         (2u << 10) | (1u << 5) | Rt;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
  // Unscaled / pre / post (bit21==0, [11:10] in 00/01/11): sweep size, opc,
  // sub-form, imm9 samples.
  for (uint32_t Size = 0; Size < 4; ++Size)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t Op4 : {0u, 1u, 2u, 3u})
        for (uint32_t Imm9 : {0u, 1u, 0xFFu, 0x100u, 0x1FFu})
          for (uint32_t Rt : {0u, 31u}) {
            uint32_t W = (Size << 30) | (0x1Cu << 24) | (Opc << 22) |
                         ((Imm9 & 0x1FF) << 12) | (Op4 << 10) | (1u << 5) | Rt;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}

// FP/SIMD load/store pair (S/D/Q) over opc x mode x load-store x imm7 samples.
TEST_F(A64NativeParity, FpLoadStorePairSweep) {
  const va_t A = 0x980000;
  const uint32_t Imm7[] = {0u, 1u, 2u, 0x3Fu, 0x40u, 0x7Eu, 0x7Fu};
  for (uint32_t Opc = 0; Opc < 4; ++Opc)         // 11 declined
    for (uint32_t Mode = 0; Mode < 4; ++Mode)    // 0=STNP/LDNP declined
      for (uint32_t L = 0; L < 2; ++L)
        for (uint32_t Rt : {0u, 31u})
          for (uint32_t Rt2 : {2u, 30u})
            for (uint32_t Rn : {0u, 31u})
              for (uint32_t Imm : Imm7) {
                uint32_t W = (Opc << 30) | (0x5u << 27) | (1u << 26) |
                             (Mode << 23) | (L << 22) | ((Imm & 0x7F) << 15) |
                             (Rt2 << 10) | (Rn << 5) | Rt;
                std::string D = checkOne(O, W, A);
                EXPECT_TRUE(D.empty()) << D;
              }
}

// Conditional compare (CCMP/CCMN, register and immediate) over sf/op/o2 x cond
// x Rn/Rm/imm5 x nzcv, plus the reserved o3/o1 bits (must be declined).
TEST_F(A64NativeParity, CondCompareSweep) {
  const va_t A = 0xC80000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t O2 = 0; O2 < 2; ++O2)     // reg / imm form
        for (uint32_t O3 = 0; O3 < 2; ++O3)   // reserved bit10
          for (uint32_t O1 = 0; O1 < 2; ++O1) // reserved bit4
            for (uint32_t Cond = 0; Cond < 16; ++Cond)
              for (uint32_t RmImm : {0u, 5u, 31u})
                for (uint32_t Rn : {0u, 31u})
                  for (uint32_t Nzcv : {0u, 0xFu}) {
                    uint32_t W = (Sf << 31) | (Op << 30) | (1u << 29) |
                                 (0xD2u << 21) | (RmImm << 16) | (Cond << 12) |
                                 (O2 << 11) | (O3 << 10) | (Rn << 5) | (O1 << 4) |
                                 Nzcv;
                    std::string D = checkOne(O, W, A);
                    EXPECT_TRUE(D.empty()) << D;
                  }
}

// BRK #imm16 over a range of immediates (id-only lift, but the strict-subset
// and single-operand detail are still locked).
TEST_F(A64NativeParity, BrkSweep) {
  const va_t A = 0xD80000;
  for (uint32_t Imm : {0u, 1u, 0x100u, 0x800u, 0xDEADu, 0xFFFFu}) {
    uint32_t W = 0xD4200000u | (Imm << 5);
    std::string D = checkOne(O, W, A);
    EXPECT_TRUE(D.empty()) << D;
  }
}

// begin() resets only the header of the reused cs_aarch64 and leaves the
// 16-slot operands[] array untouched (each decode fully writes operands[0,
// op_count)).  Decode a long, deterministic stream of every covered class into
// ONE poisoned, reused buffer and prove each still lifts identically to a fresh
// Capstone decode — i.e. no stale operand from a prior word ever leaks past the
// current word's op_count.  A regression that dropped the per-class operand
// init (or read past op_count) fails here while the fresh-buffer sweeps pass.
TEST_F(A64NativeParity, ReusedBufferNoStaleOperandLeak) {
  cs_insn NI{};
  cs_detail ND{};
  const va_t A = 0x1230000;
  std::mt19937 Rng(0xC0FFEEu);
  int Handled = 0;
  std::string First;
  // Interleave low- and high-op-count classes so a leaked slot from a 4-operand
  // decode (bitfield/extr/madd) would corrupt a following 2-operand decode.
  const uint32_t Seeds[] = {
      0x8B000000u, // add  x0, x0, x0            (3 ops)
      0xF9400000u, // ldr  x0, [x0]              (2 ops)
      0x93407C00u, // sxtw x0, w0                (2 ops, extend alias)
      0x9B007C00u, // mul  x0, x0, x0            (3 ops, madd alias)
      0x13007C00u, // extr/asr family            (varies)
      0xD65F03C0u, // ret                        (0 ops)
      0x14000000u, // b .                        (1 op)
      0xB3400000u, // bfxil/bfi                  (4 ops)
      0xD2800000u, // movz x0, #0                (2 ops)
      0x1A800400u, // csel/cinc family           (2-3 ops)
      0x8B220000u, // add x0, x0, w0, uxtb       (3 ops, extended reg)
      0x9B220000u, // smaddl/smull               (3-4 ops, widening)
      0x1E204000u, // fmov s0, s0                (2 ops, fp reg)
      0xB2400000u, // orr/mov (bitmask)          (2-3 ops)
  };
  for (int Iter = 0; Iter < 40000; ++Iter) {
    uint32_t Base = Seeds[Iter % (sizeof(Seeds) / sizeof(Seeds[0]))];
    // Perturb register/immediate fields but keep the class selector bits.
    uint32_t W = Base ^ (Rng() & 0x001FFFE0u);
    va_t Va = A + static_cast<va_t>(Iter) * 4;
    cs_insn Probe{};
    cs_detail PD{};
    if (a64native::tryDecode(W, Va, Probe, PD))
      ++Handled;
    std::string D = checkOneReuse(O, NI, ND, W, Va);
    if (!D.empty() && First.empty())
      First = D;
    EXPECT_TRUE(D.empty()) << D;
    if (!First.empty())
      break;
  }
  EXPECT_GT(Handled, 0);
}

// Any missed boundary anywhere in the 32-bit space surfaces as a mismatch or a
// strict-subset violation.  Fixed seed = reproducible.
TEST_F(A64NativeParity, RandomizedSweep) {
  std::mt19937 Rng(0x5EED1234u);
  const va_t Base = 0x40000000;
  constexpr int kWords = 400000;
  int Handled = 0, Mismatch = 0;
  std::string First;
  for (int I = 0; I < kWords; ++I) {
    uint32_t W = Rng();
    va_t A = Base + static_cast<va_t>(I) * 4;
    cs_insn NI{};
    cs_detail ND{};
    if (a64native::tryDecode(W, A, NI, ND))
      ++Handled;
    std::string D = checkOne(O, W, A);
    if (!D.empty()) {
      if (First.empty())
        First = D;
      ++Mismatch;
    }
  }
  EXPECT_EQ(Mismatch, 0) << "first: " << First;
  // Sanity: the random space should hit the covered classes many times, so a
  // zero here would mean the fast path silently stopped matching anything.
  EXPECT_GT(Handled, 0);
}

// Optional real-code corpus: NEVERD_PARITY_BIN=<raw arm64 bytes>.
TEST_F(A64NativeParity, OptionalRealBinaryWalk) {
  const char *Path = std::getenv("NEVERD_PARITY_BIN");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_PARITY_BIN to a raw arm64 code file to enable";
  std::ifstream F(Path, std::ios::binary);
  ASSERT_TRUE(F.good()) << "cannot open " << Path;
  std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(F)),
                             std::istreambuf_iterator<char>());
  ASSERT_GE(Bytes.size(), 4u);
  const va_t Base = 0x100000000ull;
  int Handled = 0, Mismatch = 0;
  std::string First;
  for (size_t Off = 0; Off + 4 <= Bytes.size(); Off += 4) {
    uint32_t W = static_cast<uint32_t>(Bytes[Off]) |
                 (static_cast<uint32_t>(Bytes[Off + 1]) << 8) |
                 (static_cast<uint32_t>(Bytes[Off + 2]) << 16) |
                 (static_cast<uint32_t>(Bytes[Off + 3]) << 24);
    va_t A = Base + Off;
    cs_insn NI{};
    cs_detail ND{};
    if (a64native::tryDecode(W, A, NI, ND))
      ++Handled;
    std::string D = checkOne(O, W, A);
    if (!D.empty()) {
      if (First.empty())
        First = D;
      ++Mismatch;
    }
  }
  EXPECT_EQ(Mismatch, 0) << "first: " << First;
  EXPECT_GT(Handled, 0) << "no covered instructions in corpus?";
}
