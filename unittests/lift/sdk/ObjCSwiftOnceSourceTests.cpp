#include "../../../lib/sdk/capi/ObjCSwiftOnceSources.h"
#include "gtest/gtest.h"

#include "neverd/loader/ObjC/ObjCEncoding.h"

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

struct AddressorFixture {
  static constexpr va_t AccessorAddress = 0x1000;
  static constexpr va_t InitializerAddress = 0x1080;
  static constexpr va_t CallerAddress = 0x10c0;
  static constexpr va_t PredicateAddress = 0x2000;
  static constexpr va_t StorageAddress = 0x2010;
  static constexpr va_t RuntimeSlot = 0x2080;
  static constexpr const char *AccessorName = "_$s4Test5valueSo8NSObjectCvau";
  static constexpr const char *InitializerName = "_$s4Test5value_WZ";
  static constexpr const char *PredicateName = "_$s4Test5value_Wz";
  static constexpr const char *StorageName = "_$s4Test5valueSo8NSObjectCvpZ";

  BinaryImage Image;
  PipelineResult Pipeline;
  ExprPtr Once;

  explicit AddressorFixture(Arch Architecture) {
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
    Image.Symbols = {{AccessorName, AccessorAddress, 0, true},
                     {InitializerName, InitializerAddress, 0, true},
                     {PredicateName, PredicateAddress, 8, false},
                     {StorageName, StorageAddress, 8, false}};
    Image.ImportPtrSlots[RuntimeSlot] = "_swift_once";

    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Integer = NdType::makeInt(8);
    auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Pointer);
    };
    SourceFunctionTypeHint NativeSignature;
    NativeSignature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    NativeSignature.ReturnType = Pointer;
    for (unsigned I = 0; I < 3; ++I)
      NativeSignature.Parameters.push_back(
          {"arg" + std::to_string(I), Pointer});
    std::string Error;
    EXPECT_TRUE(
        assignDarwinScalarSourceABI(NativeSignature, Architecture, Error));

    HighFunc Accessor;
    Accessor.Entry = AccessorAddress;
    Accessor.Name = AccessorName;
    Accessor.ReturnType = Pointer;
    for (unsigned I = 0; I < 3; ++I)
      Accessor.Params.push_back({"arg" + std::to_string(I), Pointer});
    MedVar LoadedVar;
    LoadedVar.Kind = MedVar::Temp;
    LoadedVar.Id = 1;
    LoadedVar.Size = 8;
    auto Loaded = HighExpr::makeVar(LoadedVar, Integer);
    HighStmt Load;
    Load.Kind = StmtKind::Assign;
    Load.Dst = Loaded;
    Load.Val = HighExpr::makeLoad(
        HighExpr::makeConst(PredicateAddress, 8,
                            ConstantAddressProvenance::DataAddress),
        Integer);
    const auto Runtime = swiftRuntimeSourceCallHint(Image, RuntimeSlot);
    EXPECT_TRUE(Runtime);
    Once = HighExpr::makeCall(
        "swift_once", RuntimeSlot,
        {HighExpr::makeConst(PredicateAddress, 8,
                             ConstantAddressProvenance::DataAddress),
         HighExpr::makeConst(InitializerAddress, 8,
                             ConstantAddressProvenance::CodeAddress),
         Param(2)});
    Once->Type = NdType::makeVoid();
    Once->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
    HighStmt Invoke;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = Once;
    HighStmt FirstReturn;
    FirstReturn.Kind = StmtKind::Return;
    FirstReturn.RetVal = HighExpr::makeConst(
        StorageAddress, 8, ConstantAddressProvenance::DataAddress);
    HighStmt Initialize;
    Initialize.Kind = StmtKind::If;
    Initialize.Cond = HighExpr::makeBinop(
        NdOp::INT_NOTEQUAL,
        HighExpr::makeBinop(NdOp::INT_ADD,
                            HighExpr::makeVar(LoadedVar, Integer),
                            HighExpr::makeConst(1, 8)),
        HighExpr::makeConst(0, 8));
    Initialize.Body = {Invoke, FirstReturn};
    HighStmt SecondReturn = FirstReturn;
    HighStmt ContinuationLabel;
    ContinuationLabel.Kind = StmtKind::Block;
    ContinuationLabel.Addr = AccessorAddress + 0x3c;
    Accessor.Body = {Load, Initialize, ContinuationLabel, SecondReturn};

    HighFunc Initializer;
    Initializer.Entry = InitializerAddress;
    Initializer.Name = InitializerName;
    Initializer.ReturnType = NdType::makeVoid();
    Initializer.SourceTypeHint =
        swift_once_source_detail::callbackHint(Architecture);
    Initializer.Params = {{"once_context", Pointer}};
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Initializer.Body = {Return};

    HighFunc Caller;
    Caller.Entry = CallerAddress;
    Caller.Name = "read_value";
    Caller.ReturnType = Pointer;
    Caller.SourceTypeHint = NativeSignature;
    Caller.SourceTypeHint->Parameters.clear();
    Return.RetVal = HighExpr::makeCall(AccessorName, AccessorAddress,
                                       std::vector<ExprPtr>{});
    Return.RetVal->Type = Pointer;
    auto AddressorCall = std::make_shared<SourceCallTypeHint>();
    AddressorCall->CallKind = SourceCallTypeHint::Kind::Native;
    AddressorCall->TargetAddress = AccessorAddress;
    AddressorCall->Signature =
        swift_once_source_detail::addressorHint(Architecture);
    Return.RetVal->SourceCallHint = std::move(AddressorCall);
    Caller.Body = {Return};
    Pipeline.SourceImage = &Image;
    Pipeline.HighFuncs = {Accessor, Initializer, Caller};
  }

  std::map<va_t, const HighFunc *> functions() const {
    std::map<va_t, const HighFunc *> Result;
    for (const auto &F : Pipeline.HighFuncs)
      Result.emplace(F.Entry, &F);
    return Result;
  }
};

struct ObjCThunkFixture : AddressorFixture {
  static constexpr va_t ThunkAddress = 0x10e0;
  static constexpr va_t RetainSlot = 0x2088;
  static constexpr const char *ThunkName = "_$s4Test5valueSo8NSObjectCvgZTo";

