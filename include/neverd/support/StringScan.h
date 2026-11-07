//===- StringScan.h - text strings in binary data ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finds terminated text strings in raw bytes: ASCII and UTF-8 C strings,
/// UTF-16 and UTF-32 strings in either byte order, and C strings in legacy
/// code pages.  Every string ends in a zero code unit and holds only
/// printable characters; StringEncodings.def states which characters each
/// kind may hold.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_STRINGSCAN_H
#define NEVERD_SUPPORT_STRINGSCAN_H

#include "neverd/support/Limits.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace neverd::strings {

enum class Encoding : uint8_t {
#define NEVERD_STRING_ENCODING(Id, Name, Spelling, UnitBytes, BigEndian) Id,
#define NEVERD_LEGACY_ENCODING(Id, Name, Spelling, Decoder, Table) Id,
#include "neverd/support/StringEncodings.def"
};

/// Whether \p E is an 8-bit code page rather than a Unicode encoding.
bool isLegacyEncoding(Encoding E);

/// The option and JSON name of \p E ("utf-16le"); empty past the last
/// encoding.
llvm::StringRef encodingName(Encoding E);
/// Bytes in one code unit of \p E.
unsigned encodingUnitBytes(Encoding E);
/// How a listing names \p E (`text "UTF-16LE", ...`); empty for ASCII.
llvm::StringRef encodingSpelling(Encoding E);
std::optional<Encoding> encodingNamed(llvm::StringRef Name);

/// A set of encodings, one bit per Encoding.
using EncodingSet = uint32_t;
constexpr EncodingSet encodingBit(Encoding E) {
  return EncodingSet(1) << static_cast<unsigned>(E);
}

/// The encodings a search uses unless its options name others
/// (StringEncodings.def).
constexpr EncodingSet defaultEncodings() {
  EncodingSet Set = 0;
#define NEVERD_DEFAULT_ENCODING(Id) Set |= encodingBit(Encoding::Id);
#include "neverd/support/StringEncodings.def"
  return Set;
}

struct ScanOptions {
  /// The encodings searched.  C strings that are not UTF-8 are read in each
  /// legacy code page among them.
  EncodingSet Encodings = defaultEncodings();
  /// The code page that reads a C string first, which wins where other code
  /// pages read it as text too; it is searched whether or not Encodings
  /// names it.  Otherwise the code pages read in StringEncodings.def order.
  std::optional<Encoding> Preferred;
  /// Display columns a string needs, its terminator not counted: a wide East
  /// Asian character counts two (displayColumns), so that three ideographs
  /// pass where three ASCII letters do not.
  unsigned MinLength = ::neverd::limits::support::kDefaultStringMinLength;
};

struct FoundString {
  /// Offset into the scanned bytes and length in bytes, without the
  /// terminator.
  uint64_t Offset = 0, Bytes = 0;
  unsigned Chars = 0;
  Encoding Kind = Encoding::ASCII;
  /// The characters as UTF-8.
  std::string Text;
};

/// One character \p Data decodes to.  Bytes that do not decode in the
/// encoding are characters of one byte with no code point.
struct DecodedCharacter {
  uint64_t Offset = 0;
  unsigned Bytes = 1;
  /// The code points; a Big5 pair decodes to two, and an undecodable byte to
  /// none.
  uint32_t Code[2] = {0, 0};
  unsigned Codes = 0;
};

/// Decodes \p Data in \p E from its first byte, reporting each character in
/// offset order; wide encodings decode whole units only.
void decode(llvm::ArrayRef<uint8_t> Data, Encoding E,
            llvm::function_ref<void(const DecodedCharacter &)> Found);

/// Whether a listing shows \p Code as itself: not a control, a formatting
/// or direction mark, a byte order mark or a private-use character.
bool isShownCharacter(uint32_t Code);

/// \p Text with every character case-folded by Unicode simple case folding,
/// so that text compares regardless of case in any script: "ПРИВЕТ" and
/// "привет" fold alike.  Bytes that are not UTF-8 stay as they are.
std::string foldCase(llvm::StringRef Text);

/// The columns a fixed-width display draws \p Code in: two for East Asian
/// wide and fullwidth characters (WideCharacters.def), else one.
unsigned displayColumns(uint32_t Code);

/// Where reading a string from one of its bytes begins: at the first
/// character that starts at or after that byte.
struct TextStart {
  /// The character's offset in the string's bytes, and its text's offset in
  /// the string's UTF-8 text.
  uint64_t Byte = 0, UTF8 = 0;
  /// Code points from that character to the end of the string, and the
  /// display columns they take.
  unsigned Chars = 0, Columns = 0;
};

/// Where the text of the string \p Data, in \p E and without its terminator,
/// begins when read from byte \p Offset; none when no character starts at
/// or after it.
std::optional<TextStart> textFrom(llvm::ArrayRef<uint8_t> Data, Encoding E,
                                  uint64_t Offset);

/// Runs \p Task for each index below \p Count, in any order, perhaps several
/// at once.
using ParallelTasks = llvm::function_ref<void(
    size_t Count, llvm::function_ref<void(size_t Index)> Task)>;

/// Reports the strings in \p Data in offset order, none overlapping another.
/// A wide string starts at a multiple of its code unit size; where strings
/// of different encodings overlap, the one starting first wins.  Given
/// \p Parallel, a scan of at least limits::support::kStringScanParallelBytes
/// reads spans of its data as tasks of it; it reports the same strings.
void scan(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
          llvm::function_ref<void(FoundString &&)> Found,
          ParallelTasks Parallel = {});

/// The string scan(\p Data, \p Options) reports first, where it starts at
/// \p Data's first byte; none otherwise.  Reads only what the strings that
/// start there span.
std::optional<FoundString> scanAt(llvm::ArrayRef<uint8_t> Data,
                                  const ScanOptions &Options);

} // namespace neverd::strings

#endif // NEVERD_SUPPORT_STRINGSCAN_H
