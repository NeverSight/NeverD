//===- ARMEHABIDetail.h - Private ARM EHABI decoding helpers ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation detail of the ARM EHABI reader in `lib/loader/ELF`.  The
/// constants, record shapes, and helpers declared here are shared between the
/// translation units that make up that reader and must not be included from
/// outside this directory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_ELF_ARMEHABIDETAIL_H
#define NEVERD_LIB_LOADER_ELF_ARMEHABIDETAIL_H

#include "neverd/loader/ELF/ARMEHABI.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Compiler.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::arm_ehabi {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail {

/// Every index entry is two words: a function address and its description.
inline constexpr uint64_t kIndexEntrySize = 8;

/// The reserved description meaning "this frame may not be unwound through".
inline constexpr uint32_t kCantUnwind = 1;

/// Set in a description word that carries its own descriptor rather than a
/// displacement to one.
inline constexpr uint32_t kCompactBit = 0x80000000u;

/// Bits 30-28 of a compact word select the vendor that defined the remaining
/// bits.  Only zero -- ARM's own -- has ever been defined.
inline constexpr uint32_t kCompactVendorMask = 0x70000000u;

/// Bits 27-24 of a compact word: which personality routine it selects.
inline constexpr uint32_t kCompactIndexShift = 24;
inline constexpr uint32_t kCompactIndexMask = 0xF;

/// Bits 23-16 hold the count of opcode words past the first, for the two
/// personality routines whose descriptors can need more than one word.
inline constexpr uint32_t kExtraWordShift = 16;
inline constexpr uint32_t kGenericExtraWordShift = 24;
inline constexpr uint32_t kExtraWordMask = 0xFF;

/// Ceiling on how far one entry's opcodes may run.  The count is eight bits,
/// so 255 extra words is what the encoding itself permits; stating it keeps
/// the arithmetic below obviously bounded rather than only provably so.
inline constexpr uint64_t kMaxEntryBytes = (2 + 255) * 4;

/// Ceiling on index entries read from one image.  A `.ARM.exidx` covers every
/// function, and the largest shipped ARM32 objects are far below this.
inline constexpr size_t kMaxIndexEntries = 1u << 22;

/// ARM register numbers the opcodes name.
inline constexpr uint16_t kFirstPoppedReg = 4;  // r4
inline constexpr uint16_t kLinkRegister = 14;   // r14, spelled `lr`
inline constexpr uint16_t kProgramCounter = 15; // r15, spelled `pc`

/// A slot is one word on this target, in every table EHABI defines.
inline constexpr uint64_t kWordSize = 4;

/// A section of an image, located by name and read through whichever of its
/// bytes the loader kept.
struct TableSection {
  const uint8_t *Data = nullptr;
  size_t Size = 0;
  va_t VA = 0;
};

/// What an `.ARM.extab` entry declared, before its language data is read.
struct TableEntry {
  ARMEHABIEntryKind Kind = ARMEHABIEntryKind::Generic;
  std::optional<uint8_t> PersonalityIndex;
  va_t PersonalityVA = 0;
  uint32_t ExtraWordCount = 0;
  /// Unwind opcodes, in the order the unwinder executes them.
  std::vector<uint8_t> Opcodes;
  /// First byte past the opcodes, which is where the personality routine's
  /// own data begins.
  va_t HandlerDataVA = 0;
};

/// Resolve a `prel31` field: a 31-bit signed displacement from the address of
/// the word that holds it.  The result is taken modulo the address size, as
/// the ABI defines it and as every unwinder computes it.
va_t resolvePrel31(uint32_t Word, va_t FieldVA);

/// Every `.ARM.exidx` in the image, in address order.
///
/// There is usually one, but a link that was told to keep per-function
/// sections leaves a run of them; each is internally sorted and they are laid
/// out in address order, so reading them in that order yields one index.
///
/// The section type is checked alongside the name because ARM gave the index a
/// type of its own.  A name is a convention and an index under an unexpected
/// one is still an index; the type is what the ABI reserved.
std::vector<TableSection> findIndexSections(const BinaryImage &Img);

/// Where the code an index covers stops.
///
/// An index entry states only where its function begins; the next entry's
/// function is where it ends.  The last entry has no next, so its extent is
/// bounded by the executable section it starts in -- which is what the
/// unwinder does too, having nothing else to go on.
va_t executableEndFor(const BinaryImage &Img, va_t Address);

/// Decode the EHABI unwind opcode stream in \p Bytes.
///
/// The opcodes describe unwinding -- restoring the caller's frame -- while
/// \ref UnwindOperation is defined in the saving direction, so a `pop` here
/// becomes a save and `vsp = vsp + N` becomes the allocation the prologue made
/// to put it there.  Both describe one frame; stating them the same way as
/// every other target is what lets a consumer read them without knowing which
/// table they came out of.
///
/// Returns false when the last instruction was cut short, which means the
/// entry's declared word count and its opcodes disagree.  Running out exactly
/// on an instruction boundary is not that: the encoding pads with `finish`
/// only where a word has slack left, so a sequence that fills its words ends
/// without one and is well formed.
bool decodeUnwindOpcodes(llvm::ArrayRef<uint8_t> Bytes,
                         std::vector<UnwindOperation> &Out,
                         bool &RefusesToUnwind);

/// The three readings EHABI's `R_ARM_TARGET2` can have been linked to mean,
/// as a DWARF encoding this decoder can hand to the LSDA reader.
uint8_t encodingFor(ARMTypeTableConvention Convention);

/// Decide which reading of \p Info's type-table slots the image was linked
/// with, from the slots themselves.
///
/// EHABI leaves this to the platform and the C++ runtime resolves it without
/// consulting the LSDA header, so the header byte is not evidence.  What is
/// evidence is where each reading lands: exactly one of them reaches an object
/// that looks like RTTI, and the other two reach code or a pointer cell.
///
/// Returns \ref ARMTypeTableConvention::Unknown when no slot settled it, which
/// leaves the caller to keep looking at later records rather than commit the
/// whole image to a guess made from one.
ARMTypeTableConvention proveTypeTableConvention(const BinaryImage &Img,
                                                const ItaniumEHInfo &Info);

/// Split the three or two opcode bytes a descriptor's first word carries.
void appendWordOpcodes(std::vector<uint8_t> &Out, uint32_t Word,
                       unsigned Count);

/// Read the `.ARM.extab` entry at \p TableVA.
///
/// The first word decides everything else: its top bit chooses between a
/// personality named by index and one named by address, and the fields that
/// follow differ between the two.  Returns false when the entry could not be
/// read or declares a shape the ABI does not define.
bool decodeTableEntry(const BinaryImage &Img, va_t TableVA, TableEntry &Out,
                      std::string &Diagnostic);

/// True when a generic-model entry's personality reads its handler data as an
/// Itanium LSDA.
///
/// The three ARM-defined routines do not: they take the scope descriptors
/// EHABI defines for them, which carry no type and stop nothing.  Everything
/// else on this target is a language personality that shares the Itanium
/// language-specific data area, including one this decoder cannot name -- a
/// static link leaves the routine unnamed, and refusing to read the table
/// because of that would lose the handlers of every stripped static binary.
bool readsAnItaniumLSDA(ExceptionPersonality Personality);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail
} // namespace neverd::arm_ehabi

#endif // NEVERD_LIB_LOADER_ELF_ARMEHABIDETAIL_H
