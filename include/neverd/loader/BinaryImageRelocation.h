//===- BinaryImageRelocation.h - Parsed relocation records --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The two relocation records a loader reports: the symbolic entry codegen
/// needs in order to re-resolve a reference, and the bare ASLR/PIC base
/// relocation that only says an address has to be rebased.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_BINARYIMAGERELOCATION_H
#define NEVERD_LOADER_BINARYIMAGERELOCATION_H

#include "neverd/Common.h"

#include "llvm/BinaryFormat/ELF.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace neverd {

/// Alignment of the ELF64 #[repr(C)] records parsed by the official SBF
/// loader. LLVM's endian-aware on-disk wrappers are intentionally byte-aligned
/// and therefore cannot supply this value through alignof(wrapper).
inline constexpr size_t kELF64RecordAlignment = alignof(uint64_t);

/// Identifies the ELF metadata table that selected a relocation record.  SBF
/// v0-v2 execution consumes DT_REL from the dynamic table; it does not consume
/// every section whose type happens to be SHT_REL/SHT_RELA.  Keeping that
/// distinction typed prevents a later consumer from silently widening the
/// loader's accepted input language.
enum class ELFRelocationSource : uint8_t {
  SectionTable,
  ProgramDynamicTable,
  SectionDynamicTable,
};

/// Immutable snapshot of the exact ELF symbol selected by r_sym.  A name is
/// deliberately optional: some relocation forms use st_value without ever
/// consulting a string table.  The raw fields and symbol-table section index
/// let consumers distinguish two same-named symbols without reparsing the ELF
/// or performing a lossy global name lookup.
struct ELFRelocationSymbol {
  uint32_t TableSectionIndex = 0;
  uint32_t NameOffset = 0;
  uint16_t SectionIndex = 0;
  uint8_t Info = 0;
  uint8_t Binding = 0;
  uint8_t Type = 0;
  uint8_t Other = 0;
  uint64_t Value = 0;
  uint64_t Size = 0;
  std::optional<std::string> Name;

  bool isDefined() const { return SectionIndex != llvm::ELF::SHN_UNDEF; }
  bool isFunction() const { return Type == llvm::ELF::STT_FUNC; }
};

/// ELF-only record provenance attached to a normalized RelocationEntry.
/// Address remains the mapped VM address used by codegen; RawOffset retains
/// the encoded r_offset and Table* identify the DT_REL inventory it came from.
struct ELFRelocationProvenance {
  ELFRelocationSource Source = ELFRelocationSource::SectionTable;
  uint64_t TableVirtualAddress = 0;
  uint64_t TableFileOffset = 0;
  uint64_t Ordinal = 0;
  uint64_t RawOffset = 0;
  uint64_t RawInfo = 0;
  std::optional<ELFRelocationSymbol> Symbol;
};

// ===--------------------------------------------------------------------===//
// RelocationEntry — parsed relocation for codegen
// ===--------------------------------------------------------------------===//

struct RelocationEntry {
  va_t Address = 0;
  int64_t Addend = 0;
  bool HasExplicitAddend = false;
  uint32_t Type = 0;
  std::string SymbolName;
  uint32_t SymbolIndex = 0;
  std::string SectionName;
  std::optional<ELFRelocationProvenance> ELF;
};

// ===--------------------------------------------------------------------===//
// BaseRelocation — ASLR/PIC base relocation entry
// ===--------------------------------------------------------------------===//

struct BaseRelocation {
  va_t Address = 0;
  uint8_t Type = 0;
};

} // namespace neverd

#endif // NEVERD_LOADER_BINARYIMAGERELOCATION_H
