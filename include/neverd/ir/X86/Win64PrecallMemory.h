//===- Win64PrecallMemory.h - Original CALL memory bindings -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The ordinary C projection can borrow one original pre-CALL state. Flat
/// V6 RAM and a captured virtual-address window have different pointer rules.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_WIN64PRECALLMEMORY_H
#define NEVERD_IR_X86_WIN64PRECALLMEMORY_H

#include <cstdint>

namespace neverd {
enum class Win64PrecallMemory : uint8_t { None, FlatV6, WindowV6 };
} // namespace neverd

#endif // NEVERD_IR_X86_WIN64PRECALLMEMORY_H
