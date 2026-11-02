#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCMetadataJSON.h"
#include "neverd/loader/ObjC/ObjCMethods.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "llvm/Support/Endian.h"

#include <cstring>

using namespace neverd;
namespace {
TEST(ObjCStorage, PointerOverlapMatchesLinearScanAtAddressBoundaries) {
  for (unsigned Kind = 0; Kind != 8; ++Kind) {
    SCOPED_TRACE(Kind);
    BinaryImage Image;
    std::vector<va_t> Slots;
    auto Insert = [&](va_t Address) {
      Slots.push_back(Address);
      switch (Kind) {
      case 0:
        Image.MachOResolvedChainedPointerSlots.insert(Address);
        break;
      case 1:
        Image.CodePtrRelocSlots.insert(Address);
        break;
      case 2:
        Image.DataPtrRelocSlots.insert(Address);
        break;
      case 3:
        Image.RelDataPtrRelocSlots.insert(Address);
        break;
      case 4:
        Image.RelCodeRelocSlots.insert(Address);
        break;
      case 5:
        Image.ImportPtrSlots[Address] = "pointer";
        break;
      case 6:
        Image.ImportStorageSlots[Address] = {};
        break;
      case 7:
        Image.DyldBindSlots[Address] = {};
        break;
      }
    };
    auto Check = [&](va_t Address, uint64_t Width) {
      const bool Expected =
          std::any_of(Slots.begin(), Slots.end(), [&](va_t Slot) {
            return Slot <= Address ? Address - Slot < 8
                                   : Slot - Address < Width;
          });
      EXPECT_EQ(sdk::objc_binding_detail::overlapsPointerStorage(Image, Address,
                                                                 Width),
                Expected)
          << Address << ": " << Width;
    };
    Check(0, 0);
    Check(UINT64_MAX, UINT64_MAX);
    for (va_t Slot :
         {UINT64_C(0), UINT64_C(8), UINT64_C(127), UINT64_MAX - 15, UINT64_MAX})
      Insert(Slot);
    uint64_t State = UINT64_C(0x8da736f4592bc10e);
    for (unsigned I = 0; I != 256; ++I) {
      State = State * UINT64_C(6364136223846793005) + 1;
      Insert(State);
    }
    for (va_t Slot : Slots)
      for (uint64_t Offset = 0; Offset != 18; ++Offset)
        for (uint64_t Width : {UINT64_C(0), UINT64_C(1), UINT64_C(7),
                               UINT64_C(8), UINT64_C(9), UINT64_MAX}) {
          if (Offset <= Slot)
            Check(Slot - Offset, Width);
          if (Offset <= UINT64_MAX - Slot)
            Check(Slot + Offset, Width);
        }
  }
}

struct StorageImage {
  BinaryImage Image;
  void word(va_t Address, uint32_t Value) {
    llvm::support::endian::write32le(
        Image.Segments[0].Data.data() + Address - 0x1000, Value);
  }
  void pointer(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(
        Image.Segments[0].Data.data() + Address - 0x1000, Value);
  }
  void string(va_t Address, const char *Text) {
    std::memcpy(Image.Segments[0].Data.data() + Address - 0x1000, Text,
                std::strlen(Text) + 1);
  }
  StorageImage() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Mapping;
    Mapping.VA = Mapping.FileOff = 0x1000;
    Mapping.Size = Mapping.FileSz = 0x1000;
    Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Mapping.Data.resize(0x1000);
    Image.Segments.push_back(Mapping);
    Section Records;
    Records.Name = "__objc_const";
    Records.VA = Records.FileOff = 0x1000;
    Records.Size = Records.FileSz = 0x1000;
    Records.Flags = Mapping.Flags;
    Image.Sections.push_back(Records);
    ObjCClass Class;
    Class.Name = "StoredValues";
    Class.Address = 0x1100;
    Image.ObjCClasses.push_back(Class);
    pointer(0x1120, 0x1202); // Stable Swift class, with two stored fields.
    word(0x1204, 8);
    word(0x1208, 32);
    pointer(0x1230, 0x1300);
    word(0x1300, 32);
    word(0x1304, 2);
    for (unsigned Index = 0; Index < 2; ++Index) {
      const va_t Entry = 0x1308 + Index * 32;
      pointer(Entry, 0x1400 + Index * 8);
      pointer(Entry + 8, 0x1500 + Index * 64);
      pointer(Entry + 16, 0x1520 + Index * 64);
      word(Entry + 24, 3);
      word(Entry + 28, Index ? 16 : 8);
      pointer(0x1400 + Index * 8, Index ? 16 : 8);
    }
    string(0x1500, "first");
    string(0x1520, "d");
    string(0x1540, "opaque");
    string(0x1560, "");
  }

  void runtimeOffsets() {
    Image.Segments[0].Size += 16;
    Section Slots;
    Slots.Name = "__common";
    Slots.VA = 0x2000;
    Slots.Size = 16;
    Slots.Type = llvm::MachO::S_ZEROFILL;
    Slots.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Sections.push_back(Slots);
    pointer(0x1308, 0x2000);
    pointer(0x1328, 0x2008);
    word(0x1344, 0); // A resilient field has no static width.
  }
};

TEST(ObjCStorage, Arm64eDeclarationsDoNotGrantNativeStorageProof) {
  StorageImage F;
  F.Image.MachOCPUSubtype = 2;
  F.pointer(0x1400,
            9); // A bitfield's declaration need not be naturally aligned.
  F.pointer(0x1408, 9); // Nor disjoint from another field's storage unit.
  F.string(0x1520, "b1");
  F.string(0x1560, "b3");
  parseObjCStorage(F.Image);
  ASSERT_TRUE(F.Image.ObjCMetadataDiagnostics.empty());
  ASSERT_EQ(F.Image.ObjCClasses[0].Ivars.size(), 2u);
  EXPECT_EQ(F.Image.ObjCClasses[0].IvarStatus, "metadata_only");
  EXPECT_TRUE(F.Image.ObjCSourceReferences.empty());
  ASSERT_EQ(F.Image.ObjCMetadataReferences.size(), 2u);
  auto JSON = objcMetadataJSON(F.Image);
  ASSERT_TRUE(JSON.getArray("metadata_references"));
  EXPECT_EQ(JSON.getArray("metadata_references")->size(), 2u);
  EXPECT_EQ((*JSON.getArray("metadata_references"))[0].getAsObject()->getString(
                "view"),
            "metadata_only");
  F.Image.MachOCPUSubtype = 0;
  parseObjCStorage(F.Image);
  EXPECT_FALSE(F.Image.ObjCMetadataDiagnostics.empty());
  EXPECT_TRUE(F.Image.ObjCMetadataReferences.empty());
  EXPECT_TRUE(F.Image.ObjCSourceReferences.empty());
}

