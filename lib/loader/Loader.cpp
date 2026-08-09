//===- Loader.cpp - Binary format auto-detection and factory -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the Loader factory methods that auto-detect binary format
/// from file content and instantiate the appropriate format-specific
/// loader.  Follows the LLVM ObjectFile::createObjectFile pattern.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/EVM/EVMLoader.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/ObjectFileUtils.h"
#include "neverd/evm/Bytecode.h"

namespace neverd {

std::unique_ptr<Loader> Loader::create(BinaryFormat Format) {
  switch (Format) {
  case BinaryFormat::ELF:
    return std::make_unique<ELFLoader>();
  case BinaryFormat::COFF:
    return std::make_unique<COFFLoader>();
  case BinaryFormat::MachO:
    return std::make_unique<MachOLoader>();
  case BinaryFormat::EVM:
    return std::make_unique<EVMLoader>();
  default:
    return nullptr;
  }
}

std::unique_ptr<Loader> Loader::create(const std::filesystem::path &Path) {
  BinaryFormat Fmt = detectFormat(Path);
  if (Fmt == BinaryFormat::Unknown) {
    if (evm::hasEVMFileExtension(Path) || evm::looksLikeEVMInput(Path))
      Fmt = BinaryFormat::EVM;
    else
      return nullptr;
  }
  return create(Fmt);
}

} // namespace neverd
