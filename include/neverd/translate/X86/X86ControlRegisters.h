//===- X86ControlRegisters.h - Intel paging controls ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Architectural control-write checks, before memory-domain admission or RAM.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86CONTROLREGISTERS_H
#define NEVERD_TRANSLATE_X86_X86CONTROLREGISTERS_H

#include "neverd/translate/X86/X86Paging.h"

#include <cstdint>
#include <optional>

namespace neverd::translate {

inline constexpr uint32_t kX86CR0StoredMask = 0xe005003f;

uint32_t x86SupportedCR4(const X86PagingFeatures &Features,
                         uint32_t OtherFeatures);

struct X86PagingControlWrite {
  X86PagingState Controls;
  std::optional<X86MemoryException> Exception;
  bool ReloadPDPTE = false;
};

/// CR0/CR3/CR4 source values have already been selected at the instruction's
/// width. Code64 and TR16 describe the current CS/TR, not the requested mode.
/// LMA is derived only here. This operation performs no accesses and does not
/// admit an execution or memory profile. Its consumer must validate the target
/// domain, load legacy PDPTEs if requested, then publish atomically.
X86PagingControlWrite
planX86PagingControlWrite(const X86PagingState &Old,
                          const X86PagingFeatures &Features,
                          uint32_t OtherCR4Features, unsigned Control,
                          uint64_t Value, bool Code64, bool TR16);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86CONTROLREGISTERS_H
