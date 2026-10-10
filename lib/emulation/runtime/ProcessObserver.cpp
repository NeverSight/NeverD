//===- ProcessObserver.cpp - Shared stopped-image observation ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ProcessObserver.h"

#include "../core/ExecutionDiagnostics.h"

namespace neverd::emulation {
ProcessView::~ProcessView() = default;
std::optional<ProcessModuleView> ProcessView::inputModule() {
  for (const auto &Module : modules())
    if (Module.Main)
      return Module;
  return std::nullopt;
}
llvm::Expected<uint32_t> ProcessView::instructionSize(uint64_t) {
  return diagnostic::error(diagnostic::InstructionInspectionUnsupported);
}
ProcessObserver::~ProcessObserver() = default;
llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
ProcessObserver::invoking(ProcessView &) {
  return std::nullopt;
}
llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
ProcessObserver::resuming(ProcessView &) {
  return std::nullopt;
}
llvm::Error ProcessObserver::exporting(ProcessView &, const ProcessExportView &,
                                       std::optional<uint64_t>) {
  return llvm::Error::success();
}

} // namespace neverd::emulation
