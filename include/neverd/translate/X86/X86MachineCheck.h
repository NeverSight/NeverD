//===- X86MachineCheck.h - Virtual Intel MCA banks ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Conventional global and per-bank machine-check state of one KVM Intel CPU.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86MACHINECHECK_H
#define NEVERD_TRANSLATE_X86_X86MACHINECHECK_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::translate {

struct X86MachineCheckBank {
  uint64_t Control = UINT64_MAX, Status = 0, Address = 0, Misc = 0;
  uint32_t Control2 = 0;
  // Zero selects a bank whose CMCI controls always read zero.
  uint16_t CorrectedThresholdMask = 0x7fff;
  bool operator==(const X86MachineCheckBank &) const = default;
};

struct X86MachineCheckReport {
  unsigned Bank = 0;
  uint64_t Status = 0, Address = 0, Misc = 0, GlobalStatus = 0;
  // A device submits one corrected occurrence without precomputing the
  // architectural count. The default imports an already formed KVM record.
  bool CountCorrectedError = false;
};

enum class X86MachineCheckAction { NoException, Deliver, Shutdown, NotifyCMCI };

struct X86MachineCheckState {
  static constexpr unsigned MaxBanks = 32;
  std::vector<X86MachineCheckBank> Banks;
  std::optional<uint64_t> GlobalControl;
  uint64_t GlobalStatus = 0;
  bool CMCI = false;
  bool operator==(const X86MachineCheckState &) const = default;

  static bool ownsMSR(uint32_t Index);
  llvm::Error validate() const;
  llvm::Expected<X86MachineCheckAction>
  report(const X86MachineCheckReport &Input, bool Enabled);
  /// Empty reads and rejected writes produce #GP(0) after instruction
  /// admission.
  std::optional<uint64_t> readMSR(uint32_t Index) const;
  bool writeMSR(uint32_t Index, uint64_t Value, bool &ClearedBankEnables);
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86MACHINECHECK_H
