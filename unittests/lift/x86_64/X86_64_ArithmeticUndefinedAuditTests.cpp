//===- X86_64_ArithmeticUndefinedAuditTests.cpp - Arithmetic coverage -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Complete values, preserved state and admission for integer arithmetic.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

struct PackedCase {
  uint8_t Opcode;
  unsigned LaneBits;
  bool Subtract;
};

constexpr PackedCase PackedCases[] = {
    {0xfc, 8, false}, {0xfd, 16, false}, {0xfe, 32, false}, {0xd4, 64, false},
    {0xf8, 8, true},  {0xf9, 16, true},  {0xfa, 32, true},  {0xfb, 64, true}};

void checkPacked(Arch Target, const PackedCase &Case, unsigned Destination,
                 unsigned Source) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Target));
  std::vector<uint8_t> Bytes{0x66};
  const unsigned Rex = (Destination >= 8 ? 4 : 0) | (Source >= 8 ? 1 : 0);
  if (Rex)
    Bytes.push_back(0x40 | Rex);
  Bytes.insert(Bytes.end(),
               {0x0f, Case.Opcode,
                uint8_t(0xc0 | ((Destination & 7) << 3) | (Source & 7))});
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

  for (unsigned Seed : {0u, 1u, 2u, 3u}) {
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    std::vector<llvm::APInt> Vectors;
    for (unsigned I = 0; I != x86reg::VectorRegCount; ++I) {
      std::array<uint64_t, 8> Words;
      for (unsigned J = 0; J != Words.size(); ++J) {
        const uint64_t Pattern =
            UINT64_C(0xff01fe02fd03fc04) * (1 + I + J * 17);
        Words[J] = Seed == 0   ? Pattern
                   : Seed == 1 ? ~Pattern
                   : Seed == 2 ? 0
                               : UINT64_MAX;
      }
      Vectors.emplace_back(512, llvm::ArrayRef<uint64_t>(Words));
      State.write(SymSpace::Register, x86reg::vectorReg(I),
                  Ctx.mkConst(Vectors.back()));
    }
    for (unsigned I = 0; I != 16; ++I)
      State.write(SymSpace::Register, I * 8,
                  Ctx.mkConst(64, UINT64_C(0x9876543200000000) + I));
    for (unsigned I = 0; I != Flags.size(); ++I)
      State.write(SymSpace::Register, Flags[I], Ctx.mkConst(8, (Seed + I) & 1));
    const auto A = Vectors[Destination], B = Vectors[Source];
    for (unsigned Bit = 0; Bit != 128; Bit += Case.LaneBits) {
      const auto Left = A.extractBits(Case.LaneBits, Bit);
      const auto Right = B.extractBits(Case.LaneBits, Bit);
      Vectors[Destination].insertBits(
          Case.Subtract ? Left - Right : Left + Right, Bit);
    }
    ASSERT_EQ(Exec.run(Ops), Ops.size());
    ASSERT_EQ(Exec.unmodelledCount(), 0U);
    for (unsigned I = 0; I != Vectors.size(); ++I) {
      auto Actual =
          Ctx.asConst(State.read(SymSpace::Register, x86reg::vectorReg(I), 64));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(*Actual, Vectors[I]) << "complete vector " << I;
    }
    for (unsigned I = 0; I != 16; ++I) {
      auto Actual = Ctx.asConst(State.read(SymSpace::Register, I * 8, 8));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(Actual->getZExtValue(), UINT64_C(0x9876543200000000) + I);
    }
    for (unsigned I = 0; I != Flags.size(); ++I) {
      auto Actual = Ctx.asConst(State.read(SymSpace::Register, Flags[I], 1));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(Actual->getZExtValue(), (Seed + I) & 1);
    }
  }
}

TEST(X86ArithmeticUndefinedAudit, PackedLanesPreserveFlagsAndCompleteVectors) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &Case : PackedCases)
      for (unsigned Destination : {0u, 7u, 8u, 15u})
        for (unsigned Source : {0u, 7u, 8u, 15u}) {
          if (Target == Arch::X86 && (Destination >= 8 || Source >= 8))
            continue;
          SCOPED_TRACE(::testing::Message() << unsigned(Case.Opcode) << '/'
                                            << Destination << '/' << Source);
          checkPacked(Target, Case, Destination, Source);
          if (testing::Test::HasFatalFailure())
            return;
        }
}

std::vector<uint8_t> immediateEncoding(unsigned Destination, unsigned Source,
                                       int64_t Immediate, bool Byte) {
  std::vector<uint8_t> Bytes{
      uint8_t(0x48 | (Destination >= 8 ? 4 : 0) | (Source >= 8 ? 1 : 0)),
      uint8_t(Byte ? 0x6b : 0x69),
      uint8_t(0xc0 | ((Destination & 7) << 3) | (Source & 7))};
  for (unsigned I = 0; I != (Byte ? 1u : 4u); ++I)
    Bytes.push_back(uint64_t(Immediate) >> (I * 8));
  return Bytes;
}

