//===- MedCallConventionAAPCS32.cpp - ARM AAPCS call arguments -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AAPCS passes R0-R3 and then the stack.  A 64-bit argument takes an
// even-odd register pair and an 8-byte aligned stack slot, wasting the
// register or the 4-byte lane before it.  The base variant (softfp) passes
// floating arguments the same way; the VFP variant passes them in S0-S15.
// The routines of the Itanium C++ ABI's runtime and unwinder take the
// parameters it declares.
//
//===----------------------------------------------------------------------===//

#include "../MedCallConventionItanium.h"

#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/BinaryFormat/ELF.h"

namespace neverd {

namespace {
/// The base AAPCS passes an external routine's floating arguments in r0-r3,
/// and AAPCS-VFP in the VFP registers.  An ELF image built for the VFP
/// variant says so in its header; Apple's armv7 uses the base variant, and
/// Windows on ARM the VFP one.
bool externalFloatsInCoreRegisters(const BinaryImage &Img) {
  switch (Img.abiFormat()) {
  case BinaryFormat::ELF:
    return !Img.ELFMetadata ||
           !(Img.ELFMetadata->Flags & llvm::ELF::EF_ARM_ABI_FLOAT_HARD);
  case BinaryFormat::MachO:
    return true;
  default:
    return false;
  }
}
} // namespace

extern const CallArgumentConvention AAPCS32CallArguments;
const CallArgumentConvention AAPCS32CallArguments = {
    .TheArch = Arch::ARM,
    .ImportArgumentsFromPrototype = true,
    .PairAlignedWideArguments = true,
    .ExternalFloatsInCoreRegisters = externalFloatsInCoreRegisters,
    .PrototypeArgCount = itaniumRuntimeArgCount,
    .ArgumentsFromCallSetup = true,
};

} // namespace neverd
