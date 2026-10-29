//===- HighCObjectExterns.cpp - An object's undefined data symbols -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A relocatable object reads and writes data another module defines through
/// the addresses ObjectExterns gives its undefined symbols.  The unit
/// declares each such symbol it reaches as bytes the linker binds by the
/// symbol, prints each address in its storage as `&neverd_data_X[offset]`,
/// and defines nothing there: the storage is the other module's.
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "neverd/libc/LibCNames.h"
#include "neverd/loader/SymbolDecoration.h"

#include <algorithm>

namespace neverd {

std::optional<std::pair<const BinaryImage::ObjectExternData *, uint64_t>>
declaredObjectExternAt(const BinaryImage &Img, va_t Addr) {
  const auto Extern = Img.objectExternDataAt(Addr);
  if (!Extern)
    return std::nullopt;
  if (Extern->second == 0 && libc::headerOfDataObject(cNameOfSymbol(
                                 Extern->first->Name, Img.Format, Img.Arch)))
    return std::nullopt;
  return Extern;
}

void HighCWriter::noteObjectExternAddress(va_t Addr) {
  if (!Opts.Image)
    return;
  if (const auto Extern = declaredObjectExternAt(*Opts.Image, Addr))
    ReachedObjectExterns.insert(Addr - Extern->second);
}

std::string HighCWriter::externalDataIdentifier(const std::string &Symbol) {
  if (const auto It = ExternalDataNames.find(Symbol);
      It != ExternalDataNames.end())
    return It->second;
  const auto Identifier = GlobalIdentifierAllocator.allocate(
      "neverd_data_" + Symbol, "neverd_data");
  ExternalDataNames.emplace(Symbol, Identifier);
  return Identifier;
}

void HighCWriter::addObjectExternBackings() {
  if (!Opts.Image)
    return;
  for (va_t Slot : ReachedObjectExterns) {
    const auto &Extern = Opts.Image->ObjectExternDataSymbols.at(Slot);
    ImageBacking Backing{
        Slot, Slot + Extern.Bytes, externalDataIdentifier(Extern.Name), {}, {}};
    Backing.External = true;
    ImageBackings.insert(
        std::upper_bound(ImageBackings.begin(), ImageBackings.end(),
                         Backing.Base,
                         [](va_t Base, const ImageBacking &Other) {
                           return Base < Other.Base;
                         }),
        std::move(Backing));
  }
}

void HighCWriter::writeObjectExternDeclarations() {
  if (!Opts.Image)
    return;
  // An untyped byte alias binds the linker's symbol without inventing a
  // source type or colliding with a header's declaration of the name.
  for (va_t Slot : ReachedObjectExterns) {
    const auto &Extern = Opts.Image->ObjectExternDataSymbols.at(Slot);
    OS << "extern unsigned char " << ExternalDataNames.at(Extern.Name)
       << "[] __asm__(\"";
    OS.write_escaped(Extern.Name);
    OS << "\");\n";
  }
}

} // namespace neverd
