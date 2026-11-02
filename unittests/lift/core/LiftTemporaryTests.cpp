//===- LiftTemporaryTests.cpp - Byte extents of lifted temporaries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/ARM/ARMRegs.h"
#include "neverd/lift/LiftCommon.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

// Independent 32-bit-limb arithmetic: no symbolic builders or APInt product.
struct Product {
  uint64_t Low, High;
};

Product multiply(uint64_t A, uint64_t B, bool Signed) {
  constexpr uint64_t Mask = UINT32_MAX;
  const uint64_t LL = (A & Mask) * (B & Mask);
  const uint64_t HL = (A >> 32) * (B & Mask) + (LL >> 32);
  const uint64_t LH = (A & Mask) * (B >> 32) + (HL & Mask);
  Product P{(LH << 32) | (LL & Mask),
            (A >> 32) * (B >> 32) + (HL >> 32) + (LH >> 32)};
  if (Signed) {
    if (A >> 63)
      P.High -= B;
    if (B >> 63)
      P.High -= A;
  }
  return P;
}

constexpr std::array<uint64_t, 10> Inputs{0,
                                          1,
                                          2,
                                          3,
                                          UINT64_MAX,
                                          UINT64_MAX - 1,
                                          uint64_t{1} << 63,
                                          (uint64_t{1} << 63) - 1,
                                          0x0123456789abcdef,
                                          0xfedcba9876543210};

std::vector<LowOp> lift(Arch A, llvm::ArrayRef<uint8_t> Bytes) {
  Decoder D;
  EXPECT_TRUE(D.init(A));
  D.setStrict(true);
  DecodedInsn I{};
  const auto Size = D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I);
  EXPECT_EQ(Size, Bytes.size());
  std::vector<LowOp> Ops;
  if (Size)
    D.liftToLow(I, Ops);
  EXPECT_FALSE(Ops.empty());
  return Ops;
}

void expectWord(SymContext &C, SymState &S, uint64_t R, uint16_t Bytes,
                uint64_t Expected) {
  const auto Value = C.asConst(S.read(SymSpace::Register, R, Bytes));
  ASSERT_TRUE(Value.has_value());
  EXPECT_EQ(Value->getZExtValue(), Expected);
}

TEST(LiftTemporary, AllocationsReserveCompleteByteRanges) {
  std::vector<uint16_t> Sizes{1, 2, 4, 8, 9, 10, 12, 16, 32, 64, 65535};
  for (bool Reverse : {false, true}) {
    if (Reverse)
      std::reverse(Sizes.begin(), Sizes.end());
    std::vector<LowOp> Ops;
    LiftStateBase State(0x1000, 1, Ops);
    uint64_t PreviousEnd = TmpBase;
    for (auto Size : Sizes) {
      SCOPED_TRACE(Size);
      const auto T = State.makeTemp(Size);
      EXPECT_TRUE(T.isTemp());
      EXPECT_EQ(T.Size, Size);
      EXPECT_GE(T.Offset, PreviousEnd);
      EXPECT_EQ((T.Offset - TmpBase) % TmpStride, 0U);
      PreviousEnd = T.Offset + Size;
    }
    LiftStateBase NextInstruction(0x1001, 1, Ops);
    EXPECT_EQ(NextInstruction.makeTemp(16).Offset, TmpBase);
  }
}

TEST(LiftTemporary, ScalarOffsetsAndEmptyPlaceholdersStayStable) {
  std::vector<LowOp> Ops;
  LiftStateBase State(0x1000, 1, Ops);
  unsigned Slot = 0;
  // Zero-sized values are accumulator placeholders replaced before emission.
  for (uint16_t Size : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
    const auto T = State.makeTemp(Size);
    EXPECT_EQ(T.Offset, TmpBase + Slot++ * TmpStride);
    EXPECT_EQ(T.Size, Size);
  }
}

TEST(LiftTemporary, ExhaustedByteStorageDoesNotWrap) {
  constexpr uint64_t LastSlot = (UINT64_MAX - TmpBase) / TmpStride;
  for (uint16_t Size : {8, 9}) {
    EXPECT_EXIT(
        {
          llvm::install_fatal_error_handler(
              [](void *, const char *Reason, bool) {
                std::fputs(Reason, stderr);
                std::fflush(stderr);
                std::_Exit(1);
              });
          std::vector<LowOp> Ops;
          LiftStateBase State(0x1000, 1, Ops);
          State.TmpId = LastSlot;
          State.makeTemp(Size);
          State.makeTemp(1);
        },
        ::testing::ExitedWithCode(1),
        "temporary byte storage exhausted.*instruction address 4096");
  }
  std::vector<LowOp> Ops;
  LiftStateBase State(0x1000, 1, Ops);
  State.TmpId = LastSlot;
  const auto Last = State.makeTemp(8);
  EXPECT_EQ(Last.Offset, UINT64_MAX - 7);
  EXPECT_EQ(State.TmpId, LastSlot + 1);
}

