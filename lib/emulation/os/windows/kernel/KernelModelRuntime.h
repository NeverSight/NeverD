//===- KernelModelRuntime.h - Checked guest runtime services --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded memory, string operations and debugger formatting for the Windows
/// model.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELMODELRUNTIME_H
#define NEVERD_EMULATION_KERNELMODELRUNTIME_H

#include "neverd/emulation/GuestMemory.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <string>

namespace neverd::emulation {
class KernelModel;
namespace runtime {
#define NEVERD_KERNEL_RUNTIME_LIMIT(Name, Value)                               \
  inline constexpr unsigned Name = Value;
#include "KernelRuntimeLimits.def"
#undef NEVERD_KERNEL_RUNTIME_LIMIT

llvm::Error checkBufferRange(uint64_t Address, uint64_t Size);
llvm::Error writeBytes(GuestMemory &Memory, uint64_t Address, uint64_t Size,
                       uint8_t Value = 0);
llvm::Expected<uint64_t> ansiCharToUnicode(const KernelModel &Model,
                                           GuestMemory &Memory,
                                           uint64_t PointerCell);
llvm::Expected<uint64_t> writeCountedUnicode(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             uint64_t Destination,
                                             llvm::ArrayRef<uint8_t> Text);
llvm::Expected<uint64_t> integerUnicodeString(const KernelModel &Model,
                                              GuestMemory &Memory,
                                              llvm::ArrayRef<uint64_t> A,
                                              bool Wide);

using ArgumentReader = llvm::function_ref<llvm::Expected<uint64_t>(unsigned)>;
enum class UnicodeOperation { Copy, Compare, Equal, Prefix };
llvm::Expected<uint64_t> appendUnicode(const KernelModel &, GuestMemory &,
                                       llvm::ArrayRef<uint64_t>);
llvm::Expected<uint64_t> unicodeToInteger(const KernelModel &, GuestMemory &,
                                          llvm::ArrayRef<uint64_t>);
llvm::Expected<uint64_t> charToInteger(const KernelModel &, GuestMemory &,
                                       llvm::ArrayRef<uint64_t>);
llvm::Expected<uint64_t> ansiUnicodeSize(const KernelModel &, GuestMemory &,
                                         uint64_t);
llvm::Expected<uint64_t> guidFromUnicode(const KernelModel &, GuestMemory &,
                                         llvm::ArrayRef<uint64_t>);
enum class CrtStringOperation { ConcatWide, FindLastWide, LowerC };
// RtlUpperChar/RtlUpperString are byte operations, not NLS conversions.
constexpr uint8_t upperAnsiByte(uint8_t Byte) {
  return Byte >= 'a' && Byte <= 'z' ? uint8_t(Byte - ('a' - 'A')) : Byte;
}
llvm::Expected<uint64_t> upperAnsiString(const KernelModel &Model,
                                         GuestMemory &Memory,
                                         llvm::ArrayRef<uint64_t> Arguments);
llvm::Expected<uint64_t> compareAnsiStrings(const KernelModel &Model,
                                            GuestMemory &Memory,
                                            llvm::ArrayRef<uint64_t> Arguments);
llvm::Expected<uint64_t> appendAnsiString(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          llvm::ArrayRef<uint64_t> Arguments);
llvm::Expected<uint64_t> crtStringOperation(const KernelModel &Model,
                                            GuestMemory &Memory,
                                            CrtStringOperation Operation,
                                            llvm::ArrayRef<uint64_t> Arguments);

llvm::Expected<uint64_t> unicodeOperation(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          UnicodeOperation Operation,
                                          llvm::ArrayRef<uint64_t> Arguments);
/// RtlInitAnsiString and RtlInitString.
llvm::Expected<uint64_t> initializeAnsiString(const KernelModel &Model,
                                              GuestMemory &Memory,
                                              llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> initializeUnicodeStringEx(const KernelModel &Model,
                                                   GuestMemory &Memory,
                                                   llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> copyAnsiString(const KernelModel &Model,
                                        GuestMemory &Memory,
                                        llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> timeToTimeFields(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> timeFieldsToTime(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> timeToSecondsSince1980(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                llvm::ArrayRef<uint64_t> A);
/// Pinned Unicode upcasing that is independent of the system NLS table.
/// Units at or above U+00C0 require an explicit NLS owner and fail clearly.
llvm::Expected<uint16_t> uppercaseUnicodeUnit(uint16_t Unit);
/// Scalar case exports: ANSI bytes require ASCII; WCHAR/wint_t values use
/// the same NLS-independent mapping as counted Unicode strings.
llvm::Expected<uint64_t> uppercaseCharacter(uint64_t Value, bool Narrow);
llvm::Expected<uint64_t> hashUnicodeString(const KernelModel &Model,
                                           GuestMemory &Memory,
                                           llvm::ArrayRef<uint64_t> A);
/// RtlEqualString when \p EqualityOnly is true, otherwise RtlCompareString.
llvm::Expected<uint64_t> compareAnsiStrings(const KernelModel &Model,
                                            GuestMemory &Memory,
                                            llvm::ArrayRef<uint64_t> A,
                                            bool EqualityOnly);
/// RtlAppendUnicodeStringToString when \p CountedSource is true, otherwise
/// RtlAppendUnicodeToString with a null-terminated source.
llvm::Expected<uint64_t> appendUnicodeString(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             llvm::ArrayRef<uint64_t> A,
                                             bool CountedSource);
llvm::Expected<uint64_t> prefixUnicodeString(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             llvm::ArrayRef<uint64_t> A);
llvm::Expected<uint64_t> integerToUnicodeString(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                llvm::ArrayRef<uint64_t> A,
                                                bool WideValue);
llvm::Expected<uint64_t> unicodeStringToInteger(const KernelModel &Model,
                                                GuestMemory &Memory,
                                                llvm::ArrayRef<uint64_t> A);

/// C runtime string helpers. \p UnitWidth is 1 for the narrow routines and 2
/// for the wide ones. Each reads caller memory one unit at a time; a run with
/// no terminator stops at the 1 MiB model limit.
///
/// strlen and wcslen; strnlen and wcsnlen when \p A carries a count. Returns
/// the unit count (size_t).
llvm::Expected<uint64_t> cStringLength(const KernelModel &Model,
                                       GuestMemory &Memory,
                                       llvm::ArrayRef<uint64_t> A,
                                       unsigned UnitWidth);
/// Return the pinned kernel ctype digit mask, with int/byte carrier narrowing.
uint64_t cIsDigit(uint32_t Value);
/// Locate the raw PE signature through the pinned kernel's DOS-offset checks.
/// This does not validate the optional header, sections or image loadability.
llvm::Expected<uint64_t> imageNtHeader(const KernelModel &Model,
                                       GuestMemory &Memory, uint64_t Base);
/// In-place ASCII case conversion. Other byte/UTF-16 units are unchanged;
/// only converted units are written, with preceding stores retained on faults.
llvm::Expected<uint64_t> cStringChangeCase(const KernelModel &Model,
                                           GuestMemory &Memory,
                                           uint64_t Address, unsigned UnitWidth,
                                           bool Lower);
/// Checked copies / wide append with capacity in units, optional count and
/// SIZE_MAX truncation. Preserve sequential stores and kernel errno results.
llvm::Expected<uint64_t> cStringCheckedWrite(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             llvm::ArrayRef<uint64_t> A,
                                             unsigned UnitWidth, bool Append);

/// strcpy/wcscpy, or strcat/wcscat when Append is true. A third argument
/// selects the counted form. Copy pads to the count; append always terminates.
/// Overlap and invalid storage are rejected before publishing any output.
llvm::Expected<uint64_t> cStringWrite(const KernelModel &Model,
                                      GuestMemory &Memory,
                                      llvm::ArrayRef<uint64_t> A,
                                      unsigned UnitWidth, bool Append);
/// How a comparison reports a mismatch: Normalized is strcmp/strncmp/wcscmp
/// (-1, 0 or 1); Raw is wcsncmp (the raw unit difference); FoldedRaw is the
/// case-insensitive family (ASCII A-Z folded to lowercase, then raw).
enum class CStringCompare { Normalized, Raw, FoldedRaw };
/// strcmp/wcscmp/_stricmp/_wcsicmp; the counted forms when \p A carries a
/// count. Returns an int zero-extended to 32 bits.
llvm::Expected<uint64_t> cStringCompare(const KernelModel &Model,
                                        GuestMemory &Memory,
                                        llvm::ArrayRef<uint64_t> A,
                                        unsigned UnitWidth,
                                        CStringCompare Kind);
/// strchr/wcschr when \p Reverse is false, strrchr/wcsrchr otherwise. The
/// target is truncated to the unit width; the terminator is searched too.
/// Returns the matching guest address or NULL.
llvm::Expected<uint64_t> cStringFindChar(const KernelModel &Model,
                                         GuestMemory &Memory,
                                         llvm::ArrayRef<uint64_t> A,
                                         unsigned UnitWidth, bool Reverse);
/// strstr and wcsstr. An empty needle returns the haystack. Returns the first
/// match's guest address or NULL.
llvm::Expected<uint64_t> cStringFind(const KernelModel &Model,
                                     GuestMemory &Memory,
                                     llvm::ArrayRef<uint64_t> A,
                                     unsigned UnitWidth);
/// strspn/wcsspn when \p Complement is false, wcscspn otherwise. Returns the
/// length (size_t) of the initial run.
llvm::Expected<uint64_t> cStringSpan(const KernelModel &Model,
                                     GuestMemory &Memory,
                                     llvm::ArrayRef<uint64_t> A,
                                     unsigned UnitWidth, bool Complement);
llvm::Expected<std::string> formatDebugMessage(const KernelModel &Model,
                                               GuestMemory &Memory,
                                               uint64_t Format,
                                               unsigned FirstArgument,
                                               ArgumentReader ReadArgument);
/// The same finite formatter with a guest x64 va_list and the kernel's
/// 128/256/384/512-byte prefix attempts. The entire bounded body is checked
/// before publication; truncation does not admit invalid or unsupported tails.
llvm::Expected<std::string> formatPrefixedDebugMessage(const KernelModel &Model,
                                                       GuestMemory &Memory,
                                                       uint64_t Prefix,
                                                       uint64_t Format,
                                                       uint64_t Arguments);
} // namespace runtime
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_KERNELMODELRUNTIME_H
