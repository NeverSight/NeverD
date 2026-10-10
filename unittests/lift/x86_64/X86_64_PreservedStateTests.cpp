//===- X86_64_PreservedStateTests.cpp - Native bank evidence --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86PreservedState.h"

using namespace neverd;

namespace {

TEST(X86PreservedState, FreshScalarFormsPreserveTheClosedBanks) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0x01, 0xc8},
      {0x66, 0x11, 0xc8},
      {0x48, 0x29, 0xc8},
      {0x21, 0xc8},
      {0x09, 0xc8},
      {0x31, 0xc8},
      {0x85, 0xc8},
      {0x48, 0x8b, 0x03},
      {0x64, 0x48, 0x8b, 0x03},
      {0x67, 0x48, 0x8d, 0x44, 0x88, 0x04},
      {0x0f, 0xb6, 0xc4},
      {0x48, 0x63, 0xc1},
      {0x48, 0x87, 0xc8},
      {0xfe, 0xc4},
      {0x48, 0xf7, 0xd8},
      {0x0f, 0xc8},
      {0x41, 0x54},
      {0x41, 0x5c},
      {0xd3, 0xe0},
      {0xc1, 0xe8, 1},
      {0xd1, 0xc0},
      {0xd3, 0xc8},
      {0x0f, 0xa3, 0xc8},
      {0x0f, 0xa4, 0xc8, 3},
      {0x0f, 0xc1, 0xc8},
      {0x0f, 0xc1, 0x0b},
      {0x75, 2},
      {0x0f, 0x95, 0xc0},
      {0x48, 0x0f, 0x45, 0xc1},
      {0xe8, 1, 0, 0, 0},
      {0xff, 0xd0},
      {0xc3},
      {0xc2, 8, 0},
      {0x90},
      {0x48, 0x99},
      {0xf8},
      {0xfc},
      {0x9f},
      {0x9e},
      {0x9c},
      {0x9d}};
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  for (const auto &Bytes : Cases) {
    SCOPED_TRACE(testing::PrintToString(Bytes));
    D.resetX86FpuState();
    DecodedInsn I{};
    ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
              static_cast<int>(Bytes.size()));
    std::vector<LowOp> Plain, Ops;
    D.liftToLow(I, Plain);
    D.resetX86FpuState();
    LowInstructionPreservedState Fact;
    // No undefined-output record is requested. These are separate claims.
    D.liftToLow(I, Ops, {}, {}, nullptr, &Fact);
    EXPECT_EQ(lowUndefinedOperationDigest(Plain),
              lowUndefinedOperationDigest(Ops));
    EXPECT_EQ(Fact.Audit, LowPreservedStateAudit::LegacyIntegerV1);
    LowInstructionBoundary B;
    B.Address = 0x1000;
    B.Size = Bytes.size();
    B.OpCount = Ops.size();
    EXPECT_TRUE(matchesLowPreservedState(Fact, B, Bytes, Ops));
  }
}

TEST(X86PreservedState, VectorControlAndUnauditedFormsNeverInheritARecord) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0x66, 0x0f, 0xef, 0xf6}, // pxor xmm6,xmm6
      {0x66, 0x0f, 0x56, 0xc1},
      {0x0f, 0x56, 0xc1},       // orpd/orps
      {0xd9, 0x28},             // fldcw [rax]
      {0x0f, 0xae, 0x10},       // ldmxcsr [rax]
      {0xc5, 0xf9, 0xef, 0xc0}, // vpxor xmm0,xmm0,xmm0
      {0xf0, 0x01, 0x08},
      {0xf3, 0x90},
      {0x0f, 0x05},
      {0xcb},
      {0x48, 0x0f, 0xaf, 0xc1}}; // imul is not in the separate initial audit
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  for (const auto &Bytes : Cases) {
    SCOPED_TRACE(testing::PrintToString(Bytes));
    DecodedInsn Good{}, I{};
    const uint8_t Add[] = {0x01, 0xc8};
    ASSERT_EQ(D.decodeOneForLift(Add, 2, 0x1000, Good), 2);
    std::vector<LowOp> Ops;
    LowInstructionPreservedState Fact;
    D.liftToLow(Good, Ops, {}, {}, nullptr, &Fact);
    ASSERT_EQ(Fact.Audit, LowPreservedStateAudit::LegacyIntegerV1);
    ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x2000, I),
              static_cast<int>(Bytes.size()));
    Ops.clear();
    try {
      D.liftToLow(I, Ops, {}, {}, nullptr, &Fact);
    } catch (const UnliftedInstruction &) {
    }
    EXPECT_EQ(Fact, LowInstructionPreservedState{});
  }
}

