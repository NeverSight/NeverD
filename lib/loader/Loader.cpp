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

#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/EVM/EVMLoader.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/ObjectFileUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>

namespace neverd {

llvm::StringRef getLoadRowLoader(LoadRow Row) {
  switch (Row) {
#define NEVERD_LOAD_ROW(Id, Loader, Text)                                      \
  case LoadRow::Id:                                                            \
    return Loader;
#include "neverd/loader/LoadRows.def"
  }
  llvm_unreachable("unknown load row");
}

llvm::StringRef getLoadRowText(LoadRow Row) {
  switch (Row) {
#define NEVERD_LOAD_ROW(Id, Loader, Text)                                      \
  case LoadRow::Id:                                                            \
    return Text;
#include "neverd/loader/LoadRows.def"
  }
  llvm_unreachable("unknown load row");
}

llvm::StringRef getLoadReasonText(LoadReason Reason) {
  switch (Reason) {
#define NEVERD_LOAD_REASON(Id, Text)                                           \
  case LoadReason::Id:                                                         \
    return Text;
#include "neverd/loader/LoadRows.def"
  }
  llvm_unreachable("unknown load reason");
}

std::vector<LoadCandidate> identifyFile(const std::filesystem::path &Path) {
  std::vector<LoadCandidate> Rows;
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  // Whether no header names the file's format.
  bool Headerless = false;
  if (Buffer) {
    // The loaders Loader::create picks for the file, in its order.
    const llvm::MemoryBufferRef Ref = (*Buffer)->getMemBufferRef();
    switch (magicToFormat(llvm::identify_magic(Ref.getBuffer()))) {
    case BinaryFormat::ELF:
      ELFLoader::identify(Ref, Rows);
      break;
    case BinaryFormat::COFF:
      COFFLoader::identify(Ref, Rows);
      break;
    case BinaryFormat::MachO:
      MachOLoader::identify(Ref, Rows);
      break;
    case BinaryFormat::EVM:
    case BinaryFormat::Unknown:
    case BinaryFormat::Raw:
      Headerless = true;
      // Binary data under a name other tools share reads as bytecode only
      // when the user chooses so.
      switch (evm::matchEVMInput(Path)) {
      case evm::EVMInputMatch::Bytecode:
        EVMLoader::identify(Rows, /*ByName=*/false);
        break;
      case evm::EVMInputMatch::Name:
        EVMLoader::identify(Rows, /*ByName=*/true);
        break;
      case evm::EVMInputMatch::None:
        break;
      }
      break;
    }
  }
  std::stable_partition(Rows.begin(), Rows.end(),
                        [](const LoadCandidate &Row) { return Row.First; });
  // Any file can be read as a binary file of the processor the user names.
  LoadCandidate Binary;
  Binary.Row = LoadRow::Binary;
  Binary.Format = BinaryFormat::Raw;
  Binary.Description = getLoadRowText(LoadRow::Binary).str();
  Binary.Loadable = true;
  // When no header names a processor NeverD reads, only the bytes can.
  if (Buffer &&
      (Headerless || llvm::none_of(Rows, [](const LoadCandidate &Row) {
         return Row.Loadable;
       })))
    Binary.ISA =
        identifyISA(llvm::arrayRefFromStringRef((*Buffer)->getBuffer()));
  Rows.push_back(std::move(Binary));
  return Rows;
}

llvm::Expected<BinaryImage> Loader::loadBuffer(llvm::MemoryBufferRef) {
  return llvm::make_error<llvm::StringError>(
      "buffer loading is unavailable for this format",
      llvm::inconvertibleErrorCode());
}

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
  case BinaryFormat::Raw:
    // A binary file's loader needs the processor and placement the user
    // chose; loadBinary makes it from them.
    return nullptr;
  default:
    return nullptr;
  }
}

std::unique_ptr<Loader> Loader::create(const std::filesystem::path &Path) {
  BinaryFormat Fmt = detectFormat(Path);
  if (Fmt == BinaryFormat::Unknown) {
    if (evm::matchEVMInput(Path) == evm::EVMInputMatch::Bytecode)
      Fmt = BinaryFormat::EVM;
    else
      return nullptr;
  }
  return create(Fmt);
}

std::string Loader::describeRefusal(const std::filesystem::path &Path) {
  if (evm::matchEVMInput(Path) == evm::EVMInputMatch::Name)
    return pathToUTF8(Path) +
           ": only its name ties it to EVM bytecode, a name other tools give "
           "their files too; choose its loader: evm reads it as bytecode, "
           "binary as a processor's code";
  return "unknown binary format: " + pathToUTF8(Path);
}

} // namespace neverd
