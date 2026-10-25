//===- PatternParser.cpp - FLIRT .pat text format parser -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/PatternParser.h"

#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

using namespace neverd::sigs;

namespace {

/// The fields every line opens with, by their place in it.
enum OpeningField : size_t {
#define NEVERD_PATTERN_FIELD(Name) Name##Field,
#include "neverd/sigs/PatternSyntax.def"
  OpeningFields
};

#define NEVERD_PATTERN_STRING(Name, Value)                                     \
  [[maybe_unused]] constexpr llvm::StringLiteral Name(Value);
#define NEVERD_PATTERN_CHAR(Name, Value)                                       \
  [[maybe_unused]] constexpr char Name = Value;
#define NEVERD_PATTERN_VALUE(Name, Value)                                      \
  [[maybe_unused]] constexpr unsigned Name = Value;
#include "neverd/sigs/PatternSyntax.def"

/// A byte is stated in as many characters as the unstated byte has: two hex
/// digits, the high one first.
constexpr size_t ByteChars = UnstatedByte.size();
constexpr unsigned DigitBits = llvm::ConstantLog2<Radix>();
static_assert(llvm::has_single_bit(Radix) && ByteChars * DigitBits == CHAR_BIT,
              "the digits of a byte state all its bits");

/// The stated bits of a module are gathered in words of this many.
constexpr size_t WordBits = std::numeric_limits<uint64_t>::digits;

/// One entry for each value of a character.
constexpr size_t CharValues = size_t{1} << CHAR_BIT;

/// A run of hex digits, and the value of its first.
struct HexDigitRun {
  uint8_t First;
  uint8_t Last;
  uint8_t FirstValue;
};

constexpr HexDigitRun HexDigitRuns[] = {
#define NEVERD_PATTERN_HEX_DIGITS(First, Last, FirstValue)                     \
  {First, Last, FirstValue},
#include "neverd/sigs/PatternSyntax.def"
};

/// What HexDigits holds for a character that is no hex digit.
constexpr uint8_t NotHexDigit = std::numeric_limits<uint8_t>::max();

/// Each character's value as a hex digit, or NotHexDigit.
constexpr std::array<uint8_t, CharValues> makeHexDigits() {
  std::array<uint8_t, CharValues> Digits{};
  for (uint8_t &Digit : Digits)
    Digit = NotHexDigit;
  for (const HexDigitRun &Run : HexDigitRuns)
    for (unsigned C = Run.First; C <= Run.Last; ++C)
      Digits[C] = static_cast<uint8_t>(C - Run.First + Run.FirstValue);
  return Digits;
}

constexpr auto HexDigits = makeHexDigits();

// The decoders below find a digit's value by arithmetic: its distance from
// the first decimal digit, or, once the case bit is set, from the first
// lower case letter.
constexpr HexDigitRun DecimalDigits = HexDigitRuns[0];
constexpr HexDigitRun LowerCaseDigits = HexDigitRuns[1];
constexpr HexDigitRun UpperCaseDigits = HexDigitRuns[2];
constexpr uint8_t DecimalSpan = DecimalDigits.Last - DecimalDigits.First;
constexpr uint8_t LetterSpan = LowerCaseDigits.Last - LowerCaseDigits.First;
constexpr uint8_t LetterValue = LowerCaseDigits.FirstValue;
constexpr uint8_t CaseBit = LowerCaseDigits.First ^ UpperCaseDigits.First;

/// Whether that arithmetic gives every character the value HexDigits does.
constexpr bool decodersReadHexDigits() {
  for (unsigned C = 0; C < CharValues; ++C) {
    const uint8_t Digit = static_cast<uint8_t>(C - DecimalDigits.First);
    const uint8_t Letter =
        static_cast<uint8_t>((C | CaseBit) - LowerCaseDigits.First);
    const uint8_t Value = Digit <= DecimalSpan   ? Digit
                          : Letter <= LetterSpan ? Letter + LetterValue
                                                 : NotHexDigit;
    if (Value != HexDigits[C])
      return false;
  }
  return true;
}
static_assert(decodersReadHexDigits(),
              "the decoders read the hex digits PatternSyntax.def lists");

/// The characters that separate fields.
constexpr std::array<bool, CharValues> makeSeparators() {
  std::array<bool, CharValues> Separators{};
#define NEVERD_PATTERN_SEPARATOR(Value)                                        \
  Separators[static_cast<uint8_t>(Value)] = true;
#include "neverd/sigs/PatternSyntax.def"
  return Separators;
}

constexpr auto Separators = makeSeparators();

/// One more than the greatest separator.
constexpr unsigned separatorBound() {
  unsigned Bound = 0;
  for (unsigned C = 0; C < CharValues; ++C)
    if (Separators[C])
      Bound = C + 1;
  return Bound;
}

constexpr unsigned SeparatorBound = separatorBound();

/// The first separator in [\p Position, \p End), or \p End.
const char *findSeparator(const char *Position, const char *End) {
  // Every separator is below SeparatorBound and nothing in a hex field is, so
  // a field is passed over a word at a time until a word holds such a byte.
  // The test finds any byte below the bound, and perhaps bytes after it.
  constexpr uint64_t Ones = 0x0101010101010101ULL;
  static_assert(SeparatorBound <= 0x80, "the test sees only the low bits");
  while (End - Position >= static_cast<ptrdiff_t>(sizeof(uint64_t))) {
    uint64_t Word;
    std::memcpy(&Word, Position, sizeof(Word));
    if ((Word - Ones * SeparatorBound) & ~Word & (Ones * 0x80))
      break;
    Position += sizeof(Word);
  }
  while (Position != End && !Separators[static_cast<uint8_t>(*Position)])
    ++Position;
  return Position;
}

/// The whitespace-separated fields of \p Line, as llvm::SplitString gives
/// them.
void splitFields(llvm::StringRef Line,
                 llvm::SmallVectorImpl<llvm::StringRef> &Fields) {
  const char *Position = Line.begin();
  const char *const End = Line.end();
  while (true) {
    while (Position != End && Separators[static_cast<uint8_t>(*Position)])
      ++Position;
    if (Position == End)
      return;
    const char *const Start = Position;
    Position = findSeparator(Position, End);
    Fields.emplace_back(Start, static_cast<size_t>(Position - Start));
  }
}

bool isIgnorablePatternLine(llvm::StringRef Line) {
  Line = Line.trim();
  return Line.empty() || Line.starts_with(CommentPrefix) ||
         Line.starts_with(AlternateCommentPrefix) || Line == EndMarker;
}

llvm::Error patternError(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>(Message,
                                             llvm::inconvertibleErrorCode());
}

/// A module being written into its chunk: offsets into the chunk's bytes
/// and names, which may still move while more lines are parsed into them.
struct DraftModule {
  size_t Bytes = 0;
  size_t Names = 0;
  uint32_t LeadingCount = 0;
  uint32_t TailCount = 0;
  uint32_t TotalLen = 0;
  uint32_t PublicNameCount = 0;
  uint32_t ReferenceCount = 0;
  uint16_t CRC16 = 0;
  uint8_t CRCLen = 0;
  bool RequireReferences = false;
  bool RequireCaller = false;
};

/// Where the modules of a chunk are written: each module's bytes, then its
/// stated bits, one after another in the room reserved for the chunk.
struct ChunkWriter {
  uint8_t *Bytes = nullptr;
  size_t Capacity = 0;
  size_t Used = 0;
  /// The stated bits of the module being parsed, bit I % WordBits of word
  /// I / WordBits.
  std::vector<uint64_t> Bits;
  PatternNames &Names;
};

/// The room the modules of \p TextSize characters of lines can take; see
/// PatternParser::reserveBytes.
size_t byteCapacity(size_t TextSize) { return TextSize - TextSize / 4 + 1; }

llvm::Error noRoom() {
  return patternError("the module does not fit the room reserved for it");
}

/// Set \p Width bits of \p Mask in \p Bits from bit \p First on.
void orBits(uint64_t *Bits, size_t First, uint64_t Mask, unsigned Width) {
  Bits[First / WordBits] |= Mask << (First % WordBits);
  if (First % WordBits + Width > WordBits)
    Bits[First / WordBits + 1] |= Mask >> (WordBits - First % WordBits);
}

/// Decode \p Pairs character pairs of \p Text into \p Bytes, setting bit
/// \p FirstBit + I of \p Bits for each pair I that states a byte.  A pair of
/// hex digits states a byte and the unstated byte leaves it zero; anything
/// else makes the whole call fail.
bool decodePairs(const char *Text, size_t Pairs, uint8_t *Bytes, uint64_t *Bits,
                 size_t FirstBit) {
  // A byte's characters are read as a pair: two digits, or the unstated
  // byte, which is one character twice.
  constexpr char UnstatedChar = UnstatedByte.data()[0];
  static_assert(ByteChars == 2 && UnstatedByte.data()[1] == UnstatedChar,
                "the decoders read a byte's characters as a pair");
  size_t I = 0;
#if defined(__SSE2__)
  // Eight pairs at a time: classify all sixteen characters, then fold each
  // pair's high and low characters within its 16-bit lane.
  constexpr size_t VectorPairs = sizeof(__m128i) / ByteChars;
  const __m128i FirstDigit = _mm_set1_epi8(DecimalDigits.First);
  const __m128i FirstLetter = _mm_set1_epi8(LowerCaseDigits.First);
  const __m128i Decimals = _mm_set1_epi8(DecimalSpan);
  const __m128i Letters = _mm_set1_epi8(LetterSpan);
  const __m128i Case = _mm_set1_epi8(CaseBit);
  const __m128i FirstLetterValue = _mm_set1_epi8(LetterValue);
  const __m128i UnstatedChars = _mm_set1_epi8(UnstatedChar);
  const __m128i LowByte = _mm_set1_epi16(std::numeric_limits<uint8_t>::max());
  __m128i Invalid = _mm_setzero_si128();
  for (; I + VectorPairs <= Pairs; I += VectorPairs) {
    const __m128i Chars = _mm_loadu_si128(
        reinterpret_cast<const __m128i *>(Text + ByteChars * I));
    const __m128i Digit = _mm_sub_epi8(Chars, FirstDigit);
    const __m128i IsDigit =
        _mm_cmpeq_epi8(_mm_min_epu8(Digit, Decimals), Digit);
    const __m128i Letter = _mm_sub_epi8(_mm_or_si128(Chars, Case), FirstLetter);
    const __m128i IsLetter =
        _mm_cmpeq_epi8(_mm_min_epu8(Letter, Letters), Letter);
    const __m128i IsHex = _mm_or_si128(IsDigit, IsLetter);
    const __m128i Nibble = _mm_or_si128(
        _mm_and_si128(IsDigit, Digit),
        _mm_andnot_si128(IsDigit, _mm_add_epi8(Letter, FirstLetterValue)));
    const __m128i IsUnstatedChar = _mm_cmpeq_epi8(Chars, UnstatedChars);
    const __m128i Stated = _mm_and_si128(
        _mm_and_si128(IsHex, _mm_srli_epi16(IsHex, CHAR_BIT)), LowByte);
    const __m128i Unstated = _mm_and_si128(
        _mm_and_si128(IsUnstatedChar, _mm_srli_epi16(IsUnstatedChar, CHAR_BIT)),
        LowByte);
    Invalid = _mm_or_si128(
        Invalid, _mm_andnot_si128(_mm_or_si128(Stated, Unstated), LowByte));
    const __m128i Value =
        _mm_and_si128(_mm_or_si128(_mm_slli_epi16(Nibble, DigitBits),
                                   _mm_srli_epi16(Nibble, CHAR_BIT)),
                      Stated);
    _mm_storel_epi64(reinterpret_cast<__m128i *>(Bytes + I),
                     _mm_packus_epi16(Value, Value));
    // The bits of the lanes' low bytes, gathered into one bit per pair.
    unsigned Mask = static_cast<unsigned>(_mm_movemask_epi8(Stated)) & 0x5555;
    Mask = (Mask | (Mask >> 1)) & 0x3333;
    Mask = (Mask | (Mask >> 2)) & 0x0F0F;
    Mask = (Mask | (Mask >> 4)) & 0x00FF;
    orBits(Bits, FirstBit + I, Mask, VectorPairs);
  }
  if (_mm_movemask_epi8(Invalid))
    return false;
#endif
  // The rest, and everything where there is no SSE2: the same rules a pair at
  // a time, without a branch on the data.
  bool Bad = false;
  while (I < Pairs) {
    const unsigned Width =
        static_cast<unsigned>(std::min<size_t>(WordBits, Pairs - I));
    uint64_t Mask = 0;
    for (unsigned K = 0; K < Width; ++K) {
      const uint8_t High = static_cast<uint8_t>(Text[ByteChars * (I + K)]);
      const uint8_t Low = static_cast<uint8_t>(Text[ByteChars * (I + K) + 1]);
      const uint8_t HighDigit = High - DecimalDigits.First;
      const uint8_t LowDigit = Low - DecimalDigits.First;
      const uint8_t HighLetter = (High | CaseBit) - LowerCaseDigits.First;
      const uint8_t LowLetter = (Low | CaseBit) - LowerCaseDigits.First;
      const bool HighIsDigit = HighDigit <= DecimalSpan;
      const bool LowIsDigit = LowDigit <= DecimalSpan;
      const bool Stated = (HighIsDigit || HighLetter <= LetterSpan) &&
                          (LowIsDigit || LowLetter <= LetterSpan);
      const uint8_t Value = static_cast<uint8_t>(
          ((HighIsDigit ? HighDigit : HighLetter + LetterValue) << DigitBits) |
          (LowIsDigit ? LowDigit : LowLetter + LetterValue));
      Bad |= !Stated && !(High == UnstatedChar && Low == UnstatedChar);
      Bytes[I + K] = Stated ? Value : 0;
      Mask |= uint64_t(Stated) << K;
    }
    orBits(Bits, FirstBit + I, Mask, Width);
    I += Width;
  }
  return !Bad;
}

/// Decode the hex pattern \p Pat as bytes \p Count onward of the module whose
/// bytes start at \p First in \p Writer's room.
llvm::Error appendPattern(llvm::StringRef Pat, ChunkWriter &Writer,
                          size_t First, uint32_t &Count) {
  if (Pat.size() % ByteChars != 0)
    return patternError("hex pattern has odd length");

  const size_t Length = Pat.size() / ByteChars;
  if (Length > Writer.Capacity - First - Count)
    return noRoom();
  Writer.Bits.resize(llvm::divideCeil(Count + Length, WordBits));
  if (!decodePairs(Pat.data(), Length, Writer.Bytes + First + Count,
                   Writer.Bits.data(), Count)) {
    // Name the first pair that is neither two hex digits nor the unstated
    // byte.
    for (size_t I = 0; I < Length; ++I) {
      const llvm::StringRef Pair = Pat.substr(ByteChars * I, ByteChars);
      const bool Stated = llvm::all_of(Pair, [](char C) {
        return HexDigits[static_cast<uint8_t>(C)] != NotHexDigit;
      });
      if (!Stated && Pair != UnstatedByte)
        return patternError("invalid hex byte: " + Pair);
    }
  }
  Count += static_cast<uint32_t>(Length);
  return llvm::Error::success();
}

/// Parse the pattern line \p Line into \p Writer.  The names the module adds
/// refer to \p Line's text.  On failure the writer may hold part of the line,
/// and the caller discards it.
llvm::Expected<DraftModule> parsePatternLine(llvm::StringRef Line,
                                             ChunkWriter &Writer) {
  Line = Line.trim();
  DraftModule Mod;
  Mod.Bytes = Writer.Used;
  Mod.Names = Writer.Names.Names.size();
  Writer.Bits.clear();

  // Split into tokens by whitespace.
  llvm::SmallVector<llvm::StringRef, 16> Tokens;
  splitFields(Line, Tokens);

  if (Tokens.size() < OpeningFields)
    return patternError("too few fields in pattern line");

  // The leading bytes, some perhaps unstated.
  if (llvm::Error Error = appendPattern(Tokens[LeadingBytesField], Writer,
                                        Mod.Bytes, Mod.LeadingCount))
    return std::move(Error);

  // The CRC length (a hex byte).
  unsigned CRCLen;
  if (Tokens[CRCLengthField].getAsInteger(Radix, CRCLen) ||
      CRCLen > MaxCRCLength)
    return patternError("invalid CRC length: " + Tokens[CRCLengthField]);
  Mod.CRCLen = static_cast<uint8_t>(CRCLen);

  // The CRC16 value (hex).
  unsigned CRC16Val;
  if (Tokens[CRC16Field].getAsInteger(Radix, CRC16Val) || CRC16Val > MaxCRC16)
    return patternError("invalid CRC16: " + Tokens[CRC16Field]);
  Mod.CRC16 = static_cast<uint16_t>(CRC16Val);

  // The total length (hex).
  unsigned TotalLen;
  if (Tokens[LengthField].getAsInteger(Radix, TotalLen))
    return patternError("invalid total length: " + Tokens[LengthField]);
  Mod.TotalLen = TotalLen;
  if (Mod.TotalLen == 0)
    return patternError("total length must be non-zero");
  if (Mod.CRCLen != 0 && (Mod.LeadingCount > Mod.TotalLen ||
                          Mod.CRCLen > Mod.TotalLen - Mod.LeadingCount))
    return patternError("CRC range is outside total length");

  // A field that starts an offset, or the unstated byte a tail may start
  // with, is no name.
  auto CannotBeName = [](llvm::StringRef Field) {
    return Field.starts_with(PublicNamePrefix) ||
           Field.starts_with(ReferencePrefix) || Field == RequiredReferences ||
           Field == RequiredCaller || Field.starts_with(UnstatedByte);
  };

  // Remaining tokens form :offset/name pairs, then ^offset/name references,
  // followed by at most one tail.
  bool SawTail = false;
  for (size_t I = OpeningFields; I < Tokens.size(); ++I) {
    if (Tokens[I] == CollisionEvidenceOnly)
      return patternError("collision evidence is not a matchable signature");
    if (Tokens[I] == RequiredReferences || Tokens[I] == RequiredCaller) {
      if (I != OpeningFields)
        return patternError(
            "context requirement must immediately follow total length");
      Mod.RequireReferences = Tokens[I] == RequiredReferences;
      Mod.RequireCaller = Tokens[I] == RequiredCaller;
      continue;
    }
    if (Tokens[I].starts_with(ReferencePrefix)) {
      if (SawTail)
        return patternError("reference follows the tail pattern");
      auto OffStr = Tokens[I].drop_front(ReferencePrefix.size());
      unsigned Off;
      if (OffStr.empty() || OffStr.getAsInteger(Radix, Off))
        return patternError("invalid reference offset: " + Tokens[I]);
      if (Off >= Mod.TotalLen)
        return patternError("reference offset is outside total length");
      if (I + 1 >= Tokens.size() || CannotBeName(Tokens[I + 1]))
        return patternError("reference name is missing after offset: " +
                            Tokens[I]);

      Writer.Names.Names.push_back({Off, Tokens[++I]});
      ++Mod.ReferenceCount;
      continue;
    }
    if (Tokens[I].starts_with(PublicNamePrefix)) {
      if (SawTail)
        return patternError("public name follows the tail pattern");
      if (Mod.ReferenceCount != 0)
        return patternError("public name follows a reference");
      auto OffStr = Tokens[I].drop_front(PublicNamePrefix.size());
      unsigned Off;
      if (OffStr.empty() || OffStr.getAsInteger(Radix, Off))
        return patternError("invalid public name offset: " + Tokens[I]);
      if (Off >= Mod.TotalLen)
        return patternError("public name offset is outside total length");
      if (I + 1 >= Tokens.size() || CannotBeName(Tokens[I + 1]))
        return patternError("public name is missing after offset: " +
                            Tokens[I]);

      Writer.Names.Names.push_back({Off, Tokens[++I]});
      ++Mod.PublicNameCount;
      continue;
    }

    if (SawTail)
      return patternError("unexpected field after the tail pattern: " +
                          Tokens[I]);
    uint32_t Count = Mod.LeadingCount;
    if (llvm::Error Error = appendPattern(Tokens[I], Writer, Mod.Bytes, Count))
      return patternError("invalid tail pattern: " +
                          llvm::toString(std::move(Error)));
    Mod.TailCount = Count - Mod.LeadingCount;
    SawTail = true;
  }

  if (Mod.PublicNameCount == 0)
    return patternError("no public names found");
  if (Mod.RequireReferences &&
      (Mod.ReferenceCount == 0 ||
       uint64_t(Mod.LeadingCount) + Mod.CRCLen + Mod.TailCount < Mod.TotalLen))
    return patternError(
        "!refs requires references and whole-function coverage");

  if (Mod.RequireCaller &&
      (uint64_t(Mod.LeadingCount) + Mod.CRCLen + Mod.TailCount < Mod.TotalLen ||
       llvm::any_of(
           llvm::ArrayRef(Writer.Names.Names)
               .slice(Mod.Names, Mod.PublicNameCount),
           [](const StoredName &Name) { return Name.Offset != 0; })))
    return patternError(
        "!caller requires a complete body with public names at its start");

  // The stated bits follow the bytes.
  const size_t Count = size_t{Mod.LeadingCount} + Mod.TailCount;
  const size_t StatedBytes = llvm::divideCeil(Count, CHAR_BIT);
  if (StatedBytes > Writer.Capacity - Mod.Bytes - Count)
    return noRoom();
  uint8_t *const Stated = Writer.Bytes + Mod.Bytes + Count;
  Writer.Bits.resize(llvm::divideCeil(Count, WordBits));
  constexpr size_t WordBytes = sizeof(uint64_t);
  for (size_t I = 0; I < StatedBytes; ++I)
    Stated[I] = static_cast<uint8_t>(Writer.Bits[I / WordBytes] >>
                                     (I % WordBytes * CHAR_BIT));
  Writer.Used = Mod.Bytes + Count + StatedBytes;
  return Mod;
}

/// Copy \p Names into their own text, so they stop referring to the text
/// they were parsed from.
void keepNames(PatternNames &Names) {
  size_t Length = 0;
  for (const StoredName &Name : Names.Names)
    Length += Name.Name.size();
  Names.Text.resize(Length);
  char *Text = Names.Text.data();
  for (StoredName &Name : Names.Names) {
    std::memcpy(Text, Name.Name.data(), Name.Name.size());
    Name.Name = std::string_view(Text, Name.Name.size());
    Text += Name.Name.size();
  }
}

/// The module \p Draft describes, once its chunk no longer changes.
StoredModule storedModule(const DraftModule &Draft, const uint8_t *Bytes,
                          const PatternNames &Names) {
  StoredModule Module;
  Module.Bytes = Bytes + Draft.Bytes;
  Module.Stated = Module.Bytes + Draft.LeadingCount + Draft.TailCount;
  Module.Names = Names.Names.data() + Draft.Names;
  Module.LeadingCount = Draft.LeadingCount;
  Module.TailCount = Draft.TailCount;
  Module.TotalLen = Draft.TotalLen;
  Module.PublicNameCount = Draft.PublicNameCount;
  Module.ReferenceCount = Draft.ReferenceCount;
  Module.CRC16 = Draft.CRC16;
  Module.CRCLen = Draft.CRCLen;
  Module.RequireReferences = Draft.RequireReferences;
  Module.RequireCaller = Draft.RequireCaller;
  return Module;
}

} // namespace

