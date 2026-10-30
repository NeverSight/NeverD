//===- IntrinsicTermination.h - Shared architectural termination -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_INTRINSICTERMINATION_H
#define NEVERD_IR_INTRINSICS_INTRINSICTERMINATION_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86/X86Interrupts.h"

#include <optional>

namespace neverd {

/// LLVM's AArch64 debugtrap / __debugbreak encoding. Other BRK codes retain
/// the terminating-trap contract (including Clang's #1 and GCC's #0x3E8).
inline bool isAArch64DebugBreakCode(std::optional<uint64_t> Code) {
  return Code && *Code == 0xF000;
}

/// Architectural termination, shared by LowIR and MedIR. This grants no
/// source ABI or memory contract to an intrinsic.
inline bool intrinsicNeverReturns(Intrinsic Id,
                                  std::optional<uint64_t> Operand) {
  switch (Id) {
  case Intrinsic::Brk:
    return !isAArch64DebugBreakCode(Operand);
  case Intrinsic::Hlt_A64:
  case Intrinsic::Udf_A64:
  case Intrinsic::X86Skinit:
  case Intrinsic::X86Sysenter:
    return true;
  case Intrinsic::IntN:
    return Operand && isX86NoReturnInterrupt(*Operand);
  // x86 architectural returns (far, interrupt and syscall returns) transfer
  // control to a named frame or register and never resume in the lifted
  // function.  This grants no source ABI or memory contract; it only records
  // that control leaves here.
  case Intrinsic::Retf:
  case Intrinsic::Iret:
  case Intrinsic::Sysretq:
  case Intrinsic::Sysexitq:
    return true;
  default:
    return false;
  }
}

} // namespace neverd

#endif
