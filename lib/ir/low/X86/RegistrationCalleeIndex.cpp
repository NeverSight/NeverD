//===- RegistrationCalleeIndex.cpp - PE32 callee closure ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"
#include "RegistrationStackOwner.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"

namespace neverd {
namespace {
using registration_abi::chargeCalleeWork;
} // namespace

RegistrationCallCalleeIndex::RegistrationCallCalleeIndex(
    const BinaryImage &Image, size_t *CumulativeWork,
    const detail::AbsoluteRelocationRootIndex *Roots)
    : Image(Image), Work(CumulativeWork ? *CumulativeWork : LocalWork) {
  // Own the snapshot so a subsidiary proof cannot outlive its caller's CFG.
  // Copies share the image-only runtime-role computation, never callee facts.
  if (Roots && Roots->ownsImage(Image))
    RelocationRoots =
        std::make_shared<const detail::AbsoluteRelocationRootIndex>(*Roots);
}

const detail::AbsoluteRelocationRootIndex &
RegistrationCallCalleeIndex::absoluteRelocationRoots() {
  if (!RelocationRoots)
    RelocationRoots =
        std::make_shared<const detail::AbsoluteRelocationRootIndex>(Image);
  return *RelocationRoots;
}

std::optional<std::vector<RegistrationCalleeFrameContract>>
RegistrationCallCalleeIndex::contracts(const LowFunc &Function) {
  const registration_abi::StackProofOwnerScope OwnerScope(*this, Work);
  const auto FrameCalls =
      registration_abi::checkedRegistrationFrameCalls(Image, Function, Work);
  if (!FrameCalls)
    return std::nullopt;
  std::set<va_t> Targets;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!chargeCalleeWork(Work, 1))
        return std::nullopt;
      if (!FrameCalls->count(&Op) && Op.Opcode == NdOp::CALL &&
          Op.NumInputs == 1 && Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4)
        Targets.insert(Op.Inputs[0].Offset);
      if (Targets.size() >
          ::neverd::limits::ir::
              kRegistrationCalleeIndexRegistrationCallCalleeIndexContractsTargetsSizeLimit)
        return std::nullopt;
    }
  std::vector<RegistrationCalleeFrameContract> Result;
  for (va_t Target : Targets) {
    auto Existing = Cache.find(Target);
    if (Existing == Cache.end()) {
      if (Cache.size() ==
          ::neverd::limits::ir::
              kRegistrationCalleeIndexRegistrationCallCalleeIndexContractsCacheSizeLimit)
        return std::nullopt;
      // A nested CFG must see an unavailable in-progress answer, never start
      // a fresh copy of this same invocation with another proof budget.
      Existing = Cache.emplace(Target, std::nullopt).first;
      std::optional<RegistrationCalleeFrameContract> Contract;
      if (auto Import =
              getCheckedX86RegistrationThrowImportABI(Image, Target, &Work)) {
        Contract.emplace();
        Contract->CalleeKind =
            RegistrationCalleeFrameContract::Kind::RuntimeThrow;
        Contract->Target = Target;
        Contract->DoesNotReturn = true;
        Contract->RequiredImports.push_back(Import->IATVA);
        Contract->CodeRanges.push_back({Target, Target + 6});
      } else if (auto Initializer =
                     getCheckedX86RegistrationInitializerCalleeABI(
                         Image, Target, &Work)) {
        auto &Object = Initializer->Object;
        Contract.emplace();
        Contract->CalleeKind =
            RegistrationCalleeFrameContract::Kind::Initializer;
        Contract->Target = Target;
        Contract->StackPopBytes = Object.StackPopBytes;
        Contract->ECXReads = std::move(Object.ECXReads);
        Contract->ECXWrites = std::move(Object.ECXWrites);
        Contract->ECXInitialized = std::move(Initializer->Initialized);
        Contract->ImageReads = std::move(Object.ImageReads);
        Contract->ImageWrites = std::move(Object.ImageWrites);
        Contract->CodeRanges = std::move(Object.CodeRanges);
      } else if (auto Leaf = getCheckedX86RegistrationMemberCalleeABI(
                     Image, Target, &Work, RegistrationLeafResult::Discarded)) {
        Contract.emplace();
        Contract->Target = Target;
        Contract->StackPopBytes = Leaf->StackPopBytes;
        Contract->HasIndependentScalarReturn = Leaf->HasIndependentScalarReturn;
        Contract->ECXReads = std::move(Leaf->ECXReads);
        Contract->ECXWrites = std::move(Leaf->ECXWrites);
        Contract->ImageReads = std::move(Leaf->ImageReads);
        Contract->ImageWrites = std::move(Leaf->ImageWrites);
        Contract->CallerPCWrites = std::move(Leaf->CallerPCWrites);
        Contract->CodeRanges = std::move(Leaf->CodeRanges);
      } else if (auto Throw = getCheckedX86RegistrationThrowCalleeABI(
                     Image, Target, &Work)) {
        Contract.emplace();
        Contract->CalleeKind =
            Throw->IsRethrow
                ? RegistrationCalleeFrameContract::Kind::PrivateRethrow
                : RegistrationCalleeFrameContract::Kind::PrivateThrow;
        Contract->Target = Target;
        Contract->DoesNotReturn = true;
        Contract->RequiredImports.push_back(Throw->ImportIATVA);
        if (!Throw->IsRethrow) {
          Contract->ThrownTypeVA = Throw->ThrowInfo.TypeDescriptorVA;
          Contract->ThrownObjectSize = Throw->ThrowInfo.ObjectSize;
        }
        Contract->ImageReads = std::move(Throw->ImageReads);
        Contract->ImageWrites = std::move(Throw->ImageWrites);
        Contract->CallerPCWrites = std::move(Throw->CallerPCWrites);
        Contract->CodeRanges = std::move(Throw->CodeRanges);
      } else
        Contract = cxxCallee(Target);
      if (Work == limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      Existing->second = std::move(Contract);
    }
    if (Existing->second) {
      const auto &Contract = *Existing->second;
      if (!chargeCalleeWork(
              Work,
              Contract.ECXReads.size() + Contract.ECXWrites.size() +
                  Contract.ECXInitialized.size() + Contract.ImageReads.size() +
                  Contract.ImageWrites.size() + Contract.CallerPCWrites.size() +
                  Contract.CodeRanges.size() + Contract.RequiredImports.size() +
                  Contract.RuntimeThrowInfos.size() + 1))
        return std::nullopt;
      Result.push_back(Contract);
    }
  }
  if (llvm::any_of(Result, [](const auto &C) { return C.isRuntimeThrow(); })) {
    registration_abi::ImageFrameEffects Effects;
    std::set<std::pair<va_t, va_t>> RuntimeCode;
    auto Infos = registration_abi::collectRegistrationRuntimeThrowInfos(
        Function, Image, Work, Effects, RuntimeCode);
    if (!Infos)
      return std::nullopt;
    for (auto &Contract : Result)
      if (Contract.isRuntimeThrow()) {
        if (!chargeCalleeWork(Work, Infos->size() + Effects.Reads.size() +
                                        Effects.Writes.size() +
                                        RuntimeCode.size() +
                                        Contract.CodeRanges.size()))
          return std::nullopt;
        Contract.RuntimeThrowInfos = *Infos;
        registration_abi::copyExtents(Effects.Reads, Contract.ImageReads);
        registration_abi::copyExtents(Effects.Writes, Contract.ImageWrites);
        auto Code = RuntimeCode;
        for (const auto &Range : Contract.CodeRanges)
          Code.emplace(Range.Begin, Range.End);
        Contract.CodeRanges.clear();
        registration_abi::copyExtents(Code, Contract.CodeRanges);
      }
  }
  return Result;
}

