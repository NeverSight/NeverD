//===- COFFExceptionARM32UnwindTests.cpp - ARM32 unwind code tests ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFExceptionTestsDetail.h"
#include "neverd/loader/COFF/COFFUnwindARM.h"
#include "neverd/loader/ExceptionInfo.h"

namespace {

using namespace neverd;
using namespace neverd::coff_eh_test;
using coff_loader::decodeARM32UnwindCodes;
using coff_loader::expandARM32PackedUnwind;

TEST(ARM32Unwind, DecodesThePopAndAllocateForms) {
  // `pop {r4-r7,lr}` (D7), `vpop {d8-d9}` (E1), `add sp,sp,#0x20` (08), end.
  const std::vector<uint8_t> Codes = {0xD7, 0xE1, 0x08, 0xFF};
  coff_loader::ARMUnwindDecode Decoded = decodeARM32UnwindCodes(Codes);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.Operations.size(), 4u);

  EXPECT_EQ(registersOf(Decoded.Operations[0]),
            (std::vector<uint16_t>{4, 5, 6, 7, 14}));
  EXPECT_EQ(Decoded.Operations[0].StackOffset, 20u);
  // A `pop` of low registers is a 16-bit instruction; a `vpop` is always 32.
  EXPECT_EQ(Decoded.Operations[0].InstructionSize, 2u);

  EXPECT_EQ(Decoded.Operations[1].RegisterClass,
            UnwindRegisterClass::FloatingPoint);
  EXPECT_EQ(registersOf(Decoded.Operations[1]), (std::vector<uint16_t>{8, 9}));
  EXPECT_EQ(Decoded.Operations[1].InstructionSize, 4u);

  EXPECT_EQ(Decoded.Operations[2].Kind, UnwindOperationKind::AllocateStack);
  EXPECT_EQ(Decoded.Operations[2].StackOffset, 32u);
  EXPECT_EQ(Decoded.PrologueSize, 8u);
}

TEST(ARM32Unwind, DecodesTheWideRegisterMask) {
  // `80-BF` carries a 13-bit mask over r0-r12 plus a separate lr bit.
  const std::vector<uint8_t> Codes = {0xA0, 0xF0, 0xFF};
  coff_loader::ARMUnwindDecode Decoded = decodeARM32UnwindCodes(Codes);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.Operations.size(), 2u);
  EXPECT_EQ(registersOf(Decoded.Operations[0]),
            (std::vector<uint16_t>{4, 5, 6, 7, 14}));
  EXPECT_EQ(Decoded.Operations[0].StackOffset, 20u);
}

TEST(ARM32Unwind, DecodesTheFrameChainAndReturnAddressForms) {
  // `mov sp,r7` (C7), `ldr lr,[sp],#8` (EF 02), end.
  const std::vector<uint8_t> Codes = {0xC7, 0xEF, 0x02, 0xFF};
  coff_loader::ARMUnwindDecode Decoded = decodeARM32UnwindCodes(Codes);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.Operations.size(), 3u);
  EXPECT_EQ(Decoded.Operations[0].Kind,
            UnwindOperationKind::SetStackPointerFromRegister);
  EXPECT_EQ(Decoded.Operations[0].Register, 7u);
  EXPECT_EQ(Decoded.Operations[1].Kind,
            UnwindOperationKind::LoadReturnAddress);
  EXPECT_EQ(Decoded.Operations[1].StackOffset, 8u);
}

TEST(ARM32Unwind, ExpandsPackedDataForAChainedFrame) {
  // Reg=2 (r4-r6), R=0, L=1, C=1, StackAdjust=4 words.
  const uint32_t Packed = 1u | (2u << 16) | (1u << 20) | (1u << 21) |
                          (4u << 22);
  coff_loader::ARMUnwindDecode Decoded = expandARM32PackedUnwind(Packed);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.Operations.size(), 3u);

  // The chained frame adds r11 to the pushed set alongside lr.
  EXPECT_EQ(registersOf(Decoded.Operations[0]),
            (std::vector<uint16_t>{4, 5, 6, 11, 14}));
  EXPECT_EQ(Decoded.Operations[1].Kind, UnwindOperationKind::AddFramePointer);
  EXPECT_EQ(Decoded.Operations[1].Register, 11u);
  EXPECT_EQ(Decoded.Operations[2].Kind, UnwindOperationKind::AllocateStack);
  EXPECT_EQ(Decoded.Operations[2].StackOffset, 16u);
}

TEST(ARM32Unwind, RefusesAChainedFrameThatDoesNotSaveTheLinkRegister) {
  // C=1 without L=1 is an encoding the ABI forbids, because a frame chain
  // needs both r11 and lr.
  const uint32_t Packed = 1u | (1u << 21);
  coff_loader::ARMUnwindDecode Decoded = expandARM32PackedUnwind(Packed);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Malformed);
  EXPECT_TRUE(Decoded.Operations.empty());
}

} // namespace
