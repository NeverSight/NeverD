//===- RegistrationStateCookies.cpp - x86 EH cookie proof -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include <algorithm>
#include <utility>

namespace neverd::registration_state {

void RegistrationStateSolver::initializeCookies() {
  if (KnownCxx && CxxHandler && !CxxHandler->CookieGuards.empty()) {
    initializeCxxCookies();
    return;
  }
  CompleteCookies =
      EH4 && SecurityCookieVA && SecurityCookieVA <= UINT32_MAX - 3 &&
      Chain.ScopeTableVA <= UINT32_MAX - 16 && Chain.EHCookieOffset < -2;
  EHCookieSlot = cookieSlot(Chain.EHCookieOffset);
  GSCookieSlot = Chain.GSCookieOffset != -2 ? cookieSlot(Chain.GSCookieOffset)
                                            : std::optional<int32_t>{};
  if (!EH4 || !GSCookieSlot || CookieCheckVA > UINT32_MAX)
    CookieCheckVA = 0;
  CompleteCookies &= EHCookieSlot.has_value() &&
                     (Chain.GSCookieOffset == -2 || GSCookieSlot.has_value());
}

void RegistrationStateSolver::initializeCxxCookies() {
  CookieCheckVA = 0;
  if (!CheckCalls || !CheckCleanups || !SecurityCookieVA ||
      SecurityCookieVA > UINT32_MAX - 3 ||
      SecurityCookieVA != CxxHandler->SecurityCookieVA ||
      CxxHandler->CookieGuards.size() > 2)
    return;
  std::vector<RegistrationCxxCookieCondition> Conditions;
  va_t Checker = 0;
  for (const auto &Guard : CxxHandler->CookieGuards) {
    const int64_t Slot =
        int64_t(*Chain.RegistrationOffset) + Guard.CookieOffset;
    const int64_t XOR = int64_t(*Chain.RegistrationOffset) + Guard.XOROffset;
    // A cleanup-only frame need not own SavedESP. A catch frame does, and a
    // cookie cannot occupy any of that independently used runtime word.
    const int64_t End = int64_t(*Chain.RegistrationOffset) -
                        (EH.Cxx->TryBlocks.empty() ? 0 : 4);
    if (!charge(1) || Slot < -int64_t(limits::kMaxSourceCallBorrowedBytes) ||
        Slot + 4 > End || XOR < INT32_MIN || XOR > INT32_MAX ||
        !Guard.CheckVA || Guard.CheckVA > UINT32_MAX ||
        (Checker && Checker != Guard.CheckVA))
      return;
    Checker = Guard.CheckVA;
    Conditions.push_back({int32_t(Slot), int32_t(XOR)});
  }
  CxxCookies = true;
  CookieCheckVA = Checker;
  CxxCookieConditions = std::move(Conditions);
  CompleteCookies = true;
  auto PreservesCookie = [&](const auto &Contract) {
    if (!charge(1))
      return false;
    for (const auto &Write : Contract.ImageWrites) {
      if (!charge(1))
        return false;
      if (Write.Begin < SecurityCookieVA + 4 && SecurityCookieVA < Write.End)
        return false;
    }
    return true;
  };
  for (const auto &Contract : Result.CalleeContracts)
    CompleteCookies &= PreservesCookie(Contract);
  for (const auto &Cleanup : Result.CleanupContracts)
    for (const auto &Call : Cleanup.Calls)
      CompleteCookies &= PreservesCookie(Call.Leaf);
}

bool RegistrationStateSolver::cxxCookiesReady(const Domain &State) const {
  const auto SP = parentStackOffset(State);
  if (!CxxCookies || State.Unknown || !SP)
    return false;
  for (const auto &Condition : CxxCookieConditions) {
    const auto Value = State.Frame.load(Condition.FrameOffset, 4);
    if (Condition.FrameOffset < *SP || !Value.SecurityCookie ||
        Value.CookieFrameOffset != Condition.XORFrameOffset ||
        Value.CookieXOR || Value.CookieImageXOR)
      return false;
    for (int32_t Byte = 0; Byte != 4; ++Byte)
      if (!State.InitializedFrameBytes.count(Condition.FrameOffset + Byte))
        return false;
  }
  return true;
}

std::optional<int32_t>
RegistrationStateSolver::cookieSlot(int32_t Displacement) const {
  const int64_t Offset = int64_t(*Chain.RegistrationOffset) + 16 + Displacement;
  if (Offset < INT32_MIN || Offset > INT32_MAX ||
      Offset + 4 > int64_t(*Chain.RegistrationOffset) - 8)
    return std::nullopt;
  return int32_t(Offset);
}

bool RegistrationStateSolver::cookiesReady(const FrameState &Frame) const {
  const auto SP =
      Frame.CallbackEntry
          ? Frame.load(*Chain.RegistrationOffset - 8, 4).Offset
          : Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  auto Matches = [&](std::optional<int32_t> Slot, int32_t XOROffset) {
    if (!Slot || !SP || *Slot < *SP)
      return false;
    const int64_t ExpectedFrame =
        int64_t(*Chain.RegistrationOffset) + 16 + XOROffset;
    const auto It = Frame.Cells.find(*Slot);
    return ExpectedFrame >= INT32_MIN && ExpectedFrame <= INT32_MAX &&
           It != Frame.Cells.end() && It->second.SecurityCookie &&
           It->second.CookieFrameOffset == int32_t(ExpectedFrame) &&
           It->second.CookieXOR == 0 && !It->second.CookieImageXOR;
  };
  const auto Table = Frame.Cells.find(*Chain.RegistrationOffset + 8);
  return Table != Frame.Cells.end() && Table->second.SecurityCookie &&
         !Table->second.CookieFrameOffset &&
         Table->second.cookieAddress() == Chain.ScopeTableVA &&
         Matches(EHCookieSlot, Chain.EHCookieXOROffset) &&
         (Chain.GSCookieOffset == -2 ||
          Matches(GSCookieSlot, Chain.GSCookieXOROffset));
}

bool RegistrationStateSolver::hasCompleteCookieChecks() {
  size_t Checked = 0;
  for (const auto &Identity : CookieCheckOccurrences) {
    if (!charge(1))
      return false;
    if (CookieChecks.count(Identity)) {
      ++Checked;
      continue;
    }
    // A preserved helper is proved for these invocation inputs. Its unchanged
    // unreachable code supplies no cookie observation on this call. A parent
    // being reconstructed still needs a receipt for every source occurrence.
    if (!Conditional || !Conditional->Complete)
      return false;
    const auto Boundary = Boundaries.find(Identity.first);
    if (Boundary == Boundaries.end())
      return false;
    const auto Block = Index.find(Boundary->second.first);
    if (Block == Index.end() || Block->second >= Result.Blocks.size() ||
        Result.Blocks[Block->second].Reached)
      return false;
  }
  return Checked == CookieChecks.size();
}

} // namespace neverd::registration_state