TEST(ObjCStorage, Arm64eSwiftMissingOffsetIsRetainedWithoutInventedSlot) {
  StorageImage F;
  F.Image.MachOCPUSubtype = 2;
  F.pointer(0x1308, 0);
  F.word(0x1344, 0);
  parseObjCStorage(F.Image);
  ASSERT_TRUE(F.Image.ObjCMetadataDiagnostics.empty());
  ASSERT_EQ(F.Image.ObjCClasses[0].Ivars.size(), 2u);
  EXPECT_FALSE(F.Image.ObjCClasses[0].Ivars[0].Offset);
  EXPECT_FALSE(F.Image.ObjCMetadataReferences.count(0));
  EXPECT_TRUE(F.Image.ObjCSourceReferences.empty());
  // A missing global is distinct from a nonzero unreadable pointer.
  F.pointer(0x1308, 0x3000);
  parseObjCStorage(F.Image);
  EXPECT_FALSE(F.Image.ObjCMetadataDiagnostics.empty());
  EXPECT_EQ(F.Image.ObjCClasses[0].IvarStatus, "unresolved");
  EXPECT_TRUE(F.Image.ObjCClasses[0].Ivars.empty());
}

struct StaticStringPairs : StorageImage {
  static constexpr va_t Base = 0x1608;
  static constexpr va_t Slot = 0x1f00;
  HighFunc Function;
  ExprPtr Call;
  explicit StaticStringPairs(unsigned Count = 1) {
    Image.MachOTwoLevelNamespace = true;
    Image.Symbols.push_back(
        {"_$s14StorageFixture5pairsSaySS_SStGyFTv_", Base, 0, false});
    Image.Symbols.push_back({"_next", Base + 40 + Count * 32, 0, false});
    pointer(Base + 24, Count);
    pointer(Base + 32, Count * 2);
    for (unsigned I = 0; I < Count * 2; ++I) {
      string(Base + 40 + I * 16, I % 2 ? "tw" : "ak");
      Image.Segments[0].Data[Base - 0x1000 + 40 + I * 16 + 15] = 0xe2;
    }
    Image.ImportPtrSlots[Slot] = "_swift_initStaticObject";
    Image.DyldBindSlots[Slot] = {"_swift_initStaticObject", 0,
                                 "/usr/lib/swift/libswiftCore.dylib", false};
    auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
    EXPECT_TRUE(Hint);
    Call = HighExpr::makeCall(
        "swift_initStaticObject", Slot,
        {HighExpr::makeConst(0, 8),
         HighExpr::makeConst(Base + 8, 8,
                             ConstantAddressProvenance::DataAddress)});
    if (Hint) {
      Call->Type = Hint->Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    }
    Function.ReturnType = Call->Type;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body = {Return};
  }
};

TEST(ObjCStorage, StaticStringPairsKeepTheirOnceTokenAndCompletePayload) {
  for (unsigned Count : {1U, 2U, 32U}) {
    StaticStringPairs F(Count);
    const auto Bound = sdk::bindObjCSourceReferences(F.Function, F.Image);
    ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    EXPECT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{F.Base, 40 + Count * 32}}));
    const auto Address = Bound.Function.Body[0].RetVal->Operands[1];
    ASSERT_EQ(Address->Kind, ExprKind::BinOp);
    EXPECT_EQ(Address->Op, NdOp::INT_ADD);
    ASSERT_EQ(Address->Operands.size(), 2U);
    EXPECT_EQ(Address->Operands[1]->ConstVal, 8U);
    const auto Helper = Address->Operands[0];
    ASSERT_TRUE(Helper->SourceCallHint);
    EXPECT_EQ(Helper->SourceCallHint->TargetAddress, F.Base);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Helper, F.Image, {}));
    std::set<std::string> Helpers;
    const auto Source = sdk::renderObjCLocalStorageHelpers(
        F.Image, Bound.LocalStorageExtents, Helpers);
    EXPECT_EQ(Helpers,
              std::set<std::string>{"neverd_local_storage_1608_address"});
    EXPECT_NE(Source.find("storage[" + std::to_string(40 + Count * 32) + "]"),
              std::string::npos);
    EXPECT_NE(Source.find("[24] = " + std::to_string(Count)),
              std::string::npos);
    EXPECT_NE(Source.find("[55] = 226"), std::string::npos);
    auto PayloadFunction = F.Function;
    PayloadFunction.Body[0].RetVal = HighExpr::makeConst(
        F.Base + 40, 8, ConstantAddressProvenance::DataAddress);
    const auto Payload =
        sdk::bindObjCSourceReferences(PayloadFunction, F.Image);
    ASSERT_TRUE(Payload.Limitation.empty()) << Payload.Limitation;
    EXPECT_EQ(Payload.LocalStorageExtents, Bound.LocalStorageExtents);
    const auto PayloadAddress = Payload.Function.Body[0].RetVal;
    ASSERT_EQ(PayloadAddress->Kind, ExprKind::BinOp);
    EXPECT_EQ(PayloadAddress->Operands[1]->ConstVal, 40U);
    const auto PayloadHelper = PayloadAddress->Operands[0];
    ASSERT_TRUE(PayloadHelper->SourceCallHint);
    EXPECT_EQ(PayloadHelper->SourceCallHint->TargetAddress, F.Base);
    EXPECT_TRUE(sdk::objcSourceCallBound(*PayloadHelper, F.Image, {}));
  }
}

