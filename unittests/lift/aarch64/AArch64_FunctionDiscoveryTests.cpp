//===- AArch64_FunctionDiscoveryTests.cpp - AArch64 entry discovery ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/BinaryFormat/MachO.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

Section makeMachOSection(std::string Name, va_t VA, uint64_t Size,
                         bool ContainsInstructions) {
  Section Sec;
  Sec.Name = std::move(Name);
  Sec.SegmentName = "__TEXT";
  Sec.VA = VA;
  Sec.Size = Size;
  // Linked Mach-O sections inherit their segment's VM protections.  The
  // instruction attributes, not the RX permission, distinguish __text from
  // __const inside __TEXT.
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Sec.Type = llvm::MachO::S_REGULAR;
  if (ContainsInstructions)
    Sec.Type |= llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  return Sec;
}

TEST(AArch64FunctionDiscovery, StrictPrologueAcceptsPairPushesBelowSp) {
  // A push stores below sp, so the pre-indexed offset is negative:
  // stp x29, x30, [sp, #-16]! and stp x19, x20, [sp, #-32]!.
  EXPECT_TRUE(isStrictPrologueWordAArch64(0xa9bf7bfd));
  EXPECT_TRUE(isStrictPrologueWordAArch64(0xa9be53f3));
  // stp x0, x1, [x2, #16]! stores through another base register: no push.
  EXPECT_FALSE(isStrictPrologueWordAArch64(0xa9810440));
}

TEST(AArch64FunctionDiscovery, FindsAPackedEntryMarkerAfterABlockEnd) {
  // Functions packed with no padding: a leaf `ret`, then one no call, symbol,
  // unwind row or relocation names, then a shrink-wrapped one.  The marker
  // after the ret begins the second; neither the marker of an in-function
  // landing pad, which its predecessor falls through to, nor the prologue the
  // third function's early-return test branches to past its ret begins one.
  constexpr va_t ImageVA = 0x1000;
  constexpr va_t PackedVA = ImageVA + 0x4;      // after the leaf's ret
  constexpr va_t LandingVA = ImageVA + 0x10;    // a bti c inside PackedVA
  constexpr va_t ShrinkWrapVA = ImageVA + 0x24; // the third's slow path
  constexpr uint64_t TextSize = 0x2C;
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;
  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = ImageVA;
  Text.Size = TextSize;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(TextSize, 0);
  writeLE<uint32_t>(Text.Data.data(), 0xD65F03C0u);        // ret (leaf)
  writeLE<uint32_t>(Text.Data.data() + 0x4, 0xD503233Fu);  // paciasp (PackedVA)
  writeLE<uint32_t>(Text.Data.data() + 0x8, 0xD503201Fu);  // nop
  writeLE<uint32_t>(Text.Data.data() + 0xC, 0xB4000020u);  // cbz x0, LandingVA
  writeLE<uint32_t>(Text.Data.data() + 0x10, 0xD503245Fu); // bti c (LandingVA)
  writeLE<uint32_t>(Text.Data.data() + 0x14, 0xD65F03C0u); // ret
  writeLE<uint32_t>(Text.Data.data() + 0x18, 0xB5000060u); // cbnz x0, slow path
  writeLE<uint32_t>(Text.Data.data() + 0x1C, 0xD503201Fu); // nop
  writeLE<uint32_t>(Text.Data.data() + 0x20, 0xD65F03C0u); // ret (early)
  writeLE<uint32_t>(Text.Data.data() + 0x24, 0xD503233Fu); // paciasp: slow path
  writeLE<uint32_t>(Text.Data.data() + 0x28, 0xD65F03C0u); // ret
  Img.Segments.push_back(std::move(Text));
  Img.Sections.push_back(makeMachOSection("__text", ImageVA, TextSize,
                                          /*ContainsInstructions=*/true));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  const auto Has = [&](va_t A) {
    return std::any_of(Functions.begin(), Functions.end(),
                       [&](const auto &F) { return F.first == A; });
  };
  EXPECT_TRUE(Has(PackedVA)) << "the packed entry after the ret";
  EXPECT_FALSE(Has(LandingVA)) << "the in-function cbz landing pad";
  EXPECT_FALSE(Has(ShrinkWrapVA)) << "the shrink-wrapped prologue";
}

