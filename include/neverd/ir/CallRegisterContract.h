//===- CallRegisterContract.h - Registers a call keeps ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The register contract a call follows where it is not the image's platform
/// convention: which registers keep their values across it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_CALLREGISTERCONTRACT_H
#define NEVERD_IR_CALLREGISTERCONTRACT_H

#include <cstdint>

namespace neverd {

/// Which registers a call keeps (GoCallContract.h decides it for a call).
enum class CallRegisterContract : uint8_t {
  /// The image's platform convention (TargetRegInfo::callPreservedRanges).
  Platform,
  /// A call Go code makes.  Go's ABIs have no callee-saved registers
  /// (src/cmd/compile/abi-internal.md: "There are no callee-save registers,
  /// so a call may overwrite any register that doesn't have a fixed
  /// meaning"), so only the stack and frame pointers keep their values, and
  /// the registers a summary proves the callee never writes.
  GoCode,
  /// A call to a Go runtime write barrier (GoWriteBarrierKind): every
  /// general-purpose register keeps its value but the one it returns buffer
  /// space in and arm64's R27; flags and vector registers do not.
  GoWriteBarrier,
};

} // namespace neverd

#endif // NEVERD_IR_CALLREGISTERCONTRACT_H
