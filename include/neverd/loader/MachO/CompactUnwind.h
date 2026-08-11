//===- CompactUnwind.h - Darwin __unwind_info decoding --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decodes the Darwin `__unwind_info` section: the linker-built, two-level
/// lookup table that replaces `__eh_frame` for the overwhelming majority of
/// Mach-O functions.
///
/// A Darwin binary usually has *no* FDE for a function whose frame shape fits
/// one of the compact encodings, so reading only `__eh_frame` finds almost
/// nothing.  The compact entry is also where the personality index and the
/// LSDA pointer live, which makes this section — not the DWARF one — the
/// entry point to Itanium exception recovery on Darwin.
///
/// The format is Apple's `<mach-o/compact_unwind_encoding.h>`.  LLVM does not
/// export it from a public header, so the layout is restated here.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_COMPACTUNWIND_H
#define NEVERD_LOADER_MACHO_COMPACTUNWIND_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionInfo.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::macho_unwind {

/// `__unwind_info` section header.  Every `*SectionOffset` field is an offset
/// from the start of the section; every `functionOffset` and `lsdaOffset` is
/// an offset from the Mach-O header, i.e. from the image base.
constexpr uint32_t kUnwindSectionVersion = 1;
constexpr uint32_t kSecondLevelRegular = 2;
constexpr uint32_t kSecondLevelCompressed = 3;

/// Flags shared by every architecture's compact encoding.
constexpr uint32_t kIsNotFunctionStart = 0x80000000u;
constexpr uint32_t kHasLSDA = 0x40000000u;
constexpr uint32_t kPersonalityMask = 0x30000000u;
constexpr unsigned kPersonalityShift = 28;

/// Per-architecture mode nibble.  The nibble position is shared; only the
/// meanings of the values differ.
constexpr uint32_t kModeMask = 0x0f000000u;
constexpr uint32_t kDwarfSectionOffsetMask = 0x00ffffffu;

constexpr uint32_t kX86_64ModeRBPFrame = 0x01000000u;
constexpr uint32_t kX86_64ModeStackImmediate = 0x02000000u;
constexpr uint32_t kX86_64ModeStackIndirect = 0x03000000u;
constexpr uint32_t kX86_64ModeDwarf = 0x04000000u;
constexpr uint32_t kX86_64RBPFrameRegistersMask = 0x00007fffu;
constexpr uint32_t kX86_64RBPFrameOffsetMask = 0x00ff0000u;
/// Doubles as the offset of the `sub` immediate in the indirect mode, which is
/// what makes that mode indirect: the field names where the size is, not what
/// it is.
constexpr uint32_t kX86_64FramelessStackSizeMask = 0x00ff0000u;
constexpr uint32_t kX86_64FramelessStackAdjustMask = 0x0000e000u;
constexpr uint32_t kX86_64FramelessRegCountMask = 0x00001c00u;
constexpr uint32_t kX86_64FramelessRegPermutationMask = 0x000003ffu;

constexpr uint32_t kX86ModeEBPFrame = 0x01000000u;
constexpr uint32_t kX86ModeStackImmediate = 0x02000000u;
constexpr uint32_t kX86ModeStackIndirect = 0x03000000u;
constexpr uint32_t kX86ModeDwarf = 0x04000000u;
constexpr uint32_t kX86EBPFrameRegistersMask = 0x00007fffu;
constexpr uint32_t kX86EBPFrameOffsetMask = 0x00ff0000u;
constexpr uint32_t kX86FramelessStackSizeMask = 0x00ff0000u;
constexpr uint32_t kX86FramelessStackAdjustMask = 0x0000e000u;
constexpr uint32_t kX86FramelessRegCountMask = 0x00001c00u;
constexpr uint32_t kX86FramelessRegPermutationMask = 0x000003ffu;

/// Shift and slot counts shared by both x86 encodings, whose layouts differ
/// only in which registers the slot numbers stand for.
constexpr unsigned kX86FrameOffsetShift = 16;
constexpr unsigned kX86FramelessStackAdjustShift = 13;
constexpr unsigned kX86FramelessRegCountShift = 10;
constexpr unsigned kX86FrameSlotCount = 5;
constexpr unsigned kX86FrameSlotBits = 3;
/// The x86 encodings can name six registers, numbered from one.
constexpr unsigned kX86NamedRegisterCount = 6;

constexpr uint32_t kARM64ModeFrameless = 0x02000000u;
constexpr uint32_t kARM64ModeDwarf = 0x03000000u;
constexpr uint32_t kARM64ModeFrame = 0x04000000u;
constexpr uint32_t kARM64FramelessStackSizeMask = 0x00fff000u;
constexpr uint32_t kARM64FrameX19X20Pair = 0x00000001u;
constexpr uint32_t kARM64FrameX21X22Pair = 0x00000002u;
constexpr uint32_t kARM64FrameX23X24Pair = 0x00000004u;
constexpr uint32_t kARM64FrameX25X26Pair = 0x00000008u;
constexpr uint32_t kARM64FrameX27X28Pair = 0x00000010u;
constexpr uint32_t kARM64FrameD8D9Pair = 0x00000100u;
constexpr uint32_t kARM64FrameD10D11Pair = 0x00000200u;
constexpr uint32_t kARM64FrameD12D13Pair = 0x00000400u;
constexpr uint32_t kARM64FrameD14D15Pair = 0x00000800u;

constexpr uint32_t kARMModeFrame = 0x01000000u;
constexpr uint32_t kARMModeFrameD = 0x02000000u;
constexpr uint32_t kARMModeDwarf = 0x04000000u;

struct ParseResult {
  std::vector<CompactUnwindEntry> Entries;
  /// Personality routine addresses, in the order the section listed them.  An
  /// entry's `PersonalityMask` field indexes this list from one.
  std::vector<va_t> Personalities;
  /// Address of the slot each personality was loaded through, so a
  /// dynamically bound routine can still be named.
  std::vector<va_t> PersonalitySlots;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;
  std::vector<std::string> Diagnostics;
};

/// Decode the image's `__unwind_info` section.
ParseResult parseCompactUnwind(const BinaryImage &Img);

/// Decode into \p Entry everything one compact encoding word states on its
/// own, which is every field but the frameless-indirect stack size: that one
/// lives in the function's prologue rather than in the word.
///
/// Returns false when the word names a register its own mode cannot hold, in
/// which case the register set is reported as empty rather than as a partly
/// recovered guess.  Nothing an assembler emits does this, so a false return
/// means the word is corrupt or was decoded for the wrong architecture.
bool decodeEncoding(neverd::Arch Arch, uint32_t Encoding,
                    CompactUnwindEntry &Entry);

/// Resolve the frame size a `FramelessIndirect` entry defers to the
/// `sub $imm32, %rsp` in its own prologue.
///
/// Returns false when \p CodeRange does not contain the whole immediate or the
/// image has no bytes mapped there, which leaves the size unknown; a caller
/// must not substitute zero, because a frameless-indirect function is by
/// construction one whose frame was too large to encode inline.
bool resolveIndirectStackSize(const BinaryImage &Img, uint32_t Encoding,
                              const ExceptionAddressRange &CodeRange,
                              uint32_t &StackSize);

} // namespace neverd::macho_unwind

#endif // NEVERD_LOADER_MACHO_COMPACTUNWIND_H
