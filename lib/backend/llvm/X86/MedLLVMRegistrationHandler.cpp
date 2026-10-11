//===- MedLLVMRegistrationHandler.cpp - Retained PE32 handler entries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind byte-authenticated runtime handler addresses to retained source code.
//===----------------------------------------------------------------------===//

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {
llvm::Function *
MedLLVMEmitter::resolveX86RegistrationHandlerReference(va_t Address) {
  if (!Img || TargetArch != Arch::X86 || TargetFormat != BinaryFormat::COFF)
    return nullptr;
  if (auto It = EmittedFuncNames.find(Address); It != EmittedFuncNames.end())
    return Mod->getFunction(It->second);
  if (!coff_loader::isCheckedX86CxxHandlerReference(*Img, Address))
    return nullptr;
  const auto Name = "__nd_registration_handler_" + llvm::utohexstr(Address);
  if (Mod->getNamedValue(Name))
    return nullptr;
  auto *Type = llvm::FunctionType::get(llvm::Type::getInt32Ty(*Ctx), {}, true);
  auto *Declaration = llvm::cast<llvm::Function>(
      Mod->getOrInsertFunction(Name, Type).getCallee());
  rewrite_source::setOriginalVA(*Declaration, Address);
  EmittedFuncNames[Address] = Name;
  FuncNames[Address] = Name;
  return Declaration;
}
} // namespace neverd
