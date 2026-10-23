//===- RegistrationHelperExpansion.h - Physical helper effects --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Expand authenticated linear helpers without losing physical CALL/RET
/// effects.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_REGISTRATIONHELPEREXPANSION_H
#define NEVERD_IR_MED_X86_REGISTRATIONHELPEREXPANSION_H

#include "neverd/ir/low/LowIR.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {
struct BinaryImage;
namespace x86_registration {

std::optional<uint64_t> nextRegistrationHelperTemp(const LowFunc &Source);

/// The caller must authenticate this exact source call and helper range.
/// Fresh decoding supplies the effects; scratch byte storage is disjoint from
/// all source operations and previous expansions. No partial result escapes.
std::optional<std::pair<std::vector<LowOp>, uint64_t>>
expandRegistrationHelper(const BinaryImage &Image, va_t SourceEntry,
                         va_t CallVA, va_t ReturnVA,
                         ExceptionAddressRange Helper, uint64_t TempId);

} // namespace x86_registration
} // namespace neverd

#endif // NEVERD_IR_MED_X86_REGISTRATIONHELPEREXPANSION_H
