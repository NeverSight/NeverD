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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <limits>
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
constexpr uint32_t kARMFrameStackAdjustMask = 0x00c00000u;
constexpr unsigned kARMFrameStackAdjustShift = 22;
constexpr uint32_t kARMFrameFirstPushR4 = 0x00000001u;
constexpr uint32_t kARMFrameFirstPushR5 = 0x00000002u;
constexpr uint32_t kARMFrameFirstPushR6 = 0x00000004u;
constexpr uint32_t kARMFrameFirstPushMask = 0x00000007u;
constexpr uint32_t kARMFrameSecondPushR8 = 0x00000008u;
constexpr uint32_t kARMFrameSecondPushR9 = 0x00000010u;
constexpr uint32_t kARMFrameSecondPushR10 = 0x00000020u;
constexpr uint32_t kARMFrameSecondPushR11 = 0x00000040u;
constexpr uint32_t kARMFrameSecondPushR12 = 0x00000080u;
constexpr uint32_t kARMFrameSecondPushMask = 0x000000f8u;
/// Only bits 8--10 select an ARM32 D-register pattern.  Bit 11 is reserved;
/// accepting it would alias an unproven layout onto one of the eight defined
/// patterns.
constexpr uint32_t kARMFrameDRegisterCountMask = 0x00000700u;
constexpr unsigned kARMFrameDRegisterCountShift = 8;

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

/// Lossless representation of the linker-built `__unwind_info` bytes.  Unlike
/// \ref ParseResult, these records retain section-relative offsets, page kinds,
/// packed compressed words, and source ordering so a writer can rebuild the
/// table without first normalizing away malformed or ambiguous structure.
struct CompactUnwindRawHeader {
  uint32_t Version = 0;
  uint32_t CommonEncodingsSectionOffset = 0;
  uint32_t CommonEncodingsCount = 0;
  uint32_t PersonalityArraySectionOffset = 0;
  uint32_t PersonalityArrayCount = 0;
  uint32_t IndexSectionOffset = 0;
  uint32_t IndexCount = 0;

  bool operator==(const CompactUnwindRawHeader &) const = default;
};

struct CompactUnwindRawIndexEntry {
  uint32_t FunctionOffset = 0;
  uint32_t SecondLevelPageSectionOffset = 0;
  uint32_t LSDAIndexArraySectionOffset = 0;

  bool operator==(const CompactUnwindRawIndexEntry &) const = default;
};

struct CompactUnwindRawLSDAEntry {
  uint32_t FunctionOffset = 0;
  uint32_t LSDAOffset = 0;

  bool operator==(const CompactUnwindRawLSDAEntry &) const = default;
};

struct CompactUnwindRawRegularEntry {
  uint32_t FunctionOffset = 0;
  uint32_t Encoding = 0;

  bool operator==(const CompactUnwindRawRegularEntry &) const = default;
};

struct CompactUnwindRawCompressedEntry {
  /// Exact word stored by the linker: high byte encoding index, low 24 bits
  /// function delta.
  uint32_t PackedValue = 0;
  uint32_t FunctionOffset = 0;
  uint32_t EncodingIndex = 0;
  uint32_t Encoding = 0;

  bool operator==(const CompactUnwindRawCompressedEntry &) const = default;
};

struct CompactUnwindRawPage {
  uint32_t SectionOffset = 0;
  uint32_t Kind = 0;
  uint16_t EntryPageOffset = 0;
  uint16_t EntryCount = 0;
  uint16_t EncodingsPageOffset = 0;
  uint16_t EncodingsCount = 0;
  std::vector<uint32_t> LocalEncodings;
  std::vector<CompactUnwindRawRegularEntry> RegularEntries;
  std::vector<CompactUnwindRawCompressedEntry> CompressedEntries;

  bool operator==(const CompactUnwindRawPage &) const = default;
};

struct CompactUnwindRawSection {
  /// Exact source bytes, including padding and gaps between structured
  /// regions.  A failed or no-op rewrite can therefore preserve the section
  /// byte-for-byte instead of synthesizing unobserved padding.
  std::vector<uint8_t> OriginalBytes;
  CompactUnwindRawHeader Header;
  std::vector<uint32_t> CommonEncodings;
  /// Image-relative pointer-slot offsets, retained exactly as stored.
  std::vector<uint32_t> PersonalitySlotOffsets;
  /// Includes the one terminal sentinel as its final entry.
  std::vector<CompactUnwindRawIndexEntry> Index;
  std::vector<CompactUnwindRawLSDAEntry> LSDAEntries;
  std::vector<CompactUnwindRawPage> Pages;

  bool operator==(const CompactUnwindRawSection &) const = default;
};

/// Explicit decoding policy for the strict rewrite parser.  The default has
/// no caller-imposed entry or page ceiling: the section's checked byte ranges
/// remain the authoritative structural bound.  Callers handling untrusted
/// inputs may set either ceiling without changing format semantics.
struct CompactUnwindRawParseOptions {
  llvm::endianness ByteOrder = llvm::endianness::little;
  uint64_t MaxEntries = std::numeric_limits<uint64_t>::max();
  uint64_t MaxPages = std::numeric_limits<uint64_t>::max();
};

/// Strictly decode linker structure for rewrite.  Any ambiguity that a reader
/// could tolerate but a writer could not reproduce safely is an error: tables
/// and pages must be bounded and disjoint, index/LSDA/function keys must retain
/// strict source order, the final first-level entry must be the unique
/// sentinel, and every compressed reference must resolve without arithmetic
/// overflow.  Byte order and optional caller resource ceilings are explicit.
/// Input is never sorted or repaired before validation.
llvm::Expected<CompactUnwindRawSection> parseCompactUnwindRaw(
    llvm::ArrayRef<uint8_t> SectionBytes,
    CompactUnwindRawParseOptions Options = CompactUnwindRawParseOptions{});

/// Decode the image's `__unwind_info` section.
ParseResult parseCompactUnwind(const BinaryImage &Img);

/// Decode into \p Entry everything one compact encoding word states on its
/// own, which is every field but the frameless-indirect stack size: that one
/// lives in the function's prologue rather than in the word.
///
/// Returns false when the word is corrupt, uses reserved bits, or requires a
/// runtime-dependent layout that the word alone cannot prove.  Corrupt words
/// retain no inferred frame facts.  A runtime-dependent word instead sets
/// `SemanticStatus` to `Partial` and retains only architecture-defined facts;
/// consumers may inspect those facts, but must reject the entry for rewrite.
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