PatternModule PatternParser::toPatternModule(const StoredModule &Module) {
  PatternModule Result;
  auto Pattern = [&](size_t First, size_t Count) {
    std::vector<PatternByte> Bytes(Count);
    for (size_t I = 0; I < Count; ++I) {
      Bytes[I].Value = Module.Bytes[First + I];
      Bytes[I].IsWildcard = !Module.isStated(First + I);
    }
    return Bytes;
  };
  Result.LeadingBytes = Pattern(0, Module.LeadingCount);
  Result.CRC16 = Module.CRC16;
  Result.CRCLen = Module.CRCLen;
  Result.RequireReferences = Module.RequireReferences;
  Result.RequireCaller = Module.RequireCaller;
  Result.TotalLen = Module.TotalLen;
  for (const StoredName &Name : Module.publicNames())
    Result.PublicNames.push_back({Name.Offset, std::string(Name.Name)});
  for (const StoredName &Name : Module.references())
    Result.References.push_back({Name.Offset, std::string(Name.Name)});
  Result.TailBytes = Pattern(Module.LeadingCount, Module.TailCount);
  return Result;
}

llvm::Expected<PatternModule> PatternParser::parseLine(llvm::StringRef Line) {
  if (isIgnorablePatternLine(Line))
    return patternError("skip line");
  std::vector<uint8_t> Bytes(byteCapacity(Line.size()));
  PatternNames Names;
  ChunkWriter Writer{Bytes.data(), Bytes.size(), 0, {}, Names};
  llvm::Expected<DraftModule> Draft = parsePatternLine(Line, Writer);
  if (!Draft)
    return Draft.takeError();
  return toPatternModule(storedModule(*Draft, Bytes.data(), Names));
}

