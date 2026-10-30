//===- COFFLoaderUtils.h - COFF/PE loader helpers -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// COFF/PE-specific loader utilities: IAT thunk scanning, exception
/// directory (.pdata) parsing, and related heuristic function discovery.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFLOADERUTILS_H
#define NEVERD_LOADER_COFF_COFFLOADERUTILS_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace neverd {
namespace coff_loader {

/// Loader-owned offsets of pointer-storage fields in the PE load configuration.
struct LoadConfigCallableField {
  uint32_t Offset;
  RuntimeCallablePointerSlotKind Kind;
};
llvm::ArrayRef<LoadConfigCallableField> loadConfigCallableFields(Bitness Bits);

namespace detail {

/// PE32/PE32+ named import thunks reserve every bit above the low 31.
/// Ordinal-flag handling belongs to the caller; this never masks bad bits.
constexpr bool validImportNameBits(uint64_t Value) {
  return Value <= 0x7fffffff;
}

/// A bound IAT cannot stand in for an absent independent lookup table.
constexpr bool validImportLookupSource(uint32_t LookupRVA, uint32_t Timestamp,
                                       bool HasBoundImports) {
  return LookupRVA || (!Timestamp && !HasBoundImports);
}

/// One PE section's two address spaces.  A trusted RVA mapping must be owned
/// by exactly one such section and fit completely in both its virtual and raw
/// extents; PointerToRawData alone is not an RVA proof.
struct RawBackedSectionRange {
  uint32_t RVA = 0;
  uint32_t VirtualSize = 0;
  uint32_t FileOffset = 0;
  uint32_t RawSize = 0;
};

/// Map a complete RVA range to its unique section-relative file offset.
/// Overlapping virtual owners, a virtual/raw tail crossing, malformed section
/// file ranges, and arithmetic overflow are errors.
llvm::Expected<uint64_t>
resolveUniqueRawBackedFileOffset(llvm::ArrayRef<RawBackedSectionRange> Sections,
                                 uint64_t FileSize, uint32_t RVA,
                                 uint32_t Size);

/// Resolve one CodeView payload.  If the debug entry supplies both an RVA and
/// PointerToRawData, they must resolve to the exact same file offset; equal
/// bytes at two different offsets are not the same artifact occurrence.
/// AddressOfRawData may be zero: PointerToRawData may then name a file-overlay
/// blob that is not mapped by any section, which VC6 NB10 records use.
llvm::Expected<llvm::ArrayRef<uint8_t>>
resolveCodeViewPayload(llvm::ArrayRef<uint8_t> FileData,
                       llvm::ArrayRef<RawBackedSectionRange> Sections,
                       uint32_t RVA, uint32_t RawFileOffset, uint32_t Size);

struct CodeViewRSDSRecord {
  PDBBuildIdentity Identity;
  std::string Path;
};

/// Parse one complete IMAGE_DEBUG_TYPE_CODEVIEW payload as an RSDS record.
/// Unknown signatures, truncated data, unterminated paths, and invalid
/// identities are errors so callers cannot silently downgrade them to absence.
llvm::Expected<CodeViewRSDSRecord>
parseCodeViewRSDS(llvm::ArrayRef<uint8_t> Bytes);

/// Parse one complete IMAGE_DEBUG_TYPE_CODEVIEW payload as an NB10 / PDB 2.00
/// record (timestamp signature + age + path).
llvm::Expected<CodeViewRSDSRecord>
parseCodeViewNB10(llvm::ArrayRef<uint8_t> Bytes);

/// Dispatch RSDS or NB10.  Any other signature is an error.
llvm::Expected<CodeViewRSDSRecord>
parseCodeViewIdentity(llvm::ArrayRef<uint8_t> Bytes);

/// Order-independent reduction of all CodeView records in one image.  Once a
/// malformed or conflicting record is observed, the result remains Ambiguous.
class CodeViewIdentityRegistry {
public:
  void observe(const CodeViewRSDSRecord &Record);
  void observeMalformed();

