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

#include <optional>
#include <string>
#include <utility>

namespace llvm {
class Type;
class StructType;
} // namespace llvm

namespace neverd {

struct BinaryImage;

/// \p Ty as C.  Throws std::invalid_argument for a type C cannot spell, such
/// as a record with neither a source name nor a supported layout.
std::string typeToC(const TypeRef &Ty);

/// Whether typeToC spells \p Ty, for a caller that has another type to use
/// when it does not, such as the machine type of a debug parameter.
bool hasCSpelling(const TypeRef &Ty);

/// Whether a debug type has enough layout for a C value. A named opaque
/// record can be spelled behind a pointer, but its name alone does not
/// establish a by-value argument, local, or return ABI.
bool hasCValueLayout(const TypeRef &Ty);

/// Readonly printable C/wchar image bytes as `"..."` / `L"..."`.
/// Non-ASCII or writable/executable bytes stay unnamed. Empty NUL-only
/// strings stay unnamed unless \p AllowEmpty (MSVC `??_C@` publics).  Bytes
/// that belong to a data object the image's symbols size stay unnamed too,
/// unless the object is the string: an address inside the object points
/// into it, and an object whose bytes go on past the terminator with more
/// than zeros holds more than the text.
std::optional<std::string> imageStringLiteral(const BinaryImage *Img, va_t Addr,
                                              bool AllowEmpty = false);

/// Whether C text starts with a string literal (`"..."`, `L"..."`, `u8"..."`,
/// `u"..."`, `U"..."`), which C takes as an array, not as an integer.
bool isStringLiteralText(llvm::StringRef Text);

/// The string that starts at \p Addr in readable, non-executable image bytes,
/// as a comment beside a reference to it reads it: its text in quotes, after
/// its encoding's name unless that is ASCII or UTF-8 (`GBK "你好"`), with
/// controls escaped, `*` and `/` kept apart and long text cut.  None when no
/// string starts there.
std::optional<std::string> imageStringComment(const BinaryImage *Img,
                                              va_t Addr);

/// A string the image holds, as a C literal of exactly its bytes.
struct ImageCString {
  /// The element type: `char`, `char16_t` or `char32_t`, and its bytes.
  llvm::StringRef Element;
  unsigned UnitBytes = 1;
  /// `"..."`, `u"..."` or `U"..."`: the text where the UTF-8 source shows it,
  /// escapes for the bytes of another encoding and for controls.
  std::string Literal;
  /// Bytes the string occupies with its terminator.
  uint64_t Bytes = 0;
  /// The encoding and text when the literal escapes them (`GBK "你好"`, as
  /// imageStringComment reads them); empty when the literal shows the text.
  std::string Note;
};

/// The string that starts at \p Addr in readable, non-executable image bytes
/// as a C literal.  None when no string starts there, when a relocation
/// touches its bytes, or when its code units are not in the target's
/// (little-endian) byte order, which `u"..."` and `U"..."` hold.
std::optional<ImageCString> imageCString(const BinaryImage *Img, va_t Addr);

/// Named class/struct returned by value.  MSVC x64 passes that object through
/// a hidden pointer in RCX.  Enums stay in RAX.  Forward-ref classes may
/// have Size 0.
inline bool isMsvcClassValueReturn(const TypeRef &Ty, Arch Architecture,
                                   BinaryFormat Format) {
  return Architecture == Arch::X64 && Format == BinaryFormat::COFF && Ty &&
         Ty->Kind == NdTypeKind::Struct && !Ty->SourceName.empty() &&
         !Ty->IsEnum;
}

/// TPI sometimes writes `CStringT*` for a class-by-value return, and sometimes
/// a getter really returns `T*` in RAX.  Treat the pointer encoding as sret
/// only when a call site shows a hidden result pointer.
inline bool isMsvcPointerEncodedClassReturn(const TypeRef &Ty,
                                            Arch Architecture,
                                            BinaryFormat Format) {
  return Architecture == Arch::X64 && Format == BinaryFormat::COFF && Ty &&
         Ty->Kind == NdTypeKind::Ptr && Ty->Pointee &&
         Ty->Pointee->Kind == NdTypeKind::Struct &&
         !Ty->Pointee->SourceName.empty() && !Ty->Pointee->IsEnum;
}

/// MSVC x64 returns a named C++ class/struct through a hidden pointer in RCX.
/// Enums stay in RAX.  Pointer-to-class TPI is included so current-function
/// prototypes still inject `result` for `CStringT*` methods; externs must
/// also see a real sret operand.
inline TypeRef msvcIndirectReturnRecordType(const TypeRef &Ty,
                                            Arch Architecture,
                                            BinaryFormat Format) {
  if (isMsvcClassValueReturn(Ty, Architecture, Format))
    return Ty;
  if (isMsvcPointerEncodedClassReturn(Ty, Architecture, Format))
    return Ty->Pointee;
  return nullptr;
}

inline const NdType *msvcIndirectReturnRecord(const TypeRef &Ty,
                                              Arch Architecture,
                                              BinaryFormat Format) {
  return msvcIndirectReturnRecordType(Ty, Architecture, Format).get();
}

inline bool isMsvcIndirectReturn(const TypeRef &Ty, Arch Architecture,
                                 BinaryFormat Format) {
  return msvcIndirectReturnRecord(Ty, Architecture, Format) != nullptr;
}

/// Place a declarator inside a C type, including nested function pointers.
/// An empty declarator produces the abstract type used by a cast.
std::string declarationToC(const TypeRef &Ty, llvm::StringRef Declarator);

std::string typeToCLLVM(llvm::Type *Ty);
/// Fixed, power-of-two integer vectors with native C lane widths.
bool isCIntegerVectorType(llvm::Type *Ty);
/// Native C integer, float, double, and bfloat vector storage types.
bool isCVectorType(llvm::Type *Ty);
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
/// inside the same block.  A non-empty \p Instruction (e.g. `syscall`)
/// replaces the `int` instruction.
std::string renderX86InterruptAsm(
    unsigned Vector,
    llvm::ArrayRef<std::pair<const char *, std::string>> Inputs,
    llvm::StringRef ResultVar, llvm::StringRef ResultReg,
    llvm::StringRef Instruction = "");

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
