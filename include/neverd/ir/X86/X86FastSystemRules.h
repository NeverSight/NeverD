//===- X86FastSystemRules.h - Fixed fast-transfer state --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Common cache fields, selector offsets and flags for fast system transfers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86FASTSYSTEMRULES_H
#define NEVERD_IR_X86_X86FASTSYSTEMRULES_H

#include <cstdint>

namespace neverd::detail {

constexpr uint16_t x86FlatSystemAttributes(bool Code, bool Wide, unsigned CPL) {
  return (Code ? (Wide ? 0xa09b : 0xc09b) : 0xc093) | (CPL << 5);
}

constexpr unsigned x86FastSystemSelectorOffset(bool Enter, bool Wide,
                                               bool Code) {
  return (Enter ? 0 : Wide ? 32 : 16) + (Code ? 0 : 8);
}

constexpr uint32_t x86FastSystemClearedFlags(bool Enter) {
  return Enter ? 0x30200 : 0x10000;
}

} // namespace neverd::detail

#endif // NEVERD_IR_X86_X86FASTSYSTEMRULES_H
