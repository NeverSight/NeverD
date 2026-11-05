//===- StringScan.cpp - text strings in binary data ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The scanners: where a string may start and end in each encoding.  How
// bytes decode is TextCodec's, and whether the characters are text is
// TextEvidence's.
//
//===----------------------------------------------------------------------===//

#include "neverd/support/StringScan.h"

#include "TextCodec.h"
#include "TextEvidence.h"

#include "neverd/support/Limits.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <iterator>
#include <optional>
#include <vector>

namespace neverd::strings {

using namespace detail;

namespace {

/// The first code point displayColumns counts two columns wide.
constexpr uint32_t FirstWideCharacter = [] {
  uint32_t First = UINT32_MAX;
#define NEVERD_WIDE_CHARACTERS(Low, High)                                      \
  if ((Low) < First)                                                           \
    First = (Low);
#include "neverd/support/WideCharacters.def"
  return First;
}();

/// displayColumns, without its table below the first wide character.
inline unsigned columns(uint32_t C) {
  return C < FirstWideCharacter ? 1 : displayColumns(C);
}

/// A C string in \p Legacy from \p Start: its terminator, or nothing when
/// it holds a character that is not text or does not end.  Collects the
/// code points.
const uint8_t *legacyString(const EncodingInfo &Legacy, const uint8_t *Start,
                            const uint8_t *End, std::vector<uint32_t> &Codes) {
  const bool SingleByte = Legacy.Kind == Decoder::SingleByte;
  for (const uint8_t *Cursor = Start; Cursor < End;) {
    if (!*Cursor)
      return Cursor;
    // 0xFFFF marks data, not text, though 0xFF is a letter in some pages.
    if (SingleByte && *Cursor == 0xff && Cursor + 1 < End && Cursor[1] == 0xff)
      return nullptr;
    // GBK alone reads byte 0x80 as the euro sign, which its text seldom
    // writes and data often holds.
    if (Legacy.Kind == Decoder::GB18030 && *Cursor == 0x80)
      return nullptr;
    uint32_t Code[2];
    unsigned Count = 0;
    const unsigned Size = decodeLegacy(Legacy, Cursor, End, Code, Count);
    if (!Size)
      return nullptr;
    for (unsigned I = 0; I < Count; ++I) {
      const uint32_t C = Code[I];
      // Halfwidth katakana come from single bytes 0xA1-0xDF, a quarter of
      // all bytes, and modern text does not use them.
      const bool Text = C < 0x80 ? isTextASCII(C)
                                 : isPrintableBeyondASCII(C) &&
                                       (!SingleByte || isLegacyLetter(C)) &&
                                       !(C >= 0xff61 && C <= 0xff9f);
      if (!Text)
        return nullptr;
      Codes.push_back(C);
    }
    Cursor += Size;
  }
  return nullptr;
}

/// The display columns \p Codes take.
unsigned columnsOf(llvm::ArrayRef<uint32_t> Codes) {
  unsigned Columns = 0;
  for (uint32_t C : Codes)
    Columns += columns(C);
  return Columns;
}

/// A code page, as code pages read a C string.
struct CodePage {
  Encoding Kind;
  const EncodingInfo *Info;
};

/// Reads the C string at \p Start in \p Page into \p Codes: its
/// terminator when it reads as text at least \p MinLength columns wide and
/// not all ASCII, else nothing.  A single-byte page nobody chose (not
/// \p Chosen) needs more evidence.
const uint8_t *readCString(const CodePage &Page, const uint8_t *Start,
                           const uint8_t *End, unsigned MinLength, bool Chosen,
                           std::vector<uint32_t> &Codes) {
  Codes.clear();
  const uint8_t *Terminator = legacyString(*Page.Info, Start, End, Codes);
  const bool SingleByte = Page.Info->Kind == Decoder::SingleByte;
  // Each test only rejects: the cheap ones first.
  if (!Terminator || columnsOf(Codes) < MinLength ||
      std::all_of(Codes.begin(), Codes.end(),
                  [](uint32_t C) { return C < 0x80; }) ||
      !longEnoughText(Codes) ||
      (SingleByte && !Chosen && !readsAsUnchosenText(Codes)) ||
      !readsAsText(Codes, SingleByte ? Source::SingleByte : Source::DoubleByte))
    return nullptr;
  return Terminator;
}

/// Whether a code page made for another writing system than \p Chosen's
/// reads the C string at \p Start as text in its own scripts, every page
/// counted whether searched or not and on the evidence a chosen page needs;
/// with \p Foreign, \p Chosen read it in a script it is not made for, and
/// any page's own reading contests it.
bool contested(llvm::ArrayRef<CodePage> Every, Encoding Chosen, bool Foreign,
               const uint8_t *Start, const uint8_t *End, unsigned MinLength,
               std::vector<uint32_t> &Codes) {
  for (const CodePage &Page : Every)
    if (Page.Kind != Chosen && (Foreign || !shareScripts(Page.Kind, Chosen)) &&
        readCString(Page, Start, End, MinLength, /*Chosen=*/true, Codes) &&
        inCodePageScripts(Codes, Page.Kind))
      return true;
  return false;
}

/// Calls \p Found(At, Start), in order, for each terminator At from \p From
/// before \p To that ends a run of at least \p MinRun elements, the run
/// from Start.  The elements are bytes or code units, by index; the masks
/// \p Fill(At, Count, Terminators, Runs) gives over Count of them from At (as
/// byteRunMasks or unitRunMasks do) mark the terminators and the elements
/// of a run.  No run starts before From.
template <typename FillFn, typename FoundFn>
void forEachTerminatedRun(size_t From, size_t To, size_t MinRun, FillFn Fill,
                          FoundFn Found) {
  constexpr size_t Words = 64;
  uint64_t Terminators[Words], Runs[Words];
  // Where the run that reaches the next word starts.
  size_t RunStart = From;
  uint64_t Previous = 0;
  // The terminators after the first few run elements, by the masks alone.
  const unsigned Shifts = static_cast<unsigned>(std::min<size_t>(MinRun, 8));
  for (size_t Piece = From; Piece < To; Piece += Words * 64) {
    const size_t Count = std::min(Words * 64, To - Piece);
    Fill(Piece, Count, Terminators, Runs);
    for (size_t W = 0; W * 64 < Count; ++W) {
      const size_t Base = Piece + W * 64;
      const uint64_t Run = Runs[W];
      uint64_t Ends = Terminators[W];
      for (unsigned Shift = 1; Shift <= Shifts; ++Shift)
        Ends &= Run << Shift | Previous >> (64 - Shift);
      for (; Ends; Ends &= Ends - 1) {
        const unsigned At = static_cast<unsigned>(std::countr_zero(Ends));
        const uint64_t Breaks = ~Run & ((uint64_t(1) << At) - 1);
        const size_t Start =
            Breaks ? Base + 64 - std::countl_zero(Breaks) : RunStart;
        if (Base + At - Start >= MinRun)
          Found(Base + At, Start);
      }
      // Only the last word has bits past Count, all clear.
      if (const uint64_t Breaks = ~Run)
        RunStart = Base + 64 - std::countl_zero(Breaks);
      Previous = Run;
    }
  }
}

/// Where the text that ends at \p Stop begins, reading back to \p Floor at
/// most: the first of the characters before Stop that a scan reads forward
/// as text, ASCII text and with \p UTF8 printable UTF-8 characters.  UTF-8
/// synchronizes on its first bytes, so a scan reads the same characters from
/// there.
const uint8_t *textBefore(const uint8_t *Floor, const uint8_t *Stop,
                          const uint8_t *End, bool UTF8) {
  const auto continuation = [](uint8_t B) { return B >= 0x80 && B < 0xc0; };
  const uint8_t *First = Stop;
  while (First > Floor) {
    const uint8_t Last = First[-1];
    if (Last < 0x80) {
      if (!isTextASCII(Last))
        break;
      --First;
      continue;
    }
    if (!UTF8 || !continuation(Last))
      break;
    // The sequence's first byte: at most three continuation bytes on.
    size_t Length = 2;
    while (Length < 5 && static_cast<size_t>(First - Floor) >= Length &&
           continuation(First[-static_cast<ptrdiff_t>(Length)]))
      ++Length;
    if (static_cast<size_t>(First - Floor) < Length)
      break;
    const uint8_t *Lead = First - Length;
    uint32_t Code = 0;
    if (*Lead < 0xc2 || *Lead > 0xf4 || decodeUTF8(Lead, End, Code) != Length ||
        !isPrintableBeyondASCII(Code))
      break;
    First = Lead;
  }
  return First;
}

/// ASCII and UTF-8 strings ending in NUL, and where a run after a NUL is not
/// UTF-8, a string in one of \p CodePages that reads it as text: the
/// preferred one when \p Preferred names it first; else the first that reads
/// it in the scripts it is made for, or failing that the first that reads it
/// at all, unless a page of \p Every made for another writing system reads
/// it in its own (contested).  Reads the strings that start from offset
/// \p From before \p To, or with \p FirstOnly at the first byte alone.
void scanCStrings(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
                  llvm::ArrayRef<CodePage> CodePages, bool Preferred,
                  llvm::ArrayRef<CodePage> Every, bool FirstOnly, size_t From,
                  size_t To, std::vector<FoundString> &Out) {
  const bool ASCII = Options.Encodings & encodingBit(Encoding::ASCII);
  const bool UTF8 = Options.Encodings & encodingBit(Encoding::UTF8);
  if (!ASCII && !UTF8 && CodePages.empty())
    return;
  const uint8_t *Begin = Data.data();
  const uint8_t *End = Begin + Data.size();
  std::vector<uint32_t> Codes, Kept;
  // The strings that start at Cursor; returns the last byte read, the next
  // start after it.
  const auto stringsAt = [&](const uint8_t *Cursor) {
    const uint8_t *Start = Cursor;
    unsigned Chars = 0, Columns = 0;
    bool Multibyte = false;
    while (Cursor < End && *Cursor) {
      if (isTextASCII(*Cursor)) {
        const size_t Run = textASCIIPrefix(Cursor, End - Cursor);
        Chars += Run;
        Columns += Run;
        Cursor += Run;
        continue;
      }
      uint32_t Code = 0;
      // decodeUTF8 reads no sequence from a byte that leads none.
      const unsigned Size = UTF8 && *Cursor >= 0xc2 && *Cursor <= 0xf4
                                ? decodeUTF8(Cursor, End, Code)
                                : 0;
      if (!Size || !isPrintableBeyondASCII(Code))
        break;
      ++Chars;
      Columns += columns(Code);
      Cursor += Size;
      Multibyte = true;
    }
    const bool Terminated = Cursor < End && !*Cursor;
    if (Terminated) {
      if (Columns >= Options.MinLength && (Multibyte ? UTF8 : ASCII) &&
          (!Multibyte || [&] {
            const auto Codes = utf8Codes(Start, Cursor);
            return longEnoughText(Codes) && readsAsText(Codes, Source::UTF8);
          }())) {
        FoundString Found;
        Found.Offset = static_cast<uint64_t>(Start - Begin);
        Found.Bytes = static_cast<uint64_t>(Cursor - Start);
        Found.Chars = Chars;
        Found.Kind = Multibyte ? Encoding::UTF8 : Encoding::ASCII;
        Found.Text.assign(reinterpret_cast<const char *>(Start), Found.Bytes);
        Out.push_back(std::move(Found));
      }
      return Cursor;
    }
    // A run UTF-8 does not read may be text in a legacy code page, where a
    // pool of strings would hold one: after a NUL.  Random bytes decode in a
    // double-byte code page most of the time, so starting anywhere else
    // would read most data again from every byte.
    if (CodePages.empty() || Cursor == End || (Start != Begin && Start[-1]))
      return Cursor;
    // Every code page reads a NUL or control byte as no text: one before
    // the run's NUL, or no NUL, leaves none a string.  What the UTF-8 run
    // read before Cursor holds none.
    if (const uint8_t *Stop =
            Cursor +
            findNulOrControl(Cursor, static_cast<size_t>(End - Cursor));
        Stop == End || *Stop)
      return Cursor;
    std::optional<size_t> Chosen;
    bool Foreign = false;
    const uint8_t *Terminator = nullptr;
    for (size_t I = 0; I < CodePages.size(); ++I) {
      const uint8_t *Read =
          readCString(CodePages[I], Start, End, Options.MinLength,
                      Preferred && I == 0, Codes);
      if (!Read)
        continue;
      const bool Own =
          (Preferred && I == 0) || inCodePageScripts(Codes, CodePages[I].Kind);
      if (!Own && Chosen)
        continue;
      Chosen = I;
      Foreign = !Own;
      Terminator = Read;
      Kept.swap(Codes);
      if (Own)
        break;
    }
    if (!Chosen || (!(Preferred && *Chosen == 0) &&
                    contested(Every, CodePages[*Chosen].Kind, Foreign, Start,
                              End, Options.MinLength, Codes)))
      return Cursor;
    FoundString Found;
    Found.Offset = static_cast<uint64_t>(Start - Begin);
    Found.Bytes = static_cast<uint64_t>(Terminator - Start);
    Found.Chars = static_cast<unsigned>(Kept.size());
    Found.Kind = CodePages[*Chosen].Kind;
    appendUTF8(Found.Text, Kept);
    Out.push_back(std::move(Found));
    return Terminator;
  };
  if (FirstOnly) {
    if (From < To)
      stringsAt(Begin);
    return;
  }
  // A string ends at a NUL and holds no NUL or control byte, and none of its
  // characters takes more columns than bytes (no code page reads a byte as
  // a wide character): only a run of at least MinLength other bytes before
  // a NUL holds one.  The scan reads such a run from its first byte, as no
  // string that starts before it ends past its start.  Within the run, a
  // code page reads only from its first byte, after a NUL; otherwise only
  // the ASCII or UTF-8 text that ends at the NUL is a string, which the scan
  // reads from its first character, every run before it ending before it.
  forEachTerminatedRun(
      From, To, Options.MinLength,
      [&](size_t At, size_t Count, uint64_t *Nul, uint64_t *Run) {
        byteRunMasks(Begin + At, Count, /*Text=*/false, Nul, Run);
      },
      [&](size_t Nul, size_t Start) {
        const uint8_t *Stop = Begin + Nul;
        const uint8_t *Floor = Begin + Start;
        if (!CodePages.empty() && (Floor == Begin || !Floor[-1])) {
          const uint8_t *Read = stringsAt(Floor);
          if (Read >= Stop)
            return;
          Floor = Read + 1;
        }
        if (!ASCII && !UTF8)
          return;
        if (const uint8_t *First = textBefore(Floor, Stop, End, UTF8);
            First < Stop)
          stringsAt(First);
      });
}

/// UTF-16 or UTF-32 strings ending in a zero unit, at multiples of the unit
/// size, that read as text (readsAsText).  Random 16-bit data decodes into
/// the large ideograph blocks a third of the time, so a string mostly beyond
/// ASCII also needs a zero unit before it, as in a pool of strings, and must
/// not be ASCII read one byte off (every unit's low byte zero).
/// Reads the strings that start from offset \p From before \p To, both
/// multiples of \p Unit, or with \p FirstOnly at the first byte alone.
template <unsigned Unit, bool BigEndian>
void scanWideStrings(llvm::ArrayRef<uint8_t> Data, Encoding Kind,
                     unsigned MinLength, llvm::ArrayRef<uint64_t> NarrowEnds,
                     bool FirstOnly, size_t From, size_t To,
                     std::vector<FoundString> &Out) {
  const WideTextSet &Text = wideText();
  const uint8_t *Begin = Data.data();
  const uint8_t *End = Begin + Data.size() / Unit * Unit;
  // Where a rejected run's text may begin after a character the bytes
  // before a string start with.
  const uint8_t *Resumed = nullptr;
  auto NextNarrow = std::lower_bound(NarrowEnds.begin(), NarrowEnds.end(),
                                     static_cast<uint64_t>(From));
  // The text a zero unit ends, a character to a slot, from the slot readBack
  // returns up to the units it read back over: where each character starts
  // and its code point, the tallies of the text from it on, and the slot of
  // the ASCII character that next follows it (the units if none).  Every
  // start the scan takes within the text is a character's: a narrow end
  // follows a NUL after ASCII text, which no first half of a pair holds.
  std::vector<const uint8_t *> Starts;
  std::vector<uint32_t> Codes, NextASCII;
  std::vector<unsigned> Columns, ASCII;
  std::vector<uint8_t> LowBytesZero;
  // Reads back from the zero unit at Zero to Lower at most, over the text
  // before it; returns its first slot, the slots ending at Units.
  const auto readBack = [&](const uint8_t *Lower, const uint8_t *Zero,
                            size_t Units) {
    if (Codes.size() < Units) {
      Starts.resize(Units);
      Codes.resize(Units);
      NextASCII.resize(Units);
      Columns.resize(Units);
      ASCII.resize(Units);
      LowBytesZero.resize(Units);
    }
    size_t Slot = Units;
    const uint8_t *First = Zero;
    while (First > Lower) {
      uint32_t Code = readUnit<Unit, BigEndian>(First - Unit);
      unsigned Used = Unit;
      if (Unit == 2 && Code >= 0xdc00 && Code <= 0xdfff) {
        // The second half of a pair, or of no character.
        if (static_cast<size_t>(First - Lower) < 2 * Unit)
          break;
        const uint32_t High = readUnit<Unit, BigEndian>(First - 2 * Unit);
        if (High < 0xd800 || High > 0xdbff)
          break;
        Code = 0x10000 + ((High - 0xd800) << 10) + (Code - 0xdc00);
        Used = 2 * Unit;
      } else if ((Code >= 0xd800 && Code <= 0xdfff) || Code > 0x10ffff) {
        // A first half before no second half, or no code point.
        break;
      }
      if (!Text.contains(Code))
        break;
      First -= Used;
      const bool Last = Slot == Units;
      --Slot;
      Starts[Slot] = First;
      Codes[Slot] = Code;
      Columns[Slot] = (Last ? 0 : Columns[Slot + 1]) + columns(Code);
      ASCII[Slot] = (Last ? 0 : ASCII[Slot + 1]) + (Code < 0x80);
      LowBytesZero[Slot] = (Last || LowBytesZero[Slot + 1]) &&
                           (Code < 0x80 || (Code & 0xff) == 0);
      NextASCII[Slot] = Last ? static_cast<uint32_t>(Units)
                        : Codes[Slot + 1] < 0x80
                            ? static_cast<uint32_t>(Slot + 1)
                            : NextASCII[Slot + 1];
    }
    return Slot;
  };
  // The strings that start in the text in slots [Slot, Units), which the
  // zero unit at Zero ends: from its first character, and where a retry goes
  // on after rejecting a reading; with FirstOnly from the first alone.
  const auto stringsIn = [&](size_t Slot, size_t Units, const uint8_t *Zero,
                             bool FirstOnly) {
    for (size_t K = Slot; K < Units;) {
      const uint8_t *Start = Starts[K];
      // Text that ends before MinLength columns reads as no string; nor does
      // any a retry takes from within it, which has no more columns.
      if (Columns[K] < MinLength)
        return;
      while (NextNarrow != NarrowEnds.end() &&
             (*NextNarrow < static_cast<uint64_t>(Start - Begin) ||
              *NextNarrow % Unit))
        ++NextNarrow;
      const bool AfterNarrow =
          NextNarrow != NarrowEnds.end() &&
          *NextNarrow == static_cast<uint64_t>(Start - Begin);
      if (AfterNarrow)
        ++NextNarrow;
      while (NextNarrow != NarrowEnds.end() && *NextNarrow % Unit)
        ++NextNarrow;
      const uint8_t *NarrowStart =
          NextNarrow == NarrowEnds.end() ? nullptr : Begin + *NextNarrow;
      const size_t Chars = Units - K;
      // A run that the bytes before a string start with a character, such as
      // `00 40` (U+4000) before L"HashDigestLength", is no text; the string
      // may begin at its first ASCII character all the same.  Whether the
      // scan goes on within the text, from K.
      const auto retry = [&] {
        if (FirstOnly)
          return false;
        // Retry only after rejecting the earlier wide reading: valid wide
        // text can itself contain bytes that look like a terminated ASCII
        // run.
        if (NarrowStart && NarrowStart <= Zero && NarrowStart < End) {
          Resumed = nullptr;
          K = static_cast<size_t>(std::lower_bound(Starts.begin() + K,
                                                   Starts.begin() + Units,
                                                   NarrowStart) -
                                  Starts.begin());
          return true;
        }
        if (NextASCII[K] < Units && readUnit<Unit, BigEndian>(Start) >= 0x80) {
          K = NextASCII[K];
          Resumed = Starts[K];
          return true;
        }
        return false;
      };
      // Text resumed after an arbitrary wide character must still be ASCII;
      // a verified narrow-string boundary is independent evidence instead.
      // Text mostly beyond ASCII also sits where a pool of strings puts it,
      // after a zero unit, and is not ASCII read one byte off.
      if ((Start == Resumed && ASCII[K] != Chars) ||
          (ASCII[K] * 2 < Chars &&
           (LowBytesZero[K] || (!AfterNarrow && Start != Begin &&
                                readUnit<Unit, BigEndian>(Start - Unit))))) {
        if (!retry())
          return;
        continue;
      }
      const llvm::ArrayRef<uint32_t> Read(Codes.data() + K, Chars);
      if (!longEnoughText(Read) || !readsAsText(Read, Source::Wide)) {
        if (!retry())
          return;
        continue;
      }
      FoundString Found;
      Found.Offset = static_cast<uint64_t>(Start - Begin);
      Found.Bytes = static_cast<uint64_t>(Zero - Start);
      Found.Chars = static_cast<unsigned>(Chars);
      Found.Kind = Kind;
      appendUTF8(Found.Text, Read);
      Out.push_back(std::move(Found));
      return;
    }
  };
  if (FirstOnly) {
    if (From >= To)
      return;
    // The text from the first byte, and the zero unit that ends it.
    const uint8_t *Zero = Begin;
    uint32_t Code = 0;
    while (Zero < End) {
      const unsigned Used = decodeWide<Unit, BigEndian>(Zero, End, Code);
      if (!Used || !Text.contains(Code))
        break;
      Zero += Used;
    }
    if (Zero == Begin || Zero >= End || readUnit<Unit, BigEndian>(Zero))
      return;
    const size_t Units = static_cast<size_t>(Zero - Begin) / Unit;
    const size_t Slot = readBack(Begin, Zero, Units);
    assert(Starts[Slot] == Begin && "the text reads back to its first byte");
    stringsIn(Slot, Units, Zero, /*FirstOnly=*/true);
    return;
  }
  // A string ends at a zero unit and holds no unit that is no text
  // (isNonTextUnit), and none of its characters takes more than two columns:
  // only a run of at least half MinLength other units before a zero unit
  // holds one.  The scan reads the text that ends such a run from its first
  // character, as no string that starts before it ends past its start, and
  // finds none in text of fewer columns.
  forEachTerminatedRun(
      From / Unit, To / Unit, (MinLength + 1) / 2,
      [&](size_t At, size_t Count, uint64_t *Zero, uint64_t *Run) {
        unitRunMasks(Begin + At * Unit, Count, Unit, BigEndian, Zero, Run);
      },
      [&](size_t ZeroUnit, size_t RunStart) {
        const size_t Units = ZeroUnit - RunStart;
        const size_t Slot =
            readBack(Begin + RunStart * Unit, Begin + ZeroUnit * Unit, Units);
        if (Slot < Units)
          stringsIn(Slot, Units, Begin + ZeroUnit * Unit,
                    /*FirstOnly=*/false);
      });
}

} // namespace

namespace {

/// The ends of the terminated ASCII runs at least \p MinLength long that
/// start from offset \p From before \p To: where the byte after each run's
/// NUL lies, the evidence of a string boundary the wide scans read.
void narrowEnds(llvm::ArrayRef<uint8_t> Data, unsigned MinLength, size_t From,
                size_t To, std::vector<uint64_t> &Out) {
  forEachTerminatedRun(
      From, To, MinLength,
      [&](size_t At, size_t Count, uint64_t *Nul, uint64_t *Run) {
        byteRunMasks(Data.data() + At, Count, /*Text=*/true, Nul, Run);
      },
      [&](size_t Nul, size_t) {
        Out.push_back(static_cast<uint64_t>(Nul) + 1);
      });
}

/// Offsets that split \p Data into spans of about \p Size bytes, from 0 to
/// its end, each after a NUL byte: no C string, code page string or ASCII
/// run continues past a NUL, so the scans of the spans read what one scan
/// of the whole does.
std::vector<size_t> nulBounds(llvm::ArrayRef<uint8_t> Data, size_t Size) {
  std::vector<size_t> Bounds{0};
  for (size_t At = Size; At < Data.size();) {
    const auto *Nul = static_cast<const uint8_t *>(
        std::memchr(Data.data() + At, 0, Data.size() - At));
    if (!Nul)
      break;
    const size_t After = static_cast<size_t>(Nul - Data.data()) + 1;
    if (After >= Data.size())
      break;
    Bounds.push_back(After);
    At = After + Size;
  }
  Bounds.push_back(Data.size());
  return Bounds;
}

/// Offsets that split \p Data's whole units of \p Unit bytes into spans of
/// about \p Size bytes, from 0 to their end, each after a zero unit at a
/// multiple of \p Unit: no wide string continues past one, and no retry
/// steps back over one, so the scans of the spans read what one scan of the
/// whole does.
std::vector<size_t> zeroUnitBounds(llvm::ArrayRef<uint8_t> Data, unsigned Unit,
                                   size_t Size) {
  const size_t End = Data.size() / Unit * Unit;
  const auto zero = [&](size_t At) {
    for (unsigned I = 0; I < Unit; ++I)
      if (Data[At + I])
        return false;
    return true;
  };
  std::vector<size_t> Bounds{0};
  for (size_t At = Size / Unit * Unit; At < End;) {
    while (At < End && !zero(At))
      At += Unit;
    if (At + Unit >= End)
      break;
    Bounds.push_back(At + Unit);
    At += Unit + Size / Unit * Unit;
  }
  Bounds.push_back(End);
  return Bounds;
}

/// The strings of every pass over \p Data, unordered and overlapping; with
/// \p FirstOnly, only those that start at its first byte.  With
/// \p Parallel, a large scan runs spans of each pass as its tasks.
std::vector<FoundString> scanPasses(llvm::ArrayRef<uint8_t> Data,
                                    const ScanOptions &Options, bool FirstOnly,
                                    ParallelTasks Parallel) {
  const unsigned MinLength = std::max(1u, Options.MinLength);
  ScanOptions Effective = Options;
  Effective.MinLength = MinLength;
  // The code pages searched: the preferred one, then table order.
  std::vector<CodePage> CodePages;
  const auto Known = encodings();
  const bool Preferred =
      Options.Preferred && isLegacyEncoding(*Options.Preferred);
  if (Preferred)
    CodePages.push_back({*Options.Preferred,
                         &Known[static_cast<unsigned>(*Options.Preferred)]});
  std::vector<CodePage> Every;
  for (unsigned I = 0; I < Known.size(); ++I) {
    const auto Kind = static_cast<Encoding>(I);
    if (Known[I].Kind == Decoder::Unicode)
      continue;
    Every.push_back({Kind, &Known[I]});
    if ((Options.Encodings & encodingBit(Kind)) && Kind != Options.Preferred)
      CodePages.push_back({Kind, &Known[I]});
  }
  const auto searched = [&](Encoding E) {
    return (Options.Encodings & encodingBit(E)) != 0;
  };
  const bool Wide = searched(Encoding::UTF16LE) ||
                    searched(Encoding::UTF16BE) ||
                    searched(Encoding::UTF32LE) || searched(Encoding::UTF32BE);
  const bool Split = Parallel && !FirstOnly &&
                     Data.size() >= limits::support::kStringScanParallelBytes;
  const size_t Span = limits::support::kStringScanParallelSpanBytes;
  const auto run = [&](size_t Tasks, llvm::function_ref<void(size_t)> Task) {
    if (Split && Tasks > 1)
      Parallel(Tasks, Task);
    else
      for (size_t I = 0; I < Tasks; ++I)
        Task(I);
  };
  // C strings, and the ends of the terminated ASCII runs: evidence of the
  // boundary after each that the wide scans read.  The ASCII run need not
  // end in a whole zero wide unit, and a UTF-16-only scan still needs it.  No
  // run ends before the first byte, so a scan from it reads none.
  const std::vector<size_t> Bounds =
      Split ? nulBounds(Data, Span) : std::vector<size_t>{0, Data.size()};
  std::vector<std::vector<FoundString>> Found(Bounds.size() - 1);
  std::vector<std::vector<uint64_t>> Ends(Bounds.size() - 1);
  run(Bounds.size() - 1, [&](size_t I) {
    scanCStrings(Data, Effective, CodePages, Preferred, Every, FirstOnly,
                 Bounds[I], Bounds[I + 1], Found[I]);
    if (Wide && !FirstOnly)
      narrowEnds(Data, MinLength, Bounds[I], Bounds[I + 1], Ends[I]);
  });
  std::vector<uint64_t> NarrowEnds;
  for (const std::vector<uint64_t> &Each : Ends)
    NarrowEnds.insert(NarrowEnds.end(), Each.begin(), Each.end());
  // The wide scans, a span of one encoding per task.
  struct WideSpan {
    Encoding Kind;
    size_t From, To;
  };
  std::vector<WideSpan> Spans;
  for (const unsigned Unit : {2u, 4u}) {
    const Encoding Little = Unit == 2 ? Encoding::UTF16LE : Encoding::UTF32LE;
    const Encoding Big = Unit == 2 ? Encoding::UTF16BE : Encoding::UTF32BE;
    if (!searched(Little) && !searched(Big))
      continue;
    const std::vector<size_t> Units =
        Split ? zeroUnitBounds(Data, Unit, Span)
              : std::vector<size_t>{0, Data.size() / Unit * Unit};
    for (const Encoding Kind : {Little, Big})
      if (searched(Kind))
        for (size_t I = 0; I + 1 < Units.size(); ++I)
          Spans.push_back({Kind, Units[I], Units[I + 1]});
  }
  std::vector<std::vector<FoundString>> WideFound(Spans.size());
  run(Spans.size(), [&](size_t I) {
    const WideSpan &Each = Spans[I];
    switch (Each.Kind) {
    case Encoding::UTF16LE:
      scanWideStrings<2, false>(Data, Each.Kind, MinLength, NarrowEnds,
                                FirstOnly, Each.From, Each.To, WideFound[I]);
      break;
    case Encoding::UTF16BE:
      scanWideStrings<2, true>(Data, Each.Kind, MinLength, NarrowEnds,
                               FirstOnly, Each.From, Each.To, WideFound[I]);
      break;
    case Encoding::UTF32LE:
      scanWideStrings<4, false>(Data, Each.Kind, MinLength, NarrowEnds,
                                FirstOnly, Each.From, Each.To, WideFound[I]);
      break;
    default:
      scanWideStrings<4, true>(Data, Each.Kind, MinLength, NarrowEnds,
                               FirstOnly, Each.From, Each.To, WideFound[I]);
      break;
    }
  });
  std::vector<FoundString> All;
  size_t Total = 0;
  for (const auto &Each : Found)
    Total += Each.size();
  for (const auto &Each : WideFound)
    Total += Each.size();
  All.reserve(Total);
  for (auto *Lists : {&Found, &WideFound})
    for (std::vector<FoundString> &Each : *Lists)
      std::move(Each.begin(), Each.end(), std::back_inserter(All));
  return All;
}

/// The order the scan reports strings in: the first to start keeps the
/// bytes; at one offset, the longer, then the Unicode encoding.
bool reportedBefore(const FoundString &A, const FoundString &B) {
  if (A.Offset != B.Offset)
    return A.Offset < B.Offset;
  if (A.Bytes != B.Bytes)
    return A.Bytes > B.Bytes;
  return A.Kind < B.Kind;
}

} // namespace

void scan(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
          llvm::function_ref<void(FoundString &&)> Found,
          ParallelTasks Parallel) {
  std::vector<FoundString> All =
      scanPasses(Data, Options, /*FirstOnly=*/false, Parallel);
  std::sort(All.begin(), All.end(), reportedBefore);
  uint64_t Free = 0;
  for (auto &String : All) {
    if (String.Offset < Free)
      continue;
    Free = String.Offset + String.Bytes + info(String.Kind).UnitBytes;
    Found(std::move(String));
  }
}

std::optional<FoundString> scanAt(llvm::ArrayRef<uint8_t> Data,
                                  const ScanOptions &Options) {
  // Nothing starts before the first byte, so what the scan reports first,
  // where it starts there, is the first in report order of the strings
  // that start there; every pass finds those from that byte alone.
  std::vector<FoundString> All =
      scanPasses(Data, Options, /*FirstOnly=*/true, /*Parallel=*/{});
  const auto First = std::min_element(All.begin(), All.end(), reportedBefore);
  if (First == All.end() || First->Offset != 0)
    return std::nullopt;
  return std::move(*First);
}

} // namespace neverd::strings
