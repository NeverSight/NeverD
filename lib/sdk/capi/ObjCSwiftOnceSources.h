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

struct SwiftOnceSourcePlan {
  std::map<va_t, SwiftOnceGetterContract> Getters;
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
    if (auto Contract = getterContract(F, Image))
      Plan.Getters.emplace(F.Entry, *Contract);
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
  for (const auto &[Address, Hint] : Plan.CallbackHints)
    Added += Options.SourceTypeHints.emplace(Address, Hint).second;
  return Added;
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
  return Result;
}
} // namespace neverd::sdk
#endif
