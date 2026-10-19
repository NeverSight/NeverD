//===- KernelSectionNames.h - Built-in section namespace identity ---------===//
// NeverD Decompiler
///
/// \file
/// Built-in section namespace identity.
///

#ifndef NEVERD_EMULATION_KERNELSECTIONNAMES_H
#define NEVERD_EMULATION_KERNELSECTIONNAMES_H
#include "llvm/ADT/StringRef.h"

#include <cstdint>
namespace neverd::emulation::section {
inline constexpr llvm::StringLiteral PhysicalName = "\\Device\\PhysicalMemory";
inline uint32_t accessMask(uint32_t Access) {
  uint32_t R = Access & 0x0fffffff;
  if (Access & 0x80000000)
    R |= 0x20005;
  if (Access & 0x40000000)
    R |= 0x20002;
  if (Access & 0x20000000)
    R |= 0x20008;
  if (Access & 0x10000000)
    R |= 0xf001f;
  if (R & 0x02000000)
    R = (R & ~0x02000000) | 0x1f001f;
  return R;
}
} // namespace neverd::emulation::section
#endif
