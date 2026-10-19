//===- X64SEH.cpp - Checked x64 C exception dispatch ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Plans C exception search and unwind using decoded loader metadata.
///
//===----------------------------------------------------------------------===//

#include "X64SEH.h"

#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <set>
#include <utility>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 seh::text::Prefix + Message);
}

bool nonvolatile(uint64_t Register) {
  return Register < seh::RegisterCount &&
         ((seh::NonvolatileRegisterMask >> Register) & 1);
}

bool hasLanguageHandler(const ExceptionFunction &Frame, bool Unwinding) {
  if (Frame.GSCookie)
    return Unwinding ? Frame.GSCookie->HasUnwindHandler
                     : Frame.GSCookie->HasExceptionHandler;
  return true;
}

llvm::Error validateFrame(const ExceptionFunction &F) {
  if (F.ParseStatus != ExceptionParseStatus::Complete)
    return invalid(seh::text::EncounteredIncompleteExceptionMetadata);
  if (F.Encoding != ExceptionEncoding::X64UnwindV1 || F.UnwindVersion != 1)
    return invalid(seh::text::OnlyX64V1UnwindRecordsAreSupported);
  const bool Chained = F.Kind == RuntimeFunctionKind::Chained;
  if (Chained != bool(F.UnwindFlags & seh::ChainFlag) ||
      Chained != F.ChainedPrimaryRange.has_value() ||
      (Chained && (!F.ChainedUnwindInfoRVA || !F.PrimaryFunctionIndex)) ||
      (!Chained && (F.ChainedUnwindInfoRVA || F.PrimaryFunctionIndex)) ||
      (F.Kind != RuntimeFunctionKind::Primary && !Chained))
    return invalid(seh::text::InconsistentChainedUnwindRecord);
  if (F.UnwindFlags & ~(seh::ExceptionHandlerFlag | seh::UnwindHandlerFlag |
                        seh::ChainFlag) ||
      (Chained && F.UnwindFlags != seh::ChainFlag))
    return invalid(seh::text::UnsupportedUnwindFlags);
  const bool StandaloneGS =
      F.Personality == ExceptionPersonality::GSHandlerCheck;
  const bool GS =
      StandaloneGS || F.Personality == ExceptionPersonality::GSHandlerCheckSEH;
  const bool CHandler =
      F.Personality == ExceptionPersonality::GSHandlerCheckSEH ||
      F.Personality == ExceptionPersonality::CSpecificHandler;
  if (!CHandler && !StandaloneGS && F.Personality != ExceptionPersonality::None)
    return invalid(seh::text::EncounteredUnsupportedLanguagePersonality);
  if (F.Cxx || GS != F.GSCookie.has_value() ||
      (((F.UnwindFlags & ~seh::ChainFlag) != 0) !=
       (CHandler || StandaloneGS)) ||
      (CHandler != F.SEH.has_value()))
    return invalid(seh::text::InconsistentCExceptionHandlerMetadata);
  if (GS) {
    const auto &Cookie = *F.GSCookie;
    if (Cookie.ParseStatus != ExceptionParseStatus::Complete ||
        Cookie.CookieOffset % int32_t(seh::PointerSize) ||
        (!StandaloneGS && Cookie.HasExceptionHandler &&
         !(F.UnwindFlags & seh::ExceptionHandlerFlag)) ||
        (!StandaloneGS && Cookie.HasUnwindHandler &&
         !(F.UnwindFlags & seh::UnwindHandlerFlag)) ||
        (Cookie.HasAlignment &&
         (!Cookie.Alignment || (Cookie.Alignment & (Cookie.Alignment - 1)))) ||
        (!Cookie.HasAlignment &&
         (Cookie.Alignment || Cookie.AlignmentBaseOffset)))
      return invalid(seh::text::InconsistentGsCookieMetadata);
  }
  if (F.FrameRegister && !nonvolatile(F.FrameRegister))
    return invalid(seh::text::InvalidFrameRegister);
  if (F.FrameOffset > seh::MaxFrameOffset ||
      F.FrameOffset % seh::FrameOffsetScale ||
      (!F.FrameRegister && F.FrameOffset))
    return invalid(seh::text::InvalidFrameRegisterOffset);
  if (F.UnwindOperations.size() > seh::MaxUnwindOperations)
    return invalid(seh::text::UnwindOperationLimitExceeded);
  uint32_t PreviousOffset = F.PrologueSize;
  unsigned FrameSetCount = 0;
  for (const auto &Op : F.UnwindOperations) {
    if (!Op.CodeOffset || Op.CodeOffset > PreviousOffset)
      return invalid(seh::text::InvalidUnwindOperationOrder);
    PreviousOffset = Op.CodeOffset;
    // Secondary prologues can add saves in the primary fixed allocation.
    // They cannot push registers or create another fixed allocation.
    if (Chained && Op.Kind != UnwindOperationKind::SaveNonVolatile &&
        Op.Kind != UnwindOperationKind::SaveNonVolatileFar &&
        Op.Kind != UnwindOperationKind::SaveXMM128 &&
        Op.Kind != UnwindOperationKind::SaveXMM128Far)
      return invalid(seh::text::ChainedUnwindChangesThePrimaryStackAllocation);
    switch (Op.Kind) {
    case UnwindOperationKind::PushNonVolatile:
    case UnwindOperationKind::SaveNonVolatile:
    case UnwindOperationKind::SaveNonVolatileFar:
      if (!nonvolatile(Op.Register))
        return invalid(seh::text::InvalidSavedNonvolatileRegister);
      if (Op.StackOffset % seh::PointerSize)
        return invalid(seh::text::UnalignedNonvolatileSaveOffset);
      break;
    case UnwindOperationKind::AllocateSmall:
      if (Op.StackOffset > seh::MaxSmallAllocation)
        return invalid(seh::text::InvalidSmallStackAllocation);
      [[fallthrough]];
    case UnwindOperationKind::AllocateLarge:
      if (!Op.StackOffset || Op.StackOffset % seh::PointerSize)
        return invalid(seh::text::InvalidStackAllocation);
      break;
    case UnwindOperationKind::SetFramePointer:
      if (!F.FrameRegister || ++FrameSetCount != 1)
        return invalid(seh::text::InvalidFramePointerOperation);
      break;
    case UnwindOperationKind::SaveXMM128:
    case UnwindOperationKind::SaveXMM128Far:
      if (Op.Register < seh::FirstNonvolatileXmm ||
          Op.Register >= seh::FirstNonvolatileXmm + seh::NonvolatileXmmCount)
        return invalid(seh::text::InvalidSavedNonvolatileXmmRegister);
      if (Op.StackOffset % seh::XmmSize)
        return invalid(seh::text::UnalignedXmmSaveOffset);
      break;
    default:
      return invalid(seh::text::EncounteredUnsupportedUnwindOperation);
    }
  }
  if (F.FrameRegister && !Chained && FrameSetCount != 1)
    return invalid(seh::text::FrameRegisterHasNoEstablishingOperation);
  return llvm::Error::success();
}
} // namespace

