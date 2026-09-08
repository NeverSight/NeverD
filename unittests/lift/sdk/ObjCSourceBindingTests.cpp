#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

using namespace neverd;
using namespace neverd::sdk;
namespace {
struct Fixture {
  BinaryImage Image;
  HighFunc Function;
  Fixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Mapping;
    Mapping.VA = 0x1000;
    Mapping.Size = Mapping.FileSz = 0x100;
    Mapping.Flags = SegmentFlags::Readable;
    Mapping.Data.resize(0x100);
    Image.Segments.push_back(Mapping);
    Section Data;
    Data.VA = 0x1000;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Data);
    Image.ObjCSourceReferences.emplace(
        0x1010,
        ObjCSourceReference{
            ObjCSourceReference::Kind::Selector, 0x1010, 8, "step:", {}});
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal =
        HighExpr::makeLoad(HighExpr::makeConst(0x1010, 8), NdType::makeInt(8));
    Function.Body.push_back(Return);
  }
};
} // namespace

TEST(ObjCSourceBindings, ReplacesLoadedSelectorAndKeepsOriginalProjection) {
  Fixture F;
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  auto Call = Result.Function.Body[0].RetVal;
  ASSERT_EQ(Call->Kind, ExprKind::Call);
  ASSERT_TRUE(Call->SourceCallHint);
  EXPECT_EQ(Call->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeSelector);
  EXPECT_EQ(Call->SourceCallHint->TargetName, "step:");
  EXPECT_TRUE(Call->Operands.empty());
  EXPECT_EQ(F.Function.Body[0].RetVal->Kind, ExprKind::Load);
  EXPECT_EQ(F.Function.Body[0].RetVal->Operands[0]->ConstVal, 0x1010u);
  EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, {}));
}

TEST(ObjCSourceBindings, SlotAddressIsNotTheRuntimeValue) {
  Fixture F;
  F.Function.Body[0].RetVal = HighExpr::makeConst(0x1010, 8);
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_EQ(Result.Function.Body[0].RetVal->Kind, ExprKind::Const);
  F.Function.Body[0].RetVal =
      HighExpr::makeLoad(HighExpr::makeConst(0x1014, 8), NdType::makeInt(4));
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, OrderedOrWrongWidthLoadsDoNotBecomeQueries) {
  Fixture F;
  F.Function.Body[0].RetVal->Type = NdType::makeInt(4);
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
  F.Function.Body[0].RetVal->Type = NdType::makeInt(8);
  F.Function.Body[0].RetVal->MemoryOrdering = NdMemoryOrdering::Acquire;
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, IvarCarrierPreservesReadWidthAndDeclaringClass) {
  Fixture F;
  F.Image.ObjCSourceReferences[0x1010] = {ObjCSourceReference::Kind::IvarOffset,
                                          0x1010, 8, "_wide", "Base"};
  for (uint16_t Width : {4, 8}) {
    F.Function.Body[0].RetVal->Type = NdType::makeInt(Width);
    auto Result = bindObjCSourceReferences(F.Function, F.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    ASSERT_TRUE(Result.Function.Body[0].RetVal->SourceCallHint);
    EXPECT_EQ(Result.Function.Body[0]
                  .RetVal->SourceCallHint->Signature.ReturnType->Size,
              Width);
    EXPECT_EQ(Result.InstanceLayoutClasses, std::set<std::string>{"Base"});
    EXPECT_TRUE(
        objcSourceCallBound(*Result.Function.Body[0].RetVal, F.Image, {}));
  }
  F.Image.ObjCSourceReferences[0x1010].Size = 4;
  EXPECT_FALSE(
      bindObjCSourceReferences(F.Function, F.Image).Limitation.empty());
}

TEST(ObjCSourceBindings, ForgedRuntimeIdentityCannotReuseAnotherSlot) {
  Fixture F;
  auto Result = bindObjCSourceReferences(F.Function, F.Image);
  auto Expression = Result.Function.Body[0].RetVal;
  auto Hint = std::make_shared<SourceCallTypeHint>(*Expression->SourceCallHint);
  Hint->TargetName = "different:";
  Expression->SourceCallHint = Hint;
  EXPECT_FALSE(objcSourceCallBound(*Expression, F.Image, {}));
}

TEST(ObjCSourceBindings, ExplicitABIPositionDriftIsDetected) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = NdType::makeInt(8);
  Hint.Parameters = {{"objc_self", NdType::makePtr(NdType::makeVoid())},
                     {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
  std::string Reason;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Reason));
  auto Changed = Hint;
  Changed.Parameters[1].Location.RegisterOffset += 8;
  EXPECT_FALSE(objc_projection_detail::sameHint(Hint, Changed));
  Changed = Hint;
  Changed.ReturnLocation.RegisterOffset += 8;
  EXPECT_FALSE(objc_projection_detail::sameHint(Hint, Changed));
}