  explicit ObjCThunkFixture(Arch Architecture)
      : AddressorFixture(Architecture) {
    Image.Symbols.push_back({ThunkName, ThunkAddress, 0, true});
    Image.ImportPtrSlots[RetainSlot] = "_objc_retainAutoreleaseReturnValue";
    Image.DyldBindSlots[RetainSlot] = {"_objc_retainAutoreleaseReturnValue", 0,
                                       "/usr/lib/libobjc.A.dylib", false};
    ObjCMethod Method;
    Method.Status = "supported";
    Method.Implementation = ThunkAddress;
    Method.Selector = "value";
    Method.TypeEncoding = "@16@0:8";
    Method.TypeHint =
        parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    std::string Error;
    EXPECT_TRUE(Method.TypeHint);
    if (!Method.TypeHint)
      return;
    EXPECT_TRUE(
        assignDarwinObjCSourceABI(*Method.TypeHint, Architecture, Error))
        << Error;
    Image.ObjCMethods.push_back(Method);

    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Integer = NdType::makeInt(8);
    auto Param = [&](unsigned Id) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.Id = Id;
      V.Size = 8;
      return HighExpr::makeVar(V, Integer);
    };
    auto Local = [&](unsigned Id, const TypeRef &Type) {
      MedVar V;
      V.Kind = MedVar::Temp;
      V.Id = Id;
      V.Size = Type->Size;
      return HighExpr::makeVar(V, Type);
    };
    HighFunc Thunk;
    Thunk.Entry = ThunkAddress;
    Thunk.Name = ThunkName;
    Thunk.ReturnType = Integer;
    Thunk.Params = {{"arg0", Integer}, {"arg1", Integer}, {"arg2", Integer}};
    auto PredicateValue = Local(1, Integer);
    HighStmt LoadPredicate;
    LoadPredicate.Kind = StmtKind::Assign;
    LoadPredicate.Dst = PredicateValue;
    LoadPredicate.Val = HighExpr::makeLoad(
        HighExpr::makeConst(PredicateAddress, 8,
                            ConstantAddressProvenance::DataAddress),
        Integer);
    const auto Runtime = swiftRuntimeSourceCallHint(Image, RuntimeSlot);
    EXPECT_TRUE(Runtime);
    if (!Runtime)
      return;
    auto OnceCall = HighExpr::makeCall(
        "swift_once", RuntimeSlot,
        {HighExpr::makeConst(PredicateAddress, 8,
                             ConstantAddressProvenance::DataAddress),
         HighExpr::makeConst(InitializerAddress, 8,
                             ConstantAddressProvenance::CodeAddress),
         Param(2)});
    OnceCall->Type = NdType::makeVoid();
    OnceCall->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
    HighStmt Invoke;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = OnceCall;
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = ThunkAddress + 0x3c;
    HighStmt Initialize;
    Initialize.Kind = StmtKind::If;
    Initialize.Cond =
        HighExpr::makeBinop(NdOp::INT_NOTEQUAL,
                            HighExpr::makeBinop(NdOp::INT_ADD, PredicateValue,
                                                HighExpr::makeConst(1, 8)),
                            HighExpr::makeConst(0, 8));
    Initialize.Body = {Invoke, Jump};
    HighStmt Label;
    Label.Kind = StmtKind::Block;
    Label.Addr = Jump.GotoTarget;
    auto StorageValue = Local(2, Integer);
    HighStmt LoadStorage;
    LoadStorage.Kind = StmtKind::Assign;
    LoadStorage.Dst = StorageValue;
    LoadStorage.Val = HighExpr::makeLoad(
        HighExpr::makeConst(StorageAddress, 8,
                            ConstantAddressProvenance::DataAddress),
        Integer);
    const auto Retain = objcRuntimeSourceCallHint(Image, RetainSlot);
    EXPECT_TRUE(Retain);
    if (!Retain)
      return;
    auto ResultValue = Local(3, Pointer);
    HighStmt RetainResult;
    RetainResult.Kind = StmtKind::Assign;
    RetainResult.Dst = ResultValue;
    RetainResult.Val =
        HighExpr::makeCall(Retain->TargetName, RetainSlot, {StorageValue});
    RetainResult.Val->Type = Pointer;
    RetainResult.Val->SourceCallHint =
        std::make_shared<SourceCallTypeHint>(*Retain);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = ResultValue;
    Thunk.Body = {LoadPredicate, Initialize,   Label,
                  LoadStorage,   RetainResult, Return};
    Pipeline.HighFuncs[0] = std::move(Thunk);
  }
};

struct ObjCConstructorFixture : ObjCThunkFixture {
  explicit ObjCConstructorFixture(Arch Architecture)
      : ObjCThunkFixture(Architecture) {
    auto &Constructor = Pipeline.HighFuncs[0];
    Constructor.Name = "_$s4Test6ObjectCACycfcTo";
    Image.Symbols.back().Name = Constructor.Name;
    Image.ObjCMethods[0].Selector = "init";
    MedVar Self;
    Self.Kind = MedVar::Param;
    Self.Id = 0;
    Self.Size = 8;
    // A constructor uses its declared receiver and publishes the initialized
    // object. These effects must survive removal of the unused context.
    HighStmt Publish;
    Publish.Kind = StmtKind::Store;
    Publish.StoreAddr = HighExpr::makeBinop(
        NdOp::INT_ADD, HighExpr::makeVar(Self, NdType::makeInt(8)),
        HighExpr::makeConst(8, 8));
    Publish.StoreVal = Constructor.Body[3].Dst;
    Constructor.Body.insert(Constructor.Body.begin() + 4, Publish);
  }
};