X64SEH::X64SEH(const ExceptionInfo &Metadata, uint64_t PreferredBase,
               uint64_t ActualBase, uint64_t ImageSize, ReadStack64 ReadStack,
               IsExecutable Executable, ReadCode Code,
               ReadSecurityCookie Cookie)
    : X64SEH({Image{&Metadata, PreferredBase, ActualBase, ImageSize,
                    std::move(Cookie)}},
             std::move(ReadStack), std::move(Executable), std::move(Code)) {}

X64SEH::X64SEH(std::vector<Image> Images, ReadStack64 ReadStack,
               IsExecutable Executable, ReadCode Code)
    : Images(std::move(Images)), ReadStack(std::move(ReadStack)),
      Executable(std::move(Executable)), Code(std::move(Code)) {}

llvm::Error X64SEH::validateImages(bool Live) const {
  if (Images.empty() || Images.size() > seh::MaxImages)
    return invalid(seh::text::InvalidExceptionImageCount);
  for (size_t I = 0; I < Images.size(); ++I) {
    const auto &A = Images[I];
    if (!A.Metadata || !A.Size || A.Size > UINT64_MAX - A.PreferredBase ||
        A.Size > UINT64_MAX - A.ActualBase)
      return invalid(seh::text::InvalidExceptionImageBounds);
    for (size_t J = 0; J < I; ++J) {
      const auto &B = Images[J];
      if (A.ActualBase < B.ActualBase + B.Size &&
          B.ActualBase < A.ActualBase + A.Size)
        return invalid(seh::text::OverlappingExceptionImages);
    }
  }
  if (Live)
    for (const auto &Image : Images)
      if (Image.Validate)
        if (auto E = Image.Validate())
          return E;
  return llvm::Error::success();
}
llvm::Expected<std::unique_ptr<X64SEH>>
X64SEH::withAdditionalImages(llvm::ArrayRef<Image> Added) const {
  if (Added.size() > seh::MaxImages ||
      Images.size() > seh::MaxImages - Added.size())
    return invalid(seh::text::InvalidExceptionImageCount);
  auto Next = std::make_unique<X64SEH>(*this);
  Next->Images.insert(Next->Images.end(), Added.begin(), Added.end());
  if (auto E = Next->validateImages())
    return std::move(E);
  return Next;
}
std::optional<size_t> X64SEH::imageFor(uint64_t PC) const {
  for (size_t I = 0; I < Images.size(); ++I)
    if (PC >= Images[I].ActualBase &&
        PC - Images[I].ActualBase < Images[I].Size)
      return I;
  return std::nullopt;
}

X64SEH::Dispatch X64SEH::begin(uint32_t ExceptionCode, const Context &Caller,
                               Stack Bounds) const {
  Dispatch State;
  State.Original = State.Current = Caller;
  State.Bounds = Bounds;
  State.Path.push_back({Caller, Bounds});
  State.Code = ExceptionCode;
  return State;
}

