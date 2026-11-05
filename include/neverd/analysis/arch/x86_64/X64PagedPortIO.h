//===- X64PagedPortIO.h - Explicit port device transactions ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind scalar and string port transfers to an isolated synchronous device.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDPORTIO_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDPORTIO_H

#include "neverd/analysis/arch/x86_64/X64PagedEventDelivery.h"
#include "neverd/translate/X86/X86PortIO.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::analysis {

// Preserve the x64 interface while sharing device/permission ownership.
using X64PagedPortIOProfile = translate::X86PortIOProfile;
using X64PortIORequest = translate::X86PortIORequest;
using X64PagedPortIOBinding = translate::X86PortIOBinding;
using X64PagedPortIOResult = translate::X86PortIOResult;

/// Caller authenticates instruction fetch and admission. Success publishes one
/// element's registers and ordinary RAM together; a guest fault may retain an
/// input device read and, with the fragment profile, an earlier RAM fragment.
/// Index/count publication still requires completion of the entire element.
/// Fault delivery and budget accounting belong to X64PagedExecution. No hidden
/// progress state is retained.
llvm::Expected<X64PagedPortIOResult> executeX64PagedPortIO(
    X64ContinuationStateV6 &State, translate::X86PagedMemory &Memory,
    llvm::ArrayRef<uint8_t> Bytes, X64PagedAlignmentPriority AlignmentPriority,
    const X64PagedPortIOBinding &Binding);

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDPORTIO_H
