//===- RegistrationCopyTests.cpp - PE32 copy object domains ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify the byte, frame and runtime metadata contracts of object copying.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace {
using namespace neverd;

BinaryImage copyImage(std::vector<uint8_t> Code) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Segment Text;
  Text.VA = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Code);
  Text.Size = Text.FileSz = Text.Data.size();
  Section CodeSection;
  CodeSection.VA = Text.VA;
  CodeSection.Size = CodeSection.FileSz = Text.Size;
  CodeSection.Flags = Text.Flags;
  Image.Sections.push_back(CodeSection);
  Image.Segments.push_back(std::move(Text));
  Segment Data;
  Data.VA = 0x3000;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(16);
  Data.Size = Data.Data.size();
  Image.Segments.push_back(std::move(Data));
  return Image;
}

std::vector<uint8_t> copyWords() {
  // EAX is the source, ECX is the independent destination. Return this after
  // copying both words; the CRT ignores the constructor result.
  return {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x10, 0x89, 0x11, 0x8b, 0x50,
          0x04, 0x89, 0x51, 0x04, 0x89, 0xc8, 0xc2, 0x04, 0x00};
}

TEST(RegistrationCopy, SeparatesSourceReadsAndCompleteDestinationWrites) {
  auto Code = copyWords();
  const uint8_t Counter[] = {0xff, 0x05, 0, 0x30, 0, 0};
  Code.insert(Code.begin() + 14, std::begin(Counter), std::end(Counter));
  const auto Image = copyImage(Code);
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(Image, 0x1000));
  const auto Copy = getCheckedX86RegistrationCopyCalleeABI(Image, 0x1000, 8);
  ASSERT_TRUE(Copy);
  EXPECT_EQ(Copy->Destination.StackPopBytes, 4u);
  EXPECT_FALSE(Copy->Destination.HasIndependentScalarReturn);
  EXPECT_TRUE(Copy->Destination.ECXReads.empty());
  EXPECT_EQ(Copy->Destination.ECXWrites,
            (std::vector<RegistrationObjectExtent>{{0, 8}}));
  EXPECT_EQ(Copy->SourceReads, (std::vector<RegistrationObjectExtent>{{0, 8}}));
  ASSERT_EQ(Copy->Destination.ImageWrites.size(), 1u);
  EXPECT_EQ(Copy->Destination.ImageWrites[0].Begin, 0x3000u);
  EXPECT_EQ(Copy->Destination.ImageWrites[0].End, 0x3004u);
}

TEST(RegistrationCopy, KeepsBothPointersThroughPrivateSpills) {
  const auto Image =
      copyImage({0x55, 0x89, 0xe5, 0x83, 0xec, 8,    0x89, 0x4d, 0xfc, 0x8b,
                 0x45, 8,    0x89, 0x45, 0xf8, 0x8b, 0x4d, 0xfc, 0x8b, 0x55,
                 0xf8, 0x8b, 0x02, 0x89, 0x01, 0x8b, 0x42, 4,    0x89, 0x41,
                 4,    0x89, 0xc8, 0x89, 0xec, 0x5d, 0xc2, 4,    0});
  const auto Copy = getCheckedX86RegistrationCopyCalleeABI(Image, 0x1000, 8);
  ASSERT_TRUE(Copy);
  EXPECT_EQ(Copy->SourceReads, (std::vector<RegistrationObjectExtent>{{0, 8}}));
  EXPECT_EQ(Copy->Destination.ECXWrites,
            (std::vector<RegistrationObjectExtent>{{0, 8}}));
}

TEST(RegistrationCopy, RequiresInitializationOnEveryReturningPath) {
  std::vector<uint8_t> Code = {0x8b, 0x44, 0x24, 4,    0x8b, 0x10, 0x85, 0xd2,
                               0x74, 10,   0x89, 0x11, 0x8b, 0x50, 4,    0x89,
                               0x51, 4,    0xeb, 13,   0xc7, 1,    0,    0,
                               0,    0,    0xc7, 0x41, 4,    0,    0,    0,
                               0,    0x89, 0xc8, 0xc2, 4,    0};
  EXPECT_TRUE(
      getCheckedX86RegistrationCopyCalleeABI(copyImage(Code), 0x1000, 8));
  std::fill(Code.begin() + 26, Code.begin() + 33, 0x90);
  EXPECT_FALSE(
      getCheckedX86RegistrationCopyCalleeABI(copyImage(Code), 0x1000, 8));
  const auto Image = copyImage(copyWords());
  for (uint32_t Size : {0u, 4u, 12u, UINT32_MAX}) {
    SCOPED_TRACE(Size);
    EXPECT_FALSE(getCheckedX86RegistrationCopyCalleeABI(Image, 0x1000, Size));
  }
  size_t Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(getCheckedX86RegistrationCopyCalleeABI(Image, 0x1000, 8, &Work));
}

