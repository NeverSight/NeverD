//===- UnicornUnavailable.cpp - Native-only build boundary ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64Machine.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"

#include "neverd/emulation/arch/mips/P5600MSA.h"
namespace neverd::emulation {
llvm::Expected<std::unique_ptr<MIPSMSAExecutor>> createP5600MSAExecutor(Arch) {
  return diagnostic::unavailable(diagnostic::UnicornDisabled,
                                 BackendAvailability::BuildDisabled);
}
llvm::Expected<std::unique_ptr<X64Machine>>
createUnicornX64Machine(MemoryProjection &, bool) {
  return diagnostic::unavailable(diagnostic::UnicornDisabled,
                                 BackendAvailability::BuildDisabled);
}
llvm::Expected<std::unique_ptr<AArch64Machine>>
createUnicornAArch64Machine(MemoryProjection &, bool) {
  return diagnostic::unavailable(diagnostic::UnicornDisabled,
                                 BackendAvailability::BuildDisabled);
}
} // namespace neverd::emulation
