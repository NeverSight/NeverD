#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/TargetCodegenInfo.h"

using namespace neverd;

TEST(LowControlTarget, CanonicalizesARMInterworkingTargets) {
  auto Zero = canonicalizeLowControlTarget(
      0, InstructionMode::ARM, LowInstructionTargetMode::FromTargetBit0);
  ASSERT_TRUE(static_cast<bool>(Zero));
  EXPECT_EQ(Zero->Address, 0u);
  EXPECT_EQ(Zero->Mode, InstructionMode::ARM);

  auto ThumbZero = canonicalizeLowControlTarget(
      1, InstructionMode::ARM, LowInstructionTargetMode::FromTargetBit0);
  ASSERT_TRUE(static_cast<bool>(ThumbZero));
  EXPECT_EQ(ThumbZero->Address, 0u);
  EXPECT_EQ(ThumbZero->Mode, InstructionMode::Thumb);

  auto Thumb = canonicalizeLowControlTarget(
      0x1001, InstructionMode::ARM, LowInstructionTargetMode::FromTargetBit0);
  ASSERT_TRUE(static_cast<bool>(Thumb));
  EXPECT_EQ(Thumb->Address, 0x1000u);
  EXPECT_EQ(Thumb->Mode, InstructionMode::Thumb);

  auto FixedARM = canonicalizeLowControlTarget(0x2000, InstructionMode::Thumb,
                                               LowInstructionTargetMode::ARM);
  ASSERT_TRUE(static_cast<bool>(FixedARM));
  EXPECT_EQ(FixedARM->Address, 0x2000u);
  EXPECT_EQ(FixedARM->Mode, InstructionMode::ARM);

  auto FixedThumb = canonicalizeLowControlTarget(
      0x2002, InstructionMode::ARM, LowInstructionTargetMode::Thumb);
  ASSERT_TRUE(static_cast<bool>(FixedThumb));
  EXPECT_EQ(FixedThumb->Address, 0x2002u);
  EXPECT_EQ(FixedThumb->Mode, InstructionMode::Thumb);

  auto Preserved = canonicalizeLowControlTarget(
      0x3000, InstructionMode::Thumb, LowInstructionTargetMode::Preserve);
  ASSERT_TRUE(static_cast<bool>(Preserved));
  EXPECT_EQ(Preserved->Address, 0x3000u);
  EXPECT_EQ(Preserved->Mode, InstructionMode::Thumb);

  auto WideDefault =
      canonicalizeLowControlTarget(uint64_t{1} << 40, InstructionMode::Default,
                                   LowInstructionTargetMode::Preserve);
  ASSERT_TRUE(static_cast<bool>(WideDefault));
  EXPECT_EQ(WideDefault->Address, uint64_t{1} << 40);
  EXPECT_EQ(WideDefault->Mode, InstructionMode::Default);
}

TEST(LowControlTarget, RejectsInvalidModesAlignmentAndAddressOverflow) {
  auto ExpectInvalid = [](uint64_t RawTarget, InstructionMode SourceMode,
                          LowInstructionTargetMode TargetMode) {
    auto Target =
        canonicalizeLowControlTarget(RawTarget, SourceMode, TargetMode);
    EXPECT_FALSE(Target);
    if (!Target)
      llvm::consumeError(Target.takeError());
  };

  ExpectInvalid(InvalidVA, InstructionMode::Default,
                LowInstructionTargetMode::Preserve);
  ExpectInvalid(uint64_t{1} << 32, InstructionMode::ARM,
                LowInstructionTargetMode::Preserve);
  ExpectInvalid(0x1002, InstructionMode::ARM,
                LowInstructionTargetMode::Preserve);
  ExpectInvalid(0x1001, InstructionMode::Thumb,
                LowInstructionTargetMode::Preserve);
  ExpectInvalid(0x1002, InstructionMode::Thumb,
                LowInstructionTargetMode::FromTargetBit0);
  ExpectInvalid(0x1000, InstructionMode::Default,
                LowInstructionTargetMode::FromTargetBit0);
  ExpectInvalid(0x1000, static_cast<InstructionMode>(0xff),
                LowInstructionTargetMode::Preserve);
  ExpectInvalid(0x1000, InstructionMode::ARM,
                static_cast<LowInstructionTargetMode>(0xff));
}

TEST(InstructionMode, NormalizesAndSerializesThumbCodeAddresses) {
  EXPECT_EQ(normalizeCodeAddress(0x1001, Arch::ARM, InstructionMode::Thumb),
            0x1000u);
  EXPECT_EQ(serializeCodePointer(0x1000, Arch::ARM, InstructionMode::Thumb),
            0x1001u);
  EXPECT_EQ(normalizeCodeAddress(0x1001, Arch::ARM, InstructionMode::ARM),
            0x1001u);
  EXPECT_EQ(
      serializeCodePointer(0x1000, Arch::AArch64, InstructionMode::Default),
      0x1000u);
}

