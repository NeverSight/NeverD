//===- LanguageEHTests.cpp - DWARF call frame information tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "LanguageEHTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::dwarf_eh;
using namespace neverd::language_eh_test;

//===----------------------------------------------------------------------===//
// LEB128
//===----------------------------------------------------------------------===//

TEST(DwarfEHPrimitives, ReadsCanonicalULEB128) {
  const uint8_t Bytes[] = {0xe5, 0x8e, 0x26};
  size_t Cursor = 0;
  uint64_t Value = 0;
  ASSERT_TRUE(readULEB128(Bytes, sizeof(Bytes), Cursor, Value));
  EXPECT_EQ(Value, 624485u);
  EXPECT_EQ(Cursor, 3u);
}

TEST(DwarfEHPrimitives, ReadsNegativeSLEB128) {
  const uint8_t Bytes[] = {0x9b, 0xf1, 0x59};
  size_t Cursor = 0;
  int64_t Value = 0;
  ASSERT_TRUE(readSLEB128(Bytes, sizeof(Bytes), Cursor, Value));
  EXPECT_EQ(Value, -624485);
  EXPECT_EQ(Cursor, 3u);
}

TEST(DwarfEHPrimitives, RejectsUnterminatedLEB128) {
  const uint8_t Bytes[] = {0x80, 0x80, 0x80};
  size_t Cursor = 0;
  uint64_t Value = 0;
  EXPECT_FALSE(readULEB128(Bytes, sizeof(Bytes), Cursor, Value));
  EXPECT_EQ(Cursor, 0u);
}

TEST(DwarfEHPrimitives, RejectsOverlongLEB128) {
  // Twelve continuation bytes cannot encode a 64-bit value; accepting them
  // would silently drop the high bits of a crafted length field.
  const uint8_t Bytes[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                           0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
  size_t Cursor = 0;
  uint64_t Value = 0;
  EXPECT_FALSE(readULEB128(Bytes, sizeof(Bytes), Cursor, Value));
  EXPECT_EQ(Cursor, 0u);
}

//===----------------------------------------------------------------------===//
// Encoded pointers
//===----------------------------------------------------------------------===//

TEST(DwarfEHPointer, ResolvesPCRelativeSData4) {
  ByteBuilder B;
  B.i32(-0x20);
  size_t Cursor = 0;
  va_t Out = 0;
  BinaryImage Img = makeImage();
  ASSERT_TRUE(readEncodedPointer(B.data().data(), B.size(), Cursor, 0x401000,
                                 0x1b, PointerBases{}, 8, &Img, Out));
  EXPECT_EQ(Out, 0x400fe0u);
  EXPECT_EQ(Cursor, 4u);
}

TEST(DwarfEHPointer, FollowsIndirectionAndReportsItsSlot) {
  BinaryImage Img = makeImage();
  ByteBuilder Slot;
  Slot.u64(0x401234);
  writeData(Img, kDataVA + 0x40, Slot.data());

  ByteBuilder B;
  B.u32(static_cast<uint32_t>(kDataVA + 0x40));
  size_t Cursor = 0;
  va_t Out = 0;
  va_t SlotVA = 0;
  // DW_EH_PE_indirect | DW_EH_PE_udata4
  ASSERT_TRUE(readEncodedPointer(B.data().data(), B.size(), Cursor, kDataVA,
                                 0x83, PointerBases{}, 8, &Img, Out, &SlotVA));
  EXPECT_EQ(SlotVA, kDataVA + 0x40);
  EXPECT_EQ(Out, 0x401234u);
}

TEST(DwarfEHPointer, RefusesRelativeEncodingWithoutItsBase) {
  ByteBuilder B;
  B.u32(0x10);
  size_t Cursor = 0;
  va_t Out = 0;
  BinaryImage Img = makeImage();
  // DW_EH_PE_datarel | DW_EH_PE_udata4 with no data base supplied.
  EXPECT_FALSE(readEncodedPointer(B.data().data(), B.size(), Cursor, kDataVA,
                                  0x33, PointerBases{}, 8, &Img, Out));
}

TEST(DwarfEHPointer, TreatsZeroAsAbsentRatherThanBaseRelative) {
  ByteBuilder B;
  B.u32(0);
  size_t Cursor = 0;
  va_t Out = 0xdeadbeef;
  BinaryImage Img = makeImage();
  PointerBases Bases;
  Bases.Data = kDataVA;
  ASSERT_TRUE(readEncodedPointer(B.data().data(), B.size(), Cursor, kDataVA,
                                 0x33, Bases, 8, &Img, Out));
  EXPECT_EQ(Out, 0u);
}

//===----------------------------------------------------------------------===//
// Call frame information
//===----------------------------------------------------------------------===//

TEST(DwarfEHFrame, DecodesCIEAndFDE) {
  BinaryImage Img = makeImage();
  const va_t SectionVA = kDataVA;
  FrameBytes Frame =
      buildSimpleFrame(SectionVA, kTextVA + 0x100, 0x40, "zR", 0, 0);
  writeData(Img, SectionVA, Frame.Bytes);

  FrameSection Sec{Frame.Bytes.data(), Frame.Bytes.size(), SectionVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});

  ASSERT_EQ(Result.CIEs.size(), 1u);
  ASSERT_EQ(Result.FDEs.size(), 1u);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);

  const DwarfCIE &CIE = Result.CIEs[0];
  EXPECT_EQ(CIE.Version, 1);
  EXPECT_EQ(CIE.Augmentation, "zR");
  EXPECT_EQ(CIE.CodeAlignmentFactor, 1u);
  EXPECT_EQ(CIE.DataAlignmentFactor, -8);
  EXPECT_EQ(CIE.ReturnAddressRegister, 16u);
  EXPECT_TRUE(CIE.HasAugmentationData);
  EXPECT_EQ(CIE.FDEPointerEncoding, 0x1b);

  const DwarfFDE &FDE = Result.FDEs[0];
  EXPECT_EQ(FDE.InitialLocation, kTextVA + 0x100);
  EXPECT_EQ(FDE.AddressRange, 0x40u);
  EXPECT_EQ(FDE.CIESectionOffset, Frame.CIEOffset);
}

