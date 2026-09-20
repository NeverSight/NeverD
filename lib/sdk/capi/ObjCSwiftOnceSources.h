#ifndef NEVERD_SDK_CAPI_OBJCSWIFTONCESOURCES_H
#define NEVERD_SDK_CAPI_OBJCSWIFTONCESOURCES_H

#include "ObjCSourceBindings.h"

#include "neverd/pipeline/Pipeline.h"

namespace neverd::sdk {

struct SwiftOnceGetterContract {
  size_t Predicate = 0;
  size_t Storage = 0;
  std::optional<size_t> SecondStorage;
  size_t Initializer = 0;

  size_t parameterCount() const { return SecondStorage ? 4 : 3; }
  uint64_t storageWidth() const { return SecondStorage ? 16 : 8; }
};

struct SwiftOnceAddressorContract {
  va_t Predicate = 0;
  va_t Storage = 0;
  va_t Initializer = 0;

  bool operator==(const SwiftOnceAddressorContract &Other) const {
    return Predicate == Other.Predicate && Storage == Other.Storage &&
           Initializer == Other.Initializer;
  }
};

struct SwiftObjCClassMetadataAccessorContract {
  va_t Cache = 0;
  va_t ClassReference = 0;
  va_t MetadataRuntime = 0;
  std::string ClassName;

