//===- X86_64_EncodingAccuracyTests.cpp - Encoding regressions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <algorithm>
#include <vector>

using namespace neverd;

namespace {
int decodeRoute(Decoder &Decode, unsigned Route, const uint8_t *Bytes,
                size_t Size, DecodedInsn &Out) {
  if (Route == 0)
    return Decode.decodeOne(Bytes, Size, 0x1000, Out);
  if (Route == 1)
    return Decode.decodeOneLight(Bytes, Size, 0x1000, Out);
  return Decode.decodeOneForLift(Bytes, Size, 0x1000, Out);
}
} // namespace

TEST(X86EncodingAccuracy, InvalidLockAndMovCsRejectedByEveryDecodeRoute) {
  const std::vector<std::vector<uint8_t>> Invalid = {
      {0xf0, 0x33, 0x02},       // LOCK XOR EAX,[EDX/RDX]: register destination.
      {0xf0, 0x03, 0x02},       // LOCK ADD with a memory source.
      {0xf0, 0x0b, 0x02},       // LOCK OR with a memory source.
      {0xf0, 0x13, 0x02},       // LOCK ADC with a memory source.
      {0xf0, 0x1b, 0x02},       // LOCK SBB with a memory source.
      {0xf0, 0x23, 0x02},       // LOCK AND with a memory source.
      {0xf0, 0x2b, 0x02},       // LOCK SUB with a memory source.
      {0xf0, 0x01, 0xc0},       // LOCK ADD EAX,EAX.
      {0xf0, 0x89, 0x02},       // LOCK MOV [EDX/RDX],EAX.
      {0xf0, 0x39, 0x02},       // LOCK CMP [EDX/RDX],EAX.
      {0xf0, 0x0f, 0xa3, 0x02}, // LOCK BT: no write.
      {0xf0, 0x0f, 0x1f, 0x00}, // LOCK multi-byte NOP.
      {0xf2, 0xf0, 0x33, 0x02}, // HLE cannot legalize a register destination.
      {0xf0, 0xf2, 0x33, 0x02},
      {0x8e, 0xc8}, // MOV CS,AX.
      {0x8e, 0x0a}, // MOV CS,[EDX/RDX].
      {0x66, 0x8e, 0xc8}};
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (bool Detail : {false, true})
        for (bool Text : {false, true})
          for (unsigned Route : {0U, 1U, 2U}) {
            Decoder Decode;
            ASSERT_TRUE(Decode.init(Target));
            Decode.setStrict(Strict);
            Decode.setDetail(Detail);
            Decode.setText(Text);
            auto Cases = Invalid;
            if (Target == Arch::X64) {
              Cases.push_back({0xf0, 0xd5, 0x58, 0x33, 0x02});
              Cases.push_back({0xd5, 0x44, 0x8e, 0xc8});
            }
            for (const auto &Bytes : Cases) {
              SCOPED_TRACE(static_cast<unsigned>(Target));
              SCOPED_TRACE(Strict);
              SCOPED_TRACE(Detail);
              SCOPED_TRACE(Text);
              SCOPED_TRACE(Route);
              SCOPED_TRACE(testing::PrintToString(Bytes));
              DecodedInsn Instruction;
              EXPECT_EQ(decodeRoute(Decode, Route, Bytes.data(), Bytes.size(),
                                    Instruction),
                        0);
            }
          }
}

TEST(X86EncodingAccuracy, DecodersAfterOneWithoutDetailOrTextDecodeInFull) {
  // mov rax, [rcx+8] / mov eax, [ecx+8]
  const uint8_t Bytes64[] = {0x48, 0x8b, 0x41, 0x08};
  const uint8_t Bytes32[] = {0x8b, 0x41, 0x08};
  for (Arch Target : {Arch::X86, Arch::X64}) {
    const uint8_t *Bytes = Target == Arch::X64 ? Bytes64 : Bytes32;
    const size_t Size = Target == Arch::X64 ? sizeof(Bytes64) : sizeof(Bytes32);
    // Decoders that turn detail and text off and close, as a lightweight
    // scan's do, leave nothing a later decoder sees.
    for (int Round = 0; Round < 3; ++Round) {
      {
        Decoder Light;
        ASSERT_TRUE(Light.init(Target));
        Light.setDetail(false);
        Light.setText(false);
        DecodedInsn Instruction;
        ASSERT_EQ(Light.decodeOneLight(Bytes, Size, 0x1000, Instruction),
                  static_cast<int>(Size));
      }
      Decoder Full;
      ASSERT_TRUE(Full.init(Target));
      DecodedInsn Instruction;
      ASSERT_EQ(Full.decodeOne(Bytes, Size, 0x1000, Instruction),
                static_cast<int>(Size));
      EXPECT_STREQ(Instruction.Raw->mnemonic, "mov");
      EXPECT_STREQ(Instruction.Raw->op_str, Target == Arch::X64
                                                ? "rax, qword ptr [rcx + 8]"
                                                : "eax, dword ptr [ecx + 8]");
      ASSERT_NE(Instruction.Raw->detail, nullptr);
      ASSERT_EQ(Instruction.Raw->detail->x86.op_count, 2);
      EXPECT_EQ(Instruction.Raw->detail->x86.operands[1].type, X86_OP_MEM);
      EXPECT_EQ(Instruction.Raw->detail->x86.operands[1].mem.disp, 8);
      std::vector<LowOp> Ops;
      Full.liftToLow(Instruction, Ops);
      EXPECT_FALSE(Ops.empty());
    }
  }
}

