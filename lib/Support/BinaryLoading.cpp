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

#include "neverd/Support/BinaryLoading.h"

#include "neverd/Support/TextEncoding.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

using namespace llvm;

namespace neverd {

void normalizeBinaryMetadata(BinaryImage &Img) {
  auto Normalize = [](std::string &Text) { Text = escapeInvalidUTF8(Text); };

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

Expected<BinaryImage> loadBinary(const std::filesystem::path &Path) {
  auto TheLoader = Loader::create(Path);
  if (!TheLoader)
    return make_error<StringError>("unknown binary format: " + Path.string(),
                                   inconvertibleErrorCode());
  auto ImgOrErr = TheLoader->load(Path);
  if (!ImgOrErr)
    return ImgOrErr.takeError();
  normalizeBinaryMetadata(*ImgOrErr);
  return std::move(*ImgOrErr);
}

} // namespace neverd
