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

struct StringOnceFixture : OnceFixture {
  ExprPtr Bridge;
  explicit StringOnceFixture(Arch Architecture) : OnceFixture(Architecture) {
    constexpr va_t BridgeSlot = 0x2088;
    const std::string BridgeName =
        "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF";
    Image.Symbols.push_back({"_string", 0x2020, 16, false});
    Image.ImportPtrSlots[BridgeSlot] = BridgeName;
    Image.DyldBindSlots[BridgeSlot] = {
        BridgeName, 0,
        "/System/Library/Frameworks/Foundation.framework/Foundation", false};

    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Integer = NdType::makeInt(8, false);
    auto Param = [&](unsigned Id, const TypeRef &Type) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Type);
    };
    auto &Getter = Pipeline.HighFuncs[0];
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"predicate", Pointer},
                            {"string_word", Integer},
                            {"string_storage", Integer},
                            {"initializer", Pointer}};
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Signature, Architecture, Error));
    Getter.SourceTypeHint = Signature;
    Getter.Params = {{"predicate", Pointer},
                     {"string_word", Integer},
                     {"string_storage", Integer},
                     {"initializer", Pointer}};
    Once->Operands[1] = Param(3, Pointer);
    Bridge =
        HighExpr::makeCall(BridgeName, BridgeSlot,
                           {HighExpr::makeLoad(Param(1, Integer), Integer),
                            HighExpr::makeLoad(Param(2, Integer), Integer)});
    Bridge->Type = Pointer;
    const auto BridgeHint = swiftStringSourceCallHint(Image, BridgeSlot);
    EXPECT_TRUE(BridgeHint);
    Bridge->SourceCallHint = std::make_shared<SourceCallTypeHint>(*BridgeHint);
    Getter.Body[2].RetVal = Bridge;

    auto &Caller = Pipeline.HighFuncs[2];
    Caller.Body[0].RetVal = HighExpr::makeCall(
        Getter.Name, Getter.Entry,
        {HighExpr::makeConst(0x2000, 8), HighExpr::makeConst(0x2020, 8),
         HighExpr::makeConst(0x2028, 8), HighExpr::makeConst(0x1080, 8)});
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::Native;
    Hint->TargetAddress = Getter.Entry;
    Hint->Signature = Signature;
    Caller.Body[0].RetVal->SourceCallHint = std::move(Hint);
    Caller.Body[0].RetVal->Type = Pointer;
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

TEST(SwiftOnceSources, BindsContiguousSwiftStringStorageAsOneSharedObject) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    StringOnceFixture F(Architecture);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.Getters.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    const auto &Contract = Plan.Getters.begin()->second;
    EXPECT_EQ(Contract.parameterCount(), 4U);
    EXPECT_EQ(Contract.storageWidth(), 16U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies, std::set<va_t>{0x1080});
    EXPECT_EQ(Bound.LocalStorageExtents,
              (std::map<va_t, uint64_t>{{0x2000, 8}, {0x2020, 16}}));
    const auto Call = Bound.Function.Body[0].RetVal;
    ASSERT_EQ(Call->Operands.size(), 4U);
    ASSERT_EQ(Call->Operands[2]->Kind, ExprKind::BinOp);
    EXPECT_EQ(Call->Operands[2]->Op, NdOp::INT_ADD);
    ASSERT_EQ(Call->Operands[2]->Operands.size(), 2U);
    EXPECT_EQ(Call->Operands[2]->Operands[1]->ConstVal, 8U);
    EXPECT_EQ(Call->Operands[1]->SourceCallHint->TargetAddress, 0x2020U);
    EXPECT_EQ(Call->Operands[2]->Operands[0]->SourceCallHint->TargetAddress,
              0x2020U);
    EXPECT_TRUE(swiftOnceCallbackBound(*Call->Operands[3], F.Image, Plan,
                                       F.functions()));
    EXPECT_TRUE(
        bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());
  }
}

TEST(SwiftOnceSources, RejectsUnprovedSwiftStringOnceContractsAndStorage) {
  for (unsigned Case = 0; Case < 10; ++Case) {
    SCOPED_TRACE(Case);
    StringOnceFixture F(Arch::AArch64);
    auto &Getter = F.Pipeline.HighFuncs[0];
    auto &Call = F.Pipeline.HighFuncs.back().Body[0].RetVal;
    if (Case == 0)
      std::swap(F.Bridge->Operands[0], F.Bridge->Operands[1]);
    if (Case == 1)
      Call->Operands[2] = HighExpr::makeConst(0x2030, 8);
    if (Case == 2) {
      HighStmt Escape;
      Escape.Kind = StmtKind::ExprStmt;
      Escape.Val = F.Bridge->Operands[0]->Operands[0];
      Getter.Body.insert(Getter.Body.begin(), Escape);
    }
    if (Case == 3) {
      auto Hint =
          std::make_shared<SourceCallTypeHint>(*F.Bridge->SourceCallHint);
      Hint->CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
      F.Bridge->SourceCallHint = std::move(Hint);
    }
    if (Case == 4)
      F.Bridge->Operands[0]->Type = NdType::makeInt(4, false);
    if (Case == 5) {
      HighStmt Duplicate;
      Duplicate.Kind = StmtKind::ExprStmt;
      Duplicate.Val = F.Bridge;
      Getter.Body.insert(Getter.Body.end() - 1, Duplicate);
    }
    if (Case == 6)
      F.Image.Symbols.back().Size = 8;
    if (Case == 7)
      F.Image.Symbols.push_back({"_split_string", 0x2028, 8, false});
    if (Case == 8)
      Call->Operands[0] = HighExpr::makeConst(0x2028, 8);
    if (Case == 9) {
      auto Hint =
          std::make_shared<SourceCallTypeHint>(*F.Bridge->SourceCallHint);
      Hint->Signature.Parameters[1].Type = NdType::makeInt(8, false);
      F.Bridge->SourceCallHint = std::move(Hint);
    }
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.Dependencies.empty());
    EXPECT_TRUE(Bound.LocalStorageExtents.empty());
  }
}

} // namespace
