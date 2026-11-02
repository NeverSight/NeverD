//===- AArch64_NativeDecodeParityTestsDetail.h - Decode parity harness
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The Capstone oracle, the word checkers, and the A64NativeParity
// fixture shared by the parity translation units.  The fixture lives in
// a named namespace so every TU agrees on one type for the suite.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_AARCH64_NATIVEDECODEPARITYTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_AARCH64_NATIVEDECODEPARITYTESTSDETAIL_H

#include "gtest/gtest.h"

#include "neverd/decode/AArch64/AArch64NativeDecode.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/AArch64/AArch64Lifter.h"

#include <algorithm>
#include <capstone/capstone.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace neverd::a64_parity_test {

#define PARITY_VALUE(Type, Name, Value) inline constexpr Type Name = Value;
#define PARITY_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64NativeDecodeParityFixtures.def"

enum class LiftOutcome { DecodeRejected, LiftRejected, Lifted };

/// Preserve a decoded instruction's explicit refusal. Other exceptions remain
/// test failures, and a permissive NOP cannot match a rejected instruction.
template <typename Lift> inline LiftOutcome liftOutcome(Lift &&PerformLift) {
  try {
    return PerformLift() ? LiftOutcome::Lifted : LiftOutcome::DecodeRejected;
  } catch (const UnliftedInstruction &) {
    return LiftOutcome::LiftRejected;
  }
}

// LowOp equality restricted to the fields that define the lifted semantics:
// opcode, output vnode, and the input vnodes.  (Addr is set from the insn
// address, which both paths share; Seq is positional and checked implicitly by
// comparing the vectors in order.)

inline bool lowOpEq(const LowOp &A, const LowOp &B) {
  if (A.Opcode != B.Opcode || A.NumInputs != B.NumInputs ||
      A.Output != B.Output || A.Addr != B.Addr ||
      A.MemoryOrdering != B.MemoryOrdering)
    return false;
  for (unsigned I = 0; I < A.NumInputs; ++I)
    if (A.Inputs[I] != B.Inputs[I])
      return false;
  return true;
}

inline bool matchingLifts(LiftOutcome Capstone, const std::vector<LowOp> &CsOps,
                          LiftOutcome Native,
                          const std::vector<LowOp> &NatOps) {
  return Capstone == Native && CsOps.size() == NatOps.size() &&
         std::equal(CsOps.begin(), CsOps.end(), NatOps.begin(), lowOpEq);
}

inline std::string outcomeMismatch(uint32_t Word, va_t Address,
                                   LiftOutcome Capstone, LiftOutcome Native) {
  char Buffer[kParityOutcomeDiagnosticBytes];
  std::snprintf(Buffer, sizeof(Buffer), kParityOutcomeMismatch, Word,
                static_cast<unsigned long long>(Address),
                static_cast<unsigned>(Capstone), static_cast<unsigned>(Native));
  return Buffer;
}

inline std::string vnStr(const NdVar &V) {
  const char *Sp = V.isReg()    ? "reg"
                   : V.isTemp() ? "tmp"
                   : V.isRam()  ? "ram"
                                : "cst";
  char Buf[64];
  std::snprintf(Buf, sizeof(Buf), "%s:0x%llx/%u", Sp,
                (unsigned long long)V.Offset, V.Size);
  return Buf;
}

inline std::string opsStr(const std::vector<LowOp> &Ops) {
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
    uint8_t Code[4] = {
        static_cast<uint8_t>(Word), static_cast<uint8_t>(Word >> 8),
        static_cast<uint8_t>(Word >> 16), static_cast<uint8_t>(Word >> 24)};
    const uint8_t *P = Code;
    size_t Sz = 4;
    uint64_t A = Addr;
    Insn->illegal = false;
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

inline void nativeLift(uint32_t Word, va_t Addr, const cs_insn &NI,
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

inline std::string checkOne(const Oracle &O, uint32_t Word, va_t Addr) {
  cs_insn NI{};
  cs_detail ND{};
  if (!a64native::tryDecode(Word, Addr, NI, ND))
    return ""; // declined -> nothing to prove (capstone handles it)

  std::vector<LowOp> CsOps;
  const auto Capstone = liftOutcome([&] { return O.lift(Word, Addr, CsOps); });
  if (Capstone == LiftOutcome::DecodeRejected) {
    char Buf[96];
    std::snprintf(Buf, sizeof(Buf),
                  "STRICT-SUBSET VIOLATION: native accepted w=0x%08X but "
                  "capstone rejected",
                  Word);
    return Buf;
  }
  std::vector<LowOp> NatOps;
  const auto Native = liftOutcome([&] {
    nativeLift(Word, Addr, NI, NatOps);
    return true;
  });
  if (Capstone != Native)
    return outcomeMismatch(Word, Addr, Capstone, Native);

  if (!matchingLifts(Capstone, CsOps, Native, NatOps)) {
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

inline void poison(cs_insn &NI, cs_detail &ND) {
  NI.detail = &ND;
  NI.illegal = true;
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

inline std::string checkOneReuse(const Oracle &O, cs_insn &NI, cs_detail &ND,
                                 uint32_t Word, va_t Addr) {
  poison(NI, ND);
  if (!a64native::tryDecode(Word, Addr, NI, ND))
    return "";
  std::vector<LowOp> CsOps;
  const auto Capstone = liftOutcome([&] { return O.lift(Word, Addr, CsOps); });
  if (Capstone == LiftOutcome::DecodeRejected) {
    char Buf[96];
    std::snprintf(Buf, sizeof(Buf),
                  "STRICT-SUBSET VIOLATION (reuse): native accepted w=0x%08X "
                  "but capstone rejected",
                  Word);
    return Buf;
  }
  std::vector<LowOp> NatOps;
  const auto Native = liftOutcome([&] {
    nativeLift(Word, Addr, NI, NatOps);
    return true;
  });
  if (Capstone != Native)
    return outcomeMismatch(Word, Addr, Capstone, Native);
  if (!matchingLifts(Capstone, CsOps, Native, NatOps)) {
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

class A64NativeParity : public ::testing::Test {
protected:
  Oracle O;
  void SetUp() override {
    if (!O.ok())
      GTEST_SKIP() << "capstone AArch64 unavailable";
  }
};

} // namespace neverd::a64_parity_test

#endif // NEVERD_UNITTESTS_LIFT_AARCH64_NATIVEDECODEPARITYTESTSDETAIL_H
