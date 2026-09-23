#ifndef NEVERD_SDK_CAPI_OBJCSWIFTONCESOURCES_H
#define NEVERD_SDK_CAPI_OBJCSWIFTONCESOURCES_H

#include "ObjCSourceBindings.h"

#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

#include <array>
#include <tuple>

namespace neverd::sdk {

struct SwiftOnceGetterContract {
  size_t Predicate = 0;
  size_t Storage = 0;
  std::optional<size_t> SecondStorage;
  size_t Initializer = 0;
  size_t ParameterCount = 0;

  size_t parameterCount() const { return ParameterCount; }
  uint64_t storageWidth() const { return SecondStorage ? 16 : 8; }
};

struct SwiftOnceCopyContract {
  size_t ParameterCount = 0;
  size_t Predicate = 0;
  size_t Source = 0;
  size_t Destination = 0;
  size_t Initializer = 0;

  bool operator==(const SwiftOnceCopyContract &Other) const {
    return ParameterCount == Other.ParameterCount &&
           Predicate == Other.Predicate && Source == Other.Source &&
           Destination == Other.Destination && Initializer == Other.Initializer;
  }
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

struct SwiftOnceObjCThunkContract {
  va_t Predicate = 0;
  std::optional<va_t> Storage;
  va_t Initializer = 0;
  SourceFunctionTypeHint Signature;

  bool sameIdentity(const SwiftOnceObjCThunkContract &Other) const {
    return Predicate == Other.Predicate && Storage == Other.Storage &&
           Initializer == Other.Initializer &&
           objc_projection_detail::sameHint(Signature, Other.Signature);
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
  std::map<va_t, SwiftOnceCopyContract> Copies;
  std::map<va_t, SwiftOnceAddressorContract> Addressors;
  std::map<va_t, SwiftOnceObjCThunkContract> ObjCThunks;
  std::map<va_t, SwiftOnceObjCThunkContract> NestedCallbacks;
  std::map<va_t, SwiftObjCClassMetadataAccessorContract>
      ObjCClassMetadataAccessors;
  std::map<va_t, SourceFunctionTypeHint> AddressorHints;
  std::map<va_t, SourceFunctionTypeHint> CallbackHints;
  std::set<va_t> DispatchOnceCallbacks;
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

inline bool unknownScalarView(const ExprPtr &Value) {
  auto E = Value;
  for (unsigned Depth = 0; E && Depth < 16; ++Depth) {
    if (E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default || !E->Type ||
        !E->Type->Size || E->Type->Size > 8)
      return false;
    if (E->Kind == ExprKind::Undef)
      return E->Operands.empty();
    if (E->Kind != ExprKind::Cast || E->Operands.size() != 1)
      return false;
    E = E->Operands.front();
  }
  return false;
}

inline bool projectedOnceContextUnused(const HighFunc &Function,
                                       size_t Parameter) {
  if (!Function.SourceTypeHint || Parameter >= Function.Params.size() ||
      Parameter >= Function.SourceTypeHint->Parameters.size())
    return false;
  bool Unused = true;
  size_t Budget = 100000;
  std::set<const HighExpr *> Seen;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Root) {
      std::vector<const HighExpr *> Pending{Root.get()};
      while (Unused && !Pending.empty() && Budget) {
        --Budget;
        const auto *E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E).second)
          continue;
        if (((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
             E->Var.Kind == MedVar::Param && E->Var.Id >= 0 &&
             static_cast<size_t>(E->Var.Id) == Parameter) ||
            std::any_of(E->IntrinsicOutputs.begin(), E->IntrinsicOutputs.end(),
                        [&](const MedVar &Output) {
                          return Output.Kind == MedVar::Param &&
                                 Output.Id >= 0 &&
                                 static_cast<size_t>(Output.Id) == Parameter;
                        }))
          Unused = false;
        for (const auto &Operand : E->Operands)
          Pending.push_back(Operand.get());
      }
      if (!Pending.empty())
        Unused = false;
    });
  });
  return Unused && Budget;
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

