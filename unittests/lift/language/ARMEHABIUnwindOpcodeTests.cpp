//===- ARMEHABIUnwindOpcodeTests.cpp - ARM EHABI unwind opcode tests --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "ARMEHABITestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::arm_ehabi;
using namespace neverd::arm_ehabi_test;

//===----------------------------------------------------------------------===//
// Unwind opcodes
//===----------------------------------------------------------------------===//

TEST(ARMEHABI, DecodesTheFrameOpcodesAGenericEntryCarries) {
  BinaryImage Img = makeARMImage();
  // 04 9b 84 80 a8 b0 b0
  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x01049B84u,
                          {0x80A8B0B0u}, {}));
  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_EQ(F.UnwindOperations.size(), 5u);

  // Every operation is stated the way the prologue performed it, though the
  // opcodes describe undoing it.
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::AllocateStack);
  EXPECT_EQ(F.UnwindOperations[0].StackOffset, 20u);

  EXPECT_EQ(F.UnwindOperations[1].Kind,
            UnwindOperationKind::SetStackPointerFromRegister);
  EXPECT_EQ(F.UnwindOperations[1].Register, 11u);

  // `1000iiii iiiiiiii` names r4 through r15 under a mask, and the two bits
  // set here are the frame pointer and the link register.
  EXPECT_EQ(F.UnwindOperations[2].Kind,
            UnwindOperationKind::SaveRegisterPairPreIndexed);
  EXPECT_EQ(F.UnwindOperations[2].RegisterClass,
            UnwindRegisterClass::GeneralPurpose);
  EXPECT_EQ(F.UnwindOperations[2].RegisterMask, (1u << 11) | (1u << 14));
  EXPECT_EQ(F.UnwindOperations[2].Register, 11u);
  EXPECT_EQ(F.UnwindOperations[2].StackOffset, 8u);

  // `10101nnn` names r4 upward plus the link register.
  EXPECT_EQ(F.UnwindOperations[3].RegisterMask, (1u << 4) | (1u << 14));
  EXPECT_EQ(F.UnwindOperations[3].Register, 4u);

  EXPECT_EQ(F.UnwindOperations[4].Kind, UnwindOperationKind::End);
  EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(F.NativeUnwindBytes.size(), 7u);
}

TEST(ARMEHABI, DecodesTheWideAllocationAndFloatingPointOpcodes) {
  BinaryImage Img = makeARMImage();
  // 42 b2 01 b3 21 b0
  ByteBuilder Entry;
  Entry.u32(0x810142B2u);
  Entry.u32(0x01B321B0u);
  write(Img, kExTabVA, Entry.data());

  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_EQ(F.UnwindOperations.size(), 4u);

  // `01xxxxxx` moves the stack pointer the other way: the frame the prologue
  // left is smaller than the one it was entered with.
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::DeallocateStack);
  EXPECT_EQ(F.UnwindOperations[0].StackOffset, 12u);

  // `10110010` carries a ULEB128 so that a frame larger than the six-bit form
  // can reach can still be described.
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::AllocateStack);
  EXPECT_EQ(F.UnwindOperations[1].StackOffset, 0x208u);

  // `10110011 sssscccc` pops double-precision registers with the `FSTMFDX`
  // layout, which leaves a spare word above them.
  EXPECT_EQ(F.UnwindOperations[2].RegisterClass,
            UnwindRegisterClass::FloatingPoint);
  EXPECT_EQ(F.UnwindOperations[2].Register, 2u);
  EXPECT_EQ(F.UnwindOperations[2].RegisterMask, (1u << 2) | (1u << 3));
  EXPECT_EQ(F.UnwindOperations[2].StackOffset, 2u * 8u + 4u);

  EXPECT_EQ(F.UnwindOperations[3].Kind, UnwindOperationKind::End);
}

TEST(ARMEHABI, KeepsAnOpcodeItCannotModelRatherThanDroppingIt) {
  BinaryImage Img = makeARMImage();
  // c6 21 is an Intel Wireless MMX pop, which this decoder does not model.
  write(Img, kExTabVA,
        buildGenericEntry(kExTabVA, kPersonalityVA, 0x00C621B0u, {}, {}));
  IndexBuilder Index;
  Index.tableRef(kTextVA, kExTabVA);
  Index.install(Img);

  parseARMEHABIExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_EQ(F.UnwindOperations.size(), 2u);
  EXPECT_EQ(F.UnwindOperations[0].Kind, UnwindOperationKind::Opaque);
  EXPECT_EQ(F.UnwindOperations[0].OperandBytes,
            (std::vector<uint8_t>{0xC6, 0x21}));
  EXPECT_EQ(F.UnwindOperations[1].Kind, UnwindOperationKind::End);
}

} // namespace
