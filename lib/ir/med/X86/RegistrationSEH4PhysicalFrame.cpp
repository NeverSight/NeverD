//===- RegistrationSEH4PhysicalFrame.cpp - Shared SEH4 replay -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replay all physical helper operations without replacing original source
/// facts.
//===----------------------------------------------------------------------===//

#include "RegistrationSEH4PhysicalFrame.h"

#include "RegistrationHelperExpansion.h"
#include "RegistrationPhysicalFrame.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/RegistrationSharedCall.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace neverd {
namespace {

bool complete(const RegistrationStateAnalysis &S) {
  return S.Complete && S.CallbackStatesComplete &&
         S.RegistrationLifetimeComplete && S.ChainOperationsComplete &&
         S.IncomingFrameAccessesComplete && S.SecurityCookiesComplete &&
         S.ImageReadsComplete && S.SEHInvocationExitsComplete &&
         S.SEHParentChainComplete && S.SEHStateStoresComplete &&
         S.SEHExceptionAccessesComplete && S.SEHThreadAccessesComplete &&
         S.SEHBoundCallsComplete && S.SEHLocalUnwindsComplete &&
         S.SEHJumpCapturesComplete && S.SEHJumpResumptionsComplete;
}

bool sameFacts(const RegistrationStateAnalysis &A,
               const RegistrationStateAnalysis &B, va_t Entry,
               const std::vector<X86RegistrationPhysicalExit> &Exits) {
  if (!complete(A) || !complete(B) || A.CxxPrologue || B.CxxPrologue ||
      !A.CxxCookieConditions.empty() || !B.CxxCookieConditions.empty())
    return false;
  size_t Work = 0;
  for (const auto *S : {&A, &B})
    for (size_t Count :
         {S->Blocks.size(), S->SEHControls.size(), S->SEHInvocationExits.size(),
          S->SEHStateStores.size(), S->SEHExceptionAccesses.size(),
          S->SEHThreadAccesses.size(), S->SEHBoundCalls.size(),
          S->SEHLocalUnwinds.size(), S->SEHJumpCaptures.size(),
          S->SEHJumpResumptions.size(), S->IncomingFrameAccesses.size(),
          S->CookieChecks.size(), S->ImageReads.size(),
          S->FrameValues.size()}) {
      if (Count > limits::kMaxRegistrationEHStateWork - Work)
        return false;
      Work += Count;
    }
  const auto BlockKey = [](const RegistrationBlockState &S) {
    return std::tie(S.BlockId, S.Range.Begin, S.Range.End, S.levels(),
                    S.Unknown, S.CallbackOnly, S.CanDispatch,
                    S.CxxMinimumTryLevel, S.Reached, S.CxxSearches,
                    S.CxxCatchStacks);
  };
  const auto CookieKey = [](const RegistrationCookieCheck &C) {
    return std::tie(C.Address, C.EndAddress, C.OpSeq);
  };
  if (A.Blocks.size() != B.Blocks.size() ||
      !std::equal(A.Blocks.begin(), A.Blocks.end(), B.Blocks.begin(),
                  [&](const auto &X, const auto &Y) {
                    return BlockKey(X) == BlockKey(Y);
                  }) ||
      A.SEHControls != B.SEHControls ||
      A.SEHInvocationExits != B.SEHInvocationExits ||
      A.SEHStateStores != B.SEHStateStores ||
      A.SEHExceptionAccesses != B.SEHExceptionAccesses ||
      A.SEHThreadAccesses != B.SEHThreadAccesses ||
      A.HasBoundSEHCallees != B.HasBoundSEHCallees ||
      A.SEHBoundCalls != B.SEHBoundCalls ||
      A.HasSEHLocalUnwinds != B.HasSEHLocalUnwinds ||
      A.SEHLocalUnwinds != B.SEHLocalUnwinds ||
      A.HasSEHJumpCaptures != B.HasSEHJumpCaptures ||
      A.SEHJumpCaptures != B.SEHJumpCaptures ||
      A.SEHJumpResumptions != B.SEHJumpResumptions ||
      A.IncomingFrameAccesses != B.IncomingFrameAccesses ||
      A.SecurityCookieVA != B.SecurityCookieVA ||
      A.CookieCheckVA != B.CookieCheckVA ||
      A.CookieChecks.size() != B.CookieChecks.size() ||
      !std::equal(A.CookieChecks.begin(), A.CookieChecks.end(),
                  B.CookieChecks.begin(),
                  [&](const auto &X, const auto &Y) {
                    return CookieKey(X) == CookieKey(Y);
                  }) ||
      A.ImageReads.size() != B.ImageReads.size() ||
      !std::equal(A.ImageReads.begin(), A.ImageReads.end(),
                  B.ImageReads.begin(), [](const auto &X, const auto &Y) {
                    return X.Begin == Y.Begin && X.End == Y.End;
                  }))
    return false;
  std::set<va_t> Helpers{Entry};
  for (const auto &Exit : Exits)
    Helpers.insert(Exit.Source.Address);
  auto Left = A.FrameValues.begin();
  auto Right = B.FrameValues.begin();
  while (true) {
    while (Left != A.FrameValues.end() && Helpers.count(Left->Address))
      ++Left;
    while (Right != B.FrameValues.end() && Helpers.count(Right->Address))
      ++Right;
    if (Left == A.FrameValues.end() || Right == B.FrameValues.end())
      return Left == A.FrameValues.end() && Right == B.FrameValues.end();
    if (std::tie(Left->Address, Left->OpSeq, Left->EstablishedFrameOffset) !=
        std::tie(Right->Address, Right->OpSeq, Right->EstablishedFrameOffset))
      return false;
    ++Left;
    ++Right;
  }
}

bool sameEntry(const coff_loader::X86SEH4PrologueCall &A,
               const coff_loader::X86SEH4PrologueCall &B) {
  return A.Caller.Begin == B.Caller.Begin && A.Caller.End == B.Caller.End &&
         A.Helper.Begin == B.Helper.Begin && A.Helper.End == B.Helper.End &&
         A.CallVA == B.CallVA && A.HandlerVA == B.HandlerVA &&
         A.ScopeTableVA == B.ScopeTableVA &&
         A.SecurityCookieVA == B.SecurityCookieVA &&
         A.AllocationBytes == B.AllocationBytes &&
         A.PostCallStackOffset == B.PostCallStackOffset && A.Writes == B.Writes;
}

bool foldCheckedAllocation(LowFunc &Physical,
                           const coff_loader::X86SEH4PrologueCall &Entry,
                           const RegistrationStateAnalysis &States) {
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  const auto AX = NdVar::reg(x86reg::RAX, 4);
  LowOp *Allocation = nullptr;
  for (auto &Block : Physical.Blocks)
    if (Block.StartAddr == Entry.Caller.Begin)
      for (auto &Op : Block.Ops)
        if (Op.Addr == Entry.CallVA && Op.Opcode == NdOp::INT_SUB &&
            Op.Output == SP && Op.NumInputs == 2 && Op.Inputs[1] == AX) {
          if (Allocation || Op.Inputs[0] != SP)
            return false;
          Allocation = &Op;
        }
  if (!Allocation)
    return false;
  const auto Value = llvm::find_if(States.FrameValues, [&](const auto &V) {
    return V.Address == Allocation->Addr && V.OpSeq == Allocation->Seq;
  });
  if (Value == States.FrameValues.end() ||
      Value->EstablishedFrameOffset != int64_t(Entry.PostCallStackOffset) + 16)
    return false;
  // The authenticated prefix pushes this allocation word, and the complete
  // helper replay above proves the result before any partial evaluation.
  // Expose its fixed size to ordinary stack classification; leaving a reload
  // here can make either output route invent a separate dynamic allocation.
  Allocation->Inputs[1] = NdVar::scalar(Entry.AllocationBytes, 4);
  return true;
}

} // namespace

