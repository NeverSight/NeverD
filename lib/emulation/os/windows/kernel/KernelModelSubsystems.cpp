//===- KernelModelSubsystems.cpp - Scoped subsystem call contracts -------===//
///
/// \file
/// Scoped subsystem call contracts.
///

#include "KernelModel.h"
namespace neverd::emulation {
namespace {
struct Contract {
  llvm::StringLiteral Module, Name;
  unsigned Arity, IRQL;
};
const Contract *lookup(const KernelExportRegistry::Export &E) {
  if (E.Kind != KernelExportRegistry::ExportKind::ModuleExport)
    return nullptr;
  static constexpr Contract Entries[] = {
#define NEVERD_SUBSYSTEM_API(M, N, A, I) {M, #N, A, I},
#include "KernelSubsystemAPIs.def"
#undef NEVERD_SUBSYSTEM_API
  };
  for (const auto &C : Entries)
    if (E.Module == C.Module && E.Name == C.Name)
      return &C;
  return nullptr;
}
} // namespace
std::optional<unsigned>
KernelModel::subsystemArgumentCount(const KernelExportRegistry::Export &E) {
  if (const auto *C = lookup(E))
    return C->Arity;
  return std::nullopt;
}
llvm::Expected<uint64_t>
KernelModel::callSubsystem(const KernelExportRegistry::Export &E,
                           llvm::ArrayRef<uint64_t> A) {
  APICallDetail.clear();
  const auto *C = lookup(E);
  if (!C || A.size() != C->Arity)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "unknown scoped subsystem API or wrong arity");
  if (CurrentIRQL > C->IRQL)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "subsystem call requires IRQL <= %u",
                                   C->IRQL);
  if (auto Context = executionProcessContext();
      Context && hasProcessAttachment(CurrentExecution))
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "subsystem call during process attachment is unsupported");
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "subsystem call requires windows_version 10.0.19041");
  if (E.Module == "fltmgr.sys")
    return callFilterName(E.Name, A);
  if (E.Module == "netio.sys")
    return callNetio(E.Name, A);
  if (E.Module == "ndis.sys")
    return callNdisConfiguration(E.Name, A);
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "unimplemented subsystem owner");
}
} // namespace neverd::emulation
