#ifndef NEVERD_SDK_CAPI_OBJCSWIFTONCESOURCES_H
#define NEVERD_SDK_CAPI_OBJCSWIFTONCESOURCES_H

#include "ObjCSourceBindings.h"

#include "neverd/pipeline/Pipeline.h"

namespace neverd::sdk {

struct SwiftOnceGetterContract {
  size_t Predicate = 0;
  size_t Storage = 0;
  size_t Initializer = 0;
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
      F.Params.size() != 3 || F.SourceTypeHint->Parameters.size() != 3)
    return std::nullopt;
  for (const auto &P : F.Params)
    if (!P.Type || P.Type->Kind != NdTypeKind::Ptr || P.Type->Size != 8)
      return std::nullopt;
  const auto Flow = buildHighSourceFlowGraph(F);
  if (!Flow.Diagnostics.Complete || !Flow.Diagnostics.Items.empty())
    return std::nullopt;
  std::optional<SwiftOnceGetterContract> Contract;
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
      const auto P = parameter(E->Operands[0]);
      const auto I = parameter(E->Operands[1]);
      if (Contract || !P || !I || *P >= 3 || *I >= 3 || *P == *I ||
          parameter(E->Operands[2]) != P) {
        Valid = false;
        return;
      }
      Contract = SwiftOnceGetterContract{*P, 3 - *P - *I, *I};
      return;
    }
    if (E->Kind == ExprKind::Load && E->Operands.size() == 1 && E->Type &&
        E->Type->Kind == NdTypeKind::Int && E->Type->Size == 8 &&
        E->MemoryOrdering == NdMemoryOrdering::None &&
        E->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        E->IntrinsicId == Intrinsic::None) {
      if (const auto P = parameter(E->Operands[0]); P && *P < 3) {
        Reads.insert(*P);
        return;
      }
    }
    if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Param) {
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
      forEachExpr(*Node.Statement, [&](const ExprPtr &E) { Scan(E, 0); });
  }
  if (!Valid || !Contract || Reads.count(Contract->Initializer) ||
      !Reads.count(Contract->Predicate) || !Reads.count(Contract->Storage))
    return std::nullopt;
  return Contract;
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
              E->Operands.size() == 3 && E->Operands[0] && E->Operands[1] &&
              E->Operands[2] && objcSourceCallBound(*E, Image, Functions)) {
            auto Getter = Plan.Getters.find(E->SourceCallHint->TargetAddress);
            if (Getter != Plan.Getters.end()) {
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
      Binding.SelectorResultUse || Binding.SelectorArgumentTypeUse ||
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
        E->Operands.size() != 3 || !E->Operands[0] || !E->Operands[1] ||
        !E->Operands[2] || E->IntrinsicId != Intrinsic::None ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return E;
    auto Getter = Plan.Getters.find(E->SourceCallHint->TargetAddress);
    if (Getter == Plan.Getters.end() ||
        !objcSourceCallBound(*E, Image, Functions))
      return E;
    const auto &C = Getter->second;
    const auto P =
        objc_binding_detail::constantAddress(*E->Operands[C.Predicate]);
    const auto S =
        objc_binding_detail::constantAddress(*E->Operands[C.Storage]);
    const auto I =
        objc_binding_detail::constantAddress(*E->Operands[C.Initializer]);
    if (!P || !S || !I || (*P > *S ? *P - *S : *S - *P) < 8)
      return E;
    auto Predicate = objc_binding_detail::oncePredicateStorageHint(Image, *P);
    auto Storage = objc_binding_detail::localStorageHint(Image, *S, 8);
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
    for (auto [Index, Binding] :
         {std::pair{C.Predicate, *Predicate}, std::pair{C.Storage, *Storage}}) {
      auto Value = HighExpr::makeCall({}, 0, {});
      Value->Type = E->Operands[Index]->Type;
      Result.LocalStorageExtents[Binding.TargetAddress] = 8;
      Value->SourceCallHint =
          std::make_shared<SourceCallTypeHint>(std::move(Binding));
      E->Operands[Index] = std::move(Value);
    }
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
