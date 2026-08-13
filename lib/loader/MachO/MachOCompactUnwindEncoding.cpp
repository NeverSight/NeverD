//===- MachOCompactUnwindEncoding.cpp - Compact encoding words -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/support/BinaryEncoding.h"

#include <iterator>

namespace neverd::macho_unwind {
namespace {

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

} // namespace neverd::macho_unwind