inline bool dispatchOnceCall(const HighExpr &E, const BinaryImage &Image) {
  if (E.Kind != ExprKind::Call || E.IsIndirectCall || !E.SourceCallHint ||
      E.Operands.size() != 3 || E.IntrinsicId != Intrinsic::None ||
      !E.IntrinsicOutputs.empty() ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto &Hint = *E.SourceCallHint;
  if (Hint.CallKind != SourceCallTypeHint::Kind::DarwinRuntimeCall ||
      Hint.TargetName != "dispatch_once_f" || Hint.Receiver ||
      Hint.ValueWitness || Hint.ImmutablePointerSlot)
    return false;
  const auto Expected = darwinRuntimeSourceCallHint(Image, Hint.TargetAddress);
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
      (F.Params.size() != 3 && F.Params.size() != 4 &&
       F.Params.size() != 6) ||
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
      if ((F.Params.size() != 4 && F.Params.size() != 6) ||
          StringStorageParameters || !First ||
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
                                   Initializer, F.Params.size()};
  }
  if (!StringStorageParameters)
    return std::nullopt;
  const auto [Storage, SecondStorage] = *StringStorageParameters;
  if (Predicate == Storage || Predicate == SecondStorage ||
      Initializer == Storage || Initializer == SecondStorage ||
      Reads != std::set<size_t>{Predicate, Storage, SecondStorage})
    return std::nullopt;
  // Some shared Swift thunks preserve two incoming ObjC registers ahead of
  // the four real getter inputs. They are admissible only when the complete
  // flow above proves both leading parameters unused and the suffix retains
  // the canonical predicate/storage/initializer order.
  if (F.Params.size() == 6 &&
      (Predicate != 2 || Storage != 3 || SecondStorage != 4 ||
       Initializer != 5))
    return std::nullopt;
  return SwiftOnceGetterContract{Predicate, Storage, SecondStorage,
                                 Initializer, F.Params.size()};
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

/// Prove the Objective-C entry thunk emitted for a Swift lazy static object
/// property. The machine thunk forwards the otherwise unspecified third
/// Objective-C argument register as swift_once context, but the exact
/// initializer ignores that context. A source projection may therefore pass
/// null and retain only the declared receiver and selector parameters.
inline std::optional<SwiftOnceObjCThunkContract>
objcGetterThunkContract(const HighFunc &F, const BinaryImage &Image) {
  const ObjCMethod *Method = nullptr;
  for (const auto &Candidate : Image.ObjCMethods) {
    if (Candidate.Implementation != F.Entry)
      continue;
    if (Method || Candidate.Status != "supported" || !Candidate.TypeHint)
      return std::nullopt;
    Method = &Candidate;
  }
  const bool SeparateRetain = F.Body.size() == 7;
  if (!Method || F.Params.size() != 3 || !F.ReturnType ||
      F.ReturnType->Size != 8 || (F.Body.size() != 6 && !SeparateRetain))
    return std::nullopt;
  const auto Parameters = sourceABIParameters(*Method->TypeHint);
  if (Parameters.size() != 2 || !Method->TypeHint->ReturnType ||
      Method->TypeHint->ReturnType->Kind != NdTypeKind::Ptr ||
      Method->TypeHint->ReturnType->Size != 8)
    return std::nullopt;
  for (const auto &Parameter : Parameters)
    if (!Parameter.Type || Parameter.Type->Kind != NdTypeKind::Ptr ||
        Parameter.Type->Size != 8)
      return std::nullopt;
  for (const auto &Parameter : F.Params)
    if (!Parameter.Type || Parameter.Type->Size != 8 ||
        (Parameter.Type->Kind != NdTypeKind::Int &&
         Parameter.Type->Kind != NdTypeKind::Ptr))
      return std::nullopt;

  size_t ThunkSymbols = 0;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc && Symbol.Addr == F.Entry && Symbol.Name == F.Name &&
        llvm::StringRef(Symbol.Name).ends_with("vgZTo"))
      ++ThunkSymbols;
  if (ThunkSymbols != 1)
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
  const auto Assignment = [&](size_t I) -> const HighStmt * {
    const auto &Statement = F.Body[I];
    if (Statement.Kind != StmtKind::Assign || !Statement.Dst ||
        !Statement.Val ||
        (Statement.Dst->Kind != ExprKind::Var &&
         Statement.Dst->Kind != ExprKind::Phi) ||
        !Statement.Dst->Operands.empty())
      return nullptr;
    return &Statement;
  };
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
  const auto IsConstant = [&](const ExprPtr &Value, uint64_t Expected) {
    const auto E = Plain(Value);
    return E && E->Kind == ExprKind::Const && E->ConstVal == Expected;
  };

  const auto *PredicateAssignment = Assignment(0);
  const auto *StorageAssignment = Assignment(3);
  const auto *RetainAssignment = Assignment(4);
  const auto *ResultAssignment = Assignment(SeparateRetain ? 5 : 4);
  const auto &Branch = F.Body[1];
  const auto &Label = F.Body[2];
  const auto &Return = F.Body[SeparateRetain ? 6 : 5];
  const size_t OnceIndex = Branch.Body.size() == 3 ? 1 : 0;
  if (OnceIndex) {
    const auto &EntryLabel = Branch.Body.front();
    bool HasExpression = false;
    forEachExpr(EntryLabel, [&](const ExprPtr &) { HasExpression = true; });
    if (EntryLabel.Kind != StmtKind::Block || HasExpression ||
        !EntryLabel.Body.empty() || !EntryLabel.ElseBody.empty() ||
        !EntryLabel.Cases.empty() || !EntryLabel.DefaultBody.empty() ||
        !EntryLabel.EHClauseBodies.empty() || EntryLabel.GotoTarget ||
        EntryLabel.MemoryOrdering != NdMemoryOrdering::None ||
        EntryLabel.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
  }
  if (!PredicateAssignment || !StorageAssignment || !RetainAssignment ||
      !ResultAssignment || Branch.Kind != StmtKind::If || !Branch.Cond ||
      !Branch.ElseBody.empty() || Branch.Body.size() != OnceIndex + 2 ||
      Branch.Body[OnceIndex].Kind != StmtKind::Call ||
      !Branch.Body[OnceIndex].CallExpr ||
      Branch.Body[OnceIndex + 1].Kind != StmtKind::Goto ||
      !Branch.Body[OnceIndex + 1].GotoTarget || Label.Kind != StmtKind::Block ||
      Label.Addr != Branch.Body[OnceIndex + 1].GotoTarget ||
      !Label.Body.empty() || !Label.ElseBody.empty() ||
      Return.Kind != StmtKind::Return || !Return.RetVal ||
      !SameLocal(Return.RetVal, ResultAssignment->Dst))
    return std::nullopt;

  const auto Predicate = LoadAddress(PredicateAssignment->Val);
  const auto Storage = LoadAddress(StorageAssignment->Val);
  if (!Predicate || !Storage || *Predicate == *Storage)
    return std::nullopt;
  auto Condition = Plain(Branch.Cond);
  if (!Condition || Condition->Kind != ExprKind::BinOp ||
      Condition->Op != NdOp::INT_NOTEQUAL || Condition->Operands.size() != 2)
    return std::nullopt;
  ExprPtr Added;
  if (IsConstant(Condition->Operands[0], 0))
    Added = Plain(Condition->Operands[1]);
  else if (IsConstant(Condition->Operands[1], 0))
    Added = Plain(Condition->Operands[0]);
  if (!Added || Added->Kind != ExprKind::BinOp || Added->Op != NdOp::INT_ADD ||
      Added->Operands.size() != 2)
    return std::nullopt;
  ExprPtr Loaded;
  if (IsConstant(Added->Operands[0], 1))
    Loaded = Plain(Added->Operands[1]);
  else if (IsConstant(Added->Operands[1], 1))
    Loaded = Plain(Added->Operands[0]);
  if (!SameLocal(Loaded, PredicateAssignment->Dst))
    return std::nullopt;

  const auto &Once = *Branch.Body[OnceIndex].CallExpr;
  if (!onceCall(Once, Image))
    return std::nullopt;
  const auto OncePredicate =
      objc_binding_detail::constantAddress(*Once.Operands[0]);
  const auto Initializer =
      objc_binding_detail::constantAddress(*Once.Operands[1]);
  const auto Context = parameter(Once.Operands[2]);
  if (!OncePredicate || *OncePredicate != *Predicate || !Initializer ||
      !Context || *Context != 2 || !Image.isCodeAddress(*Initializer))
    return std::nullopt;

  const auto Retain = Plain(RetainAssignment->Val);
  if (!Retain || Retain->Kind != ExprKind::Call || Retain->IsIndirectCall ||
      Retain->Operands.size() != 1 || !Retain->SourceCallHint ||
      Retain->SourceCallHint->CallKind !=
          (SeparateRetain ? SourceCallTypeHint::Kind::SwiftRuntimeCall
                          : SourceCallTypeHint::Kind::ObjCRuntimeCall) ||
      Retain->SourceCallHint->TargetName !=
          (SeparateRetain ? "swift_retain"
                          : "objc_retainAutoreleaseReturnValue") ||
      (!SeparateRetain && Retain->SourceCallHint->ReturnedArgument != 0) ||
      !SameLocal(Retain->Operands[0], StorageAssignment->Dst))
    return std::nullopt;
  const auto ExpectedRetain =
      SeparateRetain
          ? swiftRuntimeSourceCallHint(Image,
                                       Retain->SourceCallHint->TargetAddress)
          : objcRuntimeSourceCallHint(Image,
                                      Retain->SourceCallHint->TargetAddress);
  if (!ExpectedRetain || !objc_binding_detail::runtimeBindingMatches(
                             *Retain->SourceCallHint, *ExpectedRetain))
    return std::nullopt;
  if (SeparateRetain) {
    // Native Swift objects use two operations. Preserve both calls and feed
    // the actual retain result to autorelease; no identity folding is needed.
    const auto Autorelease = Plain(ResultAssignment->Val);
    const auto SwiftBind =
        Image.DyldBindSlots.find(Retain->SourceCallHint->TargetAddress);
    if (SwiftBind == Image.DyldBindSlots.end() ||
        SwiftBind->second.Module != "/usr/lib/swift/libswiftCore.dylib" ||
        Retain->IntrinsicId != Intrinsic::None ||
        !Retain->IntrinsicOutputs.empty() ||
        Retain->MemoryOrdering != NdMemoryOrdering::None ||
        Retain->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
    if (!Autorelease || Autorelease->Kind != ExprKind::Call ||
        Autorelease->IsIndirectCall || Autorelease->Operands.size() != 1 ||
        !Autorelease->SourceCallHint ||
        Autorelease->SourceCallHint->CallKind !=
            SourceCallTypeHint::Kind::ObjCRuntimeCall ||
        Autorelease->SourceCallHint->TargetName !=
            "objc_autoreleaseReturnValue" ||
        Autorelease->IntrinsicId != Intrinsic::None ||
        !Autorelease->IntrinsicOutputs.empty() ||
        Autorelease->MemoryOrdering != NdMemoryOrdering::None ||
        Autorelease->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !SameLocal(Autorelease->Operands[0], RetainAssignment->Dst))
      return std::nullopt;
    const auto ObjCBind =
        Image.DyldBindSlots.find(Autorelease->SourceCallHint->TargetAddress);
    if (ObjCBind == Image.DyldBindSlots.end() ||
        ObjCBind->second.Module != "/usr/lib/libobjc.A.dylib")
      return std::nullopt;
    const auto Expected = objcRuntimeSourceCallHint(
        Image, Autorelease->SourceCallHint->TargetAddress);
    if (!Expected || !objc_binding_detail::runtimeBindingMatches(
                         *Autorelease->SourceCallHint, *Expected))
      return std::nullopt;
  }

  size_t ContextUses = 0;
  walkStmts(F.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty()) {
        auto Expression = Pending.back();
        Pending.pop_back();
        if (!Expression)
          continue;
        if (Expression->Kind == ExprKind::Var &&
            Expression->Var.Kind == MedVar::Param && Expression->Var.Id == 2)
          ++ContextUses;
        Pending.insert(Pending.end(), Expression->Operands.begin(),
                       Expression->Operands.end());
      }
    });
  });
  if (ContextUses != 1)
    return std::nullopt;

  const auto PredicateHint =
      objc_binding_detail::oncePredicateStorageHint(Image, *Predicate);
  const auto StorageHint =
      objc_binding_detail::localStorageHint(Image, *Storage, 8);
  if (!PredicateHint || !StorageHint ||
      !llvm::StringRef(PredicateHint->TargetName).ends_with("_Wz") ||
      !llvm::StringRef(StorageHint->TargetName).ends_with("vpZ"))
    return std::nullopt;
  const std::string Prefix =
      llvm::StringRef(PredicateHint->TargetName).drop_back(3).str();
  if (!llvm::StringRef(StorageHint->TargetName).starts_with(Prefix))
    return std::nullopt;
  bool ExactInitializer = false;
  for (const auto &Symbol : Image.Symbols)
    ExactInitializer |= Symbol.IsFunc && Symbol.Addr == *Initializer &&
                        Symbol.Name == Prefix + "_WZ";
  if (!ExactInitializer)
    return std::nullopt;

  return SwiftOnceObjCThunkContract{*Predicate, *Storage, *Initializer,
                                    *Method->TypeHint};
}

