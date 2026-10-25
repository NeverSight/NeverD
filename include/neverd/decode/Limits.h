//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
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

// lib/decode/Decoder.cpp
inline constexpr size_t kDecoderMaxSpare = 16;
inline constexpr size_t kScanBatchInstructions = 64;

} // namespace neverd::limits::decode

#endif // NEVERD_DECODE_LIMITS_H