TEST(ObjCStorage, StaticStringPairsRejectChangedHeadersLayoutsAndOwnership) {
  StaticStringPairs F;
  const auto Bound = sdk::bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  const auto Helper = Bound.Function.Body[0].RetVal->Operands[1]->Operands[0];
  for (unsigned Mutation = 0; Mutation < 23; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = F.Image;
    auto *Bytes = Changed.Segments[0].Data.data() + F.Base - 0x1000;
    if (Mutation < 3)
      Bytes[Mutation * 8] = 1;
    if (Mutation == 3)
      Bytes[24] = 0;
    if (Mutation == 4)
      Bytes[24] = 33;
    if (Mutation == 5)
      Bytes[32] = 4;
    if (Mutation == 6)
      Bytes[40] = 0x80;
    if (Mutation == 7)
      Bytes[55] = 0xf2;
    if (Mutation == 8)
      Bytes[54] = 1;
    if (Mutation == 9)
      Changed.DataPtrRelocSlots.insert(F.Base + 56);
    if (Mutation == 10)
      Changed.Sections[0].Flags = SegmentFlags::Readable;
    if (Mutation == 11)
      Changed.Symbols.push_back(Changed.Symbols.front());
    if (Mutation == 12)
      Changed.Symbols.push_back({"_inside", F.Base + 48, 0, false});
    if (Mutation == 13)
      Changed.Symbols.pop_back();
    if (Mutation == 14)
      Changed.Symbols.push_back(Changed.Symbols.back());
    if (Mutation == 15)
      Changed.Exports.push_back({"_exported", 0, F.Base + 8});
    if (Mutation == 16)
      Changed.Symbols.front().Name = "_ordinary_bytes";
    if (Mutation == 17)
      Changed.Symbols.front().Size = 80;
    if (Mutation == 18)
      Changed.Bits = Bitness::Bits32;
    if (Mutation == 19)
      Changed.Arch = Arch::X64;
    if (Mutation == 20)
      Changed.MachOTwoLevelNamespace = false;
    if (Mutation == 21)
      Changed.MachOChainedFixupsAmbiguous = true;
    if (Mutation == 22)
      Changed.Sections[0].Size = F.Base - 0x1000 + 71;
    EXPECT_FALSE(sdk::objcSourceCallBound(*Helper, Changed, {}));
    const auto Rebound = sdk::bindObjCSourceReferences(F.Function, Changed);
    EXPECT_FALSE(Rebound.Limitation.empty());
    EXPECT_TRUE(Rebound.LocalStorageExtents.empty());
  }
}

TEST(ObjCStorage, StaticStringPairsAuthenticateTheCurrentImportVeneer) {
  StaticStringPairs F;
  Segment Code;
  Code.VA = 0x3000;
  Code.Size = Code.FileSz = 16;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.resize(16);
  const uint32_t PageDelta = 0x1ffffe; // ADRP x16: 0x3000 -> 0x1000.
  llvm::support::endian::write32le(Code.Data.data(),
                                   0x90000010u | ((PageDelta & 3) << 29) |
                                       (((PageDelta >> 2) & 0x7ffff) << 5));
  llvm::support::endian::write32le(Code.Data.data() + 4,
                                   0xf9400210u | (0x1e0u << 10));
  llvm::support::endian::write32le(Code.Data.data() + 8, 0xd61f0200u);
  F.Image.Segments.push_back(Code);
  Section Text;
  Text.Name = "__stubs";
  Text.VA = Code.VA;
  Text.Size = Text.FileSz = Code.Size;
  Text.Flags = Code.Flags;
  Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  F.Image.Sections.push_back(Text);
  ASSERT_EQ(darwinImportVeneerSlot(F.Image, Code.VA), F.Slot);
  F.Call->CallAddr = Code.VA;
  const auto Bound = sdk::bindObjCSourceReferences(F.Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.LocalStorageExtents,
            (std::map<va_t, uint64_t>{{F.Base, 72}}));
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    auto Changed = F.Image;
    if (Mutation == 0)
      Changed.Segments.back().Data[4] ^= 4;
    if (Mutation == 1)
      Changed.Segments.back().Data[8] ^= 0x20;
    if (Mutation == 2)
      Changed.Sections.back().Flags = SegmentFlags::Readable;
    const auto Rebound = sdk::bindObjCSourceReferences(F.Function, Changed);
    EXPECT_TRUE(Rebound.LocalStorageExtents.empty()) << Mutation;
  }
}

TEST(ObjCStorage, StaticStringPairsRequireTheExactRuntimeConsumer) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    StaticStringPairs F;
    if (Mutation == 0)
      F.Image.DyldBindSlots[F.Slot].Module = "/tmp/impostor.dylib";
    if (Mutation == 1)
      F.Call->CallAddr += 8;
    if (Mutation == 2)
      F.Call->IsIndirectCall = true;
    if (Mutation == 3)
      F.Call->Operands.push_back(HighExpr::makeConst(0, 8));
    if (Mutation == 4)
      F.Call->Operands[1]->ConstProvenance = ConstantAddressProvenance::Scalar;
    if (Mutation == 5)
      F.Call->Operands[1]->AddressOwnerVA = F.Base;
    if (Mutation == 6)
      std::swap(F.Call->Operands[0], F.Call->Operands[1]);
    if (Mutation == 7) {
      auto Hint = *F.Call->SourceCallHint;
      Hint.Signature.Parameters[1].Location.RegisterOffset += 8;
      F.Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Hint);
    }
    const auto Bound = sdk::bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Bound.LocalStorageExtents.empty());
  }
}

struct StaticStrings : StaticStringPairs {
  explicit StaticStrings(unsigned Count = 5) : StaticStringPairs(Count) {
    Image.Symbols[0].Name = "_$s14StorageFixture10DeviceInfoC8prefixes_WZTv_";
    Image.Symbols[1].Addr = Base + 40 + Count * 16;
    Image.Symbols.push_back(
        {"_$s14StorageFixture10DeviceInfoC8prefixes_WZ", 0x3000, 4, true});
    Segment Code;
    Code.VA = 0x3000;
    Code.Size = Code.FileSz = 4;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data = {0xc0, 0x03, 0x5f, 0xd6};
    Image.Segments.push_back(Code);
    Section Text;
    Text.Name = "__text";
    Text.VA = Code.VA;
    Text.Size = Text.FileSz = Code.Size;
    Text.Flags = Code.Flags;
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Text);
  }
};

