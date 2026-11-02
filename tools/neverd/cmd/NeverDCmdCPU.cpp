//===- NeverDCmdCPU.cpp - CPU capability query routing -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../NeverDCLI.h"

#include "neverd/emulation/ExecutionReportFields.h"

#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
int runCPUCapabilities() {
  auto Session = neverd_session_create();
  if (!Session) {
    llvm::WithColor::error()
        << emulation::execution_report::SessionFailed << '\n';
    return 1;
  }
  SessionGuard Guard(Session);
  const char *Report = neverd_cpu_capabilities_json(
      Session, CPUConfiguration.getValue().c_str(), CPUProbeHost ? 1 : 0);
  if (!Report) {
    llvm::WithColor::error() << takeLastError(Session) << '\n';
    return 1;
  }
  llvm::outs() << Report << '\n';
  neverd_free_string(Report);
  return 0;
}

int runX87Capabilities() {
  auto Session = neverd_session_create();
  if (!Session) {
    llvm::WithColor::error() << "could not create a capability-query session\n";
    return 1;
  }
  SessionGuard Guard(Session);
  const char *Report = neverd_x87_capabilities_json(Session);
  if (!Report) {
    llvm::WithColor::error() << takeLastError(Session) << '\n';
    return 1;
  }
  llvm::outs() << Report << '\n';
  neverd_free_string(Report);
  return 0;
}
} // namespace neverd::cli
