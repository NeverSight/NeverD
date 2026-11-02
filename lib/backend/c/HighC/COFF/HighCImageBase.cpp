//===- HighCImageBase.cpp - PE headers as the linker's __ImageBase -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A linked PE image's code reaches its own DOS and NT headers through the
/// linker's `__ImageBase`.  The unit declares that symbol when it reaches the
/// headers and prints each header address as `&__ImageBase[offset]`.  It
/// never copies the file's header bytes or folds a read of them: the headers
/// describe the image that runs, wherever its loader places it.
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "neverd/loader/SymbolDecoration.h"
#include "neverd/object/PELayout.h"

#include <algorithm>

namespace neverd {

void HighCWriter::noteImageBaseAddress(va_t Addr) {
  if (Opts.Image && Opts.Image->imageBaseOffset(Addr))
    ReachesImageBase = true;
}

bool HighCWriter::isImageBaseConstant(const HighExpr &E) const {
  return E.Kind == ExprKind::Const &&
         isExactAddressProvenance(E.ConstProvenance) && Opts.Image &&
         Opts.Image->imageBaseOffset(E.ConstVal).has_value();
}

void HighCWriter::addImageBaseBacking() {
  ImageBaseIdentifier.clear();
  if (!ReachesImageBase || !Opts.Image)
    return;
  ImageBaseIdentifier =
      GlobalIdentifierAllocator.allocateVerbatim(kPEImageBaseSymbol);
  ImageBacking Backing{Opts.Image->Base,
                       Opts.Image->Base + Opts.Image->PEHeaderBytes,
                       ImageBaseIdentifier,
                       {},
                       {}};
  Backing.External = true;
  ImageBackings.insert(
      std::upper_bound(ImageBackings.begin(), ImageBackings.end(), Backing.Base,
                       [](va_t Base, const ImageBacking &Other) {
                         return Base < Other.Base;
                       }),
      std::move(Backing));
}

void HighCWriter::writeImageBaseDeclaration() {
  if (ImageBaseIdentifier.empty())
    return;
  OS << "extern unsigned char " << ImageBaseIdentifier << "[]";
  // Another unit name holds the C spelling: bind the linker's symbol itself.
  if (llvm::StringRef(ImageBaseIdentifier) != kPEImageBaseSymbol) {
    OS << " __asm__(\"";
    OS.write_escaped(
        symbolOfCName(kPEImageBaseSymbol, Opts.Format, Opts.TheArch));
    OS << "\")";
  }
  OS << ";\n";
}

} // namespace neverd
