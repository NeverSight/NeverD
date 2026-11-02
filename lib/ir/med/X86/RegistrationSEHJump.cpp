//===- RegistrationSEHJump.cpp - Source nonlocal jump calls ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Bind checked nonlocal jump lifetimes to their surviving MedIR calls.

#include "RegistrationSEHJumpPrivate.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationSEHBoundCall.h"
#include "neverd/ir/med/X86/X86RegistrationSEHJump.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <set>

namespace neverd {

std::optional<std::map<va_t, X86RegistrationSEHJumpCall>>
getX86RegistrationSEHJumpCalls(const MedFunc &Function) {
  if (!Function.RegistrationStates || Function.SkippedSSA)
    return std::nullopt;
  const auto &States = *Function.RegistrationStates;
  if (!States.SEHJumpCapturesComplete || !States.SEHJumpResumptionsComplete ||
      States.HasSEHJumpCaptures != !States.SEHJumpCaptures.empty())
    return std::nullopt;
  std::map<va_t, X86RegistrationSEHJumpCall> Result;
  if (States.SEHJumpCaptures.empty() && States.SEHJumpResumptions.empty())
    return Result;
  // An empty jump protocol needs no callee ABI binding. Parent-exit and
  // finally-register proofs also run before SSA resolves unrelated imports.
  const auto Bound = getX86RegistrationSEHBoundCalls(Function);
  if (!Bound)
    return std::nullopt;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration)
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  const auto &Chain = *EH.Registration;
  const bool EH4 = EH.Personality == ExceptionPersonality::ExceptHandler4;
  if ((!EH4 && EH.Personality != ExceptionPersonality::ExceptHandler3) ||
      (EH4 && (!States.SecurityCookiesComplete || !States.SecurityCookieVA)))
    return std::nullopt;
  std::map<va_t, const MedBlock *> Blocks;
  std::map<int, const MedBlock *> BlocksById;
  std::map<int, const RegistrationBlockState *> BlockStates;
  std::map<va_t, const RegistrationSEHJumpCapture *> Captures;
  size_t Work = 0;
  for (const auto &State : States.Blocks)
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        !BlockStates.emplace(State.BlockId, &State).second)
      return std::nullopt;
  for (const auto &Block : Function.Blocks) {
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        !Blocks.emplace(Block.StartAddr, &Block).second ||
        !BlocksById.emplace(Block.Id, &Block).second)
      return std::nullopt;
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Op.Opcode == NdOp::CALL &&
          !Result
               .emplace(Op.Addr, X86RegistrationSEHJumpCall{nullptr, nullptr,
                                                            &Op, Block.Id})
               .second)
        return std::nullopt;
    }
  }
  auto Bind = [&](va_t Address, int Seq) -> X86RegistrationSEHJumpCall * {
    const auto It = Result.find(Address);
    if (It == Result.end())
      return nullptr;
    const auto &Op = *It->second.Operation;
    if (Op.Dead || Op.ExactArguments || Op.SourceCallHint ||
        Op.OriginSeq != Seq || Seq < 0 || Op.NumInputs != 1 ||
        !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return nullptr;
    return &It->second;
  };
  const auto ValidState = [&](int32_t State) {
    return Chain.SeededTryLevel &&
           (State == *Chain.SeededTryLevel ||
            (State >= 0 && size_t(State) < Chain.Scopes.size()));
  };
  for (const auto &Capture : States.SEHJumpCaptures) {
    auto *Call = Bind(Capture.Address, Capture.OpSeq);
    Work += Capture.Levels.size();
    if (++Work > limits::kMaxRegistrationEHStateWork || !Call ||
        Call->Operation->Inputs[0].ConstVal != Capture.TargetVA ||
        Capture.Address > UINT32_MAX - 5 ||
        Capture.Address + 5 != Capture.EndAddress ||
        Capture.UnwindVA == InvalidVA || !Capture.UnwindVA ||
        int64_t(Capture.BufferOffset) + Capture.BufferBytes > -24 ||
        int64_t(Capture.StackOffset) + (EH4 ? 20 : 16) > Capture.BufferOffset ||
        Capture.Protocol != (EH4 ? RegistrationSEHJumpCapture::Kind::EH4
                                 : RegistrationSEHJumpCapture::Kind::EH3) ||
        Capture.SecurityCookieVA != (EH4 ? States.SecurityCookieVA : 0) ||
        Capture.Levels.empty() || !llvm::is_sorted(Capture.Levels) ||
        std::adjacent_find(Capture.Levels.begin(), Capture.Levels.end()) !=
            Capture.Levels.end() ||
        !llvm::all_of(Capture.Levels, ValidState) ||
        !Blocks.count(Capture.EndAddress) ||
        !Captures.emplace(Capture.Address, &Capture).second)
      return std::nullopt;
    if (EH4) {
      auto OverlapsCookie = [&](int32_t Offset) {
        return int64_t(Capture.BufferOffset) + Capture.BufferBytes > Offset &&
               int64_t(Offset) + 4 > Capture.BufferOffset;
      };
      if (Chain.RegistrationOffset != -16 || Chain.TryLevelOffset != -4 ||
          Chain.EHCookieOffset >= -2 || OverlapsCookie(Chain.EHCookieOffset) ||
          (Chain.GSCookieOffset != -2 && OverlapsCookie(Chain.GSCookieOffset)))
        return std::nullopt;
    }
    const auto *Block = BlocksById.at(Call->Block);
    const auto State = BlockStates.find(Call->Block);
    if (Block->Succs.size() != 1 ||
        Block->Succs.front() != Blocks.at(Capture.EndAddress)->Id ||
        Block->EndAddr != Capture.EndAddress ||
        Call->Operation->DoesNotReturn || State == BlockStates.end() ||
        State->second->Unknown || State->second->CallbackOnly ||
        State->second->levels() != Capture.Levels)
      return std::nullopt;
    Call->Capture = &Capture;
  }
  struct Resumption {
    std::set<int32_t> Levels;
    std::map<std::pair<int32_t, int32_t>, int32_t> Projection;
    bool Complete = true;
  };
  std::map<va_t, Resumption> ResumeLevels;
  for (const auto &Resume : States.SEHJumpResumptions) {
    auto *Call = Bind(Resume.Address, Resume.OpSeq);
    const auto Capture = Captures.find(Resume.CaptureCall);
    if (++Work > limits::kMaxRegistrationEHStateWork || !Call ||
        Capture == Captures.end() || (Call->Capture && !Call->Resume) ||
        (Resume.ResumedLevels.empty() && Resume.SelectedScopes.empty() &&
         Resume.Replacements.empty()) ||
        Resume.TargetVA != Capture->second->EndAddress ||
        Resume.BufferOffset != Capture->second->BufferOffset ||
        Resume.StackOffset != Capture->second->StackOffset)
      return std::nullopt;
    if (Resume.CalleeVA || Resume.ResumeVA) {
      const auto Callee = Bound->find(Resume.Address);
      if (Callee == Bound->end() || Resume.ValueOffset || Resume.EntryVA ||
          Resume.OrdinaryCall || !Resume.ResumeVA ||
          Callee->second.Source->TargetVA != Resume.CalleeVA ||
          !llvm::any_of(Callee->second.Source->Arguments, [&](const auto &Arg) {
            return Arg.ArgumentKind ==
                       RegistrationSEHBoundArgument::Kind::JumpBuffer &&
                   Arg.Capture == Resume.CaptureCall;
          }))
        return std::nullopt;
    } else if (!Resume.ValueOffset || !Call->Operation->DoesNotReturn)
      return std::nullopt;
    Work += Resume.ResumedLevels.size() + Resume.StateRestores.size() +
            Resume.SelectedScopes.size();
    if (Work > limits::kMaxRegistrationEHStateWork ||
        !llvm::is_sorted(Resume.SelectedScopes) ||
        std::adjacent_find(Resume.SelectedScopes.begin(),
                           Resume.SelectedScopes.end()) !=
            Resume.SelectedScopes.end() ||
        !llvm::is_sorted(Resume.ResumedLevels) ||
        std::adjacent_find(Resume.ResumedLevels.begin(),
                           Resume.ResumedLevels.end()) !=
            Resume.ResumedLevels.end() ||
        !llvm::is_sorted(Resume.StateRestores) ||
        std::adjacent_find(Resume.StateRestores.begin(),
                           Resume.StateRestores.end()) !=
            Resume.StateRestores.end())
      return std::nullopt;
    for (uint32_t ScopeIndex : Resume.SelectedScopes) {
      if (ScopeIndex >= Chain.Scopes.size())
        return std::nullopt;
      const auto &Scope = Chain.Scopes[ScopeIndex];
      const auto Handler = Blocks.find(Scope.HandlerVA);
      if (Scope.IsFinally || !Scope.FilterVA || Handler == Blocks.end())
        return std::nullopt;
      const auto State = BlockStates.find(Handler->second->Id);
      if (State != BlockStates.end())
        Work += State->second->levels().size();
      if (Work > limits::kMaxRegistrationEHStateWork ||
          State == BlockStates.end() || !State->second->Reached ||
          State->second->Unknown || State->second->CallbackOnly ||
          !llvm::is_contained(State->second->levels(), Scope.EnclosingLevel))
        return std::nullopt;
    }
    auto &Plan = ResumeLevels[Resume.CaptureCall];
    Plan.Levels.insert(Resume.ResumedLevels.begin(),
                       Resume.ResumedLevels.end());
    Plan.Complete &=
        Resume.ResumedLevels.empty() || !Resume.StateRestores.empty();
    std::set<int32_t> Restored;
    for (const auto &State : Resume.StateRestores) {
      if (!ValidState(State.FrameLevel) || !ValidState(State.ResumeLevel) ||
          !std::binary_search(Capture->second->Levels.begin(),
                              Capture->second->Levels.end(),
                              State.CaptureLevel))
        return std::nullopt;
      const auto [It, New] = Plan.Projection.emplace(
          std::make_pair(State.CaptureLevel, State.FrameLevel),
          State.ResumeLevel);
      if (!New && It->second != State.ResumeLevel)
        return std::nullopt;
      Restored.insert(State.ResumeLevel);
    }
    if (!Resume.StateRestores.empty() &&
        !llvm::equal(Restored, Resume.ResumedLevels))
      return std::nullopt;
    if (Resume.CalleeVA)
      continue;
    if (Resume.EntryVA) {
      const auto &Scopes = Function.ExceptionMetadata->Registration->Scopes;
      Work += Scopes.size() + States.SEHControls.size();
      const auto State = BlockStates.find(Call->Block);
      if (Work > limits::kMaxRegistrationEHStateWork ||
          *Resume.ValueOffset > -4 || State == BlockStates.end() ||
          State->second->Unknown || !State->second->CallbackOnly ||
          !Blocks.count(Resume.EntryVA) ||
          !llvm::any_of(Scopes, [&](const auto &Scope) {
            return (Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA) ==
                   Resume.EntryVA;
          }))
        return std::nullopt;
      if (Resume.OrdinaryCall) {
        const auto Found = Result.find(Resume.OrdinaryCall);
        auto *Caller =
            Found == Result.end()
                ? nullptr
                : Bind(Resume.OrdinaryCall, Found->second.Operation->OriginSeq);
        if (!Caller ||
            Caller->Operation->Inputs[0].ConstVal != Resume.EntryVA ||
            !llvm::any_of(States.SEHControls, [&](const auto &Control) {
              return Control.Control ==
                         RegistrationSEHControlContract::Kind::NearCall &&
                     Control.Address == Resume.OrdinaryCall &&
                     Control.Target == Resume.EntryVA;
            }))
          return std::nullopt;
      }
    } else if (Resume.OrdinaryCall)
      return std::nullopt;
    // One instruction can run through ordinary and runtime finally entries.
    // Every invocation must agree on the native buffer and argument projection.
    if (Call->Resume) {
      auto Previous = *Call->Resume;
      if (Previous.OrdinaryCall == Resume.OrdinaryCall &&
          (Previous.ResumedLevels != Resume.ResumedLevels ||
           Previous.StateRestores != Resume.StateRestores ||
           Previous.SelectedScopes != Resume.SelectedScopes ||
           Previous.Replacements != Resume.Replacements))
        return std::nullopt;
      Previous.OrdinaryCall = Resume.OrdinaryCall;
      Previous.ResumedLevels = Resume.ResumedLevels;
      Previous.StateRestores = Resume.StateRestores;
      Previous.SelectedScopes = Resume.SelectedScopes;
      Previous.Replacements = Resume.Replacements;
      if (Previous != Resume)
        return std::nullopt;
      continue;
    }
    Call->Capture = Capture->second;
    Call->Resume = &Resume;
  }
  if (!hasCompleteSEHJumpReplacements(States, Work))
    return std::nullopt;
  std::erase_if(Result, [](const auto &Item) { return !Item.second.Capture; });
  for (auto &[Address, Call] : Result) {
    const auto Level = ResumeLevels.find(Call.Capture->Address);
    if (Level == ResumeLevels.end() || Level->second.Levels.empty()) {
      if (Call.Capture->Levels.size() == 1)
        Call.ResumeLevel = Call.Capture->Levels.front();
      else
        Call.ResumeLevel.reset();
    } else if (Level->second.Levels.size() == 1) {
      Call.ResumeLevel = *Level->second.Levels.begin();
    } else {
      if (!Level->second.Complete)
        return std::nullopt;
      Call.ResumeLevel.reset();
      for (const auto &[Frame, Resumed] : Level->second.Projection)
        if (Frame.second != Resumed)
          Call.ResumeStateMap.emplace(Frame, Resumed);
    }
  }
  return Result;
}

} // namespace neverd
