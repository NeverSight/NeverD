//===- X64Syscall.h - Linux x64 syscall ABI facts --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X64SYSCALL_H
#define NEVERD_IR_INTRINSICS_X64SYSCALL_H

#include <cstdint>
#include <optional>

namespace neverd {

/// Return the number of arguments the specified Linux x86-64 syscall reads.
/// Unknown numbers have no established arity in this table and must retain
/// all register inputs. Keep this list limited to calls whose ABI is verified.
constexpr std::optional<unsigned>
linuxX64SyscallArgumentCount(uint64_t Number) {
  switch (Number) {
  case 60:  // exit(status)
  case 231: // exit_group(status)
    return 1;
  default:
    return std::nullopt;
  }
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X64SYSCALL_H
