//===- LanguageEHARM.h - ARM EHABI unwinding tables -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalized ARM EHABI provenance: which of the four shapes an `.ARM.exidx`
/// entry took, and what an image's `R_ARM_TARGET2` relocations were linked to
/// mean, which is the whole of how an EHABI type-table slot is read.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEHARM_H
#define NEVERD_LOADER_LANGUAGEEHARM_H

#include "neverd/Common.h"

#include <cstdint>
#include <optional>

namespace neverd {

/// Which of the four shapes an `.ARM.exidx` entry takes.
///
/// The index is a sorted array of eight-byte entries covering every function
/// in the image, and its second word is a discriminant: one reserved value
/// means the frame refuses to unwind, a set top bit means the whole
/// description fits in the word, and anything else is a self-relative offset
/// into `.ARM.extab`.  Which shape an entry took decides where -- and whether
/// -- a handler can be found, so it is kept rather than flattened away.
enum class ARMEHABIEntryKind : uint8_t {
  /// `EXIDX_CANTUNWIND`.  The frame may not be unwound through: an exception
  /// reaching it terminates instead of propagating.
  CantUnwind,
  /// The index word itself holds the descriptor, using ARM-defined
  /// personality routine 0.  Three bytes leave no room for language data, so
  /// such a frame has a handler only in the sense that it can be stepped over.
  InlineCompact,
  /// An `.ARM.extab` entry naming one of the three ARM-defined personality
  /// routines by index rather than by address.
  Compact,
  /// An `.ARM.extab` entry naming its personality routine outright, which is
  /// what a frame with C++ language data uses.
  Generic,
};

const char *getARMEHABIEntryKindName(ARMEHABIEntryKind Kind);

/// What an image's `R_ARM_TARGET2` relocations were linked to mean, which is
/// the whole of how an EHABI type-table slot is read.
///
/// EHABI leaves the type table's pointer encoding to the platform: the C++
/// runtime resolves a slot through `_Unwind_decode_typeinfo_ptr` and never
/// consults the encoding byte in the LSDA header.  GCC writes the platform's
/// answer into that byte anyway; Clang leaves it as a bare `DW_EH_PE_absptr`.
/// A reader therefore cannot take the byte at face value, and the convention
/// is a property of the image rather than of any one record.
enum class ARMTypeTableConvention : uint8_t {
  /// Nothing in the image settled it, because no record named a type.
  Unknown,
  /// The slot holds the `std::type_info *`.  Bare-metal and Symbian.
  Absolute,
  /// The slot holds a self-relative displacement to the pointer.
  PCRelative,
  /// The slot holds a self-relative displacement to a cell holding the
  /// pointer, which is what `R_ARM_TARGET2` means on Linux and the BSDs.
  PCRelativeIndirect,
};

const char *getARMTypeTableConventionName(ARMTypeTableConvention Convention);

/// One `.ARM.exidx` entry and the `.ARM.extab` entry it reached, if any.
///
/// The unwind opcodes themselves normalize into
/// \ref ExceptionFunction::UnwindOperations like every other target's, and
/// the language data into \ref ExceptionFunction::Itanium.  What stays here is
/// the EHABI-specific provenance a consumer would otherwise have to re-derive
/// from the raw words.
struct ARMEHABIInfo {
  ARMEHABIEntryKind Kind = ARMEHABIEntryKind::CantUnwind;
  /// Address of the eight-byte index entry that named this function.
  va_t IndexEntryVA = 0;
  /// The index entry's second word, exactly as stored.
  uint32_t IndexWord = 0;
  /// Address of the `.ARM.extab` entry, for the two table-backed shapes.
  va_t TableEntryVA = 0;
  /// Which ARM-defined personality routine a compact entry selected.  Absent
  /// for the generic model, which names a routine by address instead.
  std::optional<uint8_t> PersonalityIndex;
  /// Words of unwind opcodes the entry declared past its first word.
  uint32_t ExtraWordCount = 0;
  /// How the type-table slots of this record's LSDA were read.
  ARMTypeTableConvention TypeTableConvention = ARMTypeTableConvention::Unknown;
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEHARM_H
