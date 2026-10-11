//===- Image.h - Container-neutral input image facts ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What every layer may know about an input without knowing its container:
/// where it is mapped, where it starts and which bytes back each mapped
/// extent. A format module derives its own image class from InputImage and
/// keeps everything container specific there; the modules that understand
/// the container reach it with llvm::dyn_cast.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_CORE_IMAGE_H
#define NEVERD_UNPACK_CORE_IMAGE_H

#include "UnpackInternal.h"

namespace neverd::unpack {
enum class ExecutionDomain { User, Kernel };
/// One extent the container maps: a section, or a segment in a container
/// that maps segments. Extents are ordered by address and do not overlap.
struct ImageRegion {
  std::string Name;
  uint64_t RVA, MemorySize;
  /// The file bytes mapped at the start of the extent; the rest is zero fill.
  uint64_t FileOffset, FileSize;
};

/// Validated facts of one input file. The image borrows the file bytes, which
/// must outlive it.
class InputImage {
public:
  virtual ~InputImage();
  FormatKind format() const { return Format; }
  emulation::GuestArchitecture architecture() const { return Architecture; }
  ExecutionDomain domain() const { return Domain; }
  llvm::ArrayRef<uint8_t> file() const { return File; }
  /// The address the image is linked for.
  uint64_t preferredBase() const { return Base; }
  /// Bytes of address space the mapped image occupies.
  uint64_t extent() const { return Extent; }
  uint64_t entryRVA() const { return EntryRVA; }
  llvm::ArrayRef<ImageRegion> regions() const { return Regions; }
  /// The extent that contains \p RVA, if any.
  const ImageRegion *regionAt(uint64_t RVA) const;
  /// File bytes mapped from \p RVA to the end of their extent's file data.
  /// Empty when \p RVA is not file backed.
  llvm::ArrayRef<uint8_t> fileBytesFrom(uint64_t RVA) const;

protected:
  InputImage(FormatKind Format, llvm::ArrayRef<uint8_t> File)
      : Format(Format), File(File) {}
  emulation::GuestArchitecture Architecture = emulation::GuestArchitecture::X64;
  ExecutionDomain Domain = ExecutionDomain::User;
  uint64_t Base = 0, Extent = 0, EntryRVA = 0;
  std::vector<ImageRegion> Regions;

private:
  const FormatKind Format;
  const llvm::ArrayRef<uint8_t> File;
};
} // namespace neverd::unpack
#endif