TEST(DwarfEHFrame, ScalesInitialInstructionsByAlignmentFactors) {
  BinaryImage Img = makeImage();
  FrameBytes Frame =
      buildSimpleFrame(kDataVA, kTextVA + 0x100, 0x40, "zR", 0, 0);
  writeData(Img, kDataVA, Frame.Bytes);
  FrameSection Sec{Frame.Bytes.data(), Frame.Bytes.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});

  ASSERT_EQ(Result.CIEs.size(), 1u);
  const auto &Ops = Result.CIEs[0].InitialInstructions;
  ASSERT_GE(Ops.size(), 2u);
  EXPECT_EQ(Ops[0].Kind, CFIOpKind::DefCFA);
  EXPECT_EQ(Ops[0].Register, 7u);
  EXPECT_EQ(Ops[0].Offset, 8);
  EXPECT_EQ(Ops[1].Kind, CFIOpKind::Offset);
  EXPECT_EQ(Ops[1].Register, 16u);
  // A factored offset of 1 with a data alignment factor of -8 is -8 bytes,
  // and a consumer must not have to re-apply the factor.
  EXPECT_EQ(Ops[1].Offset, -8);
}

TEST(DwarfEHFrame, TracksLocationAcrossAdvanceInstructions) {
  BinaryImage Img = makeImage();
  FrameBytes Frame =
      buildSimpleFrame(kDataVA, kTextVA + 0x100, 0x40, "zR", 0, 0);
  writeData(Img, kDataVA, Frame.Bytes);
  FrameSection Sec{Frame.Bytes.data(), Frame.Bytes.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});

  ASSERT_EQ(Result.FDEs.size(), 1u);
  const auto &Ops = Result.FDEs[0].Instructions;
  ASSERT_GE(Ops.size(), 2u);
  EXPECT_EQ(Ops[0].Kind, CFIOpKind::AdvanceLoc);
  EXPECT_EQ(Ops[1].Kind, CFIOpKind::DefCFAOffset);
  // The rule established after a one-byte advance takes effect one byte in.
  EXPECT_EQ(Ops[1].CodeOffset, 1u);
}

