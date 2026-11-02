//===- BinaryLoading.cpp - Binary format auto-detection -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements binary format detection and loading via the Loader factory.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/BinaryLoading.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DyldCacheLoad.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/ObjectFileUtils.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/TextEncoding.h"

#include "llvm/Support/Error.h"

#include <system_error>

using namespace llvm;

namespace neverd {

void normalizeBinaryMetadata(BinaryImage &Img) {
  auto Normalize = [](std::string &Text) { escapeInvalidUTF8InPlace(Text); };

  for (auto &Seg : Img.Segments)
    Normalize(Seg.Name);
  for (auto &Sec : Img.Sections) {
    Normalize(Sec.Name);
    Normalize(Sec.SegmentName);
  }
  for (auto &Imp : Img.Imports) {
    Normalize(Imp.Module);
    Normalize(Imp.Name);
  }
  for (auto &Exp : Img.Exports)
    Normalize(Exp.Name);
  for (auto &Sym : Img.Symbols)
    Normalize(Sym.Name);
  for (auto &Rel : Img.Relocations) {
    Normalize(Rel.SymbolName);
    Normalize(Rel.SectionName);
  }
  for (auto &[Address, Name] : Img.ImportPtrSlots) {
    (void)Address;
    Normalize(Name);
  }
  for (auto &[Address, Slot] : Img.ImportStorageSlots) {
    (void)Address;
    Normalize(Slot.Name);
  }
  for (auto &[Address, Binding] : Img.DyldBindSlots) {
    (void)Address;
    Normalize(Binding.Name);
  }

  Normalize(Img.DynInfo.SOName);
  for (auto &Name : Img.DynInfo.NeededLibs)
    Normalize(Name);
  for (auto &Path : Img.DynInfo.RPaths)
    Normalize(Path);
  Normalize(Img.DynInfo.PDBPath);
  Normalize(Img.DynInfo.UUID);
  Normalize(Img.DynInfo.MinOSVersion);

  for (auto &Diagnostic : Img.ExceptionMetadata.Diagnostics)
    Normalize(Diagnostic);
  for (auto &Function : Img.ExceptionMetadata.Functions) {
    Normalize(Function.PersonalityName);
    for (auto &Diagnostic : Function.Diagnostics)
      Normalize(Diagnostic);
  }
}

namespace {
/// An object file's relocations name its symbols, so it has no reading
/// without its symbol table; a linked image does.
Error checkStaticSymbolChoice(const BinaryImage &Img,
                              const BinaryLoadOptions &Opts) {
  if (Opts.ReadStaticSymbols || !Img.IsRelocatable)
    return Error::success();
  return make_error<StringError>(
      "an object file cannot be read without its symbol table, which its "
      "relocations name",
      inconvertibleErrorCode());
}
} // namespace

Expected<BinaryImage> loadBinary(const std::filesystem::path &Path,
                                 const BinaryLoadOptions &Opts) try {
  Opts.Progress.report("identify", 0, 1);
  std::unique_ptr<Loader> TheLoader;
  if (!Opts.Choice.MachOSharedCacheImage.empty()) {
    if (Opts.Choice.Format != BinaryFormat::Unknown &&
        Opts.Choice.Format != BinaryFormat::MachO)
      return createStringError(inconvertibleErrorCode(),
                               macho_loader::cache_load::kChoiceInvalid);
    TheLoader =
        std::make_unique<MachOLoader>(Opts.Choice.MachOSharedCacheImage);
  } else
    switch (Opts.Choice.Format) {
    case BinaryFormat::Unknown:
      TheLoader = Loader::create(Path);
      if (!TheLoader)
        return make_error<StringError>(Loader::describeRefusal(Path),
                                       inconvertibleErrorCode());
      break;
    case BinaryFormat::Raw:
      TheLoader = std::make_unique<RawLoader>(Opts.Choice.Raw);
      break;
    default:
      TheLoader = Loader::create(Opts.Choice.Format);
      break;
    }
  if (!TheLoader)
    return make_error<StringError>("no loader reads the chosen format",
                                   inconvertibleErrorCode());
  if (!Opts.OnlyFunctionEntries.empty())
    TheLoader->restrictFunctions(Opts.OnlyFunctionEntries);
  if (!Opts.ARMFunctionModes.empty())
    TheLoader->setARMFunctionModes(Opts.ARMFunctionModes);
  TheLoader->setReadsStaticSymbols(Opts.ReadStaticSymbols);
  Opts.Progress.report("identify", 1, 1);
  auto ImgOrErr = TheLoader->loadWithProgress(Path, Opts.Progress);
  if (!ImgOrErr)
    return ImgOrErr.takeError();
  if (Error E = checkStaticSymbolChoice(*ImgOrErr, Opts))
    return std::move(E);
  Opts.Progress.report("metadata", 0, 1);
  normalizeBinaryMetadata(*ImgOrErr);
  ImgOrErr->numberSharedFunctionNames();
  Opts.Progress.report("metadata", 1, 1);
  return std::move(*ImgOrErr);
} catch (const LoadCancelled &) {
  return make_error<StringError>(
      LoadCancelled::Message,
      std::make_error_code(std::errc::operation_canceled));
}

Expected<BinaryImage> loadBinaryBuffer(llvm::MemoryBufferRef Buffer,
                                       const BinaryLoadOptions &Opts) try {
  Opts.Progress.report("identify", 0, 1);
  if (!Opts.Choice.MachOSharedCacheImage.empty())
    return createStringError(inconvertibleErrorCode(),
                             macho_loader::cache_load::kBufferUnavailable);
  if (Opts.Choice.Format != BinaryFormat::Unknown)
    return make_error<StringError>(
        "native buffer does not accept a loader choice",
        inconvertibleErrorCode());
  const auto Magic = llvm::identify_magic(Buffer.getBuffer());
  if (Magic == llvm::file_magic::macho_universal_binary)
    return make_error<StringError>(
        "native buffer requires a selected Mach-O slice",
        inconvertibleErrorCode());
  auto TheLoader = Loader::create(magicToFormat(Magic));
  if (!TheLoader)
    return make_error<StringError>("unknown native buffer format",
                                   inconvertibleErrorCode());
  TheLoader->restrictFunctions(Opts.OnlyFunctionEntries);
  TheLoader->setARMFunctionModes(Opts.ARMFunctionModes);
  TheLoader->setReadsStaticSymbols(Opts.ReadStaticSymbols);
  Opts.Progress.report("identify", 1, 1);
  auto Image = TheLoader->loadBufferWithProgress(Buffer, Opts.Progress);
  if (!Image)
    return Image.takeError();
  if (Error E = checkStaticSymbolChoice(*Image, Opts))
    return std::move(E);
  Opts.Progress.report("metadata", 0, 1);
  normalizeBinaryMetadata(*Image);
  Image->numberSharedFunctionNames();
  Opts.Progress.report("metadata", 1, 1);
  return std::move(*Image);
} catch (const LoadCancelled &) {
  return make_error<StringError>(
      LoadCancelled::Message,
      std::make_error_code(std::errc::operation_canceled));
}

} // namespace neverd
