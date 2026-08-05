//===- ArchSupport.h - ISA target support helpers -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ISA target support predicates and triple helpers for lift, codegen, and
/// patch.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ARCHSUPPORT_H
#define NEVERD_ARCHSUPPORT_H

#include "neverd/Common.h"

namespace neverd {

/// Four ISAs across loader / lift / MIR codegen / Mach-O patch (see
/// docs/targets.md). CI and day-to-day dev on Apple Silicon often exercise only
/// x86_64 + arm64 binaries; arm32 / i386 paths remain built and supported
/// in-tree.

inline bool archLiftSupported(Arch A) {
  return A == Arch::X64 || A == Arch::AArch64 || A == Arch::X86 ||
         A == Arch::ARM;
}

inline bool archCodegenSupported(Arch A) { return archLiftSupported(A); }

inline bool archMachOPatchSupported(Arch A) { return archLiftSupported(A); }

inline bool archCOFFPatchSupported(Arch A) { return archLiftSupported(A); }

inline bool archELFPatchSupported(Arch A) { return archLiftSupported(A); }

constexpr uint64_t kPageSize4K = 0x1000;
constexpr uint64_t kPageSize16K = 0x4000;
constexpr uint64_t kSyntheticStackAlignment = 16;

/// Residue of the ABI entry stack pointer modulo the synthetic stack alignment.
/// x86-64 includes the pushed return address; Darwin i386 enters at 12 mod 16.
constexpr uint64_t syntheticEntryStackResidue(
    Arch A, BinaryFormat Fmt = BinaryFormat::Unknown) {
  if (A == Arch::X64)
    return 8;
  return A == Arch::X86 && Fmt == BinaryFormat::MachO ? 12 : 0;
}

inline uint64_t elfPageSize(Arch A) {
  return (A == Arch::AArch64) ? kPageSize16K : kPageSize4K;
}

inline uint64_t machoPageSize(Arch A) {
  return (A == Arch::AArch64) ? kPageSize16K : kPageSize4K;
}

inline uint64_t defaultPageSize(Arch A, BinaryFormat Fmt) {
  if (Fmt == BinaryFormat::MachO)
    return machoPageSize(A);
  if (Fmt == BinaryFormat::ELF)
    return elfPageSize(A);
  return kPageSize4K;
}

/// Resolve the LLVM target triple for a given architecture and binary format.
inline const char *getTriple(Arch A, BinaryFormat Fmt) {
  if (Fmt == BinaryFormat::COFF) {
    switch (A) {
    case Arch::X64:
      return "x86_64-pc-windows-msvc";
    case Arch::X86:
      return "i686-pc-windows-msvc";
    case Arch::AArch64:
      return "aarch64-pc-windows-msvc";
    case Arch::ARM:
      return "thumbv7-pc-windows-msvc";
    default:
      return nullptr;
    }
  }
  if (Fmt == BinaryFormat::MachO) {
    switch (A) {
    case Arch::X64:
      return "x86_64-apple-macos14.0";
    case Arch::X86:
      return "i386-apple-darwin";
    case Arch::AArch64:
      return "arm64-apple-macos14.0";
    case Arch::ARM:
      return "arm-apple-darwin";
    default:
      return nullptr;
    }
  }
  switch (A) {
  case Arch::X64:
    return "x86_64-unknown-linux-gnu";
  case Arch::X86:
    return "i386-unknown-linux-gnu";
  case Arch::AArch64:
    return "aarch64-unknown-linux-gnu";
  case Arch::ARM:
    return "arm-unknown-linux-gnueabihf";
  default:
    return nullptr;
  }
}

inline const char *llvmEmitTriple(Arch A,
                                  BinaryFormat Fmt = BinaryFormat::ELF) {
  return getTriple(A, Fmt);
}

inline const char *llvmCodegenTriple(Arch A,
                                     BinaryFormat Fmt = BinaryFormat::MachO) {
  return getTriple(A, Fmt);
}

} // namespace neverd

#endif // NEVERD_ARCHSUPPORT_H