TEST(LiftTemporary, X64MultiplyPreservesProductAndFlags) {
  const std::vector<std::vector<uint8_t>> Encodings{
      {0x48, 0x0f, 0xaf, 0xc1}, // imul rax, rcx
      {0x48, 0x0f, 0xaf, 0xc0}, // imul rax, rax
      {0x48, 0x6b, 0xc1, 0xfd}, // imul rax, rcx, -3
      {0x48, 0xf7, 0xe9},       // imul rcx
      {0x48, 0xf7, 0xe1}};      // mul rcx
  for (unsigned Form = 0; Form < Encodings.size(); ++Form) {
    SCOPED_TRACE(Form);
    const auto Ops = lift(Arch::X64, Encodings[Form]);
    for (auto A : Inputs)
      for (auto B : Inputs) {
        SCOPED_TRACE(::testing::Message() << A << " * " << B);
        const auto P = multiply(Form == 2 ? B : A,
                                Form == 1   ? A
                                : Form == 2 ? uint64_t(-3)
                                            : B,
                                Form != 4);
        const bool Overflow =
            P.High != (Form == 4 || !(P.Low >> 63) ? 0 : UINT64_MAX);
        SymContext C;
        SymState S(C);
        SymExec E(C, S);
        S.write(SymSpace::Register, x86reg::RAX, C.mkConst(64, A));
        S.write(SymSpace::Register, x86reg::RCX, C.mkConst(64, B));
        for (const auto &O : Ops)
          ASSERT_EQ(E.step(O), StepResult::Continue);
        expectWord(C, S, x86reg::RAX, 8, P.Low);
        if (Form >= 3)
          expectWord(C, S, x86reg::RDX, 8, P.High);
        expectWord(C, S, x86reg::CF, 1, Overflow);
        expectWord(C, S, x86reg::OF, 1, Overflow);
      }
  }
}

TEST(LiftTemporary, A64MultiplyHighPreservesBothOperands) {
  for (bool Signed : {false, true}) {
    const std::array<uint8_t, 4> Bytes{0x20, 0x7c,
                                       uint8_t(Signed ? 0x42 : 0xc2), 0x9b};
    const auto Ops = lift(Arch::AArch64, Bytes); // [s/u]mulh x0, x1, x2
    for (auto A : Inputs)
      for (auto B : Inputs) {
        SCOPED_TRACE(::testing::Message() << Signed << ": " << A << " * " << B);
        SymContext C;
        SymState S(C);
        SymExec E(C, S);
        S.write(SymSpace::Register, a64reg::X1, C.mkConst(64, A));
        S.write(SymSpace::Register, a64reg::X2, C.mkConst(64, B));
        for (const auto &O : Ops)
          ASSERT_EQ(E.step(O), StepResult::Continue);
        expectWord(C, S, a64reg::X0, 8, multiply(A, B, Signed).High);
        expectWord(C, S, a64reg::X1, 8, A);
        expectWord(C, S, a64reg::X2, 8, B);
      }
  }
}

TEST(LiftTemporary, ARMVectorAccumulatorPreservesEarlierLanes) {
  const std::array<uint8_t, 4> Bytes{0x44, 0x08, 0x22, 0xf2};
  const auto Ops = lift(Arch::ARM, Bytes); // vadd.i32 q0, q1, q2
  for (uint32_t Seed : {0U, 17U, UINT32_MAX}) {
    SymContext C;
    SymState S(C);
    SymExec E(C, S);
    for (unsigned Lane = 0; Lane != 4; ++Lane) {
      S.write(SymSpace::Register, armreg::D(2) + Lane * 4,
              C.mkConst(32, uint32_t(Seed + 31 * Lane)));
      S.write(SymSpace::Register, armreg::D(4) + Lane * 4,
              C.mkConst(32, uint32_t(0x81234567 + 197 * Lane)));
    }
    for (const auto &O : Ops)
      ASSERT_EQ(E.step(O), StepResult::Continue);
    for (unsigned Lane = 0; Lane != 4; ++Lane)
      expectWord(C, S, armreg::D(0) + Lane * 4, 4,
                 uint32_t(Seed + 31 * Lane + 0x81234567 + 197 * Lane));
  }
}

} // namespace