struct NestedCallbackFixture : OnceFixture {
  static constexpr va_t NestedPredicate = 0x2020;
  static constexpr va_t LeafAddress = 0x10a0;
  ExprPtr NestedOnce;
  NestedCallbackFixture() : OnceFixture(Arch::AArch64) {
    Image.Symbols.push_back({"_$s4Test5outer_WZ", 0x1080, 0, true});
    Image.Symbols.push_back({"_$s4Test4leaf_Wz", NestedPredicate, 8, false});
    Image.Symbols.push_back({"_$s4Test4leaf_WZ", LeafAddress, 0, true});
    auto &Outer = Pipeline.HighFuncs[1];
    Outer.Name = "_$s4Test5outer_WZ";
    Outer.SourceTypeHint.reset();
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    Outer.Params = {{"arg0", Pointer}, {"arg1", Pointer}, {"arg2", Pointer}};
    MedVar Context;
    Context.Kind = MedVar::Param;
    Context.Id = 2;
    Context.Size = 8;
    NestedOnce = HighExpr::makeCall(
        "swift_once", 0x2080,
        {HighExpr::makeConst(NestedPredicate, 8,
                             ConstantAddressProvenance::DataAddress),
         HighExpr::makeConst(LeafAddress, 8,
                             ConstantAddressProvenance::CodeAddress),
         HighExpr::makeVar(Context, Pointer)});
    NestedOnce->Type = NdType::makeVoid();
    NestedOnce->SourceCallHint = std::make_shared<SourceCallTypeHint>(
        *swiftRuntimeSourceCallHint(Image, 0x2080));
    HighStmt Invoke, Publish;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = NestedOnce;
    Publish.Kind = StmtKind::Store;
    Publish.StoreAddr =
        HighExpr::makeConst(0x2010, 8, ConstantAddressProvenance::DataAddress);
    Publish.StoreVal = HighExpr::makeConst(7, 8);
    Outer.Body.insert(Outer.Body.begin(), {Invoke, Publish});
    HighFunc Leaf;
    Leaf.Entry = LeafAddress;
    Leaf.Name = "_$s4Test4leaf_WZ";
    Leaf.SourceTypeHint = swift_once_source_detail::callbackHint(Image.Arch);
    Leaf.Params = {{"once_context", Pointer}};
    Leaf.ReturnType = NdType::makeVoid();
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Leaf.Body = {Return};
    Pipeline.HighFuncs.push_back(std::move(Leaf));
  }
};

TEST(SwiftOnceSources,
     ProjectsSingleNestedCallbackWithoutDroppingOtherEffects) {
  NestedCallbackFixture F;
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.NestedCallbacks.size(), 1U);
  ASSERT_EQ(Plan.CallbackHints.size(), 2U);
  const auto &Original = F.Pipeline.HighFuncs[1];
  const auto Projected =
      projectSwiftOnceNestedCallback(Original, F.Image, Plan, F.functions());
  ASSERT_TRUE(Projected);
  ASSERT_TRUE(Projected->Function.SourceTypeHint);
  EXPECT_EQ(Projected->Function.Params.size(), 1U);
  ASSERT_EQ(Projected->Function.Body.size(), Original.Body.size());
  EXPECT_EQ(Projected->Function.Body[1].Kind, StmtKind::Store);
  EXPECT_EQ(Projected->Function.Body[1].StoreVal->ConstVal, 7U);
  EXPECT_EQ(Projected->Dependencies, std::set<va_t>{F.LeafAddress});
  EXPECT_EQ(Projected->LocalStorageExtents,
            (std::map<va_t, uint64_t>{{F.NestedPredicate, 8}}));
  const auto &Call = Projected->Function.Body[0].CallExpr;
  ASSERT_TRUE(Call);
  EXPECT_EQ(Call->Operands[2]->Kind, ExprKind::Const);
  EXPECT_EQ(Call->Operands[2]->ConstVal, 0U);
  EXPECT_TRUE(
      swiftOnceCallbackBound(*Call->Operands[1], F.Image, Plan, F.functions()));
  EXPECT_EQ(F.NestedOnce->Operands[2]->Kind, ExprKind::Var);
  EXPECT_FALSE(Original.SourceTypeHint);
  auto Functions = F.functions();
  Functions[Original.Entry] = &Projected->Function;
  const auto Caller = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[2],
                                                    F.Image, Plan, Functions);
  EXPECT_EQ(Caller.Dependencies, std::set<va_t>{Original.Entry});
}

TEST(SwiftOnceSources, NestedCallbacksRequireExactIndependentLeafEvidence) {
  for (unsigned Case = 0; Case != 9; ++Case) {
    SCOPED_TRACE(Case);
    NestedCallbackFixture F;
    auto &Outer = F.Pipeline.HighFuncs[1];
    auto &Leaf = F.Pipeline.HighFuncs.back();
    if (Case == 0)
      F.NestedOnce->Operands[2]->Var.Id = 0;
    else if (Case == 1) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      Use.Val = F.NestedOnce->Operands[2];
      Outer.Body.insert(Outer.Body.begin(), Use);
    } else if (Case == 2)
      Outer.Body.insert(Outer.Body.begin(), Outer.Body.front());
    else if (Case == 3)
      F.Image.Symbols.back().Name = "_$s4Test5other_WZ";
    else if (Case == 4)
      F.Image.Symbols[F.Image.Symbols.size() - 3].Name = "ordinary_callback";
    else if (Case == 5) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      Use.Val = F.NestedOnce->Operands[2];
      Leaf.Body.insert(Leaf.Body.begin(), Use);
    } else if (Case == 6 || Case == 7) {
      LowFunc Direct;
      LowBlock Block;
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(Case == 6 ? Outer.Entry : Leaf.Entry, 8));
      Block.Ops.push_back(Call);
      Direct.Blocks.push_back(std::move(Block));
      F.Pipeline.LowFuncs.push_back(std::move(Direct));
    } else {
      auto Changed =
          std::make_shared<SourceCallTypeHint>(*F.NestedOnce->SourceCallHint);
      Changed->TargetName = "other_once";
      F.NestedOnce->SourceCallHint = std::move(Changed);
    }
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    EXPECT_TRUE(Plan.NestedCallbacks.empty());
  }
  NestedCallbackFixture F;
  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  F.Pipeline.HighFuncs[1].ReturnType = NdType::makeInt(8);
  F.Pipeline.HighFuncs[1].Body.back().RetVal = HighExpr::makeConst(7, 8);
  const auto Discarded = projectSwiftOnceNestedCallback(
      F.Pipeline.HighFuncs[1], F.Image, Plan, F.functions());
  ASSERT_TRUE(Discarded);
  EXPECT_FALSE(Discarded->Function.Body.back().RetVal);
  EXPECT_EQ(Discarded->Function.ReturnType->Kind, NdTypeKind::Void);
  F.Pipeline.HighFuncs[1].Body.back().RetVal =
      HighExpr::makeLoad(HighExpr::makeConst(0x2010, 8), NdType::makeInt(8));
  EXPECT_FALSE(projectSwiftOnceNestedCallback(F.Pipeline.HighFuncs[1], F.Image,
                                              Plan, F.functions()));
  MedVar StackReturn;
  StackReturn.Kind = MedVar::Stack;
  StackReturn.Id = 1;
  StackReturn.Size = 8;
  StackReturn.StackOff = -8;
  F.Pipeline.HighFuncs[1].Body.back().RetVal =
      HighExpr::makeVar(StackReturn, NdType::makeInt(8));
  EXPECT_FALSE(projectSwiftOnceNestedCallback(F.Pipeline.HighFuncs[1], F.Image,
                                              Plan, F.functions()));
  F.Pipeline.HighFuncs[1].Body.back().RetVal.reset();
  F.Pipeline.HighFuncs[1].ReturnType = NdType::makeVoid();
  F.Pipeline.HighFuncs.back().SourceTypeHint.reset();
  EXPECT_FALSE(projectSwiftOnceNestedCallback(F.Pipeline.HighFuncs[1], F.Image,
                                              Plan, F.functions()));
}

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

