//===- RegistrationCallTests.cpp - Checked PE32 call frame boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationStackOwner.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/RegistrationCallEffects.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace neverd;

namespace {
struct ThrowImage {
  static constexpr va_t TextVA = 0x401000;
  static constexpr va_t TableVA = 0x403000;
  static constexpr va_t DataVA = 0x404000;
  static constexpr va_t ImportVA = TextVA + 0x80;
  static constexpr va_t IATVA = DataVA + 0x30;
  static constexpr va_t CallerPCVA = DataVA + 0x20;
  BinaryImage Image;
  va_t CallVA = 0;

  ThrowImage(const std::vector<uint8_t> &Initialize = {0xc7, 0x45, 0xfc, 7, 0,
                                                       0, 0},
             const std::vector<uint8_t> &Extra = {}, int8_t Object = -4) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = TextVA;
    Segment Text;
    Text.Name = ".text";
    Text.VA = TextVA;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = {0x55, 0x8b, 0xec, 0x83, 0xec, 4};
    Text.Data.insert(Text.Data.end(), Initialize.begin(), Initialize.end());
    const std::vector<uint8_t> ObservePC = {0x8b, 0x45, 4,    0xa3,
                                            0x20, 0x40, 0x40, 0x00};
    Text.Data.insert(Text.Data.end(), ObservePC.begin(), ObservePC.end());
    Text.Data.insert(Text.Data.end(), Extra.begin(), Extra.end());
    const std::vector<uint8_t> Arguments = {
        0x68, 0x00, 0x30, 0x40, 0x00, 0x8d, 0x4d, uint8_t(Object), 0x51, 0xe8};
    Text.Data.insert(Text.Data.end(), Arguments.begin(), Arguments.end());
    CallVA = TextVA + Text.Data.size() - 1;
    const size_t Displacement = Text.Data.size();
    Text.Data.resize(Text.Data.size() + 4);
    writeLE<uint32_t>(Text.Data.data() + Displacement,
                      uint32_t(ImportVA - (CallVA + 5)));
    Text.Data.insert(Text.Data.end(), {0x90, 0x8b, 0xe5, 0x5d, 0xc3});
    Text.Data.resize(0x88, 0xcc);
    Text.Data[0x80] = 0xff;
    Text.Data[0x81] = 0x25;
    writeLE<uint32_t>(Text.Data.data() + 0x82, IATVA);
    Text.Size = Text.Data.size();
    Image.Segments.push_back(Text);
    Segment Table;
    Table.Name = ".rdata";
    Table.VA = TableVA;
    Table.Flags = SegmentFlags::Readable;
    Table.Data.resize(0x80);
    Table.Size = Table.Data.size();
    writeLE<uint32_t>(Table.Data.data() + 12, TableVA + 0x20);
    writeLE<uint32_t>(Table.Data.data() + 0x20, 1);
    writeLE<uint32_t>(Table.Data.data() + 0x24, TableVA + 0x30);
    writeLE<uint32_t>(Table.Data.data() + 0x30, 1);
    writeLE<uint32_t>(Table.Data.data() + 0x34, DataVA);
    writeLE<uint32_t>(Table.Data.data() + 0x3c, UINT32_MAX);
    writeLE<uint32_t>(Table.Data.data() + 0x44, 4);
    Image.Segments.push_back(Table);
    Segment Data;
    Data.Name = ".data";
    Data.VA = DataVA;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(0x80);
    Data.Data[8] = '.';
    Data.Data[9] = 'H';
    Data.Size = Data.Data.size();
    Image.Segments.push_back(Data);
    Import Import;
    Import.Module = "VCRUNTIME140.dll";
    Import.Name = "_CxxThrowException";
    Import.IATAddr = IATVA;
    Image.Imports.push_back(Import);
    for (auto &Segment : Image.Segments) {
      Segment.FileSz = Segment.Data.size();
      Section Section;
      Section.Name = Segment.Name;
      Section.VA = Segment.VA;
      Section.Size = Segment.Size;
      Section.FileSz = Segment.FileSz;
      Section.Flags = Segment.Flags;
      Image.Sections.push_back(Section);
    }
  }

  void tableWord(size_t Offset, uint32_t Value) {
    writeLE<uint32_t>(Image.Segments[1].Data.data() + Offset, Value);
  }
};
} // namespace

TEST(RegistrationCallABI, ThrowImportMustSurvivePrivateHelperEffects) {
  for (bool Rethrow : {false, true})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(Rethrow);
      SCOPED_TRACE(Mutation);
      std::vector<uint8_t> Extra;
      if (Mutation == 1 || Mutation == 4 || Mutation == 6) {
        Extra = {0xc7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0};
        const va_t Slot = Mutation == 4   ? ThrowImage::IATVA + 4
                          : Mutation == 6 ? ThrowImage::IATVA - 4
                                          : ThrowImage::IATVA;
        writeLE<uint32_t>(Extra.data() + 2, Slot);
        writeLE<uint32_t>(Extra.data() + 6, ThrowImage::TextVA + 0x90);
      } else if (Mutation == 2) {
        Extra = {0xc6, 0x05, 0, 0, 0, 0, 0};
        writeLE<uint32_t>(Extra.data() + 2, ThrowImage::IATVA + 3);
      } else if (Mutation == 3) {
        Extra = {0x66, 0xc7, 0x05, 0, 0, 0, 0, 0, 0};
        writeLE<uint32_t>(Extra.data() + 3, ThrowImage::IATVA - 1);
      } else if (Mutation == 5) {
        // EAX still contains the real caller PC from the fixture's observer.
        Extra = {0xa3, 0, 0, 0, 0};
        writeLE<uint32_t>(Extra.data() + 1, ThrowImage::IATVA);
      }
      ThrowImage F({0xc7, 0x45, 0xfc, 7, 0, 0, 0}, Extra);
      auto &Text = F.Image.Segments.front();
      Text.Data.resize(0x93, 0xcc);
      // A real replacement returns with the runtime's two arguments removed.
      // The original helper can then reach its retained ordinary epilogue.
      Text.Data[0x90] = 0xc2;
      Text.Data[0x91] = 8;
      Text.Data[0x92] = 0;
      Text.Size = Text.FileSz = Text.Data.size();
      F.Image.Sections.front().Size = F.Image.Sections.front().FileSz =
          Text.Size;
      if (Rethrow) {
        const size_t Args = F.CallVA - ThrowImage::TextVA - 9;
        const uint8_t NullArguments[] = {0x6a, 0,    0x90, 0x90, 0x90,
                                         0x31, 0xc9, 0x90, 0x51};
        std::copy(std::begin(NullArguments), std::end(NullArguments),
                  Text.Data.begin() + Args);
      }
      const auto Proof =
          getCheckedX86RegistrationThrowCalleeABI(F.Image, ThrowImage::TextVA);
      const bool Preserved = Mutation == 0 || Mutation == 4 || Mutation == 6;
      EXPECT_EQ(bool(Proof), Preserved);
      if (Proof)
        EXPECT_EQ(Proof->IsRethrow, Rethrow);
    }
}

TEST(RegistrationCallABI, StackCleanupRequiresEveryRestoredNearReturn) {
  auto MapText = [](BinaryImage &Image) {
    auto &Text = Image.Segments[0];
    Text.Size = Text.FileSz = Text.Data.size();
    Section Code;
    Code.Name = ".text";
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    Image.Sections = {Code};
  };
  struct Case {
    std::vector<uint8_t> Code;
    std::optional<uint32_t> Pop;
  };
  const Case Cases[] = {
      {{0xc3}, 0},
      {{0xc2, 8, 0}, 8},
      // A nested opaque call loses ESP, recovered explicitly through EBP.
      {{0x55, 0x8b, 0xec, 0x83, 0xec, 8, 0xff, 0xd0, 0x8b, 0xe5, 0x5d, 0xc3},
       0},
      // A different nonvolatile anchor also survives dynamic alignment.
      {{0x53, 0x8b, 0xdc, 0x83, 0xe4, 0xf0, 0xff, 0xd0, 0x8b, 0xe3, 0x5b, 0xc2,
        8, 0},
       8},
      // Both branches return with the same stack cleanup.
      {{0x85, 0xc0, 0x74, 3, 0xc2, 8, 0, 0xc2, 8, 0}, 8},
      // Maximum RET pop alone cannot establish either of these functions.
      {{0x85, 0xc0, 0x74, 3, 0xc2, 8, 0, 0xc3}, std::nullopt},
      {{0x85, 0xc0, 0x74, 3, 0xc2, 8, 0, 0x83, 0xec, 4, 0xc2, 8, 0},
       std::nullopt},
      {{0xff, 0xd0, 0xc3}, std::nullopt},
      // A caller-clobbered register is not a restoration anchor.
      {{0x8b, 0xc4, 0xff, 0xd1, 0x8b, 0xe0, 0xc3}, std::nullopt},
      {{0x83, 0xec, 4, 0xc3}, std::nullopt},
      {{0x5c, 0xc3}, std::nullopt},
      {{0xff, 0xe0}, std::nullopt},
      {{0xcb}, std::nullopt},
      // Memory alone cannot recover a stack identity in this domain.
      {{0x89, 0x64, 0x24, 0xfc, 0x8b, 0x64, 0x24, 0xfc, 0xc3}, std::nullopt},
  };
  for (unsigned I = 0; I != std::size(Cases); ++I) {
    SCOPED_TRACE(I);
    ThrowImage F;
    auto &Text = F.Image.Segments[0];
    Text.Data = Cases[I].Code;
    MapText(F.Image);
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA), Cases[I].Pop);
  }
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    auto &Text = F.Image.Segments[0];
    Text.Data = {0xc3};
    MapText(F.Image);
    if (Mutation == 0)
      Text.Flags = Text.Flags | SegmentFlags::Writable;
    if (Mutation == 1)
      F.Image.Format = BinaryFormat::ELF;
    if (Mutation == 2)
      F.Image.Arch = Arch::X64;
    if (Mutation == 3)
      F.Image.Bits = Bitness::Bits64;
    size_t Work = Mutation == 4 ? limits::kMaxRegistrationEHStateWork : 0;
    EXPECT_FALSE(getCheckedX86CalleeStackPop(F.Image, Text.VA, &Work));
  }
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    auto &Text = F.Image.Segments[0];
    Text.Data = {0x55, 0x8b, 0xec, 0xff, 0x15, 0x30, 0x40,
                 0x40, 0,    0x8b, 0xe5, 0x5d, 0xc3};
    MapText(F.Image);
    // This must remain an opaque returning call, not the fixture's
    // _CxxThrowException import, whose no-return contract ends the CFG.
    F.Image.Imports.clear();
    F.Image.BaseRelocations = {{Text.VA + 5, 3}};
    if (Mutation == 1)
      F.Image.BaseRelocations[0].Type = 10;
    if (Mutation == 2)
      F.Image.BaseRelocations[0].Address = Text.VA + 3;
    if (Mutation == 3)
      F.Image.BaseRelocations.push_back(F.Image.BaseRelocations[0]);
    if (Mutation == 4)
      F.Image.Segments.push_back(Text);
    EXPECT_EQ(
        getCheckedX86CalleeStackPop(F.Image, ThrowImage::TextVA).has_value(),
        Mutation == 0);
  }
}

TEST(RegistrationCallABI, ReturningStackDoesNotFreezeRelocatedOperands) {
  enum class Case {
    RelativeResult,
    RegisterRestore,
    RelocatedCancellation,
    DerivedCancellation,
    FrameStoreCancellation,
    WrongWidth,
    Duplicate,
    OpcodeFixup,
    ShortField,
    Writable,
    ConflictingTarget,
    RelativeControl
  };
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (Case C :
         {Case::RelativeResult, Case::RegisterRestore,
          Case::RelocatedCancellation, Case::DerivedCancellation,
          Case::FrameStoreCancellation, Case::WrongWidth, Case::Duplicate,
          Case::OpcodeFixup, Case::ShortField, Case::Writable,
          Case::ConflictingTarget, Case::RelativeControl}) {
      SCOPED_TRACE(Base);
      SCOPED_TRACE(static_cast<unsigned>(C));
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      Image.Base = Base;
      Image.Entry = Base + 0x1000;
      const bool Cancellation = C == Case::RelocatedCancellation ||
                                C == Case::DerivedCancellation ||
                                C == Case::FrameStoreCancellation;
      const uint32_t Value = Cancellation ? uint32_t(Image.Entry) : Base;
      Segment Text;
      Text.Name = ".text";
      Text.VA = Image.Entry;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      // The result depends on the load address; returning ESP does not.
      Text.Data = {0x8b, 0x44, 0x24, 4, 0x2d, 0, 0, 0, 0, 0xc2, 8, 0};
      size_t Field = 5;
      if (C == Case::RegisterRestore) {
        Text.Data = {0x55, 0x8b, 0xec, 0x81, 0xec, 0, 0, 0,
                     0,    0x8b, 0xe5, 0x5d, 0xc2, 8, 0};
      } else if (Cancellation) {
        // Equal preferred-base literals cease to cancel after rebasing.
        // Following the relocated MOV through another arithmetic producer
        // must not recover the original constant as a stack displacement.
        Text.Data = {0xb8, 0, 0, 0, 0};
        Field = 1;
        if (C == Case::DerivedCancellation)
          Text.Data.insert(Text.Data.end(), {0x83, 0xc0, 4});
        if (C == Case::FrameStoreCancellation)
          Text.Data.push_back(0x2d);
        else
          Text.Data.insert(Text.Data.end(), {0x01, 0xc4, 0x81, 0xec});
        const size_t Scalar = Text.Data.size();
        Text.Data.resize(Scalar + 4);
        writeLE<uint32_t>(Text.Data.data() + Scalar,
                          Value + (C == Case::DerivedCancellation ? 4 : 0));
        if (C == Case::FrameStoreCancellation)
          // Restoring ESP does not prove this apparent local write private:
          // the relocated delta changes its address after rebasing.
          Text.Data.insert(Text.Data.end(),
                           {0x83, 0xec, 4, 0x01, 0xe0, 0xc7, 0x00, 7, 0, 0, 0,
                            0x83, 0xc4, 4, 0x31, 0xc0});
        Text.Data.push_back(0xc3);
      } else if (C == Case::ShortField) {
        Text.Data = {0x66, 0x2d, 0, 0, 0x90, 0x90, 0xc3};
        Field = 2;
      } else if (C == Case::RelativeControl) {
        Text.Data = {0xe9, 0, 0, 0, 0, 0xc3};
        Field = 1;
      }
      if (C != Case::RelativeControl && C != Case::ShortField)
        writeLE<uint32_t>(Text.Data.data() + Field, Value);
      Text.Size = Text.FileSz = Text.Data.size();
      if (C == Case::Writable)
        Text.Flags = Text.Flags | SegmentFlags::Writable;
      Section Code;
      Code.Name = Text.Name;
      Code.VA = Text.VA;
      Code.Size = Code.FileSz = Text.Size;
      Code.Flags = Text.Flags;
      Image.Sections.push_back(Code);
      Image.Segments.push_back(Text);
      Image.BaseRelocations.push_back(
          {Text.VA + Field, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      if (C == Case::WrongWidth)
        Image.BaseRelocations[0].Type = llvm::COFF::IMAGE_REL_BASED_DIR64;
      if (C == Case::Duplicate)
        Image.BaseRelocations.push_back(Image.BaseRelocations[0]);
      if (C == Case::OpcodeFixup)
        --Image.BaseRelocations[0].Address;
      if (C == Case::ConflictingTarget)
        Image.DataAddressRelocOperands[Text.VA + Field] = {Base, Base + 4, 4};
      const auto ExpectedPop =
          C == Case::RelativeResult || C == Case::RegisterRestore
              ? std::optional<uint32_t>{8}
          : C == Case::FrameStoreCancellation ? std::optional<uint32_t>{0}
                                              : std::nullopt;
      EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Image.Entry), ExpectedPop);
      if (Cancellation)
        for (bool Classified : {false, true}) {
          SCOPED_TRACE(Classified);
          if (Classified)
            Image.DataAddressRelocOperands[Text.VA + Field] = {Value, Value, 4,
                                                               Text.VA};
          EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Image.Entry),
                    ExpectedPop);
          for (auto Result : {RegistrationLeafResult::Observed,
                              RegistrationLeafResult::Discarded})
            EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(
                Image, Image.Entry, nullptr, Result));
        }
    }
}

TEST(RegistrationCallABI, RelocatedLeafAddressesKeepMemoryIdentity) {
  for (bool ThroughOffset : {false, true})
    for (unsigned Classification : {0, 1, 2}) {
      SCOPED_TRACE(ThroughOffset);
      SCOPED_TRACE(Classification);
      ThrowImage F;
      auto &Text = F.Image.Segments.front();
      Text.Data = {0xb8, 0, 0, 0, 0};
      const uint32_t Address = ThrowImage::DataVA + 4;
      const uint32_t Encoded = Address - (ThroughOffset ? 4 : 0);
      writeLE<uint32_t>(Text.Data.data() + 1, Encoded);
      if (ThroughOffset)
        Text.Data.insert(Text.Data.end(), {0x83, 0xc0, 4});
      Text.Data.insert(Text.Data.end(), {0x8b, 0x00, 0xc3});
      Text.Size = Text.FileSz = Text.Data.size();
      Section Code;
      Code.Name = Text.Name;
      Code.VA = Text.VA;
      Code.Size = Code.FileSz = Text.Size;
      Code.Flags = Text.Flags;
      F.Image.Sections = {Code};
      F.Image.BaseRelocations = {{Text.VA + 1, 3}};
      if (Classification)
        F.Image.DataAddressRelocOperands[Text.VA + 1] = {
            Encoded, Encoded, 4,
            Classification == 2 ? ThrowImage::DataVA : InvalidVA};
      const auto Proof =
          getCheckedX86RegistrationLeafCalleeABI(F.Image, Text.VA);
      ASSERT_EQ(Proof.has_value(), Classification == 2);
      if (Proof) {
        ASSERT_EQ(Proof->ImageReads.size(), 1u);
        EXPECT_EQ(Proof->ImageReads[0].Begin, Address);
        EXPECT_EQ(Proof->ImageReads[0].End, Address + 4);
        EXPECT_TRUE(Proof->ImageWrites.empty());
      }
    }
}

TEST(RegistrationCallABI, StackCleanupIncludesCheckedNestedImportCleanup) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    auto &Text = F.Image.Segments[0];
    // Four stack arguments, RaiseException, then a near return without an
    // explicit ESP reset. Its exact stdcall contract restores entry ESP.
    Text.Data = {0x6a, 0,    0x6a, 0,    0x6a, 0, 0x6a, 0,
                 0xff, 0x15, 0x30, 0x40, 0x40, 0, 0xc3};
    Text.Size = Text.FileSz = Text.Data.size();
    Section Code;
    Code.Name = ".text";
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    F.Image.Sections[0] = Code;
    F.Image.Imports[0].Name = "RaiseException";
    F.Image.Imports[0].Module = "KERNEL32.dll";
    ASSERT_TRUE(F.Image.recordImportStorageSlot(
        ThrowImage::IATVA, "RaiseException", 0,
        ImportStorageEvidence::ImportDirectory));
    if (Mutation == 1)
      F.Image.Imports[0].Module = "untrusted.dll";
    if (Mutation == 2)
      F.Image.ConflictingImportStorageSlots.insert(ThrowImage::IATVA);
    if (Mutation == 3)
      F.Image.ImportStorageSlots.at(ThrowImage::IATVA).Addend = 4;
    if (Mutation == 4)
      Text.Data[0] = Text.Data[1] = 0x90; // One argument was not pushed.
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA),
              Mutation == 0 ? std::optional<uint32_t>{0} : std::nullopt);
  }
}

