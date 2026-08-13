//===- BinaryImageFlags.h - Segment flag conversions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Conversions between each container format's own permission bits and
/// NeverD's \ref SegmentFlags, in both directions: loaders read the native
/// spelling, codegen writes it back.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_BINARYIMAGEFLAGS_H
#define NEVERD_LOADER_BINARYIMAGEFLAGS_H

#include "neverd/Common.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"

#include <cstdint>

namespace neverd {

// ===--------------------------------------------------------------------===//
// Flag conversion helpers — shared by all loaders
// ===--------------------------------------------------------------------===//

inline SegmentFlags coffFlagsToNd(uint32_t Ch) {
  auto F = SegmentFlags::None;
  if (Ch & llvm::COFF::IMAGE_SCN_MEM_READ)
    F = F | SegmentFlags::Readable;
  if (Ch & llvm::COFF::IMAGE_SCN_MEM_WRITE)
    F = F | SegmentFlags::Writable;
  if (Ch & llvm::COFF::IMAGE_SCN_MEM_EXECUTE)
    F = F | SegmentFlags::Executable;
  return F;
}

inline SegmentFlags elfPFlagsToNd(uint32_t PFlags) {
  auto F = SegmentFlags::None;
  if (PFlags & llvm::ELF::PF_R)
    F = F | SegmentFlags::Readable;
  if (PFlags & llvm::ELF::PF_W)
    F = F | SegmentFlags::Writable;
  if (PFlags & llvm::ELF::PF_X)
    F = F | SegmentFlags::Executable;
  return F;
}

inline SegmentFlags elfSHFlagsToNd(uint64_t SHFlags) {
  auto F = SegmentFlags::None;
  if (SHFlags & llvm::ELF::SHF_ALLOC)
    F = F | SegmentFlags::Readable;
  if (SHFlags & llvm::ELF::SHF_WRITE)
    F = F | SegmentFlags::Writable;
  if (SHFlags & llvm::ELF::SHF_EXECINSTR)
    F = F | SegmentFlags::Executable;
  return F;
}

inline SegmentFlags machoProtToNd(uint32_t Prot) {
  auto F = SegmentFlags::None;
  if (Prot & llvm::MachO::VM_PROT_READ)
    F = F | SegmentFlags::Readable;
  if (Prot & llvm::MachO::VM_PROT_WRITE)
    F = F | SegmentFlags::Writable;
  if (Prot & llvm::MachO::VM_PROT_EXECUTE)
    F = F | SegmentFlags::Executable;
  return F;
}

// ===--------------------------------------------------------------------===//
// Reverse flag conversions — used by codegen to write format-native flags
// ===--------------------------------------------------------------------===//

inline uint32_t ndToCoffFlags(SegmentFlags F) {
  uint32_t Ch = 0;
  if (hasFlag(F, SegmentFlags::Readable))
    Ch |= llvm::COFF::IMAGE_SCN_MEM_READ;
  if (hasFlag(F, SegmentFlags::Writable))
    Ch |= llvm::COFF::IMAGE_SCN_MEM_WRITE;
  if (hasFlag(F, SegmentFlags::Executable))
    Ch |= llvm::COFF::IMAGE_SCN_MEM_EXECUTE;
  return Ch;
}

inline uint32_t ndToElfPFlags(SegmentFlags F) {
  uint32_t PF = 0;
  if (hasFlag(F, SegmentFlags::Readable))
    PF |= llvm::ELF::PF_R;
  if (hasFlag(F, SegmentFlags::Writable))
    PF |= llvm::ELF::PF_W;
  if (hasFlag(F, SegmentFlags::Executable))
    PF |= llvm::ELF::PF_X;
  return PF;
}

inline uint32_t ndToMachoProt(SegmentFlags F) {
  uint32_t Prot = 0;
  if (hasFlag(F, SegmentFlags::Readable))
    Prot |= llvm::MachO::VM_PROT_READ;
  if (hasFlag(F, SegmentFlags::Writable))
    Prot |= llvm::MachO::VM_PROT_WRITE;
  if (hasFlag(F, SegmentFlags::Executable))
    Prot |= llvm::MachO::VM_PROT_EXECUTE;
  return Prot;
}

} // namespace neverd

#endif // NEVERD_LOADER_BINARYIMAGEFLAGS_H
