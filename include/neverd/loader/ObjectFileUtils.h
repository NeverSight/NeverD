//===- ObjectFileUtils.h - LLVM ObjectFile helpers ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared helpers for opening binaries via LLVM's Object library.
/// Used by Loader::create and format-specific loaders.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_OBJECTFILEUTILS_H
#define NEVERD_LOADER_OBJECTFILEUTILS_H

#include "neverd/Common.h"

#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

namespace neverd {

/// Map LLVM's file_magic to NeverD's BinaryFormat.
inline BinaryFormat magicToFormat(llvm::file_magic Magic) {
  using llvm::file_magic;
  switch (Magic) {
  case file_magic::elf:
  case file_magic::elf_relocatable:
  case file_magic::elf_executable:
  case file_magic::elf_shared_object:
  case file_magic::elf_core:
    return BinaryFormat::ELF;
  case file_magic::macho_object:
  case file_magic::macho_executable:
  case file_magic::macho_fixed_virtual_memory_shared_lib:
  case file_magic::macho_core:
  case file_magic::macho_preload_executable:
  case file_magic::macho_dynamically_linked_shared_lib:
  case file_magic::macho_dynamic_linker:
  case file_magic::macho_bundle:
  case file_magic::macho_dynamically_linked_shared_lib_stub:
  case file_magic::macho_dsym_companion:
  case file_magic::macho_kext_bundle:
  case file_magic::macho_universal_binary:
    return BinaryFormat::MachO;
  case file_magic::coff_object:
  case file_magic::coff_import_library:
  case file_magic::pecoff_executable:
    return BinaryFormat::COFF;
  default:
    return BinaryFormat::Unknown;
  }
}

/// Detect binary format using llvm::identify_magic (cf. llvm-objdump).
inline BinaryFormat detectFormat(const std::filesystem::path &Path) {
  llvm::file_magic Magic = llvm::file_magic::unknown;
  if (llvm::identify_magic(Path.string(), Magic))
    return BinaryFormat::Unknown;
  return magicToFormat(Magic);
}

/// Open an LLVM ObjectFile from a path.  Returns nullptr on failure.
inline std::unique_ptr<llvm::object::ObjectFile>
openObjectFile(const std::filesystem::path &Path,
               std::unique_ptr<llvm::MemoryBuffer> &BufOut) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path.string());
  if (!BufOrErr)
    return nullptr;
  BufOut = std::move(*BufOrErr);
  auto ObjOrErr =
      llvm::object::ObjectFile::createObjectFile(BufOut->getMemBufferRef());
  if (!ObjOrErr) {
    llvm::consumeError(ObjOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjOrErr);
}

} // namespace neverd

#endif // NEVERD_LOADER_OBJECTFILEUTILS_H
