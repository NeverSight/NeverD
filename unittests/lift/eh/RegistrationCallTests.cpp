//===- RegistrationCallTests.cpp - Checked PE32 call frame boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
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
  }

  void tableWord(size_t Offset, uint32_t Value) {
    writeLE<uint32_t>(Image.Segments[1].Data.data() + Offset, Value);
  }
};
} // namespace

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
      F.tableWord(0x30, 0);
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
      Text.Size = Text.Data.size();
      Image.Segments.push_back(Text);
      Segment Data;
      Data.VA = TableVA - 16;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(128);
      Data.Size = Data.Data.size();
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
    const auto Runtime = coff_loader::getCheckedX86CxxPersonalityABI(
        F.Source.Code.Image, F.Source.EH);
    ASSERT_TRUE(Runtime);
    EXPECT_EQ(Runtime->RuntimeVA, Direct ? F.HandlerVA + 7 : F.RuntimeVA);
    EXPECT_EQ(Runtime->IATVA, F.IATVA);
    EXPECT_NE(Runtime->RuntimeVA, F.HandlerVA);
    EXPECT_FALSE(Runtime->CodeRanges.empty());
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
