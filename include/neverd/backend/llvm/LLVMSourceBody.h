//===- LLVMSourceBody.h - Source-only function definitions ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Mark definitions owned by a source projection, with no native entry ABI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMSOURCEBODY_H
#define NEVERD_BACKEND_LLVM_LLVMSOURCEBODY_H

#include "llvm/ADT/StringRef.h"

namespace neverd {

/// A selected C unit must include the marked definitions it calls. Ordinary
/// native callees remain declarations, even when their bodies are available.
inline constexpr llvm::StringLiteral LLVMSourceBody("neverd.source-body");

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_LLVMSOURCEBODY_H