TEST(SwiftOnceSources, BindsCanonicalZeroArgumentAddressor) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    AddressorFixture F(Architecture);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    EXPECT_TRUE(Plan.Getters.empty());
    ASSERT_EQ(Plan.Addressors.size(), 1U);
    ASSERT_EQ(Plan.AddressorHints.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    PipelineOptions Options;
    EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 2U);
    EXPECT_EQ(
        Options.SourceCalleeTypeHints.count(AddressorFixture::AccessorAddress),
        1U);
    EXPECT_EQ(
        Options.SourceTypeHints.count(AddressorFixture::InitializerAddress),
        1U);
    const auto Bound = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies,
              std::set<va_t>{AddressorFixture::InitializerAddress});
    EXPECT_EQ(
        Bound.LocalStorageExtents,
        (std::map<va_t, uint64_t>{{AddressorFixture::PredicateAddress, 8},
                                  {AddressorFixture::StorageAddress, 8}}));
    EXPECT_EQ(Bound.SwiftOnceAccessors,
              std::set<va_t>{AddressorFixture::AccessorAddress});
    const auto Call = Bound.Function.Body[0].RetVal;
    ASSERT_TRUE(Call->SourceCallHint);
    EXPECT_EQ(Call->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeSwiftOnceAccessor);
    EXPECT_TRUE(swiftOnceAddressorBound(*Call, F.Image, Plan, F.functions()));
    auto RawCaller = F.Pipeline.HighFuncs.back();
    RawCaller.Body[0].RetVal =
        std::make_shared<HighExpr>(*RawCaller.Body[0].RetVal);
    RawCaller.Body[0].RetVal->SourceCallHint.reset();
    const auto RawBound =
        bindSwiftOnceSourceReferences(RawCaller, F.Image, Plan, F.functions());
    ASSERT_TRUE(RawBound.Function.Body[0].RetVal->SourceCallHint);
    EXPECT_EQ(RawBound.Function.Body[0].RetVal->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeSwiftOnceAccessor);
    auto WrongCarrier = *Call;
    WrongCarrier.Type = NdType::makeInt(8);
    EXPECT_FALSE(
        swiftOnceAddressorBound(WrongCarrier, F.Image, Plan, F.functions()));
    auto ForgedMetadata = *Call;
    auto ForgedHint =
        std::make_shared<SourceCallTypeHint>(*ForgedMetadata.SourceCallHint);
    ForgedHint->SwiftTypeMetadata =
        SourceCallTypeHint::SwiftTypeMetadataAddress{};
    ForgedMetadata.SourceCallHint = std::move(ForgedHint);
    EXPECT_FALSE(
        swiftOnceAddressorBound(ForgedMetadata, F.Image, Plan, F.functions()));

    const auto Initializer = bindSwiftOnceSourceReferences(
        F.Pipeline.HighFuncs[1], F.Image, Plan, F.functions());
    EXPECT_EQ(Initializer.Function.Name,
              swiftOnceInitializerName(AddressorFixture::InitializerAddress));
    std::set<std::string> Shared;
    const auto Helpers = renderSwiftOnceAddressorHelpers(
        F.Image, Bound.SwiftOnceAccessors, Plan, F.functions(), Shared);
    EXPECT_NE(Helpers.find("neverd_swift_once_accessor_1000"),
              std::string::npos);
    EXPECT_NE(Helpers.find("neverd_local_storage_2000_address"),
              std::string::npos);
    EXPECT_NE(Helpers.find("neverd_local_storage_2010_address"),
              std::string::npos);
    EXPECT_NE(Helpers.find("neverd_swift_once_initializer_1080"),
              std::string::npos);
    EXPECT_EQ(Shared, std::set<std::string>{"neverd_swift_once_accessor_1000"});
  }
}

TEST(SwiftOnceSources, ProjectsCanonicalObjCLazyStaticGetterThunk) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    ObjCThunkFixture F(Architecture);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.ObjCThunks.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    EXPECT_TRUE(Plan.ObjCThunks.count(ObjCThunkFixture::ThunkAddress));
    PipelineOptions Options;
    EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 1U);
    EXPECT_TRUE(
        Options.SourceTypeHints.count(AddressorFixture::InitializerAddress));

    auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[0], F.Image,
                                               Plan, F.functions());
    EXPECT_EQ(Bound.Dependencies,
              std::set<va_t>{AddressorFixture::InitializerAddress});
    EXPECT_EQ(
        Bound.LocalStorageExtents,
        (std::map<va_t, uint64_t>{{AddressorFixture::PredicateAddress, 8},
                                  {AddressorFixture::StorageAddress, 8}}));
    EXPECT_EQ(Bound.SwiftOnceObjCThunks,
              std::set<va_t>{ObjCThunkFixture::ThunkAddress});
    ASSERT_TRUE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan));
    ASSERT_TRUE(Bound.Function.SourceTypeHint);
    EXPECT_EQ(Bound.Function.Params.size(), 2U);
    const auto &Once = *Bound.Function.Body[1].Body[0].CallExpr;
    ASSERT_EQ(Once.Operands.size(), 3U);
    EXPECT_EQ(Once.Operands[2]->Kind, ExprKind::Const);
    EXPECT_EQ(Once.Operands[2]->ConstVal, 0U);
    EXPECT_EQ(Once.Operands[2]->Type->Kind, NdTypeKind::Ptr);
    const auto Functions = F.functions();
    const auto Projection =
        bindObjCSourceReferences(Bound.Function, F.Image, nullptr, &Functions);
    EXPECT_TRUE(Projection.Limitation.empty()) << Projection.Limitation;
  }
}

