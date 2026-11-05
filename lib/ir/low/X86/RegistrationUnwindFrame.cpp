//===- RegistrationUnwindFrame.cpp - SEH unwind effects ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replays the checked SEH unwind protocol, reloading callback-written state.

#include "RegistrationStateSolver.h"

#include "neverd/loader/RegistrationScopeGraph.h"

#include <bit>
#include <cassert>
#include <map>
#include <tuple>
#include <vector>

namespace neverd::registration_state {

bool RegistrationStateSolver::runSEHUnwind(
    FrameState Frame, int32_t Level, uint32_t Bound, SEHFrameOutcomes &Outcomes,
    std::vector<RegistrationObjectExtent> &Writes, SEHJumpUnwind *Jump,
    SEHHandlerOutcomes *Selected,
    std::set<std::pair<int32_t, int32_t>> *Transitions,
    std::optional<int32_t> ParentStackOffset) {
  if (KnownCxx || !Chain.TryLevelOffset || !validLevel(Level) ||
      (Jump && Bound != uint32_t(Jump->Capture.Level)) ||
      (EH4 && (!CompleteCookies || !cookiesReady(Frame))))
    return false;
  struct Step {
    FrameState Frame;
    int32_t VisibleLevel;
    uint32_t Bound;
    std::optional<uint32_t> Handler;
    FrameValue Value;
    std::optional<int32_t> Destination;
    std::optional<RegistrationSEHJumpReplacement> Replacement;
    bool Leave = false;
  };
  Frame.store(*Chain.TryLevelOffset, 4, FrameValue::constant(uint32_t(Level)));
  std::vector<Step> Pending;
  Pending.push_back(
      {std::move(Frame), Level, Bound, std::nullopt,
       Jump ? Jump->Value : FrameValue{},
       Jump ? std::optional(Jump->Capture.BufferOffset) : std::nullopt,
       std::nullopt});
  // Keep only the active DFS path. Copying the entire path into every next
  // step makes even a straight scope chain quadratic in frame storage/work.
  // Equal frames must have equal proved TryLevel cells, so other levels need
  // neither a frame comparison nor a separate copy of the ancestry.
  using Position = std::tuple<int32_t, uint32_t, std::optional<uint32_t>,
                              FrameValue, std::optional<int32_t>>;
  std::map<Position, std::vector<FrameState>> Ancestors;
  while (!Pending.empty()) {
    Step Current = std::move(Pending.back());
    Pending.pop_back();
    if (!charge(1 + 2 * std::bit_width(Ancestors.size() + 1)))
      return false;
    if (Current.Leave) {
      auto It =
          Ancestors.find({Current.VisibleLevel, Current.Bound, Current.Handler,
                          Current.Value, Current.Destination});
      assert(It != Ancestors.end() && !It->second.empty());
      const auto &Last = It->second.back();
      if (!charge(Last.cellCount() + Last.OtherRegisterBytes.size() + 1))
        return false;
      It->second.pop_back();
      if (It->second.empty())
        Ancestors.erase(It);
      continue;
    }
    const auto NativeLevel =
        Current.Frame.load(Chain.TryLevelOffset, 4).Constant;
    if (!NativeLevel || !validLevel(int32_t(*NativeLevel)) ||
        !charge(Current.Frame.cellCount() +
                Current.Frame.OtherRegisterBytes.size() + 10))
      return false;
    const int32_t Walk = int32_t(*NativeLevel);
    // Keep the original checked CRT's unsigned stop predicate. A callback can
    // retire or redirect the state, so its enclosing link is not the next
    // iteration's state unless the callback actually preserved that value.
    const int32_t Outside = registrationOutsideTryLevel(EH4);
    if (Walk == Outside || (Current.Bound != uint32_t(Outside) &&
                            uint32_t(Walk) <= Current.Bound)) {
      if (Current.Handler) {
        if (!Selected || (EH4 && !cookiesReady(Current.Frame)))
          return false;
        const auto &Handler = Chain.Scopes[*Current.Handler];
        Current.Frame.store(
            *Chain.TryLevelOffset, 4,
            FrameValue::constant(uint32_t(Handler.EnclosingLevel)));
        Current.Frame.leaveCallback();
        if (!mergeSEHFrameOutcome((*Selected)[*Current.Handler],
                                  std::move(Current.Frame)))
          return false;
        continue;
      }
      if (Jump) {
        const auto Capture = Current.Destination
                                 ? Jump->Captures.find(*Current.Destination)
                                 : Jump->Captures.end();
        if (Capture == Jump->Captures.end() ||
            !hasSEHJumpCapture(Current.Frame, Capture->second) ||
            !mergeSEHFrameOutcome(
                Jump->Resumed[*Current.Destination][Current.Value],
                Current.Frame))
          return false;
        if (!charge(1))
          return false;
        if (*Current.Destination != Jump->Capture.BufferOffset) {
          if (!Current.Replacement)
            return false;
          Jump->Replacements.insert(*Current.Replacement);
          continue;
        }
        Jump->StateRestores.insert(
            {Current.VisibleLevel, Walk, Jump->Capture.Level});
      }
      if (!mergeSEHFrameOutcome(Outcomes, std::move(Current.Frame)))
        return false;
      if (Transitions) {
        if (!charge(1 + 2 * std::bit_width(Transitions->size() + 1)))
          return false;
        Transitions->emplace(Level, Walk);
      }
      continue;
    }
    if (Walk < 0 || size_t(Walk) >= Chain.Scopes.size())
      return false;
    auto &AtLevel = Ancestors[{Walk, Current.Bound, Current.Handler,
                               Current.Value, Current.Destination}];
    for (const auto &Ancestor : AtLevel) {
      if (!charge(Ancestor.cellCount() + Ancestor.OtherRegisterBytes.size() +
                  Current.Frame.cellCount() +
                  Current.Frame.OtherRegisterBytes.size() + 10) ||
          Ancestor == Current.Frame)
        return false;
    }
    // Re-entering a state with changed frame facts can be a finite protocol.
    // Exact recurrent configurations are refused; all other exploration,
    // including ancestor storage and branch fanout, shares the work budget.
    if (!charge(Current.Frame.cellCount() +
                Current.Frame.OtherRegisterBytes.size() + 10))
      return false;
    AtLevel.push_back(Current.Frame);
    // Leave after all children, so siblings may converge on the same frame
    // without borrowing each other's ancestry or hiding a recurrent path.
    Pending.push_back({{},
                       Walk,
                       Current.Bound,
                       Current.Handler,
                       Current.Value,
                       Current.Destination,
                       Current.Replacement,
                       true});
    const auto &Scope = Chain.Scopes[Walk];
    Current.Frame.store(*Chain.TryLevelOffset, 4,
                        FrameValue::constant(uint32_t(Scope.EnclosingLevel)));
    SEHFrameOutcomes Returned;
    SEHHandlerOutcomes Selections;
    std::optional<SEHUnwindJump> Repeated;
    if (Jump)
      Repeated.emplace(SEHUnwindJump{Jump->Captures, {}});
    if (Scope.IsFinally) {
      if (!runSEHCallbackFrame(Scope.HandlerVA, Current.Frame, false, &Writes,
                               Repeated ? &*Repeated : nullptr, nullptr, 0,
                               ParentStackOffset, &Returned,
                               Selected ? &Selections : nullptr))
        return false;
    } else if (!mergeSEHFrameOutcome(Returned, std::move(Current.Frame))) {
      return false;
    }
    if (!charge(Returned.size() + Selections.size() +
                (Repeated ? Repeated->Resumed.size() : 0) + 1))
      return false;
    for (auto It = Returned.rbegin(); It != Returned.rend(); ++It) {
      auto &[NextLevel, NextFrame] = *It;
      // A completed finally exposes its current state to the source frame.
      // Skipping a filter changes only the runtime cursor; native resumption
      // must project the old logical cell through those remaining steps.
      Pending.push_back({std::move(NextFrame),
                         Scope.IsFinally ? NextLevel : Current.VisibleLevel,
                         Current.Bound, Current.Handler, Current.Value,
                         Current.Destination, Current.Replacement});
    }
    for (auto &[ScopeIndex, Frames] : Selections) {
      if (ScopeIndex >= Chain.Scopes.size() ||
          Chain.Scopes[ScopeIndex].IsFinally || !charge(Frames.size() + 1))
        return false;
      for (auto &[NextLevel, NextFrame] : Frames)
        Pending.push_back({std::move(NextFrame), NextLevel, ScopeIndex,
                           ScopeIndex, Current.Value, Current.Destination,
                           Current.Replacement});
    }
    if (Repeated)
      for (auto &[Transfer, Frames] : Repeated->Resumed) {
        // A new jump reloads the current state but restores the capture's
        // bound. The pending handler and its bound no longer own this path.
        if (!charge(Frames.size() + 1))
          return false;
        const auto Capture = Jump->Captures.find(Transfer.BufferOffset);
        if (Capture == Jump->Captures.end())
          return false;
        auto Replacement = Current.Replacement;
        if (!Replacement && Transfer.BufferOffset != Jump->Capture.BufferOffset)
          Replacement = Transfer.Source;
        for (auto &[NextLevel, NextFrame] : Frames) {
          NextFrame.leaveCallback();
          Pending.push_back({std::move(NextFrame), NextLevel,
                             uint32_t(Capture->second.Level), std::nullopt,
                             Transfer.Value, Transfer.BufferOffset,
                             Replacement});
        }
      }
  }
  return !Outcomes.empty() || (Selected && !Selected->empty()) ||
         (Jump && !Jump->Resumed.empty());
}

} // namespace neverd::registration_state
