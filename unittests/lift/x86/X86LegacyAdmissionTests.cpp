//===- X86LegacyAdmissionTests.cpp - Incomplete legacy state admission --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decoded legacy instructions must not publish scalar approximations of
/// segment transitions or packed 3DNow! arithmetic.
//===----------------------------------------------------------------------===//

#include "X86LegacyAdmissionFixtures.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t kEntry = 0x1000;

void checkRefusal(Arch Target, unsigned Id, const std::vector<uint8_t> &Bytes,
                  bool OwnContext = false) {
  SCOPED_TRACE(static_cast<unsigned>(Target));
  SCOPED_TRACE(Id);
  SCOPED_TRACE(testing::PrintToString(Bytes));
  for (bool Strict : {false, true})
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(Strict);
      SCOPED_TRACE(Route);
      Decoder D;
      ASSERT_TRUE(D.init(Target));
      D.setStrict(Strict);
      D.setX87ContextOwnership(OwnContext);
      DecodedInsn Instruction{};
      const int Size =
          Route == 0
              ? D.decodeOne(Bytes.data(), Bytes.size(), kEntry, Instruction)
          : Route == 1 ? D.decodeOneLight(Bytes.data(), Bytes.size(), kEntry,
                                          Instruction)
                       : D.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry,
                                            Instruction);
      ASSERT_EQ(Size, Bytes.size());
      ASSERT_EQ(Instruction.Id, Id);
      LowOp Sentinel;
      Sentinel.Opcode = NdOp::COPY;
      Sentinel.Addr = kEntry - 1;
      Sentinel.Output = NdVar::tmp(0x1234, 4);
      Sentinel.addInput(NdVar::scalar(42, 4));
      std::vector<LowOp> Ops{Sentinel};
      EXPECT_THROW(D.liftToLow(Instruction, Ops), UnliftedInstruction);
      ASSERT_EQ(Ops.size(), 1u);
      EXPECT_EQ(Ops.front().Opcode, Sentinel.Opcode);
      EXPECT_EQ(Ops.front().Addr, Sentinel.Addr);
      EXPECT_EQ(Ops.front().Output, Sentinel.Output);
      EXPECT_EQ(Ops.front().NumInputs, Sentinel.NumInputs);
      EXPECT_EQ(Ops.front().Inputs[0], Sentinel.Inputs[0]);
    }
}
} // namespace

TEST(X86LegacyAdmission, ZeroBaseAAMRequiresFaultContinuation) {
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0xd4, 0}, {0x66, 0xd4, 0}, {0x67, 0x64, 0xd4, 0}})
    ASSERT_NO_FATAL_FAILURE(checkRefusal(Arch::X86, X86_INS_AAM, Bytes));
}

#include "X86DecimalAdjustTests.inc"

TEST(X86LegacyAdmission, ZeroBaseAADPreservesALAndClearsAH) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X86));
  DecodedInsn I{};
  const uint8_t Bytes[]{0xd5, 0};
  ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), kEntry, I), 2);
  std::vector<LowOp> Ops;
  D.liftToLow(I, Ops);
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  NdOpEmulator E(Image);
  E.setRegister(x86reg::RAX, 0xabcd1295);
  E.setRegister(x86reg::RAX + 1, 0x12);
  E.setStrictMode(true);
  for (const auto &Op : Ops)
    ASSERT_TRUE(E.step(Op));
  ASSERT_TRUE(E.getRegister(x86reg::RAX));
  EXPECT_EQ(*E.getRegister(x86reg::RAX) & 0xff, 0x95u);
  EXPECT_EQ(E.getRegister(x86reg::RAX + 1), 0u);
}

TEST(X86LegacyAdmission, FarPointerLoadsRequireSegmentState) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &E : test::X86FarPointerEncodings) {
      if (Target == Arch::X64 && !E.Escape)
        continue;
      for (unsigned Width : {2u, 4u, 8u}) {
        if (Target == Arch::X86 && Width == 8)
          continue;
        for (uint8_t Prefix : {0, 0x67, 0x64, 0x65}) {
          std::vector<uint8_t> Bytes;
          if (Prefix)
            Bytes.push_back(Prefix);
          if (Width != 4)
            Bytes.push_back(Width == 2 ? 0x66 : 0x48);
          if (E.Escape)
            Bytes.push_back(0x0f);
          Bytes.insert(Bytes.end(), {E.Opcode, 0x00});
          ASSERT_NO_FATAL_FAILURE(checkRefusal(Target, E.Id, Bytes));
          ++Cases;
        }
      }
    }
  EXPECT_EQ(Cases, 76u);
  RecordProperty("far_pointer_instruction_forms", Cases);
}

