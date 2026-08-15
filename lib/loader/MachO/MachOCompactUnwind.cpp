//===- MachOCompactUnwind.cpp - Darwin __unwind_info decoding ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-macho-unwind"

namespace neverd::macho_unwind {
namespace {

/// The best-effort loader parser is used while opening arbitrary binaries and
/// deliberately keeps a fixed work budget.  The strict rewrite parser below
/// instead exposes caller-selected ceilings through its public options.
constexpr uint32_t kMaxEntries = 1u << 22;
constexpr uint32_t kMaxPages = 1u << 16;
/// A compressed entry has an eight-bit encoding index.  Darwin reserves
/// indices 0..126 for the common table and the remaining indices for the
/// current page's local table.
constexpr uint32_t kMaxCommonEncodings = 127;
constexpr uint32_t kMaxIndexedEncodings = 256;

/// A bounded view over the section, so every field read is checked once here
/// instead of at each use.
class SectionReader {
public:
  SectionReader(const uint8_t *Data, size_t Size, llvm::endianness ByteOrder)
      : Data(Data), Size(Size), ByteOrder(ByteOrder) {}

  bool u32At(uint64_t Offset, uint32_t &Out) const {
    if (!rangeInBounds(Offset, 4, Size))
      return false;
    Out = llvm::support::endian::read<uint32_t>(Data + Offset, ByteOrder);
    return true;
  }
  bool u16At(uint64_t Offset, uint16_t &Out) const {
    if (!rangeInBounds(Offset, 2, Size))
      return false;
    Out = llvm::support::endian::read<uint16_t>(Data + Offset, ByteOrder);
    return true;
  }

private:
  const uint8_t *Data;
  size_t Size;
  llvm::endianness ByteOrder;
};

struct RawByteRange {
  uint64_t Begin = 0;
  uint64_t End = 0;
};

llvm::Error rawParseError(const llvm::Twine &Message) {
  return llvm::createStringError(
      llvm::errc::invalid_argument, "%s",
      (llvm::Twine("malformed __unwind_info: ") + Message).str().c_str());
}

/// Validate one byte range and add it to \p Ranges.  Empty tables still have
/// to point inside the section, but occupy no bytes and therefore cannot
/// overlap another range.
llvm::Error addRawRange(std::vector<RawByteRange> &Ranges, uint64_t Offset,
                        uint64_t Count, uint64_t ElementSize,
                        uint64_t SectionSize, llvm::StringRef Description,
                        RawByteRange &Result) {
  if (Offset > SectionSize)
    return rawParseError(llvm::Twine(Description) + " starts past the section");
  if (ElementSize == 0 ||
      Count > std::numeric_limits<uint64_t>::max() / ElementSize)
    return rawParseError(llvm::Twine(Description) + " size overflows");

  const uint64_t ByteCount = Count * ElementSize;
  if (ByteCount > SectionSize - Offset)
    return rawParseError(llvm::Twine(Description) + " leaves the section");

  Result = {Offset, Offset + ByteCount};
  if (Result.Begin == Result.End)
    return llvm::Error::success();

  for (const RawByteRange &Existing : Ranges) {
    if (Result.Begin < Existing.End && Existing.Begin < Result.End)
      return rawParseError(llvm::Twine(Description) +
                           " overlaps another table or page");
  }
  Ranges.push_back(Result);
  return llvm::Error::success();
}

} // namespace

llvm::Expected<CompactUnwindRawSection>
parseCompactUnwindRaw(llvm::ArrayRef<uint8_t> SectionBytes,
                      CompactUnwindRawParseOptions Options) {
  if (SectionBytes.size() < 28)
    return rawParseError("section is shorter than its header");
  if (Options.ByteOrder != llvm::endianness::little &&
      Options.ByteOrder != llvm::endianness::big)
    return rawParseError("byte order is not explicit");

  const SectionReader R(SectionBytes.data(), SectionBytes.size(),
                        Options.ByteOrder);
  CompactUnwindRawSection Result;
  Result.OriginalBytes.assign(SectionBytes.begin(), SectionBytes.end());
  CompactUnwindRawHeader &Header = Result.Header;
  if (!R.u32At(0, Header.Version) ||
      !R.u32At(4, Header.CommonEncodingsSectionOffset) ||
      !R.u32At(8, Header.CommonEncodingsCount) ||
      !R.u32At(12, Header.PersonalityArraySectionOffset) ||
      !R.u32At(16, Header.PersonalityArrayCount) ||
      !R.u32At(20, Header.IndexSectionOffset) ||
      !R.u32At(24, Header.IndexCount))
    return rawParseError("truncated header");
  if (Header.Version != kUnwindSectionVersion)
    return rawParseError("unsupported version");
  if (Header.IndexCount < 2)
    return rawParseError("first-level index has no terminal sentinel");
  if (Header.CommonEncodingsCount > kMaxCommonEncodings)
    return rawParseError(
        "common encoding array exceeds the seven-bit index range");
  if (uint64_t(Header.IndexCount) - 1 > Options.MaxPages)
    return rawParseError("page count exceeds the caller resource policy");
  if (Header.PersonalityArrayCount > 3)
    return rawParseError("personality array has more than three entries");

  std::vector<RawByteRange> GlobalRanges;
  RawByteRange Range;
  if (llvm::Error Error = addRawRange(GlobalRanges, 0, 28, 1,
                                      SectionBytes.size(), "header", Range))
    return std::move(Error);
  if (llvm::Error Error =
          addRawRange(GlobalRanges, Header.CommonEncodingsSectionOffset,
                      Header.CommonEncodingsCount, 4, SectionBytes.size(),
                      "common encoding array", Range))
    return std::move(Error);
  if (llvm::Error Error =
          addRawRange(GlobalRanges, Header.PersonalityArraySectionOffset,
                      Header.PersonalityArrayCount, 4, SectionBytes.size(),
                      "personality array", Range))
    return std::move(Error);
  if (llvm::Error Error = addRawRange(
          GlobalRanges, Header.IndexSectionOffset, Header.IndexCount, 12,
          SectionBytes.size(), "first-level index", Range))
    return std::move(Error);

  Result.CommonEncodings.reserve(Header.CommonEncodingsCount);
  for (uint32_t I = 0; I < Header.CommonEncodingsCount; ++I) {
    uint32_t Encoding = 0;
    const uint64_t Offset =
        uint64_t(Header.CommonEncodingsSectionOffset) + uint64_t(I) * 4;
    if (!R.u32At(Offset, Encoding))
      return rawParseError("common encoding array is truncated");
    Result.CommonEncodings.push_back(Encoding);
  }

  Result.PersonalitySlotOffsets.reserve(Header.PersonalityArrayCount);
  for (uint32_t I = 0; I < Header.PersonalityArrayCount; ++I) {
    uint32_t SlotOffset = 0;
    const uint64_t Offset =
        uint64_t(Header.PersonalityArraySectionOffset) + uint64_t(I) * 4;
    if (!R.u32At(Offset, SlotOffset))
      return rawParseError("personality array is truncated");
    Result.PersonalitySlotOffsets.push_back(SlotOffset);
  }

  Result.Index.reserve(Header.IndexCount);
  for (uint32_t I = 0; I < Header.IndexCount; ++I) {
    const uint64_t Offset =
        uint64_t(Header.IndexSectionOffset) + uint64_t(I) * 12;
    CompactUnwindRawIndexEntry Entry;
    if (!R.u32At(Offset, Entry.FunctionOffset) ||
        !R.u32At(Offset + 4, Entry.SecondLevelPageSectionOffset) ||
        !R.u32At(Offset + 8, Entry.LSDAIndexArraySectionOffset))
      return rawParseError("first-level index is truncated");
    if (!Result.Index.empty() &&
        Entry.FunctionOffset <= Result.Index.back().FunctionOffset)
      return rawParseError(
          "first-level function offsets are not strictly increasing");

    const bool IsTerminal = I + 1 == Header.IndexCount;
    if ((Entry.SecondLevelPageSectionOffset == 0) != IsTerminal)
      return rawParseError(
          "first-level index does not have one final terminal sentinel");
    Result.Index.push_back(Entry);
  }

  // The first and terminal index records bound the one global LSDA array.
  // Intermediate offsets divide it into source-ordered per-page slices.
  const uint64_t LSDAArrayStart =
      Result.Index.front().LSDAIndexArraySectionOffset;
  const uint64_t LSDAArrayEnd = Result.Index.back().LSDAIndexArraySectionOffset;
  if (LSDAArrayStart > LSDAArrayEnd)
    return rawParseError("LSDA array bounds are reversed");
  if ((LSDAArrayEnd - LSDAArrayStart) % 8 != 0)
    return rawParseError("LSDA array size is not entry-aligned");
  if (llvm::Error Error = addRawRange(GlobalRanges, LSDAArrayStart,
                                      (LSDAArrayEnd - LSDAArrayStart) / 8, 8,
                                      SectionBytes.size(), "LSDA array", Range))
    return std::move(Error);

  uint64_t PreviousLSDAIndexOffset = LSDAArrayStart;
  for (const CompactUnwindRawIndexEntry &Entry : Result.Index) {
    const uint64_t Offset = Entry.LSDAIndexArraySectionOffset;
    if (Offset < LSDAArrayStart || Offset > LSDAArrayEnd ||
        (Offset - LSDAArrayStart) % 8 != 0)
      return rawParseError("first-level LSDA slice is outside the LSDA array");
    if (Offset < PreviousLSDAIndexOffset)
      return rawParseError("first-level LSDA slices are not ordered");
    PreviousLSDAIndexOffset = Offset;
  }

  bool HavePreviousLSDA = false;
  uint32_t PreviousLSDAFunction = 0;
  for (size_t I = 0; I + 1 < Result.Index.size(); ++I) {
    const uint64_t SliceBegin = Result.Index[I].LSDAIndexArraySectionOffset;
    const uint64_t SliceEnd = Result.Index[I + 1].LSDAIndexArraySectionOffset;
    for (uint64_t Offset = SliceBegin; Offset < SliceEnd; Offset += 8) {
      CompactUnwindRawLSDAEntry Entry;
      if (!R.u32At(Offset, Entry.FunctionOffset) ||
          !R.u32At(Offset + 4, Entry.LSDAOffset))
        return rawParseError("LSDA array is truncated");
      if (Entry.FunctionOffset < Result.Index[I].FunctionOffset ||
          Entry.FunctionOffset >= Result.Index[I + 1].FunctionOffset)
        return rawParseError("LSDA function lies outside its page range");
      if (HavePreviousLSDA && Entry.FunctionOffset <= PreviousLSDAFunction)
        return rawParseError(
            "LSDA function offsets are duplicated or not sorted");
      HavePreviousLSDA = true;
      PreviousLSDAFunction = Entry.FunctionOffset;
      Result.LSDAEntries.push_back(Entry);
    }
  }

  uint64_t TotalEntries = 0;
  Result.Pages.reserve(Result.Index.size() - 1);
  for (size_t I = 0; I + 1 < Result.Index.size(); ++I) {
    const CompactUnwindRawIndexEntry &PageIndex = Result.Index[I];
    CompactUnwindRawPage Page;
    Page.SectionOffset = PageIndex.SecondLevelPageSectionOffset;
    if (!R.u32At(Page.SectionOffset, Page.Kind))
      return rawParseError("second-level page header leaves the section");
    if (Page.Kind != kSecondLevelRegular && Page.Kind != kSecondLevelCompressed)
      return rawParseError("unknown second-level page kind");
    if (!R.u16At(uint64_t(Page.SectionOffset) + 4, Page.EntryPageOffset) ||
        !R.u16At(uint64_t(Page.SectionOffset) + 6, Page.EntryCount))
      return rawParseError("second-level page header is truncated");

    const uint64_t HeaderSize =
        Page.Kind == kSecondLevelRegular ? uint64_t(8) : uint64_t(12);
    if (Page.Kind == kSecondLevelCompressed &&
        (!R.u16At(uint64_t(Page.SectionOffset) + 8, Page.EncodingsPageOffset) ||
         !R.u16At(uint64_t(Page.SectionOffset) + 10, Page.EncodingsCount)))
      return rawParseError("compressed page header is truncated");
    if (Page.EntryCount == 0)
      return rawParseError("second-level page has no entries");
    TotalEntries += Page.EntryCount;
    if (TotalEntries > Options.MaxEntries)
      return rawParseError(
          "compact unwind entries exceed the caller resource policy");

    std::vector<RawByteRange> PageRanges;
    RawByteRange HeaderRange;
    if (llvm::Error Error = addRawRange(
            PageRanges, Page.SectionOffset, HeaderSize, 1, SectionBytes.size(),
            "second-level page header", HeaderRange))
      return std::move(Error);

    const uint64_t EntriesOffset =
        uint64_t(Page.SectionOffset) + Page.EntryPageOffset;
    RawByteRange EntriesRange;
    const uint64_t EntrySize =
        Page.Kind == kSecondLevelRegular ? uint64_t(8) : uint64_t(4);
    if (llvm::Error Error = addRawRange(
            PageRanges, EntriesOffset, Page.EntryCount, EntrySize,
            SectionBytes.size(), "second-level page entries", EntriesRange))
      return std::move(Error);

    RawByteRange EncodingsRange;
    if (Page.Kind == kSecondLevelCompressed) {
      if (uint64_t(Header.CommonEncodingsCount) + Page.EncodingsCount >
          kMaxIndexedEncodings)
        return rawParseError(
            "compressed encoding tables exceed the eight-bit index space");
      const uint64_t EncodingsOffset =
          uint64_t(Page.SectionOffset) + Page.EncodingsPageOffset;
      if (llvm::Error Error = addRawRange(
              PageRanges, EncodingsOffset, Page.EncodingsCount, 4,
              SectionBytes.size(), "compressed page encodings", EncodingsRange))
        return std::move(Error);

      Page.LocalEncodings.reserve(Page.EncodingsCount);
      for (uint32_t E = 0; E < Page.EncodingsCount; ++E) {
        uint32_t Encoding = 0;
        if (!R.u32At(EncodingsOffset + uint64_t(E) * 4, Encoding))
          return rawParseError("compressed encoding array is truncated");
        Page.LocalEncodings.push_back(Encoding);
      }
    }

    uint32_t PreviousFunction = 0;
    if (Page.Kind == kSecondLevelRegular) {
      Page.RegularEntries.reserve(Page.EntryCount);
      for (uint32_t E = 0; E < Page.EntryCount; ++E) {
        CompactUnwindRawRegularEntry Entry;
        const uint64_t Offset = EntriesOffset + uint64_t(E) * 8;
        if (!R.u32At(Offset, Entry.FunctionOffset) ||
            !R.u32At(Offset + 4, Entry.Encoding))
          return rawParseError("regular page entry is truncated");
        if (Entry.FunctionOffset < PageIndex.FunctionOffset ||
            Entry.FunctionOffset >= Result.Index[I + 1].FunctionOffset)
          return rawParseError("regular entry lies outside its page range");
        if (E == 0 && Entry.FunctionOffset != PageIndex.FunctionOffset)
          return rawParseError("regular page does not begin at its index key");
        if (E != 0 && Entry.FunctionOffset <= PreviousFunction)
          return rawParseError(
              "regular entries are duplicated or not strictly sorted");
        PreviousFunction = Entry.FunctionOffset;
        Page.RegularEntries.push_back(Entry);
      }
    } else {
      Page.CompressedEntries.reserve(Page.EntryCount);
      for (uint32_t E = 0; E < Page.EntryCount; ++E) {
        CompactUnwindRawCompressedEntry Entry;
        const uint64_t Offset = EntriesOffset + uint64_t(E) * 4;
        if (!R.u32At(Offset, Entry.PackedValue))
          return rawParseError("compressed page entry is truncated");

        const uint32_t Delta = Entry.PackedValue & 0x00ffffffu;
        if (Delta >
            std::numeric_limits<uint32_t>::max() - PageIndex.FunctionOffset)
          return rawParseError("compressed function delta overflows");
        Entry.FunctionOffset = PageIndex.FunctionOffset + Delta;
        Entry.EncodingIndex = Entry.PackedValue >> 24;
        if (Entry.EncodingIndex < Result.CommonEncodings.size()) {
          Entry.Encoding = Result.CommonEncodings[Entry.EncodingIndex];
        } else {
          const uint64_t LocalIndex =
              uint64_t(Entry.EncodingIndex) - Result.CommonEncodings.size();
          if (LocalIndex >= Page.LocalEncodings.size())
            return rawParseError("compressed encoding index is out of range");
          Entry.Encoding = Page.LocalEncodings[LocalIndex];
        }

        if (Entry.FunctionOffset < PageIndex.FunctionOffset ||
            Entry.FunctionOffset >= Result.Index[I + 1].FunctionOffset)
          return rawParseError("compressed entry lies outside its page range");
        if (E == 0 && Entry.FunctionOffset != PageIndex.FunctionOffset)
          return rawParseError(
              "compressed page does not begin at its index key");
        if (E != 0 && Entry.FunctionOffset <= PreviousFunction)
          return rawParseError(
              "compressed entries are duplicated or not strictly sorted");
        PreviousFunction = Entry.FunctionOffset;
        Page.CompressedEntries.push_back(Entry);
      }
    }

    uint64_t PageEnd = HeaderRange.End;
    PageEnd = std::max(PageEnd, EntriesRange.End);
    PageEnd = std::max(PageEnd, EncodingsRange.End);
    RawByteRange PageRange;
    if (llvm::Error Error = addRawRange(
            GlobalRanges, Page.SectionOffset, PageEnd - Page.SectionOffset, 1,
            SectionBytes.size(), "second-level page", PageRange))
      return std::move(Error);
    Result.Pages.push_back(std::move(Page));
  }

  // Structural validity alone is insufficient for rewrite: the encoding word
  // and the side tables jointly define personality and LSDA behavior.  Check
  // the bidirectional relationship before exposing a model a writer could
  // otherwise reproduce as well-formed but unusable exception metadata.
  std::map<uint32_t, uint32_t> EncodingByFunction;
  for (const CompactUnwindRawPage &Page : Result.Pages) {
    for (const CompactUnwindRawRegularEntry &Entry : Page.RegularEntries)
      if (!EncodingByFunction.emplace(Entry.FunctionOffset, Entry.Encoding)
               .second)
        return rawParseError("function appears in more than one unwind page");
    for (const CompactUnwindRawCompressedEntry &Entry : Page.CompressedEntries)
      if (!EncodingByFunction.emplace(Entry.FunctionOffset, Entry.Encoding)
               .second)
        return rawParseError("function appears in more than one unwind page");
  }

  std::map<uint32_t, uint32_t> LSDAByFunction;
  for (const CompactUnwindRawLSDAEntry &Entry : Result.LSDAEntries)
    if (!LSDAByFunction.emplace(Entry.FunctionOffset, Entry.LSDAOffset).second)
      return rawParseError("function has more than one LSDA entry");

  for (const auto &[FunctionOffset, Encoding] : EncodingByFunction) {
    const uint32_t PersonalityIndex =
        (Encoding & kPersonalityMask) >> kPersonalityShift;
    if (PersonalityIndex > Result.PersonalitySlotOffsets.size())
      return rawParseError(
          "encoding personality index leaves the personality array");
    const bool EncodingHasLSDA = (Encoding & kHasLSDA) != 0;
    const bool TableHasLSDA = LSDAByFunction.count(FunctionOffset) != 0;
    if (EncodingHasLSDA != TableHasLSDA)
      return rawParseError("encoding and LSDA index disagree for a function");
  }
  for (const auto &[FunctionOffset, LSDAOffset] : LSDAByFunction) {
    (void)LSDAOffset;
    if (EncodingByFunction.count(FunctionOffset) == 0)
      return rawParseError("LSDA index has no matching unwind entry");
  }

  return Result;
}

ParseResult parseCompactUnwind(const BinaryImage &Img) {
  ParseResult Result;

  const Section *Sec = Img.getSectionByName(section_names::macho::Unwind);
  if (!Sec)
    return Result;

  const uint8_t *Data = nullptr;
  size_t Size = 0;
  if (!Sec->Data.empty()) {
    Data = Sec->Data.data();
    Size = std::min<size_t>(Sec->Data.size(), static_cast<size_t>(Sec->Size));
  } else if (Sec->Size != 0) {
    Data = Img.readVA(Sec->VA, static_cast<size_t>(Sec->Size));
    Size = Data ? static_cast<size_t>(Sec->Size) : 0;
  }
  if (!Data || Size < 28)
    return Result;

  auto malformed = [&](const char *Message) {
    Result.ParseStatus = mergeExceptionParseStatus(
        Result.ParseStatus, ExceptionParseStatus::Malformed);
    Result.Diagnostics.emplace_back(Message);
    return Result;
  };
  auto partial = [&](const char *Message) {
    Result.ParseStatus = mergeExceptionParseStatus(
        Result.ParseStatus, ExceptionParseStatus::Partial);
    Result.Diagnostics.emplace_back(Message);
  };

  const SectionReader R(Data, Size, llvm::endianness::little);
  uint32_t Version = 0, CommonEncOffset = 0, CommonEncCount = 0;
  uint32_t PersonalityOffset = 0, PersonalityCount = 0;
  uint32_t IndexOffset = 0, IndexCount = 0;
  if (!R.u32At(0, Version) || !R.u32At(4, CommonEncOffset) ||
      !R.u32At(8, CommonEncCount) || !R.u32At(12, PersonalityOffset) ||
      !R.u32At(16, PersonalityCount) || !R.u32At(20, IndexOffset) ||
      !R.u32At(24, IndexCount))
    return malformed("truncated __unwind_info header");
  if (Version != kUnwindSectionVersion)
    return malformed("unsupported __unwind_info version");
  // The index always holds at least one real entry and one sentinel.
  if (IndexCount < 2)
    return malformed("__unwind_info index has no sentinel");
  if (CommonEncCount > kMaxEntries || PersonalityCount > kMaxEntries ||
      IndexCount > kMaxPages)
    return malformed("__unwind_info declares an implausible table size");

  const unsigned PtrSize = Img.getPointerSize();
  const va_t ImageBase = macho_loader::getMachHeaderVA(Img);

  // --- Personality array --------------------------------------------------
  for (uint32_t I = 0; I < PersonalityCount; ++I) {
    uint32_t SlotRVA = 0;
    if (!R.u32At(uint64_t(PersonalityOffset) + uint64_t(I) * 4, SlotRVA)) {
      partial("__unwind_info personality array leaves its section");
      break;
    }
    // The array holds the image-relative address of the pointer slot the
    // personality is loaded from, not the routine address itself.
    const va_t SlotVA = ImageBase + SlotRVA;
    va_t Routine = 0;
    if (const uint8_t *Slot = Img.readVA(SlotVA, PtrSize))
      Routine = PtrSize == 4 ? va_t(readLE<uint32_t>(Slot))
                             : va_t(readLE<uint64_t>(Slot));
    Result.Personalities.push_back(Routine);
    Result.PersonalitySlots.push_back(SlotVA);
  }

  // --- Common encodings ---------------------------------------------------
  std::vector<uint32_t> CommonEncodings;
  CommonEncodings.reserve(CommonEncCount);
  for (uint32_t I = 0; I < CommonEncCount; ++I) {
    uint32_t Encoding = 0;
    if (!R.u32At(uint64_t(CommonEncOffset) + uint64_t(I) * 4, Encoding)) {
      partial("__unwind_info common encoding array leaves its section");
      break;
    }
    CommonEncodings.push_back(Encoding);
  }

  // --- First-level index --------------------------------------------------
  struct IndexEntry {
    uint32_t FunctionOffset = 0;
    uint32_t SecondLevelOffset = 0;
    uint32_t LSDAArrayOffset = 0;
  };
  std::vector<IndexEntry> Index;
  Index.reserve(IndexCount);
  for (uint32_t I = 0; I < IndexCount; ++I) {
    const uint64_t Base = uint64_t(IndexOffset) + uint64_t(I) * 12;
    IndexEntry Entry;
    if (!R.u32At(Base, Entry.FunctionOffset) ||
        !R.u32At(Base + 4, Entry.SecondLevelOffset) ||
        !R.u32At(Base + 8, Entry.LSDAArrayOffset))
      return malformed("__unwind_info index leaves its section");
    Index.push_back(Entry);
  }

  // --- LSDA index ---------------------------------------------------------
  // The LSDA array is one global, function-ordered table; each first-level
  // entry names where its page's slice begins, and the sentinel's offset is
  // where the whole array ends.
  std::map<uint32_t, uint32_t> LSDAByFunction;
  {
    const uint32_t ArrayStart = Index.front().LSDAArrayOffset;
    const uint32_t ArrayEnd = Index.back().LSDAArrayOffset;
    if (ArrayEnd >= ArrayStart && ArrayEnd <= Size) {
      for (uint64_t Off = ArrayStart; Off + 8 <= ArrayEnd; Off += 8) {
        uint32_t FunctionOffset = 0, LSDAOffset = 0;
        if (!R.u32At(Off, FunctionOffset) || !R.u32At(Off + 4, LSDAOffset))
          break;
        LSDAByFunction[FunctionOffset] = LSDAOffset;
      }
    } else {
      partial("__unwind_info LSDA index is not a well-ordered array");
    }
  }

  // --- Second-level pages -------------------------------------------------
  // Collect (functionOffset, encoding) in address order; a range ends where
  // the next entry begins, and the last one ends at the sentinel.
  struct RawEntry {
    uint32_t FunctionOffset = 0;
    uint32_t Encoding = 0;
  };
  std::vector<RawEntry> Raw;

  for (size_t I = 0; I + 1 < Index.size(); ++I) {
    const IndexEntry &Page = Index[I];
    // A zero second-level offset marks the sentinel, which carries no page.
    if (Page.SecondLevelOffset == 0)
      continue;

    uint32_t Kind = 0;
    if (!R.u32At(Page.SecondLevelOffset, Kind)) {
      partial("__unwind_info second-level page leaves its section");
      continue;
    }
    uint16_t EntryPageOffset = 0, EntryCount = 0;
    if (!R.u16At(uint64_t(Page.SecondLevelOffset) + 4, EntryPageOffset) ||
        !R.u16At(uint64_t(Page.SecondLevelOffset) + 6, EntryCount)) {
      partial("__unwind_info second-level page header is truncated");
      continue;
    }
    if (Raw.size() + EntryCount > kMaxEntries) {
      partial("__unwind_info exceeds the entry decode budget");
      break;
    }

    if (Kind == kSecondLevelRegular) {
      for (uint32_t E = 0; E < EntryCount; ++E) {
        const uint64_t Off = uint64_t(Page.SecondLevelOffset) +
                             EntryPageOffset + uint64_t(E) * 8;
        RawEntry Entry;
        if (!R.u32At(Off, Entry.FunctionOffset) ||
            !R.u32At(Off + 4, Entry.Encoding)) {
          partial("__unwind_info regular page entry leaves its section");
          break;
        }
        Raw.push_back(Entry);
      }
    } else if (Kind == kSecondLevelCompressed) {
      uint16_t EncodingsPageOffset = 0, EncodingsCount = 0;
      if (!R.u16At(uint64_t(Page.SecondLevelOffset) + 8, EncodingsPageOffset) ||
          !R.u16At(uint64_t(Page.SecondLevelOffset) + 10, EncodingsCount)) {
        partial("__unwind_info compressed page header is truncated");
        continue;
      }
      for (uint32_t E = 0; E < EntryCount; ++E) {
        const uint64_t Off = uint64_t(Page.SecondLevelOffset) +
                             EntryPageOffset + uint64_t(E) * 4;
        uint32_t Packed = 0;
        if (!R.u32At(Off, Packed)) {
          partial("__unwind_info compressed page entry leaves its section");
          break;
        }
        RawEntry Entry;
        // A compressed entry stores its function offset relative to the page's
        // own base, which is the first-level index entry's function offset.
        Entry.FunctionOffset = Page.FunctionOffset + (Packed & 0x00ffffffu);
        const uint32_t EncodingIndex = Packed >> 24;
        if (EncodingIndex < CommonEncodings.size()) {
          Entry.Encoding = CommonEncodings[EncodingIndex];
        } else {
          const uint32_t Local = EncodingIndex - CommonEncCount;
          if (Local >= EncodingsCount ||
              !R.u32At(uint64_t(Page.SecondLevelOffset) + EncodingsPageOffset +
                           uint64_t(Local) * 4,
                       Entry.Encoding)) {
            partial("__unwind_info entry names an undecodable encoding");
            continue;
          }
        }
        Raw.push_back(Entry);
      }
    } else {
      partial("unknown __unwind_info second-level page kind");
    }
  }

  std::stable_sort(Raw.begin(), Raw.end(),
                   [](const RawEntry &A, const RawEntry &B) {
                     return A.FunctionOffset < B.FunctionOffset;
                   });

  const uint32_t SentinelOffset = Index.back().FunctionOffset;
  Result.Entries.reserve(Raw.size());
  for (size_t I = 0; I < Raw.size(); ++I) {
    const uint32_t Begin = Raw[I].FunctionOffset;
    const uint32_t End =
        I + 1 < Raw.size() ? Raw[I + 1].FunctionOffset : SentinelOffset;
    // The sentinel bounds the last range; a page boundary that repeats an
    // address would otherwise produce an empty one.
    if (End <= Begin)
      continue;

    CompactUnwindEntry Entry;
    Entry.CodeRange = {ImageBase + Begin, ImageBase + End};
    Entry.NativeEncoding = Raw[I].Encoding;
    if (!decodeEncoding(Img.Arch, Raw[I].Encoding, Entry)) {
      if (Entry.Kind != CompactUnwindKind::Unknown)
        partial("__unwind_info entry requires register-layout facts that its "
                "encoding alone cannot prove");
      else
        partial("__unwind_info entry uses an invalid or reserved encoding");
    }

    if (Entry.Kind == CompactUnwindKind::FramelessIndirect) {
      Entry.HasStackSize = resolveIndirectStackSize(
          Img, Raw[I].Encoding, Entry.CodeRange, Entry.StackSize);
      if (!Entry.HasStackSize)
        partial("__unwind_info entry points its frame size at bytes the image "
                "does not hold");
    }

    const uint32_t PersonalityIndex =
        (Raw[I].Encoding & kPersonalityMask) >> kPersonalityShift;
    if (PersonalityIndex != 0) {
      if (PersonalityIndex <= Result.Personalities.size())
        Entry.PersonalityVA = Result.Personalities[PersonalityIndex - 1];
      else
        partial("__unwind_info entry names a missing personality");
    }

    if (Raw[I].Encoding & kHasLSDA) {
      Entry.HasLSDA = true;
      auto It = LSDAByFunction.find(Begin);
      if (It != LSDAByFunction.end())
        Entry.LSDAVA = ImageBase + It->second;
      else
        partial("__unwind_info entry declares an LSDA the index does not hold");
    }
    Result.Entries.push_back(std::move(Entry));
  }

  LLVM_DEBUG(llvm::dbgs() << "macho-unwind: decoded " << Result.Entries.size()
                          << " compact entries, " << Result.Personalities.size()
                          << " personalities\n");
  return Result;
}

} // namespace neverd::macho_unwind