namespace {
struct ObjectFixture {
  BinaryImage Image;
  static constexpr va_t ClassAddress = 0x2020;
  static constexpr va_t MetaAddress = 0x2080;
  static constexpr va_t ClassSlot = 0x2010;
  ObjectFixture() {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Segment;
    Segment.VA = 0x2000;
    Segment.Size = Segment.FileSz = 0x1000;
    Segment.Flags = SegmentFlags::Readable;
    Segment.Data.resize(0x1000);
    Image.Segments.push_back(Segment);
    Section Section;
    Section.VA = 0x2000;
    Section.Size = Section.FileSz = 0x1000;
    Section.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Section);
    put(ClassAddress, MetaAddress);
    put(ClassAddress + 32, 0x2200);
    put(MetaAddress + 32, 0x2280);
    put(0x2280, 1); // RO_META
    put(0x2218, 0x2380);
    put(0x2298, 0x2380);
    const char Name[] = "Receiver";
    std::copy(std::begin(Name), std::end(Name),
              Image.Segments[0].Data.begin() + 0x380);
    ObjCClass Class;
    Class.Address = ClassAddress;
    Class.Name = "Receiver";
    Image.ObjCClasses.push_back(Class);
    Image.ObjCSourceReferences[ClassSlot] = {
        ObjCSourceReference::Kind::Class, ClassSlot, 8, "Receiver", {}};
  }
  void put(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(
        Image.Segments[0].Data.data() + Address - 0x2000, Value);
  }
  HighFunc message(ExprPtr Receiver) {
    auto Binding = std::make_shared<SourceCallTypeHint>();
    Binding->CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Binding->TargetName = "objc_msgSend";
    Binding->Selector = "answer";
    Binding->Signature.ReturnType = NdType::makeInt(4);
    Binding->Signature.Parameters = {
        {"objc_self", NdType::makePtr(NdType::makeVoid())},
        {"objc_cmd", NdType::makePtr(NdType::makeVoid())}};
    std::string Error;
    EXPECT_TRUE(
        assignDarwinObjCSourceABI(Binding->Signature, Image.Arch, Error));
    auto Call = HighExpr::makeCall("objc_msgSend", 0,
                                   {Receiver, HighExpr::makeConst(0, 8)});
    Call->SourceCallHint = Binding;
    HighFunc Function;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = Call;
    Function.Body.push_back(Return);
    return Function;
  }
};
} // namespace

TEST(ObjCSourceBindings,
     DirectClassAndMetaclassReceiverUseVerifiedObjectIdentity) {
  ObjectFixture Fixture;
  for (va_t Address :
       {ObjectFixture::ClassAddress, ObjectFixture::MetaAddress}) {
    auto Original = Fixture.message(HighExpr::makeConst(Address, 8));
    auto Result = bindObjCSourceReferences(Original, Fixture.Image);
    EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
    auto Receiver = Result.Function.Body[0].RetVal->Operands[0];
    ASSERT_TRUE(Receiver->SourceCallHint);
    EXPECT_EQ(Receiver->SourceCallHint->TargetName, "Receiver");
    EXPECT_EQ(Receiver->SourceCallHint->CallKind,
              Address == ObjectFixture::ClassAddress
                  ? SourceCallTypeHint::Kind::RuntimeClass
                  : SourceCallTypeHint::Kind::RuntimeMetaclass);
    EXPECT_TRUE(objcSourceCallBound(*Receiver, Fixture.Image, {}));
    EXPECT_EQ(Original.Body[0].RetVal->Operands[0]->Kind, ExprKind::Const);
  }
}