TEST(DwarfEHFrame, ResolvesPersonalityAndLSDAThroughAugmentation) {
  BinaryImage Img = makeImage();
  const va_t PersonalityVA = kTextVA + 0x800;
  const va_t LSDAVA = kDataVA + 0x400;
  FrameBytes Frame = buildSimpleFrame(kDataVA, kTextVA + 0x100, 0x40, "zPLR",
                                      PersonalityVA, LSDAVA);
  writeData(Img, kDataVA, Frame.Bytes);
  FrameSection Sec{Frame.Bytes.data(), Frame.Bytes.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});

  ASSERT_EQ(Result.CIEs.size(), 1u);
  ASSERT_EQ(Result.FDEs.size(), 1u);
  EXPECT_EQ(Result.CIEs[0].PersonalityVA, PersonalityVA);
  EXPECT_EQ(Result.CIEs[0].LSDAPointerEncoding, 0x1b);
  EXPECT_EQ(Result.FDEs[0].LSDAVA, LSDAVA);
}

TEST(DwarfEHFrame, StopsAtZeroLengthTerminator) {
  BinaryImage Img = makeImage();
  FrameBytes Frame =
      buildSimpleFrame(kDataVA, kTextVA + 0x100, 0x40, "zR", 0, 0);
  // Append garbage past the terminator the builder already wrote.
  Frame.Bytes.insert(Frame.Bytes.end(), 64, 0xcc);
  writeData(Img, kDataVA, Frame.Bytes);
  FrameSection Sec{Frame.Bytes.data(), Frame.Bytes.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});
  EXPECT_EQ(Result.FDEs.size(), 1u);
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(DwarfEHFrame, RejectsEntryLengthPastTheSection) {
  BinaryImage Img = makeImage();
  ByteBuilder B;
  B.u32(0x7fffffff);
  B.u32(0);
  B.pad(8);
  writeData(Img, kDataVA, B.data());
  FrameSection Sec{B.data().data(), B.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_TRUE(Result.FDEs.empty());
}

TEST(DwarfEHFrame, RejectsFDEWhoseCIEPointerEscapesTheSection) {
  BinaryImage Img = makeImage();
  ByteBuilder B;
  const size_t LengthSlot = B.size();
  B.u32(0);
  const size_t BodyStart = B.size();
  B.u32(0xffffff); // CIE pointer far before the section
  B.u32(0);
  B.u32(0);
  B.patch32(LengthSlot, static_cast<uint32_t>(B.size() - BodyStart));
  B.u32(0);
  writeData(Img, kDataVA, B.data());
  FrameSection Sec{B.data().data(), B.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_TRUE(Result.FDEs.empty());
}

TEST(DwarfEHFrame, ReportsUnsupportedCIEVersion) {
  BinaryImage Img = makeImage();
  ByteBuilder B;
  const size_t LengthSlot = B.size();
  B.u32(0);
  const size_t BodyStart = B.size();
  B.u32(0);
  B.u8(9); // no such CIE version
  B.str("");
  B.uleb(1);
  B.sleb(-8);
  B.u8(16);
  B.patch32(LengthSlot, static_cast<uint32_t>(B.size() - BodyStart));
  B.u32(0);
  writeData(Img, kDataVA, B.data());
  FrameSection Sec{B.data().data(), B.size(), kDataVA};
  ParseResult Result = parseEHFrame(Img, Sec, PointerBases{});
  EXPECT_EQ(Result.ParseStatus, ExceptionParseStatus::Malformed);
  EXPECT_TRUE(Result.CIEs.empty());
}

} // namespace