TEST(X86ArithmeticUndefinedAudit, ImmediateMultiplyBindsFourIndependentFlags) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  const std::array<unsigned, 4> Arbitrary = {x86reg::SF, x86reg::ZF, x86reg::AF,
                                             x86reg::PF};
  for (bool Byte : {false, true})
    for (int64_t Immediate : std::array<int64_t, 8>{
             -2147483648LL, -2147483647, -129, -128, -1, 0, 127, 2147483647}) {
      if (Byte && (Immediate < -128 || Immediate > 127))
        continue;
      for (unsigned Destination : {0u, 9u})
        for (unsigned Source : {0u, 9u}) {
          const auto Bytes =
              immediateEncoding(Destination, Source, Immediate, Byte);
          DecodedInsn Insn{};
          ASSERT_EQ(
              Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              int(Bytes.size()));
          std::vector<LowOp> Ops;
          LowInstructionUndefinedEffects Effects;
          Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
          ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
              << Effects.Diagnostic;
          ASSERT_EQ(Effects.Effects.size(), Arbitrary.size());
          EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
          for (unsigned I = 0; I != Arbitrary.size(); ++I) {
            const auto &Effect = Effects.Effects[I];
            EXPECT_EQ(Effect.Output, NdVar::reg(Arbitrary[I], 1));
            EXPECT_EQ(Effect.BitOffset, 0U);
            EXPECT_EQ(Effect.BitCount, 1U);
            EXPECT_FALSE(Effect.When);
            EXPECT_GT(Effect.AfterOp, 0U);
            EXPECT_LE(Effect.AfterOp, Ops.size());
          }
          for (uint64_t Value :
               {UINT64_C(0), UINT64_C(1), UINT64_MAX,
                UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000),
                UINT64_C(0x100000005)}) {
            SymContext Ctx;
            SymState State(Ctx);
            SymExec Exec(Ctx, State);
            std::array<uint64_t, 16> Registers;
            for (unsigned I = 0; I != Registers.size(); ++I) {
              Registers[I] = UINT64_C(0x4321876500000000) + I;
              if (I == Source)
                Registers[I] = Value;
              State.write(SymSpace::Register, I * 8,
                          Ctx.mkConst(64, Registers[I]));
            }
            for (unsigned I = 0; I != Flags.size(); ++I)
              State.write(SymSpace::Register, Flags[I], Ctx.mkConst(8, 1));
            const auto Product = llvm::APInt(64, Value).sext(128) *
                                 llvm::APInt(64, uint64_t(Immediate)).sext(128);
            Registers[Destination] = Product.trunc(64).getZExtValue();
            ASSERT_EQ(Exec.run(Ops), Ops.size());
            ASSERT_EQ(Exec.unmodelledCount(), 0U);
            for (unsigned I = 0; I != Registers.size(); ++I) {
              auto Actual =
                  Ctx.asConst(State.read(SymSpace::Register, I * 8, 8));
              ASSERT_TRUE(Actual);
              EXPECT_EQ(Actual->getZExtValue(), Registers[I]);
            }
            const bool Overflow = Product.trunc(64).sext(128) != Product;
            for (unsigned Flag : {x86reg::CF, x86reg::OF, x86reg::DF}) {
              auto Actual =
                  Ctx.asConst(State.read(SymSpace::Register, Flag, 1));
              ASSERT_TRUE(Actual);
              EXPECT_EQ(Actual->getZExtValue(),
                        Flag == x86reg::DF ? 1 : Overflow);
            }
          }
        }
    }
}

void expectIncomplete(Decoder &Dec, DecodedInsn &Insn) {
  LowInstructionUndefinedEffects Effects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.Effects.push_back({1, NdVar::reg(x86reg::AF, 1), 0, 1, {}});
  Effects.OperationDigest = "stale";
  std::vector<LowOp> Ops;
  try {
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  } catch (const UnliftedInstruction &) {
  }
  EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_NE(Effects.OperationDigest, "stale");
}

TEST(X86ArithmeticUndefinedAudit,
     OtherFormsAndInconsistentDetailsStayIncomplete) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  const std::vector<std::vector<uint8_t>> OtherForms = {
      {0x0f, 0xfc, 0xc1},
      {0xc5, 0xf1, 0xfc, 0xc2},
      {0x66, 0x0f, 0xfc, 0x00},
      {0x66, 0x66, 0x0f, 0xfc, 0xc1},
      {0x48, 0x69, 0x00, 1, 0, 0, 0},
      {0x69, 0xc1, 1, 0, 0, 0},
      {0x66, 0x69, 0xc1, 1, 0},
      {0x67, 0x48, 0x6b, 0xc1, 1},
  };
  for (const auto &Bytes : OtherForms) {
    DecodedInsn Insn{};
    if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn) <= 0)
      continue;
    expectIncomplete(Dec, Insn);
  }
  using Mutation = void (*)(cs_insn &);
  const Mutation Mutations[] = {
      [](cs_insn &I) { I.bytes[I.detail->x86.encoding.modrm_offset] ^= 8; },
      [](cs_insn &I) { I.detail->x86.operands[0].reg = X86_REG_XMM3; },
      [](cs_insn &I) { I.detail->x86.operands[1].size = 4; },
      [](cs_insn &I) { I.detail->x86.rex ^= 4; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_offset += 1; },
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
  };
  for (const auto &Bytes : {std::vector<uint8_t>{0x66, 0x0f, 0xfc, 0xc1},
                            immediateEncoding(0, 1, -17, false)})
    for (Mutation Change : Mutations) {
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
                int(Bytes.size()));
      Change(*Insn.Raw);
      expectIncomplete(Dec, Insn);
    }
  for (bool Byte : {false, true}) {
    const auto Bytes = immediateEncoding(9, 0, -17, Byte);
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              int(Bytes.size()));
    Insn.Raw->detail->x86.operands[2].imm += 1;
    expectIncomplete(Dec, Insn);
  }
}

} // namespace