TEST(RegistrationCallABI, CRTMemoryImportsProveOnlyReturningStackCleanup) {
  struct Case {
    const char *Name;
    unsigned Arguments;
  };
  const Case Cases[] = {
      {"malloc", 1},       {"calloc", 2},       {"realloc", 2},
      {"free", 1},         {"memcpy", 3},       {"memmove", 3},
      {"memset", 3},       {"??2@YAPAXI@Z", 1}, {"??_U@YAPAXI@Z", 1},
      {"??3@YAXPAX@Z", 1}, {"??_V@YAXPAX@Z", 1}};
  for (const auto &C : Cases)
    for (bool ThroughStub : {false, true})
      for (unsigned Mutation = 0; Mutation != 19; ++Mutation) {
        SCOPED_TRACE(::testing::Message()
                     << C.Name << ' ' << ThroughStub << ' ' << Mutation);
        ThrowImage F;
        auto &Text = F.Image.Segments[0];
        Text.Data.clear();
        for (unsigned I = 0; I != C.Arguments; ++I)
          Text.Data.insert(Text.Data.end(), {0x6a, 0});
        const size_t Call = Text.Data.size();
        Text.Data.insert(Text.Data.end(),
                         {0xff, 0x15, 0x30, 0x40, 0x40, 0, 0x83, 0xc4,
                          uint8_t(C.Arguments * 4), 0xc3});
        if (ThroughStub) {
          Text.Data.resize(0x30, 0xcc);
          Text.Data.insert(Text.Data.end(), {0xff, 0x25, 0x30, 0x40, 0x40, 0});
          Text.Data[Call] = 0xe8;
          writeLE<uint32_t>(Text.Data.data() + Call + 1,
                            uint32_t(0x30 - Call - 5));
          Text.Data[Call + 5] = 0x90;
        }
        Text.Size = Text.FileSz = Text.Data.size();
        auto &Data = F.Image.Segments[2];
        Data.FileSz = Data.Size;
        Section Code;
        Code.Name = ".text";
        Code.VA = Text.VA;
        Code.Size = Code.FileSz = Text.Size;
        Code.Flags = Text.Flags;
        Section Storage;
        Storage.Name = ".data";
        Storage.VA = Data.VA;
        Storage.Size = Storage.FileSz = Data.Size;
        Storage.Flags = Data.Flags;
        F.Image.Sections = {Code, Storage};
        F.Image.Imports[0].Name = C.Name;
        F.Image.Imports[0].Module = "MSVCR100.dll";
        ASSERT_TRUE(F.Image.recordImportStorageSlot(
            ThrowImage::IATVA, C.Name, 0,
            ImportStorageEvidence::ImportDirectory));
        if (Mutation == 1)
          F.Image.Imports[0].Module = "msvcrt.dll";
        if (Mutation == 2)
          F.Image.Imports[0].Module = "custom.dll";
        if (Mutation == 3)
          F.Image.Imports[0].Name += "_unchecked";
        if (Mutation == 4)
          F.Image.Imports[0].IsDelayImport = true;
        if (Mutation == 5)
          F.Image.ConflictingImportStorageSlots.insert(ThrowImage::IATVA);
        if (Mutation == 6)
          F.Image.ImportStorageSlots.at(ThrowImage::IATVA).Addend = 4;
        if (Mutation == 7)
          Data.FileSz = ThrowImage::IATVA - Data.VA + 3;
        if (Mutation == 8) {
          F.Image.Imports.push_back(F.Image.Imports[0]);
          F.Image.Imports.back().Module = "custom.dll";
        }
        if (Mutation == 9)
          Text.Data[Call + 8] += 4; // Caller cleanup must still balance.
        if (Mutation == 10)
          F.Image.Format = BinaryFormat::ELF;
        if (Mutation == 11)
          F.Image.Arch = Arch::X64;
        if (Mutation == 12)
          F.Image.Bits = Bitness::Bits64;
        if (Mutation == 13)
          F.Image.ImportStorageSlots.at(ThrowImage::IATVA).Name = "different";
        if (Mutation == 14)
          F.Image.Imports[0].IATAddr += 4;
        if (Mutation == 15)
          F.Image.Sections.pop_back();
        if (Mutation == 16)
          F.Image.Segments.push_back(Data);
        if (Mutation == 17)
          F.Image.ImportStorageSlots.at(ThrowImage::IATVA).Evidence =
              ImportStorageEvidence::PointerTable;
        if (Mutation == 18) {
          F.Image.Imports[0].Name = "??3@YAXPEAX@Z"; // The x64 spelling.
          F.Image.ImportStorageSlots.at(ThrowImage::IATVA).Name =
              F.Image.Imports[0].Name;
        }
        if (Mutation < 2)
          ASSERT_TRUE(isFileBackedPEImportSlot(F.Image, ThrowImage::IATVA));
        EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, ThrowImage::TextVA),
                  Mutation < 2 ? std::optional<uint32_t>{0} : std::nullopt);
        // A returning ESP fact cannot authenticate allocation, deallocation,
        // callbacks, global writes or a private caller/object memory borrow.
        EXPECT_FALSE(getCheckedX86RegistrationObjectCalleeABI(
            F.Image, ThrowImage::TextVA));
        EXPECT_FALSE(getCheckedX86RegistrationInitializerCalleeABI(
            F.Image, ThrowImage::TextVA));
      }
}

TEST(RegistrationCallABI, RejectedTerminalLeavesBudgetForSiblingCallees) {
  ThrowImage F;
  auto &Text = F.Image.Segments.front();
  Text.Data.resize(0x500, 0xcc);
  const std::vector<uint8_t> Parent{
      0x55, 0x8b, 0xec, 0xe8, 0, 0,    0, 0,    0x85, 0xc0, 0x74, 4, 0x8b,
      0xe5, 0x5d, 0xc3, 0x6a, 0, 0x6a, 0, 0xe8, 0,    0,    0,    0};
  std::copy(Parent.begin(), Parent.end(), Text.Data.begin());
  writeLE<uint32_t>(Text.Data.data() + 4, 0x100 - 8);
  writeLE<uint32_t>(Text.Data.data() + 21, 0x80 - 25);
  constexpr unsigned Count = 24;
  for (unsigned I = 0; I != Count; ++I) {
    const size_t Offset = 0x100 + I * 0x20;
    size_t Return = Offset;
    if (I + 1 != Count) {
      Text.Data[Offset] = 0x6a;
      Text.Data[Offset + 1] = 0;
      Text.Data[Offset + 2] = 0xe8;
      writeLE<uint32_t>(Text.Data.data() + Offset + 3, 0x20 - 7);
      Return += 7;
    }
    Text.Data[Return] = 0xc2;
    Text.Data[Return + 1] = 4;
    Text.Data[Return + 2] = 0;
    F.Image.Symbols.push_back(Symbol::makeFunc(Text.VA + Offset, 0x20));
  }
  const va_t Sibling = Text.VA + 0x480;
  Text.Data[0x480] = 0xc2;
  Text.Data[0x481] = 8;
  Text.Data[0x482] = 0;
  Text.Size = Text.FileSz = Text.Data.size();
  F.Image.Sections.front().Size = F.Image.Sections.front().FileSz = Text.Size;
  F.Image.Symbols.push_back(Symbol::makeFunc(Text.VA, Parent.size()));
  F.Image.Symbols.push_back(Symbol::makeFunc(Sibling, 3));
  LowFunc Caller;
  Caller.Blocks.resize(1);
  for (va_t Target : {Text.VA, Sibling}) {
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(Target, 4));
    Caller.Blocks.front().Ops.push_back(Call);
  }
  // The reachable runtime-throw terminal cannot supply this plain returning
  // proof. Reject it before traversing the unrelated deep callee chain.
  size_t Work = limits::kMaxRegistrationEHStateWork - 1024;
  RegistrationCallCalleeIndex Index(F.Image, &Work);
  const auto Contracts = Index.stackContracts(Caller);
  ASSERT_TRUE(Contracts);
  ASSERT_EQ(Contracts->size(), 1u);
  EXPECT_EQ(Contracts->front().Target, Sibling);
  EXPECT_EQ(Contracts->front().StackPopBytes, 8u);
  EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
  EXPECT_FALSE(getCheckedX86CalleeStackPop(F.Image, Text.VA));
  // Replacing the terminal with a restored RET still takes the complete
  // ordinary proof, including the genuine nested stack dependencies.
  std::copy(Parent.begin() + 12, Parent.begin() + 16, Text.Data.begin() + 16);
  EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA),
            std::optional<uint32_t>{0});
}

TEST(RegistrationCallABI, DisconnectedTerminalsDoNotBecomeOrdinaryExits) {
  ThrowImage F;
  F.Image.Imports.clear();
  auto &Text = F.Image.Segments.front();
  Text.Data.assign(0x80, 0xcc);
  Text.Data[0] = 0xc3;
  Text.Data[0x40] = 0xff;
  Text.Data[0x41] = 0xe0;
  Text.Size = Text.FileSz = Text.Data.size();
  F.Image.Sections.front().Size = F.Image.Sections.front().FileSz = Text.Size;
  F.Image.Symbols.push_back(Symbol::makeFunc(Text.VA, Text.Size));
  F.Image.CodePtrRelocSlots.insert(ThrowImage::DataVA + 4);
  writeLE<uint32_t>(F.Image.Segments[2].Data.data() + 4, Text.VA + 0x40);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(F.Image));
  const auto Low = CFGBuilder().buildIntegerFrameProof(F.Image, Dec, Text.VA,
                                                       "separate-terminal");
  ASSERT_TRUE(Low.hasCompleteLiftCoverage());
  ASSERT_TRUE(Low.ModuleAnalysisRoots.count(Text.VA + 0x40));
  EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA),
            std::optional<uint32_t>{0});
}

TEST(RegistrationCallABI, NestedReturningStackProofIsBoundedAndNonCircular) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    F.Image.Imports.clear();
    auto &Text = F.Image.Segments[0];
    const unsigned Count = Mutation == 3 ? 32 : Mutation == 4 ? 33 : 3;
    Text.Data.assign(Count * 0x20, 0xcc);
    for (unsigned I = 0; I != Count; ++I) {
      const size_t Offset = I * 0x20;
      size_t Return = Offset;
      if (I + 1 != Count || Mutation == 2) {
        // Each wrapper pushes one argument, calls its child, and relies on
        // the child's independently proved `ret 4` to restore entry ESP.
        Text.Data[Offset] = 0x6a;
        Text.Data[Offset + 1] = 0;
        Text.Data[Offset + 2] = 0xe8;
        const size_t Target = I + 1 == Count ? 0 : Offset + 0x20;
        writeLE<uint32_t>(Text.Data.data() + Offset + 3,
                          uint32_t(Target - (Offset + 7)));
        Return += 7;
      }
      Text.Data[Return] = 0xc2;
      Text.Data[Return + 1] = Mutation == 1 && I + 1 == Count ? 8 : 4;
      Text.Data[Return + 2] = 0;
      F.Image.Symbols.push_back(Symbol::makeFunc(Text.VA + Offset, 0x20));
    }
    Text.Size = Text.FileSz = Text.Data.size();
    Section Code;
    Code.Name = ".text";
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    F.Image.Sections = {Code};
    size_t Work = Mutation == 5 ? limits::kMaxRegistrationEHStateWork - 20 : 0;
    const auto Pop = getCheckedX86CalleeStackPop(F.Image, Text.VA, &Work);
    EXPECT_EQ(Pop, Mutation == 0 || Mutation == 3 ? std::optional<uint32_t>{4}
                                                  : std::nullopt);
    EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
  }
}

TEST(RegistrationCallABI, RecordedStackPopsChargeTheirFreshWork) {
  ThrowImage F;
  F.Image.Imports.clear();
  auto &Text = F.Image.Segments[0];
  // Three wrappers, each pushing an argument and relying on its child's
  // proved `ret 4`, so the proof charges work for several nested bodies.
  constexpr unsigned Count = 3;
  Text.Data.assign(Count * 0x20, 0xcc);
  for (unsigned I = 0; I != Count; ++I) {
    const size_t Offset = I * 0x20;
    size_t Return = Offset;
    if (I + 1 != Count) {
      Text.Data[Offset] = 0x6a;
      Text.Data[Offset + 1] = 0;
      Text.Data[Offset + 2] = 0xe8;
      writeLE<uint32_t>(Text.Data.data() + Offset + 3, uint32_t(0x20 - 7));
      Return += 7;
    }
    Text.Data[Return] = 0xc2;
    Text.Data[Return + 1] = 4;
    Text.Data[Return + 2] = 0;
    F.Image.Symbols.push_back(Symbol::makeFunc(Text.VA + Offset, 0x20));
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Section Code;
  Code.Name = ".text";
  Code.VA = Text.VA;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  F.Image.Sections = {Code};

  size_t FreshWork = 0;
  const auto Fresh = getCheckedX86CalleeStackPop(F.Image, Text.VA, &FreshWork);
  ASSERT_EQ(Fresh, std::optional<uint32_t>{4});
  ASSERT_GT(FreshWork, 1u);

  RegistrationStackPopMemo Memo(F.Image);
  RegistrationStackPopMemoScope Scope(Memo);
  // The first query records its answer; later ones replay it with its work.
  for (unsigned Query = 0; Query != 2; ++Query) {
    SCOPED_TRACE(Query);
    size_t Work = 0;
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA, &Work), Fresh);
    EXPECT_EQ(Work, FreshWork);
  }
  // Outside any SEH state analysis; answers of other levels stay apart.
  ASSERT_EQ(x86SEHStateAnalysisLevel(), 0u);
  ASSERT_TRUE(Memo.find(F.Image, Text.VA, 0));
  EXPECT_FALSE(Memo.find(F.Image, Text.VA, 1));
  // The budget decides exactly as it does for a fresh computation: a total
  // that just fits succeeds at the limit, one unit less fails.
  for (const bool Fits : {true, false}) {
    SCOPED_TRACE(Fits);
    size_t Replayed = limits::kMaxRegistrationEHStateWork - FreshWork + !Fits;
    size_t Computed = Replayed;
    const auto Answer =
        getCheckedX86CalleeStackPop(F.Image, Text.VA, &Replayed);
    {
      // A fresh computation from the same remaining budget.
      RegistrationStackPopMemo Empty(F.Image);
      RegistrationStackPopMemoScope Inner(Empty);
      EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA, &Computed),
                Answer);
    }
    EXPECT_EQ(Replayed, Computed);
    EXPECT_EQ(Answer.has_value(), Fits);
    EXPECT_EQ(Replayed, limits::kMaxRegistrationEHStateWork);
  }
  // Another image never reads this memo.
  ThrowImage Other;
  Other.Image = F.Image;
  EXPECT_FALSE(Memo.find(Other.Image, Text.VA, 0));
}

TEST(RegistrationCallABI, OwnedStackQueriesKeepImageAndBudgetBoundaries) {
  ThrowImage F;
  F.Image.Imports.clear();
  auto &Text = F.Image.Segments.front();
  Text.Data = {0xc2, 4, 0};
  Text.Size = Text.FileSz = Text.Data.size();
  F.Image.Sections.front().Size = F.Image.Sections.front().FileSz = Text.Size;
  const va_t Target = Text.VA;
  auto Other = F.Image;
  Other.Segments.front().Data[1] = 8;

  size_t FreshWork = 0;
  ASSERT_EQ(getCheckedX86CalleeStackPop(F.Image, Target, &FreshWork), 4u);
  size_t Work = 0;
  RegistrationCallCalleeIndex Index(F.Image, &Work);
  ASSERT_EQ(Index.stackPop(Target), 4u);
  const size_t BeforeHit = Work;
  ASSERT_EQ(Index.stackPop(Target), 4u);
  const size_t HitWork = Work - BeforeHit;
  ASSERT_GT(HitWork, 0u);
  ASSERT_LT(HitWork, FreshWork);
  {
    registration_abi::StackProofOwnerScope Scope(Index, Work);
    const size_t BeforeOwned = Work;
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Target, &Work), 4u);
    EXPECT_EQ(Work - BeforeOwned, HitWork);
    // The same address in another image has a different return-pop encoding.
    EXPECT_EQ(getCheckedX86CalleeStackPop(Other, Target, &Work), 8u);
    const size_t BeforeOtherBudget = Work;
    size_t OtherBudget = limits::kMaxRegistrationEHStateWork - HitWork;
    // This allowance fits a hit but cannot pay for an independent proof. A
    // query on another counter must neither borrow nor charge this owner.
    EXPECT_FALSE(getCheckedX86CalleeStackPop(F.Image, Target, &OtherBudget));
    EXPECT_EQ(OtherBudget, limits::kMaxRegistrationEHStateWork);
    EXPECT_EQ(Work, BeforeOtherBudget);
    RegistrationCallCalleeIndex OtherIndex(Other, &Work);
    {
      registration_abi::StackProofOwnerScope Inner(OtherIndex, Work);
      EXPECT_EQ(getCheckedX86CalleeStackPop(Other, Target, &Work), 8u);
      EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Target, &Work), 4u);
    }
    const size_t BeforeRestored = Work;
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Target, &Work), 4u);
    EXPECT_EQ(Work - BeforeRestored, HitWork);
  }
  // Ending the owner scope restores independent public queries.
  const size_t BeforeIndependent = Work;
  EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Target, &Work), 4u);
  EXPECT_EQ(Work - BeforeIndependent, FreshWork);
}

TEST(RegistrationCallABI, ReturningStackProofDoesNotNormalizeTheX87Stack) {
  // Normalization refuses the first three bodies: an unproved tag for FCHS or
  // FXAM, or a return block split by TOP. The last FXAM has a proved tag.
  const std::vector<uint8_t> Cases[] = {
      {0xd9, 0xe0, 0xc3},
      {0xd9, 0xe5, 0xc3},
      {0x85, 0xc0, 0x74, 4, 0xd9, 0x44, 0x24, 4, 0xc3},
      {0xd9, 0x44, 0x24, 4, 0xd9, 0xe5, 0xdd, 0xd8, 0xc3},
      // Every constant ROM selector reads the control word. Neither that
      // read nor the binary80 result supplies an integer-frame value.
      {0xd9, 0xe8, 0xc3},
      {0xd9, 0xe9, 0xc3},
      {0xd9, 0xea, 0xc3},
      {0xd9, 0xeb, 0xc3},
      {0xd9, 0xec, 0xc3},
      {0xd9, 0xed, 0xc3},
      {0xd9, 0xee, 0xc3},
      {0xd9, 0x7c, 0x24, 0xfc, 0xc3}, // fnstcw [esp-4]
      {0xd9, 0x6c, 0x24, 4, 0xc3},    // fldcw [esp+4]
  };
  for (unsigned I = 0; I != std::size(Cases); ++I) {
    SCOPED_TRACE(I);
    ThrowImage F;
    F.Image.Imports.clear();
    auto &Text = F.Image.Segments[0];
    Text.Data = Cases[I];
    Text.Size = Text.FileSz = Text.Data.size();
    Section Code;
    Code.Name = ".text";
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    F.Image.Sections = {Code};
    // The x87 stack is not an input to ESP or near-return cleanup.
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA),
              std::optional<uint32_t>{0});
  }
}

