//===- RegistrationThrowImportABI.cpp - PE32 CRT throw entry -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate the runtime entry shared by private and direct throw calls.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {
std::optional<RegistrationThrowImportABI>
getCheckedX86RegistrationThrowImportABI(const BinaryImage &Image, va_t Target,
                                        size_t *CumulativeWork) {
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  if (!registration_abi::chargeCalleeWork(Work, 1) || Image.Arch != Arch::X86 ||
      Image.Bits != Bitness::Bits32 || Image.Format != BinaryFormat::COFF ||
      Target > uint64_t(UINT32_MAX) - 5 || !Image.isCodeAddress(Target))
    return std::nullopt;
  const auto *Import = Image.findImportStubAt(Target);
  const auto *Stub = Image.readVA(Target, 6);
  if (!Import || Import->Name != "_CxxThrowException" ||
      (!llvm::StringRef(Import->Module)
            .equals_insensitive("vcruntime140.dll") &&
       !llvm::StringRef(Import->Module)
            .equals_insensitive("vcruntime140d.dll")) ||
      !Import->IATAddr || Import->IATAddr > uint64_t(UINT32_MAX) - 3 ||
      !Image.readVA(Import->IATAddr, 4) || !Stub || Stub[0] != 0xff ||
      Stub[1] != 0x25 || readLE<uint32_t>(Stub + 2) != Import->IATAddr)
    return std::nullopt;
  return RegistrationThrowImportABI{Target, Import->IATAddr};
}
} // namespace neverd