TEST(X86EncodingAccuracy, LegalMemoryLockAndSegmentMovesStillLift) {
  const std::vector<std::vector<uint8_t>> Valid = {{0xf0, 0x01, 0x02},
                                                   {0xf0, 0x0f, 0xab, 0x02},
                                                   {0xf0, 0x0f, 0xb1, 0x02},
                                                   {0xf0, 0x0f, 0xc1, 0x02},
                                                   {0xf0, 0x87, 0x02},
                                                   {0x8c, 0xc8},
                                                   {0x8e, 0xe0},
                                                   {0x8e, 0xe8}};
  for (Arch Target : {Arch::X86, Arch::X64}) {
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Target));
    for (const auto &Bytes : Valid) {
      SCOPED_TRACE(testing::PrintToString(Bytes));
      DecodedInsn Instruction;
      ASSERT_EQ(
          Decode.decodeOne(Bytes.data(), Bytes.size(), 0x1000, Instruction),
          static_cast<int>(Bytes.size()));
      std::vector<LowOp> Ops;
      EXPECT_NO_THROW(Decode.liftToLow(Instruction, Ops));
      EXPECT_FALSE(Ops.empty());
    }
  }
}

// These are legal encodings the decoder accepts. The ordinary lift still
// refuses them instead of printing a plain read-modify-write.
TEST(X86EncodingAccuracy, LockedLogicWithoutAnOrderedModelIsRefused) {
  const std::vector<uint8_t> Logic = {0xf0, 0x31, 0x02};
  const std::vector<std::vector<uint8_t>> Elided = {{0xf2, 0xf0, 0x31, 0x02},
                                                    {0xf0, 0xf2, 0x31, 0x02}};
  const auto Lifts = [](Arch Target, const std::vector<uint8_t> &Bytes) {
    Decoder Decode;
    EXPECT_TRUE(Decode.init(Target));
    DecodedInsn Instruction;
    EXPECT_EQ(Decode.decodeOne(Bytes.data(), Bytes.size(), 0x1000, Instruction),
              static_cast<int>(Bytes.size()));
    std::vector<LowOp> Ops;
    try {
      Decode.liftToLow(Instruction, Ops);
    } catch (const UnliftedInstruction &) {
      EXPECT_TRUE(Ops.empty());
      return false;
    }
    EXPECT_FALSE(Ops.empty());
    return true;
  };
  EXPECT_TRUE(Lifts(Arch::X64, Logic));
  EXPECT_FALSE(Lifts(Arch::X86, Logic));
  for (const auto &Bytes : Elided) {
    SCOPED_TRACE(testing::PrintToString(Bytes));
    EXPECT_FALSE(Lifts(Arch::X86, Bytes));
    EXPECT_FALSE(Lifts(Arch::X64, Bytes));
  }
}

TEST(X86EncodingAccuracy, Ud1ConsumesCompleteAddressAndAllRoutesAgree) {
  const std::vector<std::vector<uint8_t>> Complete = {
      {0x0f, 0xb9, 0x0a},
      {0x0f, 0xb9, 0x5e, 0x7e},
      {0x0f, 0xb9, 0xbd, 0xdd, 0xcc, 0xbb, 0xaa},
      {0x0f, 0xb9, 0x24, 0x50},
      {0x0f, 0xb9, 0x4c, 0x9a, 0x7e},
      {0x0f, 0xb9, 0x9c, 0xf6, 0xdd, 0xcc, 0xbb, 0xaa},
      {0x0f, 0xb9, 0xfd},
      {0x0f, 0xb9, 0x05, 1, 2, 3, 4},
      {0x0f, 0xb9, 0x04, 0x25, 1, 2, 3, 4}};
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Detail : {false, true})
      for (bool Text : {false, true})
        for (unsigned Route : {0U, 1U, 2U}) {
          Decoder Decode;
          ASSERT_TRUE(Decode.init(Target));
          Decode.setDetail(Detail);
          Decode.setText(Text);
          auto Cases = Complete;
          if (Target == Arch::X64) {
            Cases.push_back({0x66, 0x0f, 0xb9, 0xc1});
            Cases.push_back({0x48, 0x0f, 0xb9, 0xc1});
            Cases.push_back({0xd5, 0x80, 0xb9, 0xc1});
            Cases.push_back({0xd5, 0xc0, 0xb9, 0xc1});
            Cases.push_back({0xd5, 0xa0, 0xb9, 0x04, 0x50});
            Cases.push_back({0x67, 0xd5, 0xa0, 0xb9, 0x04, 0x50});
          }
          for (const auto &Bytes : Cases) {
            SCOPED_TRACE(testing::PrintToString(Bytes));
            const auto Read = [&](const uint8_t *Data, size_t Size,
                                  DecodedInsn &Out) {
              return decodeRoute(Decode, Route, Data, Size, Out);
            };
            DecodedInsn Instruction;
            for (unsigned Truncated = 0; Truncated < Bytes.size(); ++Truncated)
              EXPECT_EQ(Read(Bytes.data(), Truncated, Instruction), 0);
            auto Followed = Bytes;
            Followed.push_back(0x90);
            ASSERT_EQ(Read(Followed.data(), Followed.size(), Instruction),
                      static_cast<int>(Bytes.size()));
            EXPECT_EQ(Instruction.Id, X86_INS_UD1);
            EXPECT_EQ(Instruction.Size, Bytes.size());
            EXPECT_TRUE(
                std::equal(Bytes.begin(), Bytes.end(), Instruction.Raw->bytes));
            ASSERT_EQ(Read(Followed.data() + Instruction.Size, 1, Instruction),
                      1);
            EXPECT_EQ(Instruction.Id, X86_INS_NOP);
          }
        }
}

