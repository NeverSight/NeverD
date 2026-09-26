//===- WindowsKernelLayout.h - Windows ABI constants ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private serialized Windows ABI constants.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWSKERNELLAYOUT_H
#define NEVERD_EMULATION_WINDOWSKERNELLAYOUT_H

#include <cstdint>

namespace neverd::emulation::windows {
#define NEVERD_WDM_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#include "KernelValues.def"
#include "WindowsKernelLayout.def"
#undef NEVERD_WDM_VALUE
} // namespace neverd::emulation::windows

#endif // NEVERD_EMULATION_WINDOWSKERNELLAYOUT_H
