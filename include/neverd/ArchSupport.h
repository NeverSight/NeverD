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

/// Whether every instruction of an \p A image decodes in the one mode the
/// image's bitness gives it.  AArch32 interworks ARM and Thumb code, whose
/// mode each address takes from the image's metadata, so code placed at a new
/// address has no mode until something states one.
constexpr bool archHasSingleInstructionMode(Arch A) { return A != Arch::ARM; }

/// Whether \p A stores a multi-byte value least significant byte first.  The
/// loaders take no big-endian image of the native ISAs; EVM memory is
/// big-endian.
constexpr bool archLittleEndian(Arch A) {
  return A == Arch::X64 || A == Arch::AArch64 || A == Arch::X86 ||
         A == Arch::ARM || A == Arch::SBF;
}

inline bool archMachOPatchSupported(Arch A) { return archLiftSupported(A); }

inline bool archCOFFPatchSupported(Arch A) { return archLiftSupported(A); }

inline bool archELFPatchSupported(Arch A) { return archLiftSupported(A); }

constexpr uint64_t kPageSize4K = 0x1000;
constexpr uint64_t kPageSize16K = 0x4000;
constexpr uint64_t kSyntheticStackAlignment = 16;
/// The widest atomic access a target performs, a 16-byte compare-and-swap:
/// the most alignment an atomically accessed image object needs in C.
constexpr uint64_t kMaxAtomicAccessBytes = 16;

/// Residue of the ABI entry stack pointer modulo the synthetic stack alignment.
/// x86-64 includes the pushed return address.  i386 enters at 12 mod 16 under
/// the System V psABI, which aligns the stack to sixteen at a call, and on
/// Darwin: code its compilers build keeps a vector there with movaps.  Windows
/// aligns it to four, which any residue keeps.  A process entry point is
/// entered aligned.
constexpr uint64_t
syntheticEntryStackResidue(Arch A, BinaryFormat Fmt = BinaryFormat::Unknown,
                           StackEntryKind Kind = StackEntryKind::Call) {
  if (Kind == StackEntryKind::ProcessEntry)
    return 0;
  if (A == Arch::X64)
    return 8;
  return A == Arch::X86 &&
                 (Fmt == BinaryFormat::MachO || Fmt == BinaryFormat::ELF)
             ? 12
             : 0;
}

/// The alignment the ABI guarantees the entry stack pointer.  ARM AAPCS
/// promises eight bytes at a public interface.  i386 promises sixteen on
/// Darwin and at a System V process entry, and only four to a function
/// Windows or Linux code calls.  The synthetic buffer's larger alignment is
/// no such guarantee.
constexpr uint64_t
guaranteedEntryStackAlignment(Arch A, BinaryFormat Fmt,
                              StackEntryKind Kind = StackEntryKind::Call) {
  if (A == Arch::ARM)
    return 8;
  if (A == Arch::AArch64 || A == Arch::X64)
    return 16;
  if (A == Arch::X86 &&
      (Fmt == BinaryFormat::MachO || Kind == StackEntryKind::ProcessEntry))
    return 16;
  return 4;
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
