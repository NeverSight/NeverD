//===- X86LegacyAdmissionTests.cpp - Incomplete legacy state admission --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decoded legacy instructions must not publish scalar approximations of
/// segment transitions or packed 3DNow! arithmetic.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Lifter.h"

#include <cstdint>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t kEntry = 0x1000;

void checkRefusal(Arch Target, unsigned Id, const std::vector<uint8_t> &Bytes) {
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

TEST(X86LegacyAdmission, FarPointerLoadsRequireSegmentState) {
  struct Encoding {
    unsigned Id;
    uint8_t Opcode;
    bool Escape;
  };
  constexpr Encoding Encodings[] = {{X86_INS_LDS, 0xc5, false},
                                    {X86_INS_LES, 0xc4, false},
                                    {X86_INS_LSS, 0xb2, true},
                                    {X86_INS_LFS, 0xb4, true},
                                    {X86_INS_LGS, 0xb5, true}};
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &E : Encodings) {
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
  // AMD APM volume 5: the final opcode selects the packed operation. Keep
  // this independent of the lifter's dispatch and exercise register/memory.
  struct Encoding {
    unsigned Id;
    uint8_t Opcode;
  };
  constexpr Encoding Encodings[] = {
      {X86_INS_PFADD, 0x9e},    {X86_INS_PFACC, 0xae},
      {X86_INS_PFNACC, 0x8a},   {X86_INS_PFPNACC, 0x8e},
      {X86_INS_PFSUB, 0x9a},    {X86_INS_PFSUBR, 0xaa},
      {X86_INS_PFMUL, 0xb4},    {X86_INS_PFMAX, 0xa4},
      {X86_INS_PFMIN, 0x94},    {X86_INS_PFCMPEQ, 0xb0},
      {X86_INS_PFCMPGE, 0x90},  {X86_INS_PFCMPGT, 0xa0},
      {X86_INS_PFRCP, 0x96},    {X86_INS_PFRCPIT1, 0xa6},
      {X86_INS_PFRCPIT2, 0xb6}, {X86_INS_PFRSQIT1, 0xa7},
      {X86_INS_PFRSQRT, 0x97},  {X86_INS_PF2ID, 0x1d},
      {X86_INS_PF2IW, 0x1c},    {X86_INS_PI2FD, 0x0d},
      {X86_INS_PI2FW, 0x0c},    {X86_INS_PSWAPD, 0xbb},
      {X86_INS_PMULHRW, 0xb7},  {X86_INS_PAVGUSB, 0xbf}};
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &E : Encodings)
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
