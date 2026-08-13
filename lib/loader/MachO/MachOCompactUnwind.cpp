//===- MachOCompactUnwind.cpp - Darwin __unwind_info decoding ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>

#define DEBUG_TYPE "neverd-macho-unwind"

namespace neverd::macho_unwind {
namespace {

/// Upper bound on entries decoded from one section.  A real program has one
/// entry per function; a crafted count field can claim far more than the
/// section has bytes for, and the per-read bounds checks would then simply
/// run for a very long time.
constexpr uint32_t kMaxEntries = 1u << 22;
constexpr uint32_t kMaxPages = 1u << 16;

/// A bounded view over the section, so every field read is checked once here
/// instead of at each use.
class SectionReader {
public:
  SectionReader(const uint8_t *Data, size_t Size) : Data(Data), Size(Size) {}

  bool u32At(uint64_t Offset, uint32_t &Out) const {
    if (!rangeInBounds(Offset, 4, Size))
      return false;
    Out = readLE<uint32_t>(Data + Offset);
    return true;
  }
  bool u16At(uint64_t Offset, uint16_t &Out) const {
    if (!rangeInBounds(Offset, 2, Size))
      return false;
    Out = readLE<uint16_t>(Data + Offset);
    return true;
  }

private:
  const uint8_t *Data;
  size_t Size;
};

} // namespace

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

  const SectionReader R(Data, Size);
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
    if (!decodeEncoding(Img.Arch, Raw[I].Encoding, Entry))
      partial("__unwind_info entry saves a register its own mode cannot hold");

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