/// A Swift Objective-C constructor can forward an unspecified third argument
/// to swift_once while using its declared receiver normally. Only erase that
/// context after the exact initializer is independently proved not to read it.
/// The constructor's control flow, memory effects and other calls are retained
/// and must pass the ordinary source-body and dependency checks.
inline std::optional<SwiftOnceObjCThunkContract>
objcConstructorThunkContract(const HighFunc &F, const BinaryImage &Image) {
  const ObjCMethod *Method = nullptr;
  for (const auto &Candidate : Image.ObjCMethods) {
    if (Candidate.Implementation != F.Entry)
      continue;
    if (Method || Candidate.Status != "supported" || !Candidate.TypeHint)
      return std::nullopt;
    Method = &Candidate;
  }
  if (!Method || Method->IsClassMethod || Method->Selector != "init" ||
      F.SourceTypeHint || F.Params.size() != 3 || !F.ReturnType ||
      F.ReturnType->Size != 8 ||
      (F.ReturnType->Kind != NdTypeKind::Int &&
       F.ReturnType->Kind != NdTypeKind::Ptr))
    return std::nullopt;
  const auto Parameters = sourceABIParameters(*Method->TypeHint);
  if (Parameters.size() != 2 || !Method->TypeHint->ReturnType ||
      Method->TypeHint->ReturnType->Kind != NdTypeKind::Ptr ||
      Method->TypeHint->ReturnType->Size != 8)
    return std::nullopt;
  for (const auto &Parameter : Parameters)
    if (!Parameter.Type || Parameter.Type->Kind != NdTypeKind::Ptr ||
        Parameter.Type->Size != 8)
      return std::nullopt;
  for (const auto &Parameter : F.Params)
    if (!Parameter.Type || Parameter.Type->Size != 8 ||
        (Parameter.Type->Kind != NdTypeKind::Int &&
         Parameter.Type->Kind != NdTypeKind::Ptr))
      return std::nullopt;
  size_t ThunkSymbols = 0;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc && Symbol.Addr == F.Entry && Symbol.Name == F.Name &&
        llvm::StringRef(Symbol.Name).ends_with("cfcTo"))
      ++ThunkSymbols;
  if (ThunkSymbols != 1)
    return std::nullopt;

  const auto Flow = buildHighSourceFlowGraph(F);
  if (!Flow.Diagnostics.Complete || !Flow.Diagnostics.Items.empty())
    return std::nullopt;
  size_t Budget = 100000;
  size_t ContextUses = 0;
  const HighExpr *Once = nullptr;
  bool Valid = true;
  std::function<void(const ExprPtr &, unsigned)> Visit = [&](const ExprPtr &E,
                                                             unsigned Depth) {
    if (!Valid)
      return;
    if (!E || !Budget-- || Depth > 128 || E->Kind == ExprKind::Undef) {
      Valid = false;
      return;
    }
    if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Param) {
      if (E->Var.Id == 2)
        ++ContextUses;
      else if (E->Var.Id < 0 || E->Var.Id > 2)
        Valid = false;
    }
    if (onceCall(*E, Image)) {
      if (Once || parameter(E->Operands[2]) != std::optional<size_t>{2})
        Valid = false;
      Once = E.get();
    }
    for (const auto &Operand : E->Operands)
      Visit(Operand, Depth + 1);
  };
  walkStmts(F.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &E) { Visit(E, 0); });
  });
  if (!Valid || !Once || ContextUses != 1)
    return std::nullopt;
  const auto Predicate =
      objc_binding_detail::constantAddress(*Once->Operands[0]);
  const auto Initializer =
      objc_binding_detail::constantAddress(*Once->Operands[1]);
  const auto PredicateHint =
      Predicate
          ? objc_binding_detail::oncePredicateStorageHint(Image, *Predicate)
          : std::nullopt;
  if (!PredicateHint || !Initializer || !Image.isCodeAddress(*Initializer) ||
      !llvm::StringRef(PredicateHint->TargetName).ends_with("_Wz"))
    return std::nullopt;
  const auto InitializerName =
      llvm::StringRef(PredicateHint->TargetName).drop_back(3).str() + "_WZ";
  size_t InitializerSymbols = 0;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc && Symbol.Addr == *Initializer &&
        Symbol.Name == InitializerName)
      ++InitializerSymbols;
  if (InitializerSymbols != 1)
    return std::nullopt;
  return SwiftOnceObjCThunkContract{*Predicate, std::nullopt, *Initializer,
                                    *Method->TypeHint};
}