TEST(SwiftOnceSources, RejectsObjCLazyStaticGetterEvidenceDrift) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    ObjCThunkFixture F(Arch::AArch64);
    auto &Thunk = F.Pipeline.HighFuncs[0];
    if (Mutation == 0)
      Thunk.Name = "forged";
    if (Mutation == 1)
      F.Image.Symbols.back().Name = "forged";
    if (Mutation == 2)
      Thunk.Body[1].Body[0].CallExpr->Operands[2] = HighExpr::makeConst(0, 8);
    if (Mutation == 3) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Id = 2;
      Context.Size = 8;
      Use.Val = HighExpr::makeVar(Context, NdType::makeInt(8));
      Thunk.Body.insert(Thunk.Body.begin() + 3, std::move(Use));
    }
    EXPECT_TRUE(
        discoverSwiftOnceSources(F.Image, F.Pipeline).ObjCThunks.empty())
        << Mutation;
  }
}

TEST(SwiftOnceSources, ProjectsConstructorOnceContextAndPreservesEffects) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    ObjCConstructorFixture F(Architecture);
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    ASSERT_EQ(Plan.ObjCThunks.size(), 1U);
    ASSERT_EQ(Plan.CallbackHints.size(), 1U);
    auto Bound = bindSwiftOnceSourceReferences(F.Pipeline.HighFuncs[0], F.Image,
                                               Plan, F.functions());
    ASSERT_EQ(Bound.SwiftOnceObjCThunks.size(), 1U);
    EXPECT_EQ(Bound.Dependencies,
              std::set<va_t>{AddressorFixture::InitializerAddress});
    EXPECT_EQ(
        Bound.LocalStorageExtents,
        (std::map<va_t, uint64_t>{{AddressorFixture::PredicateAddress, 8}}));
    ASSERT_TRUE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan));
    ASSERT_EQ(Bound.Function.Params.size(), 2U);
    ASSERT_EQ(Bound.Function.Body.size(), 7U);
    const auto &Once = *Bound.Function.Body[1].Body[0].CallExpr;
    ASSERT_EQ(Once.Operands[2]->Kind, ExprKind::Const);
    EXPECT_EQ(Once.Operands[2]->ConstVal, 0U);
    const auto &Publish = Bound.Function.Body[4];
    EXPECT_EQ(Publish.Kind, StmtKind::Store);
    EXPECT_EQ(Publish.StoreAddr->Operands[0]->Var.Id, 0);
    EXPECT_EQ(Publish.StoreAddr->Operands[1]->ConstVal, 8U);
    EXPECT_EQ(Publish.StoreVal->Var,
              F.Pipeline.HighFuncs[0].Body[4].StoreVal->Var);
    EXPECT_EQ(Bound.Function.Body[5].Val->SourceCallHint->TargetName,
              "objc_retainAutoreleaseReturnValue");
    EXPECT_EQ(Bound.Function.Body[6].RetVal->Var,
              F.Pipeline.HighFuncs[0].Body[6].RetVal->Var);
    // Projection works on copies; it cannot erase context in the machine body.
    EXPECT_EQ(
        F.Pipeline.HighFuncs[0].Body[1].Body[0].CallExpr->Operands[2]->Var.Id,
        2);
  }
}

TEST(SwiftOnceSources, RejectsConstructorOnceContextEvidenceDrift) {
  for (unsigned Mutation = 0; Mutation < 13; ++Mutation) {
    ObjCConstructorFixture F(Arch::AArch64);
    auto &Constructor = F.Pipeline.HighFuncs[0];
    auto &Once = Constructor.Body[1].Body[0].CallExpr;
    if (Mutation == 0)
      F.Image.ObjCMethods[0].Selector = "value";
    if (Mutation == 1)
      Constructor.Name = "unrelated";
    if (Mutation == 2)
      Constructor.Body[4].StoreVal = Once->Operands[2];
    if (Mutation == 3)
      Constructor.Body[1].Cond = Once->Operands[2];
    if (Mutation == 4)
      Once->Operands[2] = HighExpr::makeConst(0, 8);
    if (Mutation == 5)
      Constructor.ReturnType = NdType::makeFloat(8);
    if (Mutation == 6)
      for (auto &Symbol : F.Image.Symbols)
        if (Symbol.Addr == AddressorFixture::InitializerAddress)
          Symbol.Name = "wrong_initializer";
    if (Mutation == 7) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Once->SourceCallHint);
      Hint->WeakImport = true;
      Once->SourceCallHint = std::move(Hint);
    }
    if (Mutation == 8)
      F.Image.Symbols.push_back(F.Image.Symbols.back());
    if (Mutation == 9)
      Constructor.Body.push_back(Constructor.Body[1].Body[0]);
    if (Mutation == 10) {
      HighStmt Read;
      Read.Kind = StmtKind::ExprStmt;
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Id = 0;
      Context.Size = 8;
      Read.Val =
          HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
      F.Pipeline.HighFuncs[1].Body.insert(F.Pipeline.HighFuncs[1].Body.begin(),
                                          Read);
    }
    if (Mutation == 11) {
      LowFunc DirectCaller;
      DirectCaller.Blocks.emplace_back();
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(AddressorFixture::InitializerAddress, 8));
      DirectCaller.Blocks[0].Ops.push_back(Call);
      F.Pipeline.LowFuncs.push_back(DirectCaller);
    }
    if (Mutation == 12)
      F.Image.ObjCMethods[0].IsClassMethod = true;
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    auto Bound = bindSwiftOnceSourceReferences(Constructor, F.Image, Plan,
                                               F.functions());
    EXPECT_TRUE(Bound.SwiftOnceObjCThunks.empty()) << Mutation;
    EXPECT_FALSE(finalizeSwiftOnceObjCThunkProjection(Bound.Function, Plan))
        << Mutation;
  }
}

