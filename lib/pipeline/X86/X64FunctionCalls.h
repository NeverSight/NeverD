//===- X64FunctionCalls.h - Checked source call interfaces ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Scalar C call views of an explicitly supplied native function context.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLS_H
#define NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLS_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace neverd {

/// A producer-declared Win64 scalar view, not an inferred native signature.
/// Widths are 1, 2, 4 or 8 bytes. Every argument must match the existing entry
/// state; neither register padding nor original stack storage is initialized.
/// An absent width retains that physical slot only in native state. It has no
/// scalar formal or entry check and does not renumber subsequent slots.
struct X64FunctionCallInterface {
  va_t Entry = 0;
  std::vector<std::optional<unsigned>> ArgumentBytes;
  unsigned ResultBytes = 0;
  /// Optional original direct CALL instructions at which the caller's full
  /// state is supplied. Their targets must equal Entry and be selected.
  std::vector<va_t> CallSites;
};

/// Shared scalar declarations/checks for entry and pre-CALL stack coordinates.
/// The parameter writer returns the last checked stack byte, if any. Checks
/// return the caller's local `rejected` result without changing native state.
std::optional<uint64_t>
emitX64FunctionCallParameters(llvm::raw_ostream &OS,
                              const X64FunctionCallInterface &Call,
                              uint64_t StackBase);
llvm::json::Array emitX64FunctionCallArguments(
    llvm::raw_ostream &OS, const X64FunctionCallInterface &Call,
    uint64_t StackBase, llvm::StringRef StackOffsetKey);

/// Append checked call/resume wrappers after the V6 function context driver.
/// No instruction body or driver is copied. Returns their manifest entries.
/// Paged declarations have already been checked to retain every stack slot in
/// native state. Only register comparisons precede the paged driver.
/// Scheduled views share the scheduler and expose its producer/shadow inputs;
/// they do not invoke the ordinary environment provider implicitly.
llvm::json::Array
emitX64FunctionCalls(llvm::raw_ostream &OS,
                     llvm::ArrayRef<X64FunctionCallInterface> Calls,
                     bool PagedMemory = false, bool ScheduledEvents = false);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLS_H
