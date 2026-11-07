//===- LLVMCImageBase.cpp - PE headers as the linker's __ImageBase -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// An LLVM module reaches its PE image's own DOS and NT headers through the
/// linker's `__ImageBase` (MedLLVMEmitter::tryResolveImageHeaderData).  LLVM
/// C declares that symbol by its C name, as HighC does, and copies no header
/// byte: the headers describe the image that runs, wherever its loader places
/// it.
//===----------------------------------------------------------------------===//

#include "../LLVMCWriter.h"

#include "neverd/backend/DataSymbolMetadata.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/object/PELayout.h"

namespace neverd {

bool LLVMCWriter::isImageBaseDeclaration(const llvm::GlobalVariable &Global) {
  const auto *Bytes = llvm::dyn_cast<llvm::ArrayType>(Global.getValueType());
  return Global.isDeclaration() && Global.getName() == kPEImageBaseSymbol &&
         !data_symbol::isMarked(Global) && Bytes &&
         Bytes->getNumElements() == 0 &&
         Bytes->getElementType()->isIntegerTy(8);
}

void LLVMCWriter::allocateImageBaseIdentifier(
    const llvm::GlobalVariable &Global) {
  ExternalDataIdentifiers.emplace(
      &Global, GlobalIdentifierAllocator.allocateVerbatim(kPEImageBaseSymbol));
}

void LLVMCWriter::writeImageBaseDeclaration(llvm::StringRef Identifier) {
  OS << "extern unsigned char " << Identifier << "[]";
  // Another unit name holds the C spelling: bind the linker's symbol itself.
  if (Identifier != kPEImageBaseSymbol)
    OS << " __asm__(\""
       << escapeCString(
              symbolOfCName(kPEImageBaseSymbol, Opts.Format, Opts.TheArch))
       << "\")";
  OS << ";\n";
}

} // namespace neverd