inline std::optional<SwiftOnceObjCThunkContract>
objcThunkContract(const HighFunc &F, const BinaryImage &Image) {
  if (auto Contract = objcGetterThunkContract(F, Image))
    return Contract;
  return objcConstructorThunkContract(F, Image);
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

inline SourceFunctionTypeHint dispatchCallbackHint(Arch Architecture) {
  auto Hint = callbackHint(Architecture);
  Hint.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
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

inline bool discardableCallbackReturn(ExprPtr Value) {
  if (!Value)
    return true;
  const auto Plain = [](const ExprPtr &E) {
    return E && E->Type && E->Type->Size == 8 &&
           (E->Type->Kind == NdTypeKind::Int ||
            E->Type->Kind == NdTypeKind::Ptr) &&
           E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty() &&
           E->MemoryOrdering == NdMemoryOrdering::None &&
           E->MemoryAddressSpace == NdMemoryAddressSpace::Default;
  };
  unsigned Depth = 0;
  while (Plain(Value) &&
         (Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast) &&
         Value->Operands.size() == 1 && Depth++ < 16)
    Value = Value->Operands.front();
  return Plain(Value) && Value->Operands.empty() &&
         (Value->Kind == ExprKind::Const ||
          (Value->Kind == ExprKind::Var && (Value->Var.Kind == MedVar::Reg ||
                                            Value->Var.Kind == MedVar::Temp)));
}

/// A callback already rooted by an authenticated once call can itself forward
/// an undeclared x2 to one nested once initializer. Its only entry use must be
/// that context; the independent leaf callback proof owns whether it is
/// ignored.
inline std::optional<SwiftOnceObjCThunkContract>
nestedCallbackContract(const HighFunc &F, const BinaryImage &Image) {
  if (Image.Arch != Arch::AArch64 || F.SourceTypeHint || F.Params.size() != 3 ||
      !F.ReturnType ||
      (F.ReturnType->Kind != NdTypeKind::Void &&
       ((F.ReturnType->Kind != NdTypeKind::Int &&
         F.ReturnType->Kind != NdTypeKind::Ptr) ||
        F.ReturnType->Size != 8)))
    return std::nullopt;
  for (const auto &P : F.Params)
    if (!P.Type || P.Type->Size != 8 ||
        (P.Type->Kind != NdTypeKind::Int && P.Type->Kind != NdTypeKind::Ptr))
      return std::nullopt;
  size_t Symbols = 0;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc && Symbol.Addr == F.Entry && Symbol.Name == F.Name &&
        llvm::StringRef(Symbol.Name).starts_with("_$s") &&
        llvm::StringRef(Symbol.Name).ends_with("_WZ"))
      ++Symbols;
  if (Symbols != 1)
    return std::nullopt;
  const auto Flow = buildHighSourceFlowGraph(F);
  if (!Flow.Diagnostics.Complete || !Flow.Diagnostics.Items.empty())
    return std::nullopt;
  size_t Budget = 100000;
  size_t ContextUses = 0;
  const HighExpr *Once = nullptr;
  bool Valid = true;
  std::function<void(const ExprPtr &, unsigned)> Visit = [&](const ExprPtr &E,
                                                             unsigned Depth) {
    if (!Valid)
      return;
    if (!E || !Budget-- || Depth > 128 || E->Kind == ExprKind::Undef) {
      Valid = false;
      return;
    }
    if ((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
        E->Var.Kind == MedVar::Param) {
      if (E->Var.Id == 2)
        ++ContextUses;
      else
        Valid = false;
    }
    if (onceCall(*E, Image)) {
      if (Once || parameter(E->Operands[2]) != std::optional<size_t>{2})
        Valid = false;
      Once = E.get();
    }
    for (const auto &Operand : E->Operands)
      Visit(Operand, Depth + 1);
  };
  walkStmts(F.Body, [&](const HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Return &&
        !discardableCallbackReturn(Statement.RetVal))
      Valid = false;
    forEachExpr(Statement, [&](const ExprPtr &E) { Visit(E, 0); });
  });
  if (!Valid || !Once || ContextUses != 1)
    return std::nullopt;
  const auto Predicate =
      objc_binding_detail::constantAddress(*Once->Operands[0]);
  const auto Initializer =
      objc_binding_detail::constantAddress(*Once->Operands[1]);
  const auto PredicateHint =
      Predicate
          ? objc_binding_detail::oncePredicateStorageHint(Image, *Predicate)
          : std::nullopt;
  if (!PredicateHint || !Initializer || !Image.isCodeAddress(*Initializer) ||
      !llvm::StringRef(PredicateHint->TargetName).ends_with("_Wz"))
    return std::nullopt;
  const auto InitializerName =
      llvm::StringRef(PredicateHint->TargetName).drop_back(3).str() + "_WZ";
  size_t InitializerSymbols = 0;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc && Symbol.Addr == *Initializer &&
        Symbol.Name == InitializerName)
      ++InitializerSymbols;
  if (InitializerSymbols != 1)
    return std::nullopt;
  return SwiftOnceObjCThunkContract{*Predicate, std::nullopt, *Initializer,
                                    callbackHint(Image.Arch)};
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
// This separate use contract never changes a helper's inferred native ABI.
// Four parameters supply the roles below; an optional fifth parameter must
// have no use. Native inference alone owns removing that unused entry input.
// This proof accounts for every remaining parameter and memory/call effect.
inline std::optional<SwiftOnceCopyContract>
copyContract(const HighFunc &F, const BinaryImage &Image) {
  const auto Plain8 = [](const ExprPtr &E) {
    return E && E->Type && E->Type->Size == 8 &&
           (E->Type->Kind == NdTypeKind::Int ||
            E->Type->Kind == NdTypeKind::Ptr) &&
           E->IntrinsicId == Intrinsic::None && E->IntrinsicOutputs.empty() &&
           E->MemoryOrdering == NdMemoryOrdering::None &&
           E->MemoryAddressSpace == NdMemoryAddressSpace::Default;
  };
  const auto IdentityCast = [&](const ExprPtr &E) {
    return Plain8(E) && E->Operands.size() == 1 && Plain8(E->Operands[0]) &&
           (!E->CastTo || equalSourceTypes(E->CastTo, E->Type));
  };
  if (Image.Arch != Arch::AArch64 || !F.SourceTypeHint ||
      F.SourceTypeHint->Architecture != Image.Arch ||
      F.SourceTypeHint->Origin !=
          SourceFunctionTypeHint::OriginKind::NativeAnalysis ||
      F.SourceTypeHint->Convention !=
          SourceFunctionTypeHint::ConventionKind::C ||
      (F.Params.size() != 4 && F.Params.size() != 5) ||
      F.SourceTypeHint->Parameters.size() != F.Params.size() || !F.ReturnType ||
      F.ReturnType->Size != 8 ||
      !equalSourceTypes(F.ReturnType, F.SourceTypeHint->ReturnType) ||
      (F.ReturnType->Kind != NdTypeKind::Int &&
       F.ReturnType->Kind != NdTypeKind::Ptr))
    return std::nullopt;
  std::string Error;
  if (!validateSourceABI(*F.SourceTypeHint, Error))
    return std::nullopt;
  for (size_t I = 0; I < F.Params.size(); ++I)
    if (!F.Params[I].Type || F.Params[I].Type->Size != 8 ||
        (F.Params[I].Type->Kind != NdTypeKind::Int &&
         F.Params[I].Type->Kind != NdTypeKind::Ptr) ||
        !equalSourceTypes(F.Params[I].Type,
                          F.SourceTypeHint->Parameters[I].Type))
      return std::nullopt;
  const auto Flow = buildHighSourceFlowGraph(F);
  if (!Flow.Diagnostics.Complete || !Flow.Diagnostics.Items.empty() ||
      Flow.Nodes.empty() || Flow.Nodes.size() > 512)
    return std::nullopt;
  using Local = HighSourceLocalIdentity;
  std::map<Local, std::vector<ExprPtr>> Definitions;
  bool Valid = true;
  const HighStmt *Store = nullptr;
  const HighExpr *Once = nullptr, *Retain = nullptr;
  size_t Budget = 10000;
  std::function<void(const ExprPtr &, unsigned)> FindCalls =
      [&](const ExprPtr &E, unsigned Depth) {
        if (!E || !Budget-- || Depth > 64) {
          Valid = false;
          return;
        }
        if (E->Kind == ExprKind::Call) {
          if (onceCall(*E, Image)) {
            if (Once)
              Valid = false;
            Once = E.get();
          } else {
            const auto Expected =
                E->SourceCallHint ? objcRuntimeSourceCallHint(
                                        Image, E->SourceCallHint->TargetAddress)
                                  : std::nullopt;
            if (Retain || !Expected || Expected->TargetName != "objc_retain" ||
                !E->SourceCallHint || E->IsIndirectCall ||
                E->Operands.size() != 1 ||
                !objc_binding_detail::runtimeBindingMatches(*E->SourceCallHint,
                                                            *Expected))
              Valid = false;
            Retain = E.get();
          }
        }
        for (const auto &Operand : E->Operands)
          FindCalls(Operand, Depth + 1);
      };
  walkStmts(F.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Store) {
      if (Store || S.MemoryOrdering != NdMemoryOrdering::None ||
          S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        Valid = false;
      Store = &S;
    }
    if (S.Kind == StmtKind::Assign) {
      if (!Plain8(S.Dst) || !S.Val || S.Dst->Kind != ExprKind::Var ||
          !S.Dst->Operands.empty() ||
          (S.Dst->Var.Kind != MedVar::Reg && S.Dst->Var.Kind != MedVar::Temp))
        Valid = false;
      else
        Definitions[highSourceLocalIdentity(S.Dst->Var)].push_back(S.Val);
    }
    forEachRhsExpr(S, [&](const ExprPtr &E) { FindCalls(E, 0); });
  });
  if (!Valid || !Store || !Once || !Retain)
    return std::nullopt;

  // A scalar local must have the same parameter or exact evaluated effect on
  // every definition. Distinct loads are never merged by their address.
  using Origin = std::pair<int64_t, const HighExpr *>;
  std::function<std::optional<Origin>(const ExprPtr &, unsigned,
                                      std::set<Local> &)>
      Resolve;
  Resolve = [&](const ExprPtr &E, unsigned Depth,
                std::set<Local> &Visiting) -> std::optional<Origin> {
    if (!Plain8(E) || Depth > 64)
      return std::nullopt;
    if (E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast)
      return IdentityCast(E) ? Resolve(E->Operands[0], Depth + 1, Visiting)
                             : std::nullopt;
    if (E->Kind == ExprKind::Var)
      if (const auto P = parameter(E))
        return Origin{int64_t(*P), nullptr};
    if (E->Kind == ExprKind::Load || E->Kind == ExprKind::Call)
      return Origin{-1, E.get()};
    if (E->Kind != ExprKind::Var && E->Kind != ExprKind::Phi)
      return std::nullopt;
    const std::vector<ExprPtr> *Values = &E->Operands;
    const auto Identity = highSourceLocalIdentity(E->Var);
    bool LocalDefinition = false;
    if (Values->empty()) {
      if (E->Var.Kind != MedVar::Reg && E->Var.Kind != MedVar::Temp)
        return std::nullopt;
      const auto Found = Definitions.find(Identity);
      if (Found == Definitions.end() || !Visiting.insert(Identity).second)
        return std::nullopt;
      Values = &Found->second;
      LocalDefinition = true;
    }
    std::optional<Origin> Result;
    for (const auto &Value : *Values) {
      const auto O = Resolve(Value, Depth + 1, Visiting);
      if (!O || (Result && *Result != *O)) {
        if (LocalDefinition)
          Visiting.erase(Identity);
        return std::nullopt;
      }
      Result = O;
    }
    if (LocalDefinition)
      Visiting.erase(Identity);
    return Result;
  };
  const auto From = [&](const ExprPtr &E) {
    std::set<Local> Seen;
    return Resolve(E, 0, Seen);
  };
  const auto Param = [&](const ExprPtr &E) -> std::optional<size_t> {
    const auto O = From(E);
    return O && O->first >= 0 && size_t(O->first) < F.Params.size()
               ? std::optional<size_t>{size_t(O->first)}
               : std::nullopt;
  };
  const auto P = Param(Once->Operands[0]), I = Param(Once->Operands[1]);
  const auto S = Param(Once->Operands[2]), D = Param(Store->StoreAddr);
  const auto Stored = From(Store->StoreVal);
  if (!P || !I || !S || !D || std::set<size_t>{*P, *I, *S, *D}.size() != 4 ||
      !Stored || Stored->first != -1 || !Stored->second ||
      Stored->second->Kind != ExprKind::Load ||
      Stored->second->Operands.size() != 1 ||
      Param(Stored->second->Operands[0]) != S ||
      From(Retain->Operands[0]) != Stored)
    return std::nullopt;
  const auto *SourceLoad = Stored->second;
  const std::set<size_t> Parameters{*P, *I, *S, *D};
  std::vector<std::vector<unsigned>> Effects(Flow.Nodes.size());
  size_t PredicateReads = 0, SourceReads = 0;
  Budget = 10000;
  for (size_t N = 0; N < Flow.Nodes.size(); ++N) {
    const auto &Node = Flow.Nodes[N];
    auto &Events = Effects[N];
    std::function<void(const ExprPtr &, unsigned)> Scan = [&](const ExprPtr &E,
                                                              unsigned Depth) {
      // Statement-form void calls need not carry an expression result type.
      // onceCall already authenticated the complete runtime declaration.
      if (E && E.get() == Once) {
        Events.push_back(1);
        return;
      }
      if (!E || !Budget-- || Depth > 64 || !E->Type || E->Type->Size > 8 ||
          (E->Type->Kind != NdTypeKind::Int &&
           E->Type->Kind != NdTypeKind::Ptr &&
           E->Type->Kind != NdTypeKind::Void) ||
          E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
          E->MemoryOrdering != NdMemoryOrdering::None ||
          E->MemoryAddressSpace != NdMemoryAddressSpace::Default) {
        Valid = false;
        return;
      }
      if (E.get() == Retain) {
        Scan(E->Operands[0], Depth + 1);
        Events.push_back(4);
        return;
      }
      if (E->Kind == ExprKind::Load) {
        const auto FromParameter =
            E->Operands.size() == 1 ? Param(E->Operands[0]) : std::nullopt;
        if (!Plain8(E) || !FromParameter) {
          Valid = false;
          return;
        }
        if (*FromParameter == *P) {
          ++PredicateReads;
          Events.push_back(0);
        } else if (E.get() == SourceLoad) {
          ++SourceReads;
          Events.push_back(2);
        } else
          Valid = false;
        return;
      }
      if (Param(E)) {
        Valid = false;
        return;
      }
      if ((E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) &&
          !IdentityCast(E)) {
        Valid = false;
        return;
      }
      if (E->Kind == ExprKind::Var && E->Var.Kind != MedVar::Reg &&
          E->Var.Kind != MedVar::Temp)
        Valid = false;
      if (E->Kind != ExprKind::Var && E->Kind != ExprKind::Const &&
          E->Kind != ExprKind::Phi && E->Kind != ExprKind::Cast &&
          E->Kind != ExprKind::BitCast && E->Kind != ExprKind::BinOp &&
          E->Kind != ExprKind::UnaryOp)
        Valid = false;
      for (const auto &Operand : E->Operands)
        Scan(Operand, Depth + 1);
    };
    if (Node.Test) {
      Scan(Node.Test, 0);
      continue;
    }
    if (!Node.Statement)
      continue;
    const auto &Statement = *Node.Statement;
    switch (Statement.Kind) {
    case StmtKind::Assign:
      if (const auto A = Param(Statement.Val)) {
        if (!Parameters.count(*A))
          Valid = false;
      } else
        Scan(Statement.Val, 0);
      break;
    case StmtKind::Store:
      Scan(Statement.StoreVal, 0);
      Events.push_back(3);
      break;
    case StmtKind::Call:
      Scan(Statement.CallExpr, 0);
      break;
    case StmtKind::ExprStmt:
      Scan(Statement.Val, 0);
      break;
    case StmtKind::Return:
      if (From(Statement.RetVal) != std::optional<Origin>{{-1, Retain}})
        Valid = false;
      Scan(Statement.RetVal, 0);
      Events.push_back(5);
      break;
    case StmtKind::Goto:
    case StmtKind::Block:
    case StmtKind::Nop:
      break;
    default:
      Valid = false;
      break;
    }
  }
  if (!Valid || PredicateReads != 1 || SourceReads != 1)
    return std::nullopt;
  // Every path keeps predicate/optional once -> load -> store -> retain ->
  // return. No cycle or fallthrough exit can silently certify a partial copy.
  std::set<std::tuple<size_t, unsigned, bool>> Active, Done;
  std::function<bool(size_t, unsigned, bool)> Path =
      [&](size_t N, unsigned Phase, bool Called) {
        if (N >= Flow.Nodes.size())
          return false;
        const auto Key = std::tuple{N, Phase, Called};
        if (Done.count(Key))
          return true;
        if (!Active.insert(Key).second)
          return false;
        for (const auto Event : Effects[N]) {
          if (Event == 1) {
            if (Phase != 1 || Called)
              return false;
            Called = true;
          } else {
            const unsigned Expected = Event == 0 ? 0 : Event - 1;
            if (Phase != Expected)
              return false;
            ++Phase;
          }
        }
        const auto &Successors = Flow.Nodes[N].Successors;
        if (Successors.empty()) {
          if (Phase != 5)
            return false;
        } else {
          if (Phase == 5)
            return false;
          for (const auto Next : Successors)
            if (!Path(Next, Phase, Called))
              return false;
        }
        Active.erase(Key);
        Done.insert(Key);
        return true;
      };
  if (!Path(Flow.Entry, 0, false))
    return std::nullopt;
  return SwiftOnceCopyContract{F.Params.size(), *P, *S, *D, *I};
}

