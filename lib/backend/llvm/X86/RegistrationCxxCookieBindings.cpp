//===- RegistrationCxxCookieBindings.cpp - PE32 GS call coverage ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep source occurrence coverage separate from conditional cookie effects.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/RegistrationCxxCookieBindings.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86/X86RegistrationLayout.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/RegistrationCallEffects.h"
#include "neverd/ir/med/X86/X86RegistrationCallback.h"
#include "neverd/ir/med/X86/X86RegistrationFrame.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Errc.h"

#include <array>
#include <iterator>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace neverd::registration_frame {
namespace {
llvm::Error reject(const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "C++ cookie coverage: " + Detail);
}

bool hasSourceIdentity(const CxxSourceCallBinding &Binding) {
  const auto *MD =
      Binding.Call->getMetadata(windows_eh_md::RegistrationOperationAttachment);
  const uint64_t Values[] = {Binding.Owner, Binding.Address,
                             uint32_t(Binding.OpSeq), uint32_t(Binding.BlockId),
                             uint8_t(Binding.CallKind)};
  const unsigned Widths[] = {64, 64, 32, 32, 8};
  if (!MD || MD->getNumOperands() != std::size(Values))
    return false;
  for (unsigned I = 0; I != std::size(Values); ++I) {
    const auto *Value = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(
        MD->getOperand(I));
    if (!Value || Value->getBitWidth() != Widths[I] ||
        Value->getZExtValue() != Values[I])
      return false;
  }
  return true;
}

bool hasFunclet(const llvm::CallBase &Call, const llvm::Value *Pad) {
  const auto Bundle = Call.getOperandBundle("funclet");
  return Pad ? Call.getNumOperandBundles() == 1 && Bundle &&
                   Bundle->Inputs.size() == 1 && Bundle->Inputs[0].get() == Pad
             : !Call.getNumOperandBundles();
}

bool hasTarget(const llvm::CallBase &Call, va_t Target) {
  const auto *Callee = Call.getCalledFunction();
  if (!Callee || Callee->isIntrinsic() || !Callee->isDeclaration() ||
      !Callee->hasExternalLinkage() ||
      Callee->getParent() != Call.getModule() || Call.isInlineAsm())
    return false;
  auto Original = rewrite_source::getOriginalVA(*Callee);
  if (!Original) {
    llvm::consumeError(Original.takeError());
    return false;
  }
  return *Original && **Original == Target;
}
} // namespace