std::vector<PatternChunk> PatternParser::splitChunks(llvm::StringRef Text,
                                                     size_t ChunkBytes) {
  ChunkBytes = std::max<size_t>(ChunkBytes, 1);
  std::vector<PatternChunk> Chunks;
  Chunks.reserve(Text.size() / ChunkBytes + 1);
  size_t Start = 0;
  while (Start < Text.size()) {
    // The chunk runs to the end of the line that holds its last byte.
    size_t End = Text.size();
    if (Text.size() - Start > ChunkBytes) {
      const size_t LineFeed = Text.find(LineEnd, Start + ChunkBytes - 1);
      if (LineFeed != llvm::StringRef::npos)
        End = LineFeed + sizeof(LineEnd);
    }
    Chunks.emplace_back();
    Chunks.back().Text = Text.slice(Start, End);
    Start = End;
  }
  return Chunks;
}

std::unique_ptr<uint8_t[]>
PatternParser::reserveBytes(llvm::MutableArrayRef<PatternChunk> Chunks) {
  size_t Total = 0;
  for (const PatternChunk &Chunk : Chunks)
    Total += byteCapacity(Chunk.Text.size());
  // Left uninitialized: a page is only touched once a module is written there.
  std::unique_ptr<uint8_t[]> Bytes(new uint8_t[Total]);
  size_t Offset = 0;
  for (PatternChunk &Chunk : Chunks) {
    Chunk.Bytes = Bytes.get() + Offset;
    Chunk.ByteCapacity = byteCapacity(Chunk.Text.size());
    Offset += Chunk.ByteCapacity;
  }
  return Bytes;
}