TEST(ObjCStorage, StaticOnceStringsShareBaseHeaderAndPayloadIdentity) {
  for (unsigned Count : {1U, 5U, 32U}) {
    StaticStrings F(Count);
    for (unsigned Offset : {0U, 8U, 40U}) {
      SCOPED_TRACE(Count);
      SCOPED_TRACE(Offset);
      auto Function = F.Function;
      if (Offset == 8)
        Function.Body[0].RetVal = F.Call;
      else
        Function.Body[0].RetVal = HighExpr::makeConst(
            F.Base + Offset, 8, ConstantAddressProvenance::DataAddress);
      const auto Bound = sdk::bindObjCSourceReferences(Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      EXPECT_EQ(Bound.LocalStorageExtents,
                (std::map<va_t, uint64_t>{{F.Base, 40 + Count * 16}}));
      auto Address = Offset == 8 ? Bound.Function.Body[0].RetVal->Operands[1]
                                 : Bound.Function.Body[0].RetVal;
      if (Offset) {
        ASSERT_EQ(Address->Kind, ExprKind::BinOp);
        EXPECT_EQ(Address->Operands[1]->ConstVal, Offset);
        Address = Address->Operands[0];
      }
      ASSERT_TRUE(Address->SourceCallHint);
      EXPECT_EQ(Address->SourceCallHint->TargetAddress, F.Base);
      EXPECT_TRUE(sdk::objcSourceCallBound(*Address, F.Image, {}));
      std::set<std::string> Helpers;
      const auto Source = sdk::renderObjCLocalStorageHelpers(
          F.Image, Bound.LocalStorageExtents, Helpers);
      EXPECT_EQ(Helpers.size(), 1U);
      EXPECT_NE(Source.find("storage[" + std::to_string(40 + Count * 16) + "]"),
                std::string::npos);
    }
  }
}

TEST(ObjCStorage, StaticOnceStringsRejectStaleLayoutAndInitializerIdentity) {
  StaticStrings F;
  auto Function = F.Function;
  Function.Body[0].RetVal = HighExpr::makeConst(
      F.Base + 40, 8, ConstantAddressProvenance::DataAddress);
  const auto Bound = sdk::bindObjCSourceReferences(Function, F.Image);
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  const auto Helper = Bound.Function.Body[0].RetVal->Operands[0];
  for (unsigned Mutation = 0; Mutation != 28; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = F.Image;
    auto *Bytes = Changed.Segments[0].Data.data() + F.Base - 0x1000;
    if (Mutation < 3)
      Bytes[Mutation * 8] = 1;
    if (Mutation == 3)
      Bytes[24] = 0;
    if (Mutation == 4)
      Bytes[32] = 12;
    if (Mutation == 5)
      Bytes[40] = 0x80;
    if (Mutation == 6)
      Bytes[55] = 0xf2;
    if (Mutation == 7)
      Bytes[54] = 1;
    if (Mutation == 8)
      Changed.DataPtrRelocSlots.insert(F.Base + 40);
    if (Mutation == 9)
      Changed.Symbols[1].Addr += 16;
    if (Mutation == 10)
      Changed.Symbols.push_back(Changed.Symbols[1]);
    if (Mutation == 11)
      Changed.Symbols.push_back({"_interior", F.Base + 48, 0, false});
    if (Mutation == 12)
      Changed.Exports.push_back({"_exported", 0, F.Base + 8});
    if (Mutation == 13)
      Changed.Symbols[0].Size = 128;
    if (Mutation == 14)
      Changed.Symbols[0].Name = "_ordinary_bytes";
    if (Mutation == 15)
      Changed.Symbols[0].Name =
          "_$s14StorageFixture10DeviceInfoC8prefixes_WZTv0_";
    if (Mutation == 16)
      Changed.Symbols[0].Name =
          "_$s14StorageFixture10DeviceInfoV8prefixes_WZTv_";
    if (Mutation == 17)
      Changed.Symbols.pop_back();
    if (Mutation == 18)
      Changed.Symbols.back().IsFunc = false;
    if (Mutation == 19)
      Changed.Symbols.push_back(Changed.Symbols.back());
    if (Mutation == 20)
      Changed.Segments.back().Flags =
          Changed.Segments.back().Flags | SegmentFlags::Writable;
    if (Mutation == 21)
      Changed.Sections[0].Flags = SegmentFlags::Readable;
    if (Mutation == 22)
      Changed.MachOChainedFixupsAmbiguous = true;
    if (Mutation == 23)
      Changed.Symbols.push_back(Changed.Symbols[0]);
    if (Mutation == 24)
      Changed.Symbols.push_back({"code_inside_data", F.Base + 48, 4, true});
    if (Mutation == 25)
      Changed.Symbols.push_back({"overlapping", F.Base - 8, 16, false});
    if (Mutation == 26) {
      auto Alias = Changed.Symbols[0];
      Alias.Addr += 256;
      Changed.Symbols.push_back(Alias);
    }
    if (Mutation == 27)
      Changed.DataPtrRelocSlots.insert(F.Base - 4);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Helper, Changed, {}));
    const auto Rebound = sdk::bindObjCSourceReferences(Function, Changed);
    EXPECT_FALSE(Rebound.Limitation.empty());
    EXPECT_TRUE(Rebound.LocalStorageExtents.empty());
  }
}