llvm::Expected<X64SEH::Dispatch>
X64SEH::beginNested(uint32_t ExceptionCode, const Context &Caller, Stack Bounds,
                    const Dispatch &Suspended, const Action &Callback) const {
  if (Suspended.Complete || Suspended.Path.empty() ||
      Callback.SegmentIndex >= Suspended.Path.size() ||
      (Callback.Kind != ActionKind::Filter &&
       Callback.Kind != ActionKind::Finally))
    return invalid(seh::text::NestedDispatchRequiresAnActiveSehCallback);
  auto State = begin(ExceptionCode, Caller, Bounds);
  if (Callback.Kind == ActionKind::Filter) {
    State.Path.insert(State.Path.end(), Suspended.Path.begin(),
                      Suspended.Path.end());
    for (size_t I = 0; I <= Callback.SegmentIndex; ++I) {
      const uint64_t Boundary = I == Callback.SegmentIndex
                                    ? Callback.State.EstablisherFrame
                                    : UINT64_MAX;
      auto &Segment = State.Path[I + 1];
      Segment.NestedFrame = std::max(Segment.NestedFrame, Boundary);
    }
  } else {
    auto Segment = Suspended.Path[Callback.SegmentIndex];
    Segment.Registers = Callback.State.Registers;
    Segment.Bounds = Callback.Bounds;
    Segment.ScopeIndex = Callback.ScopeIndex;
    State.Path.push_back(Segment);
    State.Path.insert(State.Path.end(),
                      Suspended.Path.begin() + Callback.SegmentIndex + 1,
                      Suspended.Path.end());
  }
  if (State.Path.size() > seh::MaxNestedExceptions + 1)
    return invalid(seh::text::NestedExceptionPathLimitExceeded);
  return State;
}

llvm::Expected<X64SEH::Action>
X64SEH::advance(Dispatch &State, std::optional<int32_t> FilterResult) const {
  if (auto E = validateImages(true))
    return std::move(E);
  Dispatch Candidate = State;
  auto Result = advanceImpl(Candidate, FilterResult);
  if (Result)
    State = std::move(Candidate);
  return Result;
}

llvm::Expected<X64SEH::Dispatch>
X64SEH::beginAfterRejectedContinuation(uint32_t ExceptionCode,
                                       const Context &Caller, Stack Bounds,
                                       const Dispatch &Rejected) const {
  if (!Rejected.Complete || !Rejected.FilterCandidate || Rejected.Selected ||
      Rejected.Path.empty())
    return invalid(seh::text::SecondaryDispatchRequiresRejectedContinuation);
  if (Rejected.Path.size() >= seh::MaxNestedExceptions + 1)
    return invalid(seh::text::NestedExceptionPathLimitExceeded);
  auto State = begin(ExceptionCode, Caller, Bounds);
  State.Path.insert(State.Path.end(), Rejected.Path.begin(),
                    Rejected.Path.end());
  return State;
}

llvm::Expected<std::optional<X64SEH::Transfer>>
X64SEH::plan(uint32_t ExceptionCode, const Context &Caller,
             Stack Bounds) const {
  auto State = begin(ExceptionCode, Caller, Bounds);
  auto Next = advance(State);
  if (!Next)
    return Next.takeError();
  if (Next->Kind == ActionKind::Handler)
    return std::optional<Transfer>{Next->State};
  if (Next->Kind == ActionKind::Unhandled)
    return std::optional<Transfer>{};
  return invalid(seh::text::FilterOrFinallyRequiresAGuestCallbackContinuation);
}

