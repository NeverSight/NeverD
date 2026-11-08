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

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

/// Revalidate every current immutable SEH3/EH4 table word against its decoded
/// scope graph. Pointer fields need exact HIGHLOW storage and their table-owned
/// callback identity; they never become ordinary callable entries. The shared
/// registration work counter bounds storage/fixup rechecks. Frame installation,
/// callback instructions, native admission and final-image replay are separate.
/// Currently requires the table's format-specific outside-try seed.
std::optional<std::vector<uint8_t>> getCheckedX86SEHScopeTableBytes(
    const BinaryImage &Img, const ExceptionFunction &Function, size_t &Work);

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

/// Reconstruct the complete current C++ installation prefix and handler
/// identity. Return its exact absolute handler operand only when the decoded
/// publication and coordinates still match the frame. This pointer role does
/// not grant an ordinary callable ABI or permission to omit runtime guards.
std::optional<std::pair<va_t, va_t>>
getCheckedX86CxxHandlerPointerSource(const BinaryImage &Img,
                                     const ExceptionFunction &Function);

/// Image-wide runtime pointer roles, reconstructed from the checked FuncInfo,
/// registration prefix and SafeSEH parsers. These sources name runtime code or
/// table storage, not ordinary function entries. RuntimeOnlyPointerTargets have
/// no independent relocated code-pointer source. Exports, stated symbols and
/// ordinary calls retain their separate function-entry roles.
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

/// Authenticate a retained PE32 handler entry by exact membership in the
/// immutable SafeSEH table and its relocated load-config pointer. This proves
/// source code identity only, not a language personality, callable ABI,
/// callback ownership or permission to reconstruct native exception handling.
bool isCheckedX86SafeSEHHandlerReference(const BinaryImage &Img,
                                         va_t HandlerVA);

/// Checked PE32 throw metadata with one directly catchable type,
/// no forwarding callback and no pointer adjustment. The three
/// metadata records must be immutable mapped data. TypeDescriptor is retained
/// by identity; it does not supply a guessed C++ type or object layout. A
/// runtime callback address is decoded separately from its ABI and effects.
struct X86IdentityCxxThrowInfo {
  va_t Address = InvalidVA;
  va_t TypeDescriptorVA = InvalidVA;
  va_t DestructorVA = 0;
  va_t CopyConstructorVA = 0;
  uint32_t Attributes = 0;
  uint32_t ObjectSize = 0;
  std::vector<ExceptionAddressRange> ReadOnlyRanges;
  ExceptionAddressRange TypeDescriptorRange;
};
std::optional<X86IdentityCxxThrowInfo>
getCheckedX86IdentityCxxThrowInfo(const BinaryImage &Img, va_t Address);

/// The callback-free projection used by scalar source rendering. Dropping a
/// destructor from a native throw call would change its observable effects.
using X86SimpleCxxThrowInfo = X86IdentityCxxThrowInfo;
std::optional<X86SimpleCxxThrowInfo>
getCheckedX86SimpleCxxThrowInfo(const BinaryImage &Img, va_t Address);

/// Authenticate an argument-preserving veneer to the known CRT SEH3 import.
bool isCheckedX86SEH3Personality(const BinaryImage &Img, va_t HandlerVA);

/// Exact retained SEH dispatch dependencies. Code ranges include forwarding
/// veneers and every decoded scalar-normalizer path. Image reads name current
/// runtime storage, not constant contents. The checker itself and its failure
/// path require separate proofs; these ranges do not grant native admission.
struct X86SEHPersonalityABI {
  va_t IATVA = InvalidVA;
  va_t SecurityCookieVA = 0;
  va_t CookieCheckVA = 0;
  std::vector<ExceptionAddressRange> CodeRanges;
  std::vector<ExceptionAddressRange> ImageReads;
  /// Decoded four-byte address fields and their preferred targets. Missing
  /// base relocations remain possible; retained native use must independently
  /// require complete HIGHLOW coverage for these exact fields.
  std::map<va_t, va_t> AbsoluteOperands;
};
std::optional<X86SEHPersonalityABI>
getCheckedX86SEH3PersonalityABI(const BinaryImage &Img, va_t HandlerVA);
std::optional<X86SEHPersonalityABI>
getCheckedX86EH4PersonalityABI(const BinaryImage &Img, va_t HandlerVA);

/// Relocation-complete retained forwarding and cookie-success code. The eager
/// import identity and each decoded absolute operand are retained separately
/// from instruction bytes. Dynamic normalizers are currently excluded.
/// Cookie equality/failure reachability, shared entry/exit helpers, scope
/// tables, actual caller arguments and final-image preservation remain separate
/// proofs.
struct X86SEHRuntimeCode {
  va_t IATVA = InvalidVA;
  std::string ImportModule;
  std::string ImportName;
  va_t SecurityCookieVA = 0;
  va_t CookieCheckVA = 0;
  va_t CookieFailureVA = 0;
  std::map<va_t, va_t> AbsoluteOperands;
  std::map<va_t, std::vector<uint8_t>> Code;
  bool operator==(const X86SEHRuntimeCode &) const = default;
};
std::optional<X86SEHRuntimeCode>
getCheckedX86SEHRuntimeCode(const BinaryImage &Img,
                            const ExceptionFunction &Function, size_t &Work);

