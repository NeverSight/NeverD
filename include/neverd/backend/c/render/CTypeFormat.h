//===- CTypeFormat.h - Type to C string formatting -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Type-to-C-string formatting utilities.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_CTYPEFORMAT_H
#define NEVERD_BACKEND_C_RENDER_CTYPEFORMAT_H
#include "neverd/Common.h"
#include "neverd/ir/NdTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>

namespace llvm {
class Type;
class StructType;
} // namespace llvm

namespace neverd {

std::string typeToC(const TypeRef &Ty);

/// Place a declarator inside a C type, including nested function pointers.
/// An empty declarator produces the abstract type used by a cast.
std::string declarationToC(const TypeRef &Ty, llvm::StringRef Declarator);

std::string typeToCLLVM(llvm::Type *Ty);
std::string llvmStructName(llvm::StructType *ST);

std::string escapeCString(llvm::StringRef Str);

/// MSVC `<intrin.h>` GS/FS reads (`__readgsqword` / `__readfsdword`, …).
/// Implemented with the x86 C renderer.  \p SizeBytes is the access width.
/// Returns null when that width has no matching intrinsic.
const char *x86SegmentedReadIntrinsic(bool GS, unsigned SizeBytes);

/// The registers x64 Windows `int 2Dh` (the debug service) reads, in the
/// order the lifter passes them after the vector: the service code in RAX,
/// then RCX, RDX, R8 and R9.
llvm::ArrayRef<const char *> x86DebugServiceRegisters();

/// An x86 software interrupt as an MSVC `__asm` statement.  Inline asm cannot
/// take C expressions as operands, so each register input is first copied to a
/// block-scoped temporary.  \p Inputs pairs a register with the C text of its
/// value.  When \p ResultVar is not empty, \p ResultReg is moved into it
/// inside the same block.
std::string renderX86InterruptAsm(
    unsigned Vector,
    llvm::ArrayRef<std::pair<const char *, std::string>> Inputs,
    llvm::StringRef ResultVar, llvm::StringRef ResultReg);

/// Returns the platform-specific intrinsic headers for the given arch.
/// Dispatches to the per-arch lists implemented alongside the intrinsic
/// renderers (HighCIntrinsicRender{X86,ARM}.cpp).
llvm::SmallVector<const char *, 3> getArchIntrinsicHeaders(Arch TheArch);
llvm::SmallVector<const char *, 3> getX86IntrinsicHeaders();
llvm::SmallVector<const char *, 3> getARMIntrinsicHeaders();

/// Emits \p Level levels of indentation (4 spaces each) to \p OS.
void emitCIndent(llvm::raw_ostream &OS, int Level);

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_CTYPEFORMAT_H
