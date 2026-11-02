//===- X87PortableExecution.cpp - Explicit generated x87 calls ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// No-throw C bridge to the shared transactional portable semantic owner.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X87PortableExecution.h"

#include "llvm/Support/ErrorHandling.h"

using namespace neverd::translate;

extern "C" uint32_t nvd_x87_v1_step(void *Opaque, uint32_t Selector,
                                    uint64_t GuestIP,
                                    uint64_t GuestDP) noexcept {
  if (!Opaque)
    return static_cast<uint32_t>(PortableX87StepStatusV1::InvalidState);
  auto &Execution = *static_cast<PortableX87ExecutionV1 *>(Opaque);
  Execution.ResultAvailable = false;
  Execution.LastStep = {};
  Execution.LastIP = GuestIP;
  Execution.LastDP = GuestDP;
  try {
    Execution.LastStep = executePortableX87RuntimeStepV1(
        Execution.State, Execution.Memory, Selector, GuestIP, GuestDP);
  } catch (...) {
    // Allocation/planning failures cannot cross the plain C call boundary.
    // The transaction adapter performs all allocating work before committing.
    Execution.LastStep.Status = PortableX87StepStatusV1::InternalError;
  }
  Execution.ResultAvailable =
      Execution.LastStep.Status == PortableX87StepStatusV1::Completed &&
      Execution.LastStep.HasScalarResult;
  return static_cast<uint32_t>(Execution.LastStep.Status);
}

extern "C" uint64_t nvd_x87_v1_result(void *Opaque) noexcept {
  if (!Opaque)
    llvm::report_fatal_error("portable x87 result has no execution context");
  auto &Execution = *static_cast<PortableX87ExecutionV1 *>(Opaque);
  if (!Execution.ResultAvailable)
    llvm::report_fatal_error(
        "portable x87 result has no unconsumed successful producer");
  Execution.ResultAvailable = false;
  return Execution.LastStep.ScalarResult;
}
