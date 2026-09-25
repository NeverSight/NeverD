#include "../../../lib/sdk/capi/ObjCBlockSources.h"
#include "gtest/gtest.h"

#include "neverd/loader/ObjC/ObjCEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include <tuple>
using namespace neverd;
using namespace neverd::sdk;
namespace {
struct BlockFixture {
  BinaryImage Image;
  static constexpr va_t Literal = 0x2100;
  static constexpr va_t Descriptor = 0x2200;
  static constexpr va_t Signature = 0x2300;
  static constexpr va_t Layout = 0x2400;
  static constexpr va_t Invoke = 0x1100;

  BlockFixture() {
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    Segment Segment;
    Segment.VA = 0x1000;
    Segment.Size = Segment.FileSz = 0x1000;
    Segment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Segment.Data.resize(0x1000);
    Image.Segments.push_back(std::move(Segment));
    neverd::Segment DataSegment;
    DataSegment.VA = 0x2000;
    DataSegment.Size = DataSegment.FileSz = 0x3000;
    DataSegment.FileOff = 0x1000;
    DataSegment.Flags = SegmentFlags::Readable;
    DataSegment.Data.resize(0x3000);
    Image.Segments.push_back(std::move(DataSegment));
    Section Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x1000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Text);
    Section Data;
    Data.VA = 0x2000;
    Data.Size = Data.FileSz = 0x3000;
    Data.FileOff = 0x1000;
    Data.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Data);
    Image.ImportPtrSlots[Literal] = "__NSConcreteGlobalBlock";
    put32(Literal + 8, 0x50000000);
    put64(Literal + 16, Invoke);
    put64(Literal + 24, Descriptor);
    put64(Descriptor, 0);
    put64(Descriptor + 8, 32);
    put64(Descriptor + 16, Signature);
    string(Signature, "i12@?0i8");
  }

  void put32(va_t Address, uint32_t Value) {
    llvm::support::endian::write32le(
        Image.Segments[1].Data.data() + Address - 0x2000, Value);
  }
  void put64(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(
        Image.Segments[1].Data.data() + Address - 0x2000, Value);
  }
  void string(va_t Address, llvm::StringRef Value) {
    auto *Start = Image.Segments[1].Data.data() + Address - 0x2000;
    std::copy(Value.bytes_begin(), Value.bytes_end(), Start);
    Start[Value.size()] = 0;
  }
  std::optional<ObjCBlockLiteral> read(std::string &Error) {
    return readObjCBlockLiteral(Image, Literal, Error);
  }
};

TEST(ObjCBlockSources, CallbackClassRequiresExactPublishedDescriptor) {
  BlockFixture F;
  F.string(F.Signature, "v24@?0@\"WMFFeedNewsStory\"8Q16");
  PipelineResult Result;
  Result.SourceImage = &F.Image;
  const auto Plan = discoverObjCBlockSources(F.Image, Result);
  ASSERT_EQ(Plan.ParameterReceivers.count(F.Invoke), 1U);
  ASSERT_EQ(Plan.ParameterReceivers.at(F.Invoke).count(1), 1U);
  const auto Root = Plan.ParameterReceivers.at(F.Invoke).at(1);
  EXPECT_EQ(Root.ClassName, "WMFFeedNewsStory");
  EXPECT_EQ(Root.Address, F.Invoke);
  EXPECT_EQ(Root.BlockDescriptorAddress, F.Descriptor);
  EXPECT_TRUE(objcReceiverTypeHintValid(F.Image, Root));

  auto RejectedBody = Plan;
  RejectedBody.Rejections[F.Invoke] = "nested block consumer is unresolved";
  EXPECT_TRUE(objc_block_source_detail::publish(
      RejectedBody, Plan.Globals.at(F.Literal).Descriptor, F.Invoke));
  EXPECT_EQ(objcBlockParameterReceivers(F.Image, RejectedBody)
                .at(F.Invoke)
                .at(1),
            Root);
  RejectedBody.InvalidInvokeDescriptors.insert(F.Invoke);
  EXPECT_TRUE(objcBlockParameterReceivers(F.Image, RejectedBody).empty());

  auto WrongDescriptor = Root;
  ++WrongDescriptor.BlockDescriptorAddress;
  EXPECT_FALSE(objcReceiverTypeHintValid(F.Image, WrongDescriptor));
  auto WrongClass = Root;
  WrongClass.ClassName = "OtherStory";
  EXPECT_FALSE(objcReceiverTypeHintValid(F.Image, WrongClass));

  F.put32(F.Literal + 8, 0x40000000); // no global-block storage flag
  EXPECT_TRUE(
      discoverObjCBlockSources(F.Image, Result).ParameterReceivers.empty());
  F.put32(F.Literal + 8, 0x50000000);
  F.string(F.Signature, "v24@?0@8Q16"); // bare id has no class
  EXPECT_TRUE(
      discoverObjCBlockSources(F.Image, Result).ParameterReceivers.empty());
}

ExprPtr parameter(unsigned Index, TypeRef Type) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Index;
  V.Size = Type->Size;
  return HighExpr::makeVar(V, Type);
}
ExprPtr frame(const BinaryImage &Image, int64_t Offset) {
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.RegOff = getTargetRegInfo(Image.Arch).StackPointer;
  SP.Size = 8;
  return HighExpr::makeBinop(NdOp::INT_ADD,
                             HighExpr::makeVar(SP, NdType::makeInt(8)),
                             HighExpr::makeConst(Offset, 8));
}
HighStmt store(ExprPtr Address, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Store;
  S.StoreAddr = Address;
  S.StoreVal = Value;
  return S;
}
HighStmt ret(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = Value;
  return S;
}
struct SourceFixture : BlockFixture {
  PipelineResult Result;
  static constexpr va_t Caller = 0x1200, Consumer = 0x1300, StackIsa = 0x2500;
  SourceFixture(bool Stack, Arch Architecture = Arch::AArch64) {
    Image.Arch = Architecture;
    if (Stack) {
      Image.ImportPtrSlots.erase(Literal);
      Image.ImportPtrSlots[StackIsa] = "__NSConcreteStackBlock";
      put64(Descriptor + 8, 40);
      put64(Descriptor + 24, 0);
    }
    std::string Error;
    auto D = readObjCBlockDescriptor(Image, Descriptor,
                                     Stack ? 0xc0000000 : 0x50000000, Error);
    EXPECT_TRUE(D) << Error;
    auto make = [&](va_t Entry) {
      HighFunc F;
      F.Entry = Entry;
      F.Name = "sub_" + llvm::utohexstr(Entry, true);
      F.SourceTypeHint = *D->InvokeTypeHint;
      F.ReturnType = F.SourceTypeHint->ReturnType;
      for (auto &P : F.SourceTypeHint->Parameters)
        F.Params.push_back({P.Name, P.Type});
      return F;
    };
    auto Inv = make(Invoke);
    auto Value = parameter(1, NdType::makeInt(4, true));
    if (Stack)
      Value = HighExpr::makeBinop(
          NdOp::INT_ADD, Value,
          HighExpr::makeLoad(
              HighExpr::makeBinop(
                  NdOp::INT_ADD,
                  parameter(0, NdType::makePtr(NdType::makeVoid())),
                  HighExpr::makeConst(32, 8)),
              NdType::makeInt(4)));
    Inv.Body.push_back(ret(Value));
    Result.HighFuncs.push_back(Inv);
    auto Use = make(Consumer);
    auto Call = HighExpr::makeCall(
        {}, 0,
        {parameter(0, Use.Params[0].Type), parameter(1, Use.Params[1].Type)});
    auto B = std::make_shared<SourceCallTypeHint>();
    B->CallKind = SourceCallTypeHint::Kind::BlockInvoke;
    B->Signature = *D->InvokeTypeHint;
    Call->SourceCallHint = B;
    Call->Type = Use.ReturnType;
    Use.Body.push_back(ret(Call));
    Result.HighFuncs.push_back(Use);
    auto Build = make(Caller);
    Build.FrameSize = 64;
    if (Stack) {
      Build.Body.push_back(
          store(frame(Image, -48),
                HighExpr::makeLoad(HighExpr::makeConst(StackIsa, 8),
                                   NdType::makePtr(NdType::makeVoid()))));
      Build.Body.push_back(
          store(frame(Image, -40), HighExpr::makeConst(0xc0000000, 8)));
      Build.Body.push_back(
          store(frame(Image, -32), HighExpr::makeConst(Invoke, 8)));
      Build.Body.push_back(
          store(frame(Image, -24), HighExpr::makeConst(Descriptor, 8)));
      Build.Body.push_back(
          store(frame(Image, -16), parameter(1, NdType::makeInt(4))));
    }
    auto Apply = HighExpr::makeCall(
        {}, Consumer,
        {Stack ? frame(Image, -48) : HighExpr::makeConst(Literal, 8),
         parameter(1, NdType::makeInt(4))});
    auto A = std::make_shared<SourceCallTypeHint>();
    A->CallKind = SourceCallTypeHint::Kind::Native;
    A->TargetAddress = Consumer;
    A->Signature = *Use.SourceTypeHint;
    Apply->SourceCallHint = A;
    Apply->Type = Build.ReturnType;
    Build.Body.push_back(ret(Apply));
    Result.HighFuncs.push_back(Build);
    Result.SourceImage = &Image;
  }
  HighFunc &caller() { return Result.HighFuncs[2]; }
  std::map<va_t, const HighFunc *> functions() {
    std::map<va_t, const HighFunc *> F;
    for (auto &Function : Result.HighFuncs)
      F.emplace(Function.Entry, &Function);
    return F;
  }
};
struct OwnedSourceFixture : SourceFixture {
  static constexpr va_t Copy = 0x1400, Dispose = 0x1410, Flags = 0x2600,
                        CopyImport = 0x2700, RetainImport = 0x2710,
                        ReleaseImport = 0x2720;
  OwnedSourceFixture(Arch Architecture) : SourceFixture(true, Architecture) {
    put64(Descriptor + 16, Copy);
    put64(Descriptor + 24, Dispose);
    put64(Descriptor + 32, Signature);
    put64(Descriptor + 40, 0x100);
    put64(Flags, 0xc2000000);
    caller().Body[1].StoreVal =
        HighExpr::makeLoad(HighExpr::makeConst(Flags, 8), NdType::makeInt(8));
    caller().Body[4].StoreVal =
        parameter(0, NdType::makePtr(NdType::makeVoid()));
    Image.ImportPtrSlots[CopyImport] = "_objc_retainBlock";
    Image.ImportPtrSlots[RetainImport] = "_objc_retain";
    Image.ImportPtrSlots[ReleaseImport] = "_objc_release";
    auto CopyCall =
        HighExpr::makeCall("objc_retainBlock", 0, {frame(Image, -48)});
    CopyCall->SourceCallHint = std::make_shared<SourceCallTypeHint>(
        *objcRuntimeSourceCallHint(Image, CopyImport));
    CopyCall->Type = CopyCall->SourceCallHint->Signature.ReturnType;
    caller().Body.back().RetVal = CopyCall;
    std::string Error;
    auto D = readObjCBlockDescriptor(Image, Descriptor, 0xc2000000, Error);
    EXPECT_TRUE(D) << Error;
    for (bool IsCopy : {true, false}) {
      HighFunc H;
      H.Entry = IsCopy ? Copy : Dispose;
      H.Name = "helper_" + std::to_string(H.Entry);
      H.SourceTypeHint = IsCopy ? D->CopyTypeHint : D->DisposeTypeHint;
      H.ReturnType = H.SourceTypeHint->ReturnType;
      for (const auto &P : H.SourceTypeHint->Parameters)
        H.Params.push_back({P.Name, P.Type});
      auto Field = HighExpr::makeLoad(
          HighExpr::makeBinop(NdOp::INT_ADD,
                              parameter(IsCopy ? 1 : 0, H.Params[0].Type),
                              HighExpr::makeConst(32, 8)),
          H.Params[0].Type);
      HighStmt Effect;
      Effect.Kind = StmtKind::Call;
      Effect.CallExpr = HighExpr::makeCall(
          IsCopy ? "objc_retain" : "objc_release", 0, {Field});
      Effect.CallExpr->SourceCallHint =
          std::make_shared<SourceCallTypeHint>(*objcRuntimeSourceCallHint(
              Image, IsCopy ? RetainImport : ReleaseImport));
      // A discarded call result does not require an expression value type.
      Effect.CallExpr->Type.reset();
      H.Body = {Effect, ret(nullptr)};
      Result.HighFuncs.push_back(std::move(H));
    }
  }
};
} // namespace