TEST(SwiftOnceSources, RebuildsCanonicalObjCClassMetadataAccessorCall) {
  AddressorFixture F(Arch::AArch64);
  constexpr va_t Accessor = 0x1040;
  constexpr va_t CallerAddress = 0x10f0;
  constexpr va_t Cache = 0x2040;
  constexpr va_t ClassReference = 0x2030;
  constexpr va_t ObjCSelf = 0x2088;
  constexpr va_t MetadataRuntime = 0x2090;
  const std::string AccessorName = "_$sSo7UIColorCMa";
  F.Image.Symbols.push_back({AccessorName, Accessor, 0, true});
  F.Image.Symbols.push_back({"_$sSo7UIColorCML", Cache, 8, false});
  ObjCSourceReference Reference;
  Reference.Address = ClassReference;
  Reference.Size = 8;
  Reference.Name = "UIColor";
  Reference.TheKind = ObjCSourceReference::Kind::Class;
  F.Image.ObjCSourceReferences[ClassReference] = Reference;
  F.Image.ImportPtrSlots[ObjCSelf] = "_objc_opt_self";
  F.Image.DyldBindSlots[ObjCSelf] = {"_objc_opt_self", 0,
                                     "/usr/lib/libobjc.A.dylib", false};
  F.Image.ImportPtrSlots[MetadataRuntime] = "_swift_getObjCClassMetadata";
  F.Image.DyldBindSlots[MetadataRuntime] = {"_swift_getObjCClassMetadata", 0,
                                            "/usr/lib/swift/libswiftCore.dylib",
                                            false};

  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Integer = NdType::makeInt(8);
  auto Variable = [&](unsigned Id, const TypeRef &Type) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = Id;
    V.Size = 8;
    return HighExpr::makeVar(V, Type);
  };
  auto CacheValue = Variable(1, Integer);
  auto ClassValue = Variable(2, Integer);
  auto SelfValue = Variable(3, Pointer);
  auto MetadataValue = Variable(4, Pointer);

  HighFunc MetadataAccessor;
  MetadataAccessor.Entry = Accessor;
  MetadataAccessor.Name = AccessorName;
  MetadataAccessor.ReturnType = Pointer;
  SourceFunctionTypeHint AccessorSignature;
  AccessorSignature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  AccessorSignature.ReturnType = Pointer;
  AccessorSignature.Parameters = {{"request", Integer}};
  std::string Error;
  ASSERT_TRUE(
      assignDarwinScalarSourceABI(AccessorSignature, Arch::AArch64, Error));
  MetadataAccessor.SourceTypeHint = AccessorSignature;
  MetadataAccessor.Params = {{"request", Integer}};
  HighStmt LoadCache;
  LoadCache.Kind = StmtKind::Assign;
  LoadCache.Dst = CacheValue;
  LoadCache.Val = HighExpr::makeLoad(
      HighExpr::makeConst(Cache, 8, ConstantAddressProvenance::DataAddress),
      Integer);
  HighStmt FastPath;
  FastPath.Kind = StmtKind::If;
  FastPath.Cond = HighExpr::makeBinop(NdOp::INT_NOTEQUAL, CacheValue,
                                      HighExpr::makeConst(0, 8));
  HighStmt FastReturn;
  FastReturn.Kind = StmtKind::Return;
  FastReturn.RetVal = CacheValue;
  FastPath.Body = {FastReturn};
  HighStmt LoadClass;
  LoadClass.Kind = StmtKind::Assign;
  LoadClass.Dst = ClassValue;
  LoadClass.Val = HighExpr::makeLoad(
      HighExpr::makeConst(ClassReference, 8,
                          ConstantAddressProvenance::DataAddress),
      Integer);
  HighStmt GetSelf;
  GetSelf.Kind = StmtKind::Assign;
  GetSelf.Dst = SelfValue;
  GetSelf.Val = HighExpr::makeCall("objc_opt_self", ObjCSelf, {ClassValue});
  const auto ObjCSelfHint = objcRuntimeSourceCallHint(F.Image, ObjCSelf);
  ASSERT_TRUE(ObjCSelfHint);
  GetSelf.Val->Type = Pointer;
  GetSelf.Val->SourceCallHint =
      std::make_shared<SourceCallTypeHint>(*ObjCSelfHint);
  GetSelf.Val->CallAddr = 0;
  HighStmt GetMetadata;
  GetMetadata.Kind = StmtKind::Assign;
  GetMetadata.Dst = MetadataValue;
  GetMetadata.Val = HighExpr::makeCall("swift_getObjCClassMetadata",
                                       MetadataRuntime, {SelfValue});
  const auto MetadataHint =
      swiftRuntimeSourceCallHint(F.Image, MetadataRuntime);
  ASSERT_TRUE(MetadataHint);
  GetMetadata.Val->Type = Pointer;
  GetMetadata.Val->SourceCallHint =
      std::make_shared<SourceCallTypeHint>(*MetadataHint);
  GetMetadata.Val->CallAddr = 0;
  HighStmt Publish;
  Publish.Kind = StmtKind::Store;
  Publish.StoreAddr =
      HighExpr::makeConst(Cache, 8, ConstantAddressProvenance::DataAddress);
  Publish.StoreVal = MetadataValue;
  Publish.MemoryOrdering = NdMemoryOrdering::Release;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = MetadataValue;
  MetadataAccessor.Body = {LoadCache,   FastPath, LoadClass, GetSelf,
                           GetMetadata, Publish,  Return};

  HighFunc Caller;
  Caller.Entry = CallerAddress;
  Caller.Name = "metadata_user";
  Caller.ReturnType = Pointer;
  Caller.SourceTypeHint = swift_once_source_detail::callbackHint(Arch::AArch64);
  Caller.SourceTypeHint->Parameters.clear();
  Return.RetVal =
      HighExpr::makeCall(AccessorName, Accessor, {HighExpr::makeConst(0, 8)});
  Return.RetVal->Type = Pointer;
  auto NativeCall = std::make_shared<SourceCallTypeHint>();
  NativeCall->CallKind = SourceCallTypeHint::Kind::Native;
  NativeCall->TargetAddress = Accessor;
  NativeCall->TargetName = AccessorName;
  NativeCall->Signature = AccessorSignature;
  Return.RetVal->SourceCallHint = std::move(NativeCall);
  Return.RetVal->CallAddr = 0;
  Caller.Body = {Return};
  F.Pipeline.HighFuncs.push_back(MetadataAccessor);
  F.Pipeline.HighFuncs.push_back(Caller);

  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(Plan.ObjCClassMetadataAccessors.size(), 1U);
  const auto Bound = bindSwiftOnceSourceReferences(
      F.Pipeline.HighFuncs.back(), F.Image, Plan, F.functions());
  EXPECT_TRUE(Bound.Dependencies.empty());
  const auto Call = Bound.Function.Body[0].RetVal;
  ASSERT_TRUE(Call->SourceCallHint);
  EXPECT_EQ(Call->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::SwiftRuntimeCall);
  EXPECT_EQ(Call->SourceCallHint->TargetName, "swift_getObjCClassMetadata");
  ASSERT_EQ(Call->Operands.size(), 1U);
  ASSERT_TRUE(Call->Operands[0]->SourceCallHint);
  EXPECT_EQ(Call->Operands[0]->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeClass);
  EXPECT_EQ(Call->Operands[0]->SourceCallHint->TargetName, "UIColor");
  EXPECT_TRUE(
      bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());

  SourceFunctionTypeHint ZeroParameterSignature;
  ZeroParameterSignature.Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  ZeroParameterSignature.ReturnType = Pointer;
  Error.clear();
  ASSERT_TRUE(assignDarwinScalarSourceABI(ZeroParameterSignature, Arch::AArch64,
                                          Error));
  MetadataAccessor.Params.clear();
  MetadataAccessor.SourceTypeHint = ZeroParameterSignature;
  Caller.Body[0].RetVal->Operands.clear();
  auto ZeroParameterCall = std::make_shared<SourceCallTypeHint>(
      *Caller.Body[0].RetVal->SourceCallHint);
  ZeroParameterCall->Signature = ZeroParameterSignature;
  Caller.Body[0].RetVal->SourceCallHint = std::move(ZeroParameterCall);
  F.Pipeline.HighFuncs[F.Pipeline.HighFuncs.size() - 2] = MetadataAccessor;
  F.Pipeline.HighFuncs.back() = Caller;
  const auto ZeroParameterPlan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  ASSERT_EQ(ZeroParameterPlan.ObjCClassMetadataAccessors.size(), 1U);
  const auto ZeroParameterBound = bindSwiftOnceSourceReferences(
      F.Pipeline.HighFuncs.back(), F.Image, ZeroParameterPlan, F.functions());
  ASSERT_TRUE(ZeroParameterBound.Function.Body[0].RetVal->SourceCallHint);
  EXPECT_EQ(
      ZeroParameterBound.Function.Body[0].RetVal->SourceCallHint->CallKind,
      SourceCallTypeHint::Kind::SwiftRuntimeCall);

  auto Drifted = MetadataAccessor;
  Drifted.Body[5].MemoryOrdering = NdMemoryOrdering::None;
  F.Pipeline.HighFuncs[F.Pipeline.HighFuncs.size() - 2] = std::move(Drifted);
  EXPECT_TRUE(discoverSwiftOnceSources(F.Image, F.Pipeline)
                  .ObjCClassMetadataAccessors.empty());
}