struct CopyReferences {
  SourceCallTypeHint Predicate;
  SourceCallTypeHint Source;
  SourceCallTypeHint Destination;
  va_t Initializer = 0;
};

// Recompute the callee use proof and the caller's exact symbol/storage
// identities whenever discovery or binding consumes a copy contract.
inline std::optional<CopyReferences>
copyReferences(const HighExpr &E, const HighFunc &Caller,
               const BinaryImage &Image, const SwiftOnceCopyContract &Planned,
               const std::map<va_t, const HighFunc *> &Functions) {
  if (!Caller.SourceTypeHint ||
      !objc_projection_detail::sameHint(*Caller.SourceTypeHint,
                                        callbackHint(Image.Arch)) ||
      !llvm::StringRef(Caller.Name).starts_with("_$s") ||
      !llvm::StringRef(Caller.Name).ends_with("_WZ") ||
      E.Kind != ExprKind::Call || !E.SourceCallHint || E.IsIndirectCall ||
      E.CallAddr != E.SourceCallHint->TargetAddress ||
      !objc_binding_detail::plainNativeBinding(*E.SourceCallHint) ||
      E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty() ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      E.Operands.size() != Planned.ParameterCount ||
      !objcSourceCallBound(E, Image, Functions))
    return std::nullopt;
  const auto Callee = Functions.find(E.SourceCallHint->TargetAddress);
  if (Callee == Functions.end() || !Callee->second ||
      Callee->second->Entry != E.CallAddr)
    return std::nullopt;
  const auto Current = copyContract(*Callee->second, Image);
  if (!Current || !(*Current == Planned))
    return std::nullopt;
  const auto Constant = [&](size_t Index) -> std::optional<va_t> {
    const auto &Value = E.Operands[Index];
    if (!Value || Value->Kind != ExprKind::Const || !Value->Type ||
        Value->Type->Size != 8 ||
        (Value->Type->Kind != NdTypeKind::Int &&
         Value->Type->Kind != NdTypeKind::Ptr) ||
        !Value->Operands.empty() || Value->SourceCallHint ||
        Value->IntrinsicId != Intrinsic::None ||
        !Value->IntrinsicOutputs.empty() ||
        Value->MemoryOrdering != NdMemoryOrdering::None ||
        Value->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
    return Value->ConstVal;
  };
  const auto P = Constant(Current->Predicate), S = Constant(Current->Source);
  const auto D = Constant(Current->Destination),
             I = Constant(Current->Initializer);
  if (!P || !S || !D || !I || !Image.isCodeAddress(*I))
    return std::nullopt;
  const std::array<va_t, 3> Storage{*P, *S, *D};
  for (size_t A = 0; A < Storage.size(); ++A) {
    if (Storage[A] % 8 || Storage[A] > InvalidVA - 8)
      return std::nullopt;
    for (size_t B = 0; B < A; ++B)
      if (Storage[A] < Storage[B] + 8 && Storage[B] < Storage[A] + 8)
        return std::nullopt;
  }
  const auto Predicate =
      objc_binding_detail::oncePredicateStorageHint(Image, *P);
  const auto Source = objc_binding_detail::localStorageHint(Image, *S, 8);
  const auto Destination = objc_binding_detail::localStorageHint(Image, *D, 8);
  if (!Predicate || !Source || !Destination ||
      !llvm::StringRef(Predicate->TargetName).starts_with("_$s") ||
      !llvm::StringRef(Predicate->TargetName).ends_with("_Wz"))
    return std::nullopt;
  const std::string Prefix =
      llvm::StringRef(Predicate->TargetName).drop_back(3).str();
  const std::string DestinationPrefix =
      llvm::StringRef(Caller.Name).drop_back(3).str();
  const auto StorageName = [&](const std::string &Name,
                               const std::string &Base) {
    return Name.size() > Base.size() + 3 &&
           llvm::StringRef(Name).starts_with(Base) &&
           llvm::StringRef(Name).ends_with("vpZ");
  };
  if (!StorageName(Source->TargetName, Prefix) ||
      !StorageName(Destination->TargetName, DestinationPrefix))
    return std::nullopt;
  const auto UniqueSymbol = [&](va_t Address, const std::string &Name,
                                bool Function) {
    size_t Matches = 0;
    for (const auto &Symbol : Image.Symbols) {
      if (Symbol.Name == Name && Symbol.Addr != Address)
        return false;
      if (Symbol.Addr != Address)
        continue;
      if (Symbol.Name != Name || Symbol.IsFunc != Function ||
          (!Function && Symbol.Size && Symbol.Size != 8))
        return false;
      ++Matches;
    }
    return Matches == 1;
  };
  const auto Callback = Functions.find(*I);
  if (!UniqueSymbol(*P, Predicate->TargetName, false) ||
      !UniqueSymbol(*S, Source->TargetName, false) ||
      !UniqueSymbol(*D, Destination->TargetName, false) ||
      !UniqueSymbol(*I, Prefix + "_WZ", true) ||
      !UniqueSymbol(Caller.Entry, Caller.Name, true) ||
      Callback == Functions.end() || !Callback->second ||
      Callback->second->Entry != *I ||
      Callback->second->Name != Prefix + "_WZ" ||
      !ignoresContext(*Callback->second))
    return std::nullopt;
  return CopyReferences{*Predicate, *Source, *Destination, *I};
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
    else if (auto Contract = copyContract(F, Image))
      Plan.Copies.emplace(F.Entry, *Contract);
    else if (auto Contract = addressorContract(F, Image)) {
      Plan.Addressors.emplace(F.Entry, *Contract);
      Plan.AddressorHints.emplace(F.Entry, addressorHint(Image.Arch));
    } else if (auto Contract = objcThunkContract(F, Image)) {
      Plan.ObjCThunks.emplace(F.Entry, *Contract);
    }
  for (const auto &[_, Contract] : Plan.Addressors)
    if (Functions.count(Contract.Initializer) &&
        !DirectTargets.count(Contract.Initializer))
      Plan.CallbackHints.emplace(Contract.Initializer,
                                 callbackHint(Image.Arch));
  for (const auto &[_, Contract] : Plan.ObjCThunks)
    if (const auto Callback = Functions.find(Contract.Initializer);
        Callback != Functions.end() && Callback->second &&
        !DirectTargets.count(Contract.Initializer) &&
        ignoresContext(*Callback->second))
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
          if (onceCall(*E, Image)) {
            const auto Predicate =
                objc_binding_detail::constantAddress(*E->Operands[0]);
            const auto Initializer =
                objc_binding_detail::constantAddress(*E->Operands[1]);
            const auto Callback =
                Initializer ? Functions.find(*Initializer) : Functions.end();
            if (Predicate && Initializer && Callback != Functions.end() &&
                Callback->second && Image.isCodeAddress(*Initializer) &&
                !DirectTargets.count(*Initializer) &&
                objc_binding_detail::oncePredicateStorageHint(Image,
                                                              *Predicate) &&
                ignoresContext(*Callback->second))
              Plan.CallbackHints.emplace(*Initializer,
                                         callbackHint(Image.Arch));
          }
          if (dispatchOnceCall(*E, Image)) {
            const auto Predicate =
                objc_binding_detail::constantAddress(*E->Operands[0]);
            const auto Callback =
                objc_binding_detail::constantAddress(*E->Operands[2]);
            if (Predicate && Callback && Functions.count(*Callback) &&
                Image.isCodeAddress(*Callback) &&
                !DirectTargets.count(*Callback) &&
                objc_binding_detail::oncePredicateStorageHint(Image,
                                                              *Predicate)) {
              const auto Hint = dispatchCallbackHint(Image.Arch);
              const auto [It, Added] =
                  Plan.CallbackHints.emplace(*Callback, Hint);
              if ((Added ||
                   objc_projection_detail::sameHint(It->second, Hint)) &&
                  !Plan.Addressors.count(*Callback))
                Plan.DispatchOnceCallbacks.insert(*Callback);
            }
          }
          if (E->Kind == ExprKind::Call && E->SourceCallHint &&
              E->SourceCallHint->CallKind == SourceCallTypeHint::Kind::Native &&
              objcSourceCallBound(*E, Image, Functions)) {
            if (const auto Copy =
                    Plan.Copies.find(E->SourceCallHint->TargetAddress);
                Copy != Plan.Copies.end()) {
              const auto References =
                  copyReferences(*E, F, Image, Copy->second, Functions);
              if (References && !DirectTargets.count(References->Initializer))
                Plan.CallbackHints.emplace(References->Initializer,
                                           callbackHint(Image.Arch));
            }
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
  const auto RootCallbacks = Plan.CallbackHints;
  for (const auto &[Address, Hint] : RootCallbacks) {
    if (Plan.DispatchOnceCallbacks.count(Address) ||
        DirectTargets.count(Address))
      continue;
    const auto Outer = Functions.find(Address);
    const auto Contract = Outer == Functions.end() || !Outer->second
                              ? std::nullopt
                              : nestedCallbackContract(*Outer->second, Image);
    if (!Contract || Contract->Initializer == Address ||
        DirectTargets.count(Contract->Initializer))
      continue;
    const auto Inner = Functions.find(Contract->Initializer);
    if (Inner == Functions.end() || !Inner->second ||
        !ignoresContext(*Inner->second))
      continue;
    Plan.NestedCallbacks.emplace(Address, *Contract);
    Plan.CallbackHints.emplace(Contract->Initializer, callbackHint(Image.Arch));
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

// Shared authentication for projection and the current native-inference round.
// Discovery publishes call-only hints before the initializer has been
// re-lifted; neither those hints nor their presence in MedIR establish context
// independence.
inline std::optional<SourceCallTypeHint> validatedSwiftOnceAddressorCallee(
    va_t Target, const BinaryImage &Image, const SwiftOnceSourcePlan &Plan,
    const std::map<va_t, const HighFunc *> &Functions) {
  const auto Planned = Plan.Addressors.find(Target);
  const auto Function = Functions.find(Target);
  if (Planned == Plan.Addressors.end() || Function == Functions.end() ||
      !Function->second || Function->second->Entry != Target)
    return std::nullopt;
  const auto Current =
      swift_once_source_detail::addressorContract(*Function->second, Image);
  if (!Current || !(*Current == Planned->second))
    return std::nullopt;
  const auto Callback = Functions.find(Current->Initializer);
  const auto CallbackHint = Plan.CallbackHints.find(Current->Initializer);
  const auto ExpectedCallback =
      swift_once_source_detail::callbackHint(Image.Arch);
  if (Callback == Functions.end() || !Callback->second ||
      Callback->second->Entry != Current->Initializer ||
      CallbackHint == Plan.CallbackHints.end() ||
      !Callback->second->SourceTypeHint ||
      !objc_projection_detail::sameHint(CallbackHint->second,
                                        ExpectedCallback) ||
      !objc_projection_detail::sameHint(*Callback->second->SourceTypeHint,
                                        ExpectedCallback) ||
      !swift_once_source_detail::ignoresContext(*Callback->second))
    return std::nullopt;
  return swiftOnceAddressorCallHint(*Function->second, Image);
}

inline NativeSourceCalleeContracts
swiftOnceNativeCalleeContracts(const BinaryImage &Image,
                               const PipelineResult &Result,
                               const SwiftOnceSourcePlan &Plan) {
  NativeSourceCalleeContracts Contracts;
  if (Result.SourceImage != &Image)
    return Contracts;
  Contracts.SourceImage = &Image;
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &Function : Result.HighFuncs)
    if (!Functions.emplace(Function.Entry, &Function).second)
      return {&Image, {}};
  for (const auto &[Target, Contract] : Plan.Addressors)
    if (const auto Hint =
            validatedSwiftOnceAddressorCallee(Target, Image, Plan, Functions))
      Contracts.ZeroArgumentPointerCallees.emplace(Target, Hint->Signature);
  return Contracts;
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
  const auto Expected = validatedSwiftOnceAddressorCallee(
      Binding.TargetAddress, Image, Plan, Functions);
  if (!Expected || E.CallAddr != Binding.TargetAddress ||
      Binding.TargetName != Expected->TargetName || Binding.DoesNotReturn ||
      Binding.WeakImport || Binding.ReturnedArgument ||
      Binding.RuntimeObjCResultType || Binding.ValueWitness ||
      !Binding.Selector.empty() || !Binding.OwnerClass.empty() ||
      Binding.SelectorReferenceAddress || Binding.Format ||
      Binding.NilTerminated || Binding.Receiver || Binding.SelectorResultUse ||
      Binding.SelectorResultTypeUse || Binding.SelectorArgumentTypeUse ||
      Binding.SelectorForwardingUse ||
      Binding.SelectorArgumentStorageUse || Binding.ObjCIndirectResultStorage ||
      Binding.ByteCount || Binding.ImmutablePointerSlot ||
      Binding.SwiftTypeMetadata || !Binding.BorrowedByteInputs.empty() ||
      !Binding.SwiftStringInputs.empty() ||
      !objc_projection_detail::sameHint(Binding.Signature, Expected->Signature))
    return false;
  return true;
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
      Binding.NilTerminated || Binding.SelectorResultUse ||
      Binding.SelectorResultTypeUse || Binding.SelectorArgumentTypeUse ||
      Binding.SelectorForwardingUse ||
      Binding.SelectorArgumentStorageUse || Binding.ObjCIndirectResultStorage ||
      Binding.ValueWitness || Binding.ReturnedArgument ||
      Binding.RuntimeObjCResultType || !Binding.BorrowedByteInputs.empty() ||
      !Binding.SwiftStringInputs.empty() ||
      !objc_projection_detail::sameHint(*F->second->SourceTypeHint,
                                        Expected->second))
    return false;
  SourceFunctionTypeHint Address;
  Address.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  const bool Dispatch =
      Plan.DispatchOnceCallbacks.count(Binding.TargetAddress) != 0;
  return assignDarwinScalarSourceABI(Address, Image.Arch, Error) &&
         objc_projection_detail::sameHint(Binding.Signature, Address) &&
         (Dispatch || swift_once_source_detail::ignoresContext(*F->second));
}

inline ObjCSourceBindingResult bindSwiftOnceSourceReferences(
    const HighFunc &Function, const BinaryImage &Image,
    const SwiftOnceSourcePlan &Plan,
    const std::map<va_t, const HighFunc *> &Functions,
    const std::map<va_t, std::set<size_t>> *IgnoredNativeContexts = nullptr) {
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
    if (IgnoredNativeContexts && E->Kind == ExprKind::Call &&
        !E->IsIndirectCall && E->SourceCallHint &&
        E->SourceCallHint->CallKind == SourceCallTypeHint::Kind::Native &&
        objcSourceCallBound(*E, Image, Functions)) {
      const auto Proof =
          IgnoredNativeContexts->find(E->SourceCallHint->TargetAddress);
      if (Proof != IgnoredNativeContexts->end()) {
        const auto Parameters =
            sourceABIParameters(E->SourceCallHint->Signature);
        if (Parameters.size() != E->Operands.size())
          return E;
        for (const auto Index : Proof->second) {
          if (Index >= Parameters.size() || Index >= E->Operands.size() ||
              !E->Operands[Index] || !Parameters[Index].Type ||
              Parameters[Index].ParameterIndex != Index ||
              Parameters[Index].ByteOffset != 0 ||
              (Parameters[Index].Type->Kind != NdTypeKind::Int &&
               Parameters[Index].Type->Kind != NdTypeKind::Ptr) ||
              Parameters[Index].Type->Size !=
                  Parameters[Index].Location.ValueBytes ||
              !swift_once_source_detail::unknownScalarView(E->Operands[Index]))
            continue;
          auto Zero = HighExpr::makeConst(0, Parameters[Index].Type->Size,
                                          ConstantAddressProvenance::Scalar);
          Zero->Type = Parameters[Index].Type;
          E->Operands[Index] = std::move(Zero);
        }
      }
    }
    if (swift_once_source_detail::onceCall(*E, Image)) {
      const bool Nested = Plan.NestedCallbacks.count(Function.Entry) != 0;
      const auto &Contracts = Nested ? Plan.NestedCallbacks : Plan.ObjCThunks;
      const auto Planned = Contracts.find(Function.Entry);
      const auto Current =
          Nested ? swift_once_source_detail::nestedCallbackContract(Function,
                                                                    Image)
                 : swift_once_source_detail::objcThunkContract(Function, Image);
      if (Planned != Contracts.end() && Current &&
          Current->sameIdentity(Planned->second)) {
        const auto Predicate =
            objc_binding_detail::constantAddress(*E->Operands[0]);
        const auto Initializer =
            objc_binding_detail::constantAddress(*E->Operands[1]);
        const auto Context =
            swift_once_source_detail::parameter(E->Operands[2]);
        auto PredicateHint =
            Predicate ? objc_binding_detail::oncePredicateStorageHint(
                            Image, *Predicate)
                      : std::nullopt;
        if (Predicate && *Predicate == Planned->second.Predicate &&
            Initializer && *Initializer == Planned->second.Initializer &&
            Context && *Context == 2 && PredicateHint) {
          auto Address = HighExpr::makeCall({}, 0, {});
          auto AddressHint = std::make_shared<SourceCallTypeHint>();
          AddressHint->CallKind = SourceCallTypeHint::Kind::NativeAddress;
          AddressHint->TargetAddress = *Initializer;
          AddressHint->Signature.ReturnType =
              NdType::makePtr(NdType::makeVoid());
          std::string Error;
          if (assignDarwinScalarSourceABI(AddressHint->Signature, Image.Arch,
                                          Error)) {
            Address->Type = AddressHint->Signature.ReturnType;
            Address->SourceCallHint = std::move(AddressHint);
            if (swiftOnceCallbackBound(*Address, Image, Plan, Functions)) {
              auto Storage = HighExpr::makeCall({}, 0, {});
              Storage->Type = E->Operands[0]->Type;
              Storage->SourceCallHint = std::make_shared<SourceCallTypeHint>(
                  std::move(*PredicateHint));
              auto Null =
                  HighExpr::makeConst(0, 8, ConstantAddressProvenance::Scalar);
              Null->Type = NdType::makePtr(NdType::makeVoid());
              E->Operands[0] = std::move(Storage);
              E->Operands[1] = std::move(Address);
              E->Operands[2] = std::move(Null);
              Result.LocalStorageExtents[Planned->second.Predicate] = 8;
              if (Planned->second.Storage)
                Result.LocalStorageExtents[*Planned->second.Storage] = 8;
              Result.Dependencies.insert(Planned->second.Initializer);
              if (!Nested)
                Result.SwiftOnceObjCThunks.insert(Function.Entry);
              return E;
            }
          }
        }
      }
      const auto Predicate =
          objc_binding_detail::constantAddress(*E->Operands[0]);
      const auto Initializer =
          objc_binding_detail::constantAddress(*E->Operands[1]);
      auto PredicateHint =
          Predicate ? objc_binding_detail::oncePredicateStorageHint(
                          Image, *Predicate)
                    : std::nullopt;
      if (Predicate && Initializer && PredicateHint) {
        auto Address = HighExpr::makeCall({}, 0, {});
        auto AddressHint = std::make_shared<SourceCallTypeHint>();
        AddressHint->CallKind = SourceCallTypeHint::Kind::NativeAddress;
        AddressHint->TargetAddress = *Initializer;
        AddressHint->Signature.ReturnType =
            NdType::makePtr(NdType::makeVoid());
        std::string Error;
        if (assignDarwinScalarSourceABI(AddressHint->Signature, Image.Arch,
                                        Error)) {
          Address->Type = AddressHint->Signature.ReturnType;
          Address->SourceCallHint = std::move(AddressHint);
          if (swiftOnceCallbackBound(*Address, Image, Plan, Functions)) {
            const auto Context =
                swift_once_source_detail::parameter(E->Operands[2]);
            auto Storage = HighExpr::makeCall({}, 0, {});
            Storage->Type = E->Operands[0]->Type;
            Storage->SourceCallHint = std::make_shared<SourceCallTypeHint>(
                std::move(*PredicateHint));
            auto Null =
                HighExpr::makeConst(0, 8, ConstantAddressProvenance::Scalar);
            Null->Type = NdType::makePtr(NdType::makeVoid());
            E->Operands[0] = std::move(Storage);
            E->Operands[1] = std::move(Address);
            E->Operands[2] = std::move(Null);
            Result.LocalStorageExtents[*Predicate] = 8;
            Result.Dependencies.insert(*Initializer);
            if (Context)
              Result.ErasedSwiftOnceContextParameters.insert(*Context);
            return E;
          }
        }
      }
    }
    if (swift_once_source_detail::dispatchOnceCall(*E, Image)) {
      const auto Predicate =
          objc_binding_detail::constantAddress(*E->Operands[0]);
      const auto Callback =
          objc_binding_detail::constantAddress(*E->Operands[2]);
      auto PredicateHint =
          Predicate
              ? objc_binding_detail::oncePredicateStorageHint(Image, *Predicate)
              : std::nullopt;
      if (Predicate && Callback && PredicateHint &&
          Plan.DispatchOnceCallbacks.count(*Callback)) {
        auto Address = HighExpr::makeCall({}, 0, {});
        auto AddressHint = std::make_shared<SourceCallTypeHint>();
        AddressHint->CallKind = SourceCallTypeHint::Kind::NativeAddress;
        AddressHint->TargetAddress = *Callback;
        AddressHint->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
        std::string Error;
        if (assignDarwinScalarSourceABI(AddressHint->Signature, Image.Arch,
                                        Error)) {
          Address->Type = AddressHint->Signature.ReturnType;
          Address->SourceCallHint = std::move(AddressHint);
          if (swiftOnceCallbackBound(*Address, Image, Plan, Functions)) {
            auto Storage = HighExpr::makeCall({}, 0, {});
            Storage->Type = E->Operands[0]->Type;
            Storage->SourceCallHint =
                std::make_shared<SourceCallTypeHint>(std::move(*PredicateHint));
            E->Operands[0] = std::move(Storage);
            E->Operands[2] = std::move(Address);
            Result.LocalStorageExtents[*Predicate] = 8;
            Result.Dependencies.insert(*Callback);
            return E;
          }
        }
      }
    }
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
      const auto Expected = validatedSwiftOnceAddressorCallee(
          E->CallAddr, Image, Plan, Functions);
      if (Planned != Plan.Addressors.end() && Expected &&
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
    if (const auto CopyContract =
            Plan.Copies.find(E->SourceCallHint->TargetAddress);
        CopyContract != Plan.Copies.end()) {
      const auto References = swift_once_source_detail::copyReferences(
          *E, Function, Image, CopyContract->second, Functions);
      if (!References)
        return E;
      auto Address = HighExpr::makeCall({}, 0, {});
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->CallKind = SourceCallTypeHint::Kind::NativeAddress;
      Hint->TargetAddress = References->Initializer;
      Hint->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
      std::string Error;
      if (!assignDarwinScalarSourceABI(Hint->Signature, Image.Arch, Error))
        return E;
      Address->Type = Hint->Signature.ReturnType;
      Address->SourceCallHint = Hint;
      if (!swiftOnceCallbackBound(*Address, Image, Plan, Functions))
        return E;
      const auto &C = CopyContract->second;
      E->Operands[C.Initializer] = std::move(Address);
      for (const auto &[Index, Binding] :
           {std::pair{C.Predicate, References->Predicate},
            std::pair{C.Source, References->Source},
            std::pair{C.Destination, References->Destination}}) {
        auto Value = HighExpr::makeCall({}, 0, {});
        Value->Type = E->Operands[Index]->Type;
        Value->SourceCallHint = std::make_shared<SourceCallTypeHint>(Binding);
        E->Operands[Index] = std::move(Value);
        Result.LocalStorageExtents[Binding.TargetAddress] = 8;
      }
      Result.Dependencies.insert(References->Initializer);
      return E;
    }
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
  const bool IsAddressorInitializer =
      std::any_of(Plan.Addressors.begin(), Plan.Addressors.end(),
                  [&](const auto &Entry) {
                    return Entry.second.Initializer == Function.Entry;
                  }) ||
      std::any_of(Plan.ObjCThunks.begin(), Plan.ObjCThunks.end(),
                  [&](const auto &Entry) {
                    return Entry.second.Initializer == Function.Entry;
                  });
  if (IsAddressorInitializer) {
    Result.Function.Name = swiftOnceInitializerName(Function.Entry);
    Result.Function.DebugName.clear();
    Result.Function.SourceFile.clear();
  }
  return Result;
}

inline std::optional<ObjCSourceBindingResult> projectSwiftOnceNestedCallback(
    const HighFunc &Function, const BinaryImage &Image,
    const SwiftOnceSourcePlan &Plan,
    const std::map<va_t, const HighFunc *> &Functions) {
  const auto Planned = Plan.NestedCallbacks.find(Function.Entry);
  const auto Current =
      swift_once_source_detail::nestedCallbackContract(Function, Image);
  const auto Callback = Plan.CallbackHints.find(Function.Entry);
  if (Planned == Plan.NestedCallbacks.end() || !Current ||
      !Current->sameIdentity(Planned->second) ||
      Callback == Plan.CallbackHints.end() ||
      !objc_projection_detail::sameHint(Callback->second, Current->Signature))
    return std::nullopt;
  auto Result = bindSwiftOnceSourceReferences(Function, Image, Plan, Functions);
  if (!Result.Dependencies.count(Current->Initializer) ||
      !Result.LocalStorageExtents.count(Current->Predicate) ||
      !swift_once_source_detail::ignoresContext(Result.Function))
    return std::nullopt;
  // Failure to bind the extra context may leave the generic x0 return in
  // HighIR even after callback discovery. The once callback ABI returns void.
  // Discard only a side-effect-free scalar carrier, keeping every preceding
  // call and memory operation; an embedded load/call must not disappear.
  walkStmts(Result.Function.Body, [&](HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Return)
      Statement.RetVal.reset();
  });
  Result.Function.Params = {
      {"once_context", NdType::makePtr(NdType::makeVoid())}};
  Result.Function.ReturnType = Current->Signature.ReturnType;
  Result.Function.SourceTypeHint = Current->Signature;
  return Result;
}

inline bool
finalizeSwiftOnceObjCThunkProjection(HighFunc &Function,
                                     const SwiftOnceSourcePlan &Plan) {
  const auto Planned = Plan.ObjCThunks.find(Function.Entry);
  if (Planned == Plan.ObjCThunks.end() || Function.SourceTypeHint ||
      Function.Params.size() != 3)
    return false;
  const auto Parameters = sourceABIParameters(Planned->second.Signature);
  if (Parameters.size() != 2)
    return false;
  bool Valid = true;
  std::set<HighExpr *> Seen;
  walkStmts(Function.Body, [&](HighStmt &Statement) {
    forEachExpr(Statement, [&](ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty()) {
        auto Expression = Pending.back();
        Pending.pop_back();
        if (!Expression || !Seen.insert(Expression.get()).second)
          continue;
        if (Expression->Kind == ExprKind::Var &&
            Expression->Var.Kind == MedVar::Param) {
          if (Expression->Var.Id < 0 ||
              size_t(Expression->Var.Id) >= Parameters.size()) {
            Valid = false;
          } else {
            const auto &Declared = Parameters[Expression->Var.Id];
            Expression->Type = Declared.Type;
            Expression->Var.Size = Declared.Type->Size;
          }
        }
        Pending.insert(Pending.end(), Expression->Operands.begin(),
                       Expression->Operands.end());
      }
    });
  });
  if (!Valid)
    return false;
  Function.Params.clear();
  for (const auto &Parameter : Parameters)
    Function.Params.push_back({Parameter.Name, Parameter.Type});
  Function.ReturnType = Planned->second.Signature.ReturnType;
  Function.SourceTypeHint = Planned->second.Signature;
  return true;
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
    if (Planned == Plan.Addressors.end() ||
        !validatedSwiftOnceAddressorCallee(Address, Image, Plan, Functions))
      throw std::runtime_error("Swift once addressor is no longer valid");
    const auto &Current = Planned->second;

    const std::string Predicate =
        "neverd_local_storage_" +
        llvm::utohexstr(Current.Predicate, /*LowerCase=*/true) + "_address";
    const std::string Storage =
        "neverd_local_storage_" +
        llvm::utohexstr(Current.Storage, /*LowerCase=*/true) + "_address";
    const std::string Initializer =
        swiftOnceInitializerName(Current.Initializer);
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
