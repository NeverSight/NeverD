//===- MachORelocations.h - Mach-O object relocation helpers ---*- C++ -*-===//

#ifndef NEVERD_LOADER_MACHO_MACHORELOCATIONS_H
#define NEVERD_LOADER_MACHO_MACHORELOCATIONS_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd::macho_loader::detail {

struct I386VanillaValue {
  int64_t Target = 0;
  int64_t Addend = 0;
  uint64_t Place = 0;
  uint8_t Width = 0;
  bool IsPCRel = false;
};

std::optional<int64_t> evaluateI386Vanilla(const I386VanillaValue &R);

std::optional<int64_t>
evaluateI386SectionDifference(int64_t FinalA, int64_t FinalB, int64_t EncodedA,
                              int64_t EncodedB, int64_t Existing);

bool writeI386RelocationField(llvm::MutableArrayRef<uint8_t> Data,
                              uint64_t Offset, uint8_t Width, int64_t Value,
                              bool SignedValue);

} // namespace neverd::macho_loader::detail

#endif // NEVERD_LOADER_MACHO_MACHORELOCATIONS_H