std::optional<std::vector<RegistrationCleanupFrameContract>>
RegistrationCallCalleeIndex::cleanupContracts(const LowFunc &Function) {
  const registration_abi::StackProofOwnerScope OwnerScope(*this, Work);
  std::vector<RegistrationCleanupFrameContract> Result;
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->Cxx)
    return Result;
  const auto &Actions = Function.ExceptionMetadata->Cxx->UnwindMap;
  if (Actions.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  for (uint32_t State = 0; State < Actions.size(); ++State) {
    if (!chargeCalleeWork(Work, 1))
      return std::nullopt;
    const auto &Action = Actions[State];
    if (Action.Kind != CxxUnwindAction::ActionKind::Direct ||
        !Action.ActionVA || Action.ObjectOffset)
      continue;
    auto It = CleanupCache.find(Action.ActionVA);
    if (It == CleanupCache.end()) {
      // Cleanup entries are owned by the bounded unwind graph, which can
      // contain more distinct relays than the ordinary-callee cache. Keep
      // that format bound and charge every successful or failed proof to the
      // same cumulative work budget.
      if (CleanupCache.size() == limits::kMaxRegistrationEHRecords)
        return std::nullopt;
      auto Relay = getCheckedX86RegistrationCleanupRelayABI(
          Image, Action.ActionVA, &Work);
      if (Work == limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      It = CleanupCache.emplace(Action.ActionVA, std::move(Relay)).first;
    }
    if (!It->second)
      continue;
    const auto &Relay = *It->second;
    // Cached bytes describe a relay, never the parent that dispatched it.
    const auto &Chain = Function.ExceptionMetadata->Registration;
    if (Relay.RealignedParent && (!Chain || !Relay.matchesParentFrame(*Chain)))
      continue;
    RegistrationCleanupFrameContract Contract;
    Contract.ActionState = State;
    Contract.RelayTarget = Relay.Target;
    for (const auto &Call : Relay.Calls) {
      const auto &Leaf = Call.Leaf;
      if (!chargeCalleeWork(Work, Leaf.ECXReads.size() + Leaf.ECXWrites.size() +
                                      Leaf.ImageReads.size() +
                                      Leaf.ImageWrites.size() +
                                      Leaf.CallerPCWrites.size() +
                                      Leaf.CodeRanges.size() + 1))
        return std::nullopt;
      auto &Borrow = Contract.Calls.emplace_back();
      Borrow.ObjectFrameOffset = Call.ObjectFrameOffset;
      Borrow.Leaf.Target = Leaf.Target;
      Borrow.Leaf.StackPopBytes = Leaf.StackPopBytes;
      Borrow.Leaf.ECXReads = Leaf.ECXReads;
      Borrow.Leaf.ECXWrites = Leaf.ECXWrites;
      Borrow.Leaf.ImageReads = Leaf.ImageReads;
      Borrow.Leaf.ImageWrites = Leaf.ImageWrites;
      Borrow.Leaf.CallerPCWrites = Leaf.CallerPCWrites;
      Borrow.Leaf.CodeRanges = Leaf.CodeRanges;
    }
    Result.push_back(std::move(Contract));
  }
  return Result;
}

} // namespace neverd
