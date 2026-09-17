#include "../../../lib/sdk/capi/ObjCSwiftOnceSources.h"
#include "gtest/gtest.h"

using namespace neverd;
using namespace neverd::sdk;

namespace {
struct OnceFixture {
  BinaryImage Image;
  PipelineResult Pipeline;
  ExprPtr Once;
  explicit OnceFixture(Arch Architecture) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    for (unsigned I = 0; I < 2; ++I) {
      Segment S;
      S.VA = 0x1000 + 0x1000 * I;
      S.Size = S.FileSz = 0x100;
      S.Flags = SegmentFlags::Readable |
                (I ? SegmentFlags::Writable : SegmentFlags::Executable);
      S.Data.resize(S.Size);
      Image.Segments.push_back(S);
      Section Sec;
      Sec.VA = S.VA;
      Sec.Size = S.Size;
      Sec.Flags = S.Flags;
      Sec.Type = I ? 0 : llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      Image.Sections.push_back(Sec);
    }
    Image.Symbols.push_back({"_predicate", 0x2000, 8, false});
    Image.Symbols.push_back({"_object", 0x2010, 8, false});
    Image.ImportPtrSlots[0x2080] = "_swift_once";
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Pointer);
    };
    HighFunc Getter;
    Getter.Entry = 0x1000;
    Getter.Name = "unrelated_name";
    Getter.ReturnType = Pointer;
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Pointer;
    for (unsigned I = 0; I < 3; ++I) {
      Signature.Parameters.push_back({"arg" + std::to_string(I), Pointer});
      Getter.Params.push_back({"arg" + std::to_string(I), Pointer});
    }
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error));
    Getter.SourceTypeHint = Signature;
    const auto Runtime = swiftRuntimeSourceCallHint(Image, 0x2080);
    EXPECT_TRUE(Runtime);
    Once = HighExpr::makeCall("swift_once", 0x2080,
                              {Param(0), Param(2), Param(0)});
    Once->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
    Once->Type = NdType::makeVoid();
    HighStmt Predicate, Invoke, Return;
    Predicate.Kind = StmtKind::ExprStmt;
    Predicate.Val = HighExpr::makeLoad(Param(0), NdType::makeInt(8));
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = Once;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeLoad(Param(1), NdType::makeInt(8));
    Getter.Body = {Predicate, Invoke, Return};
    HighFunc Callback;
    Callback.Entry = 0x1080;
    Callback.Name = "initial_value";
    Callback.SourceTypeHint =
        swift_once_source_detail::callbackHint(Architecture);
    Callback.Params = {{"once_context", Pointer}};
    Callback.ReturnType = NdType::makeVoid();
    Return.RetVal.reset();
    Callback.Body = {Return};
    HighFunc Caller;
    Caller.Entry = 0x10c0;
    Caller.Name = "get_value";
    Caller.SourceTypeHint = Signature;
    Caller.SourceTypeHint->Parameters.clear();
    Caller.ReturnType = Pointer;
    Return.RetVal = HighExpr::makeCall(
        Getter.Name, Getter.Entry,
        {HighExpr::makeConst(0x2000, 8), HighExpr::makeConst(0x2010, 8),
         HighExpr::makeConst(Callback.Entry, 8)});
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = Getter.Entry;
    Hint->Signature = Signature;
    Return.RetVal->SourceCallHint = Hint;
    Return.RetVal->Type = Pointer;
    Caller.Body = {Return};
    Pipeline.SourceImage = &Image;
    Pipeline.HighFuncs = {Getter, Callback, Caller};
  }
  std::map<va_t, const HighFunc *> functions() const {
    std::map<va_t, const HighFunc *> Result;
    for (const auto &F : Pipeline.HighFuncs)
      Result.emplace(F.Entry, &F);
    return Result;
  }
};

