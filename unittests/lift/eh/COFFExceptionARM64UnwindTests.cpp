//===- COFFExceptionARM64UnwindTests.cpp - ARM64 unwind code tests ----===//
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
using coff_loader::decodeARM64UnwindCodes;
using coff_loader::expandARM64PackedUnwind;

TEST(ARM64Unwind, DecodesTheDocumentedFramePointerPrologue) {
  // `0xe42291e1`, the unwind word Microsoft's own worked example publishes for
  // a frame-pointer function.  Its bytes decode in the order they are stored.
  const std::vector<uint8_t> Codes = {0xE1, 0x91, 0x22, 0xE4};
  coff_loader::ARMUnwindDecode Decoded = decodeARM64UnwindCodes(Codes);

  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.Operations.size(), 4u);

  EXPECT_EQ(Decoded.Operations[0].Kind, UnwindOperationKind::SetFramePointer);
  EXPECT_EQ(Decoded.Operations[0].Register, 29u);

  // `save_fplr_x` with Z=0x11 stores <x29,lr> at [sp-(0x11+1)*8]!.
  EXPECT_EQ(Decoded.Operations[1].Kind,
            UnwindOperationKind::SaveRegisterPairPreIndexed);
  EXPECT_EQ(registersOf(Decoded.Operations[1]),
            (std::vector<uint16_t>{29, 30}));
  EXPECT_EQ(Decoded.Operations[1].StackOffset, 144u);

  // `save_r19r20_x` with Z=2 stores <x19,x20> at [sp-2*8]!.
  EXPECT_EQ(Decoded.Operations[2].Kind,
            UnwindOperationKind::SaveRegisterPairPreIndexed);
  EXPECT_EQ(registersOf(Decoded.Operations[2]),
            (std::vector<uint16_t>{19, 20}));
  EXPECT_EQ(Decoded.Operations[2].StackOffset, 16u);

  EXPECT_EQ(Decoded.Operations[3].Kind, UnwindOperationKind::End);
  // The terminator stands against no prologue instruction; the other three do.
  EXPECT_EQ(Decoded.PrologueSize, 12u);
}

TEST(ARM64Unwind, StartsAnEpilogueMidwayThroughASharedCodeArray) {
  // Microsoft's second worked example: an epilogue whose start index points
  // into the middle of the prologue's codes, so the two share a tail.
  const std::vector<uint8_t> Codes = {0xE3, 0xE3, 0xE3, 0xE3,
                                      0xD6, 0x00, 0x05, 0xE4};
  coff_loader::ARMUnwindDecode Prologue = decodeARM64UnwindCodes(Codes);
  ASSERT_EQ(Prologue.Operations.size(), 7u);
  for (unsigned I = 0; I < 4; ++I)
    EXPECT_EQ(Prologue.Operations[I].Kind, UnwindOperationKind::Nop);

  // `save_lrpair` pairs a callee-saved register with lr rather than with its
  // neighbour, which is why the two are not adjacent.
  EXPECT_EQ(Prologue.Operations[4].Kind,
            UnwindOperationKind::SaveRegisterPair);
  EXPECT_EQ(registersOf(Prologue.Operations[4]),
            (std::vector<uint16_t>{19, 30}));

  EXPECT_EQ(Prologue.Operations[5].Kind, UnwindOperationKind::AllocateStack);
  EXPECT_EQ(Prologue.Operations[5].StackOffset, 80u);

  coff_loader::ARMUnwindDecode Epilogue =
      decodeARM64UnwindCodes(Codes, /*StartOffset=*/4);
  EXPECT_EQ(Epilogue.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Epilogue.Operations.size(), 3u);
  EXPECT_EQ(Epilogue.Operations[0].Kind,
            UnwindOperationKind::SaveRegisterPair);
  EXPECT_EQ(Epilogue.Operations[0].CodeOffset, 4u);
}

TEST(ARM64Unwind, ResolvesSaveNextAgainstTheCodeThatFollowsIt) {
  // The array runs from the last prologue instruction back towards the first,
  // so the pair a `save_next` extends is described by the *following* code.
  // Here `save_fregp_x` stores d8,d9 while claiming 48 bytes, and the two
  // `save_next` codes above it are the two pairs the prologue stored after it.
  const std::vector<uint8_t> Codes = {0xE6, 0xE6, 0xDA, 0x05, 0xE4};
  coff_loader::ARMUnwindDecode Decoded = decodeARM64UnwindCodes(Codes);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.Operations.size(), 4u);

  EXPECT_EQ(registersOf(Decoded.Operations[2]), (std::vector<uint16_t>{8, 9}));
  EXPECT_EQ(Decoded.Operations[2].StackOffset, 48u);

  // A pre-indexed store leaves its registers at the bottom of what it claimed,
  // so the pair above it sits at 16 rather than at 48 plus 16.
  EXPECT_EQ(Decoded.Operations[1].Kind, UnwindOperationKind::SaveNextPair);
  EXPECT_EQ(registersOf(Decoded.Operations[1]),
            (std::vector<uint16_t>{10, 11}));
  EXPECT_EQ(Decoded.Operations[1].StackOffset, 16u);
  EXPECT_EQ(registersOf(Decoded.Operations[0]),
            (std::vector<uint16_t>{12, 13}));
  EXPECT_EQ(Decoded.Operations[0].StackOffset, 32u);
  EXPECT_EQ(Decoded.Operations[0].RegisterClass,
            UnwindRegisterClass::FloatingPoint);
}