TEST(InstructionMode, DecoderSelectsThumb2WithoutChangingARMDefault) {
  const uint8_t ThumbAdds[] = {0x01, 0x30}; // adds r0, #1
  const uint8_t ArmNop[] = {0x00, 0x00, 0xa0, 0xe1};
  DecodedInsn Insn{};

  Decoder Thumb;
  ASSERT_TRUE(Thumb.init(Arch::ARM, InstructionMode::Thumb));
  ASSERT_EQ(Thumb.decodeOne(ThumbAdds, sizeof(ThumbAdds), 0x1000, Insn), 2);
  EXPECT_STREQ(Insn.Raw->mnemonic, "adds");

  Decoder Arm;
  ASSERT_TRUE(Arm.init(Arch::ARM));
  ASSERT_EQ(Arm.decodeOne(ArmNop, sizeof(ArmNop), 0x2000, Insn), 4);
  EXPECT_STREQ(Insn.Raw->mnemonic, "mov");
}

TEST(InstructionMode, ThumbUsesHalfwordNopsAndARMUsesWordNops) {
  std::vector<uint8_t> Thumb;
  ASSERT_TRUE(
      getTargetCodegenInfo(Arch::ARM, InstructionMode::Thumb).appendNop(Thumb));
  EXPECT_EQ(Thumb, (std::vector<uint8_t>{0x00, 0xbf}));

  std::vector<uint8_t> Arm;
  ASSERT_TRUE(
      getTargetCodegenInfo(Arch::ARM, InstructionMode::ARM).appendNop(Arm));
  EXPECT_EQ(Arm.size(), 4u);
}

TEST(InstructionMode, ThumbBWUsesArchitecturalPCBias) {
  std::vector<uint8_t> Code(4, 0xcc);
  auto TCI = getTargetCodegenInfo(Arch::ARM, InstructionMode::Thumb);
  ASSERT_TRUE(TCI.writeTrampoline(Code, 0, 0x1004, 0x1000, 4));
  EXPECT_EQ(Code, (std::vector<uint8_t>{0x00, 0xf0, 0x00, 0xb8}));
}

TEST(InstructionMode, ThumbBWChecksSignedRangeAndSafeSpan) {
  auto TCI = getTargetCodegenInfo(Arch::ARM, InstructionMode::Thumb);
  for (int64_t Diff : {-(int64_t(1) << 24), (int64_t(1) << 24) - 2}) {
    std::vector<uint8_t> Code(6, 0xaa);
    EXPECT_TRUE(
        TCI.writeTrampoline(Code, 0, uint64_t(0x2000004 + Diff), 0x2000000, 4));
    EXPECT_EQ(Code[4], 0xaa);
    EXPECT_EQ(Code[5], 0xaa);
  }
  for (int64_t Diff : {-(int64_t(1) << 24) - 2, (int64_t(1) << 24)}) {
    std::vector<uint8_t> Code(6, 0xaa);
    EXPECT_FALSE(
        TCI.writeTrampoline(Code, 0, uint64_t(0x2000004 + Diff), 0x2000000, 4));
    EXPECT_EQ(Code, (std::vector<uint8_t>(6, 0xaa)));
  }
  std::vector<uint8_t> Short(6, 0xaa);
  EXPECT_FALSE(TCI.writeTrampoline(Short, 0, 0x3004, 0x3000, 2));
  EXPECT_EQ(Short, (std::vector<uint8_t>(6, 0xaa)));
}

TEST(InstructionMode, ThumbBWMatchesClangForwardAndBackwardEncodings) {
  // Generated with:
  // clang -target thumbv7-pc-windows-msvc -c /tmp/thumb-bw.s -o
  // /tmp/thumb-bw.obj /usr/bin/objdump -d /tmp/thumb-bw.obj
  auto TCI = getTargetCodegenInfo(Arch::ARM, InstructionMode::Thumb);
  std::vector<uint8_t> Forward(4);
  ASSERT_TRUE(TCI.writeTrampoline(Forward, 0, 0x1008, 0x1000, 4));
  EXPECT_EQ(Forward, (std::vector<uint8_t>{0x00, 0xf0, 0x02, 0xb8}));

  std::vector<uint8_t> Backward(4);
  ASSERT_TRUE(TCI.writeTrampoline(Backward, 0, 0x100a, 0x100e, 4));
  EXPECT_EQ(Backward, (std::vector<uint8_t>{0xff, 0xf7, 0xfc, 0xbf}));
}

TEST(InstructionMode, LegacyTrampolinesHonorExactOverwriteLimits) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::AArch64, Arch::ARM}) {
    auto TCI = getTargetCodegenInfo(A);
    ASSERT_GT(TCI.trampolineSize(), 0u);

    std::vector<uint8_t> TooShort(16, 0xaa);
    EXPECT_FALSE(TCI.writeTrampoline(TooShort, 0, 0x1100, 0x1000,
                                     TCI.trampolineSize() - 1));
    EXPECT_EQ(TooShort, (std::vector<uint8_t>(16, 0xaa)));

    std::vector<uint8_t> Exact(16, 0xaa);
    EXPECT_TRUE(
        TCI.writeTrampoline(Exact, 0, 0x1100, 0x1000, TCI.trampolineSize()));
  }
}
