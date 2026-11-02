//===- RegistrationAddress.h - PE32 image address carriers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve complete PE32 addresses in native and normalized LowIR widths.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONADDRESS_H
#define NEVERD_IR_LOW_X86_REGISTRATIONADDRESS_H

#include "neverd/ir/low/LowIR.h"

#include <cstdint>
#include <optional>

namespace neverd::registration_state {

inline std::optional<uint32_t> imageAddressConstant(const NdVar &Value) {
  // Absolute memory operands use the lifter's eight-byte address container;
  // their native PE32 address must still fit without truncation.
  if (!Value.isConst() || (Value.Size != 4 && Value.Size != 8) ||
      !isExactAddressProvenance(Value.Provenance) || Value.Offset > UINT32_MAX)
    return std::nullopt;
  return static_cast<uint32_t>(Value.Offset);
}

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONADDRESS_H
