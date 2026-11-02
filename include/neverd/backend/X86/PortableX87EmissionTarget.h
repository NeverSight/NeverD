//===- PortableX87EmissionTarget.h - Explicit portable C target -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_X86_PORTABLEX87EMISSIONTARGET_H
#define NEVERD_BACKEND_X86_PORTABLEX87EMISSIONTARGET_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Triple.h"

namespace neverd {

struct PortableX87EmissionTarget {
  llvm::Triple Triple;
  BinaryFormat Format;
};

/// This checks representation requirements, not runtime qualification. The
/// caller must link the explicit versioned runtime and supply known state.
inline llvm::Expected<PortableX87EmissionTarget>
portableX87EmissionTarget(llvm::StringRef Target) {
  llvm::Triple Triple(llvm::Triple::normalize(Target));
  if (!Triple.isArch64Bit() || !Triple.isLittleEndian() ||
      (Triple.getArch() != llvm::Triple::x86_64 &&
       Triple.getArch() != llvm::Triple::aarch64) ||
      (!Triple.isOSBinFormatELF() && !Triple.isOSBinFormatMachO() &&
       !Triple.isOSBinFormatCOFF()) ||
      llvm::DataLayout(Triple.computeDataLayout()).getPointerSizeInBits() != 64)
    return llvm::createStringError(llvm::errc::not_supported,
                                   "portable x87 output requires a 64-bit "
                                   "little-endian x64 or AArch64 C target");
  const auto Format = Triple.isOSBinFormatCOFF()    ? BinaryFormat::COFF
                      : Triple.isOSBinFormatMachO() ? BinaryFormat::MachO
                                                    : BinaryFormat::ELF;
  return PortableX87EmissionTarget{Triple, Format};
}

} // namespace neverd

#endif // NEVERD_BACKEND_X86_PORTABLEX87EMISSIONTARGET_H