  PDBIdentityState state() const { return State; }
  const std::optional<PDBBuildIdentity> &identity() const { return Identity; }
  const std::string &path() const { return Path; }

private:
  PDBIdentityState State = PDBIdentityState::Absent;
  std::optional<PDBBuildIdentity> Identity;
  std::string Path;
};

} // namespace detail

/// Parse the PE exception directory (.pdata) RUNTIME_FUNCTION entries
/// and register functions.  Applies to x64, ARM32, and AArch64.
void parseExceptions(const llvm::object::COFFObjectFile &Obj, BinaryImage &Img,
                     uint64_t ImageBase);

/// Parse complete, bounded ordinary import descriptors. Malformed metadata
/// contributes diagnostics without publishing guessed or partial bindings.
void parseImports(const llvm::object::COFFObjectFile &Obj, BinaryImage &Img);

/// Whether the symbol record \p Sym is a debug record, such as `.file` or
/// `.bf`/`.ef`: neither a name nor a place in the image.
inline bool isDebugRecord(const llvm::object::COFFSymbolRef &Sym) {
  return Sym.getSectionNumber() == llvm::COFF::IMAGE_SYM_DEBUG ||
         Sym.isFileRecord() ||
         Sym.getStorageClass() == llvm::COFF::IMAGE_SYM_CLASS_FUNCTION;
}

/// Whether the symbol record \p Sym is a section's own symbol: static, not a
/// function, and followed by its auxiliary section definition.  GNU ld keeps
/// one for each section an object contributes, at that object's first
/// function or variable.  A static function's record also has an auxiliary
/// record, its function definition, and names that function.
inline bool isSectionDefinition(const llvm::object::COFFSymbolRef &Sym) {
  return Sym.getStorageClass() == llvm::COFF::IMAGE_SYM_CLASS_STATIC &&
         Sym.getComplexType() != llvm::COFF::IMAGE_SYM_DTYPE_FUNCTION &&
         Sym.getNumberOfAuxSymbols() > 0;
}

/// Parse the COFF symbol table (.symtab) and populate Img.Symbols.
/// Applies to object files and some executables with embedded symbols.
/// A linked image's debug records are left out, and its section definitions
/// load as Symbol::IsSection: they bound what an object contributed but name
/// nothing.  An object file's relocations still name its sections.
/// SectionVAs is an optional one-based mapped-section table; relocatable COFF
/// uses it because its format-native section VirtualAddress fields must be 0.
void parseSymbolTable(const llvm::object::COFFObjectFile &Obj, BinaryImage &Img,
                      uint64_t ImageBase, llvm::ArrayRef<va_t> SectionVAs = {});

/// Record, once discovery has found the image's code, the data a linked
/// image's own symbol table names among it: a symbol of no function type in
/// an executable section that no code metadata claims, is no import thunk
/// and starts no instruction, such as GNU ld's __CTOR_LIST__ and
/// __DTOR_LIST__ in .text.  The relocated pointers to such an object, which
/// the base relocations, read before the symbol table, took for pointers to
/// code, become pointers to data (recordDataPointerTargets).
void recordSymbolTableDataInCode(BinaryImage &Img);

/// Records in Img.ConstantCodeSlots each `__imp_X` variable of an x86 or x64
/// image's symbol table, MinGW's for a function its runtime supplies in place
/// of an import (__MINGW_IMP_SYMBOL), that holds one of the image's function
/// entries under a relocation and that no instruction changes or lets escape:
/// every instruction that names it reads it, and no pointer in data or export
/// names it.  A linear sweep of each executable section, resynchronized at
/// the entries the image's metadata gives, finds those instructions.  A lazy
/// binding that the function it starts at rewrites stays a variable.
void recordConstantCodeSlots(BinaryImage &Img);

/// Record as pointer slots of data the runs of relocated pointers a linked
/// image keeps among its code: more pointer-wide fields back to back than one
/// instruction can hold (maxAdjacentRelocatedFields), which the base
/// relocations took for instruction operands.  MSVC places an i386 switch's
/// jump table so, after its function.
void recordRelocatedPointerRunsInCode(BinaryImage &Img);

/// Reclassify the relocation-covered words of tables the code reads through a
/// runtime-indexed fixed-base memory operand in an executable section: those
/// words are data the linker placed among the code, never instruction
/// operands.
void recordIndexedTableBasesInCode(BinaryImage &Img);

/// Record the [begin, end) runs of tables an indirect JUMP reads through a
/// runtime-indexed fixed-base memory operand, before the data-pointer
/// discovery scan runs: a jump table in .rdata would otherwise synthesize a
/// function symbol for every switch case.  Reads relocation state only;
/// pairs with the reclassifying recordIndexedTableBasesInCode below.
void recordIndexedJumpTableRuns(BinaryImage &Img);

/// Parse one delay-import descriptor from the already mapped image.  Supports
/// current RVA descriptors and legacy VA descriptors, returning the number of
/// imports appended.  Exposed separately for bounded synthetic-image tests.
size_t parseDelayImportDescriptor(
    const llvm::object::delay_import_directory_table_entry &Desc,
    BinaryImage &Img);

/// Parse every PE delay-import descriptor.
void parseDelayImports(const llvm::object::COFFObjectFile &Obj,
                       BinaryImage &Img);

/// Parse the TLS directory and populate function symbols plus runtime callback
/// metadata.  Handles both PE32 and PE32+.
void parseTLSDirectory(const llvm::object::COFFObjectFile &Obj,
                       BinaryImage &Img, uint64_t ImageBase);

/// Parse the PE base relocation table (.reloc) and populate
/// Img.BaseRelocations only after the whole directory is valid. Unsupported
/// relocation types retain their identity; malformed blocks return an error.
/// Handles both PE32 and PE32+; restricted loads deliberately skip the table.
llvm::Error parseBaseRelocations(const llvm::object::COFFObjectFile &Obj,
                                 BinaryImage &Img, uint64_t ImageBase);

/// Parse the PE debug directory and reduce all bounded CodeView RSDS/NB10
/// entries to one typed build identity.  The path is retained only as a
/// discovery hint.
void parseDebugDirectory(const llvm::object::COFFObjectFile &Obj,
                         BinaryImage &Img,
                         llvm::ArrayRef<uint8_t> OriginalBytes = {});

/// Parse the PE Load Configuration directory and extract security cookie
/// and CF Guard check function RVAs.  Handles both PE32 and PE32+.
void parseLoadConfiguration(const llvm::object::COFFObjectFile &Obj,
                            BinaryImage &Img, uint64_t ImageBase);

} // namespace coff_loader
} // namespace neverd

#endif // NEVERD_LOADER_COFF_COFFLOADERUTILS_H
