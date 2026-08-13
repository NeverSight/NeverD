//===- CompactUnwind.cpp - Darwin __unwind_info decoding ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/CompactUnwind.h"

#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <iterator>
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

//===----------------------------------------------------------------------===//
// Saved registers
//===----------------------------------------------------------------------===//

/// Machine register numbers for the six registers an x86 compact encoding can
/// name, indexed by the encoding's own one-based numbering.  That numbering
/// matches neither the machine's nor DWARF's — it is a private table in
/// `<mach-o/compact_unwind_encoding.h>` — so translating out of it takes a
/// table of its own.  Index zero stands for "no register".
constexpr uint16_t kX86_64RegisterNumbers[] = {0, 3, 12, 13, 14, 15, 5};
constexpr uint16_t kX86RegisterNumbers[] = {0, 3, 1, 2, 7, 6, 5};
static_assert(std::size(kX86_64RegisterNumbers) == kX86NamedRegisterCount + 1);
static_assert(std::size(kX86RegisterNumbers) == kX86NamedRegisterCount + 1);

// The 32- and 64-bit x86 encodings lay their fields out identically and differ
// only in which registers the slot numbers stand for, which is what lets one
// decoder serve both from the tables above.
static_assert(kX86_64RBPFrameRegistersMask == kX86EBPFrameRegistersMask);
static_assert(kX86_64FramelessRegCountMask == kX86FramelessRegCountMask);
static_assert(kX86_64FramelessRegPermutationMask ==
              kX86FramelessRegPermutationMask);

/// One ARM64 register pair and the bit that says the prologue saved it.  The
/// order is the order the encoding assigns the pairs their stack slots in, so
/// walking this table in order reproduces the frame layout.
struct ARM64RegisterPair {
  uint32_t Bit;
  UnwindRegisterClass RegisterClass;
  uint16_t First;
  uint16_t Second;
};
constexpr ARM64RegisterPair kARM64RegisterPairs[] = {
    {kARM64FrameX19X20Pair, UnwindRegisterClass::GeneralPurpose, 19, 20},
    {kARM64FrameX21X22Pair, UnwindRegisterClass::GeneralPurpose, 21, 22},
    {kARM64FrameX23X24Pair, UnwindRegisterClass::GeneralPurpose, 23, 24},
    {kARM64FrameX25X26Pair, UnwindRegisterClass::GeneralPurpose, 25, 26},
    {kARM64FrameX27X28Pair, UnwindRegisterClass::GeneralPurpose, 27, 28},
    {kARM64FrameD8D9Pair, UnwindRegisterClass::FloatingPoint, 8, 9},
    {kARM64FrameD10D11Pair, UnwindRegisterClass::FloatingPoint, 10, 11},
    {kARM64FrameD12D13Pair, UnwindRegisterClass::FloatingPoint, 12, 13},
    {kARM64FrameD14D15Pair, UnwindRegisterClass::FloatingPoint, 14, 15},
};

/// Append \p Register as the next slot and add it to its file's mask.
///
/// A mask holds 32 registers, which every number the tables above produce fits
/// inside; the bound is still checked so that widening one of them later
/// cannot shift a bit off the end and leave the slot list and the mask
/// disagreeing.
void addSlot(CompactUnwindEntry &Entry, UnwindRegisterClass Class,
             uint16_t Register) {
  CompactUnwindRegisterSlot Slot;
  Slot.RegisterClass = Class;
  Slot.Register = Register;
  Entry.SavedRegisterSlots.push_back(Slot);
  if (Register >= 32)
    return;
  if (Class == UnwindRegisterClass::FloatingPoint)
    Entry.SavedFPRMask |= uint32_t(1) << Register;
  else
    Entry.SavedGPRMask |= uint32_t(1) << Register;
}

void addEmptySlot(CompactUnwindEntry &Entry) {
  Entry.SavedRegisterSlots.emplace_back();
}

