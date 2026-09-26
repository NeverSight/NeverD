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

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>

namespace llvm {
class Type;
class StructType;
} // namespace llvm

namespace neverd {

struct BinaryImage;

std::string typeToC(const TypeRef &Ty);

/// Readonly printable C/wchar image bytes as `"..."` / `L"..."`.
/// Non-ASCII or writable/executable bytes stay unnamed. Empty NUL-only
/// strings stay unnamed unless \p AllowEmpty (MSVC `??_C@` publics).
std::optional<std::string> imageStringLiteral(const BinaryImage *Img,
                                              va_t Addr,
                                              bool AllowEmpty = false);

/// Named class/struct returned by value.  MSVC x64 passes that object through
/// a hidden pointer in RCX.  Enums stay in RAX.  Forward-ref classes may
/// have Size 0.
inline bool isMsvcClassValueReturn(const TypeRef &Ty) {
  return Ty && Ty->Kind == NdTypeKind::Struct && !Ty->SourceName.empty() &&
         !Ty->IsEnum;
}

/// TPI sometimes writes `CStringT*` for a class-by-value return, and sometimes
/// a getter really returns `T*` in RAX.  Treat the pointer encoding as sret
/// only when a call site shows a hidden result pointer.
inline bool isMsvcPointerEncodedClassReturn(const TypeRef &Ty) {
  return Ty && Ty->Kind == NdTypeKind::Ptr && Ty->Pointee &&
         Ty->Pointee->Kind == NdTypeKind::Struct &&
         !Ty->Pointee->SourceName.empty() && !Ty->Pointee->IsEnum;
}

/// MSVC x64 returns a named C++ class/struct through a hidden pointer in RCX.
/// Enums stay in RAX.  Pointer-to-class TPI is included so current-function
/// prototypes still inject `result` for `CStringT*` methods; externs must
/// also see a real sret operand.
inline TypeRef msvcIndirectReturnRecordType(const TypeRef &Ty) {
  if (isMsvcClassValueReturn(Ty))
    return Ty;
  if (isMsvcPointerEncodedClassReturn(Ty))
    return Ty->Pointee;
  return nullptr;
}

inline const NdType *msvcIndirectReturnRecord(const TypeRef &Ty) {
  return msvcIndirectReturnRecordType(Ty).get();
}

inline bool isMsvcIndirectReturn(const TypeRef &Ty) {
  return msvcIndirectReturnRecord(Ty) != nullptr;
}

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

/// Returns the platform-specific intrinsic headers for the given arch.
/// Dispatches to the per-arch lists implemented alongside the intrinsic
/// renderers (HighCIntrinsicRender{X86,ARM}.cpp).
llvm::SmallVector<const char *, 3> getArchIntrinsicHeaders(Arch TheArch);
llvm::SmallVector<const char *, 3> getX86IntrinsicHeaders();
llvm::SmallVector<const char *, 3> getARMIntrinsicHeaders();

/// Emits \p Level levels of indentation (4 spaces each) to \p OS.
void emitCIndent(llvm::raw_ostream &OS, int Level);

/// Prefix every nonempty line of a multi-line snippet with \p Level indents.
/// A trailing newline does not emit an extra indented blank line.
void writeCIndentedSnippet(llvm::raw_ostream &OS, llvm::StringRef Text,
                           int Level);

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_CTYPEFORMAT_H