TEST(SwiftOnceSources, BindsStorageAndCallbackAsOneDependencyGroup) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    OnceFixture F(Architecture);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Getters.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    PipelineOptions Options;
    EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 1U);
    EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 0U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
    EXPECT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{0x2000, 8}, {0x2010, 8}}));
    const auto Call = Bound.Function.Body[0].RetVal;
    EXPECT_TRUE(swiftOnceCallbackBound(*Call->Operands[2], F.Image, Plan,
                                       F.functions()));
    EXPECT_TRUE(
        bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());
    EXPECT_EQ(F.Pipeline.HighFuncs.back().Body[0].RetVal->Operands[0]->Kind,
              ExprKind::Const);
  }
}

TEST(SwiftOnceSources, RejectsUnprovedParameterUsesAndCallbackContext) {
  for (unsigned Case = 0; Case < 12; ++Case) {
    OnceFixture F(Arch::AArch64);
    auto &Getter = F.Pipeline.HighFuncs[0];
    auto &Callback = F.Pipeline.HighFuncs[1];
    if (Case == 0)
      F.Once->Operands[2] = F.Once->Operands[1];
    if (Case == 1)
      Getter.Body[2].RetVal = F.Once->Operands[0];
    if (Case == 2)
      Getter.Body[2].RetVal->Type = NdType::makeInt(4);
    if (Case == 3)
      Getter.Body[0].Val->MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Case == 4)
      F.Once->Operands[1]->Var.SSAVer = 1;
    if (Case == 5) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      Use.Val = F.Once->Operands[0];
      Callback.Body.insert(Callback.Body.begin(), Use);
    }
    if (Case == 6)
      F.Image.Segments[1].Data[0] = 1;
    if (Case == 7)
      F.Image.Symbols.clear();
    if (Case == 8)
      F.Once->IntrinsicOutputs.push_back({});
    if (Case == 9)
      Callback.SourceTypeHint->ReturnType = NdType::makeInt(8);
    if (Case == 10)
      F.Pipeline.HighFuncs.back().Body[0].RetVal->SourceCallHint =
          std::make_shared<SourceCallTypeHint>();
    if (Case == 11)
      F.Pipeline.SourceImage = nullptr;
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs.back(),
                                               F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.Dependencies.empty()) << Case;
    EXPECT_TRUE(Bound.LocalStorageExtents.empty()) << Case;
  }
}
TEST(SwiftOnceSources, RejectsForgedCallbackEffectsAndKeepsExistingABI) {
  OnceFixture F(Arch::AArch64);
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  const auto Bound = bindSwiftOnceSourceReferences(
      F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
  ASSERT_EQ(Bound.Dependencies.size(), 1U);
  const auto Callback = Bound.Function.Body[0].RetVal->Operands[2];
  for (unsigned Case = 0; Case < 3; ++Case) {
    auto Forged = *Callback;
    if (Case == 0)
      Forged.IntrinsicOutputs.push_back({});
    if (Case == 1)
      Forged.CallAddr = 0x1080;
    if (Case == 2)
      Forged.CallTarget = "unexpected";
    EXPECT_FALSE(swiftOnceCallbackBound(Forged, F.Image, Plan, F.functions()));
  }
  PipelineOptions Options;
  auto Existing = *F.Pipeline.HighFuncs[1].SourceTypeHint;
  Existing.ReturnType = NdType::makeInt(8);
  Options.SourceTypeHints.emplace(0x1080, Existing);
  EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 0U);
  EXPECT_EQ(Options.SourceTypeHints.at(0x1080).ReturnType->Kind,
            NdTypeKind::Int);
}

TEST(SwiftOnceSources, OrdinaryDirectCallerPreventsCallbackABIOverride) {
  OnceFixture F(Arch::AArch64);
  LowFunc DirectCaller;
  DirectCaller.Entry = 0x1040;
  DirectCaller.Blocks.emplace_back();
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(0x1080, 8));
  DirectCaller.Blocks[0].Ops.push_back(Call);
  F.Pipeline.LowFuncs.push_back(DirectCaller);
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  EXPECT_EQ(Plan.Getters.size(), 1U);
  EXPECT_TRUE(Plan.CallbackHints.empty());
  const auto Bound = bindSwiftOnceSourceReferences(
      F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
  EXPECT_TRUE(Bound.Dependencies.empty());
  EXPECT_TRUE(Bound.LocalStorageExtents.empty());
}

} // namespace