TEST(X86PreservedState, ExactBytesWholeSpanAndAuditVersionAreBound) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  std::vector<uint8_t> Bytes{0xb8, 7, 0, 0, 0};
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I), 5);
  std::vector<LowOp> Ops;
  LowInstructionPreservedState F;
  D.liftToLow(I, Ops, {}, {}, nullptr, &F);
  LowInstructionBoundary B;
  B.Address = 0x1000;
  B.Size = 5;
  B.OpCount = Ops.size();
  ASSERT_TRUE(matchesLowPreservedState(F, B, Bytes, Ops));
  const auto Digest = lowPreservedStateDigest(F);
  Bytes[1] = 8;
  EXPECT_FALSE(matchesLowPreservedState(F, B, Bytes, Ops));
  Bytes[1] = 7;
  auto Changed = Ops;
  Changed.front().Inputs[0] = NdVar::cst(8, 4);
  EXPECT_FALSE(matchesLowPreservedState(F, B, Bytes, Changed));
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    auto Bad = F;
    switch (Mutation) {
    case 0:
      ++Bad.SemanticsVersion;
      break;
    case 1:
      Bad.Architecture = Arch::X86;
      break;
    case 2:
      Bad.Mode = InstructionMode::ARM;
      break;
    case 3:
      ++Bad.Address;
      break;
    case 4:
      ++Bad.Size;
      break;
    case 5:
      --Bad.OpCount;
      break;
    case 6:
      Bad.StateSet = LowPreservedStateSet::None;
      break;
    }
    EXPECT_NE(lowPreservedStateDigest(Bad), Digest);
    EXPECT_FALSE(matchesLowPreservedState(Bad, B, Bytes, Ops));
  }
  ++B.FirstOp;
  EXPECT_FALSE(matchesLowPreservedState(F, B, Bytes, Ops));
}

TEST(X86PreservedState, NonStrictFailedAndOtherModeLiftsClearEvidence) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  const uint8_t Bytes[] = {0x01, 0xc8};
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes, 2, 0x1000, I), 2);
  std::vector<LowOp> Ops;
  LowInstructionPreservedState F;
  D.liftToLow(I, Ops, {}, {}, nullptr, &F);
  ASSERT_EQ(F.Audit, LowPreservedStateAudit::LegacyIntegerV1);
  const auto Good = F;
  D.setStrict(false);
  D.liftToLow(I, Ops, {}, {}, nullptr, &F);
  EXPECT_EQ(F, LowInstructionPreservedState{});
  F = Good;
  D.setStrict(true);
  D.liftToLow({}, Ops, {}, {}, nullptr, &F);
  EXPECT_EQ(F, LowInstructionPreservedState{});
  ASSERT_TRUE(D.init(Arch::X86));
  ASSERT_EQ(D.decodeOneForLift(Bytes, 2, 0x1000, I), 2);
  F = Good;
  D.liftToLow(I, Ops, {}, {}, nullptr, &F);
  EXPECT_EQ(F, LowInstructionPreservedState{});
  F = Good;
  EXPECT_FALSE(D.liftX64MemoryCallToLow(I, Ops, nullptr, &F));
  EXPECT_EQ(F, LowInstructionPreservedState{});
}

TEST(X86PreservedState, ClosedRangesUseFullContainersAndExcludeAdjacentState) {
  using namespace x86reg;
  const auto Contains = [](OpaqueStateRange::Space Space, uint64_t Offset,
                           uint16_t Bit, uint16_t Count) {
    for (const auto &R : LegacyIntegerOpaqueV1)
      if (R.Location == Space && Offset >= R.Offset && Count &&
          Offset - R.Offset < (R.BitCount + 7U) / 8 &&
          8 * (Offset - R.Offset) + Bit >= R.FirstBit &&
          8 * (Offset - R.Offset) + Bit + Count <= R.FirstBit + R.BitCount)
        return true;
    return false;
  };
  const auto Reg = OpaqueStateRange::Space::Register;
  for (unsigned I = 0; I != 32; ++I) {
    EXPECT_TRUE(Contains(Reg, vectorReg(I), 0, 128));
    EXPECT_TRUE(Contains(Reg, vectorReg(I), 0, 256));
    EXPECT_TRUE(Contains(Reg, vectorReg(I), 0, 512));
    EXPECT_FALSE(Contains(Reg, vectorReg(I), 0, 513));
  }
  EXPECT_EQ(vectorReg(1) - vectorReg(0), 64U);
  for (unsigned I = 0; I != 16; ++I)
    EXPECT_TRUE(Contains(Reg, extendedGeneralReg(I), 0, 64));
  EXPECT_TRUE(Contains(Reg, FPU_CW, 0, 16));
  EXPECT_FALSE(Contains(Reg, FPU_CW, 0, 17));
  for (auto Offset : {RAX, R15, FPU_SW, ST0, OpmaskBase, TileBase})
    EXPECT_FALSE(Contains(Reg, Offset, 0, 1));
  EXPECT_TRUE(Contains(OpaqueStateRange::Space::MXCSR, 0, 6, 10));
  EXPECT_TRUE(Contains(OpaqueStateRange::Space::MXCSR, 0, 0, 16));
  EXPECT_FALSE(Contains(OpaqueStateRange::Space::MXCSR, 0, 16, 1));
}

} // namespace
