//===- SBFProgramImage.h - Canonical Solana SBF VM image --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the immutable post-load, post-relocation SBF program image. All
/// decoders, interpreters, and emitters consume this representation so text and
/// data bytes cannot diverge after relocation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_IMAGE_SBFPROGRAMIMAGE_H
#define NEVERD_SBF_IMAGE_SBFPROGRAMIMAGE_H

#include "neverd/Common.h"
#include "neverd/sbf/SBFMetadata.h"
#include "neverd/sbf/runtime/SBFVMConfig.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;

namespace sbf {

enum class ProgramRegionKind : uint8_t {
  Bytecode,
  ReadOnly,
  LegacyReadOnly,
};

struct ProgramSectionSpan {
  std::string Name;
  size_t Offset = 0;
  size_t Size = 0;
  bool Executable = false;
};

struct ProgramRegion {
  va_t Address = 0;
  std::vector<uint8_t> Bytes;
  ProgramRegionKind Kind = ProgramRegionKind::ReadOnly;
  bool DataVisible = true;
  std::string Name;
  std::vector<ProgramSectionSpan> Sections;

  bool contains(va_t Start, size_t Size = 1) const;
};

class ProgramImage {
public:
  static constexpr size_t NoRegion = std::numeric_limits<size_t>::max();

  llvm::ArrayRef<ProgramRegion> regions() const { return Regions; }
  llvm::ArrayRef<uint8_t> text() const;
  va_t textAddress() const { return TextAddress; }
  bool empty() const { return Regions.empty(); }

  const ProgramRegion *findRegion(va_t Address, size_t Size = 1,
                                  bool DataAccess = false) const;
  llvm::Expected<llvm::ArrayRef<uint8_t>> slice(va_t Address, size_t Size,
                                                bool DataAccess = false) const;

private:
  friend llvm::Expected<ProgramImage>
  buildProgramImage(const BinaryImage &, const Metadata &, const SBFVMConfig &);
  friend llvm::Expected<ProgramImage>
  createProgramImage(llvm::ArrayRef<uint8_t>, va_t, llvm::ArrayRef<uint8_t>,
                     va_t, bool);

  llvm::Error finalize(va_t Address, size_t Size);

  std::vector<ProgramRegion> Regions;
  size_t TextRegion = NoRegion;
  size_t TextOffset = 0;
  size_t TextSize = 0;
  va_t TextAddress = 0;
};

/// Build the canonical runtime image from a loaded SBF ELF and apply every
/// legacy relocation exactly once.
llvm::Expected<ProgramImage> buildProgramImage(const BinaryImage &Image,
                                               const Metadata &Metadata,
                                               const SBFVMConfig &Config = {});

/// Construct a checked synthetic image for unit tests and backend fixtures.
llvm::Expected<ProgramImage>
createProgramImage(llvm::ArrayRef<uint8_t> Text, va_t TextAddress,
                   llvm::ArrayRef<uint8_t> Rodata = {}, va_t RodataAddress = 0,
                   bool LegacyTextIsDataVisible = false);

} // namespace sbf
} // namespace neverd

#endif // NEVERD_SBF_IMAGE_SBFPROGRAMIMAGE_H
