//===- WindowsEHMetadataEncoder.h - Windows EH metadata encoder -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Canonical serialization of normalized Windows exception information into
/// LLVM metadata.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_WINDOWSEHMETADATAENCODER_H
#define NEVERD_BACKEND_LLVM_WINDOWSEHMETADATAENCODER_H

#include "neverd/Common.h"

namespace llvm {
class LLVMContext;
class MDNode;
} // namespace llvm

namespace neverd {

struct ExceptionFunction;

namespace windows_eh_md {

/// Return the uniqued current-schema metadata projection of \p EH in
/// \p Context.
/// Re-encoding the same record in the same context returns the same node.
llvm::MDNode *getCanonicalFunctionMetadata(llvm::LLVMContext &Context,
                                           const ExceptionFunction &EH,
                                           Arch TargetArch,
                                           BinaryFormat TargetFormat);

/// Return a conservative projection when no target identity is available.
/// The lossless fields are identical, but the language-regeneration operand
/// is false because eligibility cannot be established without a target.
llvm::MDNode *getCanonicalFunctionMetadata(llvm::LLVMContext &Context,
                                           const ExceptionFunction &EH);

} // namespace windows_eh_md
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_WINDOWSEHMETADATAENCODER_H
