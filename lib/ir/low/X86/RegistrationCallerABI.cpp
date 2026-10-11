//===- RegistrationCallerABI.cpp - PE32 caller frame closure --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <deque>
#include <set>

namespace neverd {
namespace {
using registration_abi::callerPCIsNotReadBack;
using registration_abi::chargeCalleeWork;
using registration_abi::hasPrivateCallerFrame;
using registration_abi::ImageFrameEffects;
} // namespace

bool hasCallerCleanupRegistrationABI(
    const LowFunc &Function, const BinaryImage &Image,
    std::vector<ExceptionAddressRange> *CallerPCWrites) {
  if (CallerPCWrites)
    CallerPCWrites->clear();
  if (Image.Arch != Arch::X86 || Image.Format != BinaryFormat::COFF ||
      Function.CalleePopBytes || !Function.hasCompleteLiftCoverage())
    return false;
  size_t Work = 0;
  const auto *States =
      Function.RegistrationStates ? &*Function.RegistrationStates : nullptr;
  const bool UseReachability =
      Function.ExceptionMetadata && Function.ExceptionMetadata->Cxx &&
      Function.ExceptionMetadata->Registration && States && States->Complete &&
      States->CallbackStatesComplete && States->RegistrationLifetimeComplete &&
      States->CallFrameEffectsComplete;
  std::map<int, const RegistrationBlockState *> Reachability;
  if (UseReachability) {
    if (States->Blocks.size() != Function.Blocks.size() ||
        !chargeCalleeWork(Work, States->Blocks.size()))
      return false;
    for (const auto &State : States->Blocks)
      if (!Reachability.emplace(State.BlockId, &State).second)
        return false;
  }
  std::set<va_t> Targets;
  for (const auto &Block : Function.Blocks) {
    const RegistrationBlockState *State = nullptr;
    if (UseReachability) {
      const auto It = Reachability.find(Block.Id);
      if (It == Reachability.end() ||
          It->second->Range.Begin != Block.StartAddr ||
          It->second->Range.End != Block.EndAddr)
        return false;
      State = It->second;
    }
    for (const auto &Op : Block.Ops) {
      if (!chargeCalleeWork(Work, 1))
        return false;
      if (State && !State->Reached)
        continue;
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
          !Image.isCodeAddress(Op.Inputs[0].Offset))
        return false;
      if (UseReachability) {
        const auto *Call = States->callFrameEffect(Op.Addr, Op.Seq);
        if (!Call || Call->Target != Op.Inputs[0].Offset ||
            !chargeCalleeWork(Work, Block.InstructionBoundaries.size()))
          return false;
        const auto Boundary =
            llvm::find_if(Block.InstructionBoundaries,
                          [&](const auto &B) { return B.Address == Op.Addr; });
        if (Boundary == Block.InstructionBoundaries.end() ||
            Boundary->Control != LowInstructionControl::Call ||
            Op.Addr + Boundary->Size != Call->EndAddress)
          return false;
      }
      if (Function.RegistrationStates &&
          Function.RegistrationStates->SecurityCookiesComplete &&
          Function.RegistrationStates->cookieCheck(Op.Addr, Op.Seq)) {
        const va_t Check = Function.RegistrationStates->CookieCheckVA;
        if (Check != Op.Inputs[0].Offset ||
            !coff_loader::hasCheckedX86CookieCheckSuccessPath(Image, Check))
          return false;
        continue;
      }
      if (Function.ExceptionMetadata &&
          Function.ExceptionMetadata->Registration &&
          Function.RegistrationStates)
        for (const auto &Scope :
             Function.ExceptionMetadata->Registration->Scopes)
          if (Scope.IsFinally && Scope.HandlerVA == Op.Inputs[0].Offset)
            for (const auto &State : Function.RegistrationStates->Blocks)
              if (State.BlockId == Block.Id && State.CallbackOnly)
                return false;
      Targets.insert(Op.Inputs[0].Offset);
      if (Targets.size() > 256)
        return false;
    }
  }
  Decoder Decoder;
  if (!Decoder.init(Image))
    return false;
  CFGBuilder Builder;
  std::deque<va_t> Pending(Targets.begin(), Targets.end());
  std::set<va_t> Examined;
  ImageFrameEffects Effects;
  while (!Pending.empty()) {
    const va_t Target = Pending.front();
    Pending.pop_front();
    if (!Examined.insert(Target).second)
      continue;
    if (Examined.size() > 256)
      return false;
    bool OutlinedFinally = false;
    if (Function.ExceptionMetadata && Function.ExceptionMetadata->Registration)
      for (const auto &Scope : Function.ExceptionMetadata->Registration->Scopes)
        if (Target == Scope.FilterVA || Target == Scope.HandlerVA) {
          if (!Scope.IsFinally)
            return false;
          OutlinedFinally = true;
        }
    // Native lowering explicitly replaces ordinary termination calls with
    // its recovered (abnormal, parent-frame) callback ABI.
    if (OutlinedFinally)
      continue;
    if (Target == Function.Entry)
      // A recursive source call can pass values through the synthetic stack.
      // Caller cleanup alone does not project those slots onto the recursive
      // invocation's physical argument area.
      return false;
    if (Function.ExceptionMetadata && Function.ExceptionMetadata->Cxx &&
        Function.ExceptionMetadata->Registration)
      if (const auto Throw =
              getCheckedX86RegistrationThrowCalleeABI(Image, Target, &Work)) {
        if (!chargeCalleeWork(Work, Throw->ImageReads.size() +
                                        Throw->ImageWrites.size() +
                                        Throw->CallerPCWrites.size()))
          return false;
        for (const auto &Read : Throw->ImageReads)
          Effects.Reads.emplace(Read.Begin, Read.End);
        for (const auto &Write : Throw->ImageWrites)
          Effects.Writes.emplace(Write.Begin, Write.End);
        for (const auto &Write : Throw->CallerPCWrites)
          Effects.CallerPCWrites.emplace(Write.Begin, Write.End);
        continue;
      }
    if (UseReachability)
      if (const auto Leaf =
              getCheckedX86RegistrationLeafCalleeABI(Image, Target, &Work)) {
        if (!chargeCalleeWork(Work, Leaf->ImageReads.size() +
                                        Leaf->ImageWrites.size() +
                                        Leaf->CallerPCWrites.size()))
          return false;
        for (const auto &Read : Leaf->ImageReads)
          Effects.Reads.emplace(Read.Begin, Read.End);
        for (const auto &Write : Leaf->ImageWrites)
          Effects.Writes.emplace(Write.Begin, Write.End);
        for (const auto &Write : Leaf->CallerPCWrites)
          Effects.CallerPCWrites.emplace(Write.Begin, Write.End);
        continue;
      }
    const LowFunc Callee = Builder.build(Image, Decoder, Target, "abi-callee");
    if (Callee.CalleePopBytes || !Callee.hasCompleteLiftCoverage() ||
        Callee.Blocks.empty() ||
        !hasPrivateCallerFrame(Callee, Image, Work, Effects))
      return false;
    // A jump to an unexamined tail callee cannot establish its return cleanup.
    for (const auto &Block : Callee.Blocks)
      if (Block.Succs.empty() &&
          (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN))
        return false;
    // Check the complete preserved internal-call closure. An opaque helper
    // cannot hide a frame-chain observer one call below the source function.
    for (const auto &Block : Callee.Blocks)
      for (const auto &Op : Block.Ops) {
        if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
          continue;
        if (Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
            Op.Inputs[0].Size != 4)
          return false;
        const va_t Next = Op.Inputs[0].Offset;
        if (Image.findImportAt(Next)) {
          // Dispatch intentionally consumes the live registration chain.
          // Other opaque APIs need a separate frame-observation contract.
          if (!registration_abi::checkedRegistrationImportStackPop(Image, Next))
            return false;
          continue;
        }
        if (Op.Opcode != NdOp::CALL || !Image.isCodeAddress(Next))
          return false;
        if (Function.ExceptionMetadata &&
            Function.ExceptionMetadata->Registration)
          for (const auto &Scope :
               Function.ExceptionMetadata->Registration->Scopes)
            if (Next == Scope.FilterVA || Next == Scope.HandlerVA)
              return false;
        Pending.push_back(Next);
        if (Pending.size() > 256)
          return false;
      }
  }
  if (Function.ExceptionMetadata &&
      (Function.ExceptionMetadata->Personality ==
           ExceptionPersonality::ExceptHandler3 ||
       Function.ExceptionMetadata->Personality ==
           ExceptionPersonality::ExceptHandler4) &&
      Function.ExceptionMetadata->Registration) {
    const auto Table = coff_loader::getX86RegistrationSEHScopeTableRange(
        *Function.ExceptionMetadata);
    if (!Table)
      return false;
    const bool EH4 = Function.ExceptionMetadata->Personality ==
                     ExceptionPersonality::ExceptHandler4;
    const va_t CookieVA = Image.Base + Image.DynInfo.SecurityCookieRVA;
    if (EH4 && (!Image.DynInfo.SecurityCookieRVA || CookieVA > UINT32_MAX - 3))
      return false;
    for (const auto &[Begin, End] : Effects.Writes)
      if ((EH4 && Begin < CookieVA + 4 && CookieVA < End) ||
          Table->overlaps({Begin, End}))
        return false;
  }
  if (Function.ExceptionMetadata && Function.ExceptionMetadata->Cxx &&
      Function.ExceptionMetadata->Registration) {
    const auto &EH = *Function.ExceptionMetadata;
    const auto Ranges = coff_loader::getCheckedX86CxxMetadataRanges(Image, EH);
    if (!Ranges)
      return false;
    // The language dispatcher also calls these preserved actions. Their
    // physical leaf proof supplies image effects, never a parent-object borrow.
    for (const auto &Action : EH.Cxx->UnwindMap) {
      if (!chargeCalleeWork(Work, 1))
        return false;
      if (!Action.ActionVA)
        continue;
      if (Action.Kind != CxxUnwindAction::ActionKind::Direct ||
          Action.ObjectOffset)
        return false;
      const auto Relay = getCheckedX86RegistrationCleanupRelayABI(
          Image, Action.ActionVA, &Work);
      if (!Relay || !Relay->Leaf.CallerPCWrites.empty() ||
          !chargeCalleeWork(Work, Relay->Leaf.ImageReads.size() +
                                      Relay->Leaf.ImageWrites.size()))
        return false;
      for (const auto &Read : Relay->Leaf.ImageReads)
        Effects.Reads.emplace(Read.Begin, Read.End);
      for (const auto &Write : Relay->Leaf.ImageWrites)
        Effects.Writes.emplace(Write.Begin, Write.End);
    }
    if (Ranges->size() &&
        Effects.Writes.size() >
            (limits::kMaxRegistrationEHStateWork - Work) / Ranges->size())
      return false;
    Work += Effects.Writes.size() * Ranges->size();
    for (const auto &[Begin, End] : Effects.Writes)
      for (const auto &Range : *Ranges)
        if (Range.overlaps({Begin, End}))
          return false;
  }
  if (Effects.CallerPCWrites.empty())
    return true;
  // The real return PC can be exposed to an external observer, but a reload
  // inside the rewritten source/callee closure could erase its provenance and
  // use the regenerated address in a branch, arithmetic or dereference.
  if (Function.RegistrationStates) {
    if (!Function.RegistrationStates->ImageReadsComplete)
      return false;
    for (const auto &Read : Function.RegistrationStates->ImageReads)
      Effects.Reads.emplace(Read.Begin, Read.End);
  } else
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode != NdOp::STORE && lowMemoryOperands(Op).Address)
          return false;
  // Sort both sets once, then walk their interval fronts. Avoid multiplying
  // the bounded source/callee footprints by one another.
  if (!callerPCIsNotReadBack(Effects))
    return false;
  if (CallerPCWrites)
    for (const auto &[Begin, End] : Effects.CallerPCWrites) {
      if (!CallerPCWrites->empty() && Begin <= CallerPCWrites->back().End)
        CallerPCWrites->back().End = std::max(CallerPCWrites->back().End, End);
      else
        CallerPCWrites->push_back({Begin, End});
    }
  return true;
}
} // namespace neverd
