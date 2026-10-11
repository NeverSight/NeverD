//===- COFFRegistrationEH.h - x86-32 registration-chain EH ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers Windows exception state for x86-32 images, which carry no
/// `.pdata` directory at all.
///
/// On x86-32 the unwinder walks a linked list of `EXCEPTION_REGISTRATION_
/// RECORD`s rooted at `FS:[0]` that each frame's prologue pushes onto the
/// stack.  The exception tables are therefore not reachable from a directory:
/// they are reachable only from the instructions that install the record, so
/// recovery starts by proving that install sequence in the code and follows
/// the operands it pushes.  Every address that reaches a caller has been
/// checked against the image, and a table whose shape could not be proven is
/// reported rather than guessed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFREGISTRATIONEH_H
#define NEVERD_LOADER_COFF_COFFREGISTRATIONEH_H

#include "neverd/loader/BinaryImage.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd::coff_loader {

/// Decode an absolute 32-bit FS:[0] register store in executable image bytes.
/// A frame descriptor cannot determine whether the encoder used A3 or ModRM.
std::optional<uint8_t> getX86RegistrationChainStoreSize(const BinaryImage &Img,
                                                        va_t Address);

/// Scan an x86-32 image for `FS:[0]` registration installs and decode the
/// `_except_handler3`/`_except_handler4` scope tables and `__CxxFrameHandler`
/// `FuncInfo` records they reference.  A no-op for other architectures.
///
/// Must run after function discovery and the COFF symbol table are available,
/// because a recovered record is attributed to the function that contains its
/// install site and handler names come from symbols and import veneers.
void parseX86RegistrationExceptions(BinaryImage &Img);

/// Complete bounded wire decoding without inferring a registration frame or
/// a callable function from data pointers. Indexed compiler owners can use
/// this same format decoder to reanalyze an installed generated language graph.
struct X86CxxFuncInfoRecords {
  CxxExceptionInfo Cxx;
  std::vector<ExceptionAddressRange> Ranges;
  std::map<va_t, va_t> CallbackPointerSources;
  /// Analysis may retain an otherwise valid graph with aliased native records;
  /// replacement requires distinct record owners independently of decoding.
  bool HasDistinctRanges = true;
};
std::optional<X86CxxFuncInfoRecords>
getCheckedX86CxxFuncInfoRecords(const BinaryImage &Img, va_t FuncInfoVA);

/// The PE32 byte extent of the normalized SEH3/EH4 scope graph, including
/// EH4's four cookie fields. This describes format-owned storage only; it
/// grants neither immutable runtime contents nor permission to copy a table.
std::optional<ExceptionAddressRange>
getX86RegistrationSEHScopeTableRange(const ExceptionFunction &Function);

/// Reparse an absolute-pointer FuncInfo and require equality with its checked
/// normalized graph. Only these exact native fields carry runtime callback
/// roles; independent references to the same target keep their ordinary role.
std::optional<std::map<va_t, va_t>>
getCheckedX86CxxCallbackPointerSources(const BinaryImage &Img,
                                       const ExceptionFunction &Function);

/// Reparse the same complete FuncInfo graph and describe its exact PE32
/// record extents, including unwind, try, handler and exception-spec maps.
/// Overlapping distinct records are rejected. These ranges describe source
/// storage; they do not claim immutable runtime contents or copy permission.
std::optional<std::vector<ExceptionAddressRange>>
getCheckedX86CxxMetadataRanges(const BinaryImage &Img,
                               const ExceptionFunction &Function);

/// Image-wide runtime pointer roles, reconstructed from the checked FuncInfo
/// and SafeSEH parsers. These sources name callbacks or table storage, not
/// ordinary function entries. RuntimeOnlyPointerTargets have no independent
/// relocated code-pointer source. Exports, stated symbols and ordinary calls
/// retain their separate function-entry roles.
struct X86RegistrationPointerRoles {
  std::map<va_t, va_t> Sources;
  std::set<va_t> RuntimeOnlyPointerTargets;
};
std::optional<X86RegistrationPointerRoles>
getCheckedX86RegistrationPointerRoles(const BinaryImage &Img);

/// The exact load-config source slot and mapped SafeSEH table target. A table
/// in an executable allocation remains data; independent references retain
/// their own roles. Absent or malformed tables supply no such proof.
std::optional<std::pair<va_t, va_t>>
getCheckedX86SafeSEHTablePointer(const BinaryImage &Img);

/// Checked PE32 scalar throw metadata with one simple catchable type, no
/// copy/destructor/forwarding callback and no pointer adjustment. The three
/// metadata records must be immutable mapped data. TypeDescriptor is retained
/// by identity; it does not supply a guessed C++ type or object layout.
struct X86SimpleCxxThrowInfo {
  va_t Address = InvalidVA;
  va_t TypeDescriptorVA = InvalidVA;
  uint32_t Attributes = 0;
  uint32_t ObjectSize = 0;
  std::vector<ExceptionAddressRange> ReadOnlyRanges;
  ExceptionAddressRange TypeDescriptorRange;
};
std::optional<X86SimpleCxxThrowInfo>
getCheckedX86SimpleCxxThrowInfo(const BinaryImage &Img, va_t Address);

/// Authenticate an argument-preserving veneer to the known CRT SEH3 import.
bool isCheckedX86SEH3Personality(const BinaryImage &Img, va_t HandlerVA);

/// Separate the per-function EAX/FuncInfo thunk from its argument- and
/// EAX-preserving CRT dispatch entry. A generated handler must use RuntimeVA;
/// jumping to the original HandlerVA would reinstall the original FuncInfo.
/// Only immutable unwrapped code and the exact VCRUNTIME140 FH3 import qualify.
struct X86CxxPersonalityABI {
  va_t RuntimeVA = InvalidVA;
  va_t IATVA = InvalidVA;
  std::vector<ExceptionAddressRange> CodeRanges;
};
std::optional<X86CxxPersonalityABI>
getCheckedX86CxxPersonalityABI(const BinaryImage &Img,
                               const ExceptionFunction &Function);

/// Authenticate a retained FH3 handler entry directly from its thunk, FuncInfo
/// and CRT import. This supplies a code-address identity after the original
/// parent's prologue has been replaced, not ownership of its former callbacks
/// or permission to reconstruct that parent's exception contract.
bool isCheckedX86CxxHandlerReference(const BinaryImage &Img, va_t HandlerVA);

/// Authenticate the direct CRT EH4 forwarding wrapper and return its cookie
/// checker. Names/byte-search observations alone do not authorize rewriting:
/// all four dispatcher arguments, the exact image cookie and the common CRT
/// import must occupy the checked cdecl slots on the sole forwarding path.
std::optional<va_t> getCheckedX86EH4CookieCheck(const BinaryImage &Img,
                                                va_t HandlerVA);

/// Check the leaf success path: compare ECX with the exact image cookie,
/// branch to the original failure code on mismatch, otherwise return without
/// touching stack storage or other registers. This authorizes no elision for
/// an argument whose equality with that cookie has not separately been proved.
bool hasCheckedX86CookieCheckSuccessPath(const BinaryImage &Img, va_t CheckVA);

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_COFFREGISTRATIONEH_H
