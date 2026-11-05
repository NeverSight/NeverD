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
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace neverd {

/// A producer-declared Win64 scalar view, not an inferred native signature.
/// Widths are 1, 2, 4 or 8 bytes. Every argument must match the existing entry
/// state; neither register padding nor original stack storage is initialized.
struct X64FunctionCallInterface {
  va_t Entry = 0;
  std::vector<unsigned> ArgumentBytes;
  unsigned ResultBytes = 0;
};

/// Append checked call/resume wrappers after the V6 function context driver.
/// No instruction body or driver is copied. Returns their manifest entries.
llvm::json::Array
emitX64FunctionCalls(llvm::raw_ostream &OS,
                     llvm::ArrayRef<X64FunctionCallInterface> Calls);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLS_H
