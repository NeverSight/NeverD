//===- Relocations.cpp - Solana SBF relocation metadata -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/Relocations.h"

#include <array>

namespace neverd::sbf {
namespace {

constexpr std::array RelocationTable = {
#define SBF_RELOCATION(ID, VALUE, NAME, WIDTH_BITS, PURPOSE)                   \
  RelocationInfo{Relocation::ID, VALUE, NAME, WIDTH_BITS,                      \
                 RelocationPurpose::PURPOSE},
#include "neverd/sbf/SBFRelocations.def"
};

} // namespace

llvm::ArrayRef<RelocationInfo> relocationInfos() { return RelocationTable; }

const RelocationInfo *getRelocationInfo(uint32_t Value) {
  for (const RelocationInfo &Info : RelocationTable)
    if (Info.Value == Value)
      return &Info;
  return nullptr;
}

} // namespace neverd::sbf