TEST(ARM64Unwind, DecodesASaveNextFromTheCorpus) {
  // `e2 04 44 e6 28 e4`, taken byte for byte from the clang-cl AArch64 image
  // `xframe_eh_exe-clang-cl-aarch64-native-gs-o2.exe`.  Resolving the
  // `save_next` against the code before it instead of the one after names
  // x31 -- a register that does not exist -- which is how the direction was
  // caught in the first place.
  const std::vector<uint8_t> Codes = {0xE2, 0x04, 0x44, 0xE6, 0x28, 0xE4};
  coff_loader::ARMUnwindDecode Decoded = decodeARM64UnwindCodes(Codes);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.Operations.size(), 5u);

  EXPECT_EQ(Decoded.Operations[0].Kind, UnwindOperationKind::AddFramePointer);
  EXPECT_EQ(Decoded.Operations[0].StackOffset, 32u);
  EXPECT_EQ(registersOf(Decoded.Operations[1]),
            (std::vector<uint16_t>{29, 30}));
  EXPECT_EQ(Decoded.Operations[1].StackOffset, 32u);
  // The anchor stores x19,x20 while claiming 64 bytes, so the `save_next`
  // above it is x21,x22 one slot up from where those landed.
  EXPECT_EQ(registersOf(Decoded.Operations[2]),
            (std::vector<uint16_t>{21, 22}));
  EXPECT_EQ(Decoded.Operations[2].StackOffset, 16u);
  EXPECT_EQ(registersOf(Decoded.Operations[3]),
            (std::vector<uint16_t>{19, 20}));
  EXPECT_EQ(Decoded.Operations[3].StackOffset, 64u);
}

TEST(ARM64Unwind, ReportsASaveNextThatExtendsNoPair) {
  // `save_next` immediately before `end` extends nothing at all.
  const std::vector<uint8_t> Codes = {0xE6, 0xE4};
  coff_loader::ARMUnwindDecode Decoded = decodeARM64UnwindCodes(Codes);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Partial);
  EXPECT_FALSE(Decoded.Diagnostics.empty());
}

TEST(ARM64Unwind, DecodesTheArm64ECRegisterSaves) {
  // `save_any_reg` is the only code that can name a full q register or a
  // register the ordinary calling convention lets a function clobber.
  // `E7 66 89` is `stp q6,q7,[sp,#-0xA0]!` from the Arm64EC entry thunk
  // Microsoft documents.
  const std::vector<uint8_t> Codes = {0xE7, 0x66, 0x89, 0xE4};
  coff_loader::ARMUnwindDecode Decoded = decodeARM64UnwindCodes(Codes);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.Operations.size(), 2u);
  EXPECT_EQ(Decoded.Operations[0].Kind,
            UnwindOperationKind::SaveRegisterPairPreIndexed);
  EXPECT_EQ(Decoded.Operations[0].RegisterClass, UnwindRegisterClass::Vector);
  EXPECT_EQ(registersOf(Decoded.Operations[0]), (std::vector<uint16_t>{6, 7}));
  EXPECT_EQ(Decoded.Operations[0].StackOffset, 0xA0u);
}

TEST(ARM64Unwind, DecodesTheWideAndSignedForms) {
  // `alloc_l` with a 24-bit immediate, then `pac_sign_lr`.
  const std::vector<uint8_t> Codes = {0xE0, 0x00, 0x10, 0x00, 0xFC, 0xE4};
  coff_loader::ARMUnwindDecode Decoded = decodeARM64UnwindCodes(Codes);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.Operations.size(), 3u);
  EXPECT_EQ(Decoded.Operations[0].Kind, UnwindOperationKind::AllocateStack);
  EXPECT_EQ(Decoded.Operations[0].StackOffset, 0x1000u * 16u);
  EXPECT_EQ(Decoded.Operations[1].Kind,
            UnwindOperationKind::SignReturnAddress);
}

TEST(ARM64Unwind, KeepsWhatItReadBeforeATruncatedCode) {
  // A two-byte code whose second byte is missing.  The allocation before it is
  // still true, and reporting it beats discarding the whole frame.
  const std::vector<uint8_t> Codes = {0x05, 0xC8};
  coff_loader::ARMUnwindDecode Decoded = decodeARM64UnwindCodes(Codes);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Partial);
  ASSERT_EQ(Decoded.Operations.size(), 1u);
  EXPECT_EQ(Decoded.Operations[0].StackOffset, 80u);
}

