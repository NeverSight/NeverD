//===- RegistrationThrowObjectTests.cpp - PE32 destructor contracts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep decoded destructor addresses separate from native lifetime authority.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/RegistrationCxxState.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <set>

namespace {
using namespace neverd;

BinaryImage objectImage(std::vector<uint8_t> Code) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Segment Text;
  Text.VA = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Code);
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  Segment Table;
  Table.VA = 0x2000;
  Table.Flags = SegmentFlags::Readable;
  Table.Data.resize(0x100);
  Table.Size = Table.Data.size();
  for (const auto &[Offset, Value] : {std::pair{4u, 0x1000u},
                                      {12u, 0x2020u},
                                      {0x20u, 1u},
                                      {0x24u, 0x2030u},
                                      {0x34u, 0x2080u},
                                      {0x3cu, UINT32_MAX},
                                      {0x44u, 8u}})
    writeLE<uint32_t>(Table.Data.data() + Offset, Value);
  const std::string Name = ".?AUObject@@";
  std::copy(Name.begin(), Name.end(), Table.Data.begin() + 0x88);
  Image.Segments.push_back(std::move(Table));
  Segment Data;
  Data.VA = 0x3000;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(16);
  Data.Size = Data.Data.size();
  Image.Segments.push_back(std::move(Data));
  for (auto &Segment : Image.Segments) {
    Segment.FileSz = Segment.Data.size();
    Section Section;
    Section.VA = Segment.VA;
    Section.Size = Segment.Size;
    Section.FileSz = Segment.FileSz;
    Section.Flags = Segment.Flags;
    Image.Sections.push_back(Section);
  }
  return Image;
}

TEST(RegistrationThrowObject, PreservesBoundedDestructorEffects) {
  // Read the first object word, add it to an image counter, then overwrite the
  // second object word. All bytes belong to this still-live runtime object.
  const auto Image = objectImage({0x8b, 0x01, 0x01, 0x05, 0x00, 0x30, 0x00,
                                  0x00, 0x89, 0x41, 0x04, 0x31, 0xc0, 0xc3});
  const auto Decoded =
      coff_loader::getCheckedX86IdentityCxxThrowInfo(Image, 0x2000);
  ASSERT_TRUE(Decoded);
  EXPECT_EQ(Decoded->DestructorVA, 0x1000u);
  EXPECT_FALSE(coff_loader::getCheckedX86SimpleCxxThrowInfo(Image, 0x2000));
  const auto Proof = getCheckedX86RegistrationThrowObjectABI(Image, 0x2000);
  ASSERT_TRUE(Proof);
  ASSERT_TRUE(Proof->Destructor);
  EXPECT_TRUE(Proof->PreservesDispatchBindings);
  EXPECT_EQ(Proof->Info.ObjectSize, 8u);
  EXPECT_EQ(Proof->Destructor->ECXReads,
            (std::vector<RegistrationObjectExtent>{{0, 4}}));
  EXPECT_EQ(Proof->Destructor->ECXWrites,
            (std::vector<RegistrationObjectExtent>{{4, 8}}));
  ASSERT_EQ(Proof->Destructor->ImageReads.size(), 1u);
  EXPECT_EQ(Proof->Destructor->ImageReads[0].Begin, 0x3000u);
  ASSERT_EQ(Proof->Destructor->ImageWrites.size(), 1u);
  EXPECT_EQ(Proof->Destructor->ImageWrites[0].End, 0x3004u);
  ASSERT_EQ(Proof->Destructor->CodeRanges.size(), 1u);
  EXPECT_EQ(Proof->Destructor->CodeRanges[0].Begin, 0x1000u);
}

TEST(RegistrationThrowObject, RejectsUnprovedRuntimeCallbacks) {
  const std::pair<const char *, std::vector<uint8_t>> Cases[] = {
      {"out-of-bounds read", {0x8b, 0x41, 0x08, 0xc3}},
      {"out-of-bounds write", {0xc7, 0x41, 0x08, 0, 0, 0, 0, 0xc3}},
      {"negative object offset", {0x8b, 0x41, 0xfc, 0xc3}},
      {"escaping object", {0x89, 0x0d, 0, 0x30, 0, 0, 0xc3}},
      {"CRT caller PC", {0x8b, 0x04, 0x24, 0xa3, 0, 0x30, 0, 0, 0xc3}},
      {"caller stack read", {0x8b, 0x44, 0x24, 0x04, 0xc3}},
      {"FS chain read", {0x64, 0xa1, 0, 0, 0, 0, 0xc3}},
      {"indirect call", {0xff, 0xd0, 0xc3}},
      {"unchecked call", {0xe8, 0, 0, 0, 0, 0xc3}},
      {"callee cleanup", {0xc2, 0x04, 0}},
      {"changed nonvolatile register", {0x31, 0xdb, 0xc3}},
      {"ThrowInfo mutation", {0xc7, 0x05, 0, 0x20, 0, 0, 0, 0, 0, 0, 0xc3}},
      {"type mutation", {0xc7, 0x05, 0x80, 0x20, 0, 0, 0, 0, 0, 0, 0xc3}},
      {"code mutation", {0xc7, 0x05, 0, 0x10, 0, 0, 0, 0, 0, 0, 0xc3}}};
  for (const auto &[Name, Code] : Cases) {
    SCOPED_TRACE(Name);
    const auto Image = objectImage(Code);
    ASSERT_TRUE(coff_loader::getCheckedX86IdentityCxxThrowInfo(Image, 0x2000));
    EXPECT_FALSE(getCheckedX86RegistrationThrowObjectABI(Image, 0x2000));
  }
}