TEST(ObjCBlockSources,
     GlobalLiteralUsesRealInvokeAndOneSharedDescriptorStorage) {
  SourceFixture F(false);
  auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  ASSERT_EQ(Plan.Globals.size(), 1U);
  ASSERT_EQ(Plan.InvokeHints.size(), 1U);
  auto Bound =
      bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.Dependencies, std::set<va_t>{F.Invoke});
  auto E = Bound.Function.Body.back().RetVal->Operands[0];
  EXPECT_EQ(E->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeBlockLiteral);
  EXPECT_TRUE(objcBlockSourceCallBound(*E, F.Image, Plan, F.functions()));
  auto Forged = *E;
  auto WrongEffect = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
  WrongEffect->DoesNotReturn = true;
  Forged.SourceCallHint = WrongEffect;
  EXPECT_FALSE(objcBlockSourceCallBound(Forged, F.Image, Plan, F.functions()));
  std::set<std::string> Shared;
  auto Text = renderObjCBlockSourceHelpers(Plan, Bound.Descriptors,
                                           Bound.Literals, Shared);
  EXPECT_NE(Text.find("&neverd_block_invoke_1100"), std::string::npos);
  EXPECT_NE(Text.find("&storage.descriptor"), std::string::npos);
  EXPECT_EQ(Shared.size(), 3U);
  EXPECT_EQ(F.caller().Body.back().RetVal->Operands[0]->Kind, ExprKind::Const);
}
TEST(ObjCBlockSources,
     GlobalLiteralDoesNotRequireUnusedInvokeSharingItsDescriptor) {
  SourceFixture F(false);
  constexpr va_t OtherLiteral = 0x2180, MissingInvoke = 0x1180;
  F.Image.ImportPtrSlots[OtherLiteral] = "__NSConcreteGlobalBlock";
  F.put32(OtherLiteral + 8, 0x50000000);
  F.put64(OtherLiteral + 16, MissingInvoke);
  F.put64(OtherLiteral + 24, F.Descriptor);
  const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  ASSERT_EQ(Plan.Globals.size(), 2U);
  const auto Bound =
      bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.Dependencies, std::set<va_t>{F.Invoke});
  EXPECT_EQ(Bound.Literals, std::set<va_t>{F.Literal});
  std::set<std::string> Shared;
  const auto Text = renderObjCBlockSourceHelpers(Plan, Bound.Descriptors,
                                                 Bound.Literals, Shared);
  EXPECT_NE(Text.find(objcBlockHelperName(true, F.Literal)), std::string::npos);
  EXPECT_EQ(Text.find(objcBlockHelperName(true, OtherLiteral)),
            std::string::npos);
}
TEST(ObjCBlockSources, BlockHelpersPartitionLiteralsByDescriptor) {
  SourceFixture F(false);
  constexpr va_t OtherLiteral = 0x2180, OtherDescriptor = 0x2280;
  F.Image.ImportPtrSlots[OtherLiteral] = "__NSConcreteGlobalBlock";
  F.put32(OtherLiteral + 8, 0x50000000);
  F.put64(OtherLiteral + 16, F.Invoke);
  F.put64(OtherLiteral + 24, OtherDescriptor);
  F.put64(OtherDescriptor, 0);
  F.put64(OtherDescriptor + 8, 32);
  F.put64(OtherDescriptor + 16, F.Signature);
  const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  ASSERT_EQ(Plan.Globals.size(), 2U);
  ASSERT_EQ(Plan.Descriptors.size(), 2U);
  std::set<std::string> Shared;
  const auto Text = renderObjCBlockSourceHelpers(
      Plan, {F.Descriptor, OtherDescriptor}, {F.Literal, OtherLiteral}, Shared);
  EXPECT_NE(Text.find(objcBlockHelperName(true, F.Literal)), std::string::npos);
  EXPECT_NE(Text.find(objcBlockHelperName(true, OtherLiteral)),
            std::string::npos);
}
TEST(ObjCBlockSources,
     GlobalInvokeMayPassFreshFrameStorageButNotAFrameHoldingContext) {
  for (bool StoreContext : {false, true}) {
    SourceFixture F(false);
    auto &Invoke = F.Result.HighFuncs[0];
    Invoke.FrameSize = 32;
    Invoke.Body.insert(Invoke.Body.begin(),
                       store(frame(F.Image, -8), HighExpr::makeConst(0, 8)));
    if (StoreContext)
      Invoke.Body.insert(
          Invoke.Body.begin() + 1,
          store(frame(F.Image, -24), parameter(0, Invoke.Params[0].Type)));
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall({}, 0, {frame(F.Image, -8)});
    Invoke.Body.insert(Invoke.Body.end() - 1, std::move(Call));
    const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    const auto Bound =
        bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Limitation.empty(), !StoreContext) << Bound.Limitation;
  }
}
TEST(ObjCBlockSources, InvokePreservesOnlyACompleteLowPointerSubbytesView) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SourceFixture F(true);
    auto &Invoke = F.Result.HighFuncs[0];
    auto &Context =
        Invoke.Body[0].RetVal->Operands[1]->Operands[0]->Operands[0];
    auto Wide = Context;
    Wide->Type = NdType::makeInt(16);
    auto Slice = HighExpr::makeBinop(
        NdOp::SUBBYTES, Wide, HighExpr::makeConst(Mutation == 1 ? 1 : 0, 4));
    Slice->Type = Mutation == 2 ? NdType::makeInt(4) : NdType::makeInt(8);
    Context = std::move(Slice);
    const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    const auto Bound =
        bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0)
        << Mutation << ": " << Bound.Limitation;
  }
}
TEST(ObjCBlockSources, InvokeReassemblesOnlyCompleteOrderedPointerSlices) {
  for (unsigned Mutation = 0; Mutation != 2; ++Mutation) {
    SourceFixture F(true);
    auto &Invoke = F.Result.HighFuncs[0];
    auto &Context =
        Invoke.Body[0].RetVal->Operands[1]->Operands[0]->Operands[0];
    auto Slice = [&](unsigned Offset, unsigned Size) {
      auto Result = HighExpr::makeBinop(NdOp::SUBBYTES, Context,
                                        HighExpr::makeConst(Offset, 4));
      Result->Type = NdType::makeInt(Size);
      return Result;
    };
    auto Low2 = HighExpr::makeBinop(NdOp::CONCAT, Slice(Mutation ? 0 : 1, 1),
                                    Slice(0, 1));
    Low2->Type = NdType::makeInt(2);
    auto Low4 = HighExpr::makeBinop(NdOp::CONCAT, Slice(2, 2), Low2);
    Low4->Type = NdType::makeInt(4);
    auto Whole = HighExpr::makeBinop(NdOp::CONCAT, Slice(4, 4), Low4);
    Whole->Type = NdType::makeInt(8);
    Context = std::move(Whole);
    const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    const auto Bound =
        bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0)
        << Mutation << ": " << Bound.Limitation;
  }
}
TEST(ObjCBlockSources, InvokeIgnoresOnlyUndefinedHighReturnCarrierPadding) {
  for (unsigned Mutation = 0; Mutation != 2; ++Mutation) {
    SourceFixture F(true);
    auto &Invoke = F.Result.HighFuncs[0];
    const auto Context = parameter(0, Invoke.Params[0].Type);
    auto High =
        HighExpr::makeBinop(NdOp::SUBBYTES, Context, HighExpr::makeConst(4, 4));
    High->Type = NdType::makeInt(4);
    MedVar Temporary;
    Temporary.Kind = MedVar::Temp;
    Temporary.Id = 91;
    Temporary.Size = 4;
    const auto HighLocal = HighExpr::makeVar(Temporary, High->Type);
    HighStmt SaveHigh;
    SaveHigh.Kind = StmtKind::Assign;
    SaveHigh.Dst = HighLocal;
    SaveHigh.Val = High;
    auto Result = HighExpr::makeBinop(
        NdOp::CONCAT, HighLocal, Mutation ? Context : Invoke.Body[0].RetVal);
    Result->Type = NdType::makeInt(8);
    Invoke.Body.insert(Invoke.Body.begin(), std::move(SaveHigh));
    Invoke.Body[1].RetVal = std::move(Result);
    const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    const auto Bound =
        bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0)
        << Mutation << ": " << Bound.Limitation;
  }
}
TEST(ObjCBlockSources, EmptyEntryLabelsPreserveTheStraightLineEscapeProof) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SourceFixture F(false);
    auto &Invoke = F.Result.HighFuncs[0];
    HighStmt Label;
    Label.Kind = StmtKind::Block;
    Label.Addr = Invoke.Entry;
    if (Mutation == 1)
      Label.Body.push_back(ret(parameter(0, Invoke.Params[0].Type)));
    if (Mutation == 2) {
      Label.Kind = StmtKind::Goto;
      Label.GotoTarget = Invoke.Entry + 1;
    }
    Invoke.Body.insert(Invoke.Body.begin(), Label);
    if (Mutation == 3)
      Invoke.Body.back().RetVal = parameter(0, Invoke.Params[0].Type);
    auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    const auto Bound =
        bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0)
        << Mutation << ": " << Bound.Limitation;
  }
}

TEST(ObjCBlockSources, ConsumerProofUsesBranchesLoopsAndSwitchTransfers) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Shape = 0; Shape != 6; ++Shape) {
      SCOPED_TRACE(Shape);
      SourceFixture F(true, Architecture);
      auto &Use = F.Result.HighFuncs[1];
      const auto Original = Use.Body.back();
      HighStmt Control;
      Control.Cond = parameter(1, Use.Params[1].Type);
      Control.Kind = StmtKind::IfElse;
      Control.Body = {Original};
      Control.ElseBody = {ret(HighExpr::makeConst(17, 4))};
      if (Shape == 1) {
        Control.Kind = StmtKind::Switch;
        Control.SwitchExpr = Control.Cond;
        Control.Body.clear();
        Control.ElseBody.clear();
        Control.Cases.push_back({1, {Original}});
        Control.DefaultBody = {ret(HighExpr::makeConst(29, 4))};
      }
      if (Shape >= 2 && Shape <= 4) {
        Control.Kind = Shape == 2   ? StmtKind::While
                       : Shape == 3 ? StmtKind::For
                                    : StmtKind::DoWhile;
        HighStmt Call, Transfer;
        Call.Kind = StmtKind::Call;
        Call.CallExpr = Original.RetVal;
        Transfer.Kind = Shape == 3 ? StmtKind::Continue : StmtKind::Break;
        Control.Body = {Call, Transfer};
        Control.ElseBody.clear();
      }
      if (Shape == 5) {
        Control.Kind = StmtKind::Goto;
        Control.GotoTarget = Use.Entry + 16;
        Control.Body.clear();
        Control.ElseBody.clear();
      }
      auto Last = Original;
      Last.Addr = Use.Entry + 16;
      Use.Body = {Control, Last};
      const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
      EXPECT_TRUE(bindObjCBlockSourceReferences(F.caller(), F.Image, Plan,
                                                F.functions())
                      .Limitation.empty());
    }
}

TEST(ObjCBlockSources, ConsumerJoinsKeepPossibleContextIdentities) {
  for (const bool Reverse : {false, true})
    for (const auto Architecture : {Arch::AArch64, Arch::X64})
      for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
        SCOPED_TRACE(Mutation);
        SourceFixture F(true, Architecture);
        auto &Use = F.Result.HighFuncs[1];
        MedVar Alias;
        Alias.Kind = MedVar::Temp;
        Alias.Id = 17;
        Alias.Size = 8;
        const auto Local = HighExpr::makeVar(Alias, Use.Params[0].Type);
        const auto Context = parameter(0, Use.Params[0].Type);
        HighStmt Left, Right, Choose;
        Left.Kind = Right.Kind = StmtKind::Assign;
        Left.Dst = Right.Dst = Local;
        Left.Val = Context;
        Right.Val = Mutation == 0 ? Context : HighExpr::makeConst(0, 8);
        if (Mutation == 2)
          Right.Val = HighExpr::makeBinop(NdOp::INT_ADD, Context,
                                          HighExpr::makeConst(8, 8));
        Choose.Kind = StmtKind::IfElse;
        Choose.Cond = parameter(1, Use.Params[1].Type);
        Choose.Body = {Left};
        Choose.ElseBody = {Right};
        auto Call = HighExpr::makeCall(
            {}, 0, {Local, parameter(1, Use.Params[1].Type)});
        Call->Type = Use.ReturnType;
        Call->SourceCallHint = Use.Body.back().RetVal->SourceCallHint;
        if (Reverse)
          std::swap(Choose.Body, Choose.ElseBody);
        Use.Body = {Choose};
        if (Mutation == 3)
          Use.Body.push_back(Left); // A full overwrite kills the joined value.
        if (Mutation == 4 || Mutation == 5) {
          // The first visit is safe. A later back edge changes the identity.
          Choose.Kind = StmtKind::While;
          Choose.ElseBody.clear();
          HighStmt Invoke;
          Invoke.Kind = StmtKind::Call;
          Invoke.CallExpr = Call;
          Choose.Body = {Invoke, Right};
          Use.Body = {Left, Choose};
          if (Mutation == 5)
            Use.Body.push_back(Left);
        }
        Use.Body.push_back(ret(Call));
        const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
        const auto Bound = bindObjCBlockSourceReferences(F.caller(), F.Image,
                                                         Plan, F.functions());
        EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0 || Mutation == 3)
            << Bound.Limitation;
      }
}

TEST(ObjCBlockSources, ConsumerFrameJoinsCannotHidePartialPointerSpills) {
  for (const bool Reverse : {false, true})
    for (const auto Architecture : {Arch::AArch64, Arch::X64})
      for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
        SCOPED_TRACE(Mutation);
        SourceFixture F(true, Architecture);
        auto &Use = F.Result.HighFuncs[1];
        Use.FrameSize = 32;
        const auto Context = parameter(0, Use.Params[0].Type);
        HighStmt Choose;
        Choose.Kind = StmtKind::IfElse;
        Choose.Cond = parameter(1, Use.Params[1].Type);
        Choose.Body = {store(frame(F.Image, -16), Context)};
        Choose.ElseBody = {store(
            frame(F.Image, -16),
            Mutation == 0 ? Context
                          : HighExpr::makeConst(0, Mutation == 1 ? 8 : 4))};
        auto Result = Use.Body.back();
        Result.RetVal =
            HighExpr::makeLoad(frame(F.Image, -16), NdType::makeInt(4));
        if (Mutation == 0) {
          Result = Use.Body.back();
          Result.RetVal->Operands[0] =
              HighExpr::makeLoad(frame(F.Image, -16), Use.Params[0].Type);
        }
        if (Reverse)
          std::swap(Choose.Body, Choose.ElseBody);
        Use.Body = {Choose};
        if (Mutation >= 3)
          Use.Body.push_back(
              store(frame(F.Image, -16),
                    HighExpr::makeConst(0, Mutation == 3 ? 8 : 2)));
        Use.Body.push_back(Result);
        const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
        const auto Bound = bindObjCBlockSourceReferences(F.caller(), F.Image,
                                                         Plan, F.functions());
        EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0 || Mutation == 3)
            << Bound.Limitation;
      }
}

TEST(ObjCBlockSources, ConsumerProofChecksTrailingTestsAndGotoEscapes) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SourceFixture F(true);
    auto &Use = F.Result.HighFuncs[1];
    const auto Context = parameter(0, Use.Params[0].Type);
    HighStmt Control;
    Control.Kind = StmtKind::DoWhile;
    Control.Cond = HighExpr::makeCall("unknown", 0, {Context});
    Control.Cond->Type = NdType::makeInt(4);
    if (Mutation == 1) {
      Control.Kind = StmtKind::If;
      Control.Cond = parameter(1, Use.Params[1].Type);
      HighStmt Jump;
      Jump.Kind = StmtKind::Goto;
      Jump.GotoTarget = Use.Entry + 16;
      Control.Body = {Jump};
      auto Escape = ret(Context);
      Escape.Addr = Jump.GotoTarget;
      Use.Body.push_back(Escape);
    }
    if (Mutation == 2) {
      Control.Kind = StmtKind::Goto;
      Control.GotoTarget = 0x9999;
    }
    if (Mutation == 3)
      Control.Kind = StmtKind::ItaniumTry;
    Use.Body.insert(Use.Body.begin(), Control);
    const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    EXPECT_FALSE(
        bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions())
            .Limitation.empty());
  }
}

TEST(ObjCBlockSources, ConsumerFlowBudgetCannotAuthorizeIncompleteProof) {
  SourceFixture F(true);
  auto &Use = F.Result.HighFuncs[1];
  const auto Last = Use.Body.back();
  Use.Body.clear();
  for (unsigned I = 0; I != 2048; ++I) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = I;
    V.Size = 4;
    HighStmt Set;
    Set.Kind = StmtKind::Assign;
    Set.Dst = HighExpr::makeVar(V, NdType::makeInt(4));
    Set.Val = parameter(1, Use.Params[1].Type);
    Use.Body.push_back(Set);
  }
  Use.Body.push_back(Last);
  const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  const auto Bound =
      bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
  EXPECT_NE(Bound.Limitation.find("budget"), std::string::npos)
      << Bound.Limitation;
}

TEST(ObjCBlockSources, ConsumerParameterAssignmentsReplaceEntryFacts) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SourceFixture F(true);
    auto &Use = F.Result.HighFuncs[1];
    Use.Params[1].Type = Use.Params[0].Type;
    Use.ReturnType = Use.Params[0].Type;
    Use.SourceTypeHint.reset();
    const auto Context = parameter(0, Use.Params[0].Type);
    const auto Other = parameter(1, Use.Params[0].Type);
    HighStmt Set;
    Set.Kind = StmtKind::Assign;
    Set.Dst = Mutation == 0 ? Context : Other;
    Set.Val = Mutation == 0 ? HighExpr::makeConst(0, 8) : Context;
    Use.Body = {Set};
    if (Mutation == 2) {
      HighStmt Clear = Set;
      Clear.Val = HighExpr::makeConst(0, 8);
      Use.Body.push_back(Clear);
    }
    Use.Body.push_back(ret(Set.Dst));
    std::set<std::pair<va_t, size_t>> Active;
    std::string Reason;
    const ObjCBlockSourceContext Source(F.Image);
    EXPECT_EQ(objc_block_source_detail::noEscape(
                  Source, F.functions(), Use.Entry, 0, nullptr, Active, Reason),
              Mutation != 1)
        << Mutation << ": " << Reason;
  }
}
TEST(ObjCBlockSources,
     ScalarStackCapturePreservesRawWritesAndUnwrittenPadding) {
  SourceFixture F(true);
  auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  ASSERT_EQ(Plan.StackBlocks[F.Caller].size(), 1U) << Plan.Rejections[F.Caller];
  EXPECT_EQ(Plan.StackBlocks[F.Caller][0].InitializedCaptures,
            (std::set<uint64_t>{32, 33, 34, 35}));
  auto Bound =
      bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  EXPECT_EQ(Bound.Function.Body.size(), F.caller().Body.size());
  EXPECT_EQ(Bound.Function.Body[4].StoreVal->Type->Size, 4U);
  EXPECT_EQ(Bound.Function.Body[0].StoreVal->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeBlockIsa);
  EXPECT_EQ(Bound.Function.Body[2].StoreVal->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::NativeAddress);
  EXPECT_EQ(Bound.Function.Body[3].StoreVal->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeBlockDescriptor);
  EXPECT_TRUE(
      bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());
}