TEST(RegistrationCallABI, StackImportContractRequiresTheExactRuntimeProvider) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    F.Image.Imports[0].Name = "RaiseException";
    F.Image.Imports[0].Module = "KERNEL32.dll";
    ASSERT_TRUE(F.Image.recordImportStorageSlot(
        ThrowImage::IATVA, "RaiseException", 0,
        ImportStorageEvidence::ImportDirectory));
    LowFunc Caller;
    Caller.Blocks.resize(1);
    LowOp Call;
    Call.Opcode = NdOp::INDIR_CALL;
    Call.addInput(NdVar::cst(ThrowImage::IATVA, 4));
    Caller.Blocks[0].Ops = {Call};
    if (Mutation == 1)
      F.Image.Imports[0].Module = "custom.dll";
    if (Mutation == 2)
      F.Image.Imports[0].Name = "OtherException";
    if (Mutation == 3) {
      F.Image.Imports.push_back(F.Image.Imports[0]);
      F.Image.Imports[1].Module = "custom.dll";
    }
    if (Mutation == 4)
      F.Image.ConflictingImportStorageSlots.insert(ThrowImage::IATVA);
    if (Mutation == 5)
      Caller.Blocks[0].Ops[0].Opcode = NdOp::CALL;
    if (Mutation == 6)
      Caller.Blocks[0].Ops[0].Inputs[0].Size = 8;
    RegistrationCallCalleeIndex Index(F.Image);
    const auto Contracts = Index.stackContracts(Caller);
    ASSERT_TRUE(Contracts);
    ASSERT_EQ(Contracts->size(), Mutation == 0 ? 1u : 0u);
    if (!Contracts->empty()) {
      EXPECT_EQ(Contracts->front().StackPopBytes, 16u);
      EXPECT_TRUE(Contracts->front().Indirect);
    }
  }
}

TEST(RegistrationCallABI, LocalUnwindIdentityRequiresCurrentProviderAndVeneer) {
  for (bool EH4 : {false, true})
    for (bool Indirect : {false, true}) {
      for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
        SCOPED_TRACE(Indirect);
        SCOPED_TRACE(EH4);
        SCOPED_TRACE(Mutation);
        ThrowImage F;
        const char *Name = EH4 ? "_local_unwind4" : "_local_unwind2";
        F.Image.Imports[0].Name = Name;
        ASSERT_TRUE(F.Image.recordImportStorageSlot(
            ThrowImage::IATVA, Name, 0, ImportStorageEvidence::PointerTable));
        auto &Text = F.Image.Segments[0];
        Text.FileSz = Text.Size;
        Section Code;
        Code.Name = ".text";
        Code.VA = Text.VA;
        Code.Size = Code.FileSz = Text.Size;
        Code.Flags = Text.Flags;
        F.Image.Sections = {Code};
        if (Mutation == 1)
          F.Image.Imports[0].Module = "custom.dll";
        if (Mutation == 2)
          F.Image.Imports[0].Name = "_local_unwind5";
        if (Mutation == 3) {
          F.Image.Imports.push_back(F.Image.Imports[0]);
          F.Image.Imports.back().Module = "custom.dll";
        }
        if (Mutation == 4)
          F.Image.ConflictingImportStorageSlots.insert(ThrowImage::IATVA);
        if (Mutation == 5)
          Text.Data[0x81] = 0x15; // a call is not a transparent jump veneer
        if (Mutation == 6)
          Text.Flags = Text.Flags | SegmentFlags::Writable;
        if (Mutation == 7)
          F.Image.Arch = Arch::X64;
        if (Mutation == 8)
          F.Image.Format = BinaryFormat::ELF;
        if (Mutation == 9)
          F.Image.Imports[0].Module = "MSVCRT.dll";
        if (Mutation == 10)
          F.Image.Bits = Bitness::Bits64;
        if (Mutation == 11)
          F.Image.Arch = Arch::ARM;
        if (Mutation == 12)
          F.Image.Arch = Arch::AArch64;
        if (Mutation == 13)
          F.Image.Format = BinaryFormat::MachO;
        const va_t Target = Indirect ? ThrowImage::IATVA : ThrowImage::ImportVA;
        const auto Contract =
            getCheckedX86LocalUnwindContract(F.Image, Target, Indirect);
        EXPECT_EQ(Contract.has_value(),
                  Mutation == 0 || Mutation == 9 ||
                      (Indirect && (Mutation == 5 || Mutation == 6)));
        EXPECT_FALSE(
            getCheckedX86LocalUnwindContract(F.Image, Target + 1, Indirect));
        if (Contract) {
          EXPECT_EQ(Contract->Target, Target);
          EXPECT_EQ(Contract->Indirect, Indirect);
          EXPECT_EQ(Contract->ImportSlot, ThrowImage::IATVA);
          EXPECT_EQ(Contract->Protocol,
                    EH4 ? RegistrationLocalUnwindContract::Kind::EH4
                        : RegistrationLocalUnwindContract::Kind::EH3);
        }
      }
    }
}

TEST(RegistrationCallABI, LocalUnwindBindsThePhysicalCallOccurrence) {
  for (bool EH4 : {false, true})
    for (bool Indirect : {false, true})
      for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
        SCOPED_TRACE(EH4);
        SCOPED_TRACE(Indirect);
        SCOPED_TRACE(Mutation);
        ThrowImage F;
        const char *Name = EH4 ? "_local_unwind4" : "_local_unwind2";
        F.Image.Imports[0].Name = Name;
        ASSERT_TRUE(F.Image.recordImportStorageSlot(
            ThrowImage::IATVA, Name, 0, ImportStorageEvidence::PointerTable));
        auto &Text = F.Image.Segments[0];
        const unsigned Size = Indirect ? 6 : 5;
        if (Indirect) {
          const size_t Offset = F.CallVA - Text.VA;
          Text.Data[Offset] = 0xff;
          Text.Data[Offset + 1] = 0x15;
          writeLE<uint32_t>(Text.Data.data() + Offset + 2, ThrowImage::IATVA);
        }
        Text.FileSz = Text.Size;
        Section Code;
        Code.Name = ".text";
        Code.VA = Text.VA;
        Code.Size = Code.FileSz = Text.Size;
        Code.Flags = Text.Flags;
        F.Image.Sections = {Code};
        const va_t Target = Indirect ? ThrowImage::IATVA : ThrowImage::ImportVA;
        const auto Runtime =
            getCheckedX86LocalUnwindContract(F.Image, Target, Indirect);
        ASSERT_TRUE(Runtime);
        LowFunc Function;
        Function.Blocks.resize(1);
        auto &Block = Function.Blocks.front();
        Block.Id = 0;
        Block.StartAddr = F.CallVA;
        Block.EndAddr = F.CallVA + Size;
        LowOp Call;
        Call.Addr = F.CallVA;
        Call.Seq = 7;
        Call.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
        Call.addInput(NdVar::cst(Target, 4));
        Block.Ops.push_back(Call);
        Block.InstructionBoundaries = {{F.CallVA, uint16_t(Size), 0, 1}};
        auto &Boundary = Block.InstructionBoundaries.front();
        Boundary.Control = LowInstructionControl::Call;
        Boundary.ControlFlags = LowInstructionControlFlag::Call;
        if (Indirect)
          Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
        else
          Boundary.Immediate = Target;
        RegistrationSEHLocalUnwind Receipt;
        Receipt.Address = F.CallVA;
        Receipt.EndAddress = Block.EndAddr;
        Receipt.OpSeq = Call.Seq;
        Receipt.Runtime = *Runtime;
        if (Mutation == 1)
          ++Receipt.OpSeq;
        if (Mutation == 2)
          ++Receipt.EndAddress;
        if (Mutation == 3)
          ++Receipt.Runtime.ImportSlot;
        if (Mutation == 4)
          Receipt.Runtime.Protocol =
              EH4 ? RegistrationLocalUnwindContract::Kind::EH3
                  : RegistrationLocalUnwindContract::Kind::EH4;
        if (Mutation == 5)
          Text.Data[F.CallVA - Text.VA] = 0x90;
        if (Mutation == 6)
          Text.Flags = Text.Flags | SegmentFlags::Writable;
        if (Mutation == 7)
          Boundary.ControlFlags |= LowInstructionControlFlag::Conditional;
        if (Mutation == 8)
          Boundary.ControlFlags |= LowInstructionControlFlag::InstructionGuard;
        if (Mutation == 9)
          Block.Ops.front().MemoryOrdering = NdMemoryOrdering::Acquire;
        if (Mutation == 10)
          Block.Ops.front().MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        if (Mutation == 11)
          Block.InstructionBoundaries.clear();
        if (Mutation == 12)
          F.Image.ConflictingImportStorageSlots.insert(ThrowImage::IATVA);
        if (Mutation == 14) {
          Boundary.ControlFlags = LowInstructionControlFlag::Call;
          if (!Indirect)
            Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
        }
        size_t Work = Mutation == 13 ? limits::kMaxRegistrationEHStateWork : 0;
        EXPECT_EQ(hasCheckedX86LocalUnwindCall(
                      F.Image, Function, Block.Ops.front(), Receipt, Work),
                  Mutation == 0);
        EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
      }
}

TEST(RegistrationCallABI, TerminationOnlyStackProofDoesNotGrantAnObjectBorrow) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    F.Image.Imports.clear();
    auto &Text = F.Image.Segments[0];
    Text.Data.assign(0x101, 0xcc);
    const std::vector<uint8_t> Entry{
        0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0,    0x30, 0x40, 0,    0x68,
        0,    0x11, 0x40, 0,    0x64, 0xa1, 0,    0,    0,    0,    0x50,
        0x64, 0x89, 0x25, 0,    0,    0,    0,    0xc7, 0x45, 0xfc, 0,
        0,    0,    0,    0xc7, 0x45, 0xe4, 1,    0,    0,    0,    0xc7,
        0x45, 0xfc, 0xff, 0xff, 0xff, 0xff, 0xc7, 0x45, 0xe4, 0,    0,
        0,    0,    0xe8, 0x12, 0,    0,    0,    0xeb, 0x30};
    std::copy(Entry.begin(), Entry.end(), Text.Data.begin());
    const std::vector<uint8_t> Finally{
        0x8b, 0x4d, 0x0c, 0x83, 0x7d, 0xe4, 0, 0x75, 8, 0xc7, 1, 99,
        0,    0,    0,    0xeb, 6,    0xc7, 1, 100,  0, 0,    0, 0xc3};
    std::copy(Finally.begin(), Finally.end(), Text.Data.begin() + 0x50);
    const std::vector<uint8_t> Epilogue{
        0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0, 0, 0, 0, 0x8b, 0xe5, 0x5d, 0xc3};
    std::copy(Epilogue.begin(), Epilogue.end(), Text.Data.begin() + 0x70);
    Text.Data[0x100] = 0xc3;
    Text.Size = Text.FileSz = Text.Data.size();
    Section Code;
    Code.Name = ".text";
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    F.Image.Sections = {Code};
    ExceptionFunction EH;
    EH.CodeRange = {Text.VA, Text.VA + 0xa0};
    EH.Personality = ExceptionPersonality::ExceptHandler3;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    auto &Chain = EH.Registration.emplace();
    Chain.HandlerVA = Text.VA + 0x100;
    Chain.ScopeTableVA = ThrowImage::TableVA;
    Chain.SeededTryLevel = -1;
    Chain.TryLevelOffset = -4;
    Chain.RegistrationOffset = -16;
    Chain.ChainInstallVA = Text.VA + 0x16;
    Chain.ChainRemoveVA = Text.VA + 0x73;
    Chain.Scopes = {{-1, 0, Text.VA + 0x50, true}};
    Chain.TryLevelStores = {{Text.VA + 0x1d, Text.VA + 0x24, 0},
                            {Text.VA + 0x2b, Text.VA + 0x32, -1}};
    F.tableWord(0, UINT32_MAX);
    F.tableWord(4, 0);
    F.tableWord(8, Text.VA + 0x50);
    if (Mutation == 1)
      Text.Data[0x67] = 0xcb; // A far RET cannot certify a near invocation.
    if (Mutation == 2) {
      // This source body abandons its private callback stack.
      Text.Data[0x67] = 0x8b;
      Text.Data[0x68] = 0x65;
      Text.Data[0x69] = 0xe8;
      Text.Data[0x6a] = 0xc3;
    }
    if (Mutation == 3)
      EH.ParseStatus = ExceptionParseStatus::Partial;
    if (Mutation == 4)
      Text.Flags = Text.Flags | SegmentFlags::Writable;
    if (Mutation == 5)
      Text.Data[0x7a] = Text.Data[0x7b] = 0x90;
    if (Mutation == 6) {
      const std::vector<uint8_t> CorruptFrame{0xbd, 0, 0, 0, 0, 0xc3};
      std::copy(CorruptFrame.begin(), CorruptFrame.end(),
                Text.Data.begin() + 0x50);
    }
    if (Mutation == 7) {
      // A nested call's zero pop count does not prove that it preserves the
      // established frame register used by this frame-sharing finally.
      const std::vector<uint8_t> NestedCall{0xe8, 0xab, 0, 0, 0, 0xc3};
      std::copy(NestedCall.begin(), NestedCall.end(), Text.Data.begin() + 0x50);
      Text.Data.resize(0x106, 0);
      Text.Data[0x100] = 0xbd;
      Text.Data[0x105] = 0xc3;
      Text.Size = Text.FileSz = Text.Data.size();
      F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = Text.Size;
    }
    F.Image.ExceptionMetadata.Functions.push_back(EH);
    F.Image.ExceptionMetadata.rebuildIndex();
    if (Mutation == 0) {
      Decoder Dec;
      ASSERT_TRUE(Dec.init(F.Image));
      const auto Low = CFGBuilder().build(F.Image, Dec, Text.VA);
      ASSERT_TRUE(Low.RegistrationStates);
      EXPECT_FALSE(Low.RegistrationStates->CallFrameEffectsComplete);
    }
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA),
              Mutation == 0 ? std::optional<uint32_t>{0} : std::nullopt);
  }
}

TEST(RegistrationCallABI, SEHReturningStackKeepsMemoryEvidenceSeparate) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    F.Image.Imports.clear();
    auto &Text = F.Image.Segments[0];
    Text.Data.assign(0x101, 0xcc);
    const std::vector<uint8_t> Entry{
        0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0,    0x30, 0x40, 0, // scope table
        0x68, 0,    0x11, 0x40, 0, // language handler
        0x64, 0xa1, 0,    0,    0,    0,    0x50, 0x64, 0x89, 0x25,
        0,    0,    0,    0,                   // install FS:[0]
        0xc7, 0x45, 0xfc, 0,    0,    0,    0, // enter scope
        0xb8, 42,   0,    0,    0,    0xc7, 0x45, 0xfc, 0xff, 0xff,
        0xff, 0xff, 0xeb, 0x3e}; // common parent epilogue
    std::copy(Entry.begin(), Entry.end(), Text.Data.begin());
    const std::vector<uint8_t> Filter{0xb8, 1, 0, 0, 0, 0xc3};
    std::copy(Filter.begin(), Filter.end(), Text.Data.begin() + 0x50);
    const std::vector<uint8_t> Handler{0xb8, 7, 0, 0, 0, 0xeb, 9};
    std::copy(Handler.begin(), Handler.end(), Text.Data.begin() + 0x60);
    const std::vector<uint8_t> Epilogue{
        0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0, 0, 0, 0, 0x8b, 0xe5, 0x5d, 0xc3};
    std::copy(Epilogue.begin(), Epilogue.end(), Text.Data.begin() + 0x70);
    Text.Data[0x100] = 0xc3;
    Text.Size = Text.FileSz = Text.Data.size();
    Section Code;
    Code.Name = ".text";
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    F.Image.Sections = {Code};
    ExceptionFunction EH;
    EH.CodeRange = {Text.VA, Text.VA + 0xa0};
    EH.Personality = ExceptionPersonality::ExceptHandler3;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    auto &Chain = EH.Registration.emplace();
    Chain.HandlerVA = Text.VA + 0x100;
    Chain.ScopeTableVA = ThrowImage::TableVA;
    Chain.SeededTryLevel = -1;
    Chain.TryLevelOffset = -4;
    Chain.RegistrationOffset = -16;
    Chain.ChainInstallVA = Text.VA + 0x16;
    Chain.ChainRemoveVA = Text.VA + 0x73;
    Chain.Scopes = {{-1, Text.VA + 0x50, Text.VA + 0x60, false}};
    Chain.TryLevelStores = {{Text.VA + 0x1d, Text.VA + 0x24, 0},
                            {Text.VA + 0x29, Text.VA + 0x30, -1}};
    if (Mutation == 1)
      Text.Data[0x7a] = Text.Data[0x7b] = 0x90; // missing ESP restoration
    if (Mutation == 2)
      Text.Data[0x60] = 0xbd; // handler corrupts established EBP
    if (Mutation == 3) {
      Chain.Scopes.front().HandlerVA = Text.VA + 0x80;
      std::copy(Epilogue.begin(), Epilogue.end(), Text.Data.begin() + 0x80);
      Text.Data[0x8d] = 0xc2; // only the exceptional parent return pops four
      Text.Data[0x8e] = 4;
      Text.Data[0x8f] = 0;
    }
    if (Mutation == 4)
      EH.ParseStatus = ExceptionParseStatus::Partial;
    if (Mutation == 5)
      Text.Flags = Text.Flags | SegmentFlags::Writable;
    if (Mutation == 6)
      Chain.ChainInstallVA += 1;
    if (Mutation == 7) {
      // Registration state is not a memory-preservation contract. An opaque
      // helper overwrites the saved EBP that the epilogue loads into ESP.
      const std::vector<uint8_t> OpaqueEpilogue{
          0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0,    0,    0,    0,
          0x83, 0xec, 4,    0x89, 0x6d, 0xec, 0x8d, 0x4d, 0xec, 0xe8,
          0x78, 0,    0,    0,    0x8b, 0x65, 0xec, 0x5d, 0xc3};
      std::copy(OpaqueEpilogue.begin(), OpaqueEpilogue.end(),
                Text.Data.begin() + 0x70);
      const std::vector<uint8_t> Writer{0xc7, 1, 0, 0, 0, 0, 0xc3};
      Text.Data.resize(0x107);
      std::copy(Writer.begin(), Writer.end(), Text.Data.begin() + 0x100);
      Text.Size = Text.FileSz = Text.Data.size();
      F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = Text.Size;
    }
    if (Mutation == 8) {
      // An unknown store prevents a registration memory proof, but does not
      // change the epilogue's register-only restoration of returning ESP.
      const std::vector<uint8_t> UnknownStore{0x89, 0x02, 0x90, 0x90, 0x90};
      std::copy(UnknownStore.begin(), UnknownStore.end(),
                Text.Data.begin() + 0x24);
    }
    F.tableWord(0, UINT32_MAX);
    F.tableWord(4, Chain.Scopes.front().FilterVA);
    F.tableWord(8, Chain.Scopes.front().HandlerVA);
    F.Image.ExceptionMetadata.Functions.push_back(EH);
    F.Image.ExceptionMetadata.rebuildIndex();
    if (Mutation == 0 || Mutation == 3 || Mutation == 7 || Mutation == 8) {
      Decoder Dec;
      ASSERT_TRUE(Dec.init(F.Image));
      const auto Low = CFGBuilder().build(F.Image, Dec, Text.VA);
      ASSERT_TRUE(Low.RegistrationStates);
      if (Mutation == 8)
        EXPECT_FALSE(Low.RegistrationStates->Complete);
      else {
        ASSERT_TRUE(Low.RegistrationStates->Complete);
        ASSERT_TRUE(Low.RegistrationStates->RegistrationLifetimeComplete);
      }
    }
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA),
              Mutation == 0 || Mutation == 8 ? std::optional<uint32_t>{0}
                                             : std::nullopt);
    // An outer ordinary helper must reuse the checked SEH result through
    // nested CFG construction, without starting a separate callee closure.
    Text.Data.resize(0x140, 0xcc);
    Text.Data[0x120] = 0xe8;
    writeLE<uint32_t>(Text.Data.data() + 0x121, uint32_t(-0x125));
    Text.Data[0x125] = 0xc3;
    Text.Size = Text.FileSz = Text.Data.size();
    F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = Text.Size;
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, Text.VA + 0x120),
              Mutation == 0 || Mutation == 8 ? std::optional<uint32_t>{0}
                                             : std::nullopt);
  }
}