TEST(ObjCStorage, StaticOnceStringAddressArithmeticKeepsExactProvenance) {
  for (const auto Opcode : {NdOp::INT_ADD, NdOp::INT_SUB}) {
    for (bool Memory : {false, true}) {
      for (const auto Provenance :
           {ConstantAddressProvenance::Address,
            ConstantAddressProvenance::DataAddress,
            ConstantAddressProvenance::Scalar,
            ConstantAddressProvenance::Unknown,
            ConstantAddressProvenance::AddressFragment}) {
        SCOPED_TRACE(static_cast<int>(Opcode));
        SCOPED_TRACE(Memory);
        SCOPED_TRACE(static_cast<int>(Provenance));
        StaticStrings F;
        const auto Word = NdType::makeInt(8, false);
        F.Function.ReturnType = Word;
        F.Function.Params = {{"offset", Word}};
        MedVar Offset;
        Offset.Kind = MedVar::Param;
        Offset.Id = 0;
        Offset.Size = 8;
        auto Address = HighExpr::makeBinop(
            Opcode, HighExpr::makeConst(F.Base, 8, Provenance),
            HighExpr::makeVar(Offset, Word));
        F.Function.Body[0].RetVal =
            Memory ? HighExpr::makeLoad(Address, Word) : Address;
        const auto Bound = sdk::bindObjCSourceReferences(F.Function, F.Image);
        const bool Exact = Provenance == ConstantAddressProvenance::Address ||
                           Provenance == ConstantAddressProvenance::DataAddress;
        if (!Exact) {
          EXPECT_TRUE(Bound.LocalStorageExtents.empty());
          continue;
        }
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        EXPECT_EQ(Bound.LocalStorageExtents,
                  (std::map<va_t, uint64_t>{{F.Base, 120}}));
        const auto Result = Memory ? Bound.Function.Body[0].RetVal->Operands[0]
                                   : Bound.Function.Body[0].RetVal;
        ASSERT_EQ(Result->Kind, ExprKind::BinOp);
        EXPECT_EQ(Result->Op, Opcode);
        ASSERT_EQ(Result->Operands[0]->Kind, ExprKind::Call);
        EXPECT_TRUE(
            sdk::objcSourceCallBound(*Result->Operands[0], F.Image, {}));
      }
    }
  }
}

TEST(ObjCStorage, NamedReleaseStoresPreserveOrderingAndExactStorage) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const unsigned Width : {1U, 2U, 4U, 8U}) {
      for (const bool ExpressionStore : {false, true}) {
        SCOPED_TRACE(std::to_string(Width) + ":" +
                     std::to_string(ExpressionStore));
        StorageImage Fixture;
        Fixture.Image.Arch = Architecture;
        Symbol Storage;
        Storage.Name = "_published_value";
        Storage.Addr = 0x1e00;
        Storage.Size = 32;
        Fixture.Image.Symbols.push_back(Storage);
        HighFunc Function;
        Function.ReturnType = NdType::makeVoid();
        HighStmt Store;
        Store.Kind = StmtKind::Store;
        Store.StoreAddr = HighExpr::makeConst(0x1e08, 8);
        Store.StoreVal = HighExpr::makeConst(0x5a, Width);
        Store.MemoryOrdering = NdMemoryOrdering::Release;
        if (ExpressionStore) {
          auto Expression = std::make_shared<HighExpr>();
          Expression->Kind = ExprKind::Store;
          Expression->Type = Store.StoreVal->Type;
          Expression->Operands = {Store.StoreAddr, Store.StoreVal};
          Expression->MemoryOrdering = Store.MemoryOrdering;
          Store = {};
          Store.Kind = StmtKind::ExprStmt;
          Store.Val = Expression;
        }
        Function.Body = {Store};
        const auto Bound =
            sdk::bindObjCSourceReferences(Function, Fixture.Image);
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        EXPECT_EQ(Bound.LocalStorageExtents,
                  (std::map<va_t, uint64_t>{{0x1e00, 8 + Width}}));
        const auto &Result = Bound.Function.Body.front();
        EXPECT_EQ(ExpressionStore ? Result.Val->MemoryOrdering
                                  : Result.MemoryOrdering,
                  NdMemoryOrdering::Release);
        auto Address =
            ExpressionStore ? Result.Val->Operands[0] : Result.StoreAddr;
        ASSERT_EQ(Address->Kind, ExprKind::BinOp);
        ASSERT_EQ(Address->Operands.size(), 2U);
        const auto Helper = Address->Operands[0];
        ASSERT_TRUE(Helper->SourceCallHint);
        EXPECT_EQ(Helper->SourceCallHint->TargetAddress, 0x1e00U);
        EXPECT_TRUE(sdk::objcSourceCallBound(*Helper, Fixture.Image, {}));
        auto Changed = Fixture.Image;
        Changed.DataPtrRelocSlots.insert(0x1e08);
        EXPECT_FALSE(sdk::objcSourceCallBound(*Helper, Changed, {}));
      }
    }
  }
}

TEST(ObjCStorage, NamedReleaseStoresRejectUnprovedAlignmentAndStorage) {
  StorageImage Fixture;
  Symbol Storage;
  Storage.Name = "_published_value";
  Storage.Addr = 0x1e00;
  Storage.Size = 32;
  Fixture.Image.Symbols.push_back(Storage);
  HighFunc Function;
  Function.ReturnType = NdType::makeVoid();
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeConst(0x1e08, 8);
  Store.StoreVal = HighExpr::makeConst(0x5a, 8);
  Store.MemoryOrdering = NdMemoryOrdering::Release;
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    auto Image = Fixture.Image;
    auto Changed = Store;
    if (Mutation == 0)
      Changed.StoreAddr = HighExpr::makeConst(0x1e09, 8);
    if (Mutation == 1)
      Image.Symbols.back().Addr = 0x1e01;
    if (Mutation == 2) {
      Changed.StoreVal = HighExpr::makeConst(0, 8);
      Changed.StoreVal->Type = NdType::makeFloat(8);
    }
    if (Mutation == 3)
      Changed.StoreVal = HighExpr::makeConst(0, 16);
    if (Mutation == 4)
      Changed.MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Mutation == 5)
      Changed.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    if (Mutation == 6)
      Image.Symbols.clear();
    if (Mutation == 7)
      Image.Symbols.push_back(Image.Symbols.back());
    if (Mutation == 8)
      Image.DataPtrRelocSlots.insert(0x1e08);
    if (Mutation == 9)
      Image.Sections[0].Flags = SegmentFlags::Readable;
    if (Mutation == 10)
      Image.Symbols.back().Size = 12;
    if (Mutation == 11) {
      Changed.Kind = StmtKind::Return;
      Changed.RetVal =
          HighExpr::makeLoad(Changed.StoreAddr, NdType::makeInt(8));
      Changed.RetVal->MemoryOrdering = NdMemoryOrdering::Release;
      Changed.StoreAddr.reset();
      Changed.StoreVal.reset();
      Changed.MemoryOrdering = NdMemoryOrdering::None;
    }
    Function.Body = {Changed};
    EXPECT_FALSE(
        sdk::bindObjCSourceReferences(Function, Image).Limitation.empty())
        << Mutation;
  }
}

