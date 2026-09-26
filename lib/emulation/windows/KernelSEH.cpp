//===- KernelSEH.cpp - Checked x64 C exception dispatch ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Plans C exception search and unwind using decoded loader metadata.
///
//===----------------------------------------------------------------------===//

#include "KernelSEH.h"

#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <set>
#include <utility>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "x64 SEH: " + Message);
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
    return invalid("encountered incomplete exception metadata");
  if (F.Encoding != ExceptionEncoding::X64UnwindV1 || F.UnwindVersion != 1)
    return invalid("only x64 V1 unwind records are supported");
  const bool Chained = F.Kind == RuntimeFunctionKind::Chained;
  if (Chained != bool(F.UnwindFlags & seh::ChainFlag) ||
      Chained != F.ChainedPrimaryRange.has_value() ||
      (Chained && (!F.ChainedUnwindInfoRVA || !F.PrimaryFunctionIndex)) ||
      (!Chained && (F.ChainedUnwindInfoRVA || F.PrimaryFunctionIndex)) ||
      (F.Kind != RuntimeFunctionKind::Primary && !Chained))
    return invalid("inconsistent chained unwind record");
  if (F.UnwindFlags & ~(seh::ExceptionHandlerFlag | seh::UnwindHandlerFlag |
                        seh::ChainFlag) ||
      (Chained && F.UnwindFlags != seh::ChainFlag))
    return invalid("unsupported unwind flags");
  const bool StandaloneGS =
      F.Personality == ExceptionPersonality::GSHandlerCheck;
  const bool GS =
      StandaloneGS || F.Personality == ExceptionPersonality::GSHandlerCheckSEH;
  const bool CHandler =
      F.Personality == ExceptionPersonality::GSHandlerCheckSEH ||
      F.Personality == ExceptionPersonality::CSpecificHandler;
  if (!CHandler && !StandaloneGS && F.Personality != ExceptionPersonality::None)
    return invalid("encountered unsupported language personality");
  if (F.Cxx || GS != F.GSCookie.has_value() ||
      (((F.UnwindFlags & ~seh::ChainFlag) != 0) !=
       (CHandler || StandaloneGS)) ||
      (CHandler != F.SEH.has_value()))
    return invalid("inconsistent C exception-handler metadata");
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
      return invalid("inconsistent GS cookie metadata");
  }
  if (F.FrameRegister && !nonvolatile(F.FrameRegister))
    return invalid("invalid frame register");
  if (F.FrameOffset > seh::MaxFrameOffset ||
      F.FrameOffset % seh::FrameOffsetScale ||
      (!F.FrameRegister && F.FrameOffset))
    return invalid("invalid frame-register offset");
  if (F.UnwindOperations.size() > seh::MaxUnwindOperations)
    return invalid("unwind operation limit exceeded");
  uint32_t PreviousOffset = F.PrologueSize;
  unsigned FrameSetCount = 0;
  for (const auto &Op : F.UnwindOperations) {
    if (!Op.CodeOffset || Op.CodeOffset > PreviousOffset)
      return invalid("invalid unwind operation order");
    PreviousOffset = Op.CodeOffset;
    // Secondary prologues can add saves in the primary fixed allocation.
    // They cannot push registers or create another fixed allocation.
    if (Chained && Op.Kind != UnwindOperationKind::SaveNonVolatile &&
        Op.Kind != UnwindOperationKind::SaveNonVolatileFar &&
        Op.Kind != UnwindOperationKind::SaveXMM128 &&
        Op.Kind != UnwindOperationKind::SaveXMM128Far)
      return invalid("chained unwind changes the primary stack allocation");
    switch (Op.Kind) {
    case UnwindOperationKind::PushNonVolatile:
    case UnwindOperationKind::SaveNonVolatile:
    case UnwindOperationKind::SaveNonVolatileFar:
      if (!nonvolatile(Op.Register))
        return invalid("invalid saved nonvolatile register");
      if (Op.StackOffset % seh::PointerSize)
        return invalid("unaligned nonvolatile save offset");
      break;
    case UnwindOperationKind::AllocateSmall:
      if (Op.StackOffset > seh::MaxSmallAllocation)
        return invalid("invalid small stack allocation");
      [[fallthrough]];
    case UnwindOperationKind::AllocateLarge:
      if (!Op.StackOffset || Op.StackOffset % seh::PointerSize)
        return invalid("invalid stack allocation");
      break;
    case UnwindOperationKind::SetFramePointer:
      if (!F.FrameRegister || ++FrameSetCount != 1)
        return invalid("invalid frame-pointer operation");
      break;
    case UnwindOperationKind::SaveXMM128:
    case UnwindOperationKind::SaveXMM128Far:
      if (Op.Register < seh::FirstNonvolatileXmm ||
          Op.Register >= seh::FirstNonvolatileXmm + seh::NonvolatileXmmCount)
        return invalid("invalid saved nonvolatile XMM register");
      if (Op.StackOffset % seh::XmmSize)
        return invalid("unaligned XMM save offset");
      break;
    default:
      return invalid("encountered unsupported unwind operation");
    }
  }
  if (F.FrameRegister && !Chained && FrameSetCount != 1)
    return invalid("frame register has no establishing operation");
  return llvm::Error::success();
}
} // namespace