TEST(ObjCBlockSources,
     OrdinaryFrameArgumentsBeforeConstructionDoNotHideLaterStackBlocks) {
  for (bool ComputedZero : {false, true})
    for (bool AfterConstruction : {false, true}) {
      SourceFixture F(true);
      ExprPtr Zero = HighExpr::makeConst(0, 16);
      if (ComputedZero) {
        Zero = HighExpr::makeBinop(NdOp::CONCAT, HighExpr::makeConst(0, 8),
                                   HighExpr::makeConst(0, 8));
        Zero->Type = NdType::makeInt(16);
      }
      F.caller().Body.insert(F.caller().Body.begin(),
                             store(frame(F.Image, -64), Zero));
      HighStmt Call;
      Call.Kind = StmtKind::Call;
      Call.CallExpr = HighExpr::makeCall("unknown", 0, {frame(F.Image, -56)});
      if (AfterConstruction)
        F.caller().Body.insert(F.caller().Body.end() - 1, std::move(Call));
      else
        F.caller().Body.insert(F.caller().Body.begin() + 1, std::move(Call));
      const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
      const auto Rejection = Plan.Rejections.find(F.Caller);
      EXPECT_EQ(Plan.StackBlocks.count(F.Caller), AfterConstruction ? 0U : 1U)
          << (Rejection == Plan.Rejections.end() ? "" : Rejection->second);
      if (AfterConstruction) {
        ASSERT_NE(Rejection, Plan.Rejections.end());
        EXPECT_NE(Rejection->second.find("nonliteral private frame"),
                  std::string::npos);
      }
    }
  SourceFixture F(true);
  auto UnknownWide =
      HighExpr::makeBinop(NdOp::CONCAT, HighExpr::makeConst(1, 8),
                          HighExpr::makeConst(0, 8));
  UnknownWide->Type = NdType::makeInt(16);
  F.caller().Body.insert(F.caller().Body.begin(),
                         store(frame(F.Image, -64), UnknownWide));
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall("unknown", 0, {frame(F.Image, -56)});
  F.caller().Body.insert(F.caller().Body.begin() + 1, std::move(Call));
  const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  EXPECT_FALSE(Plan.StackBlocks.count(F.Caller));
}

TEST(ObjCBlockSources, CopiedStackBlockCallsKeepDescriptorAcrossBranches) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
      SCOPED_TRACE(Mutation);
      SourceFixture F(true, Architecture);
      F.string(F.Signature, "v8@?0");
      F.Image.DyldBindSlots[F.StackIsa] = {"__NSConcreteStackBlock", 0,
                                           "/usr/lib/libSystem.B.dylib", false};
      constexpr va_t CopySlot = 0x2600, RetainSlot = 0x2610, FlagsPool = 0x2700;
      F.put64(FlagsPool, UINT64_C(0xc0000000));
      for (const auto &[Slot, Name] : {std::pair{CopySlot, "_objc_retainBlock"},
                                       std::pair{RetainSlot, "_objc_retain"}}) {
        F.Image.ImportPtrSlots[Slot] = Name;
        F.Image.DyldBindSlots[Slot] = {Name, 0, "/usr/lib/libobjc.A.dylib",
                                       false};
      }
      Segment Data;
      Data.VA = 0x6000;
      Data.Size = Data.FileSz = 32;
      Data.FileOff = 0x4000;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(32);
      F.Image.Segments.push_back(Data);
      Section Section;
      Section.VA = 0x6000;
      Section.Size = Section.FileSz = 32;
      Section.FileOff = 0x4000;
      Section.Flags = Data.Flags;
      F.Image.Sections.push_back(Section);
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto SP = NdVar::reg(TRI.StackPointer, 8);
      const auto Arg = NdVar::reg(TRI.IntParamRegs[0], 8);
      const auto Result = NdVar::reg(TRI.IntReturnReg, 8);
      const auto Saved = NdVar::reg(TRI.CalleeSaveRegs.front(), 8);
      const auto Target = NdVar::reg(TRI.IntParamRegs.back(), 8);
      auto Op = [](NdOp Code, NdVar Out, std::initializer_list<NdVar> Inputs,
                   va_t Address) {
        LowOp O;
        O.Opcode = Code;
        O.Output = Out;
        O.Addr = Address;
        for (const auto &V : Inputs)
          O.addInput(V);
        return O;
      };
      LowFunc Low;
      Low.Entry = 0x1200;
      Low.Blocks.resize(4);
      for (unsigned I = 0; I < 4; ++I) {
        Low.Blocks[I].Id = I;
        Low.Blocks[I].StartAddr = 0x1200 + I * 0x100;
        Low.Blocks[I].EndAddr = Low.Blocks[I].StartAddr + 0x80;
      }
      Low.Blocks[0].Succs = {1, 2};
      Low.Blocks[1].Preds = {0};
      Low.Blocks[2].Preds = {0};
      Low.Blocks[1].Succs = {3};
      Low.Blocks[2].Succs = {3};
      Low.Blocks[3].Preds = {1, 2};
      auto &Build = Low.Blocks[0].Ops;
      Build.push_back(Op(NdOp::INT_SUB, SP, {SP, NdVar::cst(64, 4)}, 0x1200));
      Build.push_back(
          Op(NdOp::LOAD, Target, {NdVar::cst(F.StackIsa, 8)}, 0x1204));
      Build.push_back(Op(NdOp::STORE, {}, {SP, Target}, 0x1208));
      for (const auto &[Offset, Bits] :
           {std::pair{8U, uint64_t(0)}, std::pair{16U, F.Invoke},
            std::pair{24U, F.Descriptor}}) {
        const va_t At = 0x120c + Offset;
        Build.push_back(
            Op(NdOp::INT_ADD, Target, {SP, NdVar::cst(Offset, 8)}, At));
        if (Offset == 8) {
          const auto Lane = NdVar::reg(TRI.FPReturnReg, 8);
          Build.push_back(Op(NdOp::LOAD, Lane, {NdVar::cst(FlagsPool, 8)}, At));
          Build.push_back(
              Op(NdOp::INT_ZEXT, NdVar::reg(TRI.FPReturnReg, 16), {Lane}, At));
          if (Mutation == 14)
            Build.push_back(Op(NdOp::COPY, NdVar::reg(TRI.FPReturnReg, 4),
                               {NdVar::cst(0, 4)}, At));
          Build.push_back(Op(NdOp::STORE, {}, {Target, Lane}, At));
        } else if (!(Mutation == 10 && Offset == 24))
          Build.push_back(
              Op(NdOp::STORE, {}, {Target, NdVar::cst(Bits, 8)}, At));
      }
      Build.push_back(Op(NdOp::COPY, Arg, {SP}, 0x1230));
      Build.push_back(
          Op(NdOp::INDIR_CALL, Result, {NdVar::cst(CopySlot, 8)}, 0x1234));
      Build.push_back(Op(NdOp::COPY, Saved, {Result}, 0x1238));
      if (Mutation == 15)
        Build.push_back(
            Op(NdOp::STORE, {}, {NdVar::cst(0x6000, 8), Saved}, 0x123a));
      if (Mutation == 16)
        Build.push_back(
            Op(NdOp::INT_XOR, Target, {Saved, NdVar::cst(0, 8)}, 0x123a));
      if (Mutation == 17)
        Build.push_back(Op(NdOp::COPY, NdVar::reg(Target.Offset, 4),
                           {NdVar::reg(Saved.Offset, 4)}, 0x123a));
      Build.push_back(Op(NdOp::COPY, Arg, {NdVar::cst(0, 8)}, 0x123c));
      Build.push_back(
          Op(NdOp::INDIR_CALL, Result, {NdVar::cst(RetainSlot, 8)}, 0x1240));
      auto &Left = Low.Blocks[1].Ops;
      Left.push_back(
          Op(NdOp::STORE, {},
             {Mutation == 6 ? Target : NdVar::cst(0x6000, 8), NdVar::cst(7, 8)},
             0x1300));
      if (Mutation == 2)
        Left.push_back(Op(NdOp::COPY, NdVar::reg(Saved.Offset + 1, 1),
                          {NdVar::cst(0, 1)}, 0x1304));
      Left.push_back(Op(NdOp::COPY, Arg, {NdVar::cst(0, 8)}, 0x1308));
      Left.push_back(Op(NdOp::INDIR_CALL, Result,
                        {NdVar::cst(Mutation == 3 ? 0x2620 : RetainSlot, 8)},
                        0x130c));
      if (Mutation == 9)
        Low.Blocks[2].Ops.push_back(
            Op(NdOp::COPY, Saved, {NdVar::cst(0, 8)}, 0x1400));
      Low.Blocks[3].Ops = {
          Op(NdOp::INT_ADD, Target,
             {Saved, NdVar::cst(Mutation == 5 ? 24 : 16, 8)}, 0x1500),
          Op(NdOp::LOAD, Target, {Target}, 0x1504),
          Op(NdOp::COPY, Arg, {Mutation == 4 ? NdVar::cst(0, 8) : Saved},
             0x1508),
          Op(NdOp::INDIR_CALL, {}, {Target}, 0x150c),
          Op(NdOp::RETURN, {}, {}, 0x1510)};
      if (Mutation == 1)
        F.string(F.Signature, "?");
      if (Mutation == 7)
        F.Image.DyldBindSlots[CopySlot].Module = "/tmp/impostor.dylib";
      if (Mutation == 8)
        F.Image.DyldBindSlots[CopySlot].WeakImport = true;
      if (Mutation == 11) {
        F.Image.Segments[1].Flags =
            F.Image.Segments[1].Flags | SegmentFlags::Writable;
        F.Image.Sections[1].Flags =
            F.Image.Sections[1].Flags | SegmentFlags::Writable;
      }
      if (Mutation == 12)
        F.put64(FlagsPool, UINT64_C(0x1c0000000));
      if (Mutation == 13)
        F.Image.Sections.push_back(Section);
      const auto Hints = buildObjCSourceCallHints(F.Image, Low);
      EXPECT_EQ(Hints.count(0x150c), Mutation == 0 ? 1U : 0U);
      if (Mutation == 0 && Hints.count(0x150c)) {
        const auto &Hint = Hints.at(0x150c);
        EXPECT_EQ(Hint.CallKind, SourceCallTypeHint::Kind::BlockInvoke);
        EXPECT_EQ(Hint.Signature.ReturnType->Kind, NdTypeKind::Void);
        EXPECT_EQ(Hint.Signature.Parameters.size(), 1U);
        EXPECT_EQ(Hint.Signature.Origin,
                  SourceFunctionTypeHint::OriginKind::BlockRuntime);
      }
    }
  }
}
TEST(ObjCBlockSources,
     MissingHeaderByteWrongIsaFrameAndUnknownConsumerAreRejected) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    SourceFixture F(true);
    if (Mutation == 0)
      F.caller().Body.erase(F.caller().Body.begin() + 1);
    if (Mutation == 1)
      F.Image.ImportPtrSlots[F.StackIsa] = "__NSConcreteGlobalBlock";
    if (Mutation == 2)
      F.caller().FrameSize = 16;
    if (Mutation == 3) {
      auto Hint = std::make_shared<SourceCallTypeHint>(
          *F.caller().Body.back().RetVal->SourceCallHint);
      Hint->TargetAddress = 0x9999;
      F.caller().Body.back().RetVal->SourceCallHint = Hint;
    }
    if (Mutation == 4)
      F.caller().Body[1].StoreVal =
          HighExpr::makeConst(UINT64_C(1) << 32 | 0xc0000000, 8);
    F.Image.DyldBindSlots[F.StackIsa] = {F.Image.ImportPtrSlots[F.StackIsa], 0,
                                         "/usr/lib/libSystem.B.dylib", false};
    ASSERT_TRUE(darwinRuntimeGlobalAddressHint(F.Image, F.StackIsa));
    auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    EXPECT_TRUE(Plan.StackBlocks.empty()) << Mutation;
    const auto Block =
        bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
    const auto Bound = bindObjCSourceReferences(Block.Function, F.Image);
    // A public ISA storage address does not prove the block construction,
    // descriptor, native invoke, or consumer's ownership contract.
    EXPECT_FALSE(Block.Limitation.empty() && Bound.Limitation.empty())
        << Mutation;
  }
}

TEST(ObjCBlockSources, ConstructionUsesOnlyReachableBranchAndLoopStates) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Shape = 0; Shape != 6; ++Shape) {
      SCOPED_TRACE(Shape);
      SourceFixture F(true, Architecture);
      auto &Caller = F.caller();
      HighStmt Control;
      Control.Cond = parameter(1, Caller.Params[1].Type);
      Control.Kind = StmtKind::IfElse;
      Control.Body = Caller.Body;
      Control.ElseBody = {ret(HighExpr::makeConst(0, 4))};
      if (Shape >= 1 && Shape <= 3) {
        Control.Kind = Shape == 1   ? StmtKind::While
                       : Shape == 2 ? StmtKind::For
                                    : StmtKind::DoWhile;
        auto &Call = Control.Body.back();
        Call.Kind = StmtKind::Call;
        Call.CallExpr = Call.RetVal;
        Call.RetVal.reset();
        HighStmt Continue;
        Continue.Kind = StmtKind::Continue;
        Control.Body.push_back(Continue);
        Control.ElseBody.clear();
      }
      if (Shape == 4) {
        Control.Kind = StmtKind::Switch;
        Control.SwitchExpr = Control.Cond;
        Control.Cases.push_back({7, Control.Body});
        Control.DefaultBody = Control.ElseBody;
        Control.Body.clear();
        Control.ElseBody.clear();
      }
      if (Shape == 5) {
        // A return ends the proof path; dead writes cannot expose a context.
        Caller.Body.push_back(ret(frame(F.Image, -48)));
      } else {
        Caller.Body = {Control, ret(HighExpr::makeConst(0, 4))};
      }
      const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
      EXPECT_EQ(Plan.StackBlocks.count(F.Caller), 1U);
      const auto Bound =
          bindObjCBlockSourceReferences(Caller, F.Image, Plan, F.functions());
      EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
    }
}

