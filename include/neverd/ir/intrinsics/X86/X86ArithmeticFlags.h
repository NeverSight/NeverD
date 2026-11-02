//===- X86ArithmeticFlags.h - Native arithmetic flag transport -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// LAHF bits occupy AH and OF occupies AL bit zero. Reserved bits are zero.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86ARITHMETICFLAGS_H
#define NEVERD_IR_INTRINSICS_X86_X86ARITHMETICFLAGS_H

#include <array>
#include <cstdint>

namespace neverd {
inline constexpr uint16_t X86ArithmeticFlagsMask = 0xd501;
inline constexpr uint16_t X86ArithmeticFlagsBytes = 2;
// CF, PF, AF, ZF, SF, OF, in the order used by the architecture adapters.
inline constexpr std::array<unsigned, 6> X86ArithmeticFlagBits = {8,  10, 12,
                                                                  14, 15, 0};
} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86ARITHMETICFLAGS_H
