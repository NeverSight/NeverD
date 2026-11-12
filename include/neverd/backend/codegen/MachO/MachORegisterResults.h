//===- MachORegisterResults.h - Darwin native register entries -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOREGISTERRESULTS_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOREGISTERRESULTS_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Error.h"

namespace llvm {
class Module;
} // namespace llvm

namespace neverd {
struct BinaryImage;

/// Lower patch-mode entries that expose Darwin variadic cursors or X8/X9
/// results consumed by an image-local direct caller.
/// Keep their physical register operands, local control flow and unwind rows
/// together in the native LLVM body before any client rewrite.
void lowerMachORegisterResults(llvm::Module &Module, const BinaryImage &Image,
                               llvm::ArrayRef<va_t> AdmittedEntries);

/// Check each register body and its symbolic call operands against the image
/// before submitting its inline assembly to MC. Returns the original entries
/// for the separate verification of the final linked machine instructions.
llvm::Expected<llvm::DenseSet<va_t>>
validateMachORegisterResults(llvm::Module &Module, const BinaryImage &Image);
} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOREGISTERRESULTS_H
