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

#include "llvm/Object/COFF.h"

#include <cstddef>

namespace neverd {
namespace coff_loader {

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
void parseSymbolTable(const llvm::object::COFFObjectFile &Obj, BinaryImage &Img,
                      uint64_t ImageBase);

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

/// Parse the PE debug directory and extract PDB path from CodeView
/// entries.  Stores the path in Img.DynInfo.PDBPath.
void parseDebugDirectory(const llvm::object::COFFObjectFile &Obj,
                         BinaryImage &Img);

/// Parse the PE Load Configuration directory and extract security cookie
/// and CF Guard check function RVAs.  Handles both PE32 and PE32+.
void parseLoadConfiguration(const llvm::object::COFFObjectFile &Obj,
                            BinaryImage &Img, uint64_t ImageBase);

} // namespace coff_loader
} // namespace neverd

#endif // NEVERD_LOADER_COFF_COFFLOADERUTILS_H