TEST(ObjCSourceBindings,
     ObjectReceiverRewriteDoesNotAffectSharedScalarConstant) {
  ObjectFixture Fixture;
  auto Shared = HighExpr::makeConst(ObjectFixture::ClassAddress, 8);
  auto Function = Fixture.message(Shared);
  HighStmt Scalar;
  Scalar.Kind = StmtKind::Return;
  Scalar.RetVal = Shared;
  Function.Body.push_back(Scalar);
  auto Result = bindObjCSourceReferences(Function, Fixture.Image);
  ASSERT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind, ExprKind::Call);
  EXPECT_EQ(Result.Function.Body[1].RetVal->Kind, ExprKind::Const);
  EXPECT_FALSE(Result.Limitation.empty());
}

TEST(ObjCSourceBindings, ClassrefSlotAddressCannotBecomeClassObjectReceiver) {
  ObjectFixture Fixture;
  auto Function =
      Fixture.message(HighExpr::makeConst(ObjectFixture::ClassSlot, 8));
  auto Result = bindObjCSourceReferences(Function, Fixture.Image);
  EXPECT_FALSE(Result.Limitation.empty());
  EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind, ExprKind::Const);
  Function = Fixture.message(HighExpr::makeLoad(
      HighExpr::makeConst(ObjectFixture::ClassSlot, 8), NdType::makeInt(8)));
  Result = bindObjCSourceReferences(Function, Fixture.Image);
  EXPECT_TRUE(Result.Limitation.empty()) << Result.Limitation;
  auto Receiver = Result.Function.Body[0].RetVal->Operands[0];
  ASSERT_TRUE(Receiver->SourceCallHint);
  EXPECT_EQ(Receiver->SourceCallHint->TargetAddress, ObjectFixture::ClassSlot);
}

TEST(ObjCSourceBindings, MetaObjectNeedsResolvedIsaAndMatchingMetadata) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    ObjectFixture Fixture;
    if (Mutation == 0)
      Fixture.Image.MachOHasChainedFixups = true;
    if (Mutation == 1)
      Fixture.put(0x2280, 0); // A class is not a metaclass.
    if (Mutation == 2)
      Fixture.Image.ObjCClasses.push_back(Fixture.Image.ObjCClasses[0]);
    if (Mutation == 3)
      ++Fixture.Image.Sections[0].FileOff;
    auto Function =
        Fixture.message(HighExpr::makeConst(ObjectFixture::MetaAddress, 8));
    auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_FALSE(Result.Limitation.empty()) << Mutation;
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
  }
  ObjectFixture Fixture;
  Fixture.Image.MachOHasChainedFixups = true;
  Fixture.Image.MachOResolvedChainedPointerSlots = {
      ObjectFixture::ClassAddress, ObjectFixture::ClassAddress + 32,
      ObjectFixture::MetaAddress + 32, 0x2218, 0x2298};
  auto Function =
      Fixture.message(HighExpr::makeConst(ObjectFixture::MetaAddress, 8));
  EXPECT_TRUE(
      bindObjCSourceReferences(Function, Fixture.Image).Limitation.empty());
}

TEST(ObjCSourceBindings,
     NativeOrSuperStructurePointersAreNotMessageObjectReceivers) {
  ObjectFixture Fixture;
  for (auto Kind : {SourceCallTypeHint::Kind::Native,
                    SourceCallTypeHint::Kind::ObjCSuper2}) {
    auto Function =
        Fixture.message(HighExpr::makeConst(ObjectFixture::ClassAddress, 8));
    auto Binding = std::make_shared<SourceCallTypeHint>(
        *Function.Body[0].RetVal->SourceCallHint);
    Binding->CallKind = Kind;
    Function.Body[0].RetVal->SourceCallHint = Binding;
    auto Result = bindObjCSourceReferences(Function, Fixture.Image);
    EXPECT_EQ(Result.Function.Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
    EXPECT_FALSE(Result.Limitation.empty());
  }
}
