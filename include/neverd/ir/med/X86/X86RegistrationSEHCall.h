//===- X86RegistrationSEHCall.h - Checked finally calls --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared MedIR evidence for finally-call ABI recovery and native returns.

#ifndef NEVERD_IR_MED_X86_X86REGISTRATIONSEHCALL_H
#define NEVERD_IR_MED_X86_X86REGISTRATIONSEHCALL_H

#include "neverd/Common.h"

#include <map>
#include <optional>
#include <tuple>

namespace neverd {
struct MedFunc;

struct RegistrationSEHReturns {
  /// Returning source block to its finally entry.
  std::map<int, va_t> ReturnBlocks;
  /// Exact (block, address, original sequence) CALL identity to finally entry.
  /// Stable identities survive unrelated SSA result normalization.
  std::map<std::tuple<int, va_t, int>, va_t> ReturningCalls;
};

/// Bind checked physical near returns to their current CALLs and successors.
/// These invocations share the parent frame and have no ordinary ABI register
/// or stack arguments. Explicit SEH register exchanges carry their machine
/// values; native emission owns their private frame and result projection.
std::optional<RegistrationSEHReturns>
getRegistrationSEHReturns(const MedFunc &Function);

/// Bind ordinary finally calls with checked nonlocal jump exits. A call can
/// also have normal returns; consumers must prove their absence separately
/// before assigning noreturn to the generated invocation.
std::optional<std::map<std::tuple<int, va_t, int>, va_t>>
getRegistrationSEHNonlocalCalls(const MedFunc &Function);

/// Bind adjacent finally calls which cannot resume at their physical return
/// PC. Maps the caller block to the callback entry block, accepting either the
/// original edge or its checked noreturn form for independent source replay.
std::optional<std::map<int, int>>
getRegistrationSEHAdjacentExits(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_IR_MED_X86_X86REGISTRATIONSEHCALL_H
