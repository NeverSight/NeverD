//===- KernelModelIRQL.cpp - Guest IRQL raise and restore -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pair actual x64 WDK IRQL imports on one cooperative guest execution.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

namespace neverd::emulation {
namespace {
llvm::Error irqlError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::callIRQLAPI(llvm::StringRef Name,
                         llvm::ArrayRef<uint64_t> Arguments) {
  const uint8_t RequestedIRQL = static_cast<uint8_t>(Arguments[0]);
  if (!CurrentExecution)
    return irqlError("IRQL change requires an active guest execution");
  if (Name == "KfRaiseIrql") {
    if (RequestedIRQL > dispatcher::HighLevel || RequestedIRQL < CurrentIRQL)
      return irqlError("KfRaiseIrql must raise to a valid IRQL");
    if (RaisedIRQLs.size() >= 64)
      return irqlError("nested IRQL raise limit exceeded");
    const uint8_t PreviousIRQL = CurrentIRQL;
    RaisedIRQLs.push_back(
        RaisedIRQL{CurrentExecution, PreviousIRQL, RequestedIRQL});
    CurrentIRQL = RequestedIRQL;
    return PreviousIRQL;
  }
  if (Name != "KeLowerIrql")
    return irqlError("unknown IRQL operation");
  if (RaisedIRQLs.empty() || RaisedIRQLs.back().Execution != CurrentExecution ||
      RaisedIRQLs.back().NewIRQL != CurrentIRQL ||
      RaisedIRQLs.back().OldIRQL != RequestedIRQL)
    return irqlError("KeLowerIrql requires the latest saved IRQL on this "
                     "execution");
  if (RequestedIRQL < scheduler::DispatchLevel) {
    if (CancelLock.Held)
      return irqlError("cannot lower IRQL while holding the cancel spin lock");
    for (const auto &[Address, Lock] : ExecutiveSpinLocks)
      if (Lock.Execution == CurrentExecution)
        return irqlError(
            "cannot lower IRQL while holding an executive spin lock");
  }
  if (auto Required = Interrupts.manualHoldIRQL(CurrentExecution);
      Required && RequestedIRQL < *Required)
    return irqlError("cannot lower IRQL while holding an interrupt spin lock");
  RaisedIRQLs.pop_back();
  CurrentIRQL = RequestedIRQL;
  return 0;
}
} // namespace neverd::emulation
