//===- MemoryLayout.h - Physical backing allocation policy ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_MEMORYLAYOUT_H
#define NEVERD_EMULATION_CORE_MEMORYLAYOUT_H
#include <cstdint>

namespace neverd::emulation::memory {
#define NEVERD_MEMORY_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#include "MemoryLayout.def"
#undef NEVERD_MEMORY_VALUE
} // namespace neverd::emulation::memory
#endif
