//===- LanguageEHCompact.h - Darwin compact unwind ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalized Darwin `__unwind_info` compact-unwind entries: the frame shape
/// the encoding names, and the saved-register slots it describes in the
/// target's own register numbering.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEHCOMPACT_H
#define NEVERD_LOADER_LANGUAGEEHCOMPACT_H

#include "neverd/loader/ExceptionCommon.h"

#include <cstdint>
#include <vector>

namespace neverd {

/// \ref UnwindRegisterClass is declared beside the Windows unwind operations
/// in `ExceptionInfo.h`, which includes this header, so from here it can only
/// be named opaquely.  That is still worth more than forking a second register
/// file vocabulary for Darwin: a consumer that already knows how to read one
/// architecture's saved registers should not have to learn a second spelling
/// of "this number belongs to the floating-point file".
enum class UnwindRegisterClass : uint8_t;

/// The frame shape a compact-unwind entry encodes.  The concrete meaning of
/// the mode bits is architecture specific, so the normalized kind is what
/// consumers use and `NativeEncoding` retains the exact word.
enum class CompactUnwindKind : uint8_t {
  /// No unwind information: the entry exists only to terminate a range.
  None,
  /// Frame-pointer based; saved registers are at negative offsets from the
  /// frame pointer.
  FramePointer,
  /// Frameless with an immediate stack size.
  FramelessImmediate,
  /// Frameless whose stack size is read from the function's `sub` immediate.
  FramelessIndirect,
  /// Defers to a DWARF FDE for this range.
  DwarfFDE,
  Unknown,
};

const char *getCompactUnwindKindName(CompactUnwindKind Kind);

/// One slot in the run of saved-register slots a compact encoding describes.
struct CompactUnwindRegisterSlot {
  /// Value-initializes to `UnwindRegisterClass::None`, which marks a slot the
  /// encoding reserved and left empty.
  UnwindRegisterClass RegisterClass{};
  /// Register number in the target's own numbering rather than the compact
  /// encoding's private one-to-six table, so `rbx` is 3, `r12` is 12, `x19` is
  /// 19 and `d8` is 8.  A number therefore means the same physical register
  /// here as it does in a Windows unwind operation for the same machine.
  uint16_t Register = 0;
};

struct CompactUnwindEntry {
  ExceptionAddressRange CodeRange;
  uint32_t NativeEncoding = 0;
  CompactUnwindKind Kind = CompactUnwindKind::Unknown;
  /// Byte size of the frame for the frameless forms.
  uint32_t StackSize = 0;
  /// True when \ref StackSize is a size the entry actually established.  The
  /// frameless-indirect form keeps its size in the function's own prologue, so
  /// an entry whose prologue could not be read leaves this false instead of
  /// letting a zero pass for a frame that allocates nothing.
  bool HasStackSize = false;
  /// Section offset of the DWARF FDE for `DwarfFDE` entries.
  uint32_t DwarfFDEOffset = 0;
  va_t PersonalityVA = 0;
  va_t LSDAVA = 0;
  bool HasLSDA = false;

  /// The saved-register slots in the order the encoding lists them, which is
  /// the order each architecture's unwinder restores them in: the x86 forms
  /// run from the lowest-addressed slot upward, the ARM64 forms in register
  /// number order, which runs downward in memory.
  std::vector<CompactUnwindRegisterSlot> SavedRegisterSlots;
  /// Every general-purpose register the slots name, as a bitmask over the same
  /// numbering \ref CompactUnwindRegisterSlot::Register uses.  The frame
  /// pointer and return address that a frame form saves are absent: no bit of
  /// the encoding names them, \ref Kind already implies them, and a mask that
  /// invented them could not be encoded back into a word.
  uint32_t SavedGPRMask = 0;
  /// Every floating-point register the slots name.  Only the ARM64 encodings
  /// can name one.
  uint32_t SavedFPRMask = 0;
  /// Distance in bytes from the frame pointer down to slot zero, for the x86
  /// frame forms: slot *i* sits at `fp - FrameOffset + i * <pointer size>`.
  /// The ARM64 frame form fixes that distance at one pointer and spends no
  /// field on it, so it leaves this zero.
  uint32_t FrameOffset = 0;
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEHCOMPACT_H