TEST(RegistrationThrowObject, VoidDestructorAndAbsentDestructorStayDistinct) {
  auto Image = objectImage({0xff, 0x05, 0, 0x30, 0, 0, 0xc3});
  auto Proof = getCheckedX86RegistrationThrowObjectABI(Image, 0x2000);
  ASSERT_TRUE(Proof);
  ASSERT_TRUE(Proof->Destructor);
  EXPECT_TRUE(Proof->PreservesDispatchBindings);
  EXPECT_FALSE(Proof->Destructor->HasIndependentScalarReturn);
  writeLE<uint32_t>(Image.Segments[1].Data.data() + 4, 0);
  Proof = getCheckedX86RegistrationThrowObjectABI(Image, 0x2000);
  ASSERT_TRUE(Proof);
  EXPECT_FALSE(Proof->Destructor);
  EXPECT_TRUE(Proof->PreservesDispatchBindings);
  EXPECT_TRUE(coff_loader::getCheckedX86SimpleCxxThrowInfo(Image, 0x2000));
  size_t Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(getCheckedX86RegistrationThrowObjectABI(Image, 0x2000, &Work));
}

TEST(RegistrationThrowObject, MutableCallbackCodeCannotSupplyAContract) {
  auto Image = objectImage({0xc3});
  Image.Segments[0].Flags = Image.Segments[0].Flags | SegmentFlags::Writable;
  EXPECT_TRUE(coff_loader::getCheckedX86IdentityCxxThrowInfo(Image, 0x2000));
  EXPECT_FALSE(getCheckedX86RegistrationThrowObjectABI(Image, 0x2000));
}

TEST(RegistrationThrowObject, VoidDestructorMayLeaveItsObjectInEAX) {
  const auto Image = objectImage({0x89, 0xc8, 0xc7, 0x01, 0, 0, 0, 0, 0xc3});
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(Image, 0x1000));
  const auto Proof = getCheckedX86RegistrationThrowObjectABI(Image, 0x2000);
  ASSERT_TRUE(Proof);
  ASSERT_TRUE(Proof->Destructor);
  EXPECT_FALSE(Proof->Destructor->HasIndependentScalarReturn);
  EXPECT_EQ(Proof->Destructor->ECXWrites,
            (std::vector<RegistrationObjectExtent>{{0, 4}}));
}

