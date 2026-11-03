//===- MedSyscallConvention.cpp - Supervisor call registry ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedSyscallConvention.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ELF/ELFOperatingSystem.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {

extern const SyscallConvention LinuxAArch64Syscall;
extern const SyscallConvention LinuxARMSyscall;

namespace {

constexpr const SyscallConvention *LinuxSyscalls[] = {&LinuxAArch64Syscall,
                                                      &LinuxARMSyscall};

/// Whether \p Image is an arm64 Linux kernel Image, whose header holds the
/// magic "ARM\x64" 0x38 bytes in (Documentation/arch/arm64/booting.rst),
/// loaded as a binary file or through its EFI stub's PE header.  The kernel
/// makes no supervisor call; the user code it carries, its vDSO, makes
/// Linux's.
bool isAArch64LinuxKernelImage(const BinaryImage &Image) {
  constexpr va_t MagicOffset = 0x38;
  constexpr uint32_t Magic = 0x644D5241;
  if (Image.Arch != Arch::AArch64 ||
      (Image.Format != BinaryFormat::Raw && Image.Format != BinaryFormat::COFF))
    return false;
  const uint8_t *Bytes = Image.readVA(Image.Base + MagicOffset, 4);
  return Bytes && readLE<uint32_t>(Bytes) == Magic;
}

} // namespace

const SyscallConvention *syscallConvention(const BinaryImage &Image,
                                           Arch TheArch, Intrinsic Lifted,
                                           uint64_t Immediate) {
  if (!runsUnderLinux(Image) && !isAArch64LinuxKernelImage(Image))
    return nullptr;
  for (const SyscallConvention *Convention : LinuxSyscalls)
    if (Convention->TheArch == TheArch && Convention->Lifted == Lifted &&
        Convention->Immediate == Immediate)
      return Convention;
  return nullptr;
}

llvm::ArrayRef<const SyscallConvention *> syscallConventions() {
  return LinuxSyscalls;
}

const SyscallConvention *modeledSyscallConvention(Intrinsic Modeled) {
  for (const SyscallConvention *Convention : LinuxSyscalls)
    if (Convention->Modeled == Modeled)
      return Convention;
  return nullptr;
}

} // namespace neverd
