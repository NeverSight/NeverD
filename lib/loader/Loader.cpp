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

#include "neverd/loader/Loader.h"

#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/EVM/EVMLoader.h"
#include "neverd/loader/MachO/DyldCacheLoad.h"
#include "neverd/loader/MachO/DyldSharedCache.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/ObjectFileUtils.h"
#include "neverd/support/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SaveAndRestore.h"

#include <algorithm>
#include <climits>
#include <system_error>

namespace neverd {

llvm::Expected<BinaryImage>
Loader::loadWithProgress(const std::filesystem::path &Path,
                         const LoadProgress &Control) {
  llvm::SaveAndRestore<LoadProgress> Scope(Progress, Control);
  try {
    Progress.check();
    auto Image = load(Path);
    if (!Image)
      return Image.takeError();
    Progress.check();
    return Image;
  } catch (const LoadCancelled &) {
    return llvm::make_error<llvm::StringError>(
        LoadCancelled::Message,
        std::make_error_code(std::errc::operation_canceled));
  }
}

llvm::Expected<BinaryImage>
Loader::loadBufferWithProgress(llvm::MemoryBufferRef Buffer,
                               const LoadProgress &Control) {
  llvm::SaveAndRestore<LoadProgress> Scope(Progress, Control);
  try {
    Progress.check();
    auto Image = loadBuffer(Buffer);
    if (!Image)
      return Image.takeError();
    Progress.check();
    return Image;
  } catch (const LoadCancelled &) {
    return llvm::make_error<llvm::StringError>(
        LoadCancelled::Message,
        std::make_error_code(std::errc::operation_canceled));
  }
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
Loader::readFileBuffer(const std::filesystem::path &Path, BinaryFormat Fmt,
                       const LoadProgress &Control) {
  const auto Name = pathToUTF8(Path);
  Control.report("read", 0, 1, Name.c_str());
  // Binary parsers consume bounded bytes. Requiring a trailing NUL would
  // force a whole-file copy for page-aligned inputs instead of mapping them.
  auto Buffer = llvm::MemoryBuffer::getFile(Name, /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return llvm::make_error<llvm::StringError>(std::string(getFormatTag(Fmt)) +
                                                   ": cannot open " + Name,
                                               llvm::inconvertibleErrorCode());
  Control.report("read", 1, 1, Name.c_str());
  return std::move(*Buffer);
}

void Loader::copyImageBytes(llvm::ArrayRef<uint8_t> Bytes,
                            std::vector<uint8_t> &Destination,
                            const LoadProgress &Control) {
  Control.report("copy", 0, Bytes.size());
  if (!Control) {
    Destination.assign(Bytes.begin(), Bytes.end());
    return;
  }
  Destination.clear();
  Destination.reserve(Bytes.size());
  for (size_t Done = 0; Done < Bytes.size();) {
    Control.check();
    const size_t Count = std::min(limits::support::kLoadByteCheckpointBytes,
                                  Bytes.size() - Done);
    Destination.insert(Destination.end(), Bytes.begin() + Done,
                       Bytes.begin() + Done + Count);
    Done += Count;
    Control.report("copy", Done, Bytes.size());
  }
}

void Loader::initializeImage(llvm::MemoryBufferRef Buffer, BinaryImage &Img,
                             BinaryFormat Fmt, bool CopyRaw,
                             const LoadProgress &Control) {
  Img.Format = Fmt;
  const llvm::ArrayRef<uint8_t> Bytes(
      reinterpret_cast<const uint8_t *>(Buffer.getBufferStart()),
      Buffer.getBufferSize());
  Img.InputFileSHA256 = sha256(Bytes, Control);
  if (CopyRaw)
    copyImageBytes(Bytes, Img.Raw, Control);
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
Loader::readFileInto(const std::filesystem::path &Path, BinaryImage &Img,
                     BinaryFormat Fmt, bool CopyRaw,
                     const LoadProgress &Control) {
  auto Buffer = readFileBuffer(Path, Fmt, Control);
  if (!Buffer)
    return Buffer.takeError();
  initializeImage((*Buffer)->getMemBufferRef(), Img, Fmt, CopyRaw, Control);
  return std::move(*Buffer);
}

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
  bool SharedCache = false;
  if (Buffer) {
    // The loaders Loader::create picks for the file, in its order.
    const llvm::MemoryBufferRef Ref = (*Buffer)->getMemBufferRef();
    SharedCache = macho_loader::DyldSharedCache::hasMagic(Ref.getBuffer());
    if (SharedCache) {
      LoadCandidate Row;
      Row.Row = LoadRow::DyldCache;
      Row.Format = BinaryFormat::MachO;
      Row.TheArch = Arch::AArch64;
      Row.Bits = sizeof(uint64_t) * CHAR_BIT;
      Row.Description = getLoadRowText(Row.Row).str();
      Row.Reason = macho_loader::cache_load::kSelectionRequired;
      Rows.push_back(std::move(Row));
    } else
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
  if (Buffer && !SharedCache &&
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
    auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path), false, false);
    if (Buffer &&
        macho_loader::DyldSharedCache::hasMagic((*Buffer)->getBuffer()))
      return create(BinaryFormat::MachO);
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