TEST(RegistrationCallABI, LocalUnwindSplitsCurrentMachineStateAtCall) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    F.Image.Imports[0].Name = "_local_unwind2";
    ASSERT_TRUE(
        F.Image.recordImportStorageSlot(ThrowImage::IATVA, "_local_unwind2", 0,
                                        ImportStorageEvidence::PointerTable));
    auto &Text = F.Image.Segments[0];
    Text.Data.assign(0x101, 0xcc);
    const std::vector<uint8_t>
        Entry{0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0,    0x30,
              0x40, 0,    0x68, 0,    0x11, 0x40, 0,    0x64,
              0xa1, 0,    0,    0,    0,    0x50, 0x64, 0x89,
              0x25, 0,    0,    0,    0,    0x83, 0xec, 0x20, // private frame
              0x89, 0x65, 0xe8,                               // SavedESP
              0xc7, 0x45, 0xe4, 0,    0,    0,    0,          // local = 0
              0xc7, 0x45, 0xfc, 0,    0,    0,    0,          // enter scope 0
              0x6a, 0xff, 0x8d, 0x45, 0xf0, 0x50, 0xe8, 0x54,
              0,    0,    0, // _local_unwind2(frame, -1)
              0x83, 0xc4, 8,    0x8b, 0x45, 0xe4, 0xeb, 0x2c};
    std::copy(Entry.begin(), Entry.end(), Text.Data.begin());
    const std::vector<uint8_t> Finally{0xc7, 0x45, 0xe4, 7, 0, 0, 0, 0xc3};
    std::copy(Finally.begin(), Finally.end(), Text.Data.begin() + 0x50);
    if (Mutation >= 3) {
      std::vector<uint8_t> WithPrivateFrame{
          0x55, 0x8b, 0xec,                  // push ebp; mov ebp,esp
          0x83, 0xec, 8,                     // sub esp,8
          0x8b, 0x4d, 0,                     // mov ecx,[ebp]: parent's EBP
          0xc7, 0x41, 0xe4, 7,    0,   0, 0, // mov dword [ecx-28],7
          0x83, 0xc4, 8,    0x5d, 0xc3};     // add esp,8; pop ebp; ret
      if (Mutation == 4)
        WithPrivateFrame[18] = 4; // wrong return stack
      if (Mutation == 5)
        WithPrivateFrame[8] = 4; // read the runtime return PC
      if (Mutation == 6)
        WithPrivateFrame[19] = 0x58; // pop eax: EBP not restored
      if (Mutation == 7) {
        WithPrivateFrame.erase(WithPrivateFrame.begin() + 9,
                               WithPrivateFrame.begin() + 16);
        WithPrivateFrame.insert(WithPrivateFrame.begin() + 9,
                                {0x89, 0x61, 0xe4}); // escape callback ESP
      }
      std::copy(WithPrivateFrame.begin(), WithPrivateFrame.end(),
                Text.Data.begin() + 0x50);
    }
    const std::vector<uint8_t> Epilogue{
        0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0, 0, 0, 0, 0x8b, 0xe5, 0x5d, 0xc3};
    std::copy(Epilogue.begin(), Epilogue.end(), Text.Data.begin() + 0x70);
    Text.Data[0x90] = 0xff;
    Text.Data[0x91] = 0x25;
    writeLE<uint32_t>(Text.Data.data() + 0x92, ThrowImage::IATVA);
    Text.Data[0x100] = 0xc3;
    if (Mutation == 1)
      Text.Data[0x35] = 0xf4; // wrong registration argument
    if (Mutation == 2)
      Text.Data[0x52] = 0xe8; // finally overwrites SavedESP
    Text.Size = Text.FileSz = Text.Data.size();
    Section Code;
    Code.Name = ".text";
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    F.Image.Sections = {Code};
    ExceptionFunction EH;
    EH.CodeRange = {Text.VA, Text.VA + 0xa0};
    EH.Personality = ExceptionPersonality::ExceptHandler3;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    auto &Chain = EH.Registration.emplace();
    Chain.HandlerVA = Text.VA + 0x100;
    Chain.ScopeTableVA = ThrowImage::TableVA;
    Chain.SeededTryLevel = -1;
    Chain.TryLevelOffset = -4;
    Chain.RegistrationOffset = -16;
    Chain.ChainInstallVA = Text.VA + 0x16;
    Chain.ChainRemoveVA = Text.VA + 0x73;
    Chain.Scopes = {{-1, 0, Text.VA + 0x50, true}};
    Chain.TryLevelStores = {{Text.VA + 0x2a, Text.VA + 0x31, 0}};
    F.tableWord(0, UINT32_MAX);
    F.tableWord(4, 0);
    F.tableWord(8, Text.VA + 0x50);
    F.Image.ExceptionMetadata.Functions.push_back(EH);
    F.Image.ExceptionMetadata.rebuildIndex();
    Decoder Dec;
    ASSERT_TRUE(Dec.init(F.Image));
    const auto Low = CFGBuilder().build(F.Image, Dec, Text.VA);
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    ASSERT_TRUE(Low.RegistrationStates);
    const auto &States = *Low.RegistrationStates;
    const bool Valid = Mutation == 0 || Mutation == 3;
    EXPECT_EQ(States.Complete, Valid);
    EXPECT_EQ(States.RegistrationLifetimeComplete, Valid);
    if (Valid)
      EXPECT_TRUE(States.CallbackStatesComplete);
    EXPECT_FALSE(States.CallFrameEffectsComplete);
    bool SawContinuation = false;
    for (const auto &State : States.Blocks)
      if (State.Range.Begin == Text.VA + 0x3c) {
        SawContinuation = true;
        if (Valid)
          EXPECT_EQ(State.levels(), std::vector<int32_t>{-1});
      }
    EXPECT_TRUE(SawContinuation);
  }
}

TEST(RegistrationCallABI, ChecksImmutableSimpleThrowInfo) {
  for (unsigned Mutation = 0; Mutation != 26; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    switch (Mutation) {
    case 1:
      F.tableWord(0, 8);
      break;
    case 2:
      F.tableWord(4, ThrowImage::TextVA);
      break;
    case 3:
      F.tableWord(8, ThrowImage::TextVA);
      break;
    case 4:
      F.tableWord(12, 0);
      break;
    case 5:
      F.tableWord(0x20, 0);
      break;
    case 6:
      F.tableWord(0x20, 2);
      break;
    case 7:
      F.tableWord(0x24, 0);
      break;
    case 8:
      F.tableWord(0x30, 2);
      break;
    case 9:
      F.tableWord(0x30, 5);
      break;
    case 10:
      F.tableWord(0x38, 4);
      break;
    case 11:
      F.tableWord(0x3c, 0);
      break;
    case 12:
      F.tableWord(0x40, 4);
      break;
    case 13:
      F.tableWord(0x44, 0);
      break;
    case 14:
      F.tableWord(0x44, 0x100001);
      break;
    case 15:
      F.tableWord(0x48, ThrowImage::TextVA);
      break;
    case 16:
      F.tableWord(0x34, 0);
      break;
    case 17:
      F.Image.Segments[2].Data[8] = 'H';
      break;
    case 18:
      std::fill(F.Image.Segments[2].Data.begin() + 9,
                F.Image.Segments[2].Data.end(), 'H');
      break;
    case 19:
      F.Image.Segments[1].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 20:
      F.Image.Segments[1].Flags =
          SegmentFlags::Readable | SegmentFlags::Executable;
      break;
    case 21:
      F.Image.Arch = Arch::X64;
      break;
    case 22:
      F.Image.Bits = Bitness::Bits64;
      break;
    case 23:
      F.Image.Segments[1].Data.resize(15);
      break;
    case 24:
      // A valid-looking TypeDescriptor cannot overlap its CatchableType.
      F.tableWord(0x34, ThrowImage::TableVA + 0x48);
      F.Image.Segments[1].Data[0x50] = '.';
      F.Image.Segments[1].Data[0x51] = 'H';
      break;
    case 25:
      F.Image.Format = BinaryFormat::ELF;
      break;
    }
    const auto P = coff_loader::getCheckedX86SimpleCxxThrowInfo(
        F.Image, ThrowImage::TableVA);
    ASSERT_EQ(P.has_value(), Mutation == 0);
    if (!P)
      continue;
    EXPECT_EQ(P->Address, ThrowImage::TableVA);
    EXPECT_EQ(P->TypeDescriptorVA, ThrowImage::DataVA);
    EXPECT_EQ(P->ObjectSize, 4u);
    ASSERT_EQ(P->ReadOnlyRanges.size(), 3u);
    EXPECT_EQ(P->ReadOnlyRanges[0].Begin, ThrowImage::TableVA);
    EXPECT_EQ(P->ReadOnlyRanges[0].End, ThrowImage::TableVA + 16);
    EXPECT_EQ(P->TypeDescriptorRange.Begin, ThrowImage::DataVA);
    EXPECT_EQ(P->TypeDescriptorRange.End, ThrowImage::DataVA + 11);
  }
}

TEST(RegistrationCallABI, BindsTheThrowImportAndInitializedPrivateObject) {
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    switch (Mutation) {
    case 1:
      F.Image.Imports.front().Module = "untrusted.dll";
      break;
    case 2:
      F.Image.Imports.front().Name = "CxxThrowException";
      break;
    case 3:
      F.Image.Imports.front().IATAddr = 0;
      break;
    case 4:
      F.tableWord(0x20, 2);
      break;
    case 5:
      F = ThrowImage(std::vector<uint8_t>{});
      break;
    case 6:
      F = ThrowImage({0xc6, 0x45, 0xfc, 7});
      break;
    case 7:
      F = ThrowImage({0xc7, 0x45, 0xfc, 7, 0, 0, 0}, {}, -8);
      break;
    case 8:
      F = ThrowImage({0xc7, 0x45, 0xfc, 7, 0, 0, 0}, {}, 8);
      break;
    case 9:
      F = ThrowImage({0x8d, 0x45, 0xfc, 0x89, 0x45, 0xfc});
      break;
    case 10:
      F = ThrowImage({0xc7, 0x45, 0xfc, 7, 0, 0, 0}, {0x64, 0xa1, 0, 0, 0, 0});
      break;
    case 11:
      // Reading the observed real caller PC back would erase its provenance.
      F = ThrowImage({0xc7, 0x45, 0xfc, 7, 0, 0, 0},
                     {0xa1, 0x21, 0x40, 0x40, 0x00});
      break;
    case 12:
      // A runtime type-name mutation cannot hide behind its writable cache.
      F = ThrowImage({0xc7, 0x45, 0xfc, 7, 0, 0, 0},
                     {0xc6, 0x05, 0x08, 0x40, 0x40, 0x00, 'X'});
      break;
    }
    const auto P =
        getCheckedX86RegistrationThrowCalleeABI(F.Image, ThrowImage::TextVA);
    ASSERT_EQ(P.has_value(), Mutation == 0);
    if (!P)
      continue;
    EXPECT_EQ(P->Target, ThrowImage::TextVA);
    EXPECT_EQ(P->ImportVA, ThrowImage::ImportVA);
    EXPECT_EQ(P->ImportIATVA, ThrowImage::IATVA);
    EXPECT_EQ(P->ThrowCallVA, F.CallVA);
    EXPECT_EQ(P->ThrowCallEndVA, F.CallVA + 5);
    EXPECT_GE(P->ThrowOpSeq, 0);
    EXPECT_EQ(P->ObjectOffset, -4);
    EXPECT_EQ(P->ThrowInfo.ObjectSize, 4u);
    ASSERT_EQ(P->CallerPCWrites.size(), 1u);
    EXPECT_EQ(P->CallerPCWrites.front().Begin, ThrowImage::CallerPCVA);
    EXPECT_EQ(P->CallerPCWrites.front().End, ThrowImage::CallerPCVA + 4);
    ASSERT_EQ(P->CodeRanges.size(), 2u);
    EXPECT_EQ(P->CodeRanges[0].Begin, ThrowImage::TextVA);
    EXPECT_EQ(P->CodeRanges[0].End, F.CallVA + 5);
    EXPECT_EQ(P->CodeRanges[1].Begin, ThrowImage::ImportVA);
    EXPECT_EQ(P->CodeRanges[1].End, ThrowImage::ImportVA + 6);
  }
}

TEST(RegistrationCallABI, PreservedCodeExtentsExcludeUnreachablePadding) {
  ThrowImage F;
  auto &Text = F.Image.Segments[0];
  Text.Data.assign(0x26, 0xcc);
  Text.Data[0] = 0xe9;
  writeLE<int32_t>(Text.Data.data() + 1, 0x20 - 5);
  Text.Data[0x20] = 0xb8;
  writeLE<uint32_t>(Text.Data.data() + 0x21, 1);
  Text.Data[0x25] = 0xc3;
  Text.Size = Text.FileSz = Text.Data.size();
  auto Leaf = getCheckedX86RegistrationLeafCalleeABI(F.Image, Text.VA);
  ASSERT_TRUE(Leaf);
  ASSERT_EQ(Leaf->CodeRanges.size(), 2u);
  EXPECT_EQ(Leaf->CodeRanges[0].Begin, Text.VA);
  EXPECT_EQ(Leaf->CodeRanges[0].End, Text.VA + 5);
  EXPECT_EQ(Leaf->CodeRanges[1].Begin, Text.VA + 0x20);
  EXPECT_EQ(Leaf->CodeRanges[1].End, Text.VA + 0x26);
}

TEST(RegistrationCallABI, PrivateThrowCannotInheritAChangedImportThunk) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    ThrowImage F;
    auto &Text = F.Image.Segments[0];
    if (Mutation == 1)
      Text.Data[0x81] = 0x15;
    if (Mutation == 2)
      writeLE<uint32_t>(Text.Data.data() + 0x82, ThrowImage::IATVA + 4);
    EXPECT_EQ(bool(getCheckedX86RegistrationThrowCalleeABI(F.Image,
                                                           ThrowImage::TextVA)),
              Mutation == 0);
  }
}

TEST(RegistrationCallABI, UsesTheExactScalarObjectWidth) {
  for (uint32_t Width : {1u, 2u, 4u, 8u}) {
    SCOPED_TRACE(Width);
    ThrowImage F({0xc6, 0x45, 0xfc, 7});
    F.tableWord(0x44, Width);
    const auto P =
        getCheckedX86RegistrationThrowCalleeABI(F.Image, ThrowImage::TextVA);
    EXPECT_EQ(P.has_value(), Width == 1);
    if (P)
      EXPECT_EQ(P->ThrowInfo.ObjectSize, Width);
  }
}

TEST(RegistrationCallABI, SharesTheBudgetAcrossFailedAndSuccessfulProofs) {
  ThrowImage F;
  size_t Work = 0;
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(
      F.Image, ThrowImage::TextVA, &Work));
  EXPECT_GT(Work, 0u);
  const size_t FailedWork = Work;
  ASSERT_TRUE(getCheckedX86RegistrationThrowCalleeABI(
      F.Image, ThrowImage::TextVA, &Work));
  EXPECT_GT(Work, FailedWork);
  Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(
      F.Image, ThrowImage::TextVA, &Work));
  EXPECT_FALSE(getCheckedX86RegistrationThrowCalleeABI(
      F.Image, ThrowImage::TextVA, &Work));
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationCallABI, BindsTheMemoizedContractToOneBuildImage) {
  ThrowImage F;
  LowFunc Caller;
  Caller.Blocks.resize(1);
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(ThrowImage::TextVA, 4));
  Caller.Blocks[0].Ops = {Call, Call};
  RegistrationCallCalleeIndex Index(F.Image);
  const auto Contracts = Index.contracts(Caller);
  ASSERT_TRUE(Contracts);
  ASSERT_EQ(Contracts->size(), 1u);
  const auto &C = Contracts->front();
  EXPECT_EQ(C.CalleeKind, RegistrationCalleeFrameContract::Kind::PrivateThrow);
  EXPECT_TRUE(C.DoesNotReturn);
  EXPECT_EQ(C.ThrownTypeVA, ThrowImage::DataVA);
  EXPECT_EQ(C.ThrownObjectSize, 4u);
  EXPECT_TRUE(C.ECXReads.empty());
  EXPECT_TRUE(C.ECXWrites.empty());
  ASSERT_EQ(C.CallerPCWrites.size(), 1u);
  EXPECT_EQ(C.CallerPCWrites[0].Begin, ThrowImage::CallerPCVA);
  EXPECT_EQ(Index.contracts(Caller)->size(), 1u);
  F.Image.Imports[0].Name = "unknown_throw";
  RegistrationCallCalleeIndex NextBuild(F.Image);
  const auto Changed = NextBuild.contracts(Caller);
  ASSERT_TRUE(Changed);
  EXPECT_TRUE(Changed->empty());
}