TEST(ObjCBlockSources, ConstructionJoinsRequireEveryHeaderAndCaptureByte) {
  for (bool Reverse : {false, true})
    for (unsigned Field : {1U, 2U, 3U, 4U}) {
      SCOPED_TRACE(Field);
      SourceFixture F(true);
      auto &Caller = F.caller();
      HighStmt Choose;
      Choose.Kind = StmtKind::IfElse;
      Choose.Cond = parameter(1, Caller.Params[1].Type);
      Choose.Body = {Caller.Body[Field]};
      if (Reverse)
        std::swap(Choose.Body, Choose.ElseBody);
      Caller.Body.erase(Caller.Body.begin() + Field);
      Caller.Body.insert(Caller.Body.end() - 1, Choose);
      const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
      const auto Bound =
          bindObjCBlockSourceReferences(Caller, F.Image, Plan, F.functions());
      EXPECT_FALSE(Bound.Limitation.empty());
    }
  for (bool Overwrite : {false, true}) {
    SourceFixture F(true);
    auto &Caller = F.caller();
    HighStmt Choose;
    Choose.Kind = StmtKind::If;
    Choose.Cond = parameter(1, Caller.Params[1].Type);
    Choose.Body = {store(frame(F.Image, -16), frame(F.Image, -48))};
    Caller.Body.insert(Caller.Body.end() - 1, Choose);
    if (Overwrite)
      Caller.Body.insert(Caller.Body.end() - 1,
                         store(frame(F.Image, -16), HighExpr::makeConst(0, 8)));
    const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    const auto Bound =
        bindObjCBlockSourceReferences(Caller, F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Limitation.empty(), Overwrite) << Bound.Limitation;
  }
}

TEST(ObjCBlockSources, ReusedConstructionBindsEveryHeaderProducer) {
  SourceFixture F(true), Other(true);
  auto First = F.caller().Body;
  auto Second = Other.caller().Body;
  First.back().Kind = StmtKind::Call;
  First.back().CallExpr = First.back().RetVal;
  First.back().RetVal.reset();
  First.insert(First.end(), Second.begin(), Second.end());
  F.caller().Body = First;
  const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  ASSERT_EQ(Plan.StackBlocks.count(F.Caller), 1U);
  ASSERT_EQ(Plan.StackBlocks.at(F.Caller).size(), 1U);
  EXPECT_EQ(Plan.StackBlocks.at(F.Caller)[0].References.size(), 6U);
  const auto Bound =
      bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
  ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
  for (auto Index : {0U, 2U, 3U, 6U, 8U, 9U})
    EXPECT_TRUE(Bound.Function.Body[Index].StoreVal->SourceCallHint) << Index;
}

TEST(ObjCBlockSources, ImageWritesKeepPrivateBlockAddressesConfined) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(Architecture == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(Mutation);
      SourceFixture F(true, Architecture);
      auto &Invoke = F.Result.HighFuncs[0];
      ExprPtr Address = HighExpr::makeConst(0x2600, 8);
      ExprPtr Value = HighExpr::makeConst(7, 8);
      const auto Context = parameter(0, Invoke.Params[0].Type);
      if (Mutation == 1)
        Value = Context;
      if (Mutation == 2)
        Value = frame(F.Image, 0);
      if (Mutation == 3)
        Address = Context;
      if (Mutation == 4)
        Address = HighExpr::makeLoad(HighExpr::makeConst(0x2608, 8),
                                     NdType::makeInt(8));
      if (Mutation == 5)
        Address = HighExpr::makeConst(0x5000 - 4, 8);
      if (Mutation == 6)
        Address = HighExpr::makeConst(0x1100, 8);
      if (Mutation == 7)
        Address = HighExpr::makeConst(UINT64_MAX - 3, 8);
      Invoke.Body.insert(Invoke.Body.begin(), store(Address, Value));
      const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
      const auto Bound = bindObjCBlockSourceReferences(F.caller(), F.Image,
                                                       Plan, F.functions());
      EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0) << Bound.Limitation;
      if (!Mutation) {
        const auto Rebound =
            bindObjCBlockSourceReferences(Invoke, F.Image, Plan, F.functions());
        ASSERT_TRUE(Rebound.Limitation.empty()) << Rebound.Limitation;
        EXPECT_EQ(Rebound.Function.Body.front().StoreAddr->ConstVal, 0x2600U);
        EXPECT_EQ(Rebound.Function.Body.front().StoreVal->ConstVal, 7U);
      }
    }
}

TEST(ObjCBlockSources, UntouchedEntryPointerStoresCannotAliasPrivateBlock) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      SCOPED_TRACE(Architecture == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(Mutation);
      SourceFixture F(true, Architecture);
      auto &Caller = F.caller();
      const auto Pointer = parameter(0, Caller.Params[0].Type);
      ExprPtr Address = Pointer;
      if (Mutation == 1)
        Address = HighExpr::makeBitCast(Pointer, NdType::makeInt(8));
      if (Mutation == 2)
        Address = HighExpr::makeLoad(Pointer, Caller.Params[0].Type);
      if (Mutation == 3) {
        HighStmt Reassign;
        Reassign.Kind = StmtKind::Assign;
        Reassign.Dst = Pointer;
        Reassign.Val = HighExpr::makeLoad(Pointer, Caller.Params[0].Type);
        Caller.Body.insert(Caller.Body.end() - 1, Reassign);
      }
      Caller.Body.insert(Caller.Body.end() - 1,
                         store(Address, HighExpr::makeConst(7, 8)));
      const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
      EXPECT_EQ(Plan.StackBlocks.count(F.Caller), Mutation < 2 ? 1U : 0U)
          << (Plan.Rejections.count(F.Caller) ? Plan.Rejections.at(F.Caller)
                                              : "");
      if (Mutation < 2) {
        const auto Bound =
            bindObjCBlockSourceReferences(Caller, F.Image, Plan, F.functions());
        EXPECT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      }
    }
}

TEST(ObjCBlockSources, SuperMessageBorrowsOnlyDisjointCompleteFrameRecord) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(Architecture == Arch::AArch64 ? "arm64" : "x86_64");
      SCOPED_TRACE(Mutation);
      SourceFixture F(true, Architecture);
      auto &Caller = F.caller();
      Caller.Body.back().Kind = StmtKind::ExprStmt;
      Caller.Body.back().Val = Caller.Body.back().RetVal;
      Caller.Body.back().RetVal.reset();
      Caller.Body.push_back(
          store(frame(F.Image, -64), parameter(0, Caller.Params[0].Type)));
      if (Mutation != 2)
        Caller.Body.push_back(
            store(frame(F.Image, -56), Mutation == 1
                                           ? frame(F.Image, 0)
                                           : HighExpr::makeConst(0x2800, 8)));
      auto Signature = parseObjCMethodEncoding("dealloc", "v16@0:8");
      ASSERT_TRUE(Signature);
      std::string Error;
      ASSERT_TRUE(assignDarwinObjCSourceABI(*Signature, Architecture, Error))
          << Error;
      auto Super =
          HighExpr::makeCall("objc_msgSendSuper2", 0,
                             {frame(F.Image, Mutation == 3 ? -40 : -64),
                              HighExpr::makeConst(0x2900, 8)});
      Super->Type = NdType::makeVoid();
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->CallKind = SourceCallTypeHint::Kind::ObjCSuper2;
      Hint->TargetName = Mutation == 4 ? "unrelated" : "objc_msgSendSuper2";
      Hint->Selector = "dealloc";
      Hint->Signature = *Signature;
      Super->SourceCallHint = Hint;
      HighStmt Send;
      Send.Kind = StmtKind::Call;
      Send.CallExpr = Super;
      Caller.Body.push_back(Send);
      Caller.Body.push_back(ret(HighExpr::makeConst(0, 4)));
      const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
      EXPECT_EQ(Plan.StackBlocks.count(F.Caller), Mutation == 0 ? 1U : 0U)
          << (Plan.Rejections.count(F.Caller) ? Plan.Rejections.at(F.Caller)
                                              : "");
    }
}

TEST(ObjCBlockSources, DeclaredConsumerRequiresExactImportAndCallbackABI) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const auto *Name :
         {"dispatch_sync", "dispatch_async", "dispatch_barrier_async"})
      for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
        SCOPED_TRACE(Mutation);
        SourceFixture F(true, Architecture);
        constexpr va_t Slot = 0x2800;
        const std::string Import = std::string("_") + Name;
        F.Image.ImportPtrSlots[Slot] = Import;
        F.Image.DyldBindSlots[Slot] = {
            Import, 0, "/usr/lib/system/libdispatch.dylib", false};
        auto Binding = darwinRuntimeSourceCallHint(F.Image, Slot);
        auto Contract = darwinBlockParameterContract(F.Image, Slot, 1);
        ASSERT_TRUE(Contract);
        auto Callback = std::optional(Contract->Signature);
        const bool Copies = std::string(Name) != "dispatch_sync";
        EXPECT_EQ(Contract->Storage ==
                      DarwinBlockParameterContract::Lifetime::Copied,
                  Copies);
        EXPECT_EQ(bool(darwinNonEscapingBlockSignature(F.Image, Slot, 1)),
                  !Copies);
        ASSERT_TRUE(Binding);
        ASSERT_TRUE(Callback);
        EXPECT_FALSE(darwinBlockParameterContract(F.Image, Slot, 0));
        EXPECT_FALSE(darwinBlockParameterContract(F.Image, Slot, 2));
        F.string(F.Signature, "v8@?0");
        auto &Invoke = F.Result.HighFuncs[0];
        Invoke.SourceTypeHint = Callback;
        Invoke.ReturnType = NdType::makeVoid();
        Invoke.Params.resize(1);
        Invoke.Body = {ret(nullptr)};
        auto Call = HighExpr::makeCall(
            Name, 0,
            {parameter(0, F.caller().Params[0].Type), frame(F.Image, -48)});
        Call->Type = NdType::makeVoid();
        if (Mutation == 1)
          F.Image.DyldBindSlots[Slot].Module = "/usr/lib/unrelated.dylib";
        if (Mutation == 2)
          F.Image.DyldBindSlots[Slot].WeakImport = true;
        if (Mutation == 3)
          F.string(F.Signature, "i12@?0i8");
        if (Mutation == 4)
          std::swap(Call->Operands[0], Call->Operands[1]);
        if (Mutation == 5)
          Binding->TargetName = "dispatch_apply";
        if (Mutation == 6)
          Binding->Signature.ReturnType = NdType::makeInt(4);
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Binding);
        if (Mutation == 0 && Copies) {
          HighStmt Dispatch;
          Dispatch.Kind = StmtKind::Call;
          Dispatch.CallExpr = Call;
          HighStmt OrdinaryFrameCall;
          OrdinaryFrameCall.Kind = StmtKind::Call;
          OrdinaryFrameCall.CallExpr =
              HighExpr::makeCall("unknown", 0, {frame(F.Image, -56)});
          F.caller().Body.back() = std::move(Dispatch);
          F.caller().Body.push_back(std::move(OrdinaryFrameCall));
          F.caller().Body.push_back(ret(nullptr));
        } else {
          F.caller().Body.back() = ret(Call);
        }
        if (Mutation >= 7) {
          auto Original = F.caller().Body;
          auto &First = F.caller().Body.back();
          First.Kind = StmtKind::Call;
          First.CallExpr = First.RetVal;
          First.RetVal.reset();
          if (Mutation == 8)
            F.caller().Body.insert(F.caller().Body.end(), Original.begin(),
                                   Original.end());
          else
            F.caller().Body.push_back(ret(Call));
        }
        F.caller().ReturnType = NdType::makeVoid();
        F.caller().SourceTypeHint->ReturnType = NdType::makeVoid();
        const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
        const auto Bound = bindObjCBlockSourceReferences(F.caller(), F.Image,
                                                         Plan, F.functions());
        EXPECT_EQ(Bound.Limitation.empty(), Mutation == 0 || Mutation == 8)
            << Bound.Limitation;
      }
}

TEST(ObjCBlockSources, DispatchConsumersCopyOnlyAuthenticatedBlockArguments) {
  constexpr std::pair<const char *, unsigned> Consumers[] = {
      {"dispatch_after", 2},
      {"dispatch_group_async", 2},
      {"dispatch_group_notify", 2},
      {"dispatch_source_set_event_handler", 1},
  };
  for (const auto &[Name, Parameter] : Consumers) {
    SCOPED_TRACE(Name);
    SourceFixture F(true);
    constexpr va_t Slot = 0x2800;
    const std::string Import = std::string("_") + Name;
    F.Image.ImportPtrSlots[Slot] = Import;
    F.Image.DyldBindSlots[Slot] = {Import, 0,
                                   "/usr/lib/system/libdispatch.dylib", false};
    const auto Contract =
        darwinBlockParameterContract(F.Image, Slot, Parameter);
    ASSERT_TRUE(Contract);
    EXPECT_EQ(Contract->Storage,
              DarwinBlockParameterContract::Lifetime::Copied);
    EXPECT_EQ(Contract->Signature.Parameters.size(), 1U);
    EXPECT_FALSE(darwinBlockParameterContract(F.Image, Slot, Parameter - 1));
    EXPECT_FALSE(darwinNonEscapingBlockSignature(F.Image, Slot, Parameter));

    F.Image.DyldBindSlots[Slot].Module = "/usr/lib/unrelated.dylib";
    EXPECT_FALSE(darwinBlockParameterContract(F.Image, Slot, Parameter));
  }
}

TEST(ObjCBlockSources, CoreDataAsyncConsumerRequiresQualifiedCopiedContract) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (llvm::StringRef Owner :
         {"NSManagedObjectContext", "NSPersistentStoreCoordinator"}) {
      BlockFixture F;
      F.Image.Arch = Architecture;
      F.Image.DynInfo.NeededLibs = {
          "/System/Library/Frameworks/CoreData.framework/CoreData",
          "/System/Library/Frameworks/Foundation.framework/Foundation"};
      ObjCClass Class;
      Class.Name = "TestContext";
      Class.SuperclassName = Owner.str();
      Class.InheritanceStatus = "resolved";
      F.Image.ObjCClasses.push_back(Class);
      ObjCMethod Method;
      Method.Implementation = 0x1200;
      Method.ClassName = Class.Name;
      Method.Selector = "submit:";
      Method.TypeHint = parseObjCMethodEncoding("submit:", "v24@0:8@16");
      ASSERT_TRUE(Method.TypeHint);
      F.Image.ObjCMethods.push_back(Method);
      auto Receiver =
          objcMethodReceiverTypeHint(F.Image, Method.Implementation);
      ASSERT_TRUE(Receiver);
      SourceCallTypeHint Call;
      Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
      Call.Selector = "performBlock:";
      Call.Receiver = *Receiver;
      auto Parent =
          objcReceiverSourceTypeHint(F.Image, Call.Selector, *Receiver);
      ASSERT_TRUE(Parent.Signature);
      Call.Signature = *Parent.Signature;
      auto Contract = objcBlockParameterContract(F.Image, Call, 2);
      ASSERT_TRUE(Contract) << Owner.str();
      EXPECT_EQ(Contract->Storage,
                ObjCBlockParameterContract::Lifetime::Copied);
      EXPECT_EQ(Contract->Signature.ReturnType->Kind, NdTypeKind::Void);
      EXPECT_EQ(Contract->Signature.Parameters.size(), 1U);
      EXPECT_FALSE(objcNonEscapingBlockSignature(F.Image, Call, 2));

      auto Changed = Call;
      Changed.Receiver.reset();
      EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 2));
      Changed = Call;
      Changed.Signature.Parameters.pop_back();
      EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 2));
      Changed = Call;
      Changed.Selector = "performBlockAndWait:";
      auto Synchronous = objcBlockParameterContract(F.Image, Changed, 2);
      ASSERT_TRUE(Synchronous);
      EXPECT_EQ(Synchronous->Storage,
                ObjCBlockParameterContract::Lifetime::NonEscaping);
      EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 1));

      F.Image.ObjCClasses.front().SuperclassName = "NSPersistentContainer";
      EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 2));
    }
}