void clearRegisters(CompactUnwindEntry &Entry) {
  Entry.SavedRegisterSlots.clear();
  Entry.SavedGPRMask = 0;
  Entry.SavedFPRMask = 0;
}

/// Decode the five three-bit slots of an x86 frame-pointer encoding.
///
/// A slot holds the encoding's number for the register that came to rest
/// there, or zero for one the prologue skipped.  A skipped slot still occupies
/// its pointer of stack, so an empty one between two registers has to survive
/// into the slot list: dropping it would move every register above it one
/// pointer closer to the frame pointer.
bool decodeX86FrameRegisters(uint32_t Encoding, const uint16_t *Numbers,
                             CompactUnwindEntry &Entry) {
  uint32_t Slots = Encoding & kX86_64RBPFrameRegistersMask;
  bool Valid = true;
  for (unsigned I = 0; I < kX86FrameSlotCount;
       ++I, Slots >>= kX86FrameSlotBits) {
    const uint32_t Number = Slots & ((1u << kX86FrameSlotBits) - 1);
    if (Number == 0) {
      addEmptySlot(Entry);
      continue;
    }
    // The last of the six numbers names the frame pointer, which this mode has
    // already spent on the frame itself.  A word that claims it saved the
    // frame pointer into its own frame describes nothing an unwinder can walk.
    if (Number >= kX86NamedRegisterCount) {
      Valid = false;
      addEmptySlot(Entry);
      continue;
    }
    addSlot(Entry, UnwindRegisterClass::GeneralPurpose, Numbers[Number]);
  }
  // Trailing empty slots sit at and above the frame pointer, where the saved
  // frame pointer and the return address are; they are padding in the word
  // rather than positions in the frame.
  while (!Entry.SavedRegisterSlots.empty() &&
         Entry.SavedRegisterSlots.back().RegisterClass ==
             UnwindRegisterClass::None)
    Entry.SavedRegisterSlots.pop_back();
  return Valid;
}

/// Undo the permutation an x86 frameless encoding packs its register order
/// into.
///
/// The encoder renumbers each saved register by how many smaller ones precede
/// it, which turns the order into a Lehmer code, then folds that code into a
/// single variable-base number so six registers fit in ten bits.  Decoding
/// runs the fold backwards and then re-expands each digit into the register it
/// counted past.  The digit weights are not one plain factorial series because
/// the last register of a six-register set is determined by the other five, so
/// the register count picks the divisors.  Mirrors
/// `stepWithCompactEncodingFrameless` in `libunwind/src/CompactUnwinder.hpp`
/// and the `permute_encode` reference in
/// `libunwind/include/mach-o/compact_unwind_encoding.h`.
bool decodeX86FramelessRegisters(uint32_t Encoding, const uint16_t *Numbers,
                                 CompactUnwindEntry &Entry) {
  const uint32_t Count =
      (Encoding & kX86_64FramelessRegCountMask) >> kX86FramelessRegCountShift;
  if (Count > kX86NamedRegisterCount)
    return false;

  static constexpr uint32_t kDivisors[kX86NamedRegisterCount + 1]
                                     [kX86NamedRegisterCount] = {
                                         {},
                                         {1},
                                         {5, 1},
                                         {20, 4, 1},
                                         {60, 12, 3, 1},
                                         {120, 24, 6, 2, 1},
                                         {120, 24, 6, 2, 1},
                                     };
  // Six registers encode only five digits: whichever register the first five
  // digits did not claim is the sixth, so its digit is always zero.
  const unsigned Digits = Count == kX86NamedRegisterCount ? Count - 1 : Count;

  uint32_t Permutation = Encoding & kX86_64FramelessRegPermutationMask;
  uint32_t Renumbered[kX86NamedRegisterCount] = {};
  for (unsigned I = 0; I < Digits; ++I) {
    Renumbered[I] = Permutation / kDivisors[Count][I];
    Permutation -= Renumbered[I] * kDivisors[Count][I];
  }

  bool Used[kX86NamedRegisterCount + 1] = {};
  for (unsigned I = 0; I < Count; ++I) {
    // The digit counts how many still-unclaimed registers to skip, so walking
    // the unclaimed ones in order and stopping at that rank recovers it.
    unsigned Rank = 0;
    unsigned Chosen = 0;
    for (unsigned R = 1; R <= kX86NamedRegisterCount; ++R) {
      if (Used[R])
        continue;
      if (Rank == Renumbered[I]) {
        Chosen = R;
        break;
      }
      ++Rank;
    }
    // A digit larger than the number of registers still unclaimed cannot have
    // come from the encoder, and guessing past it would silently rename every
    // register after it.
    if (Chosen == 0)
      return false;
    Used[Chosen] = true;
    addSlot(Entry, UnwindRegisterClass::GeneralPurpose, Numbers[Chosen]);
  }
  return true;
}