TEST(RegistrationCallABI, PrivateThrowClosesOnlyOrdinaryFallthrough) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ThrowImage F;
    Segment Parent;
    Parent.VA = 0x405000;
    Parent.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Parent.Data = {
        0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0, 0x11, 0x40, 0, 0x64, 0xa1, 0, 0,
        0, 0, 0x50, 0x64, 0x89, 0x25, 0, 0, 0, 0, 0xe8, 0, 0, 0, 0,
        // This dead branch lands inside the independently rooted callback.
        0xeb, 1, 0xb8, 1, 2, 3, 4, 0xc3};
    writeLE<uint32_t>(Parent.Data.data() + 25,
                      uint32_t(ThrowImage::TextVA - (Parent.VA + 29)));
    Parent.Size = Parent.FileSz = Parent.Data.size();
    ExceptionFunction EH;
    EH.CodeRange = {Parent.VA, Parent.VA + Parent.Size};
    EH.Personality = ExceptionPersonality::CxxFrameHandler3;
    EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
    auto &Chain = EH.Registration.emplace();
    Chain.HandlerVA = ThrowImage::TextVA + 0x100;
    Chain.SeededTryLevel = -1;
    Chain.TryLevelOffset = -4;
    Chain.RegistrationOffset = -12;
    Chain.ChainInstallVA = Parent.VA + 17;
    Chain.TryLevelStores = {{Parent.VA + 3, Parent.VA + 5, -1}};
    auto &Cxx = EH.Cxx.emplace();
    Cxx.MaxState = 2;
    Cxx.UnwindMap.resize(2);
    CxxTryBlock Try;
    Try.TryLow = Try.TryHigh = 0;
    Try.CatchHigh = 1;
    CxxCatchHandler Catch;
    Catch.Adjectives = 0x40;
    Catch.HandlerVA = Parent.VA + 31;
    Try.Handlers.push_back(Catch);
    Cxx.TryBlocks.push_back(Try);
    F.Image.Segments.push_back(Parent);
    F.Image.ExceptionMetadata.Functions.push_back(EH);
    F.Image.ExceptionMetadata.rebuildIndex();
    if (Mutation == 1)
      F.Image.Imports.front().Name = "unknown_throw";
    if (Mutation == 2)
      F.Image.Segments.front().Data[0] = 0xc3;
    Decoder Decode;
    ASSERT_TRUE(Decode.init(F.Image));
    const auto Low = CFGBuilder().build(F.Image, Decode, Parent.VA);
    bool DeadBranch = false, Callback = false, NoReturnCall = false;
    for (const auto &Block : Low.Blocks)
      for (const auto &Boundary : Block.InstructionBoundaries) {
        DeadBranch |= Boundary.Address == Parent.VA + 29;
        Callback |= Boundary.Address == Parent.VA + 31;
        if (Boundary.Address == Parent.VA + 24)
          NoReturnCall = hasLowInstructionControlFlag(
              Boundary.ControlFlags, LowInstructionControlFlag::NoReturn);
      }
    EXPECT_EQ(NoReturnCall, Mutation == 0);
    EXPECT_EQ(DeadBranch, Mutation != 0);
    EXPECT_TRUE(Callback);
    if (Mutation == 0) {
      auto Error = validateLowInstructionBoundaries(
          Low, LowInstructionBoundaryRequirement::Required);
      EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    }
  }
}

TEST(RegistrationCallABI, RuntimeThrowTypesBelongToTheCurrentCaller) {
  for (bool Reverse : {false, true}) {
    ThrowImage F;
    LowFunc Caller;
    Caller.Blocks.resize(1);
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(ThrowImage::ImportVA, 4));
    Caller.Blocks[0].Ops.push_back(Call);
    auto Typed = Caller;
    LowOp Table;
    Table.Opcode = NdOp::COPY;
    Table.addInput(NdVar::cst(ThrowImage::TableVA, 4));
    Typed.Blocks[0].Ops.push_back(Table);
    RegistrationCallCalleeIndex Index(F.Image);
    auto Check = [&](bool WithType) {
      const auto Contracts = Index.contracts(WithType ? Typed : Caller);
      ASSERT_TRUE(Contracts);
      ASSERT_EQ(Contracts->size(), 1u);
      const auto &Contract = Contracts->front();
      ASSERT_TRUE(Contract.isRuntimeThrow());
      ASSERT_EQ(Contract.RuntimeThrowInfos.size(), unsigned(WithType));
      if (WithType) {
        EXPECT_EQ(Contract.RuntimeThrowInfos[0].Address, ThrowImage::TableVA);
        EXPECT_EQ(Contract.RuntimeThrowInfos[0].TypeDescriptorVA,
                  ThrowImage::DataVA);
        EXPECT_EQ(Contract.RuntimeThrowInfos[0].ObjectSize, 4u);
      }
    };
    Check(Reverse);
    Check(!Reverse);
    Check(Reverse);
    auto Malformed = Typed;
    Malformed.Blocks[0].Ops.back().NumInputs = 255;
    EXPECT_FALSE(Index.contracts(Malformed));
    F.tableWord(0x44, 0);
    RegistrationCallCalleeIndex ChangedIndex(F.Image);
    auto Changed = ChangedIndex.contracts(Typed);
    ASSERT_TRUE(Changed);
    ASSERT_EQ(Changed->size(), 1u);
    EXPECT_TRUE(Changed->front().RuntimeThrowInfos.empty());
  }
}

TEST(RegistrationCallABI, ScopeExtentKeepsTheFormatHeaderAndPE32Bounds) {
  for (bool EH4 : {false, true}) {
    ExceptionFunction F;
    F.Personality = EH4 ? ExceptionPersonality::ExceptHandler4
                        : ExceptionPersonality::ExceptHandler3;
    F.Encoding = EH4 ? ExceptionEncoding::X86ScopeTableEH4
                     : ExceptionEncoding::X86ScopeTableEH3;
    auto &C = F.Registration.emplace();
    C.ScopeTableVA = 0x403000;
    C.Scopes.resize(2);
    const auto Extent = coff_loader::getX86RegistrationSEHScopeTableRange(F);
    ASSERT_TRUE(Extent);
    EXPECT_EQ(Extent->Begin, 0x403000u);
    EXPECT_EQ(Extent->End, 0x403000u + 24 + (EH4 ? 16 : 0));
    C.Scopes.resize(1);
    C.ScopeTableVA = uint64_t(UINT32_MAX) + 1 - 12 - (EH4 ? 16 : 0);
    const auto Last = coff_loader::getX86RegistrationSEHScopeTableRange(F);
    ASSERT_TRUE(Last);
    EXPECT_EQ(Last->End, uint64_t(UINT32_MAX) + 1);
    ++C.ScopeTableVA;
    EXPECT_FALSE(coff_loader::getX86RegistrationSEHScopeTableRange(F));
    C.ScopeTableVA = 0;
    EXPECT_FALSE(coff_loader::getX86RegistrationSEHScopeTableRange(F));
    C.ScopeTableVA = 0x403000;
    C.Scopes.clear();
    EXPECT_FALSE(coff_loader::getX86RegistrationSEHScopeTableRange(F));
    C.Scopes.resize(1);
    F.Encoding = ExceptionEncoding::X86CxxFuncInfo;
    EXPECT_FALSE(coff_loader::getX86RegistrationSEHScopeTableRange(F));
  }
}

TEST(RegistrationCallABI, PreservedCalleesCannotChangeEitherSEHScopeFormat) {
  for (bool EH4 : {false, true}) {
    const va_t TableVA = 0x403000;
    const va_t CookieVA = 0x403040;
    const va_t EndVA = TableVA + 24 + (EH4 ? 16 : 0);
    for (va_t WriteVA : {TableVA - 4, TableVA, TableVA + 8, EndVA - 4,
                         EndVA - 1, EndVA, CookieVA}) {
      SCOPED_TRACE(WriteVA);
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      Image.Base = 0x400000;
      Image.DynInfo.SecurityCookieRVA = CookieVA - Image.Base;
      Segment Text;
      Text.VA = 0x401000;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data = {0xc7, 0x05, 0, 0, 0, 0, 7, 0, 0, 0, 0x31, 0xc0, 0xc3};
      writeLE<uint32_t>(Text.Data.data() + 2, WriteVA);
      Text.Size = Text.FileSz = Text.Data.size();
      Image.Segments.push_back(Text);
      Section Code;
      Code.VA = Text.VA;
      Code.Size = Code.FileSz = Text.Size;
      Code.Flags = Text.Flags;
      Image.Sections.push_back(Code);
      Segment Data;
      Data.VA = TableVA - 16;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(128);
      Data.Size = Data.FileSz = Data.Data.size();
      Image.Segments.push_back(Data);
      LowFunc Source;
      Source.Entry = 0x402000;
      Source.Blocks.emplace_back();
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(Text.VA, 4));
      Source.Blocks[0].Ops.push_back(Call);
      auto &F = Source.ExceptionMetadata.emplace();
      F.Personality = EH4 ? ExceptionPersonality::ExceptHandler4
                          : ExceptionPersonality::ExceptHandler3;
      F.Encoding = EH4 ? ExceptionEncoding::X86ScopeTableEH4
                       : ExceptionEncoding::X86ScopeTableEH3;
      auto &C = F.Registration.emplace();
      C.ScopeTableVA = TableVA;
      C.Scopes = {{-1, 0x402040, 0x402050, false},
                  {0, 0x402060, 0x402070, false}};
      const bool WritesTable = WriteVA < EndVA && TableVA < WriteVA + 4;
      EXPECT_EQ(hasCallerCleanupRegistrationABI(Source, Image),
                !WritesTable && !(EH4 && WriteVA == CookieVA))
          << EH4;
    }
  }
}

namespace {
struct CleanupRelayImage {
  static constexpr va_t RelayVA = 0x401000;
  static constexpr va_t LeafVA = RelayVA + 0x40;
  BinaryImage Image;

  CleanupRelayImage(bool Wide = false, int32_t ObjectOffset = -24) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = RelayVA;
    Segment Text;
    Text.Name = ".text";
    Text.VA = RelayVA;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x80, 0xcc);
    Text.Data[0] = 0x8d;
    Text.Data[1] = Wide ? 0x8d : 0x4d;
    const unsigned Jump = Wide ? 6 : 3;
    if (Wide)
      writeLE<int32_t>(Text.Data.data() + 2, ObjectOffset);
    else
      Text.Data[2] = uint8_t(ObjectOffset);
    Text.Data[Jump] = 0xe9;
    writeLE<uint32_t>(Text.Data.data() + Jump + 1,
                      uint32_t(LeafVA - (RelayVA + Jump + 5)));
    // A returning thiscall leaf reading exactly the borrowed scalar.
    Text.Data[0x40] = 0x8b;
    Text.Data[0x41] = 0x01;
    Text.Data[0x42] = 0xc3;
    Text.Size = Text.FileSz = Text.Data.size();
    Image.Segments.push_back(Text);
    Section Section;
    Section.Name = Text.Name;
    Section.VA = Text.VA;
    Section.Size = Section.FileSz = Text.Size;
    Section.Flags = Text.Flags;
    Image.Sections.push_back(Section);
  }
};
} // namespace

TEST(RegistrationCallABI, ChecksMSVCParentFrameCleanupRelays) {
  for (bool Wide : {false, true})
    for (int32_t Offset : {0, -24, 100}) {
      SCOPED_TRACE(Wide);
      SCOPED_TRACE(Offset);
      CleanupRelayImage F(Wide, Offset);
      const auto P = getCheckedX86RegistrationCleanupRelayABI(
          F.Image, CleanupRelayImage::RelayVA);
      ASSERT_TRUE(P);
      EXPECT_EQ(P->Target, CleanupRelayImage::RelayVA);
      EXPECT_EQ(P->EndAddress, CleanupRelayImage::RelayVA + (Wide ? 11 : 8));
      EXPECT_EQ(P->Calls[0].ObjectFrameOffset, Offset);
      EXPECT_EQ(P->Calls[0].Leaf.Target, CleanupRelayImage::LeafVA);
      EXPECT_EQ(P->Calls[0].Leaf.StackPopBytes, 0u);
      ASSERT_EQ(P->Calls[0].Leaf.ECXReads.size(), 1u);
      EXPECT_EQ(P->Calls[0].Leaf.ECXReads[0].Begin, 0);
      EXPECT_EQ(P->Calls[0].Leaf.ECXReads[0].End, 4);
      EXPECT_TRUE(P->Calls[0].Leaf.ECXWrites.empty());
    }
}

TEST(RegistrationCallABI, RejectsUnprovedCleanupRelayStorageAndEffects) {
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CleanupRelayImage F;
    auto &Text = F.Image.Segments[0];
    switch (Mutation) {
    case 0:
      Text.Flags = Text.Flags | SegmentFlags::Writable;
      break;
    case 1:
      F.Image.Sections.clear();
      break;
    case 2:
      F.Image.Sections.push_back(F.Image.Sections.front());
      break;
    case 3:
      F.Image.Segments.push_back(Text);
      break;
    case 4:
      Text.FileSz = 7;
      break;
    case 5:
      Text.Data[0] = 0x66;
      break;
    case 6:
      Text.Data[1] = 0x45; // EAX does not carry the thiscall object.
      break;
    case 7:
      Text.Data[3] = 0xe8; // CALL changes the invocation frame.
      break;
    case 8:
      Text.Data[3] = 0x90;
      break;
    case 9:
      F.Image.BaseRelocations.push_back({CleanupRelayImage::RelayVA + 4, 3});
      break;
    case 10:
      Text.Data[0x42] = 0xc2;
      Text.Data[0x43] = 4;
      Text.Data[0x44] = 0;
      break;
    case 11:
      Text.Data[0x40] = 0x64; // An FS-dependent leaf is not frame-private.
      Text.Data[0x41] = 0xa1;
      std::fill(Text.Data.begin() + 0x42, Text.Data.begin() + 0x46, 0);
      Text.Data[0x46] = 0xc3;
      break;
    case 12:
      F.Image.IsRelocatable = true;
      break;
    case 13:
      F.Image.Arch = Arch::X64;
      break;
    case 14:
      F.Image.Format = BinaryFormat::ELF;
      break;
    }
    EXPECT_FALSE(getCheckedX86RegistrationCleanupRelayABI(
        F.Image, CleanupRelayImage::RelayVA));
  }
  CleanupRelayImage F;
  size_t Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(getCheckedX86RegistrationCleanupRelayABI(
      F.Image, CleanupRelayImage::RelayVA, &Work));
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationCallABI, UsesPE32RelativeBranchWrapWithoutWrappingStorage) {
  CleanupRelayImage F;
  auto Relay = F.Image.Segments.front();
  auto Section = F.Image.Sections.front();
  Relay.VA = Section.VA = uint64_t(UINT32_MAX) + 1 - 8;
  Relay.Data.resize(8);
  Relay.Size = Relay.FileSz = Section.Size = Section.FileSz = 8;
  writeLE<uint32_t>(Relay.Data.data() + 4, CleanupRelayImage::LeafVA);
  F.Image.Segments.push_back(Relay);
  F.Image.Sections.push_back(Section);
  size_t Work = 0;
  const auto P =
      getCheckedX86RegistrationCleanupRelayABI(F.Image, Relay.VA, &Work);
  ASSERT_TRUE(P);
  EXPECT_EQ(P->EndAddress, uint64_t(UINT32_MAX) + 1);
  EXPECT_EQ(P->Calls[0].Leaf.Target, CleanupRelayImage::LeafVA);
  EXPECT_GT(Work, 8u);
  auto &Last = F.Image.Segments.back();
  ++Last.VA;
  ++F.Image.Sections.back().VA;
  const size_t EarlierWork = Work;
  EXPECT_FALSE(
      getCheckedX86RegistrationCleanupRelayABI(F.Image, Last.VA, &Work));
  EXPECT_GT(Work, EarlierWork);
}

TEST(RegistrationCallABI, MemoizesCleanupRelaysWithoutGrantingAFrameBorrow) {
  CleanupRelayImage F;
  LowFunc Parent;
  auto &EH = Parent.ExceptionMetadata.emplace();
  auto &Cxx = EH.Cxx.emplace();
  Cxx.UnwindMap = {
      {-1, 0, CxxUnwindAction::ActionKind::None},
      {0, CleanupRelayImage::RelayVA, CxxUnwindAction::ActionKind::Direct}};
  RegistrationCallCalleeIndex Index(F.Image);
  const auto Contracts = Index.cleanupContracts(Parent);
  ASSERT_TRUE(Contracts);
  ASSERT_EQ(Contracts->size(), 1u);
  const auto &C = Contracts->front();
  EXPECT_EQ(C.ActionState, 1u);
  EXPECT_EQ(C.RelayTarget, CleanupRelayImage::RelayVA);
  EXPECT_EQ(C.Calls[0].ObjectFrameOffset, -24);
  EXPECT_EQ(C.Calls[0].Leaf.Target, CleanupRelayImage::LeafVA);
  EXPECT_EQ(C.Calls[0].Leaf.ECXReads,
            (std::vector<RegistrationObjectExtent>{{0, 4}}));
  EXPECT_EQ(Index.cleanupContracts(Parent)->size(), 1u);
  F.Image.Segments[0].Data[0] = 0x90;
  RegistrationCallCalleeIndex NextImage(F.Image);
  ASSERT_TRUE(NextImage.cleanupContracts(Parent));
  EXPECT_TRUE(NextImage.cleanupContracts(Parent)->empty());
}

TEST(RegistrationCallABI, CleanupCacheUsesTheBoundedUnwindGraph) {
  for (unsigned Count : {257u, 889u, limits::kMaxRegistrationEHRecords}) {
    SCOPED_TRACE(Count);
    CleanupRelayImage F;
    auto &Text = F.Image.Segments[0];
    Text.Data.resize(0x100 + Count * 16, 0xcc);
    Text.Size = Text.FileSz = Text.Data.size();
    F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = Text.Size;
    LowFunc Parent;
    auto &Cxx = Parent.ExceptionMetadata.emplace().Cxx.emplace();
    Cxx.MaxState = Count;
    for (unsigned I = 0; I != Count; ++I) {
      const unsigned Offset = 0x100 + I * 16;
      const va_t Entry = Text.VA + Offset;
      Text.Data[Offset] = 0x8d; // lea ecx, [ebp + object]
      Text.Data[Offset + 1] = 0x8d;
      writeLE<int32_t>(Text.Data.data() + Offset + 2, -24 - 4 * int32_t(I));
      Text.Data[Offset + 6] = 0xe9;
      writeLE<uint32_t>(Text.Data.data() + Offset + 7,
                        CleanupRelayImage::LeafVA - (Entry + 11));
      Cxx.UnwindMap.push_back({-1, Entry, CxxUnwindAction::ActionKind::Direct});
    }
    size_t Work = 0;
    RegistrationCallCalleeIndex Index(F.Image, &Work);
    const auto Contracts = Index.cleanupContracts(Parent);
    ASSERT_TRUE(Contracts);
    ASSERT_EQ(Contracts->size(), Count);
    for (unsigned I = 0; I != Count; ++I) {
      const auto &C = (*Contracts)[I];
      EXPECT_EQ(C.ActionState, I);
      EXPECT_EQ(C.RelayTarget, Text.VA + 0x100 + I * 16);
      ASSERT_EQ(C.Calls.size(), 1u);
      EXPECT_EQ(C.Calls[0].ObjectFrameOffset, -24 - 4 * int32_t(I));
      EXPECT_EQ(C.Calls[0].Leaf.Target, CleanupRelayImage::LeafVA);
      EXPECT_EQ(C.Calls[0].Leaf.ECXReads,
                (std::vector<RegistrationObjectExtent>{{0, 4}}));
    }
    const size_t FirstWork = Work;
    const auto Again = Index.cleanupContracts(Parent);
    ASSERT_TRUE(Again);
    EXPECT_EQ(Again->size(), Count);
    EXPECT_LT(Work - FirstWork, FirstWork);
    EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);

    // A fresh current-image query rejects an altered relay even beyond the
    // old cache boundary, while preserving unrelated exact contracts.
    Text.Data[0x100 + (Count - 1) * 16] = 0x90;
    RegistrationCallCalleeIndex Changed(F.Image);
    const auto Partial = Changed.cleanupContracts(Parent);
    ASSERT_TRUE(Partial);
    ASSERT_EQ(Partial->size(), Count - 1);
    EXPECT_EQ(Partial->back().ActionState, Count - 2);

    size_t Exhausted = limits::kMaxRegistrationEHStateWork - 1;
    RegistrationCallCalleeIndex Limited(F.Image, &Exhausted);
    EXPECT_FALSE(Limited.cleanupContracts(Parent));
    EXPECT_EQ(Exhausted, limits::kMaxRegistrationEHStateWork);
    Cxx.UnwindMap.resize(limits::kMaxRegistrationEHRecords + 1);
    EXPECT_FALSE(Index.cleanupContracts(Parent));
  }
}

