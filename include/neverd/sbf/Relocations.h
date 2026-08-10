//===- Relocations.h - Solana SBF relocation metadata --------*- C++ -*-===//

#ifndef NEVERD_SBF_RELOCATIONS_H
#define NEVERD_SBF_RELOCATIONS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <limits>

namespace neverd::sbf {

enum class Relocation : uint32_t {
#define SBF_RELOCATION(ID, VALUE, NAME, WIDTH_BITS, PURPOSE) ID = VALUE,
#include "neverd/sbf/SBFRelocations.def"
  Unknown = std::numeric_limits<uint32_t>::max(),
};

enum class RelocationPurpose : uint8_t { Absolute, Relative, Call };

struct RelocationInfo {
  Relocation ID;
  uint32_t Value;
  llvm::StringLiteral Name;
  uint8_t Width;
  RelocationPurpose Purpose;
};

llvm::ArrayRef<RelocationInfo> relocationInfos();
const RelocationInfo *getRelocationInfo(uint32_t Value);

} // namespace neverd::sbf

#endif // NEVERD_SBF_RELOCATIONS_H