TEST(ObjCStorage, RuntimeSwiftOffsetsRetainIdentityWithoutInventingLayout) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    StorageImage Fixture;
    Fixture.Image.Arch = Architecture;
    Fixture.runtimeOffsets();
    // The segment's file range can include page padding over zero-fill
    // sections. Those bytes do not establish an initialized ivar offset.
    Fixture.Image.Segments[0].FileSz += 16;
    Fixture.Image.Segments[0].Data.resize(0x1010, 0xa5);
    parseObjCStorage(Fixture.Image);
    const auto &Class = Fixture.Image.ObjCClasses.front();
    ASSERT_EQ(Class.IvarStatus, "runtime");
    ASSERT_EQ(Class.Ivars.size(), 2U);
    EXPECT_FALSE(Class.Ivars[0].Offset);
    EXPECT_FALSE(Class.Ivars[1].Offset);
    EXPECT_EQ(Class.Ivars[0].Size, 8U);
    EXPECT_EQ(Class.Ivars[1].Size, 0U);
    const auto JSON = objcMetadataJSON(Fixture.Image);
    const auto *Classes = JSON.getArray("classes");
    ASSERT_NE(Classes, nullptr);
    const auto *Fields = Classes->front().getAsObject()->getArray("ivars");
    ASSERT_NE(Fields, nullptr);
    ASSERT_EQ(Fields->size(), 2U);
    for (const auto &Field : *Fields)
      EXPECT_TRUE(Field.getAsObject()->get("offset")->getAsNull());
    for (unsigned Width : {4U, 8U}) {
      HighFunc Function;
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x2000, 8),
                                         NdType::makeInt(Width));
      Function.Body.push_back(Return);
      const auto Bound = sdk::bindObjCSourceReferences(Function, Fixture.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      const auto &Expression = *Bound.Function.Body[0].RetVal;
      ASSERT_TRUE(Expression.SourceCallHint);
      EXPECT_EQ(Expression.SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeIvarOffset);
      EXPECT_EQ(Expression.SourceCallHint->OwnerClass, "StoredValues");
      EXPECT_EQ(Expression.SourceCallHint->TargetName, "first");
      EXPECT_TRUE(sdk::objcSourceCallBound(Expression, Fixture.Image, {}));
      auto Changed = Fixture.Image;
      Changed.ObjCSourceReferences.at(0x2000).Name = "opaque";
      EXPECT_FALSE(sdk::objcSourceCallBound(Expression, Changed, {}));
    }
  }
}

TEST(ObjCStorage, RuntimeOffsetIdentitiesRejectMalformedAndAmbiguousStorage) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(Mutation);
      StorageImage F;
      F.Image.Arch = Architecture;
      F.runtimeOffsets();
      auto &Slot = F.Image.Sections.back();
      switch (Mutation) {
      case 0:
        F.pointer(0x1120, 0x1200);
        break;
      case 1:
        Slot.Flags = SegmentFlags::Readable;
        break;
      case 2:
        Slot.Flags = SegmentFlags::Writable;
        break;
      case 3:
        Slot.Flags = Slot.Flags | SegmentFlags::Executable;
        break;
      case 4:
        Slot.Type = llvm::MachO::S_THREAD_LOCAL_ZEROFILL;
        break;
      case 5:
        Slot.FileSz = 8;
        break;
      case 6:
        Slot.Size = 8;
        break;
      case 7:
        F.Image.Segments[0].Size -= 8;
        break;
      case 8:
        F.Image.Segments[0].Flags = SegmentFlags::Readable;
        break;
      case 9:
        F.pointer(0x1308, 0x2001);
        break;
      case 10:
        F.pointer(0x1328, 0x2000);
        break;
      case 11:
        F.pointer(0x1330, 0x1500);
        break;
      case 12:
        F.Image.Sections[0].FileSz = 0x320;
        break;
      case 13:
        F.Image.Sections.push_back(Slot);
        break;
      case 14:
        F.Image.Segments.push_back(F.Image.Segments[0]);
        break;
      case 15:
        F.Image.ImportPtrSlots[0x2000] = "_foreign";
        break;
      case 16:
        F.Image.DataPtrRelocSlots.insert(0x1fff);
        break;
      case 17:
        F.pointer(0x1308, 0x1400);
        F.pointer(0x1400, UINT64_MAX);
        break;
      case 18:
        F.word(0x1324, UINT32_MAX);
        break;
      case 19:
        F.word(0x1320, 17);
        break;
      }
      parseObjCStorage(F.Image);
      EXPECT_EQ(F.Image.ObjCClasses.front().IvarStatus, "unresolved");
      EXPECT_TRUE(F.Image.ObjCClasses.front().Ivars.empty());
      EXPECT_TRUE(F.Image.ObjCSourceReferences.empty());
    }
}

