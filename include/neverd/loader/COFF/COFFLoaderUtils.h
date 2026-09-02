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

namespace detail {

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

/// Parse a single ordinary imported symbol and append to Img.Imports.
void addImportedSymbol(const llvm::object::imported_symbol_iterator &SI,
                       llvm::StringRef ModuleName, va_t IATAddr,
                       BinaryImage &Img);

/// Parse the COFF symbol table (.symtab) and populate Img.Symbols.
/// Applies to object files and some executables with embedded symbols.
/// SectionVAs is an optional one-based mapped-section table; relocatable COFF
/// uses it because its format-native section VirtualAddress fields must be 0.
void parseSymbolTable(const llvm::object::COFFObjectFile &Obj, BinaryImage &Img,
                      uint64_t ImageBase, llvm::ArrayRef<va_t> SectionVAs = {});

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
/// Img.BaseRelocations.  Handles both PE32 and PE32+.
void parseBaseRelocations(const llvm::object::COFFObjectFile &Obj,
                          BinaryImage &Img, uint64_t ImageBase);

/// Parse the PE debug directory and reduce all bounded CodeView RSDS entries
/// to one typed build identity.  The path is retained only as a discovery hint.
void parseDebugDirectory(const llvm::object::COFFObjectFile &Obj,
                         BinaryImage &Img);

/// Parse the PE Load Configuration directory and extract security cookie
/// and CF Guard check function RVAs.  Handles both PE32 and PE32+.
void parseLoadConfiguration(const llvm::object::COFFObjectFile &Obj,
                            BinaryImage &Img, uint64_t ImageBase);

} // namespace coff_loader
} // namespace neverd

#endif // NEVERD_LOADER_COFF_COFFLOADERUTILS_H