TEST(RegistrationThrowObject, PrivateThrowRetainsCodeInBothAddressOrders) {
  for (bool DestructorFirst : {false, true}) {
    SCOPED_TRACE(DestructorFirst);
    const va_t Entry = DestructorFirst ? 0x1100 : 0x1000;
    const va_t Destructor = DestructorFirst ? 0x1000 : 0x1100;
    const std::vector<uint8_t> Destroy = {0xff, 0x05, 0, 0x30, 0, 0, 0xc3};
    auto Image = objectImage({});
    Image.Entry = Entry;
    auto &Text = Image.Segments[0];
    Text.Data.resize(0x140, 0xcc);
    Text.Size = Text.FileSz = Text.Data.size();
    Image.Sections.front().Size = Image.Sections.front().FileSz = Text.Size;
    std::copy(Destroy.begin(), Destroy.end(),
              Text.Data.begin() + Destructor - Text.VA);
    writeLE<uint32_t>(Image.Segments[1].Data.data() + 4, Destructor);
    // Initialize both private object words, then pass the exact object/table
    // pair to the original stdcall CRT thunk. The helper never returns.
    std::vector<uint8_t> Helper = {
        0x55, 0x8b, 0xec, 0x83, 0xec, 8,    0xc7, 0x45, 0xf8, 7,
        0,    0,    0,    0xc7, 0x45, 0xfc, 13,   0,    0,    0,
        0x68, 0,    0x20, 0,    0,    0x8d, 0x4d, 0xf8, 0x51, 0xe8};
    const size_t Displacement = Helper.size();
    Helper.resize(Helper.size() + 4);
    writeLE<uint32_t>(Helper.data() + Displacement,
                      uint32_t(0x1080 - (Entry + Helper.size())));
    std::copy(Helper.begin(), Helper.end(),
              Text.Data.begin() + Entry - Text.VA);
    Text.Data[0x80] = 0xff;
    Text.Data[0x81] = 0x25;
    writeLE<uint32_t>(Text.Data.data() + 0x82, 0x3008);
    Import Runtime;
    Runtime.Module = "VCRUNTIME140.dll";
    Runtime.Name = "_CxxThrowException";
    Runtime.IATAddr = 0x3008;
    Image.Imports.push_back(Runtime);
    const auto Proof = getCheckedX86RegistrationThrowCalleeABI(Image, Entry);
    ASSERT_TRUE(Proof);
    EXPECT_TRUE(Proof->PreservesDispatchBindings);
    EXPECT_EQ(Proof->ThrowInfo.DestructorVA, Destructor);
    for (va_t Address : {Entry, Destructor, va_t(0x1080)})
      EXPECT_TRUE(llvm::any_of(Proof->CodeRanges, [&](const auto &Range) {
        return Range.contains(Address);
      }));
    ASSERT_EQ(Proof->ImageWrites.size(), 1u);
    EXPECT_EQ(Proof->ImageWrites[0].Begin, 0x3000u);
    EXPECT_EQ(Proof->ImageWrites[0].End, 0x3004u);
  }
}

TEST(RegistrationThrowObject, InputPE32DispatchRechecksRuntimeCallbacks) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Loaded = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Loaded));
  const auto Entry = llvm::find_if(Loaded->Exports, [](const auto &Export) {
    return Export.Name == "callback_parent";
  });
  ASSERT_NE(Entry, Loaded->Exports.end());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Loaded));
  const auto Source = CFGBuilder().build(*Loaded, Dec, Entry->Addr);
  ASSERT_TRUE(Source.RegistrationStates);
  const auto &States = *Source.RegistrationStates;
  ASSERT_TRUE(States.SourceCallFrameEffectsComplete);
  ASSERT_TRUE(States.RuntimeObjectAccessesComplete);
  if (llvm::none_of(States.CallFrameEffects, [&](const auto &Call) {
        return States.CalleeContracts[Call.CalleeIndex].isRethrow() ||
               (Call.RuntimeThrow && Call.RuntimeThrow->isRethrow());
      }))
    GTEST_SKIP() << "requires a rethrow after runtime destruction";
  std::optional<RegistrationThrowObjectABI> Object;
  for (const auto &Callee : States.CalleeContracts) {
    std::vector<va_t> Tables;
    if (Callee.CalleeKind ==
        RegistrationCalleeFrameContract::Kind::PrivateThrow) {
      const auto Throw =
          getCheckedX86RegistrationThrowCalleeABI(*Loaded, Callee.Target);
      ASSERT_TRUE(Throw);
      Tables.push_back(Throw->ThrowInfo.Address);
    }
    for (const auto &Info : Callee.RuntimeThrowInfos)
      Tables.push_back(Info.Address);
    for (va_t Table : Tables) {
      auto Candidate = getCheckedX86RegistrationThrowObjectABI(*Loaded, Table);
      if (!Candidate || !Candidate->Destructor)
        continue;
      if (llvm::none_of(States.CxxCatchObjects, [&](const auto &Catch) {
            return Catch.Reference &&
                   Catch.TypeDescriptorVA == Candidate->Info.TypeDescriptorVA;
          }))
        continue;
      // Isolate CRT destruction from the separate parent cleanup proof.
      if (llvm::any_of(States.CleanupContracts, [&](const auto &Cleanup) {
            return llvm::any_of(Cleanup.Calls, [&](const auto &Call) {
              return Call.Leaf.Target == Candidate->Info.DestructorVA;
            });
          }))
        continue;
      Object = std::move(Candidate);
      break;
    }
    if (Object)
      break;
  }
  if (!Object)
    GTEST_SKIP() << "requires a runtime-only destructor and a later throw";
  ASSERT_TRUE(Object->PreservesDispatchBindings);
  ASSERT_FALSE(Object->Destructor->ImageWrites.empty());
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = *Loaded;
    auto Changed = Source;
    Changed.RegistrationStates.reset();
    auto Replace = [&](va_t Address, const std::vector<uint8_t> &Bytes) {
      auto Owner = llvm::find_if(Image.Segments, [&](const auto &Segment) {
        return Address >= Segment.VA &&
               Address - Segment.VA <= Segment.Data.size() &&
               Bytes.size() <= Segment.Data.size() - (Address - Segment.VA);
      });
      ASSERT_NE(Owner, Image.Segments.end());
      std::copy(Bytes.begin(), Bytes.end(),
                Owner->Data.begin() + Address - Owner->VA);
    };
    if (Mutation == 1)
      Replace(Object->Info.DestructorVA, {0xa1, 0, 0, 0, 0, 0xc3});
    if (Mutation == 2)
      Replace(Object->Info.DestructorVA, {0xd9, 0xee, 0xc3});
    if (Mutation == 3) {
      Import Other;
      Other.Module = "application.dll";
      Other.Name = "callback";
      Other.IATAddr = Object->Destructor->ImageWrites.front().Begin;
      Image.Imports.push_back(Other);
    }
    if (Mutation == 4)
      for (auto &Section : Image.Sections)
        if (Section.VA <= Object->Destructor->ImageWrites.front().Begin &&
            Object->Destructor->ImageWrites.front().Begin - Section.VA <
                Section.Size)
          Section.Flags = SegmentFlags::Readable;
    if (Mutation == 5)
      Replace(Object->Info.Address + 4, {1, 0, 0, 0});
    if (Mutation == 6) {
      const auto *Array = Loaded->readVA(Object->Info.Address + 12, 4);
      ASSERT_TRUE(Array);
      const auto *Type = Loaded->readVA(readLE<uint32_t>(Array) + 4, 4);
      ASSERT_TRUE(Type);
      Replace(readLE<uint32_t>(Type) + 20, {1, 0, 0, 0});
    }
    if (Mutation == 7)
      for (auto &Segment : Image.Segments)
        if (Segment.VA <= Object->Info.DestructorVA &&
            Object->Info.DestructorVA - Segment.VA < Segment.Size)
          Segment.Flags = Segment.Flags | SegmentFlags::Writable;
    if (Mutation == 8)
      std::reverse(Changed.Blocks.begin(), Changed.Blocks.end());
    const auto Current = analyzeCheckedX86CxxStates(
        Image, Changed, &States.CalleeContracts, &States.CleanupContracts);
    EXPECT_EQ(Current.SourceCallFrameEffectsComplete,
              Mutation == 0 || Mutation == 8);
  }
}

