//===- X86MachineCheck.cpp - Conventional x86 MCA reporting --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Record, prioritize and acknowledge machine errors for the single-vCPU
/// conventional-bank profile. Inputs carry the platform's actual error record.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86MachineCheck.h"

#include "llvm/Support/Errc.h"

#include <algorithm>

namespace neverd::translate {
namespace {
constexpr uint32_t MCGCap = 0x179, MCGStatus = 0x17a, MCGControl = 0x17b;
constexpr uint32_t MC0Control2 = 0x280, MC0Control = 0x400;
constexpr uint64_t Valid = uint64_t{1} << 63, Overflow = uint64_t{1} << 62,
                   Uncorrected = uint64_t{1} << 61, Corrupt = uint64_t{1} << 57,
                   Signaled = uint64_t{1} << 56,
                   ActionRequired = uint64_t{1} << 55, InProgress = 4;
constexpr uint32_t CMCIEnable = 1u << 30, ThresholdMask = 0x7fff;
constexpr unsigned CorrectedCountShift = 38;
constexpr uint64_t CorrectedCountMask = uint64_t(ThresholdMask)
                                        << CorrectedCountShift;
constexpr uint16_t StickyCountOverflow = 0x4000;

constexpr uint64_t X86MachineCheckBank::*Fields[] = {
    &X86MachineCheckBank::Control, &X86MachineCheckBank::Status,
    &X86MachineCheckBank::Address, &X86MachineCheckBank::Misc};
bool validControl(uint64_t Value) {
  return !Value || (Value | 0x401) == UINT64_MAX;
}
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}
} // namespace

bool X86MachineCheckState::ownsMSR(uint32_t Index) {
  return (Index >= MCGCap && Index <= MCGControl) ||
         (Index >= MC0Control && Index - MC0Control < 4 * MaxBanks) ||
         (Index >= MC0Control2 && Index - MC0Control2 < MaxBanks);
}

llvm::Error X86MachineCheckState::validate() const {
  if (Banks.empty() || Banks.size() > MaxBanks ||
      (GlobalControl && *GlobalControl && *GlobalControl != UINT64_MAX) ||
      std::any_of(Banks.begin(), Banks.end(), [&](const auto &B) {
        const unsigned Mask = B.CorrectedThresholdMask;
        const uint32_t Writable = CMCI && Mask ? (CMCIEnable | Mask) : 0;
        return !validControl(B.Control) || Mask > ThresholdMask ||
               (Mask & (Mask + 1)) || (B.Control2 & ~Writable);
      }))
    return invalid("invalid conventional x86 machine-check bank binding");
  return llvm::Error::success();
}

llvm::Expected<X86MachineCheckAction>
X86MachineCheckState::report(const X86MachineCheckReport &Input, bool Enabled) {
  if (auto E = validate())
    return std::move(E);
  if (Input.Bank >= Banks.size() || !(Input.Status & Valid))
    return invalid(
        "machine-check report requires a present bank and a valid status");
  auto &Bank = Banks[Input.Bank];
  const bool UC = Input.Status & Uncorrected;
  if (Input.CountCorrectedError &&
      (!CMCI || UC || (Input.Status & CorrectedCountMask) ||
       Input.GlobalStatus))
    return invalid(
        "a counted corrected error requires CMCI and a fresh corrected record");
  const bool NoAction = UC && !Input.GlobalStatus &&
                        !(Input.Status & (Corrupt | Signaled | ActionRequired));
  if (UC && !NoAction) {
    if ((GlobalControl && *GlobalControl != UINT64_MAX) ||
        Bank.Control != UINT64_MAX)
      return X86MachineCheckAction::NoException;
    if (!Enabled || (GlobalStatus & InProgress))
      return X86MachineCheckAction::Shutdown;
  }
  if (!UC && (Bank.Status & (Valid | Uncorrected)) == (Valid | Uncorrected)) {
    Bank.Status |= Overflow;
    return X86MachineCheckAction::NoException;
  }
  const bool Overwrite = (Bank.Status & Valid) && !NoAction;
  uint64_t Status = Input.Status;
  bool ThresholdReached = false;
  if (Input.CountCorrectedError) {
    const auto Previous = Bank.Status & Valid
                              ? unsigned((Bank.Status & CorrectedCountMask) >>
                                         CorrectedCountShift)
                              : 0u;
    const unsigned Count =
        ((Previous + 1) & ThresholdMask) | (Previous & StickyCountOverflow);
    Status |= uint64_t(Count) << CorrectedCountShift;
    ThresholdReached = Count == (Bank.Control2 & ThresholdMask);
  }
  Bank.Status = Status | (Overwrite ? Overflow : 0);
  Bank.Address = Input.Address;
  Bank.Misc = Input.Misc;
  if (UC)
    GlobalStatus = Input.GlobalStatus;
  if ((NoAction || ThresholdReached) && CMCI && (Bank.Control2 & CMCIEnable))
    return X86MachineCheckAction::NotifyCMCI;
  return UC && !NoAction ? X86MachineCheckAction::Deliver
                         : X86MachineCheckAction::NoException;
}

std::optional<uint64_t> X86MachineCheckState::readMSR(uint32_t Index) const {
  if (Index == MCGCap)
    return Banks.size() | (GlobalControl ? 0x100 : 0) | (CMCI ? 0x400 : 0);
  if (Index == MCGStatus)
    return GlobalStatus;
  if (Index == MCGControl)
    return GlobalControl;
  if (CMCI && Index >= MC0Control2 && Index - MC0Control2 < Banks.size())
    return Banks[Index - MC0Control2].Control2;
  if (Index < MC0Control || Index - MC0Control >= 4 * Banks.size())
    return {};
  const auto Offset = Index - MC0Control;
  return Banks[Offset / 4].*Fields[Offset % 4];
}

bool X86MachineCheckState::writeMSR(uint32_t Index, uint64_t Value,
                                    bool &ClearedBankEnables) {
  ClearedBankEnables = false;
  if (CMCI && Index >= MC0Control2 && Index - MC0Control2 < Banks.size()) {
    if (Value & ~uint64_t(CMCIEnable | ThresholdMask))
      return false;
    auto &Bank = Banks[Index - MC0Control2];
    const uint32_t Mask = Bank.CorrectedThresholdMask;
    Bank.Control2 = Mask ? uint32_t(Value) & (CMCIEnable | Mask) : 0;
    return true;
  }
  if (Index == MCGStatus) {
    GlobalStatus = Value;
    return true;
  }
  if (Index == MCGControl) {
    if (!GlobalControl || (Value && Value != UINT64_MAX))
      return false;
    GlobalControl = Value;
    return true;
  }
  if (Index < MC0Control || Index - MC0Control >= 4 * Banks.size())
    return false;
  const auto Offset = Index - MC0Control;
  if ((Offset % 4 == 0 && !validControl(Value)) || (Offset % 4 == 1 && Value))
    return false;
  auto &Cell = Banks[Offset / 4].*Fields[Offset % 4];
  ClearedBankEnables = Offset % 4 == 0 && (Cell & ~Value);
  Cell = Value;
  return true;
}

} // namespace neverd::translate