KernelSEH::KernelSEH(const ExceptionInfo &Metadata, uint64_t PreferredBase,
                     uint64_t ActualBase, uint64_t ImageSize,
                     ReadStack64 ReadStack, IsExecutable Executable,
                     ReadCode Code, ReadSecurityCookie Cookie)
    : Metadata(Metadata), PreferredBase(PreferredBase), ActualBase(ActualBase),
      ImageSize(ImageSize), ReadStack(std::move(ReadStack)),
      Executable(std::move(Executable)), Code(std::move(Code)),
      Cookie(std::move(Cookie)) {}

KernelSEH::Dispatch KernelSEH::begin(uint32_t ExceptionCode,
                                     const Context &Caller,
                                     Stack Bounds) const {
  Dispatch State;
  State.Original = State.Current = Caller;
  State.Bounds = Bounds;
  State.Path.push_back({Caller, Bounds});
  State.Code = ExceptionCode;
  return State;
}

llvm::Expected<KernelSEH::Dispatch>
KernelSEH::beginNested(uint32_t ExceptionCode, const Context &Caller,
                       Stack Bounds, const Dispatch &Suspended,
                       const Action &Callback) const {
  if (Suspended.Complete || Suspended.Path.empty() ||
      Callback.SegmentIndex >= Suspended.Path.size() ||
      (Callback.Kind != ActionKind::Filter &&
       Callback.Kind != ActionKind::Finally))
    return invalid("nested dispatch requires an active SEH callback");
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
    return invalid("nested exception path limit exceeded");
  return State;
}

llvm::Expected<KernelSEH::Action>
KernelSEH::advance(Dispatch &State, std::optional<int32_t> FilterResult) const {
  Dispatch Candidate = State;
  auto Result = advanceImpl(Candidate, FilterResult);
  if (Result)
    State = std::move(Candidate);
  return Result;
}

llvm::Expected<std::optional<KernelSEH::Transfer>>
KernelSEH::plan(uint32_t ExceptionCode, const Context &Caller,
                Stack Bounds) const {
  auto State = begin(ExceptionCode, Caller, Bounds);
  auto Next = advance(State);
  if (!Next)
    return Next.takeError();
  if (Next->Kind == ActionKind::Handler)
    return std::optional<Transfer>{Next->State};
  if (Next->Kind == ActionKind::Unhandled)
    return std::optional<Transfer>{};
  return invalid("filter or finally requires a guest callback continuation");
}

