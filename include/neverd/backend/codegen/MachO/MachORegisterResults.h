//===- MachORegisterResults.h - Native register entries -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bind decoded physical register operands and unwind rows to patch-mode LLVM
/// bodies.
///
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
struct MedFunc;

/// An indexed access using the physical SP cannot use a reconstructed stack.
bool needsMachONativeStackIndex(const MedFunc &Function);

/// Lower audited patch-mode entries that use Darwin variadic cursors, indexed
/// SP accesses, ordinary indexed/writeback memory, scalar FP/NEON data
/// operations or X8/X9 results consumed by an image-local direct caller,
/// including across local control-flow edges.
/// Keep physical operands, control flow and unwind rows in one native body.
/// Return an error if any declaration withheld for a physical SP access
/// cannot be defined; the caller must discard the incomplete module.
llvm::Error
lowerMachORegisterResults(llvm::Module &Module, const BinaryImage &Image,
                          llvm::ArrayRef<va_t> AdmittedEntries,
                          llvm::ArrayRef<va_t> RequiredNativeEntries);

/// Check each register body and its symbolic call operands against the image
/// before submitting its inline assembly to MC. Returns the original entries
/// for the separate verification of the final linked machine instructions.
llvm::Expected<llvm::DenseSet<va_t>>
validateMachORegisterResults(llvm::Module &Module, const BinaryImage &Image);
} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOREGISTERRESULTS_H
