//===- InterpreterMachineStateProfile.h - Recovery environment -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETERMACHINESTATEPROFILE_H
#define NEVERD_ANALYSIS_INTERPRETERMACHINESTATEPROFILE_H

#include <cstdint>

namespace neverd::analysis {

enum class InterpreterMachineStateProfile : uint8_t {
  /// 64-bit user mode, CPL=3, IOPL=0, shadow stacks disabled, no asynchronous
  /// events or debug-exception delivery, and successful nonfaulting ordinary
  /// execution. TF, RF, VM, AC, VIF and VIP are zero at entry; every executed
  /// POPFQ image must keep TF and AC zero. Reserved bits have their
  /// architectural
  /// values. These conditions are explicit preconditions, not inferred from a
  /// successful run. TF/AC rejection restricts this profile; it is not a model
  /// of a hardware exception caused immediately by POPFQ.
  UserX64NoFaultV1,
  /// 64-bit supervisor mode, CPL=0, IOPL=0, shadow stacks disabled, no
  /// asynchronous events, debug-exception delivery or privilege transitions,
  /// and successful nonfaulting execution. Entry TF/RF/VM/AC/VIF/VIP are zero;
  /// every POPFQ image must keep TF, AC and IOPL zero. IF is writable and is
  /// included in the mandatory final system-state observation. Rejection of
  /// a nonzero TF/AC/IOPL image restricts this profile, not hardware semantics.
  /// This native-proof profile supplies no kernel API, IRQL, resource,
  /// exception-dispatch or source-ABI evidence. User machine-source projection
  /// continues to reject it rather than applying CPL=3 semantics.
  KernelX64NoFaultV1,
  /// The supervisor V1 domain with CPUID.07H.00H:EBX.SMAP[20]=1 and a live
  /// AC bit. STAC/CLAC and POPFQ may change AC; TF and IOPL remain zero.
  /// Successful ordinary memory accesses are still an explicit precondition
  /// for every step, including the page-access permissions selected by AC.
  /// This does not model page tables, SMAP faults or user-mode execution.
  KernelX64SmapNoFaultV2,
};

constexpr bool
isKernelX64MachineStateProfile(InterpreterMachineStateProfile Profile) {
  return Profile == InterpreterMachineStateProfile::KernelX64NoFaultV1 ||
         Profile == InterpreterMachineStateProfile::KernelX64SmapNoFaultV2;
}

/// A provider-owned projection whose meaning requires an explicit environment.
/// It never promotes the original architectural undefined-output sidecar.
enum class InterpreterProfileProjection : uint8_t {
  None,
  CetDisabledReadShadowStackV1,
  CetDisabledIncrementShadowStackTrapV1,
};

} // namespace neverd::analysis

#endif