TEST(AArch64FunctionDiscovery,
     IgnoresDirectCallBitPatternsInMachONonInstructionSections) {
  constexpr va_t ImageVA = 0x1000;
  constexpr va_t RealCodeVA = ImageVA + 0x8;
  constexpr va_t DataVA = ImageVA + 0x10;
  constexpr va_t HiddenCodeVA = ImageVA + 0x20;

  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = ImageVA;
  Text.Size = 0x24;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  writeLE<uint32_t>(Text.Data.data(), 0x94000002u);     // bl RealCodeVA
  writeLE<uint32_t>(Text.Data.data() + 4, 0xD65F03C0u); // ret
  writeLE<uint32_t>(Text.Data.data() + (RealCodeVA - ImageVA),
                    0xD65F03C0u); // ret
  // Data bytes in __TEXT,__const that decode as `bl HiddenCodeVA`.
  writeLE<uint32_t>(Text.Data.data() + (DataVA - ImageVA), 0x94000004u);
  writeLE<uint32_t>(Text.Data.data() + (HiddenCodeVA - ImageVA),
                    0xD65F03C0u); // ret
  Img.Segments.push_back(std::move(Text));

  Img.Sections.push_back(
      makeMachOSection("__text", ImageVA, 12, /*ContainsInstructions=*/true));
  Img.Sections.push_back(
      makeMachOSection("__const", DataVA, 4, /*ContainsInstructions=*/false));
  Img.Sections.push_back(makeMachOSection("__hidden", HiddenCodeVA, 4,
                                          /*ContainsInstructions=*/true));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);

  ASSERT_EQ(Functions.size(), 2u);
  EXPECT_EQ(Functions.front().first, ImageVA);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == RealCodeVA; }),
            1u);
  EXPECT_EQ(
      std::count_if(Functions.begin(), Functions.end(),
                    [](const auto &F) { return F.first == HiddenCodeVA; }),
      0u);
}

TEST(AArch64FunctionDiscovery,
     VerifiesZeroSizedTypedFunctionStartInsideKnownRange) {
  constexpr va_t ImageVA = 0x5000;
  constexpr va_t TypedStartVA = ImageVA + 0x8;

  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = ImageVA;
  Text.Size = 0x10;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  writeLE<uint32_t>(Text.Data.data(), 0xD65F03C0u);
  writeLE<uint32_t>(Text.Data.data() + (TypedStartVA - ImageVA), 0xD65F03C0u);
  Img.Segments.push_back(std::move(Text));
  Img.Sections.push_back(
      makeMachOSection("__text", ImageVA, 0x10, /*ContainsInstructions=*/true));
  Img.KnownCodeRanges.emplace_back(ImageVA, ImageVA + 0x10);
  Img.Symbols.push_back(Symbol::makeFunc(TypedStartVA));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(
      std::count_if(Functions.begin(), Functions.end(),
                    [](const auto &F) { return F.first == TypedStartVA; }),
      1u);
}

TEST(AArch64FunctionDiscovery, RejectsZeroSizedTypedFunctionStartWithBadCode) {
  constexpr va_t ImageVA = 0x6000;
  constexpr va_t TypedStartVA = ImageVA + 0x8;

  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = ImageVA;
  Text.Size = 0x10;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  writeLE<uint32_t>(Text.Data.data(), 0xD65F03C0u);
  // Zero is the valid UDF #0 instruction, so use an unallocated encoding.
  writeLE<uint32_t>(Text.Data.data() + (TypedStartVA - ImageVA), 0xFFFFFFFFu);
  Img.Segments.push_back(std::move(Text));
  Img.Sections.push_back(
      makeMachOSection("__text", ImageVA, 0x10, /*ContainsInstructions=*/true));
  Img.KnownCodeRanges.emplace_back(ImageVA, ImageVA + 0x10);
  Img.Symbols.push_back(Symbol::makeFunc(TypedStartVA));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(
      std::count_if(Functions.begin(), Functions.end(),
                    [](const auto &F) { return F.first == TypedStartVA; }),
      0u);
}

