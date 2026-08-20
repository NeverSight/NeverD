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
     PreservesTypedSymbolInsideBroadCompactUnwindRange) {
  constexpr va_t ImageVA = 0x2000;
  constexpr va_t LeafVA = ImageVA + 0x8;

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
  writeLE<uint32_t>(Text.Data.data(), 0xD65F03C0u); // ret
  writeLE<uint32_t>(Text.Data.data() + (LeafVA - ImageVA),
                    0xD65F03C0u); // ret
  Img.Segments.push_back(std::move(Text));
  Img.Sections.push_back(
      makeMachOSection("__text", ImageVA, 0x10, /*ContainsInstructions=*/true));

  Symbol Covering = Symbol::makeFunc(ImageVA, 0x10);
  Covering.Name = "_covering";
  Img.Symbols.push_back(std::move(Covering));
  Symbol Leaf = Symbol::makeFunc(LeafVA);
  Leaf.Name = "_leaf";
  Img.Symbols.push_back(std::move(Leaf));
  Img.Exports.push_back({"_covering", 0, ImageVA});
  Img.Exports.push_back({"_leaf", 0, LeafVA});
  Img.KnownCodeRanges.emplace_back(ImageVA, ImageVA + 0x10);

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

} // namespace
