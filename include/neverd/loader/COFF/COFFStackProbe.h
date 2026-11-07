//===- COFFStackProbe.h - Checked PE stack probes -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Authenticates complete x86 and x64 stack-probe protocols from immutable
/// caller and helper bytes, separately from named helper ABI recognition.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFSTACKPROBE_H
#define NEVERD_LOADER_COFF_COFFSTACKPROBE_H

#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <optional>

namespace neverd::coff_loader {

/// Exact immutable `mov eax, allocation; call helper` and its allocating
/// page-probe implementation. The helper preserves EBP and the other GPRs
/// except EAX/ESP, touches each descending page and moves the return PC below
/// the allocation. EAX returns the call's continuation PC; arithmetic flags
/// are those of the final page comparison. On a nonwrapping successful path,
/// returned ESP is the caller's pre-call ESP minus AllocationBytes. The
/// helper clamps an underflowed target to zero and still probes its pages.
///
/// This source description grants no ordinary callee ABI, frame lifetime,
/// native EH or call-elision permission. Page faults, the underflow path and
/// both transient stack stores remain observable. Caller instruction ownership
/// and the incoming EAX/ESP identities require separate source proofs.
struct X86AllocatingStackProbeCall {
  ExceptionAddressRange Caller;
  ExceptionAddressRange Helper;
  va_t CallVA = 0;
  uint32_t AllocationBytes = 0;
};

std::optional<X86AllocatingStackProbeCall>
getCheckedX86AllocatingStackProbeCall(const BinaryImage &Image, va_t CallVA);

/// A byte-authenticated x64 page probe reached by a direct near CALL. On an
/// uninterrupted normal return, RAX is unchanged and bounds the descending
/// allocation. This is stronger than recognizing the helper's ABI by name.
/// It does not model fault recovery or authorize eliding a call.
struct X64StackProbeCall {
  ExceptionAddressRange Helper;

  // Even with 57-bit linear addresses, descending 4 KiB probes cannot cross
  // the noncanonical hole. Include the first unprobed step in this bound;
  // no assumption about the caller's absolute stack address is needed.
  static constexpr uint64_t MaximumAllocation = (uint64_t{1} << 57) + 4095;
};

std::optional<X64StackProbeCall>
getCheckedX64StackProbeCall(const BinaryImage &Image, va_t CallVA);

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_COFFSTACKPROBE_H
