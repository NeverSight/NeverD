//===- MachOArm64e.h - Authenticated Mach-O pointer layouts -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Typed views of MachOArm64e.def shared by chained and object fixup readers.
/// These fields describe signing recipes, not process-signed pointers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_MACHOARM64E_H
#define NEVERD_LOADER_MACHO_MACHOARM64E_H

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/MathExtras.h"

#include <cstdint>
#include <optional>

namespace neverd::macho_loader::arm64e {

struct PointerField {
  unsigned Low;
  unsigned Width;

  constexpr uint64_t extract(uint64_t Word) const {
    return (Word >> Low) & llvm::maskTrailingOnes<uint64_t>(Width);
  }
  int64_t signExtend(uint64_t Word) const {
    return llvm::SignExtend64(extract(Word), Width);
  }
};

#define NEVERD_MACHO_ARM64E_FIELD(Name, Low, Width)                            \
  inline constexpr PointerField Name{Low, Width};
#define NEVERD_MACHO_ARM64E_VALUE(Type, Name, Value)                           \
  inline constexpr Type Name = Value;
#define NEVERD_MACHO_ARM64E_TEXT(Name, Text)                                   \
  inline constexpr char Name[] = Text;
#define NEVERD_MACHO_ARM64E_SUBTYPE(Name, Value, Label)                        \
  inline constexpr uint32_t Name = Value;
#include "neverd/loader/MachO/MachOArm64e.def"

inline bool isSubtype(uint32_t Subtype) {
  switch (Subtype & ~uint32_t(llvm::MachO::CPU_SUBTYPE_MASK)) {
#define NEVERD_MACHO_ARM64E_SUBTYPE(Name, Value, Label) case Name:
#include "neverd/loader/MachO/MachOArm64e.def"
    return true;
  default:
    return false;
  }
}

struct ChainFormat {
  unsigned Stride;
  bool Offset;
  unsigned OrdinalBits;
  bool Segmented = false;
};

inline std::optional<ChainFormat> chainFormat(uint16_t Format) {
  switch (Format) {
#define NEVERD_MACHO_ARM64E_FORMAT(Name, Stride, Offset, OrdinalBits)          \
  case llvm::MachO::Name:                                                      \
    return ChainFormat{Stride, Offset, OrdinalBits};
#define NEVERD_MACHO_ARM64E_SEGMENTED_FORMAT(Name, Stride)                     \
  case Name:                                                                   \
    return ChainFormat{Stride, false, 0, true};
#include "neverd/loader/MachO/MachOArm64e.def"
  default:
    return std::nullopt;
  }
}

} // namespace neverd::macho_loader::arm64e

#endif // NEVERD_LOADER_MACHO_MACHOARM64E_H