void PatternParser::parseChunk(PatternChunk &Chunk) {
  ChunkWriter Writer{
      Chunk.Bytes, Chunk.Bytes ? Chunk.ByteCapacity : 0, 0, {}, Chunk.Names};
  std::vector<DraftModule> Drafts;
  llvm::StringRef Rest = Chunk.Text;
  size_t Line = 0;
  while (!Rest.empty()) {
    const size_t LineFeed = Rest.find(LineEnd);
    const llvm::StringRef Text = Rest.take_front(LineFeed);
    Rest = LineFeed == llvm::StringRef::npos
               ? llvm::StringRef()
               : Rest.drop_front(LineFeed + sizeof(LineEnd));
    if (!isIgnorablePatternLine(Text)) {
      llvm::Expected<DraftModule> Draft = parsePatternLine(Text, Writer);
      if (!Draft) {
        Chunk.ErrorLine = Line;
        Chunk.Error = llvm::toString(Draft.takeError());
        return;
      }
      Drafts.push_back(*Draft);
    }
    ++Line;
  }
  Chunk.Lines = Line;

  keepNames(Chunk.Names);
  Chunk.Modules.reserve(Drafts.size());
  for (const DraftModule &Draft : Drafts)
    Chunk.Modules.push_back(storedModule(Draft, Chunk.Bytes, Chunk.Names));
}