TEST(AArch64FunctionDiscovery, RejectsMisalignedCandidatesBeforeTrialLifting) {
  constexpr va_t ImageVA = 0x7000;
  constexpr va_t MisalignedVA = ImageVA + 0xb;
  constexpr va_t LeafVA = ImageVA + 0x20;
  constexpr va_t SlotsVA = 0x8000;

  for (bool FromRelocation : {false, true}) {
    SCOPED_TRACE(FromRelocation ? "relocation" : "zero-sized symbol");
    BinaryImage Img;
    Img.Arch = Arch::AArch64;
    Img.Bits = Bitness::Bits64;
    Img.Format = BinaryFormat::MachO;
    Img.Base = ImageVA;
    Img.Entry = ImageVA;

    Segment Text;
    Text.Name = "__TEXT";
    Text.VA = ImageVA;
    Text.Size = 0x28;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0);
    writeLE<uint32_t>(Text.Data.data(), 0xD65F03C0u); // ret
    // These four bytes were observed at an unaligned candidate in a real
    // image. Trial lifting them reached an invalid integer-width extension.
    // A following ret makes the preliminary linear probe accept the bytes.
    writeLE<uint32_t>(Text.Data.data() + (MisalignedVA - ImageVA), 0x04606DA9u);
    writeLE<uint32_t>(Text.Data.data() + (MisalignedVA - ImageVA) + 4,
                      0xD65F03C0u);
    writeLE<uint32_t>(Text.Data.data() + (LeafVA - ImageVA), 0x52800540u);
    writeLE<uint32_t>(Text.Data.data() + (LeafVA - ImageVA) + 4, 0xD65F03C0u);
    Img.Segments.push_back(std::move(Text));
    Img.Sections.push_back(makeMachOSection("__text", ImageVA, 0x28,
                                            /*ContainsInstructions=*/true));

    if (FromRelocation) {
      Segment Data;
      Data.Name = "__DATA";
      Data.VA = SlotsVA;
      Data.Size = 16;
      Data.Flags = SegmentFlags::Readable;
      Data.Data.assign(Data.Size, 0);
      writeLE<uint64_t>(Data.Data.data(), MisalignedVA);
      writeLE<uint64_t>(Data.Data.data() + 8, LeafVA);
      Img.Segments.push_back(std::move(Data));
      Img.CodePtrRelocSlots.insert(SlotsVA);
      Img.CodePtrRelocSlots.insert(SlotsVA + 8);
    } else {
      Img.Symbols.push_back(Symbol::makeFunc(MisalignedVA));
      Img.Symbols.push_back(Symbol::makeFunc(LeafVA));
    }

    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    FuncDetector Detector;
    const auto Functions = Detector.detect(Img, Dec);

    ASSERT_EQ(Functions.size(), 2u);
    EXPECT_EQ(Functions[0].first, ImageVA);
    EXPECT_EQ(Functions[1].first, LeafVA);
    EXPECT_EQ(Img.VerifiedFunctionEntries.count(MisalignedVA), 0u);
    EXPECT_EQ(Img.VerifiedFunctionEntries.count(LeafVA), 1u);
  }
}

TEST(AArch64FunctionDiscovery,
     RejectsMisalignedTrustedMetadataWithAndWithoutAnEntryPoint) {
  constexpr va_t ImageVA = 0x9000;
  constexpr va_t EntryVA = ImageVA + 0x3;
  constexpr va_t SizedVA = ImageVA + 0xb;
  constexpr va_t ExportVA = ImageVA + 0x13;
  constexpr va_t LeafVA = ImageVA + 0x20;

  for (bool HasEntry : {false, true}) {
    SCOPED_TRACE(HasEntry ? "linked entry" : "relocatable without entry");
    BinaryImage Img;
    Img.Arch = Arch::AArch64;
    Img.Bits = Bitness::Bits64;
    Img.Format = BinaryFormat::MachO;
    Img.Base = ImageVA;
    Img.Entry = HasEntry ? EntryVA : 0;
    Img.IsRelocatable = !HasEntry;

    Segment Text;
    Text.Name = "__TEXT";
    Text.VA = ImageVA;
    Text.Size = 0x24;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Text.Size, 0);
    for (va_t Addr : {EntryVA, SizedVA, ExportVA, LeafVA})
      writeLE<uint32_t>(Text.Data.data() + (Addr - ImageVA), 0xD65F03C0u);
    Img.Segments.push_back(std::move(Text));
    Img.Sections.push_back(makeMachOSection("__text", ImageVA, 0x24,
                                            /*ContainsInstructions=*/true));
    Img.Symbols.push_back(Symbol::makeFunc(EntryVA, 4));
    Img.Symbols.push_back(Symbol::makeFunc(SizedVA, 4));
    Img.Symbols.push_back(Symbol::makeFunc(LeafVA, 4));
    Img.Exports.push_back({"_misaligned", 0, ExportVA});

    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    FuncDetector Detector;
    const auto Functions = Detector.detect(Img, Dec);

    ASSERT_EQ(Functions.size(), 1u);
    EXPECT_EQ(Functions.front().first, LeafVA);
    for (va_t Addr : {EntryVA, SizedVA, ExportVA})
      EXPECT_EQ(Img.VerifiedFunctionEntries.count(Addr), 0u);
  }
}

