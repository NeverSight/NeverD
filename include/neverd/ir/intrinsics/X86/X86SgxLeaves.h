//===- X86SgxLeaves.h - SGX leaf register dependencies --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Leaf-specific register reads become explicit conditional LowIR inputs.
/// Backends consume those inputs without inferring or preloading SGX memory.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86SGXLEAVES_H
#define NEVERD_IR_INTRINSICS_X86_X86SGXLEAVES_H

#include <array>
#include <cstdint>

namespace neverd {

// rBX/rCX/rDX read sets from the SDM leaf tables. EDBGRD also consumes the
// old rBX because a failed completion leaves it unchanged. Registers unused
// by a leaf must not invent source parameters or evaluate an undefined input.
inline constexpr std::array<uint32_t, 3> X86EnclsInputLeafMasks = {
    0x000dedf7, 0x000fffff, 0x000c0984};
inline constexpr std::array<uint32_t, 3> X86EnclvInputLeafMasks = {3, 7, 4};
inline constexpr unsigned X86SgxLeafMaskBits = 32;
inline constexpr unsigned X86SgxDebugReadLeaf = 4;

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86SGXLEAVES_H