TEST(ObjCBlockSources, CoreDataAsyncCallCopiesOnlyAuthenticatedStackBlock) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SourceFixture F(true);
    F.string(F.Signature, Mutation == 2 ? "i12@?0i8" : "v8@?0");
    std::string Error;
    auto Descriptor =
        readObjCBlockDescriptor(F.Image, F.Descriptor, 0xc0000000, Error);
    ASSERT_TRUE(Descriptor) << Error;
    auto &Invoke = F.Result.HighFuncs[0];
    Invoke.SourceTypeHint = *Descriptor->InvokeTypeHint;
    Invoke.ReturnType = Invoke.SourceTypeHint->ReturnType;
    Invoke.Params.clear();
    for (const auto &Parameter : Invoke.SourceTypeHint->Parameters)
      Invoke.Params.push_back({Parameter.Name, Parameter.Type});
    Invoke.Body = {ret(Mutation == 2 ? HighExpr::makeConst(0, 4) : nullptr)};

    F.Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/CoreData.framework/CoreData",
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    ObjCClass Class;
    Class.Name = "TestContext";
    Class.SuperclassName = "NSManagedObjectContext";
    Class.InheritanceStatus = "resolved";
    F.Image.ObjCClasses.push_back(Class);
    ObjCMethod Method;
    Method.Implementation = F.Caller;
    Method.ClassName = Class.Name;
    Method.Selector = "submit:";
    Method.TypeHint = parseObjCMethodEncoding("submit:", "v24@0:8@16");
    ASSERT_TRUE(Method.TypeHint);
    F.Image.ObjCMethods.push_back(Method);
    auto Receiver = objcMethodReceiverTypeHint(F.Image, F.Caller);
    ASSERT_TRUE(Receiver);
    SourceCallTypeHint Binding;
    Binding.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Binding.TargetName = "objc_msgSend";
    Binding.Selector = "performBlock:";
    Binding.Receiver = *Receiver;
    auto Parent =
        objcReceiverSourceTypeHint(F.Image, Binding.Selector, *Receiver);
    ASSERT_TRUE(Parent.Signature);
    Binding.Signature = *Parent.Signature;
    if (Mutation == 1)
      Binding.Receiver.reset();

    auto Call = HighExpr::makeCall("objc_msgSend", 0,
                                   {parameter(0, F.caller().Params[0].Type),
                                    HighExpr::makeConst(0x2600, 8),
                                    frame(F.Image, -48)});
    Call->Type = Binding.Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Binding);
    if (Mutation != 1)
      EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, F.functions()));
    HighStmt Send;
    Send.Kind = StmtKind::Call;
    Send.CallExpr = Call;
    F.caller().Body.back() = Send;
    F.caller().Body.push_back(ret(HighExpr::makeConst(0, 4)));
    const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    EXPECT_EQ(Plan.StackBlocks.count(F.Caller), Mutation == 0 ? 1U : 0U)
        << (Plan.Rejections.count(F.Caller) ? Plan.Rejections.at(F.Caller)
                                            : "");
    if (Mutation)
      ASSERT_TRUE(Plan.Rejections.count(F.Caller));
    if (Mutation == 1)
      EXPECT_NE(Plan.Rejections.at(F.Caller).find("unqualified receiver"),
                std::string::npos);
    if (Mutation == 2)
      EXPECT_NE(Plan.Rejections.at(F.Caller).find("callback ABI differs"),
                std::string::npos);
  }
}

TEST(ObjCBlockSources, FLAnimatedImageLoggingBlockIsNonEscaping) {
  BlockFixture F;
  F.Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  ObjCClass Class;
  Class.Name = "FLAnimatedImage";
  Class.Address = 0x2700;
  Class.SuperclassName = "NSObject";
  Class.InheritanceStatus = "resolved";
  F.Image.ObjCClasses.push_back(Class);
  ObjCMethod Method;
  Method.Implementation = 0x1300;
  Method.MetadataAddress = 0x2800;
  Method.ClassAddress = Class.Address;
  Method.ClassName = Class.Name;
  Method.Selector = "logStringFromBlock:withLevel:";
  Method.TypeEncoding = "v32@0:8@?16Q24";
  Method.IsClassMethod = true;
  Method.Status = "supported";
  Method.TypeHint = parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  ASSERT_TRUE(Method.TypeHint);
  F.Image.ObjCMethods.push_back(Method);
  ObjCSourceReference Reference;
  Reference.TheKind = ObjCSourceReference::Kind::Class;
  Reference.Address = 0x2900;
  Reference.Size = 8;
  Reference.Name = Class.Name;
  F.Image.ObjCSourceReferences.emplace(Reference.Address, Reference);
  ObjCReceiverTypeHint Receiver;
  Receiver.Origin = ObjCReceiverTypeHint::OriginKind::ClassReference;
  Receiver.Address = Reference.Address;
  Receiver.ClassName = Class.Name;
  Receiver.IsClassMethod = true;
  SourceCallTypeHint Call;
  Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Call.Selector = Method.Selector;
  Call.Receiver = Receiver;
  const auto Declaration =
      objcReceiverSourceTypeHint(F.Image, Call.Selector, Receiver);
  ASSERT_TRUE(Declaration.Signature);
  Call.Signature = *Declaration.Signature;
  const auto Contract = objcBlockParameterContract(F.Image, Call, 2);
  ASSERT_TRUE(Contract);
  EXPECT_EQ(Contract->Storage,
            ObjCBlockParameterContract::Lifetime::NonEscaping);
  EXPECT_EQ(Contract->Signature.Parameters.size(), 1U);
  EXPECT_EQ(Contract->Signature.ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 3));

  auto Changed = Call;
  Changed.Receiver.reset();
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 2));
  Changed = Call;
  Changed.Signature.Parameters.pop_back();
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 2));
  F.Image.ObjCMethods.front().TypeEncoding = "v32@0:8@?16i24";
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 2));
  F.Image.ObjCMethods.front().TypeEncoding = Method.TypeEncoding;
  F.Image.ObjCMethods.front().IsClassMethod = false;
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 2));
}

TEST(ObjCBlockSources, MantleTransformerFactoriesCopyBlocks) {
  BlockFixture F;
  F.Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  ObjCClass Class;
  Class.Name = "MTLValueTransformer";
  Class.Address = 0x2700;
  Class.SuperclassName = "NSValueTransformer";
  Class.InheritanceStatus = "resolved";
  F.Image.ObjCClasses.push_back(Class);
  ObjCSourceReference Reference;
  Reference.TheKind = ObjCSourceReference::Kind::Class;
  Reference.Address = 0x2900;
  Reference.Size = 8;
  Reference.Name = Class.Name;
  F.Image.ObjCSourceReferences.emplace(Reference.Address, Reference);
  ObjCReceiverTypeHint Receiver;
  Receiver.Origin = ObjCReceiverTypeHint::OriginKind::ClassReference;
  Receiver.Address = Reference.Address;
  Receiver.ClassName = Class.Name;
  Receiver.IsClassMethod = true;
  for (const auto &[Selector, Encoding, Count] :
       {std::tuple<const char *, const char *, unsigned>{
            "transformerUsingForwardBlock:", "@24@0:8@?16", 3},
        {"transformerUsingForwardBlock:reverseBlock:",
         "@32@0:8@?16@?24", 4}}) {
    SCOPED_TRACE(Selector);
    F.Image.ObjCMethods.clear();
    ObjCMethod Method;
    Method.Implementation = 0x1300;
    Method.MetadataAddress = 0x2800;
    Method.ClassAddress = Class.Address;
    Method.ClassName = Class.Name;
    Method.Selector = Selector;
    Method.TypeEncoding = Encoding;
    Method.IsClassMethod = true;
    Method.Status = "supported";
    Method.TypeHint = parseObjCMethodEncoding(Selector, Encoding);
    ASSERT_TRUE(Method.TypeHint);
    F.Image.ObjCMethods.push_back(Method);
    SourceCallTypeHint Call;
    Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Call.Selector = Selector;
    Call.Receiver = Receiver;
    const auto Declaration =
        objcReceiverSourceTypeHint(F.Image, Call.Selector, Receiver);
    ASSERT_TRUE(Declaration.Signature);
    Call.Signature = *Declaration.Signature;
    for (unsigned Parameter = 2; Parameter < Count; ++Parameter) {
      const auto Contract = objcBlockParameterContract(F.Image, Call, Parameter);
      ASSERT_TRUE(Contract);
      EXPECT_EQ(Contract->Storage,
                ObjCBlockParameterContract::Lifetime::Copied);
      ASSERT_EQ(Contract->Signature.Parameters.size(), 4U);
      EXPECT_EQ(Contract->Signature.ReturnType->Kind, NdTypeKind::Ptr);
    }
    EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 1));
    auto Changed = Call;
    Changed.Receiver.reset();
    EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 2));
    Changed = Call;
    Changed.Signature.Parameters.pop_back();
    EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 2));
    F.Image.ObjCMethods.front().TypeEncoding = "@24@0:8@16";
    EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 2));
  }
}

TEST(ObjCBlockSources, WMFAsyncBlockOperationCopiesEscapingSwiftClosure) {
  BlockFixture F;
  F.Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  ObjCClass Base;
  Base.Name = "WMFAsyncOperation";
  Base.Address = 0x2680;
  Base.SuperclassName = "NSOperation";
  Base.InheritanceStatus = "resolved";
  F.Image.ObjCClasses.push_back(Base);
  ObjCClass Class;
  Class.Name = "WMFAsyncBlockOperation";
  Class.Address = 0x2700;
  Class.SuperclassName = Base.Name;
  Class.InheritanceStatus = "resolved";
  F.Image.ObjCClasses.push_back(Class);
  ObjCMethod Method;
  Method.Implementation = 0x1300;
  Method.MetadataAddress = 0x2800;
  Method.ClassAddress = Class.Address;
  Method.ClassName = Class.Name;
  Method.Selector = "initWithAsyncBlock:";
  Method.TypeEncoding = "@24@0:8@?16";
  Method.Status = "supported";
  Method.TypeHint = parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  ASSERT_TRUE(Method.TypeHint);
  F.Image.ObjCMethods.push_back(Method);
  ObjCMethod Caller;
  Caller.Implementation = 0x1200;
  Caller.ClassAddress = Class.Address;
  Caller.ClassName = Class.Name;
  Caller.Selector = "submit:";
  Caller.TypeEncoding = "v24@0:8@16";
  Caller.Status = "supported";
  Caller.TypeHint = parseObjCMethodEncoding(Caller.Selector, Caller.TypeEncoding);
  ASSERT_TRUE(Caller.TypeHint);
  F.Image.ObjCMethods.push_back(Caller);
  const auto Receiver = objcMethodReceiverTypeHint(F.Image, Caller.Implementation);
  ASSERT_TRUE(Receiver);
  SourceCallTypeHint Call;
  Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Call.Selector = Method.Selector;
  Call.Receiver = *Receiver;
  const auto Declaration =
      objcReceiverSourceTypeHint(F.Image, Call.Selector, *Receiver);
  ASSERT_TRUE(Declaration.Signature);
  Call.Signature = *Declaration.Signature;
  const auto Contract = objcBlockParameterContract(F.Image, Call, 2);
  ASSERT_TRUE(Contract);
  EXPECT_EQ(Contract->Storage,
            ObjCBlockParameterContract::Lifetime::Copied);
  EXPECT_EQ(Contract->Signature.Parameters.size(), 2U);
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 1));

  auto Changed = Call;
  Changed.Receiver.reset();
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 2));
  Changed = Call;
  Changed.Signature.Parameters.pop_back();
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 2));
  F.Image.ObjCMethods.front().TypeEncoding = "@24@0:8@16";
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 2));
  F.Image.ObjCMethods.front().TypeEncoding = Method.TypeEncoding;
  F.Image.ObjCClasses.back().SuperclassName = "OtherOperation";
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 2));
}

TEST(ObjCBlockSources, WMFSessionCopiesJSONCompletion) {
  BlockFixture F;
  F.Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  ObjCClass Class;
  Class.Name = "WMFSession";
  Class.Address = 0x2700;
  Class.SuperclassName = "NSObject";
  Class.InheritanceStatus = "resolved";
  F.Image.ObjCClasses.push_back(Class);
  ObjCMethod Method;
  Method.Implementation = 0x1300;
  Method.MetadataAddress = 0x2800;
  Method.ClassAddress = Class.Address;
  Method.ClassName = Class.Name;
  Method.Selector =
      "getJSONDictionaryFromURL:ignoreCache:completionHandler:";
  Method.TypeEncoding = "@36@0:8@16B24@?28";
  Method.Status = "supported";
  Method.TypeHint = parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
  ASSERT_TRUE(Method.TypeHint);
  F.Image.ObjCMethods.push_back(Method);
  ObjCMethod Caller;
  Caller.Implementation = 0x1200;
  Caller.ClassAddress = Class.Address;
  Caller.ClassName = Class.Name;
  Caller.Selector = "submit:";
  Caller.TypeEncoding = "v24@0:8@16";
  Caller.Status = "supported";
  Caller.TypeHint = parseObjCMethodEncoding(Caller.Selector, Caller.TypeEncoding);
  ASSERT_TRUE(Caller.TypeHint);
  F.Image.ObjCMethods.push_back(Caller);
  const auto Receiver = objcMethodReceiverTypeHint(F.Image, Caller.Implementation);
  ASSERT_TRUE(Receiver);
  SourceCallTypeHint Call;
  Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Call.Selector = Method.Selector;
  Call.Receiver = *Receiver;
  const auto Declaration =
      objcReceiverSourceTypeHint(F.Image, Call.Selector, *Receiver);
  ASSERT_TRUE(Declaration.Signature);
  Call.Signature = *Declaration.Signature;
  const auto Contract = objcBlockParameterContract(F.Image, Call, 4);
  ASSERT_TRUE(Contract);
  EXPECT_EQ(Contract->Storage, ObjCBlockParameterContract::Lifetime::Copied);
  EXPECT_EQ(Contract->Signature.Parameters.size(), 4U);
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 3));

  auto Changed = Call;
  Changed.Receiver.reset();
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 4));
  Changed = Call;
  Changed.Signature.Parameters.pop_back();
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 4));
  F.Image.ObjCMethods.front().TypeEncoding = "@36@0:8@16B24@28";
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 4));
}