llvm::Expected<X64SEH::Action>
X64SEH::advanceImpl(Dispatch &State,
                    std::optional<int32_t> FilterResult) const {
  const auto Bounds = State.Bounds;
  if (State.Complete)
    return invalid(seh::text::ExceptionDispatchAlreadyCompleted);
  if (!Bounds.Size || Bounds.Size > UINT64_MAX - Bounds.Base || !ReadStack ||
      !Executable)
    return invalid(seh::text::InvalidImageStackOrMemoryContract);
  if (!State.FrameActive && !State.Selected) {
    const uint64_t SP = State.Current.GPR[seh::StackRegister];
    if (SP % seh::PointerSize || SP < Bounds.Base ||
        SP - Bounds.Base >= Bounds.Size ||
        Bounds.Size - (SP - Bounds.Base) < seh::PointerSize)
      return invalid(seh::text::InvalidExceptionFrameStackPointer);
    auto Owner = imageFor(State.Current.PC);
    if (!Owner) {
      if (++State.SegmentIndex < State.Path.size()) {
        const auto &Segment = State.Path[State.SegmentIndex];
        State.Current = Segment.Registers;
        State.Bounds = Segment.Bounds;
        return advanceImpl(State, {});
      }
      State.Complete = true;
      return Action{};
    }
    State.ImageIndex = *Owner;
  }
  const auto &Owner = Images[State.ImageIndex];
  const auto &Metadata = *Owner.Metadata;
  const auto PreferredBase = Owner.PreferredBase, ActualBase = Owner.ActualBase;
  const auto ImageSize = Owner.Size;
  if (Metadata.Functions.size() > seh::MaxFunctions)
    return invalid(seh::text::RuntimeFunctionLimitExceeded);
  if (Metadata.StructuralDecode &&
      Metadata.StructuralDecode->ParseStatus != ExceptionParseStatus::Complete)
    return invalid(seh::text::IncompleteExceptionDirectoryMetadata);
  const uint64_t StackEnd = Bounds.Base + Bounds.Size;
  const auto InStack = [&](uint64_t Address, uint64_t Size) {
    return Address >= Bounds.Base && Address <= StackEnd &&
           Size <= StackEnd - Address;
  };
  const auto Read = [&](uint64_t Address) -> llvm::Expected<uint64_t> {
    if (Address % seh::PointerSize || !InStack(Address, seh::PointerSize))
      return invalid(seh::text::UnwindReadExceedsTheCurrentExecutionStack);
    return ReadStack(Address);
  };
  const auto ToActual = [&](uint64_t Address) -> llvm::Expected<uint64_t> {
    if (Address < PreferredBase || Address - PreferredBase >= ImageSize)
      return invalid(seh::text::ExceptionTargetLiesOutsideItsImage);
    const uint64_t Actual = ActualBase + (Address - PreferredBase);
    if (!Executable(Actual))
      return invalid(seh::text::ExceptionTargetIsNotExecutable);
    return Actual;
  };
  const auto ResolveChain = [&](const ExceptionFunction &First)
      -> llvm::Expected<std::vector<const ExceptionFunction *>> {
    std::vector<const ExceptionFunction *> Frames;
    const ExceptionFunction *Frame = &First;
    while (true) {
      if (Frames.size() >= seh::MaxFrames)
        return invalid(seh::text::ChainedUnwindRecordLimitExceeded);
      if (std::find(Frames.begin(), Frames.end(), Frame) != Frames.end())
        return invalid(seh::text::CyclicChainedUnwindRecords);
      if (auto E = validateFrame(*Frame))
        return std::move(E);
      if (Frame->CodeRange.Begin < PreferredBase ||
          Frame->CodeRange.End > PreferredBase + ImageSize ||
          !Frame->CodeRange.isValid() ||
          Frame->PrologueSize > Frame->CodeRange.size())
        return invalid(seh::text::RuntimeFunctionLiesOutsideItsImage);
      Frames.push_back(Frame);
      if (Frame->Kind != RuntimeFunctionKind::Chained)
        return Frames;
      if (*Frame->PrimaryFunctionIndex >= Metadata.Functions.size())
        return invalid(seh::text::ChainedPrimaryIndexIsOutOfRange);
      const auto &Parent = Metadata.Functions[*Frame->PrimaryFunctionIndex];
      if (Frame->ChainedPrimaryRange->Begin != Parent.CodeRange.Begin ||
          Frame->ChainedPrimaryRange->End != Parent.CodeRange.End ||
          Frame->ChainedUnwindInfoRVA != Parent.UnwindInfoRVA)
        return invalid(seh::text::ChainedPrimaryDoesNotMatchItsDirectoryRecord);
      if (Frame->FrameRegister != Parent.FrameRegister ||
          Frame->FrameOffset != Parent.FrameOffset)
        return invalid(seh::text::ChainedUnwindHasADifferentFrameRegister);
      Frame = &Parent;
    }
  };
  const auto ContainsContinuation =
      [&](uint64_t Address) -> llvm::Expected<bool> {
    if (State.Frame->CodeRange.contains(Address))
      return true;
    for (const auto &Candidate : Metadata.Functions) {
      if (!Candidate.CodeRange.contains(Address))
        continue;
      auto Chain = ResolveChain(Candidate);
      if (!Chain)
        return Chain.takeError();
      if (Chain->back() == State.Frame)
        return true;
    }
    return false;
  };
  const auto CollectCleanups = [&]() -> llvm::Error {
    if (!State.Frame || State.InPrologue)
      return llvm::Error::success();
    if (State.Frame->GSCookie &&
        (State.Frame->UnwindFlags & seh::UnwindHandlerFlag)) {
      if (State.Cleanups.size() >= seh::MaxUnwindSteps)
        return invalid(seh::text::ExceptionCleanupLimitExceeded);
      State.Cleanups.push_back(
          {{ActionKind::Unhandled,
            {State.Current, State.Establisher, 0, State.Code},
            State.Bounds},
           State.Frame,
           State.ImageIndex});
    }
    if (!State.Frame->SEH || !hasLanguageHandler(*State.Frame, true))
      return llvm::Error::success();
    for (size_t I = State.ScopeFloor; I < State.Frame->SEH->Scopes.size();
         ++I) {
      const auto &Scope = State.Frame->SEH->Scopes[I];
      if (Scope.Kind != SEHScopeKind::Finally ||
          !Scope.GuardedRange.contains(State.OriginalPC))
        continue;
      if (State.Selected) {
        const uint64_t Target =
            PreferredBase + (State.Selected->HandlerPC - ActualBase);
        if (Scope.GuardedRange.contains(Target))
          continue;
      }
      if (Scope.ParseStatus != ExceptionParseStatus::Complete ||
          !Scope.GuardedRange.isValid() ||
          !(State.Frame->UnwindFlags & seh::UnwindHandlerFlag) ||
          !Scope.FilterOrFinallyVA || Scope.ContinuationVA ||
          Scope.HandlerVA != Scope.FilterOrFinallyVA)
        return invalid(seh::text::InvalidFinallyScope);
      auto Target = ToActual(Scope.FilterOrFinallyVA);
      if (!Target)
        return Target.takeError();
      if (State.Cleanups.size() >= seh::MaxUnwindSteps)
        return invalid(seh::text::ExceptionCleanupLimitExceeded);
      State.Cleanups.push_back(
          {{ActionKind::Finally,
            {State.Current, State.Establisher, *Target, State.Code},
            State.Bounds,
            State.SegmentIndex,
            I + 1}});
    }
    return llvm::Error::success();
  };
  if (State.FilterCandidate) {
    if (!FilterResult)
      return invalid(seh::text::PendingFilterRequiresItsGuestReturnValue);
    if (*FilterResult < 0) {
      State.Complete = true;
      return Action{ActionKind::ContinueExecution,
                    {State.Original, 0, State.Original.PC, State.Code}};
    }
    if (*FilterResult > 0) {
      State.Selected = State.FilterCandidate;
      if (auto E = CollectCleanups())
        return std::move(E);
    }
    State.FilterCandidate.reset();
  } else if (FilterResult) {
    return invalid(seh::text::FilterResultHasNoPendingFilter);
  }

  while (true) {
    if (State.Selected) {
      if (State.CleanupIndex < State.Cleanups.size()) {
        const auto &Step = State.Cleanups[State.CleanupIndex++];
        if (Step.CookieFrame) {
          if (auto E = checkGSCookie(*Step.CookieFrame,
                                     Step.Call.State.EstablisherFrame,
                                     Step.Call.Bounds, Images[Step.ImageIndex]))
            return std::move(E);
          continue;
        }
        auto Cleanup = Step.Call;
        Cleanup.ExceptionFlags = seh::ExceptionUnwindingFlag;
        if (Cleanup.Bounds.Base == State.Bounds.Base &&
            Cleanup.State.EstablisherFrame == State.Selected->EstablisherFrame)
          Cleanup.ExceptionFlags |= seh::ExceptionTargetUnwindFlag;
        return Cleanup;
      }
      State.Complete = true;
      return Action{ActionKind::Handler, *State.Selected, State.Bounds,
                    State.SegmentIndex};
    }
    auto &Current = State.Current;
    const uint64_t SP = Current.GPR[seh::StackRegister];
    if (!State.FrameActive) {
      if (State.Depth++ >= seh::MaxFrames)
        return invalid(seh::text::ExceptionFrameLimitExceeded);
      if (SP % seh::PointerSize || !InStack(SP, seh::PointerSize))
        return invalid(seh::text::InvalidExceptionFrameStackPointer);
      if (!State.Seen.emplace(Current.PC, SP).second)
        return invalid(seh::text::CyclicExceptionFrameChain);
      if (!Executable(Current.PC))
        return invalid(seh::text::ExceptionControlAddressIsNotExecutable);
      State.OriginalPC = PreferredBase + (Current.PC - ActualBase);
      State.Frame = nullptr;
      State.UnwindFrames.clear();
      for (const auto &Candidate : Metadata.Functions) {
        if (!Candidate.CodeRange.contains(State.OriginalPC))
          continue;
        auto Chain = ResolveChain(Candidate);
        if (!Chain)
          return Chain.takeError();
        if (!State.UnwindFrames.empty()) {
          if (std::find(State.UnwindFrames.begin(), State.UnwindFrames.end(),
                        &Candidate) != State.UnwindFrames.end())
            continue;
          if (std::find(Chain->begin(), Chain->end(),
                        State.UnwindFrames.front()) == Chain->end())
            return invalid(seh::text::AmbiguousOverlappingRuntimeFunctions);
        }
        State.UnwindFrames = std::move(*Chain);
        State.Frame = State.UnwindFrames.back();
      }
      if (!State.Frame && !Metadata.StructuralDecode &&
          Metadata.ParseStatus != ExceptionParseStatus::Complete)
        return invalid(
            seh::text::IncompleteExceptionDirectoryCannotEstablishALeaf);
      State.Establisher = SP;
      State.InPrologue = false;
      if (State.Frame) {
        const auto &Frame = *State.UnwindFrames.front();
        State.ControlOffset = State.OriginalPC - Frame.CodeRange.Begin;
        State.InPrologue = State.ControlOffset < Frame.PrologueSize;
        if (!State.InPrologue) {
          auto Epilogue = unwindEpilogue(Frame, Current, State.Bounds, Owner);
          if (!Epilogue)
            return Epilogue.takeError();
          if (*Epilogue) {
            Current = **Epilogue;
            return advanceImpl(State, {});
          }
        }
        const bool FrameEstablished =
            State.UnwindFrames.size() > 1 || !State.InPrologue ||
            std::any_of(
                Frame.UnwindOperations.begin(), Frame.UnwindOperations.end(),
                [&](const UnwindOperation &Op) {
                  return Op.Kind == UnwindOperationKind::SetFramePointer &&
                         Op.CodeOffset <= State.ControlOffset;
                });
        if (Frame.FrameRegister && FrameEstablished) {
          const uint64_t FP = Current.GPR[Frame.FrameRegister];
          if (FP < Frame.FrameOffset)
            return invalid(seh::text::FrameRegisterOffsetUnderflows);
          State.Establisher = FP - Frame.FrameOffset;
        }
        if (State.Establisher < SP || State.Establisher % seh::PointerSize ||
            !InStack(State.Establisher, seh::PointerSize))
          return invalid(seh::text::EstablisherFrameExceedsTheCurrentStack);
        if (State.Frame->SEH &&
            State.Frame->SEH->Scopes.size() > seh::MaxScopes)
          return invalid(seh::text::SehScopeLimitExceeded);
        if (!State.InPrologue && State.Frame->GSCookie &&
            (State.Frame->UnwindFlags & seh::ExceptionHandlerFlag))
          if (auto E = checkGSCookie(*State.Frame, State.Establisher,
                                     State.Bounds, Owner))
            return std::move(E);
      }
      const auto &Segment = State.Path[State.SegmentIndex];
      State.ScopeFloor = Current.PC == Segment.Registers.PC &&
                                 SP == Segment.Registers.GPR[seh::StackRegister]
                             ? Segment.ScopeIndex
                             : 0;
      if (State.ScopeFloor &&
          (!State.Frame || !State.Frame->SEH ||
           State.ScopeFloor > State.Frame->SEH->Scopes.size()))
        return invalid(seh::text::CollidedUnwindHasAnInvalidScopeCursor);
      State.ScopeIndex = State.ScopeFloor;
      State.FrameActive = true;
    }
    if (State.Frame && State.Frame->SEH && !State.InPrologue &&
        hasLanguageHandler(*State.Frame, false)) {
      const auto &Frame = *State.Frame;
      while (State.ScopeIndex < Frame.SEH->Scopes.size()) {
        const auto &Scope = Frame.SEH->Scopes[State.ScopeIndex++];
        if (!Scope.GuardedRange.contains(State.OriginalPC))
          continue;
        if (Scope.ParseStatus != ExceptionParseStatus::Complete ||
            !Scope.GuardedRange.isValid())
          return invalid(seh::text::EncounteredIncompleteSehScope);
        if (Scope.Kind == SEHScopeKind::Finally)
          continue;
        if (Scope.Kind != SEHScopeKind::CatchAll &&
            Scope.Kind != SEHScopeKind::Filter)
          return invalid(seh::text::EncounteredUnsupportedSehScope);
        auto InFunction = ContainsContinuation(Scope.HandlerVA);
        if (!InFunction)
          return InFunction.takeError();
        // C scope endpoints describe protected control PCs, not handler
        // ownership. LLVM's EndLabel + 1 may include the landing-pad address.
        if (!(Frame.UnwindFlags & seh::ExceptionHandlerFlag) ||
            Scope.NormalizedFilterVA ||
            Scope.HandlerVA != Scope.ContinuationVA || !*InFunction)
          return invalid(seh::text::InvalidExceptionHandlerContinuation);
        auto Target = ToActual(Scope.HandlerVA);
        if (!Target)
          return Target.takeError();
        Context Handler = Current;
        Handler.GPR[seh::StackRegister] = State.Establisher;
        Handler.GPR[seh::ReturnRegister] = State.Code;
        Handler.PC = *Target;
        Handler.FromReturnAddress = false;
        Transfer Candidate{Handler, State.Establisher, *Target, State.Code};
        if (Scope.Kind == SEHScopeKind::Filter) {
          if (!Scope.FilterOrFinallyVA) // Constant EXCEPTION_CONTINUE_SEARCH.
            continue;
          auto Filter = ToActual(Scope.FilterOrFinallyVA);
          if (!Filter)
            return Filter.takeError();
          State.FilterCandidate = Candidate;
          const auto NestedFrame = State.Path[State.SegmentIndex].NestedFrame;
          return Action{ActionKind::Filter,
                        {Current, State.Establisher, *Filter, State.Code},
                        State.Bounds,
                        State.SegmentIndex,
                        State.ScopeIndex,
                        NestedFrame && State.Establisher <= NestedFrame
                            ? uint32_t(seh::ExceptionNestedCallFlag)
                            : 0};
        }
        if (Scope.FilterOrFinallyVA)
          return invalid(seh::text::CatchAllScopeHasAnUnexpectedFilter);
        State.Selected = Candidate;
        if (auto E = CollectCleanups())
          return std::move(E);
        break;
      }
    }
    if (State.Selected)
      continue;
    if (auto E = CollectCleanups())
      return std::move(E);
    uint64_t Cursor = SP;
    for (const auto *Frame : State.UnwindFrames) {
      for (const auto &Op : Frame->UnwindOperations) {
        if (Frame == State.UnwindFrames.front() && State.InPrologue &&
            Op.CodeOffset > State.ControlOffset)
          continue;
        switch (Op.Kind) {
        case UnwindOperationKind::PushNonVolatile: {
          auto Value = Read(Cursor);
          if (!Value)
            return Value.takeError();
          Current.GPR[Op.Register] = *Value;
          Cursor += seh::PointerSize;
          break;
        }
        case UnwindOperationKind::AllocateSmall:
        case UnwindOperationKind::AllocateLarge:
          if (!InStack(Cursor, Op.StackOffset))
            return invalid(seh::text::UnwindAllocationExceedsTheCurrentStack);
          Cursor += Op.StackOffset;
          break;
        case UnwindOperationKind::SetFramePointer:
          Cursor = State.Establisher;
          break;
        case UnwindOperationKind::SaveNonVolatile:
        case UnwindOperationKind::SaveNonVolatileFar: {
          if (Op.StackOffset > UINT64_MAX - State.Establisher)
            return invalid(seh::text::SavedRegisterAddressOverflows);
          auto Value = Read(State.Establisher + Op.StackOffset);
          if (!Value)
            return Value.takeError();
          Current.GPR[Op.Register] = *Value;
          break;
        }
        case UnwindOperationKind::SaveXMM128:
        case UnwindOperationKind::SaveXMM128Far: {
          if (Op.StackOffset > UINT64_MAX - State.Establisher)
            return invalid(seh::text::SavedXmmAddressOverflows);
          const uint64_t Address = State.Establisher + Op.StackOffset;
          if (!InStack(Address, seh::XmmSize))
            return invalid(
                seh::text::XmmUnwindReadExceedsTheCurrentExecutionStack);
          auto &Xmm = Current.Xmm[Op.Register - seh::FirstNonvolatileXmm];
          for (size_t I = 0; I < Xmm.size(); ++I) {
            auto Value = Read(Address + I * seh::PointerSize);
            if (!Value)
              return Value.takeError();
            Xmm[I] = *Value;
          }
          break;
        }
        default:
          llvm_unreachable(seh::text::ValidatedOperation);
        }
      }
    }
    auto Return = Read(Cursor);
    if (!Return)
      return Return.takeError();
    if (!*Return)
      return invalid(seh::text::NullUnwindReturnAddress);
    Current.GPR[seh::StackRegister] = Cursor + seh::PointerSize;
    if (Current.GPR[seh::StackRegister] <= SP)
      return invalid(seh::text::UnwindDidNotAdvanceTheStack);
    Current.PC = *Return - 1;
    Current.FromReturnAddress = true;
    State.FrameActive = false;
    return advanceImpl(State, {});
  }
}