TEST(RegistrationCopy, RejectsEscapesWrongDomainsAndCallerStorage) {
  const std::pair<const char *, std::vector<uint8_t>> Cases[] = {
      {"source write", {0xc7, 0, 0, 0, 0, 0}},
      {"source bounds", {0x8b, 0x50, 8}},
      {"negative source", {0x8b, 0x50, 0xfc}},
      {"uninitialized destination", {0x8b, 0x11}},
      {"source pointer in destination", {0x89, 0x01}},
      {"destination pointer in source", {0x89, 0x08}},
      {"source pointer escape", {0xa3, 0, 0x30, 0, 0}},
      {"destination pointer escape", {0x89, 0x0d, 0, 0x30, 0, 0}},
      {"caller stack", {0x8b, 0x54, 0x24, 8}},
      {"argument slot overwrite", {0x89, 0x44, 0x24, 4}},
      {"caller PC", {0x8b, 0x14, 0x24, 0x89, 0x15, 0, 0x30, 0, 0}},
      {"changed nonvolatile", {0x31, 0xdb}},
      {"unchecked call", {0xff, 0xd0}}};
  for (const auto &[Name, Prefix] : Cases) {
    SCOPED_TRACE(Name);
    auto Code = copyWords();
    Code.insert(Code.begin() + 4, Prefix.begin(), Prefix.end());
    EXPECT_FALSE(
        getCheckedX86RegistrationCopyCalleeABI(copyImage(Code), 0x1000, 8));
  }
  for (uint8_t Pop : {0, 8}) {
    auto Code = copyWords();
    Code[Code.size() - 2] = Pop;
    EXPECT_FALSE(
        getCheckedX86RegistrationCopyCalleeABI(copyImage(Code), 0x1000, 8));
  }
}

TEST(RegistrationCopy, MetadataRequiresTheIndependentCopyProof) {
  auto Image = copyImage(copyWords());
  Segment Table;
  Table.VA = 0x2000;
  Table.Flags = SegmentFlags::Readable;
  Table.Data.resize(0x100);
  Table.Size = Table.Data.size();
  for (const auto &[Offset, Value] : {std::pair{12u, 0x2020u},
                                      {0x20u, 1u},
                                      {0x24u, 0x2030u},
                                      {0x34u, 0x2080u},
                                      {0x3cu, UINT32_MAX},
                                      {0x44u, 8u},
                                      {0x48u, 0x1000u}})
    writeLE<uint32_t>(Table.Data.data() + Offset, Value);
  const std::string Name = ".?AUCopied@@";
  std::copy(Name.begin(), Name.end(), Table.Data.begin() + 0x88);
  Image.Segments.insert(Image.Segments.begin() + 1, std::move(Table));
  const auto Decoded =
      coff_loader::getCheckedX86IdentityCxxThrowInfo(Image, 0x2000);
  ASSERT_TRUE(Decoded);
  EXPECT_EQ(Decoded->CopyConstructorVA, 0x1000u);
  EXPECT_FALSE(coff_loader::getCheckedX86SimpleCxxThrowInfo(Image, 0x2000));
  const auto Object = getCheckedX86RegistrationThrowObjectABI(Image, 0x2000);
  ASSERT_TRUE(Object);
  ASSERT_TRUE(Object->CopyConstructor);
  EXPECT_FALSE(Object->Destructor);
  EXPECT_FALSE(Object->PreservesDispatchBindings);
  Image.Segments[0].Flags = Image.Segments[0].Flags | SegmentFlags::Writable;
  EXPECT_TRUE(coff_loader::getCheckedX86IdentityCxxThrowInfo(Image, 0x2000));
  EXPECT_FALSE(getCheckedX86RegistrationThrowObjectABI(Image, 0x2000));
  Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  writeLE<uint32_t>(Image.Segments[1].Data.data() + 0x44, 12);
  EXPECT_TRUE(coff_loader::getCheckedX86IdentityCxxThrowInfo(Image, 0x2000));
  EXPECT_FALSE(getCheckedX86RegistrationThrowObjectABI(Image, 0x2000));
  writeLE<uint32_t>(Image.Segments[1].Data.data() + 0x30, 1);
  EXPECT_FALSE(coff_loader::getCheckedX86IdentityCxxThrowInfo(Image, 0x2000));
}

TEST(RegistrationCopy, InputPE32BindsRuntimeConstruction) {
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
  std::set<va_t> Constructors;
  for (const auto &Callee : States.CalleeContracts) {
    std::vector<va_t> Tables;
    if (Callee.isRuntimeThrow()) {
      for (const auto &Info : Callee.RuntimeThrowInfos)
        Tables.push_back(Info.Address);
    } else if (Callee.CalleeKind ==
               RegistrationCalleeFrameContract::Kind::PrivateThrow) {
      const auto Throw =
          getCheckedX86RegistrationThrowCalleeABI(*Image, Callee.Target);
      ASSERT_TRUE(Throw);
      Tables.push_back(Throw->ThrowInfo.Address);
    }
    for (va_t Table : Tables) {
      const auto Object =
          getCheckedX86RegistrationThrowObjectABI(*Image, Table);
      ASSERT_TRUE(Object);
      if (!Object->CopyConstructor)
        continue;
      const auto &Copy = *Object->CopyConstructor;
      Constructors.insert(Copy.Destination.Target);
      EXPECT_EQ(Copy.Destination.StackPopBytes, 4u);
      EXPECT_EQ(Copy.SourceReads, (std::vector<RegistrationObjectExtent>{
                                      {0, int32_t(Object->Info.ObjectSize)}}));
      EXPECT_EQ(Copy.Destination.ECXWrites, Copy.SourceReads);
      EXPECT_FALSE(Copy.Destination.ImageWrites.empty());
      for (const auto &Range : Copy.Destination.CodeRanges)
        EXPECT_TRUE(llvm::any_of(Callee.CodeRanges, [&](const auto &Owner) {
          return Owner.Begin <= Range.Begin && Range.End <= Owner.End;
        }));
      for (const auto &Range : Copy.Destination.ImageWrites)
        EXPECT_TRUE(llvm::any_of(Callee.ImageWrites, [&](const auto &Owner) {
          return Owner.Begin <= Range.Begin && Range.End <= Owner.End;
        }));
    }
  }
  EXPECT_EQ(Constructors.size(), 2u);
}
} // namespace
