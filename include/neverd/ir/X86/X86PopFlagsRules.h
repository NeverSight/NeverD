//===- X86PopFlagsRules.h - Protected POPF masks --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Writable flags for protected POPF/POPFD/POPFQ outside virtual-8086 mode.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_X86_X86POPFLAGSRULES_H
#define NEVERD_IR_X86_X86POPFLAGSRULES_H

#include <cstdint>

namespace neverd {
inline uint64_t x86PopFlagsMask(unsigned CPL, uint64_t EntryFlags,
                                unsigned Bytes) {
  // Entry IOPL controls IF even when this pop also changes IOPL. PVI does not
  // redirect POPF to VIF. Word forms preserve AC/ID; RF clears on completion.
  return 0x4dd5 | (CPL == 0 ? 0x3000 : 0) |
         (CPL <= ((EntryFlags >> 12) & 3) ? 0x200 : 0) |
         (Bytes != 2 ? 0x240000 : 0);
}
} // namespace neverd
#endif // NEVERD_IR_X86_X86POPFLAGSRULES_H