TEST(ObjCBlockSources, WMFInstanceMethodsCopyAsyncCallbacks) {
  struct Case {
    const char *Owner;
    const char *Selector;
    const char *Encoding;
    unsigned Parameter;
    unsigned CallbackParameters;
  };
  constexpr Case Cases[] = {
      {"MWKDataStore", "setupCoreDataStackWithContainerURL:completion:",
       "v32@0:8@16@?24", 3, 1},
      {"MWKDataStore",
       "performBackgroundCoreDataOperationOnATemporaryContext:",
       "v24@0:8@?16", 2, 2},
      {"WMFFeedContentSource", "fetchContentForDate:force:completion:",
       "v36@0:8@16B24@?28", 4, 3},
      {"WMFFeedContentFetcher",
       "fetchFeedContentForURL:date:force:failure:success:",
       "v52@0:8@16@24B32@?36@?44", 5, 2},
      {"WMFFeedContentFetcher",
       "fetchFeedContentForURL:date:force:failure:success:",
       "v52@0:8@16@24B32@?36@?44", 6, 2},
      {"WMFAnnouncementsFetcher",
       "fetchAnnouncementsForURL:force:failure:success:",
       "v44@0:8@16B24@?28@?36", 4, 2},
      {"WMFAnnouncementsFetcher",
       "fetchAnnouncementsForURL:force:failure:success:",
       "v44@0:8@16B24@?28@?36", 5, 2},
      {"WMFRelatedSearchFetcher",
       "fetchRelatedArticlesForArticleWithURL:completion:",
       "v32@0:8@16@?24", 3, 3},
      {"WMFExploreFeedContentController",
       "updateExploreFeedPreferences:willTurnOnContentGroupOrLanguage:"
       "waitForCallbackFromCoordinator:apply:updateFeed:",
       "v40@0:8@?16B24B28B32B36", 2, 2},
      {"WMFNearbyContentSource",
       "getGroupForLocation:inManagedObjectContext:force:completion:failure:",
       "v52@0:8@16@24B32@?36@?44", 5, 4},
      {"WMFNearbyContentSource",
       "getGroupForLocation:inManagedObjectContext:force:completion:failure:",
       "v52@0:8@16@24B32@?36@?44", 6, 2},
      {"WMFEchoSubscriptionFetcher",
       "subscribeWithSiteURL:deviceToken:completion:",
       "v40@0:8@16@24@?32", 4, 2},
      {"WMFEchoSubscriptionFetcher",
       "unsubscribeWithSiteURL:deviceToken:completion:",
       "v40@0:8@16@24@?32", 4, 2},
      {"WMFExploreFeedContentController", "performBackgroundFetch:",
       "v24@0:8@?16", 2, 2},
      {"MWKImageInfoFetcher",
       "fetchGalleryInfoForImageFiles:fromSiteURL:success:failure:",
       "@48@0:8@16@24@?32@?40", 4, 2},
      {"MWKImageInfoFetcher",
       "fetchGalleryInfoForImageFiles:fromSiteURL:success:failure:",
       "@48@0:8@16@24@?32@?40", 5, 2},
  };
  for (const auto &C : Cases) {
    BlockFixture F;
    F.Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    ObjCClass Class;
    Class.Name = C.Owner;
    Class.Address = 0x2700;
    Class.SuperclassName = "NSObject";
    Class.InheritanceStatus = "resolved";
    F.Image.ObjCClasses.push_back(Class);
    ObjCMethod Method;
    Method.Implementation = 0x1300;
    Method.MetadataAddress = 0x2800;
    Method.ClassAddress = Class.Address;
    Method.ClassName = Class.Name;
    Method.Selector = C.Selector;
    Method.TypeEncoding = C.Encoding;
    Method.Status = "supported";
    Method.TypeHint = parseObjCMethodEncoding(Method.Selector,
                                              Method.TypeEncoding);
    ASSERT_TRUE(Method.TypeHint);
    F.Image.ObjCMethods.push_back(Method);
    ObjCMethod Caller;
    Caller.Implementation = 0x1200;
    Caller.ClassAddress = Class.Address;
    Caller.ClassName = Class.Name;
    Caller.Selector = "submit:";
    Caller.TypeEncoding = "v24@0:8@16";
    Caller.Status = "supported";
    Caller.TypeHint = parseObjCMethodEncoding(Caller.Selector,
                                              Caller.TypeEncoding);
    ASSERT_TRUE(Caller.TypeHint);
    F.Image.ObjCMethods.push_back(Caller);
    const auto Receiver =
        objcMethodReceiverTypeHint(F.Image, Caller.Implementation);
    ASSERT_TRUE(Receiver);
    SourceCallTypeHint Call;
    Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Call.Selector = Method.Selector;
    Call.Receiver = *Receiver;
    const auto Declaration =
        objcReceiverSourceTypeHint(F.Image, Call.Selector, *Receiver);
    ASSERT_TRUE(Declaration.Signature);
    Call.Signature = *Declaration.Signature;
    const auto Contract = objcBlockParameterContract(F.Image, Call, C.Parameter);
    ASSERT_TRUE(Contract) << C.Selector;
    EXPECT_EQ(Contract->Storage, ObjCBlockParameterContract::Lifetime::Copied);
    EXPECT_EQ(Contract->Signature.Parameters.size(), C.CallbackParameters);
    if (std::string(C.Selector) == "performBackgroundFetch:") {
      ASSERT_EQ(Contract->Signature.Parameters[1].Type->Kind, NdTypeKind::Int);
      EXPECT_FALSE(Contract->Signature.Parameters[1].Type->IsSigned);
    }
    EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 1));
    auto Changed = Call;
    Changed.Receiver.reset();
    EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, C.Parameter));
    F.Image.ObjCMethods.front().TypeEncoding = "v16@0:8";
    EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, C.Parameter));
  }
}

TEST(ObjCBlockSources, WMFContentGroupEnumerationBorrowsCallback) {
  BlockFixture F;
  F.Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/CoreData.framework/CoreData",
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  ObjCClass Class;
  Class.Name = "NSManagedObjectContext";
  Class.Address = 0x2700;
  Class.SuperclassName = "NSObject";
  Class.InheritanceStatus = "resolved";
  F.Image.ObjCClasses.push_back(Class);
  ObjCMethod Method;
  Method.Implementation = 0x1300;
  Method.MetadataAddress = 0x2800;
  Method.CategoryAddress = 0x2900;
  Method.CategoryName = "WMFArticle";
  Method.ClassAddress = Class.Address;
  Method.ClassName = Class.Name;
  Method.Selector = "enumerateContentGroupsOfKind:withBlock:";
  Method.TypeEncoding = "v28@0:8i16@?20";
  Method.Status = "supported";
  Method.TypeHint = parseObjCMethodEncoding(Method.Selector,
                                            Method.TypeEncoding);
  ASSERT_TRUE(Method.TypeHint);
  F.Image.ObjCMethods.push_back(Method);
  ObjCMethod Caller;
  Caller.Implementation = 0x1200;
  Caller.ClassAddress = Class.Address;
  Caller.ClassName = Class.Name;
  Caller.Selector = "submit:";
  Caller.TypeEncoding = "v24@0:8@16";
  Caller.Status = "supported";
  Caller.TypeHint = parseObjCMethodEncoding(Caller.Selector,
                                            Caller.TypeEncoding);
  ASSERT_TRUE(Caller.TypeHint);
  F.Image.ObjCMethods.push_back(Caller);
  const auto Receiver =
      objcMethodReceiverTypeHint(F.Image, Caller.Implementation);
  ASSERT_TRUE(Receiver);
  SourceCallTypeHint Call;
  Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Call.Selector = Method.Selector;
  Call.Receiver = *Receiver;
  const auto Declaration =
      objcReceiverSourceTypeHint(F.Image, Call.Selector, *Receiver);
  ASSERT_TRUE(Declaration.Signature);
  Call.Signature = *Declaration.Signature;
  const auto Contract = objcBlockParameterContract(F.Image, Call, 3);
  ASSERT_TRUE(Contract);
  EXPECT_EQ(Contract->Storage,
            ObjCBlockParameterContract::Lifetime::NonEscaping);
  EXPECT_EQ(Contract->Signature.Parameters.size(), 3U);
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 2));
  auto Changed = Call;
  Changed.Receiver.reset();
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Changed, 3));
  F.Image.ObjCMethods.front().CategoryName = "Other";
  EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, 3));
}

TEST(ObjCBlockSources, WMFCollectionSwiftExtensionsCopyCallbacks) {
  struct Case {
    const char *Owner;
    const char *Selector;
    const char *Encoding;
    unsigned Parameter;
    unsigned CallbackParameters;
    NdTypeKind ReturnKind;
  };
  constexpr Case Cases[] = {
      {"NSArray", "wmf_map:", "@24@0:8@?16", 2, 2, NdTypeKind::Ptr},
      {"NSArray", "wmf_select:", "@24@0:8@?16", 2, 2, NdTypeKind::Int},
      {"NSArray", "wmf_match:", "@24@0:8@?16", 2, 2, NdTypeKind::Int},
      {"NSArray", "wmf_reduce:withBlock:", "@32@0:8@16@?24", 3, 3,
       NdTypeKind::Ptr},
      {"NSSet", "wmf_map:", "@24@0:8@?16", 2, 2, NdTypeKind::Ptr},
      {"NSSet", "wmf_select:", "@24@0:8@?16", 2, 2, NdTypeKind::Int},
      {"NSSet", "wmf_match:", "@24@0:8@?16", 2, 2, NdTypeKind::Int},
      {"NSSet", "wmf_reduce:withBlock:", "@32@0:8@16@?24", 3, 3,
       NdTypeKind::Ptr},
      {"NSDictionary", "wmf_map:", "@24@0:8@?16", 2, 3, NdTypeKind::Ptr},
      {"NSDictionary", "wmf_select:", "@24@0:8@?16", 2, 3,
       NdTypeKind::Int},
      {"NSDictionary", "wmf_match:", "@24@0:8@?16", 2, 3,
       NdTypeKind::Int},
      {"NSDictionary", "wmf_reduce:withBlock:", "@32@0:8@16@?24", 3, 4,
       NdTypeKind::Ptr},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(std::string(Case.Owner) + " " + Case.Selector);
    BlockFixture F;
    F.Image.DynInfo.NeededLibs = {
        "/System/Library/Frameworks/Foundation.framework/Foundation"};
    ObjCMethod Method;
    Method.Implementation = 0x1300;
    Method.MetadataAddress = 0x2800;
    Method.CategoryAddress = 0x2700;
    Method.CategoryName = "WMFBlocksKit";
    Method.ClassName = Case.Owner;
    Method.Selector = Case.Selector;
    Method.TypeEncoding = Case.Encoding;
    Method.Status = "supported";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    ASSERT_TRUE(Method.TypeHint);
    F.Image.ObjCMethods.push_back(Method);
    ObjCMethod Caller;
    Caller.Implementation = 0x1200;
    Caller.ClassName = Case.Owner;
    Caller.Selector = "submit:";
    Caller.TypeEncoding = "v24@0:8@16";
    Caller.Status = "supported";
    Caller.TypeHint =
        parseObjCMethodEncoding(Caller.Selector, Caller.TypeEncoding);
    ASSERT_TRUE(Caller.TypeHint);
    F.Image.ObjCMethods.push_back(Caller);
    const auto Receiver = objcMethodReceiverTypeHint(F.Image, Caller.Implementation);
    ASSERT_TRUE(Receiver);
    SourceCallTypeHint Call;
    Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
    Call.Selector = Case.Selector;
    Call.Receiver = *Receiver;
    const auto Declaration =
        objcReceiverSourceTypeHint(F.Image, Case.Selector, *Receiver);
    ASSERT_TRUE(Declaration.Signature);
    Call.Signature = *Declaration.Signature;
    const auto Contract =
        objcBlockParameterContract(F.Image, Call, Case.Parameter);
    ASSERT_TRUE(Contract);
    EXPECT_EQ(Contract->Storage,
              ObjCBlockParameterContract::Lifetime::Copied);
    EXPECT_EQ(Contract->Signature.Parameters.size(), Case.CallbackParameters);
    EXPECT_EQ(Contract->Signature.ReturnType->Kind, Case.ReturnKind);
    EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, Case.Parameter - 1));
    F.Image.ObjCMethods.front().CategoryAddress = 0;
    EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, Case.Parameter));
  }
}

TEST(ObjCBlockSources, AppleAsyncConsumersRequireExactSDKBlockContract) {
  struct Case {
    const char *Framework;
    const char *Owner;
    const char *Selector;
    unsigned Parameter;
    unsigned CallbackParameters;
    bool ClassMethod;
    bool X64Available;
  };
  constexpr Case Cases[] = {
      {"Foundation", "NSBlockOperation", "blockOperationWithBlock:", 2, 1, true,
       true},
      {"CoreData", "NSPersistentContainer",
       "loadPersistentStoresWithCompletionHandler:", 2, 3, false, true},
      {"CoreLocation", "CLGeocoder",
       "reverseGeocodeLocation:completionHandler:", 3, 3, false, true},
      {"UserNotifications", "UNUserNotificationCenter",
       "getNotificationSettingsWithCompletionHandler:", 2, 2, false, true},
      {"UserNotifications", "UNUserNotificationCenter",
       "requestAuthorizationWithOptions:completionHandler:", 3, 3, false,
       false},
  };
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const auto &Case : Cases) {
      SCOPED_TRACE(Case.Selector);
      BlockFixture F;
      F.Image.Arch = Architecture;
      F.Image.DynInfo.NeededLibs = {
          std::string("/System/Library/Frameworks/") + Case.Framework +
              ".framework/" + Case.Framework,
          "/System/Library/Frameworks/Foundation.framework/Foundation"};
      ObjCMethod Method;
      Method.Implementation = 0x1200;
      Method.ClassName = Case.Owner;
      Method.Selector = "submit:";
      Method.IsClassMethod = Case.ClassMethod;
      Method.TypeHint = parseObjCMethodEncoding("submit:", "v24@0:8@16");
      ASSERT_TRUE(Method.TypeHint);
      F.Image.ObjCMethods.push_back(Method);
      auto Receiver =
          objcMethodReceiverTypeHint(F.Image, Method.Implementation);
      ASSERT_TRUE(Receiver);
      SourceCallTypeHint Call;
      Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
      Call.Selector = Case.Selector;
      Call.Receiver = *Receiver;
      auto Parent =
          objcReceiverSourceTypeHint(F.Image, Call.Selector, *Receiver);
      ASSERT_TRUE(Parent.Signature);
      Call.Signature = *Parent.Signature;
      auto Contract = objcBlockParameterContract(F.Image, Call, Case.Parameter);
      const bool Available = Architecture != Arch::X64 || Case.X64Available;
      EXPECT_EQ(bool(Contract), Available);
      if (Contract) {
        EXPECT_EQ(Contract->Storage,
                  ObjCBlockParameterContract::Lifetime::Copied);
        EXPECT_EQ(Contract->Signature.ReturnType->Kind, NdTypeKind::Void);
        EXPECT_EQ(Contract->Signature.Parameters.size(),
                  Case.CallbackParameters);
      }
      EXPECT_FALSE(
          objcNonEscapingBlockSignature(F.Image, Call, Case.Parameter));

      auto Changed = Call;
      Changed.Receiver.reset();
      EXPECT_FALSE(
          objcBlockParameterContract(F.Image, Changed, Case.Parameter));
      Changed = Call;
      Changed.Signature.Parameters.pop_back();
      EXPECT_FALSE(
          objcBlockParameterContract(F.Image, Changed, Case.Parameter));
      EXPECT_FALSE(
          objcBlockParameterContract(F.Image, Call, Case.Parameter - 1));
      F.Image.DynInfo.NeededLibs = {"/tmp/unrelated.framework/unrelated"};
      EXPECT_FALSE(objcBlockParameterContract(F.Image, Call, Case.Parameter));
    }
}

