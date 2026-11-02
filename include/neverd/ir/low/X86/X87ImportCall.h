//===- X87ImportCall.h - Microsoft CRT register-stack calls -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_X86_X87IMPORTCALL_H
#define NEVERD_IR_LOW_X86_X87IMPORTCALL_H

#include "neverd/ir/low/ImportCallee.h"
#include "neverd/loader/COFF/RuntimeProviders.h"

#include "llvm/ADT/StringSwitch.h"

namespace neverd {

/// These CRT entry points take their arguments on the x87 stack, independently
/// of the ordinary C ABI. Their names must never produce zero-argument calls.
inline bool isX87CRTImportName(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(Name)
#define NEVERD_X87_CRT_IMPORT(Name, Arguments) .Case(Name, true)
#include "neverd/ir/low/X86/X87CRTImports.def"
      .Default(false);
}

/// Supported PE32 CRT contracts, in logical ST0, ST1 order. The result replaces
/// the arguments with one ST0 value. This describes transport only: the native
/// runtime still owns precision, error handling, global state and exceptions.
inline unsigned x87CRTImportArguments(const BinaryImage &Image, va_t Target) {
  if (Image.Arch != Arch::X86 || Image.abiFormat() != BinaryFormat::COFF)
    return 0;
  const Import *Imported = Image.findImportAt(Target);
  if (!Imported)
    Imported = Image.findImportStubAt(Target);
  if (!Imported || Imported->IsDelayImport ||
      importCalleeName(Image, Target) != Imported->Name)
    return 0;
  const auto Storage = Image.collectImportStorageSlot(Imported->IATAddr);
  const auto Slot = Storage.Slots.find(Imported->IATAddr);
  if (Storage.Conflicts.count(Imported->IATAddr) ||
      Slot == Storage.Slots.end() || Slot->second.Name != Imported->Name ||
      Slot->second.Addend != 0)
    return 0;
  if (std::count_if(Image.Imports.begin(), Image.Imports.end(),
                    [&](const Import &Other) {
                      return Other.IATAddr == Imported->IATAddr;
                    }) != 1)
    return 0;
  const llvm::StringRef Module = Imported->Module;
  if (!coff_loader::isX86LegacyCRTProvider(Module))
    return 0;
  // The LLVM external symbol has one provider. Different DLLs exporting the
  // same spelling require provider-qualified linkage before they can share it.
  if (std::any_of(
          Image.Imports.begin(), Image.Imports.end(), [&](const Import &Other) {
            return Other.Name == Imported->Name &&
                   !llvm::StringRef(Other.Module).equals_insensitive(Module);
          }))
    return 0;
  return llvm::StringSwitch<unsigned>(Imported->Name)
#define NEVERD_X87_CRT_IMPORT(Name, Arguments) .Case(Name, Arguments)
#include "neverd/ir/low/X86/X87CRTImports.def"
      .Default(0);
}

} // namespace neverd

#endif // NEVERD_IR_LOW_X86_X87IMPORTCALL_H