  bool operator==(const SwiftObjCClassMetadataAccessorContract &Other) const {
    return Cache == Other.Cache && ClassReference == Other.ClassReference &&
           MetadataRuntime == Other.MetadataRuntime &&
           ClassName == Other.ClassName;
  }
};

struct SwiftOnceSourcePlan {
  std::map<va_t, SwiftOnceGetterContract> Getters;
  std::map<va_t, SwiftOnceAddressorContract> Addressors;
  std::map<va_t, SwiftObjCClassMetadataAccessorContract>
      ObjCClassMetadataAccessors;
  std::map<va_t, SourceFunctionTypeHint> AddressorHints;
  std::map<va_t, SourceFunctionTypeHint> CallbackHints;
};

namespace swift_once_source_detail {
inline std::optional<size_t> parameter(const ExprPtr &Value) {
  auto E = Value;
  unsigned Depth = 0;
  while (E && E->Kind == ExprKind::Cast && E->Operands.size() == 1 && E->Type &&
         E->Type->Size == 8 && E->IntrinsicId == Intrinsic::None &&
         E->IntrinsicOutputs.empty() &&
         E->MemoryOrdering == NdMemoryOrdering::None &&
         E->MemoryAddressSpace == NdMemoryAddressSpace::Default && Depth++ < 16)
    E = E->Operands.front();
  if (!E || E->Kind != ExprKind::Var || E->Var.Kind != MedVar::Param ||
      E->Var.Id < 0 || E->Var.SSAVer != 0 || E->Var.RenameTag != -1 ||
      !E->Operands.empty() || !E->Type || E->Type->Size != 8 ||
      E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
      E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return std::nullopt;
  return static_cast<size_t>(E->Var.Id);
}

inline bool onceCall(const HighExpr &E, const BinaryImage &Image) {
  if (E.Kind != ExprKind::Call || E.IsIndirectCall || !E.SourceCallHint ||
      E.Operands.size() != 3 || E.IntrinsicId != Intrinsic::None ||
      !E.IntrinsicOutputs.empty() ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto &Hint = *E.SourceCallHint;
  if (Hint.CallKind != SourceCallTypeHint::Kind::SwiftRuntimeCall ||
      Hint.TargetName != "swift_once" || Hint.Receiver || Hint.ValueWitness ||
      Hint.ImmutablePointerSlot)
    return false;
  const auto Expected = swiftRuntimeSourceCallHint(Image, Hint.TargetAddress);
  return Expected &&
         objc_binding_detail::runtimeBindingMatches(Hint, *Expected);
}

inline bool swiftStringBridgeCall(const HighExpr &E, const BinaryImage &Image) {
  if (E.Kind != ExprKind::Call || E.IsIndirectCall || !E.SourceCallHint ||
      E.Operands.size() != 2 || E.IntrinsicId != Intrinsic::None ||
      !E.IntrinsicOutputs.empty() ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto &Hint = *E.SourceCallHint;
  if (Hint.CallKind != SourceCallTypeHint::Kind::SwiftStringBridge ||
      Hint.Receiver || Hint.ValueWitness || Hint.ImmutablePointerSlot)
    return false;
  const auto Expected = swiftStringSourceCallHint(Image, Hint.TargetAddress);
  return Expected &&
         objc_binding_detail::runtimeBindingMatches(Hint, *Expected);
}

inline std::optional<SwiftObjCClassMetadataAccessorContract>
objcClassMetadataAccessorContract(const HighFunc &F, const BinaryImage &Image) {
  if (F.Params.size() > 1 || !F.ReturnType || F.ReturnType->Size != 8 ||
      (F.ReturnType->Kind != NdTypeKind::Int &&
       F.ReturnType->Kind != NdTypeKind::Ptr) ||
      F.Body.size() != 7)
    return std::nullopt;
  if (!F.Params.empty() && (!F.Params[0].Type || F.Params[0].Type->Size != 8 ||
                            (F.Params[0].Type->Kind != NdTypeKind::Int &&
                             F.Params[0].Type->Kind != NdTypeKind::Ptr)))
    return std::nullopt;
  const auto Plain = [](ExprPtr E) {
    unsigned Depth = 0;
    while (E && (E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) &&
           E->Operands.size() == 1 && Depth++ < 16)
      E = E->Operands.front();
    return E;
  };
  const auto Assignment = [&](size_t I) -> const HighStmt * {
    const auto &S = F.Body[I];
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val ||
        (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi) ||
        !S.Dst->Operands.empty())
      return nullptr;
    return &S;
  };
  const auto *CacheAssignment = Assignment(0);
  const auto *ClassAssignment = Assignment(2);
  const auto *SelfAssignment = Assignment(3);
  const auto *MetadataAssignment = Assignment(4);
  if (!CacheAssignment || !ClassAssignment || !SelfAssignment ||
      !MetadataAssignment)
    return std::nullopt;
  const auto SameLocal = [&](const ExprPtr &Value, const ExprPtr &Definition) {
    const auto E = Plain(Value);
    return E && Definition &&
           (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
           highSourceLocalIdentity(E->Var) ==
               highSourceLocalIdentity(Definition->Var);
  };
  const auto LoadAddress = [&](const ExprPtr &Value) -> std::optional<va_t> {
    const auto E = Plain(Value);
    if (!E || E->Kind != ExprKind::Load || E->Operands.size() != 1 ||
        !E->Type || E->Type->Size != 8 ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty())
      return std::nullopt;
    return objc_binding_detail::constantAddress(*E->Operands.front());
  };
  const auto Cache = LoadAddress(CacheAssignment->Val);
  const auto ClassReference = LoadAddress(ClassAssignment->Val);
  if (!Cache || !ClassReference || *Cache == *ClassReference)
    return std::nullopt;
  const auto CacheHint =
      objc_binding_detail::localStorageHint(Image, *Cache, 8);
  const auto ClassHint =
      objc_binding_detail::classReferenceAddressHint(Image, *ClassReference);
  if (!CacheHint || !ClassHint ||
      ClassHint->CallKind !=
          SourceCallTypeHint::Kind::RuntimeClassReferenceAddress)
    return std::nullopt;

  const auto &Conditional = F.Body[1];
  if (Conditional.Kind != StmtKind::If || !Conditional.Cond ||
      !Conditional.ElseBody.empty() || Conditional.Body.size() != 1 ||
      Conditional.Body[0].Kind != StmtKind::Return ||
      !Conditional.Body[0].RetVal)
    return std::nullopt;
  const auto Condition = Plain(Conditional.Cond);
  if (!Condition || Condition->Kind != ExprKind::BinOp ||
      Condition->Op != NdOp::INT_NOTEQUAL || Condition->Operands.size() != 2)
    return std::nullopt;
  const auto IsZero = [&](const ExprPtr &E) {
    const auto V = Plain(E);
    return V && V->Kind == ExprKind::Const && V->ConstVal == 0;
  };
  const bool CacheTest =
      (SameLocal(Condition->Operands[0], CacheAssignment->Dst) &&
       IsZero(Condition->Operands[1])) ||
      (SameLocal(Condition->Operands[1], CacheAssignment->Dst) &&
       IsZero(Condition->Operands[0]));
  if (!CacheTest ||
      !SameLocal(Conditional.Body[0].RetVal, CacheAssignment->Dst))
    return std::nullopt;

  const auto RuntimeCall = [&](const ExprPtr &Value, llvm::StringRef Name,
                               bool Swift) -> const HighExpr * {
    const auto E = Plain(Value);
    if (!E || E->Kind != ExprKind::Call || E->IsIndirectCall ||
        !E->SourceCallHint || E->Operands.size() != 1 ||
        E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return nullptr;
    const auto Target = E->SourceCallHint->TargetAddress;
    const auto Expected = Swift ? swiftRuntimeSourceCallHint(Image, Target)
                                : objcRuntimeSourceCallHint(Image, Target);
    if (!Expected || Expected->TargetName != Name ||
        !objc_binding_detail::runtimeBindingMatches(*E->SourceCallHint,
                                                    *Expected))
      return nullptr;
    return E.get();
  };
  const auto *Self = RuntimeCall(SelfAssignment->Val, "objc_opt_self", false);
  const auto *Metadata =
      RuntimeCall(MetadataAssignment->Val, "swift_getObjCClassMetadata", true);
  if (!Self || !Metadata ||
      !SameLocal(Self->Operands[0], ClassAssignment->Dst) ||
      !SameLocal(Metadata->Operands[0], SelfAssignment->Dst))
    return std::nullopt;

  const auto &Store = F.Body[5];
  const auto &Return = F.Body[6];
  const auto StoreAddress =
      Store.StoreAddr ? objc_binding_detail::constantAddress(*Store.StoreAddr)
                      : std::nullopt;
  if (Store.Kind != StmtKind::Store || !StoreAddress ||
      *StoreAddress != *Cache || !Store.StoreVal ||
      !SameLocal(Store.StoreVal, MetadataAssignment->Dst) ||
      Store.MemoryOrdering != NdMemoryOrdering::Release ||
      Store.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Return.Kind != StmtKind::Return || !Return.RetVal ||
      !SameLocal(Return.RetVal, MetadataAssignment->Dst))
    return std::nullopt;

  llvm::StringRef FunctionName(F.Name);
  if (!FunctionName.ends_with("CMa") ||
      CacheHint->TargetName != FunctionName.drop_back().str() + "L")
    return std::nullopt;
  const std::string ExpectedName =
      "_$sSo" + std::to_string(ClassHint->TargetName.size()) +
      ClassHint->TargetName + "CMa";
  if (F.Name != ExpectedName)
    return std::nullopt;
  bool ExactFunction = false;
  for (const auto &Symbol : Image.Symbols)
    ExactFunction |=
        Symbol.IsFunc && Symbol.Addr == F.Entry && Symbol.Name == F.Name;
  if (!ExactFunction)
    return std::nullopt;
  return SwiftObjCClassMetadataAccessorContract{
      *Cache, *ClassReference, Metadata->SourceCallHint->TargetAddress,
      ClassHint->TargetName};
}

// Recognize a use contract, not a symbol name or instruction template. Every
// use of each pointer parameter must be accounted for. The callback receives
// the predicate as context; binding additionally requires that it ignores it.
inline std::optional<SwiftOnceGetterContract>
getterContract(const HighFunc &F, const BinaryImage &Image) {
  if (!F.SourceTypeHint ||
      F.SourceTypeHint->Origin !=
          SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
      F.SourceTypeHint->Convention !=
          SourceFunctionTypeHint::ConventionKind::C ||
      (F.Params.size() != 3 && F.Params.size() != 4) ||
      F.SourceTypeHint->Parameters.size() != F.Params.size())
    return std::nullopt;
  for (const auto &P : F.Params)
    if (!P.Type || P.Type->Size != 8 ||
        (P.Type->Kind != NdTypeKind::Ptr && P.Type->Kind != NdTypeKind::Int))
      return std::nullopt;
  const auto Flow = buildHighSourceFlowGraph(F);
  if (!Flow.Diagnostics.Complete || !Flow.Diagnostics.Items.empty())
    return std::nullopt;
  using Local = HighSourceLocalIdentity;
  std::map<Local, std::vector<ExprPtr>> Definitions;
  bool DefinitionsValid = true;
  walkStmts(F.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst)
      return;
    if ((S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi) ||
        !S.Dst->Operands.empty() || !S.Dst->Type || S.Dst->Type->Size != 8 ||
        (S.Dst->Type->Kind != NdTypeKind::Int &&
         S.Dst->Type->Kind != NdTypeKind::Ptr) ||
        S.Dst->IntrinsicId != Intrinsic::None ||
        !S.Dst->IntrinsicOutputs.empty() ||
        S.Dst->MemoryOrdering != NdMemoryOrdering::None ||
        S.Dst->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (S.Dst->Var.Kind != MedVar::Reg && S.Dst->Var.Kind != MedVar::Temp) ||
        !S.Val) {
      if (S.Dst->Kind == ExprKind::Var && S.Dst->Var.Kind == MedVar::Param)
        DefinitionsValid = false;
      return;
    }
    Definitions[highSourceLocalIdentity(S.Dst->Var)].push_back(S.Val);
  });
  if (!DefinitionsValid)
    return std::nullopt;

  const auto PlainScalar = [](const ExprPtr &E) {
    return E && E->Type && E->Type->Size == 8 &&
           (E->Type->Kind == NdTypeKind::Int ||
            E->Type->Kind == NdTypeKind::Ptr) &&
           E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty() &&
           E->MemoryOrdering == NdMemoryOrdering::None &&
           E->MemoryAddressSpace == NdMemoryAddressSpace::Default;
  };
  std::function<std::optional<size_t>(const ExprPtr &, unsigned,
                                      std::set<Local> &)>
      AliasOrigin;
  AliasOrigin = [&](const ExprPtr &Value, unsigned Depth,
                    std::set<Local> &Visiting) -> std::optional<size_t> {
    if (!PlainScalar(Value) || Depth > 64)
      return std::nullopt;
    if (Value->Kind == ExprKind::Var && Value->Var.Kind == MedVar::Param)
      return parameter(Value);
    if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
        Value->Operands.size() == 1)
      return AliasOrigin(Value->Operands[0], Depth + 1, Visiting);
    if (Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi)
      return std::nullopt;
    if (!Value->Operands.empty()) {
      std::optional<size_t> Origin;
      for (const auto &Operand : Value->Operands) {
        const auto Candidate = AliasOrigin(Operand, Depth + 1, Visiting);
        if (!Candidate || (Origin && *Origin != *Candidate))
          return std::nullopt;
        Origin = Candidate;
      }
      return Origin;
    }
    if (Value->Var.Kind != MedVar::Reg && Value->Var.Kind != MedVar::Temp)
      return std::nullopt;
    const auto Identity = highSourceLocalIdentity(Value->Var);
    const auto Found = Definitions.find(Identity);
    if (Found == Definitions.end() || Found->second.empty() ||
        !Visiting.insert(Identity).second)
      return std::nullopt;
    std::optional<size_t> Origin;
    for (const auto &Definition : Found->second) {
      const auto Candidate = AliasOrigin(Definition, Depth + 1, Visiting);
      if (!Candidate || (Origin && *Origin != *Candidate)) {
        Visiting.erase(Identity);
        return std::nullopt;
      }
      Origin = Candidate;
    }
    Visiting.erase(Identity);
    return Origin;
  };
  const auto Alias = [&](const ExprPtr &Value) {
    std::set<Local> Visiting;
    return AliasOrigin(Value, 0, Visiting);
  };

  std::function<std::optional<size_t>(const ExprPtr &, unsigned,
                                      std::set<Local> &)>
      LoadOrigin;
  LoadOrigin = [&](const ExprPtr &Value, unsigned Depth,
                   std::set<Local> &Visiting) -> std::optional<size_t> {
    if (!PlainScalar(Value) || Depth > 64)
      return std::nullopt;
    if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
        Value->Operands.size() == 1)
      return LoadOrigin(Value->Operands[0], Depth + 1, Visiting);
    if (Value->Kind == ExprKind::Load && Value->Operands.size() == 1)
      return Alias(Value->Operands[0]);
    if ((Value->Kind != ExprKind::Var && Value->Kind != ExprKind::Phi) ||
        !Value->Operands.empty() ||
        (Value->Var.Kind != MedVar::Reg && Value->Var.Kind != MedVar::Temp))
      return std::nullopt;
    const auto Identity = highSourceLocalIdentity(Value->Var);
    const auto Found = Definitions.find(Identity);
    if (Found == Definitions.end() || Found->second.empty() ||
        !Visiting.insert(Identity).second)
      return std::nullopt;
    std::optional<size_t> Origin;
    for (const auto &Definition : Found->second) {
      const auto Candidate = LoadOrigin(Definition, Depth + 1, Visiting);
      if (!Candidate || (Origin && *Origin != *Candidate)) {
        Visiting.erase(Identity);
        return std::nullopt;
      }
      Origin = Candidate;
    }
    Visiting.erase(Identity);
    return Origin;
  };
  const auto LoadedFrom = [&](const ExprPtr &Value) {
    std::set<Local> Visiting;
    return LoadOrigin(Value, 0, Visiting);
  };
  std::set<const HighExpr *> AliasDefinitions;
  for (const auto &[Identity, Values] : Definitions)
    for (const auto &Value : Values)
      if (Alias(Value))
        AliasDefinitions.insert(Value.get());

  std::optional<std::pair<size_t, size_t>> OnceParameters;
  std::optional<std::pair<size_t, size_t>> StringStorageParameters;
  std::set<size_t> Reads;
  size_t Budget = 100000;
  bool Valid = true;
  std::function<void(const ExprPtr &, unsigned)> Scan = [&](const ExprPtr &E,
                                                            unsigned Depth) {
    if (!Valid)
      return;
    if (!E || !Budget-- || Depth > 128 || E->Kind == ExprKind::Undef) {
      Valid = false;
      return;
    }
    if (onceCall(*E, Image)) {
      const auto P = Alias(E->Operands[0]);
      const auto I = Alias(E->Operands[1]);
      if (OnceParameters || !P || !I || *P >= F.Params.size() ||
          *I >= F.Params.size() || *P == *I || Alias(E->Operands[2]) != P) {
        Valid = false;
        return;
      }
      OnceParameters = std::pair{*P, *I};
      return;
    }
    if (swiftStringBridgeCall(*E, Image)) {
      const auto First = LoadedFrom(E->Operands[0]);
      const auto Second = LoadedFrom(E->Operands[1]);
      if (F.Params.size() != 4 || StringStorageParameters || !First ||
          !Second || *First >= F.Params.size() || *Second >= F.Params.size() ||
          *First == *Second) {
        Valid = false;
        return;
      }
      StringStorageParameters = std::pair{*First, *Second};
      Reads.insert(*First);
      Reads.insert(*Second);
      return;
    }
    if (E->Kind == ExprKind::Load && E->Operands.size() == 1 && E->Type &&
        E->Type->Kind == NdTypeKind::Int && E->Type->Size == 8 &&
        E->MemoryOrdering == NdMemoryOrdering::None &&
        E->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty()) {
      if (const auto P = Alias(E->Operands[0]); P && *P < F.Params.size()) {
        Reads.insert(*P);
        return;
      }
    }
    if (AliasDefinitions.count(E.get()))
      return;
    if (Alias(E)) {
      Valid = false;
      return;
    }
    for (const auto &Op : E->Operands)
      Scan(Op, Depth + 1);
  };
  for (const auto &Node : Flow.Nodes) {
    if (Node.Test)
      Scan(Node.Test, 0);
    if (Node.Statement)
      forEachRhsExpr(*Node.Statement, [&](const ExprPtr &E) { Scan(E, 0); });
  }
  if (!Valid || !OnceParameters)
    return std::nullopt;
  const auto [Predicate, Initializer] = *OnceParameters;
  if (F.Params.size() == 3) {
    if (StringStorageParameters)
      return std::nullopt;
    const size_t Storage = 3 - Predicate - Initializer;
    if (Reads != std::set<size_t>{Predicate, Storage})
      return std::nullopt;
    return SwiftOnceGetterContract{Predicate, Storage, std::nullopt,
                                   Initializer};
  }
  if (!StringStorageParameters)
    return std::nullopt;
  const auto [Storage, SecondStorage] = *StringStorageParameters;
  if (Predicate == Storage || Predicate == SecondStorage ||
      Initializer == Storage || Initializer == SecondStorage ||
      Reads != std::set<size_t>{Predicate, Storage, SecondStorage})
    return std::nullopt;
  return SwiftOnceGetterContract{Predicate, Storage, SecondStorage,
                                 Initializer};
}

/// Prove the canonical Swift lazy-global addressor after HighIR structuring.
/// The native signature may contain x0/x1 placeholders solely to reach the
/// incidental swift_once context in x2; the compiler-level addressor itself
/// has no parameters. Restricting this to the exact load/test/call/return
/// shape keeps that machine artifact from becoming public source ABI.
inline std::optional<SwiftOnceAddressorContract>
addressorContract(const HighFunc &F, const BinaryImage &Image) {
  const auto InertLabel = [](const HighStmt &S) {
    return S.Kind == StmtKind::Block && S.Body.empty() && S.ElseBody.empty() &&
           !S.Dst && !S.Val && !S.Cond && !S.RetVal && !S.StoreAddr &&
           !S.StoreVal && !S.CallExpr && !S.SwitchExpr && S.Cases.empty() &&
           S.DefaultBody.empty() && !S.GotoTarget && !S.LoopHeaderAddr &&
           S.MemoryOrdering == NdMemoryOrdering::None &&
           S.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
           !S.EHRange.Begin && !S.EHRange.End && S.EHClauses.empty() &&
           S.EHClauseBodies.empty() && !S.EHIsReducible && !S.IsPhiCopy;
  };
  std::vector<const HighStmt *> Body;
  Body.reserve(F.Body.size());
  for (const auto &S : F.Body)
    if (!InertLabel(S))
      Body.push_back(&S);
  if (F.Params.size() != 3 || !F.ReturnType || F.ReturnType->Size != 8 ||
      (F.ReturnType->Kind != NdTypeKind::Int &&
       F.ReturnType->Kind != NdTypeKind::Ptr) ||
      Body.size() != 3 || Body[0]->Kind != StmtKind::Assign || !Body[0]->Dst ||
      !Body[0]->Val || Body[1]->Kind != StmtKind::If || !Body[1]->Cond ||
      !Body[1]->ElseBody.empty() || Body[1]->Body.size() != 2 ||
      Body[1]->Body[0].Kind != StmtKind::Call || !Body[1]->Body[0].CallExpr ||
      Body[1]->Body[1].Kind != StmtKind::Return || !Body[1]->Body[1].RetVal ||
      Body[2]->Kind != StmtKind::Return || !Body[2]->RetVal)
    return std::nullopt;
  for (const auto &Parameter : F.Params)
    if (!Parameter.Type || Parameter.Type->Size != 8 ||
        (Parameter.Type->Kind != NdTypeKind::Int &&
         Parameter.Type->Kind != NdTypeKind::Ptr))
      return std::nullopt;

  const auto Plain = [](ExprPtr Value) {
    unsigned Depth = 0;
    while (
        Value &&
        (Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
        Value->Operands.size() == 1 && Depth++ < 16)
      Value = Value->Operands.front();
    return Value;
  };
  const auto &Assignment = *Body[0];
  if ((Assignment.Dst->Kind != ExprKind::Var &&
       Assignment.Dst->Kind != ExprKind::Phi) ||
      !Assignment.Dst->Operands.empty() || !Assignment.Dst->Type ||
      Assignment.Dst->Type->Size != 8 ||
      Assignment.Dst->IntrinsicId != Intrinsic::None ||
      !Assignment.Dst->IntrinsicOutputs.empty() ||
      Assignment.Dst->MemoryOrdering != NdMemoryOrdering::None ||
      Assignment.Dst->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return std::nullopt;
  const auto Loaded = Plain(Assignment.Val);
  if (!Loaded || Loaded->Kind != ExprKind::Load ||
      Loaded->Operands.size() != 1 || !Loaded->Type ||
      Loaded->Type->Kind != NdTypeKind::Int || Loaded->Type->Size != 8 ||
      Loaded->MemoryOrdering != NdMemoryOrdering::None ||
      Loaded->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Loaded->IntrinsicId != Intrinsic::None ||
      !Loaded->IntrinsicOutputs.empty())
    return std::nullopt;
  const auto Predicate =
      objc_binding_detail::constantAddress(*Loaded->Operands.front());

  const auto ResolveAssigned = [&](ExprPtr Value) {
    Value = Plain(std::move(Value));
    if (Value &&
        (Value->Kind == ExprKind::Var || Value->Kind == ExprKind::Phi) &&
        highSourceLocalIdentity(Value->Var) ==
            highSourceLocalIdentity(Assignment.Dst->Var))
      Value = Loaded;
    return Plain(std::move(Value));
  };
  const auto IsConstant = [&](const ExprPtr &Value, uint64_t Expected) {
    const auto E = Plain(Value);
    return E && E->Kind == ExprKind::Const && E->ConstVal == Expected;
  };
  auto Condition = Plain(Body[1]->Cond);
  if (!Condition || Condition->Kind != ExprKind::BinOp ||
      Condition->Op != NdOp::INT_NOTEQUAL || Condition->Operands.size() != 2)
    return std::nullopt;
  ExprPtr Added;
  if (IsConstant(Condition->Operands[0], 0))
    Added = ResolveAssigned(Condition->Operands[1]);
  else if (IsConstant(Condition->Operands[1], 0))
    Added = ResolveAssigned(Condition->Operands[0]);
  if (!Added || Added->Kind != ExprKind::BinOp || Added->Op != NdOp::INT_ADD ||
      Added->Operands.size() != 2)
    return std::nullopt;
  ExprPtr AddedValue;
  if (IsConstant(Added->Operands[0], 1))
    AddedValue = ResolveAssigned(Added->Operands[1]);
  else if (IsConstant(Added->Operands[1], 1))
    AddedValue = ResolveAssigned(Added->Operands[0]);
  if (!AddedValue || AddedValue.get() != Loaded.get())
    return std::nullopt;

  const auto &Once = *Body[1]->Body[0].CallExpr;
  if (!onceCall(Once, Image))
    return std::nullopt;
  const auto OncePredicate =
      objc_binding_detail::constantAddress(*Once.Operands[0]);
  const auto Initializer =
      objc_binding_detail::constantAddress(*Once.Operands[1]);
  const auto Context = parameter(Once.Operands[2]);
  const auto FirstStorage = objc_binding_detail::constantAddress(
      *ResolveAssigned(Body[1]->Body[1].RetVal));
  const auto SecondStorage =
      objc_binding_detail::constantAddress(*ResolveAssigned(Body[2]->RetVal));
  if (!Predicate || !OncePredicate || *Predicate != *OncePredicate ||
      !Initializer || !Context || *Context != 2 || !FirstStorage ||
      !SecondStorage || *FirstStorage != *SecondStorage ||
      *Predicate == *FirstStorage)
    return std::nullopt;

  const auto PredicateHint =
      objc_binding_detail::oncePredicateStorageHint(Image, *Predicate);
  const auto StorageHint =
      objc_binding_detail::localStorageHint(Image, *FirstStorage, 8);
  if (!PredicateHint || !StorageHint || !Image.isCodeAddress(*Initializer))
    return std::nullopt;
  llvm::StringRef AccessorName(F.Name);
  if (!AccessorName.ends_with("vau") ||
      StorageHint->TargetName != AccessorName.drop_back(3).str() + "vpZ")
    return std::nullopt;
  llvm::StringRef PredicateName(PredicateHint->TargetName);
  if (!PredicateName.ends_with("_Wz"))
    return std::nullopt;
  const std::string Prefix = PredicateName.drop_back(3).str();
  if (!AccessorName.starts_with(Prefix))
    return std::nullopt;
  const std::string InitializerName = Prefix + "_WZ";
  bool ExactAccessor = false, ExactInitializer = false;
  for (const auto &Symbol : Image.Symbols) {
    ExactAccessor |=
        Symbol.IsFunc && Symbol.Addr == F.Entry && Symbol.Name == F.Name;
    ExactInitializer |= Symbol.IsFunc && Symbol.Addr == *Initializer &&
                        Symbol.Name == InitializerName;
  }
  if (!ExactAccessor || !ExactInitializer)
    return std::nullopt;
  return SwiftOnceAddressorContract{*Predicate, *FirstStorage, *Initializer};
}

inline SourceFunctionTypeHint callbackHint(Arch Architecture) {
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Hint.ReturnType = NdType::makeVoid();
  Hint.Parameters = {{"once_context", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  if (!assignDarwinScalarSourceABI(Hint, Architecture, Error))
    throw std::invalid_argument(Error);
  return Hint;
}

inline SourceFunctionTypeHint addressorHint(Arch Architecture) {
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  if (!assignDarwinScalarSourceABI(Hint, Architecture, Error))
    throw std::invalid_argument(Error);
  return Hint;
}

inline bool ignoresContext(const HighFunc &F) {
  const auto Flow = buildHighSourceFlowGraph(F);
  if (!Flow.Diagnostics.Complete || !Flow.Diagnostics.Items.empty())
    return false;
  size_t Budget = 100000;
  std::vector<ExprPtr> Pending;
  for (const auto &Node : Flow.Nodes) {
    if (Node.Test)
      Pending.push_back(Node.Test);
    if (Node.Statement)
      forEachExpr(*Node.Statement,
                  [&](const ExprPtr &E) { Pending.push_back(E); });
  }
  std::set<const HighExpr *> Seen;
  while (!Pending.empty()) {
    auto E = Pending.back();
    Pending.pop_back();
    if (!E || !Budget-- || E->Kind == ExprKind::Undef)
      return false;
    if (!Seen.insert(E.get()).second)
      continue;
    if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Param)
      return false;
    Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
  }
  return true;
}
} // namespace swift_once_source_detail

inline SwiftOnceSourcePlan
discoverSwiftOnceSources(const BinaryImage &Image,
                         const PipelineResult &Result) {
  using namespace swift_once_source_detail;
  SwiftOnceSourcePlan Plan;
  if (Result.SourceImage != &Image || Image.Format != BinaryFormat::MachO ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return Plan;
  std::set<va_t> DirectTargets;
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &F : Result.HighFuncs)
    Functions.emplace(F.Entry, &F);
  for (const auto &F : Result.LowFuncs)
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst())
          DirectTargets.insert(Op.Inputs[0].Offset);
  for (const auto &F : Result.HighFuncs)
    if (auto Contract = objcClassMetadataAccessorContract(F, Image))
      Plan.ObjCClassMetadataAccessors.emplace(F.Entry, *Contract);
    else if (auto Contract = getterContract(F, Image))
      Plan.Getters.emplace(F.Entry, *Contract);
    else if (auto Contract = addressorContract(F, Image)) {
      Plan.Addressors.emplace(F.Entry, *Contract);
      Plan.AddressorHints.emplace(F.Entry, addressorHint(Image.Arch));
    }
  for (const auto &[_, Contract] : Plan.Addressors)
    if (Functions.count(Contract.Initializer) &&
        !DirectTargets.count(Contract.Initializer))
      Plan.CallbackHints.emplace(Contract.Initializer,
                                 callbackHint(Image.Arch));
  for (const auto &F : Result.HighFuncs) {
    if (!F.SourceTypeHint)
      continue;
    walkStmts(F.Body, [&](const HighStmt &S) {
      forEachRhsExpr(S, [&](const ExprPtr &Root) {
        std::vector<ExprPtr> Pending{Root};
        size_t Budget = 100000;
        while (!Pending.empty() && Budget--) {
          auto E = Pending.back();
          Pending.pop_back();
          if (!E)
            continue;
          if (E->Kind == ExprKind::Call && E->SourceCallHint &&
              E->SourceCallHint->CallKind == SourceCallTypeHint::Kind::Native &&
              objcSourceCallBound(*E, Image, Functions)) {
            auto Getter = Plan.Getters.find(E->SourceCallHint->TargetAddress);
            if (Getter != Plan.Getters.end() &&
                E->Operands.size() == Getter->second.parameterCount() &&
                std::all_of(
                    E->Operands.begin(), E->Operands.end(),
                    [](const ExprPtr &Operand) { return bool(Operand); })) {
              auto Target = objc_binding_detail::constantAddress(
                  *E->Operands[Getter->second.Initializer]);
              if (Target && Functions.count(*Target) &&
                  Image.isCodeAddress(*Target) && !DirectTargets.count(*Target))
                Plan.CallbackHints.emplace(*Target, callbackHint(Image.Arch));
            }
          }
          Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
        }
      });
    });
  }
  return Plan;
}

inline size_t applySwiftOnceSourceHints(const SwiftOnceSourcePlan &Plan,
                                        PipelineOptions &Options) {
  size_t Added = 0;
  for (const auto &[Address, Hint] : Plan.AddressorHints)
    Added += Options.SourceCalleeTypeHints.emplace(Address, Hint).second;
  for (const auto &[Address, Hint] : Plan.CallbackHints)
    Added += Options.SourceTypeHints.emplace(Address, Hint).second;
  return Added;
}

inline std::string swiftOnceInitializerName(va_t Address) {
  return "neverd_swift_once_initializer_" +
         llvm::utohexstr(Address, /*LowerCase=*/true);
}

inline std::string swiftOnceAccessorName(va_t Address) {
  return "neverd_swift_once_accessor_" +
         llvm::utohexstr(Address, /*LowerCase=*/true);
}

inline std::optional<SourceCallTypeHint>
swiftOnceAddressorCallHint(const HighFunc &Function, const BinaryImage &Image) {
  if (!swift_once_source_detail::addressorContract(Function, Image))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeSwiftOnceAccessor;
  Hint.TargetAddress = Function.Entry;
  Hint.TargetName = Function.Name;
  Hint.Signature = swift_once_source_detail::addressorHint(Image.Arch);
  return Hint;
}

inline bool
swiftOnceAddressorBound(const HighExpr &E, const BinaryImage &Image,
                        const SwiftOnceSourcePlan &Plan,
                        const std::map<va_t, const HighFunc *> &Functions) {
  if (E.Kind != ExprKind::Call || !E.SourceCallHint || E.IsIndirectCall ||
      !E.Operands.empty() || E.IntrinsicId != Intrinsic::None ||
      !E.IntrinsicOutputs.empty() || !E.CallTarget.empty() || !E.Type ||
      E.Type->Kind != NdTypeKind::Ptr || E.Type->Size != 8 ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      E.SourceCallHint->CallKind !=
          SourceCallTypeHint::Kind::RuntimeSwiftOnceAccessor)
    return false;
  const auto &Binding = *E.SourceCallHint;
  const auto Planned = Plan.Addressors.find(Binding.TargetAddress);
  const auto Function = Functions.find(Binding.TargetAddress);
  if (Planned == Plan.Addressors.end() || Function == Functions.end() ||
      !Function->second || E.CallAddr != Binding.TargetAddress)
    return false;
  const auto Current =
      swift_once_source_detail::addressorContract(*Function->second, Image);
  const auto Expected = swiftOnceAddressorCallHint(*Function->second, Image);
  if (!Current || !(*Current == Planned->second) || !Expected ||
      Binding.TargetName != Expected->TargetName || Binding.DoesNotReturn ||
      Binding.WeakImport || Binding.ReturnedArgument ||
      Binding.RuntimeObjCResultType || Binding.ValueWitness ||
      !Binding.Selector.empty() || !Binding.OwnerClass.empty() ||
      Binding.SelectorReferenceAddress || Binding.Format || Binding.Receiver ||
      Binding.SelectorResultUse || Binding.SelectorResultTypeUse ||
      Binding.SelectorArgumentTypeUse || Binding.SelectorArgumentStorageUse ||
      Binding.ByteCount || Binding.ImmutablePointerSlot ||
      Binding.SwiftTypeMetadata || !Binding.BorrowedByteInputs.empty() ||
      !Binding.SwiftStringInputs.empty() ||
      !objc_projection_detail::sameHint(Binding.Signature, Expected->Signature))
    return false;
  const auto Callback = Functions.find(Current->Initializer);
  const auto CallbackHint = Plan.CallbackHints.find(Current->Initializer);
  return Callback != Functions.end() && Callback->second &&
         CallbackHint != Plan.CallbackHints.end() &&
         Callback->second->SourceTypeHint &&
         objc_projection_detail::sameHint(*Callback->second->SourceTypeHint,
                                          CallbackHint->second) &&
         swift_once_source_detail::ignoresContext(*Callback->second);
}

inline bool
swiftOnceCallbackBound(const HighExpr &E, const BinaryImage &Image,
                       const SwiftOnceSourcePlan &Plan,
                       const std::map<va_t, const HighFunc *> &Functions) {
  if (E.Kind != ExprKind::Call || !E.SourceCallHint || E.IsIndirectCall ||
      !E.Operands.empty() || E.IntrinsicId != Intrinsic::None ||
      !E.IntrinsicOutputs.empty() || E.CallAddr != 0 || !E.CallTarget.empty() ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      E.SourceCallHint->CallKind != SourceCallTypeHint::Kind::NativeAddress)
    return false;
  const auto &Binding = *E.SourceCallHint;
  auto Expected = Plan.CallbackHints.find(Binding.TargetAddress);
  auto F = Functions.find(Binding.TargetAddress);
  if (Expected == Plan.CallbackHints.end() || F == Functions.end() ||
      !F->second->SourceTypeHint || !Binding.TargetName.empty() ||
      Binding.DoesNotReturn || Binding.WeakImport ||
      !Binding.Selector.empty() || !Binding.OwnerClass.empty() ||
      Binding.ByteCount || Binding.ImmutablePointerSlot ||
      Binding.SelectorReferenceAddress || Binding.Receiver || Binding.Format ||
      Binding.SelectorResultUse || Binding.SelectorResultTypeUse ||
      Binding.SelectorArgumentTypeUse || Binding.SelectorArgumentStorageUse ||
      Binding.ValueWitness || Binding.ReturnedArgument ||
      Binding.RuntimeObjCResultType || !Binding.BorrowedByteInputs.empty() ||
      !Binding.SwiftStringInputs.empty() ||
      !objc_projection_detail::sameHint(*F->second->SourceTypeHint,
                                        Expected->second))
    return false;
  SourceFunctionTypeHint Address;
  Address.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  return assignDarwinScalarSourceABI(Address, Image.Arch, Error) &&
         objc_projection_detail::sameHint(Binding.Signature, Address) &&
         swift_once_source_detail::ignoresContext(*F->second);
}

inline ObjCSourceBindingResult bindSwiftOnceSourceReferences(
    const HighFunc &Function, const BinaryImage &Image,
    const SwiftOnceSourcePlan &Plan,
    const std::map<va_t, const HighFunc *> &Functions) {
  ObjCSourceBindingResult Result{Function};
  size_t Budget = 100000;
  std::map<const HighExpr *, ExprPtr> Copies;
  std::function<ExprPtr(const ExprPtr &, unsigned)> Copy =
      [&](const ExprPtr &Original, unsigned Depth) -> ExprPtr {
    if (!Original || !Budget || Depth > 128)
      return Original;
    if (auto Found = Copies.find(Original.get()); Found != Copies.end())
      return Found->second;
    --Budget;
    auto E = std::make_shared<HighExpr>(*Original);
    Copies.emplace(Original.get(), E);
    for (auto &Op : E->Operands)
      Op = Copy(Op, Depth + 1);
    if (E->Kind == ExprKind::Call && !E->IsIndirectCall &&
        E->Operands.size() <= 1 && E->SourceCallHint &&
        E->SourceCallHint->CallKind == SourceCallTypeHint::Kind::Native &&
        E->SourceCallHint->TargetAddress && E->IntrinsicId == Intrinsic::None &&
        E->IntrinsicOutputs.empty() &&
        E->MemoryOrdering == NdMemoryOrdering::None &&
        E->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      const auto Target = E->SourceCallHint->TargetAddress;
      const auto Planned = Plan.ObjCClassMetadataAccessors.find(Target);
      const auto Function = Functions.find(Target);
      const auto Current =
          Function == Functions.end() || !Function->second
              ? std::nullopt
              : swift_once_source_detail::objcClassMetadataAccessorContract(
                    *Function->second, Image);
      const auto Request =
          E->Operands.empty()
              ? std::optional<va_t>{0}
              : objc_binding_detail::constantAddress(*E->Operands.front());
      const bool NativeCarrier =
          Function != Functions.end() && Function->second &&
          objc_binding_detail::plainNativeBinding(*E->SourceCallHint) &&
          (!E->CallAddr || E->CallAddr == Target) &&
          (E->SourceCallHint->TargetName.empty() ||
           E->SourceCallHint->TargetName == Function->second->Name) &&
          (E->CallTarget.empty() || E->CallTarget == Function->second->Name) &&
          Function->second->SourceTypeHint &&
          objc_projection_detail::sameHint(E->SourceCallHint->Signature,
                                           *Function->second->SourceTypeHint);
      if (Planned != Plan.ObjCClassMetadataAccessors.end() && Current &&
          *Current == Planned->second && Request && !*Request &&
          NativeCarrier) {
        const auto Runtime =
            swiftRuntimeSourceCallHint(Image, Planned->second.MetadataRuntime);
        const auto Reference =
            Image.ObjCSourceReferences.find(Planned->second.ClassReference);
        if (Runtime && Runtime->TargetName == "swift_getObjCClassMetadata" &&
            Reference != Image.ObjCSourceReferences.end() &&
            Reference->second.TheKind == ObjCSourceReference::Kind::Class &&
            Reference->second.Name == Planned->second.ClassName) {
          auto Class = HighExpr::makeCall({}, 0, {});
          auto ClassHint = std::make_shared<SourceCallTypeHint>();
          ClassHint->CallKind = SourceCallTypeHint::Kind::RuntimeClass;
          ClassHint->TargetAddress = Planned->second.ClassReference;
          ClassHint->TargetName = Planned->second.ClassName;
          ClassHint->OwnerClass = Reference->second.ClassName;
          ClassHint->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
          std::string Error;
          if (assignDarwinScalarSourceABI(ClassHint->Signature, Image.Arch,
                                          Error)) {
            Class->Type = ClassHint->Signature.ReturnType;
            Class->SourceCallHint = std::move(ClassHint);
            auto Replacement = HighExpr::makeCall(
                Runtime->TargetName, Runtime->TargetAddress, {Class});
            Replacement->Type =
                E->Type ? E->Type : Runtime->Signature.ReturnType;
            Replacement->SourceCallHint =
                std::make_shared<SourceCallTypeHint>(*Runtime);
            return Replacement;
          }
        }
      }
    }
    const auto NativeAddressorCarrier = [&]() {
      if (!E->SourceCallHint)
        return true;
      const auto Function = Functions.find(E->CallAddr);
      const auto Expected =
          Function == Functions.end() || !Function->second
              ? std::nullopt
              : swiftOnceAddressorCallHint(*Function->second, Image);
      const auto &Binding = *E->SourceCallHint;
      return Expected && objc_binding_detail::plainNativeBinding(Binding) &&
             Binding.TargetAddress == E->CallAddr &&
             (Binding.TargetName.empty() ||
              Binding.TargetName == Function->second->Name) &&
             (E->CallTarget.empty() ||
              E->CallTarget == Function->second->Name) &&
             objc_projection_detail::sameHint(Binding.Signature,
                                              Expected->Signature);
    };
    if (E->Kind == ExprKind::Call && !E->IsIndirectCall &&
        E->Operands.empty() && E->CallAddr &&
        E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty() &&
        E->MemoryOrdering == NdMemoryOrdering::None &&
        E->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        NativeAddressorCarrier()) {
      const auto Planned = Plan.Addressors.find(E->CallAddr);
      const auto Function = Functions.find(E->CallAddr);
      const auto Expected =
          Function == Functions.end() || !Function->second
              ? std::nullopt
              : swiftOnceAddressorCallHint(*Function->second, Image);
      const auto Callback = Planned == Plan.Addressors.end()
                                ? Functions.end()
                                : Functions.find(Planned->second.Initializer);
      const auto CallbackHint =
          Planned == Plan.Addressors.end()
              ? Plan.CallbackHints.end()
              : Plan.CallbackHints.find(Planned->second.Initializer);
      if (Planned != Plan.Addressors.end() && Expected &&
          Callback != Functions.end() && Callback->second &&
          CallbackHint != Plan.CallbackHints.end() &&
          Callback->second->SourceTypeHint &&
          objc_projection_detail::sameHint(*Callback->second->SourceTypeHint,
                                           CallbackHint->second) &&
          swift_once_source_detail::ignoresContext(*Callback->second) &&
          (!E->Type ||
           (E->Type->Size == 8 && (E->Type->Kind == NdTypeKind::Int ||
                                   E->Type->Kind == NdTypeKind::Ptr)))) {
        E->CallTarget.clear();
        E->SourceCallHint =
            std::make_shared<SourceCallTypeHint>(std::move(*Expected));
        E->Type = E->SourceCallHint->Signature.ReturnType;
        Result.Dependencies.insert(Planned->second.Initializer);
        Result.LocalStorageExtents[Planned->second.Predicate] = 8;
        Result.LocalStorageExtents[Planned->second.Storage] = 8;
        Result.SwiftOnceAccessors.insert(E->CallAddr);
        return E;
      }
    }
    if (E->Kind != ExprKind::Call || !E->SourceCallHint || E->IsIndirectCall ||
        E->SourceCallHint->CallKind != SourceCallTypeHint::Kind::Native ||
        E->IntrinsicId != Intrinsic::None ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return E;
    auto Getter = Plan.Getters.find(E->SourceCallHint->TargetAddress);
    if (Getter == Plan.Getters.end() ||
        E->Operands.size() != Getter->second.parameterCount() ||
        !std::all_of(E->Operands.begin(), E->Operands.end(),
                     [](const ExprPtr &Operand) { return bool(Operand); }) ||
        !objcSourceCallBound(*E, Image, Functions))
      return E;
    const auto &C = Getter->second;
    const auto P =
        objc_binding_detail::constantAddress(*E->Operands[C.Predicate]);
    const auto S =
        objc_binding_detail::constantAddress(*E->Operands[C.Storage]);
    const auto S2 = C.SecondStorage ? objc_binding_detail::constantAddress(
                                          *E->Operands[*C.SecondStorage])
                                    : std::optional<va_t>{};
    const auto I =
        objc_binding_detail::constantAddress(*E->Operands[C.Initializer]);
    const uint64_t StorageWidth = C.storageWidth();
    if (!P || !S || !I || (C.SecondStorage && !S2) ||
        (S2 && (*S > InvalidVA - 8 || *S2 != *S + 8)) ||
        (*P <= *S ? *P > InvalidVA - 8 || *P + 8 > *S
                  : *S > InvalidVA - StorageWidth || *S + StorageWidth > *P))
      return E;
    auto Predicate = objc_binding_detail::oncePredicateStorageHint(Image, *P);
    auto Storage =
        objc_binding_detail::localStorageHint(Image, *S, StorageWidth);
    if (!Predicate || !Storage)
      return E;
    auto Address = HighExpr::makeCall({}, 0, {});
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->CallKind = SourceCallTypeHint::Kind::NativeAddress;
    Hint->TargetAddress = *I;
    Hint->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
    std::string Error;
    if (!assignDarwinScalarSourceABI(Hint->Signature, Image.Arch, Error))
      return E;
    Address->Type = Hint->Signature.ReturnType;
    Address->SourceCallHint = Hint;
    if (!swiftOnceCallbackBound(*Address, Image, Plan, Functions))
      return E;
    E->Operands[C.Initializer] = Address;
    auto BindStorage = [&](size_t Index, const SourceCallTypeHint &Binding,
                           uint64_t Offset) {
      auto Value = HighExpr::makeCall({}, 0, {});
      Value->Type = E->Operands[Index]->Type;
      Value->SourceCallHint = std::make_shared<SourceCallTypeHint>(Binding);
      E->Operands[Index] =
          Offset ? HighExpr::makeBinop(
                       NdOp::INT_ADD, Value,
                       HighExpr::makeConst(Offset, 8,
                                           ConstantAddressProvenance::Scalar))
                 : std::move(Value);
    };
    BindStorage(C.Predicate, *Predicate, 0);
    BindStorage(C.Storage, *Storage, 0);
    if (C.SecondStorage)
      BindStorage(*C.SecondStorage, *Storage, 8);
    Result.LocalStorageExtents[Predicate->TargetAddress] = 8;
    Result.LocalStorageExtents[Storage->TargetAddress] = StorageWidth;
    Result.Dependencies.insert(*I);
    return E;
  };
  walkStmts(Result.Function.Body, [&](HighStmt &S) {
    forEachExpr(S, [&](ExprPtr &E) { E = Copy(E, 0); });
  });
  const bool IsAddressorInitializer = std::any_of(
      Plan.Addressors.begin(), Plan.Addressors.end(), [&](const auto &Entry) {
        return Entry.second.Initializer == Function.Entry;
      });
  if (IsAddressorInitializer) {
    Result.Function.Name = swiftOnceInitializerName(Function.Entry);
    Result.Function.DebugName.clear();
    Result.Function.SourceFile.clear();
  }
  return Result;
}

inline std::string renderSwiftOnceAddressorHelpers(
    const BinaryImage &Image, const std::set<va_t> &Accessors,
    const SwiftOnceSourcePlan &Plan,
    const std::map<va_t, const HighFunc *> &Functions,
    std::set<std::string> &SharedFunctions) {
  std::string Source;
  if (!Accessors.empty())
    Source +=
        "\nextern void neverd_swift_once(void *, void (*)(void *), void *) "
        "__asm__(\"_swift_once\");\n";
  for (const va_t Address : Accessors) {
    const auto Planned = Plan.Addressors.find(Address);
    const auto Function = Functions.find(Address);
    if (Planned == Plan.Addressors.end() || Function == Functions.end() ||
        !Function->second)
      throw std::runtime_error("Swift once addressor is no longer valid");
    const auto Current =
        swift_once_source_detail::addressorContract(*Function->second, Image);
    const auto Callback = Functions.find(Planned->second.Initializer);
    const auto CallbackHint =
        Plan.CallbackHints.find(Planned->second.Initializer);
    if (!Current || !(*Current == Planned->second) ||
        Callback == Functions.end() || !Callback->second ||
        CallbackHint == Plan.CallbackHints.end() ||
        !Callback->second->SourceTypeHint ||
        !objc_projection_detail::sameHint(*Callback->second->SourceTypeHint,
                                          CallbackHint->second) ||
        !swift_once_source_detail::ignoresContext(*Callback->second))
      throw std::runtime_error("Swift once addressor is no longer valid");

    const std::string Predicate =
        "neverd_local_storage_" +
        llvm::utohexstr(Current->Predicate, /*LowerCase=*/true) + "_address";
    const std::string Storage =
        "neverd_local_storage_" +
        llvm::utohexstr(Current->Storage, /*LowerCase=*/true) + "_address";
    const std::string Initializer =
        swiftOnceInitializerName(Current->Initializer);
    const std::string Accessor = swiftOnceAccessorName(Address);
    SharedFunctions.insert(Accessor);
    Source += "\nextern uintptr_t " + Predicate +
              "(void);\n"
              "extern uintptr_t " +
              Storage +
              "(void);\n"
              "extern void " +
              Initializer +
              "(void *);\n"
              "uintptr_t " +
              Accessor +
              "(void) {\n"
              "  intptr_t *predicate = (intptr_t *)(uintptr_t)" +
              Predicate +
              "();\n"
              "  if (__atomic_load_n(predicate, __ATOMIC_ACQUIRE) != -1)\n"
              "    neverd_swift_once(predicate, &" +
              Initializer +
              ", predicate);\n"
              "  return " +
              Storage + "();\n}\n";
  }
  return Source;
}
} // namespace neverd::sdk
#endif
