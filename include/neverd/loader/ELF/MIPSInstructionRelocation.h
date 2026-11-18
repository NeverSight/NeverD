//===- MIPSInstructionRelocation.h - Symbolic immediate fields -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Loader-authenticated split symbol addresses for source reconstruction.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_MIPSINSTRUCTIONRELOCATION_H
#define NEVERD_LOADER_ELF_MIPSINSTRUCTIONRELOCATION_H

#include "neverd/Common.h"

#include <cstdint>
#include <optional>

namespace neverd {

enum class MIPSImmediatePart : uint8_t {
#define NEVERD_MIPS_IMMEDIATE_RELOCATION(Part, Type, Shift, Carry) Part,
#include "neverd/loader/ELF/MIPSImmediateRelocations.def"
};

struct MIPSImmediateExtraction {
  unsigned Shift;
  uint64_t Carry;

  uint16_t apply(uint64_t Value) const {
    return uint16_t((Value + Carry) >> Shift);
  }
};

constexpr MIPSImmediateExtraction extractionOf(MIPSImmediatePart Part) {
  switch (Part) {
#define NEVERD_MIPS_IMMEDIATE_RELOCATION(Part, Type, Shift, Carry)             \
  case MIPSImmediatePart::Part:                                                \
    return {Shift, Carry};
#include "neverd/loader/ELF/MIPSImmediateRelocations.def"
  }
  return {};
}

struct MIPSSymbolImmediate {
  /// Serialized callable value, including the local instruction-mode tag.
  va_t SymbolValue = 0;
  int64_t Addend = 0;
  uint32_t Instruction = 0;
  MIPSImmediatePart Part = MIPSImmediatePart::Lo16;
};

struct MIPSInstructionRelocation {
  va_t Address = 0;
  /// A missing value records a claimed field whose source semantics could not
  /// be authenticated. Source conversion must refuse that occurrence, while
  /// ordinary decoding and physical lifting keep the original bytes.
  std::optional<MIPSSymbolImmediate> Value;
};

} // namespace neverd

#endif // NEVERD_LOADER_ELF_MIPSINSTRUCTIONRELOCATION_H