llvm::Expected<std::vector<uint8_t>>
X64SEH::encodeRecords(const Exception &Raised, uint64_t Storage) {
  if (Storage % seh::RecordAlignment ||
      Storage > UINT64_MAX - seh::RecordsSize ||
      Raised.Parameters.size() > seh::MaxExceptionParameters)
    return invalid(seh::text::InvalidExceptionRecordStorageOrParameterCount);
  std::vector<uint8_t> Bytes(seh::RecordsSize);
  const auto Write = [&](uint64_t Offset, uint64_t Value, unsigned Size) {
    for (unsigned I = 0; I < Size; ++I)
      Bytes[Offset + I] = static_cast<uint8_t>(Value >> (I * 8));
  };
  Write(0, Raised.Code, 4);
  Write(seh::ExceptionFlagsOffset, Raised.Flags, 4);
  Write(seh::ExceptionLinkOffset, Raised.PreviousRecord, seh::PointerSize);
  Write(seh::ExceptionAddressOffset, Raised.Address, seh::PointerSize);
  Write(seh::ExceptionParameterCountOffset, Raised.Parameters.size(), 4);
  for (size_t I = 0; I < Raised.Parameters.size(); ++I)
    Write(seh::ExceptionParametersOffset + I * seh::PointerSize,
          Raised.Parameters[I], seh::PointerSize);
  Write(seh::ExceptionPointersOffset, Storage, seh::PointerSize);
  Write(seh::ExceptionPointersOffset + seh::PointerSize,
        Storage + seh::ContextOffset, seh::PointerSize);
  Write(seh::ContextOffset + seh::ContextFlagsOffset,
        seh::ContextIntegerControl |
            (Raised.Registers.SSE ? seh::ContextFloatingPoint : 0),
        4);
  Write(seh::ContextOffset + seh::ContextCSOffset, Raised.Registers.CS, 2);
  Write(seh::ContextOffset + seh::ContextSSOffset, Raised.Registers.SS, 2);
  Write(seh::ContextOffset + seh::ContextEFlagsOffset, Raised.Registers.Flags,
        4);
  // AMD64 CONTEXT stores RSP before RBP, matching architectural GPR numbering.
  for (size_t I = 0; I < Raised.Registers.GPR.size(); ++I)
    Write(seh::ContextOffset + seh::ContextGPROffset + I * seh::PointerSize,
          Raised.Registers.GPR[I], seh::PointerSize);
  Write(seh::ContextOffset + seh::ContextPCOffset, Raised.Registers.PC,
        seh::PointerSize);
  if (const auto &SSE = Raised.Registers.SSE) {
    Write(seh::ContextOffset + seh::ContextMXCSROffset, SSE->MXCSR, 4);
    Write(seh::ContextOffset + seh::ContextFXMXCSROffset, SSE->MXCSR, 4);
    for (size_t I = 0; I < seh::FirstNonvolatileXmm + seh::NonvolatileXmmCount;
         ++I) {
      const auto &Xmm =
          I < seh::FirstNonvolatileXmm
              ? SSE->VolatileXmm[I]
              : Raised.Registers.Xmm[I - seh::FirstNonvolatileXmm];
      for (size_t Word = 0; Word < Xmm.size(); ++Word)
        Write(seh::ContextOffset + seh::ContextXmmOffset + I * seh::XmmSize +
                  Word * seh::PointerSize,
              Xmm[Word], seh::PointerSize);
    }
  }
  return Bytes;
}

