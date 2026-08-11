//===- ExceptionCommon.h - Shared exception primitives --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Primitives shared by every exception model: the completeness state of a
/// decoded record and the checked address range every decoder reports in.
/// Kept separate from the model headers so a format-specific model can depend
/// on them without depending on the other formats' models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONCOMMON_H
#define NEVERD_LOADER_EXCEPTIONCOMMON_H

#include "neverd/Common.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// Quality of a decoded record.  Partial records remain useful for analysis,
/// while Malformed records must never be used to regenerate native metadata.
enum class ExceptionParseStatus : uint8_t {
  Complete,
  Partial,
  Malformed,
};

inline const char *getExceptionParseStatusName(ExceptionParseStatus Status) {
  switch (Status) {
  case ExceptionParseStatus::Complete:
    return "complete";
  case ExceptionParseStatus::Partial:
    return "partial";
  case ExceptionParseStatus::Malformed:
    return "malformed";
  }
  return "unknown";
}

inline ExceptionParseStatus mergeExceptionParseStatus(ExceptionParseStatus A,
                                                      ExceptionParseStatus B) {
  return static_cast<ExceptionParseStatus>(
      std::max(static_cast<unsigned>(A), static_cast<unsigned>(B)));
}

/// Checked half-open virtual-address range [Begin, End).
struct ExceptionAddressRange {
  va_t Begin = 0;
  va_t End = 0;

  static std::optional<ExceptionAddressRange> fromStartAndSize(va_t Start,
                                                               uint64_t Size) {
    if (Size == 0 || Size > std::numeric_limits<va_t>::max() - Start)
      return std::nullopt;
    return ExceptionAddressRange{Start, Start + Size};
  }

  bool isValid() const { return Begin < End; }
  uint64_t size() const { return isValid() ? End - Begin : 0; }
  bool contains(va_t Address) const {
    return isValid() && Address >= Begin && Address < End;
  }
  bool contains(const ExceptionAddressRange &Other) const {
    return isValid() && Other.isValid() && Other.Begin >= Begin &&
           Other.End <= End;
  }
  bool overlaps(const ExceptionAddressRange &Other) const {
    return isValid() && Other.isValid() && Begin < Other.End &&
           Other.Begin < End;
  }
};

/// The language runtime an image's exception machinery belongs to.
///
/// Declared beside the other shared primitives rather than in
/// `LanguageRuntime.h` so that \ref ExceptionInfo can carry the identity it
/// was classified with.  Two languages can share one table schema — Rust and
/// C++ both use the Itanium LSDA, Delphi reuses the Windows registration
/// chain — so the schema a record was decoded from does not determine how its
/// landing pads behave, and the identity has to travel with the tables.
enum class SourceLanguageRuntime : uint8_t {
  Unknown,
  /// C compiled with `-fexceptions`: cleanup-only Itanium tables.
  C,
  /// C++ with the Itanium ABI (libstdc++/libc++).
  CxxItanium,
  /// C++ with the Microsoft ABI.
  CxxMSVC,
  Rust,
  Go,
  Delphi,
  ObjectiveC,
  Swift,
};

const char *getSourceLanguageRuntimeName(SourceLanguageRuntime Runtime);

/// What an image's evidence proved about its runtime.
struct LanguageRuntimeInfo {
  SourceLanguageRuntime Runtime = SourceLanguageRuntime::Unknown;
  /// Runtime version string when the image published one, such as the Go
  /// build version or the Rust compiler release recorded in a panic message.
  std::string Version;
  /// Human-readable evidence, in the order it was found.  Recorded so a
  /// surprising classification can be audited instead of trusted.
  std::vector<std::string> Evidence;
  /// True when more than one runtime left evidence, which a mixed-language
  /// image legitimately does (cgo, a Rust staticlib inside a C++ program).
  bool IsMixed = false;
  std::vector<SourceLanguageRuntime> SecondaryRuntimes;

  bool is(SourceLanguageRuntime R) const {
    if (Runtime == R)
      return true;
    for (SourceLanguageRuntime S : SecondaryRuntimes)
      if (S == R)
        return true;
    return false;
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONCOMMON_H