llvm::Expected<KernelSEH::Action>
KernelSEH::advanceImpl(Dispatch &State,
                       std::optional<int32_t> FilterResult) const {
  const auto Bounds = State.Bounds;
  if (State.Complete)
    return invalid("exception dispatch already completed");
  if (!ImageSize || ImageSize > UINT64_MAX - PreferredBase ||
      ImageSize > UINT64_MAX - ActualBase || !Bounds.Size ||
      Bounds.Size > UINT64_MAX - Bounds.Base || !ReadStack || !Executable)
    return invalid("invalid image, stack or memory contract");
  if (Metadata.Functions.size() > seh::MaxFunctions)
    return invalid("runtime-function limit exceeded");
  if (Metadata.StructuralDecode &&
      Metadata.StructuralDecode->ParseStatus != ExceptionParseStatus::Complete)
    return invalid("incomplete exception directory metadata");
  const uint64_t StackEnd = Bounds.Base + Bounds.Size;
  const auto InStack = [&](uint64_t Address, uint64_t Size) {
    return Address >= Bounds.Base && Address <= StackEnd &&
           Size <= StackEnd - Address;
  };
  const auto Read = [&](uint64_t Address) -> llvm::Expected<uint64_t> {
    if (Address % seh::PointerSize || !InStack(Address, seh::PointerSize))
      return invalid("unwind read exceeds the current execution stack");
    return ReadStack(Address);
  };
  const auto ToActual = [&](uint64_t Address) -> llvm::Expected<uint64_t> {
    if (Address < PreferredBase || Address - PreferredBase >= ImageSize)
      return invalid("exception target lies outside its image");
    const uint64_t Actual = ActualBase + (Address - PreferredBase);
    if (!Executable(Actual))
      return invalid("exception target is not executable");
    return Actual;
  };
  const auto ResolveChain = [&](const ExceptionFunction &First)
      -> llvm::Expected<std::vector<const ExceptionFunction *>> {
    std::vector<const ExceptionFunction *> Frames;
    const ExceptionFunction *Frame = &First;
    while (true) {
      if (Frames.size() >= seh::MaxFrames)
        return invalid("chained unwind record limit exceeded");
      if (std::find(Frames.begin(), Frames.end(), Frame) != Frames.end())
        return invalid("cyclic chained unwind records");
      if (auto E = validateFrame(*Frame))
        return std::move(E);
      if (Frame->CodeRange.Begin < PreferredBase ||
          Frame->CodeRange.End > PreferredBase + ImageSize ||
          !Frame->CodeRange.isValid() ||
          Frame->PrologueSize > Frame->CodeRange.size())
        return invalid("runtime function lies outside its image");
      Frames.push_back(Frame);
      if (Frame->Kind != RuntimeFunctionKind::Chained)
        return Frames;
      if (*Frame->PrimaryFunctionIndex >= Metadata.Functions.size())
        return invalid("chained primary index is out of range");
      const auto &Parent = Metadata.Functions[*Frame->PrimaryFunctionIndex];
      if (Frame->ChainedPrimaryRange->Begin != Parent.CodeRange.Begin ||
          Frame->ChainedPrimaryRange->End != Parent.CodeRange.End ||
          Frame->ChainedUnwindInfoRVA != Parent.UnwindInfoRVA)
        return invalid("chained primary does not match its directory record");
      if (Frame->FrameRegister != Parent.FrameRegister ||
          Frame->FrameOffset != Parent.FrameOffset)
        return invalid("chained unwind has a different frame register");
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
        return invalid("exception cleanup limit exceeded");
      State.Cleanups.push_back(
          {{ActionKind::Unhandled,
            {State.Current, State.Establisher, 0, State.Code},
            State.Bounds},
           State.Frame});
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
        return invalid("invalid finally scope");
      auto Target = ToActual(Scope.FilterOrFinallyVA);
      if (!Target)
        return Target.takeError();
      if (State.Cleanups.size() >= seh::MaxUnwindSteps)
        return invalid("exception cleanup limit exceeded");
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
      return invalid("pending filter requires its guest return value");
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
    return invalid("filter result has no pending filter");
  }

  while (true) {
    if (State.Selected) {
      if (State.CleanupIndex < State.Cleanups.size()) {
        const auto &Step = State.Cleanups[State.CleanupIndex++];
        if (Step.CookieFrame) {
          if (auto E = checkGSCookie(*Step.CookieFrame,
                                     Step.Call.State.EstablisherFrame,
                                     Step.Call.Bounds))
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
        return invalid("exception frame limit exceeded");
      if (SP % seh::PointerSize || !InStack(SP, seh::PointerSize))
        return invalid("invalid exception frame stack pointer");
      if (!State.Seen.emplace(Current.PC, SP).second)
        return invalid("cyclic exception frame chain");
      // Only an explicit SEH continuation joins private callback stacks.
      // Ordinary model callbacks remain independent exception boundaries.
      if (Current.PC < ActualBase || Current.PC - ActualBase >= ImageSize) {
        if (++State.SegmentIndex < State.Path.size()) {
          const auto &Segment = State.Path[State.SegmentIndex];
          State.Current = Segment.Registers;
          State.Bounds = Segment.Bounds;
          State.FrameActive = false;
          return advanceImpl(State, {});
        }
        State.Complete = true;
        return Action{};
      }
      if (!Executable(Current.PC))
        return invalid("exception control address is not executable");
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
            return invalid("ambiguous overlapping runtime functions");
        }
        State.UnwindFrames = std::move(*Chain);
        State.Frame = State.UnwindFrames.back();
      }
      if (!State.Frame && !Metadata.StructuralDecode &&
          Metadata.ParseStatus != ExceptionParseStatus::Complete)
        return invalid(
            "incomplete exception directory cannot establish a leaf");
      State.Establisher = SP;
      State.InPrologue = false;
      if (State.Frame) {
        const auto &Frame = *State.UnwindFrames.front();
        State.ControlOffset = State.OriginalPC - Frame.CodeRange.Begin;
        State.InPrologue = State.ControlOffset < Frame.PrologueSize;
        if (!State.InPrologue) {
          auto Epilogue = unwindEpilogue(Frame, Current, State.Bounds);
          if (!Epilogue)
            return Epilogue.takeError();
          if (*Epilogue) {
            Current = **Epilogue;
            continue;
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
            return invalid("frame-register offset underflows");
          State.Establisher = FP - Frame.FrameOffset;
        }
        if (State.Establisher < SP || State.Establisher % seh::PointerSize ||
            !InStack(State.Establisher, seh::PointerSize))
          return invalid("establisher frame exceeds the current stack");
        if (State.Frame->SEH &&
            State.Frame->SEH->Scopes.size() > seh::MaxScopes)
          return invalid("SEH scope limit exceeded");
        if (!State.InPrologue && State.Frame->GSCookie &&
            (State.Frame->UnwindFlags & seh::ExceptionHandlerFlag))
          if (auto E =
                  checkGSCookie(*State.Frame, State.Establisher, State.Bounds))
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
        return invalid("collided unwind has an invalid scope cursor");
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
          return invalid("encountered incomplete SEH scope");
        if (Scope.Kind == SEHScopeKind::Finally)
          continue;
        if (Scope.Kind != SEHScopeKind::CatchAll &&
            Scope.Kind != SEHScopeKind::Filter)
          return invalid("encountered unsupported SEH scope");
        auto InFunction = ContainsContinuation(Scope.HandlerVA);
        if (!InFunction)
          return InFunction.takeError();
        if (!(Frame.UnwindFlags & seh::ExceptionHandlerFlag) ||
            Scope.NormalizedFilterVA ||
            Scope.HandlerVA != Scope.ContinuationVA || !*InFunction ||
            Scope.GuardedRange.contains(Scope.HandlerVA))
          return invalid("invalid exception handler continuation");
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
          return invalid("catch-all scope has an unexpected filter");
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
            return invalid("unwind allocation exceeds the current stack");
          Cursor += Op.StackOffset;
          break;
        case UnwindOperationKind::SetFramePointer:
          Cursor = State.Establisher;
          break;
        case UnwindOperationKind::SaveNonVolatile:
        case UnwindOperationKind::SaveNonVolatileFar: {
          if (Op.StackOffset > UINT64_MAX - State.Establisher)
            return invalid("saved-register address overflows");
          auto Value = Read(State.Establisher + Op.StackOffset);
          if (!Value)
            return Value.takeError();
          Current.GPR[Op.Register] = *Value;
          break;
        }
        case UnwindOperationKind::SaveXMM128:
        case UnwindOperationKind::SaveXMM128Far: {
          if (Op.StackOffset > UINT64_MAX - State.Establisher)
            return invalid("saved XMM address overflows");
          const uint64_t Address = State.Establisher + Op.StackOffset;
          if (!InStack(Address, seh::XmmSize))
            return invalid(
                "XMM unwind read exceeds the current execution stack");
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
          llvm_unreachable("unwind operation was validated");
        }
      }
    }
    auto Return = Read(Cursor);
    if (!Return)
      return Return.takeError();
    if (!*Return)
      return invalid("null unwind return address");
    Current.GPR[seh::StackRegister] = Cursor + seh::PointerSize;
    if (Current.GPR[seh::StackRegister] <= SP)
      return invalid("unwind did not advance the stack");
    Current.PC = *Return - 1;
    Current.FromReturnAddress = true;
    State.FrameActive = false;
  }
}

llvm::Expected<std::vector<uint8_t>>
KernelSEH::encodeRecords(const Exception &Raised, uint64_t Storage) {
  if (Storage % seh::RecordAlignment ||
      Storage > UINT64_MAX - seh::RecordsSize ||
      Raised.Parameters.size() > seh::MaxExceptionParameters)
    return invalid("invalid exception record storage or parameter count");
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
        seh::ContextIntegerControl, 4);
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
  return Bytes;
}

llvm::Expected<KernelSEH::Action>
KernelSEH::finishFilter(Dispatch &State, int32_t FilterResult,
                        llvm::ArrayRef<uint8_t> Records,
                        const Exception &Raised, uint64_t Storage) const {
  auto Context = validateRecords(Records, Raised, Storage);
  if (!Context)
    return Context.takeError();
  return advance(State, FilterResult);
}

llvm::Expected<KernelSEH::Context>
KernelSEH::validateRecords(llvm::ArrayRef<uint8_t> Records,
                           const Exception &Raised, uint64_t Storage) const {
  auto Expected = encodeRecords(Raised, Storage);
  if (!Expected)
    return Expected.takeError();
  if (Records.size() != Expected->size())
    return invalid("invalid exception record size");
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
    return invalid("continuation changes unsupported control flags");
  Exception Updated = Raised;
  Updated.Registers = Result;
  auto Allowed = encodeRecords(Updated, Storage);
  if (!Allowed)
    return Allowed.takeError();
  if (!std::equal(Records.begin(), Records.end(), Allowed->begin()))
    return invalid("continuation changes unsupported exception context fields");
  return Result;
}

llvm::Expected<KernelSEH::Context>
KernelSEH::continuation(llvm::ArrayRef<uint8_t> Records,
                        const Exception &Raised, uint64_t Storage,
                        Stack Bounds) const {
  auto Result = validateRecords(Records, Raised, Storage);
  if (!Result)
    return Result.takeError();
  const uint64_t SP = Result->GPR[seh::StackRegister];
  if (!Bounds.Size || Bounds.Size > UINT64_MAX - Bounds.Base ||
      SP < Bounds.Base || SP >= Bounds.Base + Bounds.Size ||
      SP % seh::PointerSize)
    return invalid("continuation stack pointer exceeds its execution stack");
  if (Result->PC < ActualBase || Result->PC - ActualBase >= ImageSize ||
      !Executable || !Executable(Result->PC))
    return invalid("continuation does not name executable image code");
  return Result;
}

} // namespace neverd::emulation