TEST(AArch64FunctionDiscovery,
     X64CallScanRejectsDataAndCodeToDataBoundaryPatterns) {
  constexpr va_t ImageVA = 0x3000;
  constexpr va_t RealCodeVA = ImageVA + 0x10;
  constexpr va_t SplitCallVA = ImageVA + 0x20;
  constexpr va_t DataCallVA = ImageVA + 0x30;
  constexpr va_t SplitTargetVA = ImageVA + 0x40;
  constexpr va_t DataTargetVA = ImageVA + 0x48;

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = ImageVA;
  Text.Size = 0x49;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0x90);
  auto WriteCall = [&](va_t From, va_t To) {
    const size_t Off = static_cast<size_t>(From - ImageVA);
    Text.Data[Off] = 0xE8;
    writeLE<int32_t>(Text.Data.data() + Off + 1,
                     static_cast<int32_t>(To - (From + 5)));
  };
  WriteCall(ImageVA, RealCodeVA);
  Text.Data[5] = 0xC3;
  Text.Data[RealCodeVA - ImageVA] = 0xC3;
  // The opcode is the final byte of __text; its displacement belongs to
  // __const. A segment-wide decoder must not join the two owners.
  WriteCall(SplitCallVA, SplitTargetVA);
  // A complete CALL-shaped byte sequence entirely inside __const.
  WriteCall(DataCallVA, DataTargetVA);
  Text.Data[SplitTargetVA - ImageVA] = 0xC3;
  Text.Data[DataTargetVA - ImageVA] = 0xC3;
  Img.Segments.push_back(std::move(Text));

  Img.Sections.push_back(makeMachOSection("__text", ImageVA, 0x21,
                                          /*ContainsInstructions=*/true));
  Img.Sections.push_back(makeMachOSection("__const_tail", ImageVA + 0x21, 4,
                                          /*ContainsInstructions=*/false));
  Img.Sections.push_back(makeMachOSection("__const", DataCallVA, 5,
                                          /*ContainsInstructions=*/false));
  Img.Sections.push_back(makeMachOSection("__hidden", SplitTargetVA, 9,
                                          /*ContainsInstructions=*/true));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == RealCodeVA; }),
            1u);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) {
                            return F.first == SplitTargetVA ||
                                   F.first == DataTargetVA;
                          }),
            0u);
}

TEST(AArch64FunctionDiscovery,
     ARMCallScanRejectsMachONonInstructionSectionPatterns) {
  constexpr va_t ImageVA = 0x4000;
  constexpr va_t RealCodeVA = ImageVA + 0x10;
  constexpr va_t DataCallVA = ImageVA + 0x20;
  constexpr va_t HiddenCodeVA = ImageVA + 0x30;

  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Bits = Bitness::Bits32;
  Img.Mode = InstructionMode::ARM;
  Img.Format = BinaryFormat::MachO;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = ImageVA;
  Text.Size = 0x34;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  // ARM BL uses PC+8 as its base: both calls reach 0x10 bytes ahead.
  writeLE<uint32_t>(Text.Data.data(), 0xEB000002u);
  writeLE<uint32_t>(Text.Data.data() + 4, 0xE12FFF1Eu); // bx lr
  writeLE<uint32_t>(Text.Data.data() + (RealCodeVA - ImageVA), 0xE12FFF1Eu);
  writeLE<uint32_t>(Text.Data.data() + (DataCallVA - ImageVA), 0xEB000002u);
  writeLE<uint32_t>(Text.Data.data() + (HiddenCodeVA - ImageVA), 0xE12FFF1Eu);
  Img.Segments.push_back(std::move(Text));

  Img.Sections.push_back(makeMachOSection("__text", ImageVA, 0x14,
                                          /*ContainsInstructions=*/true));
  Img.Sections.push_back(makeMachOSection("__const", DataCallVA, 4,
                                          /*ContainsInstructions=*/false));
  Img.Sections.push_back(makeMachOSection("__hidden", HiddenCodeVA, 4,
                                          /*ContainsInstructions=*/true));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::ARM));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == RealCodeVA; }),
            1u);
  EXPECT_EQ(
      std::count_if(Functions.begin(), Functions.end(),
                    [](const auto &F) { return F.first == HiddenCodeVA; }),
      0u);
}