TEST(X86EncodingAccuracy, Ud1OperandsDoNotBecomeMemoryOrArithmeticEffects) {
  for (Arch Target : {Arch::X86, Arch::X64}) {
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Target));
    const std::vector<std::vector<uint8_t>> Cases = {
        {0x0f, 0xb9, 0xc1}, {0x0f, 0xb9, 0x9c, 0xf6, 0xdd, 0xcc, 0xbb, 0xaa}};
    for (const auto &Bytes : Cases) {
      DecodedInsn Instruction;
      ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000,
                                        Instruction),
                static_cast<int>(Bytes.size()));
      ASSERT_EQ(Instruction.Raw->detail->x86.op_count, 2);
      std::vector<LowOp> Ops;
      Decode.liftToLow(Instruction, Ops);
      ASSERT_EQ(Ops.size(), 1U);
      EXPECT_EQ(Ops[0].Opcode, NdOp::INTRINSIC);
      ASSERT_EQ(Ops[0].NumInputs, 1);
      EXPECT_TRUE(Ops[0].Inputs[0].isConst());
      EXPECT_EQ(Ops[0].Inputs[0].Offset, static_cast<uint64_t>(Intrinsic::Ud1));
    }
  }
}

TEST(X86EncodingAccuracy, Ud1AddressOverridesAndLengthLimitAreExplicit) {
  const std::vector<std::vector<uint8_t>> Address16 = {
      {0x67, 0x0f, 0xb9, 0x06, 0x34, 0x12},
      {0x67, 0x0f, 0xb9, 0x86, 0x34, 0x12},
      {0x67, 0x0f, 0xb9, 0x44, 0x12}};
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X86));
  DecodedInsn Instruction;
  for (const auto &Bytes : Address16) {
    ASSERT_EQ(Decode.decodeOne(Bytes.data(), Bytes.size(), 0x1000, Instruction),
              static_cast<int>(Bytes.size()));
    EXPECT_EQ(Instruction.Raw->detail->x86.addr_size, 2);
  }
  ASSERT_TRUE(Decode.init(Arch::X64));
  const uint8_t Address32[] = {0x67, 0x0f, 0xb9, 0x84, 0x25, 1, 2, 3, 4};
  ASSERT_EQ(Decode.decodeOne(Address32, sizeof(Address32), 0x1000, Instruction),
            9);
  EXPECT_EQ(Instruction.Raw->detail->x86.addr_size, 4);
  EXPECT_EQ(Instruction.Raw->detail->x86.encoding.disp_offset, 5);
  EXPECT_EQ(Instruction.Raw->detail->x86.encoding.disp_size, 4);
  std::vector<uint8_t> Maximum(12, 0x66);
  Maximum.insert(Maximum.end(), {0x0f, 0xb9, 0xc0});
  ASSERT_EQ(
      Decode.decodeOne(Maximum.data(), Maximum.size(), 0x1000, Instruction),
      15);
  Maximum.insert(Maximum.begin(), 0x66);
  EXPECT_EQ(
      Decode.decodeOne(Maximum.data(), Maximum.size(), 0x1000, Instruction), 0);
}

TEST(X86EncodingAccuracy, MandatoryPrefixPreservesI386AddressOverride) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X86));
  const uint8_t Bytes[] = {0x67, 0xf2, 0x0f, 0x2d, 0x00};
  DecodedInsn Instruction;
  ASSERT_EQ(Decode.decodeOne(Bytes, sizeof(Bytes), 0x1000, Instruction), 5);
  EXPECT_EQ(Instruction.Id, X86_INS_CVTSD2SI);
  const auto &X86 = Instruction.Raw->detail->x86;
  ASSERT_EQ(X86.op_count, 2);
  EXPECT_EQ(X86.addr_size, 2);
  EXPECT_EQ(X86.operands[1].type, X86_OP_MEM);
  EXPECT_EQ(X86.operands[1].mem.base, X86_REG_BX);
  EXPECT_EQ(X86.operands[1].mem.index, X86_REG_SI);
}