TEST(X86LegacyAdmission, ThreeDNowCannotBecomeCopiesOrScalarFloats) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &E : test::X86ThreeDNowEncodings)
      for (uint8_t ModRM : {0xc1, 0x01})
        for (uint8_t Prefix : {0, 0x67, 0x64, 0x65}) {
          std::vector<uint8_t> Bytes;
          if (Prefix)
            Bytes.push_back(Prefix);
          Bytes.insert(Bytes.end(), {0x0f, 0x0f, ModRM, E.Opcode});
          ASSERT_NO_FATAL_FAILURE(checkRefusal(Target, E.Id, Bytes));
          ++Cases;
        }
  EXPECT_EQ(Cases, 384u);
  RecordProperty("three_d_now_instruction_forms", Cases);
}

TEST(X86LegacyAdmission, OrdinaryLoadsAndMmxArithmeticRemainAvailable) {
  for (Arch Target : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(Target));
    D.setStrict(true);
    for (const auto &Bytes : {std::vector<uint8_t>{0x8b, 0x00},
                              std::vector<uint8_t>{0x0f, 0xdc, 0xc1}}) {
      DecodedInsn Instruction{};
      ASSERT_EQ(
          D.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Instruction),
          Bytes.size());
      std::vector<LowOp> Ops;
      EXPECT_NO_THROW(D.liftToLow(Instruction, Ops));
      ASSERT_FALSE(Ops.empty());
      for (const auto &Op : Ops)
        EXPECT_NE(Op.Opcode, NdOp::NOP);
    }
  }
}

TEST(X86LegacyAdmission, StateInstructionsCannotBecomeNopsWithoutOwnership) {
  struct StateInstruction {
    unsigned Id;
    std::vector<uint8_t> Bytes;
  };
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &Instruction :
         {StateInstruction{X86_INS_FNOP, {0xd9, 0xd0}},
          StateInstruction{X86_INS_EMMS, {0x0f, 0x77}},
          StateInstruction{X86_INS_FEMMS, {0x0f, 0x0e}}})
      for (uint8_t Prefix : {0, 0x67, 0x2e, 0x36, 0x64, 0x65}) {
        auto Bytes = Instruction.Bytes;
        if (Prefix)
          Bytes.insert(Bytes.begin(), Prefix);
        ASSERT_NO_FATAL_FAILURE(checkRefusal(Target, Instruction.Id, Bytes));
        // An ownership request cannot supply an i386 context.
        if (Target == Arch::X86)
          ASSERT_NO_FATAL_FAILURE(
              checkRefusal(Target, Instruction.Id, Bytes, true));
      }
}

TEST(X86LegacyAdmission, OwnedX64FnopEmmsAndFemmsRetainTheirStateOperation) {
  for (const auto &Bytes :
       {std::vector<uint8_t>{0xd9, 0xd0}, std::vector<uint8_t>{0x0f, 0x77},
        std::vector<uint8_t>{0x0f, 0x0e}})
    for (bool Strict : {false, true}) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::X64));
      D.setStrict(Strict);
      D.setX87ContextOwnership(true);
      DecodedInsn Instruction{};
      ASSERT_EQ(
          D.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Instruction),
          Bytes.size());
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(Instruction, Ops));
      ASSERT_EQ(Ops.size(), 1u);
      const auto &Op = Ops.front();
      ASSERT_EQ(Op.Opcode, NdOp::INTRINSIC);
      ASSERT_EQ(Op.NumInputs, 6u);
      EXPECT_EQ(Op.Inputs[0].Offset, unsigned(Intrinsic::X87ContextStep));
      EXPECT_EQ(Op.Inputs[2].Offset, Bytes[0] | unsigned(Bytes[1]) << 8);
      EXPECT_TRUE(x87ContextLowShapeIsValid(Op, Arch::X64));
    }
}
