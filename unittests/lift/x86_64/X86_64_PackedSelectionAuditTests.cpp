//===- X86_64_PackedSelectionAuditTests.cpp - Packed selections -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent values, full state and exact encoding admission for SSE2.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

std::vector<uint8_t> encoding(uint8_t Opcode, unsigned Destination,
                              unsigned Source, uint8_t Immediate = 0) {
  std::vector<uint8_t> Bytes{0x66};
  const unsigned Rex = (Destination >= 8 ? 4 : 0) | (Source >= 8 ? 1 : 0);
  if (Rex)
    Bytes.push_back(0x40 | Rex);
  Bytes.insert(
      Bytes.end(),
      {0x0f, Opcode, uint8_t(0xc0 | ((Destination & 7) << 3) | (Source & 7))});
  if (Opcode == 0x70)
    Bytes.push_back(Immediate);
  return Bytes;
}

void check(Arch Target, uint8_t Opcode, unsigned Destination, unsigned Source,
           uint8_t Immediate, unsigned Seed) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Target));
  const auto Bytes = encoding(Opcode, Destination, Source, Immediate);
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            int(Bytes.size()));
  std::vector<LowOp> Ops, Plain;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  Dec.liftToLow(Insn, Plain);
  ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
  for (const auto &Op : Ops)
    EXPECT_NE(Op.Opcode, NdOp::INTRINSIC);

  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  std::vector<llvm::APInt> Vectors;
  std::array<uint64_t, 16> GPRs;
  // Include both sides of the signed saturation boundaries, the boundaries
  // themselves, extreme values and unrelated lane patterns.
  constexpr int64_t Values[] = {
      -2147483648LL, -32769, -32768, -129,  -128,       -1,         0,
      127,           128,    32767,  32768, 2147483647, 0x13572468, -0x1234567};
  for (unsigned I = 0; I != x86reg::VectorRegCount; ++I) {
    std::array<uint64_t, 8> Words;
    for (unsigned J = 0; J != Words.size(); ++J)
      Words[J] = UINT64_C(0xff01fe02fd03fc04) * (1 + I + J * 17 + Seed);
    Vectors.emplace_back(512, llvm::ArrayRef<uint64_t>(Words));
    const unsigned Bits = Opcode == 0x63 ? 16 : 32;
    for (unsigned Lane = 0; Lane != 128 / Bits; ++Lane)
      Vectors.back().insertBits(
          llvm::APInt(Bits, Values[(Lane + I + Seed) % std::size(Values)]),
          Lane * Bits);
    if (Opcode == 0x50) {
      Vectors.back().setBitVal(63, Seed & 1);
      Vectors.back().setBitVal(127, Seed & 2);
    }
    State.write(SymSpace::Register, x86reg::vectorReg(I),
                Ctx.mkConst(Vectors.back()));
  }
  // Partial equality exercises the per-lane mask, beyond the self-compare.
  if (Opcode == 0x76 && Destination != Source) {
    Vectors[Source].insertBits(Vectors[Destination].extractBits(32, 32), 32);
    State.write(SymSpace::Register, x86reg::vectorReg(Source),
                Ctx.mkConst(Vectors[Source]));
  }
  for (unsigned I = 0; I != GPRs.size(); ++I) {
    GPRs[I] = UINT64_C(0x9876543200000000) + I;
    State.write(SymSpace::Register, I * 8, Ctx.mkConst(64, GPRs[I]));
  }
  for (unsigned I = 0; I != Flags.size(); ++I)
    State.write(SymSpace::Register, Flags[I], Ctx.mkConst(8, (Seed + I) & 1));

  const auto A = Vectors[Destination], B = Vectors[Source];
  llvm::APInt Result(128, 0);
  switch (Opcode) {
  case 0x70:
    for (unsigned Lane = 0; Lane != 4; ++Lane)
      Result.insertBits(B.extractBits(32, ((Immediate >> (Lane * 2)) & 3) * 32),
                        Lane * 32);
    break;
  case 0x76:
    for (unsigned Lane = 0; Lane != 4; ++Lane)
      Result.insertBits(llvm::APInt(32, A.extractBits(32, Lane * 32) ==
                                                B.extractBits(32, Lane * 32)
                                            ? UINT32_MAX
                                            : 0),
                        Lane * 32);
    break;
  case 0xdb:
    Result = A.trunc(128) & B.trunc(128);
    break;
  case 0x6c:
    Result.insertBits(A.trunc(64), 0);
    Result.insertBits(B.trunc(64), 64);
    break;
  case 0x63:
  case 0x6b: {
    const unsigned InputBits = Opcode == 0x63 ? 16 : 32;
    const unsigned OutputBits = InputBits / 2;
    const int64_t Minimum = -(INT64_C(1) << (OutputBits - 1));
    const int64_t Maximum = -Minimum - 1;
    for (unsigned Half = 0; Half != 2; ++Half)
      for (unsigned Lane = 0; Lane != 128 / InputBits; ++Lane) {
        const int64_t Value = (Half ? B : A)
                                  .extractBits(InputBits, Lane * InputBits)
                                  .getSExtValue();
        Result.insertBits(
            llvm::APInt(OutputBits, std::clamp(Value, Minimum, Maximum)),
            Half * 64 + Lane * OutputBits);
      }
    break;
  }
  case 0x50: {
    const uint64_t Mask = unsigned(B[63]) | (unsigned(B[127]) << 1);
    GPRs[Destination] =
        Target == Arch::X64
            ? Mask
            : (GPRs[Destination] & UINT64_C(0xffffffff00000000)) | Mask;
    break;
  }
  default:
    FAIL() << "unknown oracle opcode";
  }
  if (Opcode != 0x50)
    Vectors[Destination].insertBits(Result, 0);
  ASSERT_EQ(Exec.run(Ops), Ops.size());
  ASSERT_EQ(Exec.unmodelledCount(), 0U);
  for (unsigned I = 0; I != Vectors.size(); ++I) {
    const auto Actual =
        Ctx.asConst(State.read(SymSpace::Register, x86reg::vectorReg(I), 64));
    ASSERT_TRUE(Actual);
    EXPECT_EQ(*Actual, Vectors[I]) << "complete vector " << I;
  }
  for (unsigned I = 0; I != GPRs.size(); ++I) {
    const auto Actual = Ctx.asConst(State.read(SymSpace::Register, I * 8, 8));
    ASSERT_TRUE(Actual);
    EXPECT_EQ(Actual->getZExtValue(), GPRs[I]) << "GPR " << I;
  }
  for (unsigned I = 0; I != Flags.size(); ++I) {
    const auto Actual =
        Ctx.asConst(State.read(SymSpace::Register, Flags[I], 1));
    ASSERT_TRUE(Actual);
    EXPECT_EQ(Actual->getZExtValue(), (Seed + I) & 1);
  }
}

