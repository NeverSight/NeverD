//===- MedLLVMPEImageBase.cpp - PE headers as __ImageBase ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A linked PE image's code reaches its own DOS and NT headers through the
/// linker's `__ImageBase`.  The module declares that symbol and addresses the
/// headers as byte offsets from it.  It never copies the file's header bytes:
/// the headers a rebuilt image runs with describe that image, and a loader
/// may place it anywhere.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/object/PELayout.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"

#include <optional>

namespace neverd {

llvm::Constant *MedLLVMEmitter::tryResolveImageHeaderData(uint64_t Addr,
                                                          uint64_t OwnerVA) {
  if (!Img || !Mod || !Ctx || !Img->PEHeaderBytes ||
      (OwnerVA != InvalidVA && OwnerVA != Img->Base))
    return nullptr;
  std::optional<uint64_t> Offset = Img->imageBaseOffset(Addr);
  // The header owner names one past its last byte too.
  if (!Offset && OwnerVA == Img->Base && Addr - Img->Base == Img->PEHeaderBytes)
    Offset = Img->PEHeaderBytes;
  if (!Offset)
    return nullptr;
  auto *Bytes = llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), 0);
  auto *ImageBase = llvm::dyn_cast_or_null<llvm::GlobalVariable>(
      Mod->getNamedValue(kPEImageBaseSymbol));
  if (!ImageBase && !Mod->getNamedValue(kPEImageBaseSymbol))
    ImageBase = new llvm::GlobalVariable(
        *Mod, Bytes, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr, kPEImageBaseSymbol);
  if (!ImageBase || !ImageBase->isDeclaration() ||
      ImageBase->getValueType() != Bytes) {
    if (!FatalCodePointerResolution && !FatalDataPointerResolution)
      syncError() << "med_llvm_emitter: image header address 0x"
                  << llvm::utohexstr(Addr) << " needs " << kPEImageBaseSymbol
                  << ", which another module identity holds; refusing "
                     "stale-address fallback\n";
    FatalDataPointerResolution = true;
    return nullptr;
  }
  return llvm::ConstantExpr::getGetElementPtr(
      llvm::Type::getInt8Ty(*Ctx), ImageBase,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), *Offset));
}

} // namespace neverd