TEST(ObjCStorage, StableSwiftPreservesWideOffsetsAndAbsentFieldTypes) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    StorageImage Fixture;
    Fixture.Image.Arch = Architecture;
    parseObjCStorage(Fixture.Image);
    const auto &Class = Fixture.Image.ObjCClasses.front();
    ASSERT_EQ(Class.IvarStatus, "recovered");
    ASSERT_EQ(Class.Ivars.size(), 2U);
    EXPECT_EQ(Class.Ivars[0].TypeEncoding, "d");
    EXPECT_TRUE(Class.Ivars[1].TypeEncoding.empty());
    EXPECT_EQ(Class.Ivars[1].Size, 16U);
    ASSERT_EQ(Fixture.Image.ObjCSourceReferences.size(), 2U);
    EXPECT_EQ(Fixture.Image.ObjCSourceReferences.at(0x1400).Size, 8U);
    for (unsigned Width : {4U, 8U}) {
      HighFunc Function;
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x1400, 8),
                                         NdType::makeInt(Width));
      Function.Body.push_back(Return);
      auto Bound = sdk::bindObjCSourceReferences(Function, Fixture.Image);
      EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_TRUE(Bound.Function.Body[0].RetVal->SourceCallHint);
      EXPECT_EQ(Bound.Function.Body[0].RetVal->SourceCallHint->CallKind,
                SourceCallTypeHint::Kind::RuntimeIvarOffset);
      EXPECT_EQ(Bound.Function.Body[0].RetVal->SourceCallHint->TargetName,
                "first");
    }
  }
}

TEST(ObjCStorage, ProtocolSlotsRequireCompleteLocalRuntimeDeclarations) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Case = 0; Case < 11; ++Case) {
      SCOPED_TRACE(Case);
      StorageImage F;
      F.Image.Arch = Architecture;
      F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = 0x800;
      Section References;
      References.Name = "__objc_protorefs";
      References.VA = References.FileOff = 0x1800;
      References.Size = References.FileSz = 8;
      References.Flags = SegmentFlags::Readable;
      F.Image.Sections.push_back(References);
      ObjCProtocol Protocol;
      Protocol.Address = 0x1700;
      Protocol.Name = "ValueProtocol";
      Protocol.Status = "recovered";
      F.Image.ObjCProtocols.push_back(Protocol);
      F.pointer(0x1800, 0x1700);
      switch (Case) {
      case 0:
        break;
      case 1:
        F.Image.ObjCProtocols[0].Status = "invalid_metadata";
        break;
      case 2:
        F.Image.ObjCProtocols[0].Status = "invalid_inheritance";
        break;
      case 3:
        F.Image.ObjCProtocols.clear();
        break;
      case 4:
        Protocol.Address += 8;
        Protocol.Status = "invalid_metadata";
        F.Image.ObjCProtocols.push_back(Protocol);
        break;
      case 5:
        Protocol.Name = "Other";
        F.Image.ObjCProtocols.push_back(Protocol);
        break;
      case 6:
        F.Image.MachOHasChainedFixups = true;
        break;
      case 7:
        F.Image.ImportPtrSlots[0x1800] = "_OBJC_PROTOCOL_$_ValueProtocol";
        break;
      case 8:
        F.Image.ConflictingImportStorageSlots.insert(0x1800);
        break;
      case 9:
        F.Image.Sections.back().FileSz = 4;
        break;
      case 10:
        F.Image.Sections.back().Flags =
            SegmentFlags::Readable | SegmentFlags::Executable;
        break;
      }
      parseObjCStorage(F.Image);
      const auto Found = F.Image.ObjCSourceReferences.find(0x1800);
      if (Case) {
        EXPECT_EQ(Found, F.Image.ObjCSourceReferences.end());
        continue;
      }
      ASSERT_NE(Found, F.Image.ObjCSourceReferences.end());
      EXPECT_EQ(Found->second.TheKind, ObjCSourceReference::Kind::Protocol);
      EXPECT_EQ(Found->second.Name, "ValueProtocol");
      HighFunc Function;
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x1800, 8),
                                         NdType::makePtr(NdType::makeVoid()));
      Function.Body = {Return};
      const auto Bound = sdk::bindObjCSourceReferences(Function, F.Image);
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      ASSERT_TRUE(Bound.Function.Body[0].RetVal->SourceCallHint);
      EXPECT_EQ(Bound.RuntimeProtocols, std::set<std::string>{"ValueProtocol"});
      EXPECT_TRUE(sdk::objcSourceCallBound(*Bound.Function.Body[0].RetVal,
                                           F.Image, {}));
      auto Hint = std::make_shared<SourceCallTypeHint>(
          *Bound.Function.Body[0].RetVal->SourceCallHint);
      Hint->TargetName = "Other";
      Bound.Function.Body[0].RetVal->SourceCallHint = Hint;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Bound.Function.Body[0].RetVal,
                                            F.Image, {}));
    }
  }
}

TEST(ObjCStorage, DuplicateProtocolReferencesKeepOneRegisteredIdentity) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    StorageImage F;
    F.Image.Arch = Architecture;
    F.Image.Sections[0].Size = F.Image.Sections[0].FileSz = 0x800;
    Section References;
    References.Name = "__objc_protorefs";
    References.VA = References.FileOff = 0x1800;
    References.Size = References.FileSz = 16;
    References.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    F.Image.Sections.push_back(References);
    for (unsigned Index = 0; Index != 2; ++Index) {
      ObjCProtocol Protocol;
      Protocol.Address = 0x1700 + Index * 8;
      Protocol.Name = "ValueProtocol";
      Protocol.Status = "recovered";
      ObjCProtocolMethod Method;
      Method.MetadataAddress = 0x1600 + Index * 32;
      Method.Selector = "measure:";
      Method.TypeEncoding = Index ? "q24@0:8q16" : "i24@0:8i16";
      Method.Status = "supported";
      Method.TypeHint =
          parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
      ASSERT_TRUE(Method.TypeHint);
      std::string Diagnostic;
      ASSERT_TRUE(
          assignDarwinObjCSourceABI(*Method.TypeHint, Architecture, Diagnostic))
          << Diagnostic;
      Protocol.Methods.push_back(std::move(Method));
      F.pointer(0x1800 + Index * 8, Protocol.Address);
      F.Image.ObjCProtocols.push_back(std::move(Protocol));
    }
    for (unsigned Order = 0; Order != 2; ++Order) {
      parseObjCStorage(F.Image);
      // The runtime registry supplies the same object to both references.
      // Conflicting method ABIs still have no selected declaration.
      EXPECT_FALSE(objcSelectorSourceTypeHint(F.Image, "measure:"));
      for (va_t Slot : {0x1800U, 0x1808U}) {
        const auto Found = F.Image.ObjCSourceReferences.find(Slot);
        ASSERT_NE(Found, F.Image.ObjCSourceReferences.end());
        EXPECT_EQ(Found->second.TheKind, ObjCSourceReference::Kind::Protocol);
        EXPECT_EQ(Found->second.Name, "ValueProtocol");
        HighFunc Function;
        HighStmt Return;
        Return.Kind = StmtKind::Return;
        Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(Slot, 8),
                                           NdType::makePtr(NdType::makeVoid()));
        Function.Body = {Return};
        const auto Bound = sdk::bindObjCSourceReferences(Function, F.Image);
        ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        ASSERT_TRUE(Bound.Function.Body[0].RetVal->SourceCallHint);
        EXPECT_EQ(Bound.RuntimeProtocols,
                  std::set<std::string>{"ValueProtocol"});
        EXPECT_TRUE(sdk::objcSourceCallBound(*Bound.Function.Body[0].RetVal,
                                             F.Image, {}));
        F.Image.ObjCSourceReferences.at(Slot).Name = "OtherProtocol";
        EXPECT_FALSE(sdk::objcSourceCallBound(*Bound.Function.Body[0].RetVal,
                                              F.Image, {}));
      }
      std::reverse(F.Image.ObjCProtocols.begin(), F.Image.ObjCProtocols.end());
    }
    F.Image.ObjCProtocols[1].Status = "invalid_inheritance";
    parseObjCStorage(F.Image);
    EXPECT_FALSE(F.Image.ObjCSourceReferences.count(0x1800));
    EXPECT_FALSE(F.Image.ObjCSourceReferences.count(0x1808));
  }
}