TEST(X86PackedSelectionAudit, EveryShuffleControlAndRegisterAlias) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (auto Pair : {std::array<unsigned, 2>{0, 7}, {7, 7}, {8, 15}, {15, 15}})
      if (Target == Arch::X64 || Pair[0] < 8)
        for (unsigned Immediate = 0; Immediate != 256; ++Immediate) {
          SCOPED_TRACE(::testing::Message()
                       << Pair[0] << '/' << Pair[1] << '/' << Immediate);
          check(Target, 0x70, Pair[0], Pair[1], Immediate, Immediate & 1);
          if (testing::Test::HasFatalFailure())
            return;
        }
}

TEST(X86PackedSelectionAudit, ComparisonPackingAndMasksPreserveCompleteState) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Destination : {0u, 7u, 8u, 15u})
      for (unsigned Source : {0u, 7u, 8u, 15u})
        if (Target == Arch::X64 || (Destination < 8 && Source < 8))
          for (uint8_t Opcode : {0x76, 0xdb, 0x6c, 0x63, 0x6b, 0x50})
            for (unsigned Seed = 0; Seed != 14; ++Seed) {
              SCOPED_TRACE(::testing::Message()
                           << unsigned(Opcode) << '/' << Destination << '/'
                           << Source << '/' << Seed);
              check(Target, Opcode, Destination, Source, 0, Seed);
              if (testing::Test::HasFatalFailure())
                return;
            }
}

void incomplete(Decoder &Dec, const DecodedInsn &Insn) {
  LowInstructionUndefinedEffects Effects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.OperationDigest = "stale";
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  std::vector<LowOp> Ops;
  try {
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  } catch (const UnliftedInstruction &) {
  }
  EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_NE(Effects.OperationDigest, "stale");
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
}

TEST(X86PackedSelectionAudit, InconsistentBytesDetailsAndOtherFormsRefuse) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  using Mutation = void (*)(cs_insn &);
  const Mutation Mutations[] = {
      [](cs_insn &I) { I.bytes[I.detail->x86.encoding.modrm_offset] ^= 8; },
      [](cs_insn &I) { I.detail->x86.operands[0].reg = X86_REG_XMM3; },
      [](cs_insn &I) { I.detail->x86.operands[1].size = 4; },
      [](cs_insn &I) { I.detail->x86.rex ^= 4; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_offset += 1; },
      [](cs_insn &I) { I.detail->x86.eflags = X86_EFLAGS_MODIFY_CF; },
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
  };
  for (uint8_t Opcode : {0x70, 0x76, 0xdb, 0x6c, 0x63, 0x6b, 0x50})
    for (Mutation Change : Mutations) {
      const auto Bytes = encoding(Opcode, 0, 1, 0x9c);
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
                int(Bytes.size()));
      Change(*Insn.Raw);
      incomplete(Dec, Insn);
    }
  const std::vector<std::vector<uint8_t>> OtherForms = {
      {0x66, 0x0f, 0x70, 0x01, 0x9c}, {0x66, 0x66, 0x0f, 0x70, 0xc1, 0x9c},
      {0xc5, 0xf9, 0x70, 0xc1, 0x9c}, {0x66, 0x0f, 0x76, 0x01},
      {0x66, 0x0f, 0x6b, 0x01},       {0x66, 0x0f, 0x63, 0x01},
      {0x66, 0x0f, 0x6c, 0x01},       {0x0f, 0xdb, 0xc1},
      {0xc5, 0xf1, 0xdb, 0xc2},       {0x66, 0x48, 0x0f, 0x50, 0xc1},
  };
  for (const auto &Bytes : OtherForms) {
    DecodedInsn Insn{};
    if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn) > 0)
      incomplete(Dec, Insn);
  }
  const auto Bytes = encoding(0x70, 0, 1, 0x9c);
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            int(Bytes.size()));
  Insn.Raw->detail->x86.operands[2].imm ^= 1;
  incomplete(Dec, Insn);
}

} // namespace