void PatternParser::parseChunks(llvm::MutableArrayRef<PatternChunk> Chunks,
                                llvm::function_ref<void(size_t)> Parsed) {
  neverd::parallelForEach(Chunks.size(), [&](auto Claim, size_t Total) {
    for (size_t I = Claim(); I < Total; I = Claim()) {
      parseChunk(Chunks[I]);
      if (Parsed)
        Parsed(I);
    }
  });
}

llvm::Error PatternParser::firstError(llvm::ArrayRef<PatternChunk> Chunks) {
  // Every chunk before the first failure was parsed to its end, so their
  // line counts are whole.
  size_t LinesBefore = 0;
  for (const PatternChunk &Chunk : Chunks) {
    if (Chunk.ErrorLine)
      return patternError("pattern line " +
                          llvm::Twine(LinesBefore + *Chunk.ErrorLine + 1) +
                          ": " + Chunk.Error);
    LinesBefore += Chunk.Lines;
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<PatternModule>>
PatternParser::parseText(llvm::StringRef Text, size_t ChunkBytes) {
  std::vector<PatternChunk> Chunks = splitChunks(Text, ChunkBytes);
  const std::unique_ptr<uint8_t[]> Bytes = reserveBytes(Chunks);
  parseChunks(Chunks);
  if (llvm::Error Error = firstError(Chunks))
    return std::move(Error);

  std::vector<PatternModule> Modules;
  for (const PatternChunk &Chunk : Chunks)
    for (const StoredModule &Module : Chunk.Modules)
      Modules.push_back(toPatternModule(Module));
  return Modules;
}

llvm::Expected<std::vector<PatternModule>>
PatternParser::parseFile(const std::filesystem::path &Path) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path.string(), /*IsText=*/false,
                                              /*RequiresNullTerminator=*/false);
  if (!BufOrErr)
    return patternError("cannot open pattern file: " + Path.string());

  return parseText((*BufOrErr)->getBuffer());
}