namespace x86_registration {
std::optional<SEH4PhysicalProjection>
projectSEH4PhysicalFrame(const BinaryImage &Image, const LowFunc &Source) {
  const auto Entry = getCheckedX86RegistrationSEH4PrologueCall(Image, Source);
  if (!Entry || !complete(*Source.RegistrationStates))
    return std::nullopt;
  auto Temp = nextRegistrationHelperTemp(Source);
  if (!Temp)
    return std::nullopt;
  auto Frame = std::shared_ptr<X86RegistrationSEH4PhysicalFrame>(
      new X86RegistrationSEH4PhysicalFrame);
  Frame->Entry = *Source.RegistrationStates->SEH4Prologue;
  Frame->EntryCall = *Entry;
  LowFunc Physical = Source;
  std::vector<size_t> Expanded;
  for (size_t I = 0; I != Source.Blocks.size(); ++I) {
    const auto &Block = Source.Blocks[I];
    if (Block.Ops.empty())
      continue;
    const auto Exit =
        getCheckedX86RegistrationSEH4EpilogueCall(Image, Source, Block);
    const bool IsEntry = Block.StartAddr == Entry->Caller.Begin &&
                         Block.EndAddr == Entry->Caller.End;
    if (!IsEntry && !Exit)
      continue;
    const auto &Call = Block.Ops.back();
    auto Ops =
        expandRegistrationHelper(Image, Source.Entry, Call.Addr, Block.EndAddr,
                                 IsEntry ? Entry->Helper : Exit->Helper, *Temp);
    if (!Ops)
      return std::nullopt;
    if (Exit)
      Frame->Exits.push_back({{Call.Addr, Block.EndAddr, Call.Seq,
                               RegistrationChainAccess::Kind::Remove},
                              Exit->Helper.Begin});
    auto &Output = Physical.Blocks[I];
    Output.Ops.pop_back();
    Output.Ops.insert(Output.Ops.end(), Ops->first.begin(), Ops->first.end());
    auto &Boundary = Output.InstructionBoundaries.back();
    Boundary.OpCount = Ops->first.size();
    Boundary.Control = LowInstructionControl::None;
    Boundary.ControlFlags = LowInstructionControlFlag::None;
    Boundary.Immediate.reset();
    Expanded.push_back(I);
    Temp = Ops->second;
  }
  // Expanded helpers do not erase the stack and runtime contracts of the
  // ordinary calls left in the body. Recheck them against the current image
  // through the same owner used by ordinary CFG analysis.
  RegistrationCallCalleeIndex Callees(Image);
  const auto Stacks = Callees.stackContracts(Physical);
  const auto LocalUnwinds = Callees.localUnwindContracts(Physical);
  if (!Stacks || !LocalUnwinds)
    return std::nullopt;
  Frame->States = analyzeCheckedX86SEHStates(
      Image, Physical, &*Stacks, &*LocalUnwinds,
      &Source.RegistrationStates->SEHControls, &Callees);
  if (Frame->States.SEH4Prologue ||
      !sameFacts(*Source.RegistrationStates, Frame->States, Entry->CallVA,
                 Frame->Exits) ||
      !matchesPhysicalChain(Source.RegistrationStates->ChainAccesses,
                            {Frame->Entry.Address, Frame->Entry.EndAddress,
                             Frame->Entry.OpSeq,
                             RegistrationChainAccess::Kind::Install},
                            Frame->Exits, Frame->States.ChainAccesses))
    return std::nullopt;
  if (!foldCheckedAllocation(Physical, *Entry, Frame->States))
    return std::nullopt;
  // This is a private execution view. Keep source call boundaries and all
  // original receipts for callers; only the emitted operation count changes.
  for (size_t I : Expanded) {
    auto &Boundary = Physical.Blocks[I].InstructionBoundaries.back();
    const size_t Count = Boundary.OpCount;
    Boundary = Source.Blocks[I].InstructionBoundaries.back();
    Boundary.OpCount = Count;
  }
  return SEH4PhysicalProjection{std::move(Physical), std::move(Frame)};
}
} // namespace x86_registration

