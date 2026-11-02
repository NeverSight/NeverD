//===- Limits.h - Component limits and defaults ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_LIMITS_H
#define NEVERD_DECODE_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::decode {

/// Closed x86 decoder handles retained for reuse across decoder instances.
inline constexpr size_t kDecoderMaxSpare = 16;

} // namespace neverd::limits::decode

#endif // NEVERD_DECODE_LIMITS_H