TEST(AArch64FunctionDiscovery,
     DistinguishesMachOConstExportsFromInstructionSectionExports) {
  constexpr va_t ImageVA = 0x1800;
  constexpr va_t TextExportVA = ImageVA + 0x4;
  constexpr va_t ConstExportVA = ImageVA + 0x8;

  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = ImageVA;
  Text.Size = 0xc;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  writeLE<uint32_t>(Text.Data.data(), 0xD65F03C0u); // ret
  writeLE<uint32_t>(Text.Data.data() + (TextExportVA - ImageVA),
                    0xD65F03C0u); // ret
  // Deliberately valid instruction bytes: section identity, rather than
  // speculative decoding, must keep this exported constant out of the
  // discovered function set.
  writeLE<uint32_t>(Text.Data.data() + (ConstExportVA - ImageVA),
                    0xD65F03C0u); // ret-shaped data
  Img.Segments.push_back(std::move(Text));

  Img.Sections.push_back(makeMachOSection("__text", ImageVA, 0x8,
                                          /*ContainsInstructions=*/true));
  Img.Sections.push_back(makeMachOSection("__const", ConstExportVA, 0x4,
                                          /*ContainsInstructions=*/false));

  Img.Exports.push_back({"_text_export", 0, TextExportVA});
  Img.Exports.push_back({"_const_export", 0, ConstExportVA});

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(
      std::count_if(Functions.begin(), Functions.end(),
                    [](const auto &F) { return F.first == TextExportVA; }),
      1u);
  EXPECT_EQ(
      std::count_if(Functions.begin(), Functions.end(),
                    [](const auto &F) { return F.first == ConstExportVA; }),
      0u);
}

TEST(AArch64FunctionDiscovery,
     VerifiesZeroSizedTypedSymbolsInsideBroadCompactUnwindRange) {
  constexpr va_t ImageVA = 0x2000;
  constexpr va_t LeafVA = ImageVA + 0x8;
  constexpr va_t InvalidVA = ImageVA + 0x10;

  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = ImageVA;
  Text.Size = 0x14;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  writeLE<uint32_t>(Text.Data.data(), 0xD65F03C0u); // ret
  writeLE<uint32_t>(Text.Data.data() + (LeafVA - ImageVA),
                    0xD65F03C0u); // ret
  writeLE<uint32_t>(Text.Data.data() + (InvalidVA - ImageVA),
                    0xFFFFFFFFu); // undecodable
  Img.Segments.push_back(std::move(Text));
  Img.Sections.push_back(
      makeMachOSection("__text", ImageVA, 0x14, /*ContainsInstructions=*/true));

  Symbol Covering = Symbol::makeFunc(ImageVA, 0x14);
  Covering.Name = "_covering";
  Img.Symbols.push_back(std::move(Covering));
  Symbol Leaf = Symbol::makeFunc(LeafVA);
  Leaf.Name = "_leaf";
  Img.Symbols.push_back(std::move(Leaf));
  Symbol Invalid = Symbol::makeFunc(InvalidVA);
  Invalid.Name = "_invalid";
  Img.Symbols.push_back(std::move(Invalid));
  Img.Exports.push_back({"_covering", 0, ImageVA});
  Img.KnownCodeRanges.emplace_back(ImageVA, ImageVA + 0x14);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == ImageVA; }),
            1u);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == LeafVA; }),
            1u);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == InvalidVA; }),
            0u);
  EXPECT_EQ(Img.VerifiedFunctionEntries.count(LeafVA), 1u);
  EXPECT_EQ(Img.VerifiedFunctionEntries.count(InvalidVA), 0u);
}

TEST(AArch64FunctionDiscovery,
     PreservesVerifiedDirectCallInsideBroadCompactUnwindRange) {
  constexpr va_t ImageVA = 0x3000;
  constexpr va_t HelperVA = ImageVA + 0x10;
  constexpr va_t InvalidVA = ImageVA + 0x18;

  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = ImageVA;
  Text.Size = 0x1c;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  writeLE<uint32_t>(Text.Data.data(), 0x94000004u);      // bl HelperVA
  writeLE<uint32_t>(Text.Data.data() + 4, 0x94000005u);  // bl InvalidVA
  writeLE<uint32_t>(Text.Data.data() + 8, 0xD65F03C0u);  // ret
  writeLE<uint32_t>(Text.Data.data() + 12, 0xD503201Fu); // nop
  writeLE<uint32_t>(Text.Data.data() + 16, 0x52800540u); // mov w0, #42
  writeLE<uint32_t>(Text.Data.data() + 20, 0xD65F03C0u); // ret
  writeLE<uint32_t>(Text.Data.data() + 24, 0xFFFFFFFFu); // undecodable
  Img.Segments.push_back(std::move(Text));
  Img.Sections.push_back(
      makeMachOSection("__text", ImageVA, 0x1c, /*ContainsInstructions=*/true));

  Symbol Covering = Symbol::makeFunc(ImageVA, 0x1c);
  Covering.Name = "_main";
  Img.Symbols.push_back(std::move(Covering));
  Img.Exports.push_back({"_main", 0, ImageVA});
  Img.KnownCodeRanges.emplace_back(ImageVA, ImageVA + 0x1c);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == ImageVA; }),
            1u);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == HelperVA; }),
            1u);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == InvalidVA; }),
            0u);
}

