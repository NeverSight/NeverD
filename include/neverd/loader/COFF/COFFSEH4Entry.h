//===- COFFSEH4Entry.h - Physical shared SEH4 calls ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate shared SEH4 frame installation and restoration instructions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFSEH4ENTRY_H
#define NEVERD_LOADER_COFF_COFFSEH4ENTRY_H

#include "neverd/loader/ExceptionInfo.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;
namespace coff_loader {

struct X86SEH4PrologueWrite {
  enum class Value {
    ReturnPC,
    Handler,
    PreviousChain,
    PreviousEBP,
    EBX,
    ESI,
    EDI,
    EncodedTable,
    EncodedCookie,
    StackPointer,
    InitialState
  };
  int32_t FrameOffset;
  Value Source;
  bool operator==(const X86SEH4PrologueWrite &) const = default;
};

/// The caller pushes its allocation and table before the near call. The
/// helper establishes EBP at the allocation word, returns with EAX = EBP-16
/// and ESP = PostCallStackOffset, and preserves the other five GPRs. Writes
/// retain their physical order, including transient PC storage and overlaps.
/// This is not an ordinary callee ABI or a native reconstruction permission.
struct X86SEH4PrologueCall {
  static constexpr int32_t RegistrationOffset = -16;
  static constexpr int32_t TryLevelOffset = -4;
  ExceptionAddressRange Caller;
  ExceptionAddressRange Helper;
  va_t CallVA;
  va_t HandlerVA;
  va_t ScopeTableVA;
  va_t SecurityCookieVA;
  uint32_t AllocationBytes;
  int32_t PostCallStackOffset;
  std::vector<X86SEH4PrologueWrite> Writes;
  /// Decoded table, handler and cookie address fields in Caller and Helper.
  /// Retained native use separately requires complete HIGHLOW coverage.
  std::map<va_t, va_t> AbsoluteOperands;
};

struct X86SEH4EpilogueCall {
  ExceptionAddressRange Caller;
  ExceptionAddressRange Helper;
};

std::optional<X86SEH4PrologueCall>
getCheckedX86SEH4PrologueCall(const BinaryImage &Image,
                              const ExceptionFunction &Function);
std::optional<X86SEH4EpilogueCall>
getCheckedX86SEH4EpilogueCall(const BinaryImage &Image,
                              const ExceptionFunction &Function, va_t CallVA);

} // namespace coff_loader
} // namespace neverd

#endif // NEVERD_LOADER_COFF_COFFSEH4ENTRY_H