/// Decode the nine ARM64 register-pair bits, which both the frame and the
/// frameless mode carry: a leaf that saves callee-saved registers without
/// establishing a frame pointer is still frameless, and LLVM's ARM64 backend
/// emits exactly that combination.
void decodeARM64Registers(uint32_t Encoding, CompactUnwindEntry &Entry) {
  for (const ARM64RegisterPair &Pair : kARM64RegisterPairs) {
    if ((Encoding & Pair.Bit) == 0)
      continue;
    addSlot(Entry, Pair.RegisterClass, Pair.First);
    addSlot(Entry, Pair.RegisterClass, Pair.Second);
  }
}

} // namespace

bool decodeEncoding(neverd::Arch Arch, uint32_t Encoding,
                    CompactUnwindEntry &Entry) {
  Entry.Kind = CompactUnwindKind::Unknown;
  Entry.StackSize = 0;
  Entry.HasStackSize = false;
  Entry.DwarfFDEOffset = 0;
  Entry.FrameOffset = 0;
  clearRegisters(Entry);

  const uint32_t Mode = Encoding & kModeMask;
  bool Valid = true;

  switch (Arch) {
  case Arch::X64:
    if (Mode == kX86_64ModeRBPFrame) {
      Entry.Kind = CompactUnwindKind::FramePointer;
      Entry.FrameOffset =
          ((Encoding & kX86_64RBPFrameOffsetMask) >> kX86FrameOffsetShift) * 8;
      Valid = decodeX86FrameRegisters(Encoding, kX86_64RegisterNumbers, Entry);
      break;
    }
    if (Mode == kX86_64ModeStackImmediate) {
      Entry.Kind = CompactUnwindKind::FramelessImmediate;
      Entry.StackSize =
          ((Encoding & kX86_64FramelessStackSizeMask) >> kX86FrameOffsetShift) *
          8;
      Entry.HasStackSize = true;
      Valid =
          decodeX86FramelessRegisters(Encoding, kX86_64RegisterNumbers, Entry);
      break;
    }
    if (Mode == kX86_64ModeStackIndirect) {
      Entry.Kind = CompactUnwindKind::FramelessIndirect;
      Valid =
          decodeX86FramelessRegisters(Encoding, kX86_64RegisterNumbers, Entry);
      break;
    }
    if (Mode == kX86_64ModeDwarf) {
      Entry.Kind = CompactUnwindKind::DwarfFDE;
      Entry.DwarfFDEOffset = Encoding & kDwarfSectionOffsetMask;
    }
    break;
  case Arch::X86:
    if (Mode == kX86ModeEBPFrame) {
      Entry.Kind = CompactUnwindKind::FramePointer;
      Entry.FrameOffset =
          ((Encoding & kX86EBPFrameOffsetMask) >> kX86FrameOffsetShift) * 4;
      Valid = decodeX86FrameRegisters(Encoding, kX86RegisterNumbers, Entry);
      break;
    }
    if (Mode == kX86ModeStackImmediate) {
      Entry.Kind = CompactUnwindKind::FramelessImmediate;
      Entry.StackSize =
          ((Encoding & kX86FramelessStackSizeMask) >> kX86FrameOffsetShift) * 4;
      Entry.HasStackSize = true;
      Valid = decodeX86FramelessRegisters(Encoding, kX86RegisterNumbers, Entry);
      break;
    }
    if (Mode == kX86ModeStackIndirect) {
      Entry.Kind = CompactUnwindKind::FramelessIndirect;
      Valid = decodeX86FramelessRegisters(Encoding, kX86RegisterNumbers, Entry);
      break;
    }
    if (Mode == kX86ModeDwarf) {
      Entry.Kind = CompactUnwindKind::DwarfFDE;
      Entry.DwarfFDEOffset = Encoding & kDwarfSectionOffsetMask;
    }
    break;
  case Arch::AArch64:
    if (Mode == kARM64ModeFrame) {
      Entry.Kind = CompactUnwindKind::FramePointer;
      decodeARM64Registers(Encoding, Entry);
      break;
    }
    if (Mode == kARM64ModeFrameless) {
      Entry.Kind = CompactUnwindKind::FramelessImmediate;
      Entry.StackSize = ((Encoding & kARM64FramelessStackSizeMask) >> 12) * 16;
      Entry.HasStackSize = true;
      decodeARM64Registers(Encoding, Entry);
      break;
    }
    if (Mode == kARM64ModeDwarf) {
      Entry.Kind = CompactUnwindKind::DwarfFDE;
      Entry.DwarfFDEOffset = Encoding & kDwarfSectionOffsetMask;
    }
    break;
  case Arch::ARM:
    // Apple's ARM32 compact encodings are not in the header this decoder is
    // written against, so the frame shape is all that can be named for them
    // without guessing at a register layout.
    if (Mode == kARMModeFrame || Mode == kARMModeFrameD)
      Entry.Kind = CompactUnwindKind::FramePointer;
    else if (Mode == kARMModeDwarf) {
      Entry.Kind = CompactUnwindKind::DwarfFDE;
      Entry.DwarfFDEOffset = Encoding & kDwarfSectionOffsetMask;
    }
    break;
  default:
    return true;
  }

  // Mode zero with no other bits set is how the linker spells "this range has
  // no unwind information", which is a fact about the range rather than a
  // decoding failure.
  if (Entry.Kind == CompactUnwindKind::Unknown && Mode == 0)
    Entry.Kind = CompactUnwindKind::None;

  if (!Valid)
    clearRegisters(Entry);
  return Valid;
}