TEST(SwiftOnceSources, RejectsAddressorEvidenceDrift) {
  for (unsigned Case = 0; Case < 15; ++Case) {
    SCOPED_TRACE(Case);
    AddressorFixture F(Arch::AArch64);
    auto &Accessor = F.Pipeline.HighFuncs[0];
    auto &Initializer = F.Pipeline.HighFuncs[1];
    auto &Caller = F.Pipeline.HighFuncs[2];
    if (Case == 0)
      Accessor.Name += "x";
    if (Case == 1)
      F.Image.Symbols[2].Name += "x";
    if (Case == 2)
      F.Image.Symbols[3].Name += "x";
    if (Case == 3)
      F.Once->Operands[0] = HighExpr::makeConst(0x2018, 8);
    if (Case == 4)
      F.Once->Operands[2] = F.Once->Operands[1];
    if (Case == 5)
      Accessor.Body[1].Body[1].RetVal = HighExpr::makeConst(0x2018, 8);
    if (Case == 6)
      Accessor.Body[1].Cond->Op = NdOp::INT_EQUAL;
    if (Case == 7) {
      HighStmt Use;
      Use.Kind = StmtKind::ExprStmt;
      MedVar Context;
      Context.Kind = MedVar::Param;
      Context.Id = 0;
      Context.Size = 8;
      Use.Val = HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
      Initializer.Body.insert(Initializer.Body.begin(), Use);
    }
    if (Case == 8)
      Caller.Body[0].RetVal->Operands.push_back(HighExpr::makeConst(1, 8));
    if (Case == 9) {
      LowFunc DirectCaller;
      DirectCaller.Blocks.emplace_back();
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::cst(AddressorFixture::InitializerAddress, 8));
      DirectCaller.Blocks[0].Ops.push_back(Call);
      F.Pipeline.LowFuncs.push_back(DirectCaller);
    }
    if (Case == 10) {
      HighStmt Effect;
      Effect.Kind = StmtKind::ExprStmt;
      Effect.Val = HighExpr::makeConst(1, 8);
      Accessor.Body[2].Body.push_back(Effect);
    }
    if (Case == 11)
      Accessor.Params[1].Type = NdType::makeInt(4);
    if (Case == 12)
      Accessor.ReturnType = NdType::makeVoid();
    if (Case == 13 || Case == 14) {
      auto Forged = std::make_shared<SourceCallTypeHint>(
          *Caller.Body[0].RetVal->SourceCallHint);
      if (Case == 13)
        Forged->TargetAddress += 8;
      else
        Forged->Signature.ReturnType = NdType::makeInt(8);
      Caller.Body[0].RetVal->SourceCallHint = std::move(Forged);
    }
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    const auto Bound =
        bindSwiftOnceSourceReferences(Caller, F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.Dependencies.empty());
    EXPECT_TRUE(Bound.LocalStorageExtents.empty());
    EXPECT_TRUE(Bound.SwiftOnceAccessors.empty());
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

TEST(SwiftOnceSources, RebuildsDispatchOncePredicateAndCallbackAddress) {
  OnceFixture F(Arch::AArch64);
  constexpr va_t DispatchSlot = 0x2088;
  auto &Wrapper = F.Pipeline.HighFuncs[0];
  auto &Callback = F.Pipeline.HighFuncs[1];
  F.Image.Symbols.erase(F.Image.Symbols.begin());
  F.Image.Sections[1].Type = llvm::MachO::S_ZEROFILL;
  F.Image.ImportPtrSlots[DispatchSlot] = "_dispatch_once_f";
  F.Image.DyldBindSlots[DispatchSlot] = {
      "_dispatch_once_f", 0, "/usr/lib/system/libdispatch.dylib", false};
  const auto Runtime = darwinRuntimeSourceCallHint(F.Image, DispatchSlot);
  ASSERT_TRUE(Runtime);
  auto Call = HighExpr::makeCall("dispatch_once_f", DispatchSlot,
                                 {HighExpr::makeConst(0x2000, 8),
                                  HighExpr::makeConst(0, 8),
                                  HighExpr::makeConst(Callback.Entry, 8)});
  Call->Type = NdType::makeVoid();
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
  HighStmt Invoke;
  Invoke.Kind = StmtKind::Call;
  Invoke.CallExpr = Call;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Wrapper.Params.clear();
  Wrapper.ReturnType = NdType::makeVoid();
  Wrapper.SourceTypeHint->Parameters.clear();
  Wrapper.SourceTypeHint->ReturnType = NdType::makeVoid();
  Wrapper.Body = {Invoke, Return};
  Callback.SourceTypeHint =
      swift_once_source_detail::dispatchCallbackHint(F.Image.Arch);
  Callback.Params = {{"once_context", NdType::makePtr(NdType::makeVoid())}};
  HighStmt Use;
  Use.Kind = StmtKind::ExprStmt;
  MedVar Context;
  Context.Kind = MedVar::Param;
  Context.Id = 0;
  Context.Size = 8;
  Use.Val = HighExpr::makeVar(Context, NdType::makePtr(NdType::makeVoid()));
  Callback.Body.insert(Callback.Body.begin(), Use);

  const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
  EXPECT_EQ(Plan.DispatchOnceCallbacks, std::set<va_t>{Callback.Entry});
  ASSERT_EQ(Plan.CallbackHints.count(Callback.Entry), 1U);
  PipelineOptions Options;
  EXPECT_EQ(applySwiftOnceSourceHints(Plan, Options), 1U);
  EXPECT_EQ(Options.SourceTypeHints.at(Callback.Entry).Origin,
            SourceFunctionTypeHint::OriginKind::DarwinSDK);

  const auto Bound =
      bindSwiftOnceSourceReferences(Wrapper, F.Image, Plan, F.functions());
  EXPECT_EQ(Bound.Dependencies, std::set<va_t>{Callback.Entry});
  EXPECT_EQ(Bound.LocalStorageExtents, (std::map<va_t, uint64_t>{{0x2000, 8}}));
  const auto BoundCall = Bound.Function.Body[0].CallExpr;
  ASSERT_TRUE(BoundCall);
  ASSERT_EQ(BoundCall->Operands.size(), 3U);
  ASSERT_TRUE(BoundCall->Operands[0]->SourceCallHint);
  EXPECT_EQ(BoundCall->Operands[0]->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::RuntimeLocalStorageAddress);
  EXPECT_EQ(BoundCall->Operands[0]->SourceCallHint->TargetAddress, 0x2000U);
  ASSERT_TRUE(BoundCall->Operands[2]->SourceCallHint);
  EXPECT_EQ(BoundCall->Operands[2]->SourceCallHint->CallKind,
            SourceCallTypeHint::Kind::NativeAddress);
  EXPECT_TRUE(swiftOnceCallbackBound(*BoundCall->Operands[2], F.Image, Plan,
                                     F.functions()));
  EXPECT_TRUE(
      bindObjCSourceReferences(Bound.Function, F.Image).Limitation.empty());
}

TEST(SwiftOnceSources, RejectsUnprovedDispatchOnceReferences) {
  for (unsigned Case = 0; Case < 8; ++Case) {
    OnceFixture F(Arch::AArch64);
    constexpr va_t DispatchSlot = 0x2088;
    F.Image.ImportPtrSlots[DispatchSlot] = "_dispatch_once_f";
    F.Image.DyldBindSlots[DispatchSlot] = {
        "_dispatch_once_f", 0, "/usr/lib/system/libdispatch.dylib", false};
    const auto Runtime = darwinRuntimeSourceCallHint(F.Image, DispatchSlot);
    ASSERT_TRUE(Runtime);
    auto &Wrapper = F.Pipeline.HighFuncs[0];
    auto &Callback = F.Pipeline.HighFuncs[1];
    F.Image.Symbols.erase(F.Image.Symbols.begin());
    F.Image.Sections[1].Type = llvm::MachO::S_ZEROFILL;
    auto Call = HighExpr::makeCall("dispatch_once_f", DispatchSlot,
                                   {HighExpr::makeConst(0x2000, 8),
                                    HighExpr::makeConst(0, 8),
                                    HighExpr::makeConst(Callback.Entry, 8)});
    Call->Type = NdType::makeVoid();
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Runtime);
    HighStmt Invoke;
    Invoke.Kind = StmtKind::Call;
    Invoke.CallExpr = Call;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Wrapper.Params.clear();
    Wrapper.ReturnType = NdType::makeVoid();
    Wrapper.SourceTypeHint->Parameters.clear();
    Wrapper.SourceTypeHint->ReturnType = NdType::makeVoid();
    Wrapper.Body = {Invoke, Return};
    Callback.SourceTypeHint =
        swift_once_source_detail::dispatchCallbackHint(F.Image.Arch);
    Callback.Params = {{"once_context", NdType::makePtr(NdType::makeVoid())}};
    if (Case == 0)
      F.Image.DyldBindSlots[DispatchSlot].Module = "/tmp/libdispatch.dylib";
    if (Case == 1)
      Call->Operands[0] = HighExpr::makeConst(0x2001, 8);
    if (Case == 2)
      Call->Operands[2] = HighExpr::makeConst(0x2010, 8);
    if (Case == 3) {
      LowFunc Direct;
      Direct.Blocks.emplace_back();
      LowOp Op;
      Op.Opcode = NdOp::CALL;
      Op.addInput(NdVar::cst(Callback.Entry, 8));
      Direct.Blocks[0].Ops.push_back(Op);
      F.Pipeline.LowFuncs.push_back(Direct);
    }
    if (Case == 4)
      Callback.SourceTypeHint->ReturnType = NdType::makeInt(8);
    if (Case == 5)
      F.Image.Sections[1].Type = 0;
    if (Case == 6)
      F.Image.Segments[1].Data[0] = 1;
    if (Case == 7)
      F.Image.Symbols.push_back({"_overlap", 0x1ff8, 16, false});
    const auto Plan = discoverSwiftOnceSources(F.Image, F.Pipeline);
    const auto Bound =
        bindSwiftOnceSourceReferences(Wrapper, F.Image, Plan, F.functions());
    EXPECT_TRUE(Bound.Dependencies.empty()) << Case;
    EXPECT_TRUE(Bound.LocalStorageExtents.empty()) << Case;
  }
}

} // namespace