TEST(ObjCBlockSources, ConstructionRejectsLateUnsafeEdgesTransactionally) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SourceFixture F(true);
    auto &Caller = F.caller();
    HighStmt Control;
    Control.Kind = StmtKind::While;
    Control.Cond = parameter(1, Caller.Params[1].Type);
    auto Invoke = Caller.Body.back();
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = Invoke.RetVal;
    Invoke.RetVal.reset();
    Control.Body = {Invoke,
                    store(frame(F.Image, -24), HighExpr::makeConst(0x9999, 8))};
    Caller.Body.back() = Control;
    Caller.Body.push_back(ret(HighExpr::makeConst(0, 4)));
    if (Mutation == 1) {
      Caller.Body = SourceFixture(true).caller().Body;
      Caller.Body.back().Addr = F.Caller + 16;
      Control.Kind = StmtKind::Goto;
      Control.GotoTarget = F.Caller + 16;
      Control.Body.clear();
      Caller.Body.insert(Caller.Body.begin() + 1, Control);
    }
    if (Mutation == 2) {
      Control.Kind = StmtKind::Goto;
      Control.GotoTarget = 0x9999;
      Control.Body.clear();
      Caller.Body.push_back(Control);
    }
    if (Mutation == 3) {
      Control.Kind = StmtKind::ItaniumTry;
      Caller.Body.push_back(Control);
    }
    const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    EXPECT_TRUE(Plan.StackBlocks.empty());
    const auto Bound =
        bindObjCBlockSourceReferences(Caller, F.Image, Plan, F.functions());
    // An invalid graph can stop before the first ISA write is visited. It
    // still supplies no block bindings, and ordinary source admission fails.
    EXPECT_FALSE(
        Bound.Limitation.empty() &&
        bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());
  }
}
TEST(ObjCBlockSources,
     InvokeCannotReadPaddingOrEscapeThroughReturnStoreOrUnknownCall) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    SourceFixture F(true);
    auto &Invoke = F.Result.HighFuncs[0];
    if (Mutation == 0)
      Invoke.Body[0].RetVal->Operands[1]->Type = NdType::makeInt(8);
    if (Mutation == 1)
      Invoke.Body[0].RetVal = parameter(0, Invoke.Params[0].Type);
    if (Mutation == 2)
      Invoke.Body.insert(Invoke.Body.begin(),
                         store(HighExpr::makeConst(0x2990, 8),
                               parameter(0, Invoke.Params[0].Type)));
    if (Mutation == 3)
      Invoke.Body[0].RetVal =
          HighExpr::makeCall({}, 0, {parameter(0, Invoke.Params[0].Type)});
    auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    auto Bound =
        bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions());
    EXPECT_FALSE(Bound.Limitation.empty()) << Mutation;
  }
}
TEST(ObjCBlockSources,
     DescriptorABIConflictAndUnrecoveredInvokeCannotAcquireSourceStatus) {
  SourceFixture F(false);
  auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  F.Result.HighFuncs[0].SourceTypeHint.reset();
  EXPECT_FALSE(
      bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions())
          .Limitation.empty());
  SourceFixture G(true);
  auto P = discoverObjCBlockSources(G.Image, G.Result);
  auto Bad = P.StackBlocks[G.Caller][0].Descriptor;
  Bad.InvokeTypeHint->ReturnType = NdType::makeInt(8);
  EXPECT_FALSE(objc_block_source_detail::publish(P, Bad, G.Invoke));
  EXPECT_FALSE(
      bindObjCBlockSourceReferences(G.caller(), G.Image, P, G.functions())
          .Limitation.empty());
}
TEST(ObjCBlockSources, SourceHintsRerunOnlyForNewOrNativeAnalysisBindings) {
  SourceFixture F(false);
  auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  PipelineOptions O;
  EXPECT_EQ(applyObjCBlockInvokeHints(Plan, O), 1U);
  EXPECT_EQ(applyObjCBlockInvokeHints(Plan, O), 0U);
  O.SourceTypeHints[F.Invoke].Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  EXPECT_EQ(applyObjCBlockInvokeHints(Plan, O), 1U);
  O.SourceTypeHints[F.Invoke].Origin =
      SourceFunctionTypeHint::OriginKind::ObjCRuntime;
  EXPECT_EQ(applyObjCBlockInvokeHints(Plan, O), 0U);
}

TEST(ObjCBlockSources,
     EscapingStrongCaptureRequiresBothRecoveredOwnershipHelpers) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      OwnedSourceFixture F(Architecture);
      if (Mutation == 1)
        F.caller().Body[4].StoreVal = HighExpr::makeConst(0, 4);
      if (Mutation == 2)
        F.Result.HighFuncs.back().SourceTypeHint.reset();
      if (Mutation == 3)
        F.Result.HighFuncs.back()
            .Body[0]
            .CallExpr->Operands[0]
            ->Operands[0]
            ->Operands[1] = HighExpr::makeConst(40, 8);
      if (Mutation == 4)
        F.Image.ImportPtrSlots[F.CopyImport] = "_unknown_consumer";
      if (Mutation == 5)
        F.caller().Body.back().RetVal = frame(F.Image, -48);
      if (Mutation == 6)
        F.Result.HighFuncs.erase(F.Result.HighFuncs.begin() + 3);
      if (Mutation == 7) {
        F.Image.ImportPtrSlots[F.CopyImport] = "_objc_retain";
        F.caller().Body.back().RetVal->SourceCallHint =
            std::make_shared<SourceCallTypeHint>(
                *objcRuntimeSourceCallHint(F.Image, F.CopyImport));
      }
      auto Plan = discoverObjCBlockSources(F.Image, F.Result);
      const auto Bound = bindObjCBlockSourceReferences(F.caller(), F.Image,
                                                       Plan, F.functions());
      const bool Accepted =
          !Plan.StackBlocks.empty() && Bound.Limitation.empty();
      EXPECT_EQ(Accepted, Mutation == 0)
          << static_cast<int>(Architecture) << ": " << Mutation << ": "
          << Bound.Limitation;
      if (Mutation)
        continue;
      EXPECT_EQ(Bound.Dependencies,
                (std::set<va_t>{F.Invoke, F.Copy, F.Dispose}));
      EXPECT_EQ(Bound.Function.Body[1].StoreVal->Kind, ExprKind::Const);
      EXPECT_EQ(Bound.Function.Body[1].StoreVal->ConstVal, 0xc2000000U);
      EXPECT_EQ(F.caller().Body[1].StoreVal->Kind, ExprKind::Load);
      PipelineOptions Options;
      EXPECT_EQ(applyObjCBlockInvokeHints(Plan, Options), 3U);
      EXPECT_EQ(applyObjCBlockInvokeHints(Plan, Options), 0U);
      std::set<std::string> Shared;
      auto Text = renderObjCBlockSourceHelpers(Plan, Bound.Descriptors,
                                               Bound.Literals, Shared);
      EXPECT_NE(Text.find("void (*copy)(void *, void *)"), std::string::npos);
      EXPECT_NE(Text.find("&neverd_block_helper_1400"), std::string::npos);
      EXPECT_NE(Text.find("&neverd_block_helper_1410"), std::string::npos);
      EXPECT_EQ(Shared.size(), 3U);
    }
  }
}

TEST(ObjCBlockSources,
     RuntimeObjectAssignmentWritesOnlyInitializedStrongFields) {
  for (uint32_t FieldFlag : {3U, 7U})
    for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
      for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
        OwnedSourceFixture F(Architecture);
        F.Image.ImportPtrSlots[F.CopyImport] = "__Block_copy";
        F.Image.ImportPtrSlots[F.RetainImport] = "__Block_object_assign";
        F.Image.ImportPtrSlots[F.ReleaseImport] = "__Block_object_dispose";
        F.caller().Body.back().RetVal->SourceCallHint =
            std::make_shared<SourceCallTypeHint>(
                *darwinRuntimeSourceCallHint(F.Image, F.CopyImport));
        auto &Copy = F.Result.HighFuncs[3];
        auto &Dispose = F.Result.HighFuncs[4];
        auto Object = Copy.Body[0].CallExpr->Operands[0];
        auto Destination = HighExpr::makeBinop(
            NdOp::INT_ADD, parameter(0, Copy.Params[0].Type),
            HighExpr::makeConst(32, 8));
        Copy.Body[0].CallExpr->Operands = {Destination, Object,
                                           HighExpr::makeConst(FieldFlag, 4)};
        Copy.Body[0].CallExpr->SourceCallHint =
            std::make_shared<SourceCallTypeHint>(
                *darwinRuntimeSourceCallHint(F.Image, F.RetainImport));
        Dispose.Body[0].CallExpr->Operands.push_back(
            HighExpr::makeConst(FieldFlag, 4));
        Dispose.Body[0].CallExpr->SourceCallHint =
            std::make_shared<SourceCallTypeHint>(
                *darwinRuntimeSourceCallHint(F.Image, F.ReleaseImport));
        if (Mutation == 1)
          Destination->Operands[1] = HighExpr::makeConst(24, 8);
        if (Mutation == 2)
          Destination->Operands[1] = HighExpr::makeConst(36, 8);
        if (Mutation == 3)
          Copy.Body[0].CallExpr->Operands[2] = HighExpr::makeConst(8, 4);
        if (Mutation == 4)
          F.Image.ImportPtrSlots[F.RetainImport] =
              "__Block_object_assign_unproved";
        if (Mutation == 5)
          F.put64(F.Descriptor + 40, 0);
        if (Mutation == 6)
          F.caller().Body[4].StoreVal = HighExpr::makeConst(0, 4);
        if (Mutation == 7)
          Copy.Body[0].CallExpr->Operands[1] =
              parameter(0, Copy.Params[0].Type);
        auto Plan = discoverObjCBlockSources(F.Image, F.Result);
        const auto Capture = Plan.CapturedCallFields.find(F.Invoke);
        if (Mutation == 0 && FieldFlag == 7) {
          ASSERT_EQ(Plan.StackBlocks[F.Caller].size(), 1U)
              << Plan.Rejections[F.Caller];
          const auto &Block = Plan.StackBlocks[F.Caller][0];
          std::set<uint64_t> StrongFields{32};
          std::map<uint64_t, std::set<uint64_t>> AssignmentFlags;
          std::set<std::pair<va_t, size_t>> Active;
          std::string Reason;
          const ObjCBlockSourceContext Source(F.Image);
          EXPECT_TRUE(objc_block_source_detail::noEscape(
              Source, F.functions(), F.Copy, 0, &Block.InitializedCaptures,
              Active, Reason, &StrongFields, &AssignmentFlags))
              << Reason;
          EXPECT_EQ(AssignmentFlags[32], (std::set<uint64_t>{7}));
          ASSERT_NE(Capture, Plan.CapturedCallFields.end());
          EXPECT_EQ(Capture->second.BlockWords, (std::set<uint64_t>{32}));
          EXPECT_TRUE(Capture->second.ScalarWords.count(32));
        } else {
          EXPECT_EQ(Capture, Plan.CapturedCallFields.end());
        }
        const auto Bound = bindObjCBlockSourceReferences(F.caller(), F.Image,
                                                         Plan, F.functions());
        EXPECT_EQ(!Plan.StackBlocks.empty() && Bound.Limitation.empty(),
                  Mutation == 0)
            << static_cast<int>(Architecture) << ':' << Mutation << ':'
            << Bound.Limitation;
        if (Mutation == 0)
          EXPECT_EQ(Bound.Dependencies,
                    (std::set<va_t>{F.Invoke, F.Copy, F.Dispose}));
      }
    }
}

TEST(ObjCBlockSources,
     InvokeMayPassItsProvenNestedBlockWithoutExposingOuterContext) {
  SourceFixture F(true);
  auto Outer = F.caller();
  Outer.Entry = 0x1500;
  Outer.Name = "nested_block_invoke";
  F.Result.HighFuncs.push_back(std::move(Outer));
  const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  ASSERT_EQ(Plan.StackBlocks.at(0x1500).size(), 1U);
  const auto &Function = F.Result.HighFuncs.back();
  const auto *Consumer = Function.Body.back().RetVal.get();
  EXPECT_TRUE(
      Plan.StackBlocks.at(0x1500)[0].ValidatedConsumers.count(Consumer));

  const ObjCBlockSourceContext Source(F.Image);
  const std::set<uint64_t> Initialized;
  std::set<std::pair<va_t, size_t>> Active;
  std::string Reason;
  EXPECT_FALSE(objc_block_source_detail::noEscape(
      Source, F.functions(), Function.Entry, 0, &Initialized, Active, Reason));
  EXPECT_NE(Reason.find("exposes private context storage"), std::string::npos);
  Reason.clear();
  EXPECT_TRUE(objc_block_source_detail::noEscape(
      Source, F.functions(), Function.Entry, 0, &Initialized, Active, Reason,
      nullptr, nullptr, &Plan))
      << Reason;

  Function.Body.back().RetVal->SourceCallHint.reset();
  Reason.clear();
  EXPECT_FALSE(objc_block_source_detail::noEscape(
      Source, F.functions(), Function.Entry, 0, &Initialized, Active, Reason,
      nullptr, nullptr, &Plan));
}

TEST(ObjCBlockSources,
     NSArrayFastEnumerationBorrowsOnlyDisjointBoundedFrameRanges) {
  SourceFixture F(true);
  F.Image.DynInfo.NeededLibs = {
      "/System/Library/Frameworks/Foundation.framework/Foundation"};
  ObjCMethod Method;
  Method.Implementation = F.Consumer;
  Method.ClassName = "NSArray";
  Method.Selector = "test";
  Method.TypeHint = parseObjCMethodEncoding("test", "v16@0:8");
  ASSERT_TRUE(Method.TypeHint);
  F.Image.ObjCMethods.push_back(Method);
  const auto Receiver = objcMethodReceiverTypeHint(F.Image, F.Consumer);
  ASSERT_TRUE(Receiver);
  EXPECT_EQ(objcReceiverInstanceClassName(F.Image, *Receiver),
            std::optional<std::string>{"NSArray"});

  SourceCallTypeHint Binding;
  Binding.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Binding.TargetName = "objc_msgSend";
  Binding.Selector = "countByEnumeratingWithState:objects:count:";
  Binding.Receiver = *Receiver;
  const auto Declaration =
      objcReceiverSourceTypeHint(F.Image, Binding.Selector, *Receiver);
  ASSERT_TRUE(Declaration.Signature);
  Binding.Signature = *Declaration.Signature;

  auto &Function = F.Result.HighFuncs[1];
  Function.FrameSize = 256;
  auto Call = HighExpr::makeCall(
      "objc_msgSend", 0,
      {HighExpr::makeConst(0x3000, 8), HighExpr::makeConst(0x2600, 8),
       frame(F.Image, -256), frame(F.Image, -192),
       HighExpr::makeConst(16, 8)});
  Call->Type = Binding.Signature.ReturnType;
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Binding);
  EXPECT_TRUE(objcSourceCallBound(*Call, F.Image, F.functions()));
  HighStmt Send;
  Send.Kind = StmtKind::Call;
  Send.CallExpr = Call;
  Function.Body = {store(frame(F.Image, -64),
                         parameter(0, Function.Params[0].Type)),
                   Send, ret(HighExpr::makeConst(0, 4))};
  const ObjCBlockSourceContext Source(F.Image);
  std::set<std::pair<va_t, size_t>> Active;
  std::string Reason;
  auto Safe = [&] {
    Reason.clear();
    return objc_block_source_detail::noEscape(
        Source, F.functions(), Function.Entry, 0, nullptr, Active, Reason);
  };
  EXPECT_TRUE(Safe()) << Reason;
  ObjCBlockSourcePlan Nested;
  ObjCStackBlockSource Literal;
  Literal.FrameOffset = -152;
  Literal.Descriptor.LiteralSize = 40;
  Nested.StackBlocks[Function.Entry].push_back(Literal);
  Reason.clear();
  EXPECT_FALSE(objc_block_source_detail::noEscape(
      Source, F.functions(), Function.Entry, 0, nullptr, Active, Reason,
      nullptr, nullptr, &Nested)); // borrowed buffer overlaps full literal
  Call->Operands[4] = HighExpr::makeConst(17, 8);
  EXPECT_FALSE(Safe()); // output buffer now overlaps the context spill
  Call->Operands[4] = HighExpr::makeConst(16, 8);
  Call->Operands[2] = frame(F.Image, -64);
  EXPECT_FALSE(Safe()); // state overlaps the context spill
  Call->Operands[2] = frame(F.Image, -256);
  Binding.Receiver.reset();
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Binding);
  EXPECT_FALSE(Safe());
}

