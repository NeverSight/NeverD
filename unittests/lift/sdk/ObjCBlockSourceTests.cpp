#include "../../../lib/sdk/capi/ObjCBlockSources.h"
#include "gtest/gtest.h"

#include "llvm/BinaryFormat/MachO.h"
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
  SourceFixture(bool Stack) {
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
  std::set<std::string> Shared;
  auto Text = renderObjCBlockSourceHelpers(Plan, Bound.Descriptors, Shared);
  EXPECT_NE(Text.find("&neverd_block_invoke_1100"), std::string::npos);
  EXPECT_NE(Text.find("&storage.descriptor"), std::string::npos);
  EXPECT_EQ(Shared.size(), 3U);
  EXPECT_EQ(F.caller().Body.back().RetVal->Operands[0]->Kind, ExprKind::Const);
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
    auto Plan = discoverObjCBlockSources(F.Image, F.Result);
    EXPECT_TRUE(Plan.StackBlocks.empty()) << Mutation;
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
     IndirectBlockInvokeRequiresCompleteBindingAndOrdinaryMemory) {
  SourceFixture F(true);
  auto P = discoverObjCBlockSources(F.Image, F.Result);
  auto E = F.Result.HighFuncs[1].Body[0].RetVal;
  E->IsIndirectCall = true;
  EXPECT_TRUE(objcBlockSourceCallBound(*E, F.Image, P, F.functions()));
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
    EXPECT_TRUE(discoverObjCBlockSources(Changed, F.Result).StackBlocks.empty());
    EXPECT_FALSE(objcBlockSourceCallBound(*Isa, Changed, Plan, F.functions()));
    EXPECT_TRUE(discoverObjCBlockSources(F.Image, F.Result).StackBlocks.empty());
    EXPECT_FALSE(objcBlockSourceCallBound(*Isa, F.Image, Plan, F.functions()));
  }
}

TEST(ObjCBlockSources, SharedImportsDoNotReusePipelineFunctionProofs) {
  SourceFixture F(true);
  const ObjCBlockSourceContext Source(F.Image);
  auto Plan = discoverObjCBlockSources(Source, F.Result);
  ASSERT_EQ(Plan.StackBlocks[F.Caller].size(), 1U);
  ASSERT_TRUE(bindObjCBlockSourceReferences(F.caller(), Source, Plan,
                                           F.functions())
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
  EXPECT_FALSE(bindObjCBlockSourceReferences(Next.HighFuncs[2], Source, NextPlan,
                                            Functions)
                   .Limitation.empty());
  EXPECT_EQ(discoverObjCBlockSources(Source, F.Result).StackBlocks[F.Caller]
                .size(),
            1U);
  EXPECT_TRUE(bindObjCBlockSourceReferences(F.caller(), Source, Plan,
                                           F.functions())
                  .Limitation.empty());
}

TEST(ObjCBlockSources, ImportContextRejectsAnotherPipelineImage) {
  SourceFixture F(true), Other(true);
  const ObjCBlockSourceContext Source(F.Image);
  EXPECT_THROW(discoverObjCBlockSources(Source, Other.Result),
               std::invalid_argument);
  EXPECT_THROW(discoverObjCBlockSources(F.Image, Other.Result),
               std::invalid_argument);
  EXPECT_EQ(discoverObjCBlockSources(Source, F.Result).StackBlocks[F.Caller]
                .size(),
            1U);
}
