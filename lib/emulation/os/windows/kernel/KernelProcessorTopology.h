//===- KernelProcessorTopology.h - The bounded scheduler's processor facts ===//
// NeverD Decompiler
///
/// \file
/// The bounded scheduler's processor facts.
///

#ifndef NEVERD_KERNEL_PROCESSOR_TOPOLOGY_H
#define NEVERD_KERNEL_PROCESSOR_TOPOLOGY_H
#include <cstdint>
namespace neverd::emulation::processor {
// The executor, scheduler and isolated native thread constructor implement
// one active processor. These are guest facts, independent of host topology.
inline constexpr uint32_t ActiveCount = 1, CurrentIndex = 0;
inline constexpr uint16_t GroupCount = 1, CurrentGroup = 0;
inline constexpr uint8_t CurrentNumber = 0;
inline constexpr uint64_t ActiveMask = 1;
inline constexpr uint64_t NativeAffinityHeader19041 = 0x140000 | GroupCount;
} // namespace neverd::emulation::processor
#endif
