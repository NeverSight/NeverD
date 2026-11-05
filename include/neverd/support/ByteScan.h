//===- ByteScan.h - Bounded text byte scans ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared ASCII admission and optional bulk UTF-8 validation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_BYTESCAN_H
#define NEVERD_SUPPORT_BYTESCAN_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace neverd {

/// True only when the optional bulk validator proves the complete span valid.
/// False requires the caller's ordinary validation/repair path, including when
/// acceleration is disabled. Never reads padding outside Text.
bool knownValidUTF8(std::string_view Text);

namespace strings::detail {
inline bool isTextASCII(uint32_t C) {
  return (C >= 0x20 && C < 0x7f) || C == '\t' || C == '\n' || C == '\r';
}

size_t longTextASCIIPrefix(const uint8_t *Data, size_t Size);

/// Whether a byte is NUL or a control byte that no text holds: a C string
/// cannot hold it, and no code page reads it as text, since a byte below
/// 0x80 is ASCII in every code page or the trail of a double-byte character,
/// at 0x30 or above.
inline bool isNulOrControl(uint8_t B) { return B < 0x80 && !isTextASCII(B); }
/// Offset of the first NUL or control byte in Data (isNulOrControl), or Size.
size_t findNulOrControl(const uint8_t *Data, size_t Size);
/// Whether a wide code unit (UTF-16 or UTF-32) of value \p C holds no text:
/// a control below 0xA0, or past the last code point.
inline bool isNonTextUnit(uint32_t C) {
  return C < 0xa0 ? !isTextASCII(C) : C > 0x10ffff;
}
/// Marks the \p Count bytes at \p Data, bit I of word W for byte 64 W + I:
/// in \p Nul the NUL bytes, in \p Run the bytes a run holds, which with
/// \p Text are ASCII text (isTextASCII) and else any but NUL or control
/// bytes (isNulOrControl).  Each array holds (Count + 63) / 64 words; the
/// bits past Count are clear.
void byteRunMasks(const uint8_t *Data, size_t Count, bool Text, uint64_t *Nul,
                  uint64_t *Run);
/// Marks the \p Count code units of \p UnitBytes (2 or 4) at \p Data, in
/// the byte order \p BigEndian, bit I of word W for unit 64 W + I: in
/// \p Zero the zero units, in \p Run the units that may hold text (not
/// isNonTextUnit).  Each array holds (Count + 63) / 64 words; the bits past
/// Count are clear.
void unitRunMasks(const uint8_t *Data, size_t Count, unsigned UnitBytes,
                  bool BigEndian, uint64_t *Zero, uint64_t *Run);

/// Length of the initial ASCII text run, reading at most Size bytes.
inline size_t textASCIIPrefix(const uint8_t *Data, size_t Size) {
  // Most printable runs in binary images are tiny. Keep them inline and only
  // dispatch after proving a useful prefix, independent of the backend.
  const size_t Probe = Size < 16 ? Size : 16;
  size_t I = 0;
  for (; I < Probe; ++I)
    if (!isTextASCII(Data[I]))
      return I;
  return I == Size ? I : I + longTextASCIIPrefix(Data + I, Size - I);
}
} // namespace strings::detail
} // namespace neverd

#endif