TEST(RegistrationThrowObject, InputPE32BindsDestruction) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Entry = llvm::find_if(Image->Exports, [](const auto &Export) {
    return Export.Name == "callback_parent";
  });
  ASSERT_NE(Entry, Image->Exports.end());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  const auto Low = CFGBuilder().build(*Image, Decode, Entry->Addr);
  ASSERT_TRUE(Low.RegistrationStates);
  const auto &States = *Low.RegistrationStates;
  ASSERT_TRUE(States.Complete);
  ASSERT_TRUE(States.CxxCatchObjectsComplete);
  ASSERT_TRUE(States.RuntimeObjectAccessesComplete);
  ASSERT_EQ(States.CxxCatchObjects.size(), 2u);
  std::set<va_t> Destructors;
  for (const auto &Callee : States.CalleeContracts) {
    std::vector<va_t> Tables;
    if (Callee.CalleeKind ==
        RegistrationCalleeFrameContract::Kind::PrivateThrow) {
      const auto Throw =
          getCheckedX86RegistrationThrowCalleeABI(*Image, Callee.Target);
      ASSERT_TRUE(Throw);
      Tables.push_back(Throw->ThrowInfo.Address);
    }
    for (const auto &Info : Callee.RuntimeThrowInfos)
      Tables.push_back(Info.Address);
    for (va_t Table : Tables) {
      const auto Object =
          getCheckedX86RegistrationThrowObjectABI(*Image, Table);
      ASSERT_TRUE(Object);
      if (!Object->Destructor)
        continue;
      Destructors.insert(Object->Info.DestructorVA);
      EXPECT_FALSE(Object->Destructor->ECXReads.empty());
      EXPECT_FALSE(Object->Destructor->ImageWrites.empty());
      EXPECT_TRUE(llvm::any_of(Callee.CodeRanges, [&](const auto &Range) {
        return Range.contains(Object->Info.DestructorVA);
      }));
      for (const auto &Write : Object->Destructor->ImageWrites)
        EXPECT_TRUE(llvm::any_of(Callee.ImageWrites, [&](const auto &Range) {
          return Range.Begin <= Write.Begin && Write.End <= Range.End;
        }));
    }
  }
  EXPECT_EQ(Destructors.size(), 2u);
}
} // namespace