const RegistrationStateAnalysis *
getX86RegistrationSEHPhysicalStates(const MedFunc &Function) {
  if (!Function.RegistrationStates)
    return nullptr;
  const auto &Source = *Function.RegistrationStates;
  const auto *Frame = Function.RegistrationSEH4PhysicalFrame.get();
  if (!Source.SEH4Prologue)
    return Frame ? nullptr : &Source;
  if (!Frame || !Function.ExceptionMetadata || Function.SkippedSSA ||
      Function.Entry != Function.ExceptionMetadata->CodeRange.Begin ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Cxx ||
      Function.ExceptionMetadata->Registration->RealignedFrame ||
      *Source.SEH4Prologue != Frame->Entry ||
      !sameFacts(Source, Frame->States, Frame->Entry.Address, Frame->Exits) ||
      !x86_registration::matchesPhysicalChain(
          Source.ChainAccesses,
          {Frame->Entry.Address, Frame->Entry.EndAddress, Frame->Entry.OpSeq,
           RegistrationChainAccess::Kind::Install},
          Frame->Exits, Frame->States.ChainAccesses) ||
      !x86_registration::hasCurrentPhysicalChain(Function,
                                                 Frame->States.ChainAccesses))
    return nullptr;
  return &Frame->States;
}

bool hasCheckedX86RegistrationSEHPhysicalFrame(const BinaryImage &Image,
                                               const MedFunc &Function) {
  if (!getX86RegistrationSEHPhysicalStates(Function) ||
      !Function.ExceptionMetadata)
    return false;
  const auto Entry = coff_loader::getCheckedX86SEH4PrologueCall(
      Image, *Function.ExceptionMetadata);
  const auto *Frame = Function.RegistrationSEH4PhysicalFrame.get();
  if (!Frame)
    return !Entry;
  if (!Entry || !sameEntry(*Entry, Frame->EntryCall))
    return false;
  for (const auto &Exit : Frame->Exits) {
    const auto Current = coff_loader::getCheckedX86SEH4EpilogueCall(
        Image, *Function.ExceptionMetadata, Exit.Source.Address);
    if (!Current || Current->Helper.Begin != Exit.HelperVA ||
        Current->Caller.End != Exit.Source.EndAddress)
      return false;
  }
  return true;
}

std::optional<uint64_t>
getX86RegistrationSEHFrameStorage(const MedFunc &Function) {
  if (!Function.RegistrationSEH4PhysicalFrame &&
      (!Function.RegistrationStates ||
       !Function.RegistrationStates->SEH4Prologue))
    return 0;
  if (!getX86RegistrationSEHPhysicalStates(Function))
    return std::nullopt;
  // EBP is entry ESP - 4; the moved helper PC is another word below its
  // post-call ESP. Reserve that transient word even though RET consumes it.
  return uint64_t(int64_t(8) -
                  Function.RegistrationSEH4PhysicalFrame->Entry.StackOffset);
}

} // namespace neverd