TEST(ObjCBlockSources, PartialContextPointerInFrameRemainsPrivate) {
  SourceFixture F(true);
  auto &Function = F.Result.HighFuncs[1];
  Function.FrameSize = 16;
  const ObjCBlockSourceContext Source(F.Image);
  std::set<std::pair<va_t, size_t>> Active;
  std::string Reason;
  auto CallFrameConsumer = [&](ExprPtr Value) {
    auto Call = HighExpr::makeCall({}, 0, {frame(F.Image, -8)});
    Call->Type = Function.ReturnType;
    Function.Body = {store(frame(F.Image, -8), Value), ret(Call)};
    Reason.clear();
    return objc_block_source_detail::noEscape(
        Source, F.functions(), Function.Entry, 0, nullptr, Active, Reason);
  };
  EXPECT_TRUE(CallFrameConsumer(HighExpr::makeConst(7, 8))) << Reason;
  auto Partial =
      HighExpr::makeBinop(NdOp::SUBBYTES, parameter(0, Function.Params[0].Type),
                          HighExpr::makeConst(0, 4));
  Partial->Type = NdType::makeInt(4, false);
  auto Extended = HighExpr::makeUnary(NdOp::INT_ZEXT, Partial);
  Extended->Type = NdType::makeInt(8, false);
  EXPECT_FALSE(CallFrameConsumer(Extended));
  EXPECT_NE(Reason.find("exposes private context storage"), std::string::npos);
}

TEST(ObjCBlockSources, CompleteWideCaptureSpillRetainsConstructionEvidence) {
  SourceFixture F(true);
  F.put64(F.Descriptor + 8, 48);
  auto &Caller = F.caller();
  Caller.FrameSize = 128;
  auto Wide = NdType::makeInt(16);
  Caller.Body.insert(
      Caller.Body.begin(),
      store(frame(F.Image, -80),
            HighExpr::makeLoad(parameter(0, Caller.Params[0].Type), Wide)));
  Caller.Body[5] =
      store(frame(F.Image, -16), HighExpr::makeLoad(frame(F.Image, -80), Wide));
  const auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  ASSERT_FALSE(Plan.Rejections.count(F.Caller))
      << (Plan.Rejections.count(F.Caller) ? Plan.Rejections.at(F.Caller) : "");
  ASSERT_EQ(Plan.StackBlocks.at(F.Caller).size(), 1U);
  EXPECT_EQ(Plan.StackBlocks.at(F.Caller)[0].InitializedCaptures.size(), 16U);

  // Two exact zero words form initialized, pointer-free capture bytes.
  auto Computed = HighExpr::makeBinop(NdOp::CONCAT, HighExpr::makeConst(0, 8),
                                      HighExpr::makeConst(0, 8));
  Computed->Type = Wide;
  Caller.Body[0].StoreVal = Computed;
  const auto ZeroCapture = discoverObjCBlockSources(F.Image, F.Result);
  ASSERT_FALSE(ZeroCapture.Rejections.count(F.Caller));
  EXPECT_EQ(ZeroCapture.StackBlocks.at(F.Caller)[0].InitializedCaptures.size(),
            16U);
}

TEST(ObjCBlockSources, CompleteWideContextCaptureMayBeSpilledPrivately) {
  SourceFixture F(true);
  auto &Function = F.Result.HighFuncs[1];
  Function.FrameSize = 32;
  auto Wide = NdType::makeInt(16);
  auto Address =
      HighExpr::makeBinop(NdOp::INT_ADD, parameter(0, Function.Params[0].Type),
                          HighExpr::makeConst(32, 8));
  auto Call = HighExpr::makeCall({}, 0, {frame(F.Image, -16)});
  Call->Type = Function.ReturnType;
  Function.Body = {
      store(frame(F.Image, -16), HighExpr::makeLoad(Address, Wide)), ret(Call)};
  std::set<uint64_t> Initialized;
  for (uint64_t Byte = 32; Byte < 48; ++Byte)
    Initialized.insert(Byte);
  const ObjCBlockSourceContext Source(F.Image);
  std::set<std::pair<va_t, size_t>> Active;
  std::string Reason;
  EXPECT_TRUE(objc_block_source_detail::noEscape(
      Source, F.functions(), Function.Entry, 0, &Initialized, Active, Reason))
      << Reason;
  Initialized.erase(47);
  EXPECT_FALSE(objc_block_source_detail::noEscape(
      Source, F.functions(), Function.Entry, 0, &Initialized, Active, Reason));
  EXPECT_NE(Reason.find("uninitialized capture storage"), std::string::npos);

  auto PackedPointer =
      HighExpr::makeBinop(NdOp::CONCAT, HighExpr::makeConst(0, 8),
                          parameter(0, Function.Params[0].Type));
  PackedPointer->Type = Wide;
  Function.Body[0].StoreVal = PackedPointer;
  Initialized.insert(47);
  EXPECT_FALSE(objc_block_source_detail::noEscape(
      Source, F.functions(), Function.Entry, 0, &Initialized, Active, Reason));
  EXPECT_NE(Reason.find("exposes private context storage"), std::string::npos);
}

TEST(ObjCBlockSources,
     PooledHeaderBitsRequireImmutableUnambiguousScalarStorage) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    OwnedSourceFixture F(Arch::AArch64);
    if (Mutation == 1)
      F.Image.Sections[1].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    if (Mutation == 2)
      F.Image.Segments[1].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
    if (Mutation == 3)
      F.Image.DataPtrRelocSlots.insert(F.Flags);
    if (Mutation == 4)
      F.Image.Sections.push_back(F.Image.Sections[1]);
    if (Mutation == 5) {
      F.put64(F.Flags, F.Invoke);
      F.caller().Body[2].StoreVal = F.caller().Body[1].StoreVal;
      F.caller().Body[1].StoreVal = HighExpr::makeConst(0xc2000000, 8);
    }
    auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    EXPECT_EQ(!Plan.StackBlocks.empty(), Mutation == 0) << Mutation;
  }
}

TEST(ObjCBlockSources, ScalarLoadConversionsPreserveIndependentConstruction) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Bytes : {1U, 2U, 4U, 8U}) {
      for (NdOp Extension : {NdOp::INT_ZEXT, NdOp::INT_SEXT}) {
        for (unsigned Destination : {0U, 2U, 3U}) {
          SCOPED_TRACE(::testing::Message()
                       << static_cast<int>(Architecture) << ':' << Bytes << ':'
                       << static_cast<int>(Extension) << ':' << Destination);
          SourceFixture F(true, Architecture);
          auto Loaded = HighExpr::makeLoad(HighExpr::makeConst(0x2900, 8),
                                           NdType::makeInt(Bytes));
          auto Converted = HighExpr::makeUnary(Extension, Loaded);
          Converted->Type = NdType::makeInt(Bytes == 8 ? 4 : 8);
          if (Bytes == 8)
            Converted->Kind = ExprKind::Cast;
          if (Destination) {
            // Equal runtime bits cannot turn a numeric load into an invoke
            // or descriptor identity through an integer conversion.
            F.put64(0x2900, Destination == 2 ? F.Invoke : F.Descriptor);
            F.caller().Body[Destination].StoreVal = Converted;
          } else {
            MedVar Local;
            Local.Kind = MedVar::Temp;
            Local.Id = 42;
            Local.Size = Converted->Type->Size;
            HighStmt Prefix;
            Prefix.Kind = StmtKind::Assign;
            Prefix.Dst = HighExpr::makeVar(Local);
            Prefix.Val = Converted;
            // An unrelated scalar load before the first ISA store must not
            // prevent discovering the independently initialized literal.
            F.caller().Body.insert(F.caller().Body.begin(), Prefix);
          }
          auto Plan = discoverObjCBlockSources(F.Image, F.Result);
          EXPECT_EQ(!Plan.StackBlocks.empty(), Destination == 0)
              << Plan.Rejections[F.Caller];
        }
      }
    }
  }
}

TEST(ObjCBlockSources, DescriptorOwnershipAndFunctionRolesCannotConflict) {
  OwnedSourceFixture F(Arch::AArch64);
  auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  ASSERT_EQ(Plan.StackBlocks[F.Caller].size(), 1U);
  auto D = Plan.StackBlocks[F.Caller][0].Descriptor;
  D.CopyHelper = F.Dispose;
  EXPECT_FALSE(objc_block_source_detail::publish(Plan, D, F.Invoke));
  EXPECT_FALSE(
      bindObjCBlockSourceReferences(F.caller(), F.Image, Plan, F.functions())
          .Limitation.empty());
  OwnedSourceFixture G(Arch::X64);
  auto P = discoverObjCBlockSources(G.Image, G.Result);
  EXPECT_FALSE(objc_block_source_detail::publish(
      P, P.StackBlocks[G.Caller][0].Descriptor, G.Copy));
  EXPECT_TRUE(P.Rejections.count(G.Copy));
}

TEST(ObjCBlockSources,
     IndirectBlockInvokeRequiresCompleteBindingAndOrdinaryMemory) {
  SourceFixture F(true);
  auto P = discoverObjCBlockSources(F.Image, F.Result);
  auto E = F.Result.HighFuncs[1].Body[0].RetVal;
  E->IsIndirectCall = true;
  EXPECT_TRUE(objcBlockSourceCallBound(*E, F.Image, P, F.functions()));
  auto Forged = *E;
  auto WrongEffect = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
  WrongEffect->DoesNotReturn = true;
  Forged.SourceCallHint = WrongEffect;
  EXPECT_FALSE(objcBlockSourceCallBound(Forged, F.Image, P, F.functions()));
  E->Operands.pop_back();
  EXPECT_FALSE(objcBlockSourceCallBound(*E, F.Image, P, F.functions()));
}
TEST(ObjCBlockSources, SharedExpressionDAGCannotExhaustTheProofEvaluator) {
  SourceFixture F(true);
  auto Value = HighExpr::makeConst(1, 8);
  for (unsigned I = 0; I < 30; ++I)
    Value = HighExpr::makeBinop(NdOp::INT_ADD, Value, Value);
  const ObjCBlockSourceContext Source(F.Image);
  objc_block_source_detail::Values State(Source, F.caller());
  EXPECT_THROW(State.eval(Value), objc_block_source_detail::Invalid);
}
TEST(ObjCBlockSources, PartialSpillReadsCannotEraseContextAddressProvenance) {
  for (bool PartialOverwrite : {false, true}) {
    SourceFixture F(true);
    auto &Consumer = F.Result.HighFuncs[1];
    Consumer.FrameSize = 16;
    Consumer.Body = {
        store(frame(F.Image, -8), parameter(0, Consumer.Params[0].Type))};
    if (PartialOverwrite)
      Consumer.Body.push_back(
          store(frame(F.Image, -8), HighExpr::makeConst(0, 4)));
    Consumer.Body.push_back(ret(HighExpr::makeLoad(
        frame(F.Image, PartialOverwrite ? -4 : -8), NdType::makeInt(4))));
    auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    EXPECT_TRUE(Plan.StackBlocks.empty())
        << "partial overwrite=" << PartialOverwrite;
  }
}

TEST(ObjCBlockSources, CompleteScalarOverwriteClearsOnlyReplacedIdentityBytes) {
  SourceFixture F(true);
  auto &Consumer = F.Result.HighFuncs[1];
  Consumer.FrameSize = 16;
  Consumer.Body = {
      store(frame(F.Image, -8), parameter(0, Consumer.Params[0].Type)),
      store(frame(F.Image, -8), HighExpr::makeConst(0, 8)),
      ret(HighExpr::makeLoad(frame(F.Image, -4), NdType::makeInt(4)))};
  auto Plan = discoverObjCBlockSources(F.Image, F.Result);
  EXPECT_EQ(Plan.StackBlocks[F.Caller].size(), 1U) << Plan.Rejections[F.Caller];
}

TEST(ObjCBlockSources, NewExportObservesChangedImportIdentityAndConflicts) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SourceFixture F(true);
    ObjCBlockSourcePlan Plan;
    ExprPtr Isa;
    {
      const ObjCBlockSourceContext Source(F.Image);
      Plan = discoverObjCBlockSources(Source, F.Result);
      ASSERT_EQ(Plan.StackBlocks[F.Caller].size(), 1U);
      auto Bound = bindObjCBlockSourceReferences(F.caller(), Source, Plan,
                                                 F.functions());
      ASSERT_TRUE(Bound.Limitation.empty()) << Bound.Limitation;
      Isa = Bound.Function.Body[0].StoreVal;
      ASSERT_TRUE(objcBlockSourceCallBound(*Isa, Source, Plan, F.functions()));
    }
    if (Mutation == 0)
      F.Image.ImportPtrSlots[F.StackIsa] = "__NSConcreteGlobalBlock";
    if (Mutation == 1)
      F.Image.ImportStorageSlots[F.StackIsa] = {
          "__NSConcreteGlobalBlock", 0, ImportStorageEvidence::LoaderBind};
    if (Mutation == 2)
      F.Image.DyldBindSlots[F.StackIsa] = {"__NSConcreteStackBlock", 8};
    if (Mutation == 3)
      F.Image.ConflictingImportStorageSlots.insert(F.StackIsa);
    const ObjCBlockSourceContext Changed(F.Image);
    EXPECT_TRUE(
        discoverObjCBlockSources(Changed, F.Result).StackBlocks.empty());
    EXPECT_FALSE(objcBlockSourceCallBound(*Isa, Changed, Plan, F.functions()));
    EXPECT_TRUE(
        discoverObjCBlockSources(F.Image, F.Result).StackBlocks.empty());
    EXPECT_FALSE(objcBlockSourceCallBound(*Isa, F.Image, Plan, F.functions()));
  }
}

TEST(ObjCBlockSources, SharedImportsDoNotReusePipelineFunctionProofs) {
  SourceFixture F(true);
  const ObjCBlockSourceContext Source(F.Image);
  auto Plan = discoverObjCBlockSources(Source, F.Result);
  ASSERT_EQ(Plan.StackBlocks[F.Caller].size(), 1U);
  ASSERT_TRUE(
      bindObjCBlockSourceReferences(F.caller(), Source, Plan, F.functions())
          .Limitation.empty());

  PipelineResult Next;
  Next.SourceImage = &F.Image;
  Next.HighFuncs = F.Result.HighFuncs;
  auto &Consumer = Next.HighFuncs[1];
  Consumer.Body = {ret(parameter(0, Consumer.Params[0].Type))};
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : Next.HighFuncs)
    Functions.emplace(Function.Entry, &Function);
  EXPECT_TRUE(discoverObjCBlockSources(Source, Next).StackBlocks.empty());

  Consumer.Body = F.Result.HighFuncs[1].Body;
  auto &Invoke = Next.HighFuncs[0];
  Invoke.Body = {ret(parameter(0, Invoke.Params[0].Type))};
  auto NextPlan = discoverObjCBlockSources(Source, Next);
  ASSERT_EQ(NextPlan.StackBlocks[F.Caller].size(), 1U);
  EXPECT_FALSE(bindObjCBlockSourceReferences(Next.HighFuncs[2], Source,
                                             NextPlan, Functions)
                   .Limitation.empty());
  EXPECT_EQ(
      discoverObjCBlockSources(Source, F.Result).StackBlocks[F.Caller].size(),
      1U);
  EXPECT_TRUE(
      bindObjCBlockSourceReferences(F.caller(), Source, Plan, F.functions())
          .Limitation.empty());
}

TEST(ObjCBlockSources, ImportContextRejectsAnotherPipelineImage) {
  SourceFixture F(true), Other(true);
  const ObjCBlockSourceContext Source(F.Image);
  EXPECT_THROW(discoverObjCBlockSources(Source, Other.Result),
               std::invalid_argument);
  EXPECT_THROW(discoverObjCBlockSources(F.Image, Other.Result),
               std::invalid_argument);
  EXPECT_EQ(
      discoverObjCBlockSources(Source, F.Result).StackBlocks[F.Caller].size(),
      1U);
}