TEST(AArch64FunctionDiscovery,
     PreservesCompactRangeCalleeEndingInNoReturnImport) {
  constexpr va_t ImageVA = 0x3000;
  constexpr va_t HelperVA = ImageVA + 0x8;
  constexpr va_t AbortStubVA = ImageVA + 0x150;

  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Base = ImageVA;
  Img.Entry = ImageVA;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = ImageVA;
  Text.Size = 0x154;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  writeLE<uint32_t>(Text.Data.data(), 0x94000002u);      // bl HelperVA
  writeLE<uint32_t>(Text.Data.data() + 4, 0xD65F03C0u);  // ret
  writeLE<uint32_t>(Text.Data.data() + 8, 0x52800000u);  // mov w0, #0
  writeLE<uint32_t>(Text.Data.data() + 12, 0x94000051u); // bl AbortStubVA
  for (size_t Off = 0x10; Off < 0x150; Off += sizeof(uint32_t))
    writeLE<uint32_t>(Text.Data.data() + Off, 0xD503201Fu); // nop
  writeLE<uint32_t>(Text.Data.data() + 0x150, 0xD61F0200u); // br x16
  Img.Segments.push_back(std::move(Text));
  Img.Sections.push_back(makeMachOSection("__text", ImageVA, 0x150,
                                          /*ContainsInstructions=*/true));

  Symbol Covering = Symbol::makeFunc(ImageVA, 0x150);
  Covering.Name = "_main";
  Img.Symbols.push_back(std::move(Covering));
  Img.Exports.push_back({"_main", 0, ImageVA});
  Img.KnownCodeRanges.emplace_back(ImageVA, AbortStubVA);
  Img.Imports.push_back({"libSystem.B.dylib", "_abort", 0, 0});
  ASSERT_TRUE(Img.recordImportStub(AbortStubVA, 0));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  FuncDetector Detector;
  auto Functions = Detector.detect(Img, Dec);

  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == HelperVA; }),
            1u);
}

TEST(AArch64FunctionDiscovery, SectionlessPLTHeaderIsLoaderMachinery) {
  for (bool Bti : {false, true})
    for (bool HasBinding : {false, true}) {
      SCOPED_TRACE(Bti);
      SCOPED_TRACE(HasBinding);
      constexpr va_t Entry = 0x10CC0;
      constexpr va_t Slot = 0x30EB0;
      BinaryImage Img;
      Img.Arch = Arch::AArch64;
      Img.Bits = Bitness::Bits64;
      Img.Format = BinaryFormat::ELF;
      // Padding gives the ordinary discovery scan its false prologue seed.
      std::vector<uint32_t> Words = {0, 0, 0, 0};
      if (Bti)
        Words.push_back(0xD503245Fu);
      Words.insert(Words.end(), {0xa9bf7bf0u, 0x90000110u, 0xF9475A11u,
                                 0x913AC210u, 0xD61F0220u});
      Segment Text;
      Text.VA = Entry - 16;
      Text.Size = Words.size() * 4;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data.resize(Text.Size);
      for (size_t I = 0; I < Words.size(); ++I)
        writeLE<uint32_t>(Text.Data.data() + I * 4, Words[I]);
      Img.Segments.push_back(std::move(Text));
      Segment Data;
      Data.VA = Slot;
      Data.Size = 8;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(8);
      Img.Segments.push_back(std::move(Data));
      if (HasBinding)
        ASSERT_TRUE(Img.recordRuntimeCallablePointerSlot(
            Slot, RuntimeCallablePointerSlotKind::ELFLazyResolver));
      scanImportThunks(Img);
      EXPECT_EQ(Img.isImportStubAt(Entry), HasBinding);
      EXPECT_EQ(Img.findImportAt(Entry), nullptr);
      scanPaddingBoundaries(Img);
      if (HasBinding) {
        EXPECT_EQ(Img.findSymbolAt(Entry), nullptr);
        EXPECT_EQ(Img.findSymbolAt(Entry + (Bti ? 4 : 0)), nullptr);
      }
    }
}

