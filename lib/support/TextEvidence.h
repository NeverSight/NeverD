//===- TextEvidence.h - Whether decoded characters are text -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The evidence that characters decoded from bytes are text rather than
/// numbers that happen to decode: which characters text holds, the writing
/// systems they belong to, and the rules a candidate string must meet.  The
/// character ranges are rows of StringEncodings.def.  Private to the support
/// library.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SUPPORT_TEXTEVIDENCE_H
#define NEVERD_LIB_SUPPORT_TEXTEVIDENCE_H

#include "neverd/support/ByteScan.h"
#include "neverd/support/Limits.h"
#include "neverd/support/StringScan.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>

namespace neverd::strings::detail {

/// A printable character beyond ASCII: no control, surrogate, private-use
/// or noncharacter code point.
inline bool isPrintableBeyondASCII(uint32_t C) {
  if (C < 0xa0 || C > 0x10ffff)
    return false;
  if ((C >= 0xd800 && C <= 0xdfff) || (C >= 0xe000 && C <= 0xf8ff) ||
      (C >= 0xfdd0 && C <= 0xfdef) || (C & 0xfffe) == 0xfffe || C >= 0xf0000)
    return false;
  return true;
}

/// Which code points a wide string may hold: printable ASCII and the
/// printable characters of the text blocks, a bit each below U+10000.
class WideTextSet {
public:
  WideTextSet();
  bool contains(uint32_t C) const {
    if (C <= 0xffff)
      return Bits[C >> 6] >> (C & 63) & 1;
    return containsSupplementary(C);
  }

private:
  bool containsSupplementary(uint32_t C) const;
  void set(uint32_t C) { Bits[C >> 6] |= uint64_t(1) << (C & 63); }
  uint64_t Bits[0x10000 / 64] = {};
};

const WideTextSet &wideText();

/// A letter or typographic mark a single-byte code page string may hold.
bool isLegacyLetter(uint32_t C);

/// Where a candidate string's characters came from: each source needs its
/// own amount of evidence that they are text.
enum class Source : uint8_t { UTF8, Wide, DoubleByte, SingleByte };

/// Whether \p Codes read as text rather than numbers that decode:
///  - its letters beyond ASCII come from one writing system (ideographs go
///    with Chinese, Japanese or Korean); a word does not mix ASCII letters
///    with them, except accented Latin and, in UTF-8, East Asian text
///    ("DVD光驱"); and a word holding cased letters beyond ASCII is
///    lowercase, Capitalized or uppercase;
///  - no character beyond ASCII makes up half of them, as fill bytes do,
///    and in a longer string beyond ASCII no character fills two in five
///    positions, as a table of pairs does;
///  - its ideographs are common ones, all of them in a short string and
///    nine in ten in a long one (random values rarely are), or half of them
///    in UTF-8, which random bytes rarely decode as at all;
///  - East Asian text has two such characters, and other text three
///    letters, four from a single-byte code page or outside Latin, its
///    letters beyond ASCII inside words and, in a Latin script, at most three
///    in five letters accented.
bool readsAsText(llvm::ArrayRef<uint32_t> Codes, Source From);

/// Whether \p Codes hold enough characters for their kind of text: at least
/// MinTextBeyondASCII when most are beyond ASCII.
bool longEnoughText(llvm::ArrayRef<uint32_t> Codes);

/// More evidence that a single-byte code page nobody chose reads text: two
/// letters beyond ASCII or more, each lowercase or starting its word, and in
/// words of three letters or more.  A stray byte between ASCII strings reads
/// as one such letter, and bytes of a table as capitals.
bool readsAsUnchosenText(llvm::ArrayRef<uint32_t> Codes);

/// Whether every letter of \p Codes beyond ASCII belongs to a writing system
/// the code page \p Page is made for (NEVERD_CODE_PAGE_SCRIPT).
bool inCodePageScripts(llvm::ArrayRef<uint32_t> Codes, Encoding Page);

/// Whether code pages \p A and \p B are made for a writing system in common,
/// as the East Asian pages are for ideographs.
bool shareScripts(Encoding A, Encoding B);

} // namespace neverd::strings::detail

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::strings::detail {
inline constexpr size_t MinTextBeyondASCII =
    ::neverd::limits::support::kTextEvidenceMinTextBeyondASCII;
}

#endif // NEVERD_LIB_SUPPORT_TEXTEVIDENCE_H