namespace {
struct CxxMetadataImage {
  static constexpr va_t TableVA = 0x403000;
  CleanupRelayImage Code;
  ExceptionFunction EH;

  CxxMetadataImage() {
    Segment Table;
    Table.VA = TableVA;
    Table.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Table.Data.resize(0x100);
    Table.Size = Table.FileSz = Table.Data.size();
    auto Word = [&](size_t Offset, uint32_t Value) {
      writeLE<uint32_t>(Table.Data.data() + Offset, Value);
    };
    Word(0, 0x19930522);
    Word(4, 2);
    Word(8, TableVA + 0x30);
    Word(12, 1);
    Word(16, TableVA + 0x50);
    Word(32, 1);
    Word(0x30, uint32_t(-1));
    Word(0x38, 0);
    Word(0x3c, CleanupRelayImage::RelayVA);
    Word(0x58, 1);
    Word(0x5c, 1);
    Word(0x60, TableVA + 0x70);
    Word(0x7c, CleanupRelayImage::LeafVA);
    Code.Image.Segments.push_back(Table);
    EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
    EH.Personality = ExceptionPersonality::CxxFrameHandler3;
    EH.HandlerDataVA = TableVA;
    EH.Registration.emplace().ScopeTableVA = TableVA;
    auto &Cxx = EH.Cxx.emplace();
    Cxx.NativeFuncInfoVA = TableVA;
    Cxx.Magic = 0x19930522;
    Cxx.Version = CxxFuncInfoVersion::WithEHFlags;
    Cxx.MaxState = 2;
    Cxx.Flags = 1;
    Cxx.IsSynchronous = true;
    Cxx.UnwindMap = {
        {-1, 0, CxxUnwindAction::ActionKind::None},
        {0, CleanupRelayImage::RelayVA, CxxUnwindAction::ActionKind::Direct}};
    CxxTryBlock Try;
    Try.TryLow = Try.TryHigh = 0;
    Try.CatchHigh = 1;
    CxxCatchHandler Catch;
    Catch.HandlerVA = CleanupRelayImage::LeafVA;
    Try.Handlers.push_back(Catch);
    Cxx.TryBlocks.push_back(Try);
  }

  void tableWord(size_t Offset, uint32_t Value) {
    writeLE<uint32_t>(Code.Image.Segments.back().Data.data() + Offset, Value);
  }
};
} // namespace

TEST(RegistrationCallABI, CxxMetadataExtentsBindTheReparsedSourceGraph) {
  CxxMetadataImage F;
  const auto Records = coff_loader::getCheckedX86CxxFuncInfoRecords(
      F.Code.Image, F.EH.Cxx->NativeFuncInfoVA);
  ASSERT_TRUE(Records);
  EXPECT_EQ(Records->Cxx, *F.EH.Cxx);
  EXPECT_EQ(Records->CallbackPointerSources.size(), 2u);
  const auto Ranges =
      coff_loader::getCheckedX86CxxMetadataRanges(F.Code.Image, F.EH);
  ASSERT_TRUE(Ranges);
  ASSERT_EQ(Ranges->size(), 4u);
  const std::pair<uint32_t, uint32_t> Expected[] = {
      {0, 36}, {0x30, 0x40}, {0x50, 0x64}, {0x70, 0x80}};
  for (size_t I = 0; I < Ranges->size(); ++I) {
    EXPECT_EQ((*Ranges)[I].Begin, F.TableVA + Expected[I].first);
    EXPECT_EQ((*Ranges)[I].End, F.TableVA + Expected[I].second);
  }
  F.tableWord(0x58, 0);
  const auto Changed = coff_loader::getCheckedX86CxxFuncInfoRecords(
      F.Code.Image, F.EH.Cxx->NativeFuncInfoVA);
  EXPECT_FALSE(Changed);
  EXPECT_FALSE(coff_loader::getCheckedX86CxxMetadataRanges(F.Code.Image, F.EH));
  EXPECT_FALSE(
      coff_loader::getCheckedX86CxxCallbackPointerSources(F.Code.Image, F.EH));
}

namespace {
struct CxxPersonalityImage {
  CxxMetadataImage Source;
  static constexpr va_t HandlerVA = CleanupRelayImage::RelayVA + 0x50;
  static constexpr va_t RuntimeVA = CleanupRelayImage::RelayVA + 0x70;
  static constexpr va_t IATVA = CxxMetadataImage::TableVA + 0x90;
  CxxPersonalityImage(bool Direct = false) {
    auto &Image = Source.Code.Image;
    auto &Bytes = Image.Segments[0].Data;
    Bytes[0x50] = Bytes[0x51] = 0x90;
    Bytes[0x52] = 0xb8;
    writeLE<uint32_t>(Bytes.data() + 0x53, Source.TableVA);
    if (Direct) {
      Bytes[0x57] = 0xff;
      Bytes[0x58] = 0x25;
      writeLE<uint32_t>(Bytes.data() + 0x59, IATVA);
    } else {
      Bytes[0x57] = 0xe9;
      writeLE<uint32_t>(Bytes.data() + 0x58, RuntimeVA - (HandlerVA + 12));
    }
    Bytes[0x70] = 0xff;
    Bytes[0x71] = 0x25;
    writeLE<uint32_t>(Bytes.data() + 0x72, IATVA);
    Image.BaseRelocations = {
        {HandlerVA + 3, llvm::COFF::IMAGE_REL_BASED_HIGHLOW},
        {Direct ? HandlerVA + 9 : RuntimeVA + 2,
         llvm::COFF::IMAGE_REL_BASED_HIGHLOW}};
    Import Import;
    Import.Name = "__CxxFrameHandler3";
    Import.Module = "VCRUNTIME140.dll";
    Import.IATAddr = IATVA;
    Image.Imports.push_back(Import);
    Source.EH.PersonalityVA = HandlerVA;
    Source.EH.Registration->HandlerVA = HandlerVA;
  }
};
} // namespace

TEST(RegistrationCallABI, CxxPersonalitySkipsTheOriginalFuncInfoOperand) {
  for (bool Direct : {false, true}) {
    CxxPersonalityImage F(Direct);
    const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(
        F.Source.Code.Image, F.Source.EH);
    ASSERT_TRUE(Handler);
    EXPECT_TRUE(Handler->CookieGuards.empty());
    EXPECT_EQ(Handler->SecurityCookieVA, 0u);
    const auto Runtime = coff_loader::getCheckedX86CxxPersonalityABI(
        F.Source.Code.Image, F.Source.EH);
    ASSERT_TRUE(Runtime);
    EXPECT_EQ(Runtime->RuntimeVA, Direct ? F.HandlerVA + 7 : F.RuntimeVA);
    EXPECT_EQ(Runtime->IATVA, F.IATVA);
    EXPECT_NE(Runtime->RuntimeVA, F.HandlerVA);
    EXPECT_FALSE(Runtime->CodeRanges.empty());
    EXPECT_TRUE(coff_loader::isCheckedX86CxxHandlerReference(
        F.Source.Code.Image, F.HandlerVA));
  }
}

namespace {
struct CxxGuardCase {
  std::vector<uint8_t> Prefix;
  std::vector<unsigned> Calls;
  std::vector<std::pair<int32_t, int32_t>> Offsets;
};

const CxxGuardCase CxxGuardCases[] = {
    // One node-relative cookie; EBP and cookie+4 are distinct XOR coordinates.
    {{0x8b, 0x54, 0x24, 8, 0x8d, 0x42, 12, 0x8b, 0x4a, 0xfc, 0x33, 0xc8, 0xe8,
      0, 0, 0, 0},
     {12},
     {{-4, 12}}},
    {{0x8b, 0x54, 0x24, 8, 0x8d, 0x42, 0xd4, 0x8b, 0x4a, 0xd0, 0x33, 0xc8, 0xe8,
      0, 0, 0, 0},
     {12},
     {{-48, -44}}},
    // Each displacement can independently use the wide encoding.
    {{0x8b, 0x54, 0x24, 8,    0x8d, 0x42, 12, 0x8b, 0x8a, 0,
      0xff, 0xff, 0xff, 0x33, 0xc8, 0xe8, 0,  0,    0,    0},
     {15},
     {{-256, 12}}},
    {{0x8b, 0x54, 0x24, 8,    0x8d, 0x82, 4, 0xff, 0xff, 0xff,
      0x8b, 0x4a, 0xfc, 0x33, 0xc8, 0xe8, 0, 0,    0,    0},
     {15},
     {{-4, -252}}},
    {{0x8b, 0x54, 0x24, 8,    0x8d, 0x82, 4,    0xff, 0xff, 0xff, 0x8b, 0x8a,
      0,    0xff, 0xff, 0xff, 0x33, 0xc8, 0xe8, 0,    0,    0,    0},
     {18},
     {{-256, -252}}},
    // Two checks can reuse EAX or add twelve before the second comparison.
    {{0x8b, 0x54, 0x24, 8,    0x8d, 0x42, 12, 0x8b, 0x4a,
      0xec, 0x33, 0xc8, 0xe8, 0,    0,    0,  0,    0x8b,
      0x4a, 0xf8, 0x33, 0xc8, 0xe8, 0,    0,  0,    0},
     {12, 22},
     {{-20, 12}, {-8, 12}}},
    {{0x8b, 0x54, 0x24, 8,    0x8d, 0x42, 0, 0x8b, 0x4a, 0xfc,
      0x33, 0xc8, 0xe8, 0,    0,    0,    0, 0x83, 0xc0, 12,
      0x8b, 0x4a, 0xf8, 0x33, 0xc8, 0xe8, 0, 0,    0,    0},
     {12, 25},
     {{-4, 0}, {-8, 12}}},
    {{0x8b, 0x54, 0x24, 8,    0x8d, 0x42, 12, 0x8b, 0x8a, 0,
      0xff, 0xff, 0xff, 0x33, 0xc8, 0xe8, 0,  0,    0,    0,
      0x8b, 0x4a, 0xf8, 0x33, 0xc8, 0xe8, 0,  0,    0,    0},
     {15, 25},
     {{-256, 12}, {-8, 12}}},
    {{0x8b, 0x54, 0x24, 8,    0x8d, 0x82, 0,    0xff, 0xff, 0xff, 0x8b, 0x8a,
      0,    0xff, 0xff, 0xff, 0x33, 0xc8, 0xe8, 0,    0,    0,    0,    0x83,
      0xc0, 12,   0x8b, 0x4a, 0xf8, 0x33, 0xc8, 0xe8, 0,    0,    0,    0},
     {18, 31},
     {{-256, -256}, {-8, -244}}},
};

struct CxxGuardImage {
  CxxPersonalityImage Source;
  static constexpr va_t HandlerVA = 0x401100;
  static constexpr va_t CheckVA = 0x401200;
  static constexpr va_t CookieVA = 0x4030a0;
  va_t BodyVA;