TEST(AArch64FunctionDiscovery, RecognizesCanonicalELFPLTVeneer) {
  constexpr va_t StubVA = 0x10CC0;
  constexpr va_t IATAddr = 0x30EB0;

  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;

  Segment Text;
  Text.Name = ".plt";
  Text.VA = StubVA;
  Text.Size = 16;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  writeLE<uint32_t>(Text.Data.data(), 0x90000110u); // adrp x16, 0x30000
  writeLE<uint32_t>(Text.Data.data() + 4,
                    0xF9475A11u); // ldr x17, [x16, #0xeb0]
  writeLE<uint32_t>(Text.Data.data() + 8, 0x913AC210u);  // add x16, x16, #0xeb0
  writeLE<uint32_t>(Text.Data.data() + 12, 0xD61F0220u); // br x17
  Img.Segments.push_back(std::move(Text));
  Img.Imports.push_back({"extern", "getenv", 0, IATAddr});

  scanImportThunks(Img);

  const Import *Resolved = Img.findImportAt(StubVA);
  ASSERT_NE(Resolved, nullptr);
  EXPECT_EQ(Resolved->Name, "getenv");
  const Symbol *Stub = Img.findSymbolAt(StubVA);
  ASSERT_NE(Stub, nullptr);
  EXPECT_TRUE(Stub->IsFunc);
  EXPECT_EQ(Stub->Size, 16u);
}

// A BTI build (`-z force-bti`) starts each PLT entry with the landing pad its
// callers branch to, and a PAC one (`-z pac-plt`) authenticates x17 before
// the branch.  The veneer, and the import a call to it reaches, start at the
// landing pad.
TEST(AArch64FunctionDiscovery, RecognizesBTIAndPACELFPLTVeneers) {
  constexpr va_t StubVA = 0x10CC0;
  constexpr va_t IATAddr = 0x30EB0;
  for (const bool Authenticates : {false, true}) {
    SCOPED_TRACE(Authenticates ? "bti c ... autia1716" : "bti c");
    std::vector<uint32_t> Words = {
        0xD503245Fu, // bti c
        0x90000110u, // adrp x16, 0x30000
        0xF9475A11u, // ldr x17, [x16, #0xeb0]
        0x913AC210u, // add x16, x16, #0xeb0
    };
    if (Authenticates)
      Words.push_back(0xD503219Fu); // autia1716
    Words.push_back(0xD61F0220u);   // br x17

    BinaryImage Img;
    Img.Arch = Arch::AArch64;
    Img.Bits = Bitness::Bits64;
    Img.Format = BinaryFormat::ELF;
    Segment Text;
    Text.Name = ".plt";
    Text.VA = StubVA;
    Text.Size = Words.size() * sizeof(uint32_t);
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    for (size_t I = 0; I < Words.size(); ++I)
      writeLE<uint32_t>(Text.Data.data() + I * sizeof(uint32_t), Words[I]);
    const uint64_t VeneerSize = Text.Size;
    Img.Segments.push_back(std::move(Text));
    Img.Imports.push_back({"extern", "getenv", 0, IATAddr});

    scanImportThunks(Img);

    const Import *Resolved = Img.findImportAt(StubVA);
    ASSERT_NE(Resolved, nullptr);
    EXPECT_EQ(Resolved->Name, "getenv");
    EXPECT_EQ(Img.findImportAt(StubVA + sizeof(uint32_t)), nullptr);
    const Symbol *Stub = Img.findSymbolAt(StubVA);
    ASSERT_NE(Stub, nullptr);
    EXPECT_TRUE(Stub->IsFunc);
    EXPECT_EQ(Stub->Size, VeneerSize);
    EXPECT_EQ(Img.findSymbolAt(StubVA + sizeof(uint32_t)), nullptr);
  }
}

TEST(AArch64FunctionDiscovery,
     IgnoresImportThunkPatternCrossingIntoMachONonInstructionSection) {
  constexpr va_t StubVA = 0x10CC0;
  constexpr va_t IATAddr = 0x30EB0;

  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = StubVA;
  Text.Size = 16;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  writeLE<uint32_t>(Text.Data.data(), 0x90000110u); // adrp x16, 0x30000
  writeLE<uint32_t>(Text.Data.data() + 4,
                    0xF9475A11u); // ldr x17, [x16, #0xeb0]
  writeLE<uint32_t>(Text.Data.data() + 8,
                    0x913AC210u);                        // add x16, x16, #0xeb0
  writeLE<uint32_t>(Text.Data.data() + 12, 0xD61F0220u); // br x17
  Img.Segments.push_back(std::move(Text));
  Img.Sections.push_back(
      makeMachOSection("__text", StubVA, 4, /*ContainsInstructions=*/true));
  Img.Sections.push_back(makeMachOSection("__const", StubVA + 4, 12,
                                          /*ContainsInstructions=*/false));
  Img.Imports.push_back({"extern", "getenv", 0, IATAddr});

  ASSERT_TRUE(Img.isCodeAddress(StubVA));
  ASSERT_FALSE(Img.isCodeRange(StubVA, 16));
  scanImportThunks(Img);

  EXPECT_EQ(Img.findSymbolAt(StubVA), nullptr);
  EXPECT_FALSE(Img.isImportStubAt(StubVA));
}

} // namespace