llvm::Expected<CxxCookieCoverage> bindCxxCookieCoverage(
    const BinaryImage &Image, const MedFunc &Source,
    const llvm::Function &Parent, const llvm::AllocaInst &Frame,
    const X86RegistrationFrameLayout &Layout,
    llvm::ArrayRef<CxxSourceCallBinding> Calls,
    llvm::ArrayRef<CxxCleanupCallBinding> Cleanups,
    const std::map<X86RegistrationCatchIdentity, const llvm::CatchPadInst *>
        &Catches,
    size_t &Work) {
  auto Charge = [&] {
    if (Work >= limits::kMaxRegistrationEHStateWork) {
      Work = limits::kMaxRegistrationEHStateWork;
      return false;
    }
    ++Work;
    return true;
  };
  if (!Charge() || !Parent.getParent() || Parent.isDeclaration() ||
      !Frame.getParent() || Frame.getFunction() != &Parent ||
      &Parent.getEntryBlock() != Frame.getParent() || !Frame.isStaticAlloca() ||
      Frame.getAddressSpace() ||
      Parent.getParent()->getDataLayout().getPointerSize() != 4 ||
      Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || !Source.ExceptionMetadata ||
      !Source.ExceptionMetadata->Registration || !Source.RegistrationStates ||
      Source.SkippedSSA || !Source.Entry || Source.Entry > UINT32_MAX ||
      Source.Entry != Source.ExceptionMetadata->CodeRange.Begin)
    return reject("missing current PE32 parent or source frame");
  auto ParentVA = rewrite_source::getOriginalVA(Parent);
  if (!ParentVA)
    return ParentVA.takeError();
  if (*ParentVA != Source.Entry)
    return reject("current parent changed its original source identity");
  const auto &EH = *Source.ExceptionMetadata;
  const auto &States = *Source.RegistrationStates;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.ChainOperationsComplete ||
      !States.IncomingFrameAccessesComplete ||
      !States.CallFrameEffectsComplete || !States.CleanupFrameEffectsComplete ||
      !States.CxxContinuationsComplete || !States.CxxCatchObjectsComplete ||
      !States.RuntimeObjectAccessesComplete || !States.ImageReadsComplete ||
      !States.SecurityCookiesComplete || !EH.Registration->RegistrationOffset)
    return reject("source cookie and lifetime closure is incomplete");
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, EH);
  const auto Coordinate = cxxRegistrationFrameCoordinate(Source);
  const auto Size =
      Frame.getAllocationSize(Parent.getParent()->getDataLayout());
  const auto CurrentLayout = Coordinate && Size && !Size->isScalable()
                                 ? projectX86RegistrationFrame(
                                       *Coordinate, Size->getFixedValue(),
                                       Layout.EntrySP, Frame.getAlign().value())
                                 : std::nullopt;
  if (!Handler || Handler->CookieGuards.empty() || !CurrentLayout ||
      CurrentLayout->Size != Layout.Size ||
      CurrentLayout->Establisher != Layout.Establisher ||
      Handler->SecurityCookieVA != States.SecurityCookieVA ||
      Handler->CookieGuards.size() != States.CxxCookieConditions.size())
    return reject("handler guards or physical frame projection changed");
  std::vector<CookieFrameGuard> Guards;
  for (size_t I = 0; I != Handler->CookieGuards.size(); ++I) {
    const auto &Guard = Handler->CookieGuards[I];
    const int64_t Slot =
        int64_t(*EH.Registration->RegistrationOffset) + Guard.CookieOffset;
    const int64_t Base =
        int64_t(*EH.Registration->RegistrationOffset) + Guard.XOROffset;
    if (!Charge() || Slot < INT32_MIN || Slot > INT32_MAX || Base < INT32_MIN ||
        Base > INT32_MAX || States.CxxCookieConditions[I].FrameOffset != Slot ||
        States.CxxCookieConditions[I].XORFrameOffset != Base ||
        Guard.CheckVA != States.CookieCheckVA)
      return reject("node-relative guard differs from its source condition");
    const auto ProjectedSlot = Layout.runtimeOffset(int32_t(Slot), 4);
    const auto ProjectedBase = Layout.runtimeOffset(int32_t(Base), 0);
    if (!ProjectedSlot || !ProjectedBase)
      return reject("guard leaves the current logical allocation");
    Guards.push_back(
        {{&Frame, *ProjectedSlot}, {&Frame, *ProjectedBase}, Guard.CheckVA});
  }
  const auto CatchOwners = registrationCatchBlocks(Source);
  const auto CleanupParents = registrationCleanupParents(Source);
  if (!CatchOwners || !CleanupParents)
    return reject("source runtime invocation ownership is incomplete");
  std::set<const llvm::CatchPadInst *> Pads;
  size_t CatchCount = 0;
  for (uint32_t Region = 0; Region != EH.Cxx->TryBlocks.size(); ++Region)
    for (uint32_t Clause = 0;
         Clause != EH.Cxx->TryBlocks[Region].Handlers.size(); ++Clause) {
      const auto Found = Catches.find({Region, Clause});
      if (!Charge() || Found == Catches.end() || !Found->second ||
          !Found->second->getParent() ||
          Found->second->getFunction() != &Parent ||
          !Pads.insert(Found->second).second)
        return reject("current catch identities are incomplete or duplicated");
      ++CatchCount;
    }
  if (CatchCount != Catches.size())
    return reject("current catch identity set has extra entries");

  using Occurrence = std::pair<va_t, int>;
  using Key = std::tuple<int, va_t, int, CxxSourceCallBinding::Kind>;
  std::map<Key, const CxxSourceCallBinding *> Bindings;
  std::set<const llvm::CallBase *> CurrentCalls;
  for (const auto &Binding : Calls)
    if (!Charge() || Binding.Owner != Source.Entry || Binding.BlockId < 0 ||
        Binding.OpSeq < 0 || !Binding.Call || !Binding.Call->getParent() ||
        Binding.Call->getFunction() != &Parent || !hasSourceIdentity(Binding) ||
        !CurrentCalls.insert(Binding.Call).second ||
        !Bindings
             .emplace(Key{Binding.BlockId, Binding.Address, Binding.OpSeq,
                          Binding.CallKind},
                      &Binding)
             .second)
      return reject("source call binding is foreign, duplicated or changed");
  std::map<Occurrence, const RegistrationCookieCheck *> Checks;
  for (const auto &Check : States.CookieChecks)
    if (!Charge() ||
        !Checks.emplace(Occurrence{Check.Address, Check.OpSeq}, &Check).second)
      return reject("source cookie occurrence is duplicated");
  std::map<Occurrence, const RegistrationCallFrameEffect *> Effects;
  for (const auto &Effect : States.CallFrameEffects) {
    const Occurrence ID{Effect.Address, Effect.OpSeq};
    if (!Charge() || Effect.CalleeIndex >= States.CalleeContracts.size() ||
        States.CalleeContracts[Effect.CalleeIndex].Target != Effect.Target ||
        Checks.count(ID) || !Effects.emplace(ID, &Effect).second)
      return reject("source call has conflicting effect authorities");
  }
  using Range = std::pair<va_t, va_t>;
  std::map<Range, const RegistrationBlockState *> Reached;
  std::set<int> StateIDs;
  for (const auto &State : States.Blocks) {
    if (!Charge())
      return reject("source blocks exceed the shared work budget");
    if (State.Reached &&
        (State.Unknown || State.BlockId < 0 || !State.Range.isValid() ||
         !StateIDs.insert(State.BlockId).second ||
         !Reached.emplace(Range{State.Range.Begin, State.Range.End}, &State)
              .second))
      return reject("reached source blocks are ambiguous");
  }
  CxxCookieCoverage Result;
  Result.SecurityCookieVA = Handler->SecurityCookieVA;
  std::set<Occurrence> SeenChecks, SeenEffects;
  std::set<int> SeenBlocks;
  for (const auto &Block : Source.Blocks) {
    if (!Charge())
      return reject("source replay exceeds the shared work budget");
    const auto Found = Reached.find({Block.StartAddr, Block.EndAddr});
    if (Found == Reached.end())
      continue;
    const auto &State = *Found->second;
    if (!SeenBlocks.insert(State.BlockId).second)
      return reject("source block has more than one current MedIR owner");
    for (const auto &Op : Block.Ops) {
      if (!Charge() || Op.Opcode == NdOp::INDIR_CALL)
        return reject("source call has no bounded direct occurrence");
      if (Op.Opcode != NdOp::CALL)
        continue;
      const Occurrence ID{Op.Addr, Op.OriginSeq};
      const auto Check = Checks.find(ID);
      const auto Effect = Effects.find(ID);
      const bool IsCookie = Check != Checks.end();
      if (!IsCookie && Effect == Effects.end())
        return reject("reached source call has no effect receipt");
      const va_t End =
          IsCookie ? Check->second->EndAddress : Effect->second->EndAddress;
      const va_t Target =
          IsCookie ? States.CookieCheckVA : Effect->second->Target;
      const auto Kind = IsCookie ? CxxSourceCallBinding::Kind::Cookie
                                 : CxxSourceCallBinding::Kind::Ordinary;
      auto Binding =
          Bindings.find({State.BlockId, Op.Addr, Op.OriginSeq, Kind});
      if (Binding == Bindings.end() || Op.Dead || Op.OriginSeq < 0 ||
          Op.Addr < Block.StartAddr || End <= Op.Addr || End > Block.EndAddr ||
          Binding->second->EndAddress != End || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
          Op.Inputs[0].ConstVal != Target ||
          !(IsCookie ? SeenChecks : SeenEffects).insert(ID).second)
        return reject("source call occurrence was omitted or altered");
      const auto &Call = *Binding->second->Call;
      const auto Owner = CatchOwners->find(Block.Id);
      const llvm::Value *Pad =
          Owner == CatchOwners->end() ? nullptr : Catches.at(Owner->second);
      if (State.CallbackOnly != bool(Pad) || !hasFunclet(Call, Pad) ||
          !hasTarget(Call, Target))
        return reject("current source call changed its target or funclet");
      if (IsCookie) {
        if (!hasExactCookieCallABI(Call, Target))
          return reject("retained cookie changed its physical ECX ABI");
        Result.Calls.push_back({&Call, Target});
      } else if (State.CanDispatch) {
        // A live level -1 call still dispatches through the GS handler even
        // when no local cleanup/catch requires an LLVM invoke.
        Result.Dispatches.emplace(&Call, Guards);
      }
      Bindings.erase(Binding);
      if (!IsCookie && Effect->second->DoesNotReturn)
        break;
    }
  }
  if (SeenBlocks.size() != Reached.size() || !Bindings.empty() ||
      SeenChecks.size() != Checks.size() ||
      SeenEffects.size() != Effects.size())
    return reject("source/current call coverage is incomplete");

  const std::array<const llvm::Function *, 1> Functions{&Parent};
  coff_registration::RegistrationFrameStores Stores;
  if (!Stores.build(Functions, Work))
    return reject("current instruction index exceeds its work budget");
  bool Exhausted = false;
  AddressProof Addresses(Parent, nullptr, Result.SecurityCookieVA, Stores, Work,
                         Exhausted);
  using CleanupKey = std::pair<uint32_t, uint32_t>;
  std::map<CleanupKey, const CxxCleanupCallBinding *> CleanupCalls;
  for (const auto &Binding : Cleanups)
    if (!Charge() || Binding.Owner != Source.Entry || !Binding.Call ||
        !Binding.Pad || !Binding.Pad->getParent() ||
        Binding.Pad->getFunction() != &Parent || !Binding.Call->getParent() ||
        Binding.Call->getParent() != Binding.Pad->getParent() ||
        !CurrentCalls.insert(Binding.Call).second ||
        !CleanupCalls
             .emplace(CleanupKey{Binding.ActionState, Binding.Ordinal},
                      &Binding)
             .second)
      return reject("cleanup occurrence is foreign or duplicated");
  std::set<uint32_t> Actions;
  std::set<const llvm::CleanupPadInst *> CleanupPads;
  for (const auto &Contract : States.CleanupContracts) {
    if (!Charge() || Contract.ActionState >= EH.Cxx->UnwindMap.size() ||
        !Actions.insert(Contract.ActionState).second ||
        !CleanupParents->count(Contract.ActionState) ||
        Contract.RelayTarget !=
            EH.Cxx->UnwindMap[Contract.ActionState].ActionVA)
      return reject("cleanup source action changed");
    const auto Relay = getCheckedX86RegistrationCleanupRelayABI(
        Image, Contract.RelayTarget, &Work);
    if (!Relay || !Relay->matchesParentFrame(*EH.Registration) ||
        Contract.Calls.empty() || Contract.Calls.size() != Relay->Calls.size())
      return reject("cleanup relay lost its current ordered byte proof");
    const llvm::CleanupPadInst *Pad = nullptr;
    std::vector<const llvm::CallBase *> Ordered;
    for (uint32_t I = 0; I != Contract.Calls.size(); ++I) {
      const auto Binding = CleanupCalls.find({Contract.ActionState, I});
      const auto &Fresh = Relay->Calls[I];
      const auto &Saved = Contract.Calls[I];
      if (!Charge() || Binding == CleanupCalls.end() ||
          Binding->second->RelayTarget != Contract.RelayTarget ||
          Saved.ObjectFrameOffset != Fresh.ObjectFrameOffset ||
          !hasSameX86RegistrationLeafEffects(Saved.Leaf, Fresh.Leaf, Work) ||
          (Pad && Pad != Binding->second->Pad))
        return reject("cleanup transfer identity or order changed");
      Pad = Binding->second->Pad;
      const auto &Call = *Binding->second->Call;
      const auto Offset =
          EH.Registration->cxxSourceFrameOffset(Saved.ObjectFrameOffset);
      const auto Projected =
          Offset ? Layout.runtimeOffset(*Offset, 0) : std::nullopt;
      const auto Actual = Call.arg_size() == 1
                              ? Addresses.locate(Call.getArgOperand(0))
                              : std::nullopt;
      if (!llvm::isa<llvm::CallInst>(Call) ||
          !hasTarget(Call, Fresh.Leaf.Target) || !hasFunclet(Call, Pad) ||
          !Projected || !Actual || *Actual != Cell{&Frame, *Projected} ||
          Call.getMetadata(windows_eh_md::RegistrationOperationAttachment))
        return reject("cleanup call changed its current object or invocation");
      Ordered.push_back(&Call);
      Result.Dispatches.emplace(&Call, Guards);
      CleanupCalls.erase(Binding);
    }
    const auto &Owner = CleanupParents->at(Contract.ActionState);
    const llvm::Value *ParentPad =
        Owner ? static_cast<const llvm::Value *>(Catches.at(*Owner))
              : llvm::ConstantTokenNone::get(Parent.getContext());
    if (!CleanupPads.insert(Pad).second || Pad->getParentPad() != ParentPad)
      return reject("cleanup changed its active parent invocation");
    size_t Index = 0;
    for (const auto &I : *Pad->getParent()) {
      if (!Charge())
        return reject("cleanup execution exceeds the shared work budget");
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
          Call && !llvm::isa<llvm::IntrinsicInst>(Call))
        if (Index == Ordered.size() || Ordered[Index++] != Call)
          return reject("cleanup current call order differs from the relay");
    }
    if (Index != Ordered.size())
      return reject("cleanup transfer set is incomplete");
  }
  if (!CleanupCalls.empty() || Actions.size() != CleanupParents->size())
    return reject("cleanup coverage omitted or added a source action");
  for (const auto &Block : Parent)
    for (const auto &I : Block) {
      if (!Charge())
        return reject("current call closure exceeds the shared work budget");
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
          Call && !llvm::isa<llvm::IntrinsicInst>(Call) &&
          !CurrentCalls.erase(Call))
        return reject("current function gained an unbound external call");
    }
  if (Exhausted || !CurrentCalls.empty())
    return reject("current call closure is incomplete");
  return Result;
}
} // namespace neverd::registration_frame