llvm::Expected<X64SEH::Action>
X64SEH::finishFilter(Dispatch &State, int32_t FilterResult,
                     llvm::ArrayRef<uint8_t> Records, const Exception &Raised,
                     uint64_t Storage) const {
  auto Context = validateRecords(Records, Raised, Storage);
  if (!Context)
    return Context.takeError();
  return advance(State, FilterResult);
}

llvm::Expected<X64SEH::Context>
X64SEH::validateRecords(llvm::ArrayRef<uint8_t> Records,
                        const Exception &Raised, uint64_t Storage) const {
  auto Expected = encodeRecords(Raised, Storage);
  if (!Expected)
    return Expected.takeError();
  if (Records.size() != Expected->size())
    return invalid(seh::text::InvalidExceptionRecordSize);
  const auto Read = [&](uint64_t Offset, unsigned Size) {
    uint64_t Value = 0;
    for (unsigned I = 0; I < Size; ++I)
      Value |= uint64_t(Records[Offset + I]) << (I * 8);
    return Value;
  };
  Context Result = Raised.Registers;
  for (size_t I = 0; I < Result.GPR.size(); ++I)
    Result.GPR[I] =
        Read(seh::ContextOffset + seh::ContextGPROffset + I * seh::PointerSize,
             seh::PointerSize);
  Result.PC = Read(seh::ContextOffset + seh::ContextPCOffset, seh::PointerSize);
  Result.Flags = Read(seh::ContextOffset + seh::ContextEFlagsOffset, 4);
  if ((Result.Flags ^ Raised.Registers.Flags) & ~seh::MutableEFlags)
    return invalid(seh::text::ContinuationChangesUnsupportedControlFlags);
  if (auto &SSE = Result.SSE) {
    // The kernel restores the top-level MXCSR, masked by its processor profile.
    // FltSave.MxCsr is an observation slot, not another restoration authority.
    SSE->MXCSR =
        Read(seh::ContextOffset + seh::ContextMXCSROffset, 4) & SSE->MXCSRMask;
    for (size_t I = 0; I < seh::FirstNonvolatileXmm + seh::NonvolatileXmmCount;
         ++I) {
      auto &Xmm = I < seh::FirstNonvolatileXmm
                      ? SSE->VolatileXmm[I]
                      : Result.Xmm[I - seh::FirstNonvolatileXmm];
      for (size_t Word = 0; Word < Xmm.size(); ++Word)
        Xmm[Word] = Read(seh::ContextOffset + seh::ContextXmmOffset +
                             I * seh::XmmSize + Word * seh::PointerSize,
                         seh::PointerSize);
    }
  }
  Exception Updated = Raised;
  Updated.Registers = Result;
  auto Allowed = encodeRecords(Updated, Storage);
  if (!Allowed)
    return Allowed.takeError();
  if (Result.SSE)
    for (auto Offset : {seh::ContextMXCSROffset, seh::ContextFXMXCSROffset})
      std::copy_n(Records.begin() + seh::ContextOffset + Offset,
                  sizeof(uint32_t),
                  Allowed->begin() + seh::ContextOffset + Offset);
  if (!std::equal(Records.begin(), Records.end(), Allowed->begin()))
    return invalid(
        seh::text::ContinuationChangesUnsupportedExceptionContextFields);
  return Result;
}

llvm::Expected<X64SEH::Context>
X64SEH::continuation(llvm::ArrayRef<uint8_t> Records, const Exception &Raised,
                     uint64_t Storage, Stack Bounds) const {
  auto Result = validateRecords(Records, Raised, Storage);
  if (!Result)
    return Result.takeError();
  const uint64_t SP = Result->GPR[seh::StackRegister];
  if (!Bounds.Size || Bounds.Size > UINT64_MAX - Bounds.Base ||
      SP < Bounds.Base || SP >= Bounds.Base + Bounds.Size ||
      SP % seh::PointerSize)
    return invalid(seh::text::ContinuationStackPointerExceedsItsExecutionStack);
  if (auto E = validateImages(true))
    return std::move(E);
  if (!imageFor(Result->PC) || !Executable || !Executable(Result->PC))
    return invalid(seh::text::ContinuationDoesNotNameExecutableImageCode);
  return Result;
}

} // namespace neverd::emulation
