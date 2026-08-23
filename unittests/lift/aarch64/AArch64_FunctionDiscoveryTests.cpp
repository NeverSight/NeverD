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