TEST(ARM64Unwind, SkipsAReservedMultiByteCodeWithoutLosingSync) {
  // `0xFA` reserves four payload bytes.  Reading it as a one-byte code would
  // decode its payload as three more operations that do not exist.
  const std::vector<uint8_t> Codes = {0xFA, 0x11, 0x22, 0x33, 0xE1, 0xE4};
  coff_loader::ARMUnwindDecode Decoded = decodeARM64UnwindCodes(Codes);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Partial);
  ASSERT_EQ(Decoded.Operations.size(), 3u);
  EXPECT_EQ(Decoded.Operations[0].Kind, UnwindOperationKind::Opaque);
  EXPECT_EQ(Decoded.Operations[1].Kind, UnwindOperationKind::SetFramePointer);
}

TEST(ARM64Unwind, ExpandsPackedDataIntoTheCanonicalPrologue) {
  // RegI=2, RegF=0, H=0, CR=01 (unchained, lr saved), FrameSize=3 (48 bytes).
  const uint32_t Packed = 1u | (0u << 13) | (2u << 16) | (0u << 20) |
                          (1u << 21) | (3u << 23);
  coff_loader::ARMUnwindDecode Decoded = expandARM64PackedUnwind(Packed);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_GE(Decoded.Operations.size(), 2u);

  // x19 and x20 are saved by the store that also allocates the saved area.
  EXPECT_EQ(Decoded.Operations[0].Kind,
            UnwindOperationKind::SaveRegisterPairPreIndexed);
  EXPECT_EQ(registersOf(Decoded.Operations[0]),
            (std::vector<uint16_t>{19, 20}));
  // intsz is 2*8 for x19/x20 plus 8 for lr, rounded up to 32.
  EXPECT_EQ(Decoded.Operations[0].StackOffset, 32u);

  // lr follows on its own, because an even RegI leaves it unpaired.
  EXPECT_EQ(Decoded.Operations[1].Kind, UnwindOperationKind::SaveRegister);
  EXPECT_EQ(Decoded.Operations[1].Register, 30u);
  EXPECT_EQ(Decoded.Operations[1].StackOffset, 16u);

  // 48 bytes of frame less the 32 the saves take leaves 16 of locals.
  ASSERT_EQ(Decoded.Operations.size(), 3u);
  EXPECT_EQ(Decoded.Operations[2].Kind, UnwindOperationKind::AllocateStack);
  EXPECT_EQ(Decoded.Operations[2].StackOffset, 16u);
}

TEST(ARM64Unwind, PairsTheLinkRegisterWithAnOddNumberedIntegerSave) {
  // RegI=1 with CR=01 merges lr into the one integer pair rather than storing
  // it separately, which is the case the encoding calls out by name.
  const uint32_t Packed = 1u | (1u << 16) | (1u << 21) | (2u << 23);
  coff_loader::ARMUnwindDecode Decoded = expandARM64PackedUnwind(Packed);
  ASSERT_GE(Decoded.Operations.size(), 1u);
  EXPECT_EQ(Decoded.Operations[0].Kind,
            UnwindOperationKind::SaveRegisterPairPreIndexed);
  EXPECT_EQ(registersOf(Decoded.Operations[0]),
            (std::vector<uint16_t>{19, 30}));
}

TEST(ARM64Unwind, ChainsAFrameThroughTheFramePointer) {
  // RegI=0, RegF=0, CR=11 (chained), FrameSize=2 (32 bytes).
  const uint32_t Packed = 1u | (3u << 21) | (2u << 23);
  coff_loader::ARMUnwindDecode Decoded = expandARM64PackedUnwind(Packed);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Complete);
  ASSERT_EQ(Decoded.Operations.size(), 2u);
  EXPECT_EQ(Decoded.Operations[0].Kind,
            UnwindOperationKind::SaveRegisterPairPreIndexed);
  EXPECT_EQ(registersOf(Decoded.Operations[0]),
            (std::vector<uint16_t>{29, 30}));
  EXPECT_EQ(Decoded.Operations[0].StackOffset, 32u);
  EXPECT_EQ(Decoded.Operations[1].Kind, UnwindOperationKind::SetFramePointer);
}

TEST(ARM64Unwind, RefusesPackedDataThatSavesMoreThanItsFrameHolds) {
  // RegI=10 needs 80 bytes of saves, but FrameSize=1 declares 16 in total.
  const uint32_t Packed = 1u | (10u << 16) | (1u << 23);
  coff_loader::ARMUnwindDecode Decoded = expandARM64PackedUnwind(Packed);
  EXPECT_EQ(Decoded.Status, ExceptionParseStatus::Malformed);
  EXPECT_TRUE(Decoded.Operations.empty());
}

} // namespace