bool resolveIndirectStackSize(const BinaryImage &Img, uint32_t Encoding,
                              const ExceptionAddressRange &CodeRange,
                              uint32_t &StackSize) {
  uint32_t OffsetMask = 0;
  uint32_t AdjustMask = 0;
  uint32_t AdjustScale = 0;
  switch (Img.Arch) {
  case Arch::X64:
    OffsetMask = kX86_64FramelessStackSizeMask;
    AdjustMask = kX86_64FramelessStackAdjustMask;
    AdjustScale = 8;
    break;
  case Arch::X86:
    OffsetMask = kX86FramelessStackSizeMask;
    AdjustMask = kX86FramelessStackAdjustMask;
    AdjustScale = 4;
    break;
  default:
    return false;
  }

  const uint32_t ImmediateOffset =
      (Encoding & OffsetMask) >> kX86FrameOffsetShift;
  // The offset names the immediate field of the prologue's stack-allocating
  // `sub`, so the whole field has to fall inside the function it belongs to;
  // a crafted offset that merely lands in a mapped page would otherwise read
  // some unrelated function's bytes as a frame size.
  if (!rangeInBounds(ImmediateOffset, sizeof(uint32_t), CodeRange.size()))
    return false;
  const uint8_t *Immediate =
      Img.readVA(CodeRange.Begin + ImmediateOffset, sizeof(uint32_t));
  if (!Immediate)
    return false;

  const uint32_t Adjust =
      ((Encoding & AdjustMask) >> kX86FramelessStackAdjustShift) * AdjustScale;
  StackSize = readLE<uint32_t>(Immediate) + Adjust;
  return true;
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