/// Separate the per-function EAX/FuncInfo thunk from its argument- and
/// EAX-preserving CRT dispatch entry. A generated handler must use RuntimeVA;
/// jumping to the original HandlerVA would reinstall the original FuncInfo.
/// Only immutable unwrapped code and an exact known PE32 FH3 import qualify.
struct X86CxxPersonalityABI {
  va_t RuntimeVA = InvalidVA;
  va_t IATVA = InvalidVA;
  std::vector<ExceptionAddressRange> CodeRanges;
};
std::optional<X86CxxPersonalityABI>
getCheckedX86CxxPersonalityABI(const BinaryImage &Img,
                               const ExceptionFunction &Function);

/// One checked handler-prefix comparison. Both displacements are relative to
/// the registration node passed at [ESP+8], not necessarily to source EBP.
/// The handler loads CookieOffset, XORs its value with node+XOROffset and
/// invokes CheckVA. Mismatch branches to FailureVA in the original checker.
struct X86CxxHandlerCookieGuard {
  int32_t CookieOffset = 0;
  int32_t XOROffset = 0;
  va_t CallVA = 0;
  va_t CheckVA = 0;
  va_t FailureVA = 0;
};

/// The dispatch identity and its exact cookie preconditions. CodeRanges
/// includes the immutable thunk, dispatcher veneers and checker success
/// paths. This does not prove that any parent initializes or preserves the
/// cookie cells, that a check succeeds, or that the prefix can be omitted.
struct X86CxxHandlerABI {
  X86CxxPersonalityABI Dispatch;
  va_t SecurityCookieVA = 0;
  std::vector<X86CxxHandlerCookieGuard> CookieGuards;
};
std::optional<X86CxxHandlerABI>
getCheckedX86CxxHandlerABI(const BinaryImage &Img,
                           const ExceptionFunction &Function);

/// A live stack word written by the shared EAX-handler C++ prologue. Offsets
/// use its established EBP, which is four bytes below caller entry ESP.
struct X86CxxPrologueWrite {
  enum class Value {
    PreviousEBP,
    Handler,
    PreviousChain,
    EBX,
    ESI,
    EDI,
    EncodedCookie,
    StackPointer,
    InitialState
  };
  int32_t FrameOffset = 0;
  Value Source = Value::PreviousEBP;
  bool operator==(const X86CxxPrologueWrite &) const = default;
};

/// Exact immutable caller/helper instructions, including the moved return PC.
/// Writes retain machine order, including any overlap. On return EBP is the
/// frame origin, EAX is frame-12, ESP is PostCallStackOffset, and the other
/// five GPRs are unchanged. Arithmetic flags come from cookie XOR EBP, not from
/// the incoming flags. The transient return-PC word below final ESP is not
/// live.
///
/// This describes a physical call, not an ordinary returning callee ABI.
/// Applying it still requires the current caller's stack word, EAX, instruction
/// boundary and entry-frame identities. It does not authorize native EH,
/// cookie-check elision, callback entry, or a frame coordinate by itself.
struct X86CxxPrologueCall {
  static constexpr int32_t RegistrationOffset = -12;
  static constexpr int32_t TryLevelOffset = -4;
  ExceptionAddressRange Caller;
  ExceptionAddressRange Helper;
  va_t CallVA = 0;
  va_t HandlerVA = 0;
  va_t HandlerPointerVA = 0;
  va_t SecurityCookieVA = 0;
  uint32_t AllocationBytes = 0;
  int32_t PostCallStackOffset = 0;
  std::vector<X86CxxPrologueWrite> Writes;
};
/// Authenticate only the immutable physical entry/helper protocol, charging
/// the supplied cumulative work account. No FuncInfo, personality, SafeSEH,
/// source occurrence or native EH contract is granted by this query.
std::optional<X86CxxPrologueCall>
getCheckedX86CxxPhysicalPrologueCall(const BinaryImage &Img, va_t Entry,
                                     size_t &Work, size_t Limit);

std::optional<X86CxxPrologueCall>
getCheckedX86CxxPrologueCall(const BinaryImage &Img,
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
struct X86CookieCheckSuccessPath {
  va_t SecurityCookieVA = 0;
  va_t FailureVA = 0;
  ExceptionAddressRange CodeRange;
  /// The decoded CMP operand; this record alone does not prove relocation.
  std::map<va_t, va_t> AbsoluteOperands;
};
std::optional<X86CookieCheckSuccessPath>
getCheckedX86CookieCheckSuccessPath(const BinaryImage &Img, va_t CheckVA);
bool hasCheckedX86CookieCheckSuccessPath(const BinaryImage &Img, va_t CheckVA);

/// The instructions ordinary flow runs from \p Function's entry and each
/// runtime entry its table names, in the code it owns: what is no function of
/// its own, whatever its bytes look like, as a `push ecx` that lands on a
/// 16-byte boundary in the middle of a `__finally`.
std::set<va_t>
getX86RegistrationBodyInstructions(const BinaryImage &Image,
                                   const ExceptionFunction &Function);

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_COFFREGISTRATIONEH_H