// bionic's AArch64 assembly opens each frame after the `bti c` that starts the
// function.  The loader records where the function begins; the detector
// reports that, and not the frame's first address as a function of its own.
TEST(AArch64FunctionDiscovery, StartsAFrameAfterALandingPadAtThePad) {
  constexpr va_t StubVA = 0x7000;
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Base = StubVA;

  Segment Text;
  Text.Name = "LOAD";
  Text.VA = StubVA;
  Text.Size = 0x10;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0);
  writeLE<uint32_t>(Text.Data.data() + 0, 0xD503245Fu);  // bti c
  writeLE<uint32_t>(Text.Data.data() + 4, 0xD2800408u);  // mov x8, #32
  writeLE<uint32_t>(Text.Data.data() + 8, 0xD4000001u);  // svc #0
  writeLE<uint32_t>(Text.Data.data() + 12, 0xD65F03C0u); // ret
  Img.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.VA = StubVA;
  Code.Size = 0x10;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  Img.Symbols.push_back(Symbol::makeFunc(StubVA));
  ExceptionFunction Frame;
  Frame.CodeRange = {StubVA + 4, StubVA + 0x10};
  Frame.FunctionEntry = StubVA;
  Img.ExceptionMetadata.Functions.push_back(std::move(Frame));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);

  const auto At = [&](va_t VA) {
    return std::count_if(Functions.begin(), Functions.end(),
                         [&](const auto &F) { return F.first == VA; });
  };
  EXPECT_EQ(At(StubVA), 1);
  EXPECT_EQ(At(StubVA + 4), 0);
}

TEST(AArch64FunctionDiscovery,
     DistantConditionalTargetsKeepTheirFrameInTheCaller) {
  constexpr va_t Base = 0x1000;
  constexpr size_t FrameOffset = 0x1014;
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Entry = Img.Base = Base;
  Segment Text;
  Text.VA = Base;
  Text.Size = FrameOffset + 8;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  for (size_t I = 0; I < Text.Data.size(); I += 4)
    writeLE<uint32_t>(Text.Data.data() + I, 0xd503201fu);
  // cbnz x0, slow_path; ...; ret; slow_path: paciasp; ret
  writeLE<uint32_t>(Text.Data.data(),
                    0xb5000000u | (uint32_t(FrameOffset / 4) << 5));
  writeLE<uint32_t>(Text.Data.data() + FrameOffset - 4, 0xd65f03c0u);
  writeLE<uint32_t>(Text.Data.data() + FrameOffset, 0xd503233fu);
  writeLE<uint32_t>(Text.Data.data() + FrameOffset + 4, 0xd65f03c0u);
  Img.Segments.push_back(std::move(Text));
  Img.Sections.push_back(
      makeMachOSection("__text", Base, FrameOffset + 8, true));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  const auto Functions = FuncDetector().detect(Img, Dec);
  EXPECT_FALSE(
      std::any_of(Functions.begin(), Functions.end(),
                  [](const auto &F) { return F.first == Base + FrameOffset; }));
}

TEST(AArch64FunctionDiscovery, MarkersAndBlockEndsAreOnlyBoundaryCandidates) {
  EXPECT_TRUE(isFunctionEntryMarkerWordAArch64(0xa9bf7bfdu));  // push -16
  EXPECT_FALSE(isFunctionEntryMarkerWordAArch64(0xa9817bfdu)); // store at +16
  EXPECT_FALSE(isAArch64BlockEndWord(0xd503201fu)); // nop falls through
  EXPECT_FALSE(isAArch64BlockEndWord(0x94000001u)); // bl falls through
  EXPECT_TRUE(isAArch64BlockEndWord(0xd65f03c0u));  // ret
  for (uint32_t Word : {0x54000100u, 0x54000110u, 0xb4000100u, 0xb5000100u,
                        0x36000100u, 0x37000100u})
    EXPECT_EQ(branch::a64ConditionalBranchDisplacement(Word), 32);
  EXPECT_EQ(branch::a64ConditionalBranchDisplacement(0x54ffffe0u), -4);
  EXPECT_EQ(branch::a64ConditionalBranchDisplacement(0x3607ffe0u), -4);
  EXPECT_FALSE(branch::a64ConditionalBranchDisplacement(0xd503201fu));
}
