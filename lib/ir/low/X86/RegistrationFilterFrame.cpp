//===- RegistrationFilterFrame.cpp - Continuing SEH search effects -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separates returning search effects from handler and nonlocal exits.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/RegistrationScopeGraph.h"

#include <deque>
#include <optional>

namespace neverd::registration_state {

bool RegistrationStateSolver::runSEHFilterSearch(
    const LowOp &Raise, const FrameState &Parent,
    std::vector<RegistrationObjectExtent> *Writes, SEHFrameOutcomes &Outcomes,
    bool RequireResume, SEHHandlerOutcomes *Selected, SEHUnwindJump *Jump) {
  if (KnownCxx || !Chain.TryLevelOffset ||
      (EH4 && (!CompleteCookies || !cookiesReady(Parent))))
    return false;
  const auto Level = Parent.load(*Chain.TryLevelOffset, 4).Constant;
  if (!Level || !validLevel(int32_t(*Level)))
    return false;
  const auto &SP = Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  const auto Code =
      SP.CallbackAddress
          ? Parent.loadCallback(SP.CallbackAddress->Offset, 4).Constant
          : Parent.load(SP.Offset, 4).Constant;
  const std::optional<SEHExceptionOrigin> Origin =
      Code
          ? std::make_optional(SEHExceptionOrigin{Raise.Addr, Raise.Seq, *Code})
          : std::nullopt;
  struct Search {
    FrameState Frame;
    int32_t Cursor;
    size_t Steps;
  };
  std::deque<Search> Pending{{Parent, int32_t(*Level), 0}};
  while (!Pending.empty()) {
    Search Current = std::move(Pending.front());
    Pending.pop_front();
    if (!charge(Current.Frame.cellCount() +
                Current.Frame.OtherRegisterBytes.size() + 10))
      return false;
    if (Current.Cursor == registrationOutsideTryLevel(EH4)) {
      // A search outside this registration can return through an external
      // handler. It contributes ordinary effects but cannot close a bounded
      // nonlocal-unwind proof.
      if (RequireResume ||
          !mergeSEHFrameOutcome(Outcomes, std::move(Current.Frame)))
        return false;
      continue;
    }
    if (size_t(Current.Cursor) >= Chain.Scopes.size() ||
        ++Current.Steps > Chain.Scopes.size())
      return false;
    const auto &Scope = Chain.Scopes[Current.Cursor];
    if (Scope.IsFinally) {
      Pending.push_back(
          {std::move(Current.Frame), Scope.EnclosingLevel, Current.Steps});
      continue;
    }
    SEHFilterOutcomes Returned;
    if (!Scope.FilterVA ||
        !runSEHCallbackFrame(Scope.FilterVA, Current.Frame, false, Writes, Jump,
                             &Returned, 0, std::nullopt, nullptr, nullptr,
                             Origin))
      return false;
    for (auto &[Disposition, Frames] : Returned)
      for (auto &[State, Frame] : Frames) {
        if (EH4 && (!CompleteCookies || !cookiesReady(Frame)))
          return false;
        // The checked CRT tests EAX's sign bit; a negative LONG continues.
        if (int32_t(Disposition) < 0) {
          if (!mergeSEHFrameOutcome(Outcomes, std::move(Frame)))
            return false;
        } else if (Disposition == 0) {
          // The runtime captured the search cursor before invoking the
          // filter. Its state write affects later unwind, not this cursor.
          Pending.push_back(
              {std::move(Frame), Scope.EnclosingLevel, Current.Steps});
        } else if (RequireResume || Selected) {
          // Selection abandons the suspended invocation. The unwind owner
          // must finish the selected scope's local unwind before dispatch.
          if (!Selected || !Scope.HandlerVA ||
              !Entries.count(Scope.HandlerVA) ||
              !mergeSEHFrameOutcome((*Selected)[uint32_t(Current.Cursor)],
                                    std::move(Frame)))
            return false;
        }
      }
  }
  return !RequireResume || !Outcomes.empty() ||
         (Selected && !Selected->empty()) || (Jump && !Jump->Resumed.empty());
}

} // namespace neverd::registration_state
