//===- LocalDataReference.h - Source-local data relations -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Identifies an object-local data operand by the typed function and exact
/// operand that independently anchors it. No static spelling escapes its
/// object.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_LOCALDATAREFERENCE_H
#define NEVERD_SIGS_LOCALDATAREFERENCE_H

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::sigs {

namespace local_data_syntax {
#define NEVERD_PATTERN_CHAR(Name, Value) inline constexpr char Name = Value;
#define NEVERD_PATTERN_VALUE(Name, Value)                                      \
  inline constexpr unsigned Name = Value;
#include "neverd/sigs/PatternSyntax.def"
} // namespace local_data_syntax

struct LocalDataAnchor {
  uint32_t Offset;
  llvm::StringRef Name;
  int64_t Delta = 0;
};

/// Decode OFFSET[+/-DELTA]:NAME. Zero deltas use the original OFFSET:NAME
/// spelling; both numeric fields are canonical eight-digit uppercase hex.
inline std::optional<LocalDataAnchor> localDataAnchor(llvm::StringRef Text) {
  constexpr size_t Digits = local_data_syntax::LocalDataAnchorOffsetDigits;
  constexpr size_t DeltaDigits = local_data_syntax::LocalDataDeltaDigits;
  if (Text.size() <= Digits + 1)
    return std::nullopt;
  auto Number = [](llvm::StringRef Hex) -> std::optional<uint32_t> {
    for (char C : Hex)
      if (!((C >= '0' && C <= '9') || (C >= 'A' && C <= 'F')))
        return std::nullopt;
    uint32_t Value;
    if (Hex.getAsInteger(local_data_syntax::Radix, Value))
      return std::nullopt;
    return Value;
  };
  const auto Offset = Number(Text.take_front(Digits));
  if (!Offset)
    return std::nullopt;
  size_t Separator = Digits;
  int64_t Delta = 0;
  if (Text[Digits] == '+' || Text[Digits] == '-') {
    Separator += 1 + DeltaDigits;
    if (Text.size() <= Separator + 1)
      return std::nullopt;
    const auto Magnitude = Number(Text.substr(Digits + 1, DeltaDigits));
    if (!Magnitude || !*Magnitude)
      return std::nullopt;
    Delta = Text[Digits] == '-' ? -int64_t(*Magnitude) : *Magnitude;
  }
  if (Text[Separator] != local_data_syntax::LocalDataAnchorSeparator)
    return std::nullopt;
  const llvm::StringRef Name = Text.drop_front(Separator + 1);
  for (unsigned char C : Name)
    if (C <= ' ' || C == 0x7f)
      return std::nullopt;
  return LocalDataAnchor{*Offset, Name, Delta};
}

} // namespace neverd::sigs

#endif // NEVERD_SIGS_LOCALDATAREFERENCE_H