  CxxGuardImage(const CxxGuardCase &Case, bool Direct = false,
                unsigned Padding = 0, bool SameChecker = true) {
    auto &Image = Source.Source.Code.Image;
    auto &EH = Source.Source.EH;
    auto &Text = Image.Segments.front();
    Text.Data.resize(0x300, 0xcc);
    Text.Size = Text.FileSz = Text.Data.size();
    Image.Sections.front().Size = Image.Sections.front().FileSz = Text.Size;
    Image.DynInfo.SecurityCookieRVA = CookieVA - Image.Base;
    Image.Imports.front().Module = "MSVCR100.dll";
    EH.PersonalityVA = EH.Registration->HandlerVA = HandlerVA;
    Image.BaseRelocations.clear();
    Image.BaseRelocations.push_back(
        {Source.RuntimeVA + 2, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    auto *Bytes = Text.Data.data() + HandlerVA - Text.VA;
    std::fill(Bytes, Bytes + Padding, 0x90);
    std::copy(Case.Prefix.begin(), Case.Prefix.end(), Bytes + Padding);
    for (size_t I = 0; I < Case.Calls.size(); ++I) {
      const va_t Check = CheckVA + (SameChecker ? 0 : I * 0x20);
      const va_t Call = HandlerVA + Padding + Case.Calls[I];
      writeLE<uint32_t>(Bytes + Padding + Case.Calls[I] + 1,
                        Check - (Call + 5));
    }
    BodyVA = HandlerVA + Padding + Case.Prefix.size();
    auto *Body = Text.Data.data() + BodyVA - Text.VA;
    Body[0] = 0xb8;
    writeLE<uint32_t>(Body + 1, Source.Source.TableVA);
    Image.BaseRelocations.push_back(
        {BodyVA + 1, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    if (Direct) {
      Body[5] = 0xff;
      Body[6] = 0x25;
      writeLE<uint32_t>(Body + 7, Source.IATVA);
      Image.BaseRelocations.push_back(
          {BodyVA + 7, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    } else {
      Body[5] = 0xe9;
      writeLE<uint32_t>(Body + 6, Source.RuntimeVA - (BodyVA + 10));
    }
    const uint8_t CheckBytes[] = {0x3b, 0x0d, 0xa0, 0x30, 0x40,
                                  0,    0x75, 0x10, 0xc3};
    for (unsigned Offset : {0u, 0x20u}) {
      std::copy(std::begin(CheckBytes), std::end(CheckBytes),
                Text.Data.begin() + CheckVA + Offset - Text.VA);
      Image.BaseRelocations.push_back(
          {CheckVA + Offset + 2, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    }
  }
};
} // namespace

TEST(RegistrationCallABI, CxxHandlerRetainsEveryCookiePrecondition) {
  for (const auto &Case : CxxGuardCases)
    for (bool Direct : {false, true})
      for (unsigned Padding : {0u, 2u, 8u})
        for (bool SameChecker : {false, true}) {
          SCOPED_TRACE(Case.Prefix.size());
          SCOPED_TRACE(Direct);
          SCOPED_TRACE(Padding);
          SCOPED_TRACE(SameChecker);
          CxxGuardImage F(Case, Direct, Padding, SameChecker);
          auto &Image = F.Source.Source.Code.Image;
          auto &EH = F.Source.Source.EH;
          const auto Handler =
              coff_loader::getCheckedX86CxxHandlerABI(Image, EH);
          ASSERT_TRUE(Handler);
          EXPECT_EQ(Handler->SecurityCookieVA, F.CookieVA);
          EXPECT_EQ(Handler->Dispatch.RuntimeVA,
                    Direct ? F.BodyVA + 5 : F.Source.RuntimeVA);
          EXPECT_EQ(Handler->Dispatch.IATVA, F.Source.IATVA);
          ASSERT_EQ(Handler->CookieGuards.size(), Case.Offsets.size());
          for (size_t I = 0; I < Case.Offsets.size(); ++I) {
            const auto &Guard = Handler->CookieGuards[I];
            EXPECT_EQ(Guard.CookieOffset, Case.Offsets[I].first);
            EXPECT_EQ(Guard.XOROffset, Case.Offsets[I].second);
            EXPECT_EQ(Guard.CallVA, F.HandlerVA + Padding + Case.Calls[I]);
            EXPECT_EQ(Guard.CheckVA, F.CheckVA + (SameChecker ? 0 : I * 0x20));
            EXPECT_EQ(Guard.FailureVA, Guard.CheckVA + 24);
            EXPECT_TRUE(std::any_of(Handler->Dispatch.CodeRanges.begin(),
                                    Handler->Dispatch.CodeRanges.end(),
                                    [&](const auto &Range) {
                                      return Range.Begin == Guard.CheckVA &&
                                             Range.End == Guard.CheckVA + 9;
                                    }));
          }
          // No parent frame has been proved. The unguarded native-dispatch
          // API must still reject this otherwise authenticated handler.
          EXPECT_FALSE(EH.Registration->RegistrationOffset);
          EXPECT_FALSE(coff_loader::getCheckedX86CxxPersonalityABI(Image, EH));
          EXPECT_FALSE(
              coff_loader::isCheckedX86CxxHandlerReference(Image, F.HandlerVA));
        }
}

TEST(RegistrationCallABI, CxxCookieSourceProofReachesTheParentAndMedCallABI) {
  auto Case = CxxGuardCases[0];
  Case.Prefix[9] = 0xe8; // cookie at node-24 == EBP-36
  CxxGuardImage F(Case);
  auto &Image = F.Source.Source.Code.Image;
  auto &Text = Image.Segments.front();
  const va_t Entry = Text.VA, LeafVA = Text.VA + 0x90;
  std::vector<uint8_t> Body = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
  auto Word = [&](uint32_t Value) {
    const size_t Offset = Body.size();
    Body.resize(Offset + 4);
    writeLE<uint32_t>(Body.data() + Offset, Value);
  };
  auto Address = [&](va_t Value) {
    Image.BaseRelocations.push_back(
        {Entry + Body.size(), llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    recordAbsolutePointerRelocation(Image, Entry + Body.size(), Value);
    Word(uint32_t(Value));
  };
  Address(F.HandlerVA);
  Body.insert(Body.end(), {0x64, 0xa1, 0, 0, 0, 0, 0x50, 0x83, 0xec, 8, 0x53,
                           0x56, 0x57, 0xa1});
  Address(F.CookieVA);
  Body.insert(Body.end(),
              {0x33, 0xc5, 0x50, 0x8d, 0x45, 0xf4, 0x64, 0xa3, 0, 0,
               0,    0,    0xc7, 0x45, 0xfc, 0,    0,    0,    0, 0xe8});
  Word(uint32_t(LeafVA - (Entry + Body.size() + 4)));
  // Unlink before popping the cookie. Its source storage is still live
  // throughout every call at which the C++ handler can be dispatched.
  Body.insert(Body.end(),
              {0xc7, 0x45, 0xfc, 0xff, 0xff, 0xff, 0xff, 0x8b, 0x4d, 0xf4, 0x64,
               0x89, 0x0d, 0,    0,    0,    0,    0x59, 0x33, 0xcd, 0xe8});
  Word(uint32_t(F.CheckVA - (Entry + Body.size() + 4)));
  Body.insert(Body.end(), {0x5f, 0x5e, 0x5b, 0x8b, 0xe5, 0x5d, 0xc3});
  ASSERT_LT(Body.size(), 0x70u);
  std::fill(Text.Data.begin(), Text.Data.begin() + 0x70, 0xcc);
  std::copy(Body.begin(), Body.end(), Text.Data.begin());
  const uint8_t Leaf[] = {0xb8, 37, 0, 0, 0, 0xc3};
  std::copy(std::begin(Leaf), std::end(Leaf),
            Text.Data.begin() + LeafVA - Text.VA);
  auto &LeafExport = Image.Exports.emplace_back();
  LeafExport.Name = "cookie_leaf";
  LeafExport.Addr = LeafVA;
  // One state with no cleanup or catch, retained in actual FuncInfo bytes.
  F.Source.Source.tableWord(4, 1);
  F.Source.Source.tableWord(12, 0);
  F.Source.Source.tableWord(16, 0);
  Image.ExceptionMetadata = {};
  coff_loader::parseX86RegistrationExceptions(Image);
  const auto *EH = Image.ExceptionMetadata.findFunction(Entry);
  ASSERT_TRUE(EH && EH->Cxx && EH->Registration);
  ASSERT_EQ(EH->Registration->RegistrationOffset, -12);
  ASSERT_TRUE(coff_loader::getCheckedX86CxxHandlerABI(Image, *EH));
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  const auto Low = CFGBuilder().build(Image, Decode, Entry, "cookie_parent");
  ASSERT_TRUE(Low.RegistrationStates);
  const auto &States = *Low.RegistrationStates;
  for (const auto &Diagnostic : States.Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  ASSERT_TRUE(States.SecurityCookiesComplete);
  EXPECT_EQ(States.CxxCookieConditions,
            (std::vector<RegistrationCxxCookieCondition>{{-36, 0}}));
  ASSERT_EQ(States.CookieChecks.size(), 1u);
  const auto &Check = States.CookieChecks.front();
  EXPECT_FALSE(States.callFrameEffect(Check.Address, Check.OpSeq));
  ASSERT_EQ(getCheckedX86RegistrationCxxParentABI(Low, Image), 0);
  EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Entry), 0u);
  size_t CxxWork = 0;
  const auto Cxx =
      getCheckedX86RegistrationCxxCalleeABI(Image, Entry, &CxxWork);
  ASSERT_TRUE(Cxx);
  EXPECT_TRUE(Cxx->isCxxParent());
  EXPECT_TRUE(Cxx->HasIndependentScalarReturn);
  EXPECT_EQ(Cxx->StackPopBytes, 0u);
  ASSERT_FALSE(Cxx->RequiredImports.empty());
  EXPECT_TRUE(Cxx->CallerPCWrites.empty());
  const size_t CxxUsed = CxxWork;
  {
    RegistrationStackPopMemo Memo(Image);
    RegistrationStackPopMemoScope Scope(Memo);
    const unsigned Level = x86SEHStateAnalysisLevel();
    for (unsigned Query = 0; Query != 2; ++Query) {
      size_t Used = 0;
      const auto Again =
          getCheckedX86RegistrationCxxCalleeABI(Image, Entry, &Used);
      ASSERT_TRUE(Again);
      EXPECT_EQ(Used, CxxUsed);
      size_t MatchWork = 0;
      EXPECT_TRUE(hasSameX86RegistrationCxxEffects(*Again, *Cxx, MatchWork));
      if (Query == 0) {
        // A nested query must neither publish nor consume an answer keyed
        // without its C++ proof ancestry. An independent stack query may.
        EXPECT_FALSE(Memo.find(Image, LeafVA, Level));
        RegistrationCallCalleeIndex Index(Image);
        EXPECT_EQ(Index.stackPop(Entry), 0u);
        EXPECT_FALSE(Memo.find(Image, LeafVA, Level));
        EXPECT_EQ(getCheckedX86CalleeStackPop(Image, LeafVA), 0u);
        EXPECT_TRUE(Memo.find(Image, LeafVA, Level));
        EXPECT_EQ(getCheckedX86CalleeStackPop(Image, Entry), 0u);
      }
    }
  }
  CxxWork = limits::kMaxRegistrationEHStateWork - CxxUsed;
  EXPECT_TRUE(getCheckedX86RegistrationCxxCalleeABI(Image, Entry, &CxxWork));
  EXPECT_EQ(CxxWork, limits::kMaxRegistrationEHStateWork);
  CxxWork = limits::kMaxRegistrationEHStateWork - CxxUsed + 1;
  EXPECT_FALSE(getCheckedX86RegistrationCxxCalleeABI(Image, Entry, &CxxWork));
  EXPECT_EQ(CxxWork, limits::kMaxRegistrationEHStateWork);
  auto ReturnedFrame = Image;
  const uint8_t FrameResult[] = {0x8b, 0xc5, 0xc3};
  std::copy(std::begin(FrameResult), std::end(FrameResult),
            ReturnedFrame.Segments.front().Data.begin() + LeafVA - Text.VA);
  const auto Aliased =
      getCheckedX86RegistrationCxxCalleeABI(ReturnedFrame, Entry);
  ASSERT_TRUE(Aliased);
  EXPECT_FALSE(Aliased->HasIndependentScalarReturn);
  auto Recursive = Image;
  auto *Child = Recursive.Segments.front().Data.data() + LeafVA - Text.VA;
  Child[0] = 0xe8;
  writeLE<uint32_t>(Child + 1, uint32_t(Entry - (LeafVA + 5)));
  Child[5] = 0xc3;
  size_t RecursiveWork = 0;
  EXPECT_FALSE(
      getCheckedX86RegistrationCxxCalleeABI(Recursive, Entry, &RecursiveWork));
  EXPECT_LT(RecursiveWork, limits::kMaxRegistrationEHStateWork);
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Forged = *Cxx;
    switch (Mutation) {
    case 0:
      Forged.RequiredImports.clear();
      break;
    case 1:
      Forged.HasIndependentScalarReturn = false;
      break;
    case 2:
      Forged.ImageWrites.push_back({0x404000, 0x404004});
      break;
    case 3:
      Forged.CalleeKind = RegistrationCalleeFrameContract::Kind::Leaf;
      break;
    }
    size_t MatchWork = 0;
    EXPECT_FALSE(hasSameX86RegistrationCxxEffects(Forged, *Cxx, MatchWork));
  }
  auto Unbalanced = Image;
  ASSERT_EQ(Body[Body.size() - 2], 0x5d);
  Unbalanced.Segments.front().Data[Body.size() - 2] = 0x90;
  EXPECT_FALSE(getCheckedX86CalleeStackPop(Unbalanced, Entry));
  EXPECT_FALSE(getCheckedX86RegistrationCxxCalleeABI(Unbalanced, Entry));
  EXPECT_FALSE(coff_loader::getCheckedX86CxxPersonalityABI(Image, *EH));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  recoverCallAbi(Med, Arch::X86, {}, &Image);
  unsigned Matches = 0;
  for (const auto &Block : Med.Blocks)
    for (size_t I = 0; I != Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      if (Op.Addr != Check.Address || Op.OriginSeq != Check.OpSeq)
        continue;
      ++Matches;
      const auto *Call = Med.findCall(Block.Id, I);
      ASSERT_TRUE(Call);
      EXPECT_EQ(Call->Args.size(), 1u);
      EXPECT_EQ(Call->IntegerRegisterArgCount, 1);
      EXPECT_EQ(Call->FloatingRegisterArgCount, 0);
      EXPECT_TRUE(Op.PreservesCallerSaved);
      EXPECT_EQ(Op.Output.Id, -1);
      EXPECT_EQ(Op.Output.Size, 0);
    }
  EXPECT_EQ(Matches, 1u);
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Low;
    auto ChangedImage = Image;
    auto &Proof = *Changed.RegistrationStates;
    switch (Mutation) {
    case 0:
      Proof.SecurityCookiesComplete = false;
      break;
    case 1:
      ++Proof.SecurityCookieVA;
      break;
    case 2:
      ++Proof.CookieChecks.front().EndAddress;
      break;
    case 3:
      for (auto &Block : Changed.Blocks)
        for (auto &Boundary : Block.InstructionBoundaries)
          if (Boundary.Address == Check.Address)
            Boundary.Control = LowInstructionControl::None;
      break;
    case 4:
      ChangedImage.Segments.front().Data[F.CheckVA - Text.VA] = 0x33;
      break;
    case 5:
      ChangedImage.DynInfo.SecurityCookieRVA += 4;
      break;
    case 6:
      ++ChangedImage.Segments.front().Data[Check.Address + 1 - Text.VA];
      break;
    case 7:
      ChangedImage.BaseRelocations.push_back(
          {Check.Address + 1, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      break;
    case 8:
      ChangedImage.Segments.front().Data[Check.Address - Text.VA] = 0x90;
      break;
    }
    EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Changed, ChangedImage));
  }
}

TEST(RegistrationCallABI, InputPE32CleanupParentRetainsThrowBinding) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_CLEANUP_ONLY_PE32");
  if (!Path)
    GTEST_SKIP() << "set the authored C++ cleanup image";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Export = llvm::find_if(Image->Exports, [](const auto &E) {
    return E.Name == "cleanup_only_parent";
  });
  ASSERT_NE(Export, Image->Exports.end());
  const va_t Entry = Export->Addr;
  const auto *EH = Image->ExceptionMetadata.findFunction(Entry);
  ASSERT_TRUE(EH && EH->Cxx && EH->Registration);
  ASSERT_TRUE(EH->Cxx->TryBlocks.empty());
  if (EH->Registration->RealignedFrame) {
    // Realigned parent coordinates still need their separate stack owner.
    EXPECT_FALSE(getCheckedX86CalleeStackPop(*Image, Entry));
    EXPECT_FALSE(getCheckedX86RegistrationCxxCalleeABI(*Image, Entry));
    return;
  }
  const auto Frame = getCheckedX86RegistrationCxxCalleeABI(*Image, Entry);
  ASSERT_TRUE(Frame);
  EXPECT_TRUE(Frame->isCxxParent());
  EXPECT_TRUE(Frame->HasIndependentScalarReturn);
  EXPECT_FALSE(Frame->ImageReads.empty());
  EXPECT_FALSE(Frame->ImageWrites.empty());
  EXPECT_FALSE(Frame->RuntimeThrowInfos.empty());
  EXPECT_GE(Frame->RequiredImports.size(), 2u);
  const auto Metadata =
      coff_loader::getCheckedX86CxxMetadataRanges(*Image, *EH);
  ASSERT_TRUE(Metadata);
  auto Reads = [&](const ExceptionAddressRange &Expected) {
    return llvm::any_of(Frame->ImageReads, [&](const auto &Range) {
      return Range.Begin <= Expected.Begin && Expected.End <= Range.End;
    });
  };
  for (const auto &Range : *Metadata)
    EXPECT_TRUE(Reads(Range));
  for (va_t Slot : Frame->RequiredImports)
    EXPECT_TRUE(Reads({Slot, Slot + 4}));
  size_t Work = 0;
  EXPECT_EQ(getCheckedX86CalleeStackPop(*Image, Entry, &Work), 0u);
  ASSERT_GT(Work, 0u);
  ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
  const size_t Used = Work;
  Work = limits::kMaxRegistrationEHStateWork - Used;
  EXPECT_EQ(getCheckedX86CalleeStackPop(*Image, Entry, &Work), 0u);
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
  Work = limits::kMaxRegistrationEHStateWork - Used + 1;
  EXPECT_FALSE(getCheckedX86CalleeStackPop(*Image, Entry, &Work));
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);

  LowFunc Caller;
  Caller.Entry = Entry - 1;
  Caller.Blocks.resize(1);
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(Entry, 4));
  Caller.Blocks.front().Ops.push_back(Call);
  RegistrationCallCalleeIndex Index(*Image);
  const auto Contracts = Index.stackContracts(Caller);
  ASSERT_TRUE(Contracts);
  ASSERT_EQ(Contracts->size(), 1u);
  const auto &Contract = Contracts->front();
  EXPECT_EQ(Contract.Target, Entry);
  EXPECT_EQ(Contract.StackPopBytes, 0u);
  EXPECT_FALSE(Contract.PreservesImportBindings);
  const auto Throw = llvm::find_if(Image->Imports, [](const auto &I) {
    return I.Name == "_CxxThrowException";
  });
  ASSERT_NE(Throw, Image->Imports.end());
  EXPECT_NE(std::find(Contract.RequiredImports.begin(),
                      Contract.RequiredImports.end(), Throw->IATAddr),
            Contract.RequiredImports.end());

  for (bool ReplaceImport : {false, true}) {
    SCOPED_TRACE(ReplaceImport);
    auto Wrapped = *Image;
    va_t Address = 0;
    for (const auto &Segment : Wrapped.Segments)
      Address = std::max(Address, Segment.VA + Segment.Size);
    ASSERT_LE(Address, UINT32_MAX - 32);
    Address = (Address + 15) & ~va_t{15};
    Segment Wrapper;
    Wrapper.Name = ".wrapper";
    Wrapper.VA = Address;
    Wrapper.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    if (ReplaceImport) {
      // Even one changed byte revokes the nonreturning-path assumption.
      Wrapper.Data = {0xc6, 0x05, 0, 0, 0, 0, 0};
      writeLE<uint32_t>(Wrapper.Data.data() + 2, Throw->IATAddr + 3);
    }
    Wrapper.Data.push_back(0xe8);
    const size_t Displacement = Wrapper.Data.size();
    Wrapper.Data.resize(Displacement + 4);
    writeLE<uint32_t>(Wrapper.Data.data() + Displacement,
                      uint32_t(Entry - (Address + Displacement + 4)));
    Wrapper.Data.push_back(0xc3);
    Wrapper.Size = Wrapper.FileSz = Wrapper.Data.size();
    Section Code;
    Code.Name = Wrapper.Name;
    Code.VA = Wrapper.VA;
    Code.Size = Code.FileSz = Wrapper.Size;
    Code.Flags = Wrapper.Flags;
    Wrapped.Sections.push_back(Code);
    Wrapped.Symbols.push_back(Symbol::makeFunc(Address, Wrapper.Size));
    Wrapped.Segments.push_back(std::move(Wrapper));
    EXPECT_EQ(getCheckedX86CalleeStackPop(Wrapped, Address),
              ReplaceImport ? std::nullopt : std::optional<uint32_t>{0});
  }

  auto Changed = *Image;
  const auto ChangedThrow = llvm::find_if(Changed.Imports, [&](const auto &I) {
    return I.IATAddr == Throw->IATAddr;
  });
  ASSERT_NE(ChangedThrow, Changed.Imports.end());
  ChangedThrow->Name = "unknown_throw";
  EXPECT_FALSE(getCheckedX86CalleeStackPop(Changed, Entry));

  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  const auto Parent = CFGBuilder().build(*Image, Decode, Entry, "stack-parent");
  unsigned Returns = 0;
  for (const auto &Block : Parent.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries)
      if (Insn.Control == LowInstructionControl::Return) {
        ++Returns;
        Changed = *Image;
        for (auto &Segment : Changed.Segments)
          if (Segment.VA <= Insn.Address &&
              Insn.Address - Segment.VA < Segment.Data.size())
            Segment.Data[Insn.Address - Segment.VA] = 0xcb;
        EXPECT_FALSE(getCheckedX86CalleeStackPop(Changed, Entry));
      }
  EXPECT_GT(Returns, 0u);

  Changed = *Image;
  for (auto &Segment : Changed.Segments)
    if (Segment.VA <= Entry && Entry < Segment.VA + Segment.Size)
      Segment.Flags = Segment.Flags | SegmentFlags::Writable;
  EXPECT_FALSE(getCheckedX86CalleeStackPop(Changed, Entry));
}

TEST(RegistrationCallABI, CxxHandlerRejectsChangedCookieGuardSemantics) {
  for (unsigned Mutation = 0; Mutation != 24; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CxxGuardImage F(CxxGuardCases[6], false, 0, false);
    auto &Image = F.Source.Source.Code.Image;
    auto &Text = Image.Segments.front();
    auto *Prefix = Text.Data.data() + F.HandlerVA - Text.VA;
    auto *Check = Text.Data.data() + F.CheckVA - Text.VA;
    switch (Mutation) {
    case 0:
      Prefix[3] = 4;
      break; // wrong handler argument
    case 1:
      Prefix[1] = 0x4c;
      break; // destroys ECX instead of retaining EDX
    case 2:
      Prefix[5] = 0x45;
      break; // EBP is not the node
    case 3:
      Prefix[8] = 0x48;
      break; // reads through EAX
    case 4:
      Prefix[10] = 0x03;
      break; // ADD does not decode an XOR cookie
    case 5:
      Prefix[11] = 0xca;
      break; // XORs with EDX instead of EAX
    case 6:
      Prefix[12] = 0xe9;
      break; // tail call does not return to dispatch
    case 7:
      Prefix[18] = 0xc2;
      break; // modifies the retained node
    case 8:
      Prefix[19] = 8;
      break; // unmodeled rebase
    case 9:
      Prefix[21] = 0x49;
      break; // second load loses node identity
    case 10:
      Prefix[24] = 0xca;
      break; // second check uses the wrong base
    case 11:
      Prefix[25] = 0xe9;
      break;
    case 12:
      Check[0x20 + 6] = 0x74;
      break; // inverted second comparison
    case 13:
      Check[0x20 + 2] += 4;
      break; // second checker uses another cookie
    case 14:
      Check[8] = 0x5a;
      break; // success path destroys EDX
    case 15:
      Check[7] = 0;
      break; // mismatch also reaches RET
    case 16:
      Image.DynInfo.SecurityCookieRVA = 0;
      break;
    case 17:
      Image.DynInfo.SecurityCookieRVA = UINT32_MAX;
      break;
    case 18:
      Text.Flags = Text.Flags | SegmentFlags::Writable;
      break;
    case 19:
      Image.BaseRelocations.push_back(
          {F.HandlerVA + 13, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      break;
    case 20:
      Image.BaseRelocations.push_back(
          {F.CheckVA + 0x20 + 6, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      break;
    case 21:
      Image.Sections.front().FileSz = F.CheckVA - Text.VA + 8;
      break;
    case 22:
      Image.Imports.front().Module = "application.dll";
      break;
    case 23:
      Image.Segments.push_back(Text);
      break;
    }
    EXPECT_FALSE(
        coff_loader::getCheckedX86CxxHandlerABI(Image, F.Source.Source.EH));
    EXPECT_FALSE(
        coff_loader::getCheckedX86CxxPersonalityABI(Image, F.Source.Source.EH));
  }
  auto Extra = CxxGuardCases[0];
  Extra.Prefix.push_back(0x90);
  CxxGuardImage F(Extra);
  EXPECT_FALSE(coff_loader::getCheckedX86CxxHandlerABI(
      F.Source.Source.Code.Image, F.Source.Source.EH));
  Extra = CxxGuardCases[5];
  Extra.Prefix.insert(Extra.Prefix.end(),
                      {0x8b, 0x4a, 0xfc, 0x33, 0xc8, 0xe8, 0, 0, 0, 0});
  Extra.Calls.push_back(32);
  CxxGuardImage Three(Extra);
  EXPECT_FALSE(coff_loader::getCheckedX86CxxHandlerABI(
      Three.Source.Source.Code.Image, Three.Source.Source.EH));
}

TEST(RegistrationCallABI, CxxDispatchAndThrowShareTheKnownRuntimeABI) {
  const std::pair<const char *, bool> Cases[] = {
      {"VCRUNTIME140.dll", true},     {"vcruntime140d.DLL", true},
      {"MSVCR100.dll", true},         {"msvcr100.DLL", true},
      {"MSVCR100-custom.dll", false}, {"MSVCR100.dll.extra", false},
      {"MSVCR100.exe", false},        {"MSVCR100", false},
      {"MSVCR90.dll", false},         {"MSVCRT.dll", false},
      {"application.dll", false}};
  for (const auto &[Module, Known] : Cases) {
    SCOPED_TRACE(Module);
    for (bool Direct : {false, true}) {
      SCOPED_TRACE(Direct);
      CxxPersonalityImage F(Direct);
      F.Source.Code.Image.Imports.front().Module = Module;
      const auto Runtime = coff_loader::getCheckedX86CxxPersonalityABI(
          F.Source.Code.Image, F.Source.EH);
      ASSERT_EQ(Runtime.has_value(), Known);
      EXPECT_EQ(coff_loader::isCheckedX86CxxHandlerReference(
                    F.Source.Code.Image, F.HandlerVA),
                Known);
      if (Runtime) {
        EXPECT_EQ(Runtime->RuntimeVA, Direct ? F.HandlerVA + 7 : F.RuntimeVA);
        EXPECT_EQ(Runtime->IATVA, F.IATVA);
      }
    }
    ThrowImage F;
    F.Image.Imports.front().Module = Module;
    const auto Entry =
        getCheckedX86RegistrationThrowImportABI(F.Image, ThrowImage::ImportVA);
    ASSERT_EQ(Entry.has_value(), Known);
    const auto Throw =
        getCheckedX86RegistrationThrowCalleeABI(F.Image, ThrowImage::TextVA);
    ASSERT_EQ(Throw.has_value(), Known);
    if (Throw) {
      EXPECT_EQ(Throw->ImportVA, ThrowImage::ImportVA);
      EXPECT_EQ(Throw->ImportIATVA, ThrowImage::IATVA);
      EXPECT_EQ(Throw->ObjectOffset, -4);
      EXPECT_EQ(Throw->ThrowInfo.ObjectSize, 4u);
    }
  }
}

TEST(RegistrationCallABI, CxxPersonalityRejectsStorageAndRuntimeConflicts) {
  for (unsigned Mutation = 0; Mutation != 21; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CxxPersonalityImage F;
    auto &Image = F.Source.Code.Image;
    switch (Mutation) {
    case 0:
      Image.Imports[0].Name = "__CxxFrameHandler";
      break;
    case 1:
      Image.Imports[0].Module = "application.dll";
      break;
    case 2:
      Image.Imports.push_back(Image.Imports[0]);
      break;
    case 3:
      Image.Segments[0].Data[0x50] = 0x55;
      break;
    case 4:
      ++F.Source.EH.Registration->HandlerVA;
      break;
    case 5:
      writeLE<uint32_t>(Image.Segments[0].Data.data() + 0x53,
                        F.Source.TableVA + 4);
      break;
    case 6:
      Image.Segments[0].Flags =
          Image.Segments[0].Flags | SegmentFlags::Writable;
      break;
    case 7:
      Image.Sections[0].Flags =
          Image.Sections[0].Flags | SegmentFlags::Writable;
      break;
    case 8:
      Image.Segments.push_back(Image.Segments[0]);
      break;
    case 9:
      Image.Sections.push_back(Image.Sections[0]);
      break;
    case 10:
      Image.Segments[0].FileSz = 0x55;
      break;
    case 11:
      Image.Sections[0].FileSz = 0x55;
      break;
    case 12:
      Image.BaseRelocations[0].Type = llvm::COFF::IMAGE_REL_BASED_DIR64;
      break;
    case 13:
      --Image.BaseRelocations[0].Address;
      break;
    case 14:
      Image.BaseRelocations.push_back(Image.BaseRelocations[0]);
      break;
    case 15:
      Image.BaseRelocations[0].Address = F.HandlerVA;
      break;
    case 16:
      Image.Segments[0].Data[0x70] = 0xe9;
      writeLE<uint32_t>(Image.Segments[0].Data.data() + 0x71, uint32_t(-5));
      break;
    case 17:
      Image.Imports.clear();
      break;
    case 18:
      Image.DataAddressRelocOperands[F.HandlerVA + 3] = {F.Source.TableVA,
                                                         F.Source.TableVA, 8};
      break;
    case 19:
      Image.DataAddressRelocOperands[F.HandlerVA + 3] = {
          F.Source.TableVA, F.Source.TableVA + 4, 4};
      break;
    case 20:
      Image.CodePtrRelocSlots.insert(F.HandlerVA + 3);
      break;
    }
    EXPECT_FALSE(
        coff_loader::getCheckedX86CxxPersonalityABI(Image, F.Source.EH));
    EXPECT_FALSE(coff_loader::getCheckedX86CxxHandlerABI(Image, F.Source.EH));
    // The independent reference has no parent record to trust. A forged
    // parent association leaves the actual retained thunk identity intact.
    EXPECT_EQ(coff_loader::isCheckedX86CxxHandlerReference(Image, F.HandlerVA),
              Mutation == 4);
  }
}

TEST(RegistrationCallABI, CxxMetadataExtentsIncludeLegacyAndSpecRecords) {
  for (uint32_t Magic : {0x19930520u, 0x19930521u, 0x19930522u}) {
    SCOPED_TRACE(Magic);
    CxxMetadataImage F;
    F.tableWord(0, Magic);
    auto &Cxx = *F.EH.Cxx;
    Cxx.Magic = Magic;
    Cxx.Version = CxxFuncInfoVersion(uint8_t(Magic - 0x19930520));
    if (Magic != 0x19930522u) {
      Cxx.Flags = 0;
      Cxx.IsSynchronous = false;
    }
    if (Magic != 0x19930520u) {
      F.tableWord(28, F.TableVA + 0x90);
      F.tableWord(0x90, 1);
      F.tableWord(0x94, F.TableVA + 0xa0);
      Cxx.ESTypeListVA = F.TableVA + 0x90;
      Cxx.ExceptionSpecTypes.resize(1);
    }
    const auto Ranges =
        coff_loader::getCheckedX86CxxMetadataRanges(F.Code.Image, F.EH);
    ASSERT_TRUE(Ranges);
    EXPECT_EQ(Ranges->front().End, F.TableVA + (Magic == 0x19930520u   ? 28
                                                : Magic == 0x19930521u ? 32
                                                                       : 36));
    EXPECT_EQ(Ranges->size(), Magic == 0x19930520u ? 4u : 6u);
    if (Ranges->size() == 6) {
      EXPECT_EQ((*Ranges)[4].Begin, F.TableVA + 0x90);
      EXPECT_EQ((*Ranges)[4].End, F.TableVA + 0x98);
      EXPECT_EQ((*Ranges)[5].Begin, F.TableVA + 0xa0);
      EXPECT_EQ((*Ranges)[5].End, F.TableVA + 0xb0);
    }
  }
}

TEST(RegistrationCallABI, CxxMetadataExtentsRejectDistinctOverlappingRecords) {
  CxxMetadataImage F;
  F.tableWord(0, 0x19930520);
  F.tableWord(8, F.TableVA + 24);
  F.tableWord(24, uint32_t(-1));
  F.tableWord(28, 0);
  F.tableWord(32, 0);
  F.tableWord(36, CleanupRelayImage::RelayVA);
  auto &Cxx = *F.EH.Cxx;
  Cxx.Magic = 0x19930520;
  Cxx.Version = CxxFuncInfoVersion::Original;
  Cxx.Flags = 0;
  Cxx.IsSynchronous = false;
  // The unused legacy IP-map word aliases a valid first unwind row. Analysis
  // may describe that graph; replacing its records requires disjoint owners.
  ASSERT_TRUE(
      coff_loader::getCheckedX86CxxCallbackPointerSources(F.Code.Image, F.EH));
  EXPECT_FALSE(coff_loader::getCheckedX86CxxMetadataRanges(F.Code.Image, F.EH));
  const auto Records = coff_loader::getCheckedX86CxxFuncInfoRecords(
      F.Code.Image, F.EH.Cxx->NativeFuncInfoVA);
  ASSERT_TRUE(Records);
  EXPECT_FALSE(Records->HasDistinctRanges);
  EXPECT_EQ(Records->Cxx, Cxx);
}

TEST(RegistrationCallABI, CxxMetadataBoundsDoNotWrapAtThePE32Limit) {
  CxxMetadataImage F;
  auto &Table = F.Code.Image.Segments.back();
  Table.VA = uint64_t(UINT32_MAX) + 1 - 36;
  Table.Data.resize(36);
  Table.Size = Table.FileSz = 36;
  writeLE<uint32_t>(Table.Data.data() + 4, 0);
  writeLE<uint32_t>(Table.Data.data() + 12, 0);
  auto &Cxx = *F.EH.Cxx;
  Cxx.NativeFuncInfoVA = F.EH.HandlerDataVA = F.EH.Registration->ScopeTableVA =
      Table.VA;
  Cxx.MaxState = 0;
  Cxx.UnwindMap.clear();
  Cxx.TryBlocks.clear();
  const auto Ranges =
      coff_loader::getCheckedX86CxxMetadataRanges(F.Code.Image, F.EH);
  ASSERT_TRUE(Ranges);
  ASSERT_EQ(Ranges->size(), 1u);
  EXPECT_EQ(Ranges->front().End, uint64_t(UINT32_MAX) + 1);
  ++Table.VA;
  Cxx.NativeFuncInfoVA = F.EH.HandlerDataVA = F.EH.Registration->ScopeTableVA =
      Table.VA;
  EXPECT_FALSE(coff_loader::getCheckedX86CxxMetadataRanges(F.Code.Image, F.EH));
}

TEST(RegistrationCallABI, PreservedCallsAndCleanupCannotMutateCxxMetadata) {
  for (bool Cleanup : {false, true}) {
    for (uint32_t Offset : {0u, 32u, 36u, 0x30u, 0x3fu, 0x40u, 0x50u, 0x60u,
                            0x64u, 0x70u, 0x7fu, 0x80u}) {
      SCOPED_TRACE(Cleanup);
      SCOPED_TRACE(Offset);
      CxxMetadataImage F;
      auto &Text = F.Code.Image.Segments.front();
      const uint8_t Leaf[] = {0xc7, 0x05, 0, 0,    0,    0,   7,
                              0,    0,    0, 0x31, 0xc0, 0xc3};
      std::copy(std::begin(Leaf), std::end(Leaf), Text.Data.begin() + 0x40);
      writeLE<uint32_t>(Text.Data.data() + 0x42, F.TableVA + Offset);
      LowFunc Parent;
      Parent.Entry = 0x402000;
      Parent.ExceptionMetadata = F.EH;
      if (!Cleanup) {
        Parent.Blocks.resize(1);
        LowOp Call;
        Call.Opcode = NdOp::CALL;
        Call.addInput(NdVar::cst(CleanupRelayImage::LeafVA, 4));
        Parent.Blocks[0].Ops.push_back(Call);
      }
      const bool TouchesRecord = Offset < 36 ||
                                 (Offset < 0x40 && Offset + 4 > 0x30) ||
                                 (Offset < 0x64 && Offset + 4 > 0x50) ||
                                 (Offset < 0x80 && Offset + 4 > 0x70);
      EXPECT_EQ(hasCallerCleanupRegistrationABI(Parent, F.Code.Image),
                !TouchesRecord);
    }
  }
}

TEST(RegistrationCallABI, CxxClosureConsumesTheCheckedPrivateThrowHelper) {
  ThrowImage Throw;
  CxxMetadataImage Metadata;
  auto Table = Metadata.Code.Image.Segments.back();
  Table.VA = 0x405000;
  auto Word = [&](size_t Offset, uint32_t Value) {
    writeLE<uint32_t>(Table.Data.data() + Offset, Value);
  };
  Word(8, Table.VA + 0x30);
  Word(16, Table.VA + 0x50);
  Word(0x60, Table.VA + 0x70);
  Word(0x3c, 0);
  Throw.Image.Segments.push_back(Table);
  auto &Cxx = *Metadata.EH.Cxx;
  Cxx.NativeFuncInfoVA = Metadata.EH.HandlerDataVA =
      Metadata.EH.Registration->ScopeTableVA = Table.VA;
  Cxx.UnwindMap[1].ActionVA = 0;
  Cxx.UnwindMap[1].Kind = CxxUnwindAction::ActionKind::None;
  LowFunc Parent;
  Parent.Entry = 0x406000;
  Parent.ExceptionMetadata = Metadata.EH;
  Parent.Blocks.resize(1);
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(ThrowImage::TextVA, 4));
  Parent.Blocks[0].Ops.push_back(Call);
  Parent.RegistrationStates.emplace().ImageReadsComplete = true;
  std::vector<ExceptionAddressRange> PCs;
  EXPECT_TRUE(hasCallerCleanupRegistrationABI(Parent, Throw.Image, &PCs));
  ASSERT_EQ(PCs.size(), 1u);
  EXPECT_EQ(PCs[0].Begin, ThrowImage::CallerPCVA);
  EXPECT_EQ(PCs[0].End, ThrowImage::CallerPCVA + 4);
  Throw.Image.Imports[0].Name = "unknown_throw";
  EXPECT_FALSE(hasCallerCleanupRegistrationABI(Parent, Throw.Image, &PCs));
  EXPECT_TRUE(PCs.empty());
}

TEST(RegistrationCallABI, PhysicalScalarReturnNeedsMoreThanFramePrivacy) {
  for (const bool Computes : {false, true}) {
    CleanupRelayImage F;
    auto &Text = F.Image.Segments.front();
    if (Computes) {
      const uint8_t Leaf[] = {0x31, 0xc0, 0xc3};
      std::copy(std::begin(Leaf), std::end(Leaf), Text.Data.begin() + 0x40);
    } else
      Text.Data[0x40] = 0xc3;
    const auto P = getCheckedX86RegistrationLeafCalleeABI(
        F.Image, CleanupRelayImage::LeafVA);
    ASSERT_TRUE(P);
    EXPECT_EQ(P->HasIndependentScalarReturn, Computes);
  }
  CleanupRelayImage F;
  const auto Borrow = getCheckedX86RegistrationLeafCalleeABI(
      F.Image, CleanupRelayImage::LeafVA);
  ASSERT_TRUE(Borrow);
  EXPECT_TRUE(Borrow->HasIndependentScalarReturn);
  const auto Relay = getCheckedX86RegistrationCleanupRelayABI(
      F.Image, CleanupRelayImage::RelayVA);
  ASSERT_TRUE(Relay);
  EXPECT_TRUE(Relay->Calls[0].Leaf.HasIndependentScalarReturn);
}

TEST(RegistrationCallABI, ScalarReturnDescribesEAXBesideAnX87Result) {
  for (const bool Computes : {false, true}) {
    SCOPED_TRACE(Computes);
    CleanupRelayImage F;
    auto &Text = F.Image.Segments.front();
    // The leaf also returns the borrowed float on the x87 stack. The contract
    // still describes EAX, which is what the caller holds after the call.
    std::vector<uint8_t> Leaf = {0xd9, 0x01, 0xc3};
    if (Computes)
      Leaf.insert(Leaf.begin(), {0x31, 0xc0});
    std::copy(Leaf.begin(), Leaf.end(), Text.Data.begin() + 0x40);
    const auto P = getCheckedX86RegistrationLeafCalleeABI(
        F.Image, CleanupRelayImage::LeafVA);
    ASSERT_TRUE(P);
    EXPECT_EQ(P->HasIndependentScalarReturn, Computes);
    EXPECT_EQ(P->StackPopBytes, 0u);
    ASSERT_EQ(P->ECXReads.size(), 1u);
    EXPECT_EQ(P->ECXReads[0].Begin, 0);
    EXPECT_EQ(P->ECXReads[0].End, 4);
  }
}

TEST(RegistrationCallABI, PhysicalReturnRejectsCallerPCAndMixedPredecessors) {
  CleanupRelayImage F;
  auto &Text = F.Image.Segments.front();
  const uint8_t CallerPC[] = {0x8b, 0x04, 0x24, 0xc3};
  std::copy(std::begin(CallerPC), std::end(CallerPC), Text.Data.begin() + 0x40);
  const auto PC = getCheckedX86RegistrationLeafCalleeABI(
      F.Image, CleanupRelayImage::LeafVA);
  ASSERT_TRUE(PC);
  EXPECT_FALSE(PC->HasIndependentScalarReturn);
  // A branch can bypass the EAX definition. Both ordinary return paths matter.
  const uint8_t Mixed[] = {0x83, 0x3d, 0x00, 0x30, 0x40, 0x00, 0,
                           0x74, 3,    0x31, 0xc0, 0xc3, 0xc3};
  std::copy(std::begin(Mixed), std::end(Mixed), Text.Data.begin() + 0x40);
  Segment Data;
  Data.VA = 0x403000;
  Data.Size = Data.FileSz = 4;
  Data.Data.resize(4);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  F.Image.Segments.push_back(Data);
  const auto Branch = getCheckedX86RegistrationLeafCalleeABI(
      F.Image, CleanupRelayImage::LeafVA);
  ASSERT_TRUE(Branch);
  EXPECT_FALSE(Branch->HasIndependentScalarReturn);
}

TEST(RegistrationCallABI, ReachabilityRequiresCurrentBlocksAndCallReceipts) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CxxMetadataImage F;
    LowFunc Parent;
    Parent.Entry = 0x402000;
    Parent.ExceptionMetadata = F.EH;
    Parent.Blocks.resize(2);
    for (int I = 0; I != 2; ++I) {
      auto &Block = Parent.Blocks[I];
      Block.Id = I;
      Block.StartAddr = Parent.Entry + I * 16;
      Block.EndAddr = Block.StartAddr + 5;
      Block.InstructionBoundaries = {{Block.StartAddr, 5, 0, 1,
                                      InstructionMode::Default,
                                      LowInstructionControl::Call}};
      LowOp Call;
      Call.Addr = Block.StartAddr;
      Call.Seq = 0;
      Call.Opcode = NdOp::CALL;
      Call.addInput(
          NdVar::cst(I == 0 ? CleanupRelayImage::LeafVA : 0xdeadc0de, 4));
      Block.Ops.push_back(Call);
    }
    auto &A = Parent.RegistrationStates.emplace();
    A.Complete = A.CallbackStatesComplete = A.RegistrationLifetimeComplete =
        A.CallFrameEffectsComplete = A.ImageReadsComplete = true;
    for (const auto &Block : Parent.Blocks) {
      RegistrationBlockState State;
      State.BlockId = Block.Id;
      State.Range = {Block.StartAddr, Block.EndAddr};
      State.Reached = Block.Id == 0;
      A.Blocks.push_back(State);
    }
    RegistrationCallFrameEffect Call;
    Call.Address = Parent.Entry;
    Call.EndAddress = Parent.Entry + 5;
    Call.OpSeq = 0;
    Call.Target = CleanupRelayImage::LeafVA;
    A.CallFrameEffects.push_back(Call);
    switch (Mutation) {
    case 1:
      A.Complete = false;
      break;
    case 2:
      A.CallbackStatesComplete = false;
      break;
    case 3:
      A.RegistrationLifetimeComplete = false;
      break;
    case 4:
      A.CallFrameEffectsComplete = false;
      break;
    case 5:
      A.Blocks.pop_back();
      break;
    case 6:
      ++A.Blocks[0].Range.End;
      break;
    case 7:
      ++A.CallFrameEffects[0].Target;
      break;
    case 8:
      ++A.CallFrameEffects[0].EndAddress;
      break;
    case 9:
      A.Blocks[1].Reached = true;
      break;
    case 10:
      A.CallFrameEffects.clear();
      break;
    }
    EXPECT_EQ(hasCallerCleanupRegistrationABI(Parent, F.Code.Image),
              Mutation == 0);
  }
}