TEST(ObjCStorage, ImportedClassSlotsDoNotRequireAClassReferenceSection) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Metaclass : {false, true}) {
      for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
        StorageImage Fixture;
        auto &Image = Fixture.Image;
        Image.Arch = Architecture;
        Image.ObjCClasses.clear();
        Image.Sections.front().Name = "__got";
        const std::string Name =
            Metaclass ? "_OBJC_METACLASS_$_Example" : "_OBJC_CLASS_$_Example";
        Image.ImportPtrSlots[0x1800] = Name;
        Image.DyldBindSlots[0x1800] = {Name, 0};
        if (Mutation == 1)
          Image.DyldBindSlots[0x1800].WeakImport = true;
        if (Mutation == 2)
          Image.DyldBindSlots[0x1800].Addend = 8;
        if (Mutation == 3)
          Image.ConflictingImportStorageSlots.insert(0x1800);
        if (Mutation == 4)
          Image.ImportStorageSlots[0x1800] = {"_other", 0};
        if (Mutation == 5)
          Image.Sections.front().Flags =
              Image.Sections.front().Flags | SegmentFlags::Executable;
        if (Mutation == 6)
          Image.Sections.front().FileSz = 0x804;
        if (Mutation == 7)
          Image.DyldBindSlots[0x1800].Name = "_different";
        parseObjCStorage(Image);
        const auto Found = Image.ObjCSourceReferences.find(0x1800);
        if (Mutation) {
          EXPECT_EQ(Found, Image.ObjCSourceReferences.end()) << Mutation;
          continue;
        }
        ASSERT_NE(Found, Image.ObjCSourceReferences.end());
        EXPECT_EQ(Found->second.Name, "Example");
        EXPECT_EQ(Found->second.TheKind,
                  Metaclass ? ObjCSourceReference::Kind::Metaclass
                            : ObjCSourceReference::Kind::Class);
        HighFunc Function;
        HighStmt Return;
        Return.Kind = StmtKind::Return;
        Return.RetVal = HighExpr::makeLoad(HighExpr::makeConst(0x1800, 8),
                                           NdType::makePtr(NdType::makeVoid()));
        Function.Body.push_back(Return);
        const auto Bound = sdk::bindObjCSourceReferences(Function, Image);
        EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
        ASSERT_TRUE(Bound.Function.Body[0].RetVal->SourceCallHint);
        EXPECT_EQ(Bound.Function.Body[0].RetVal->SourceCallHint->TargetName,
                  "Example");
      }
    }
  }
}

TEST(ObjCStorage, Arm64ObjCOffsetsDoNotBorrowTheFollowingWord) {
  StorageImage Fixture;
  Fixture.pointer(0x1120, 0x1200);
  Fixture.string(0x1560, "q");
  Fixture.word(0x1344, 8); // Second ivar size, at entry + 28.
  Fixture.word(0x1404, UINT32_MAX);
  parseObjCStorage(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCClasses.front().IvarStatus, "recovered");
  EXPECT_EQ(Fixture.Image.ObjCSourceReferences.at(0x1400).Size, 4U);
  HighFunc Function;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal =
      HighExpr::makeLoad(HighExpr::makeConst(0x1400, 8), NdType::makeInt(8));
  Function.Body.push_back(Return);
  EXPECT_FALSE(sdk::bindObjCSourceReferences(Function, Fixture.Image)
                   .Limitation.empty());
}

TEST(ObjCStorage, UnprovenEmptyTypesAndTruncatedWideOffsetsRemainRejected) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    StorageImage Fixture;
    if (Mutation == 0)
      Fixture.pointer(0x1120, 0x1200);
    if (Mutation == 1)
      Fixture.pointer(0x1120, 0x1201); // Legacy Swift is a separate ABI.
    if (Mutation == 2)
      Fixture.word(0x1404, 1); // Must not truncate the live 64-bit offset.
    if (Mutation == 3)
      Fixture.pointer(0x1338, 0); // Null type pointer is not an empty string.
    if (Mutation == 4)
      Fixture.string(0x1540, ""); // Field names always require identity.
    if (Mutation == 5)
      Fixture.pointer(0x1308, 0x1ffc); // Only four offset bytes are mapped.
    parseObjCStorage(Fixture.Image);
    EXPECT_EQ(Fixture.Image.ObjCClasses.front().IvarStatus, "unresolved")
        << Mutation;
    EXPECT_TRUE(Fixture.Image.ObjCSourceReferences.empty()) << Mutation;
  }
}
} // namespace
