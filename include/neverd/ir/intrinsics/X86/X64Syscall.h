//===- X64Syscall.h - Linux syscall ABI facts ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X64SYSCALL_H
#define NEVERD_IR_INTRINSICS_X86_X64SYSCALL_H

#include "neverd/Common.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstdint>
#include <optional>

namespace neverd {

/// The registers a Linux x86-64 syscall takes its arguments in, in order.
inline constexpr uint64_t kLinuxX64SyscallArgumentRegisters[] = {
    x86reg::RDI, x86reg::RSI, x86reg::RDX, x86reg::R10, x86reg::R8, x86reg::R9};

/// Return the number of arguments Linux syscall \p Number reads on \p Target.
/// Unknown numbers have no established arity in this table and must retain
/// all register inputs (LinuxSyscallArguments.def lists the verified ones).
constexpr std::optional<unsigned> linuxSyscallArgumentCount(Arch Target,
                                                            uint64_t Number) {
#define NEVERD_LINUX_SYSCALL_ARGUMENTS(TargetArch, Value, Count)               \
  if (Target == Arch::TargetArch && Number == Value)                           \
    return Count;
#include "neverd/ir/intrinsics/LinuxSyscallArguments.def"
  return std::nullopt;
}

/// Return the number of arguments the specified Linux x86-64 syscall reads.
constexpr std::optional<unsigned>
linuxX64SyscallArgumentCount(uint64_t Number) {
  return linuxSyscallArgumentCount(Arch::X64, Number);
}

/// Whether a Linux x86-64 syscall of \p Count arguments reads operand
/// \p Operand of its intrinsic, whose operands after the number are RDI,
/// RDX:RSI, R8:R10 and R9: the first, the second and third, the fourth and
/// fifth, and the sixth argument.  A pair is read whole once either half is.
constexpr bool x64SyscallOperandRead(size_t Operand, unsigned Count) {
  switch (Operand) {
  case 1:
    return Count >= 1;
  case 2:
    return Count >= 2;
  case 3:
    return Count >= 4;
  case 4:
    return Count >= 6;
  default:
    return true;
  }
}

/// How many of kLinuxX64SyscallArgumentRegisters, from the first, a syscall
/// of \p Count arguments reads through the operands that carry them
/// (x64SyscallOperandRead).
constexpr unsigned x64SyscallRegistersRead(unsigned Count) {
  return Count == 0 ? 0 : Count == 1 ? 1 : Count <= 3 ? 3 : Count <= 5 ? 5 : 6;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X64SYSCALL_H
