//===- RegistrationRuntimePointerTests.cpp - PE32 table pointers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// SafeSEH storage in RX bytes cannot invent ordinary callback predecessors.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

namespace {
using namespace neverd;

struct RuntimeTableImage {
  static constexpr va_t Entry = 0x401000;
  static constexpr va_t Table = Entry + 0x40;
  static constexpr va_t Config = 0x402000;
  BinaryImage Image;

  RuntimeTableImage() {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = Entry;
    Image.DynInfo.LoadConfigRVA = Config - Image.Base;
    Image.DynInfo.LoadConfigSize = 0x48;
    Segment Text;
    Text.Name = ".text";
    Text.VA = Entry;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100, 0xcc);
    Text.Data[0] = Text.Data[0xc3] = 0xc3;
    // This valid handler RVA starts with RET when misread as instructions.
    writeLE<uint32_t>(Text.Data.data() + 0x40, 0x10c3);
    Text.Size = Text.FileSz = Text.Data.size();
    Segment Data;
    Data.Name = ".rdata";
    Data.VA = Config;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.resize(0x100);
    writeLE<uint32_t>(Data.Data.data(), 0x48);
    writeLE<uint32_t>(Data.Data.data() + 0x40, Table);
    writeLE<uint32_t>(Data.Data.data() + 0x44, 1);
    Data.Size = Data.FileSz = Data.Data.size();
    Image.Segments = {Text, Data};
    for (const auto &Segment : Image.Segments) {
      Section S;
      S.Name = Segment.Name;
      S.VA = Segment.VA;
      S.Size = S.FileSz = Segment.Size;
      S.Flags = Segment.Flags;
      Image.Sections.push_back(S);
    }
    Image.KnownCodeRanges.emplace_back(Entry, Entry + 0x80);
    Image.CodePtrRelocSlots.insert(Config + 0x40);
    Image.Symbols.push_back(Symbol::makeFunc(Table));
  }
};

TEST(RegistrationRuntimePointers, PreserveIndependentTableAddressReferences) {
  for (bool OrdinaryReference : {false, true}) {
    RuntimeTableImage F;
    if (OrdinaryReference) {
      writeLE<uint32_t>(F.Image.Segments[1].Data.data() + 0x80, F.Table);
      F.Image.CodePtrRelocSlots.insert(F.Config + 0x80);
    }
    const auto Pointer = coff_loader::getCheckedX86SafeSEHTablePointer(F.Image);
    ASSERT_TRUE(Pointer);
    EXPECT_EQ(*Pointer, std::make_pair(F.Config + 0x40, F.Table));
    const auto Roles =
        coff_loader::getCheckedX86RegistrationPointerRoles(F.Image);
    ASSERT_TRUE(Roles);
    EXPECT_EQ(Roles->Sources,
              (std::map<va_t, va_t>{{F.Config + 0x40, F.Table}}));
    EXPECT_EQ(Roles->RuntimeOnlyPointerTargets.count(F.Table),
              !OrdinaryReference);
    Decoder Decode;
    ASSERT_TRUE(Decode.init(F.Image));
    const auto Entries = FuncDetector().detect(F.Image, Decode);
    EXPECT_EQ(
        llvm::any_of(Entries,
                     [&](const auto &Entry) { return Entry.first == F.Table; }),
        OrdinaryReference);
    const auto Low = CFGBuilder().build(F.Image, Decode, F.Entry, "parent");
    EXPECT_EQ(Low.OrdinaryModuleAnalysisRoots.count(F.Table),
              OrdinaryReference);
  }
}

TEST(RegistrationRuntimePointers, RequireTheCompleteNativeTable) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    RuntimeTableImage F;
    switch (Mutation) {
    case 0:
      F.Image.DynInfo.LoadConfigSize = 0x44;
      break;
    case 1:
      writeLE<uint32_t>(F.Image.Segments[1].Data.data() + 0x44, 0);
      break;
    case 2:
      writeLE<uint32_t>(F.Image.Segments[1].Data.data() + 0x44, UINT32_MAX);
      break;
    case 3:
      writeLE<uint32_t>(F.Image.Segments[1].Data.data() + 0x40, 0x500000);
      break;
    case 4:
      writeLE<uint32_t>(F.Image.Segments[0].Data.data() + 0x40, 0x2000);
      break;
    case 5:
      F.Image.Bits = Bitness::Bits64;
      break;
    }
    EXPECT_FALSE(coff_loader::getCheckedX86SafeSEHTablePointer(F.Image));
  }
}
} // namespace
