//===- X86CompatibilityReturnUndefined.h - Mode-return bits ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Arbitrary legacy GPR high halves on a 64-bit to compatibility-mode return.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86COMPATIBILITYRETURNUNDEFINED_H
#define NEVERD_LIB_LIFT_X86_X86COMPATIBILITYRETURNUNDEFINED_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowUndefinedEffects.h"
#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace neverd::compatibilityreturn {

// Intel SDM Vol. 1, 3.4.1.1: legacy GPR high halves are not preserved across
// this mode switch. R8-R15 have a separate preservation guarantee.
inline constexpr std::array<unsigned, 8> Registers = {
    x86reg::RAX, x86reg::RCX, x86reg::RDX, x86reg::RBX,
    x86reg::RSP, x86reg::RBP, x86reg::RSI, x86reg::RDI};

inline void record(X86Lifter::LiftState &S) {
  for (auto Register : Registers)
    S.recordUndefinedBits(NdVar::reg(Register, 8), 32, 32);
}

inline bool matches(llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  if (Ops.size() != 1 || Ops[0].Opcode != NdOp::INTRINSIC ||
      Ops[0].NumInputs != 3 || Ops[0].Output.Size ||
      Effects.Effects.size() != Registers.size())
    return false;
  const bool Sysret =
      Ops[0].Inputs[0] == NdVar::cst(uint64_t(Intrinsic::Sysretq), 2) &&
      Ops[0].Inputs[1] == NdVar::reg(x86reg::RCX, 4) &&
      Ops[0].Inputs[2] == NdVar::reg(x86reg::R11, 4);
  const bool Sysexit =
      Ops[0].Inputs[0] == NdVar::cst(uint64_t(Intrinsic::Sysexitq), 2) &&
      Ops[0].Inputs[1] == NdVar::reg(x86reg::RDX, 4) &&
      Ops[0].Inputs[2] == NdVar::reg(x86reg::RCX, 4);
  if (!Sysret && !Sysexit)
    return false;
  for (size_t I = 0; I != Registers.size(); ++I) {
    const auto &E = Effects.Effects[I];
    if (E.AfterOp != 1 || E.Output != NdVar::reg(Registers[I], 8) ||
        E.BitOffset != 32 || E.BitCount != 32 || E.When)
      return false;
  }
  return true;
}

} // namespace neverd::compatibilityreturn

#endif // NEVERD_LIB_LIFT_X86_X86COMPATIBILITYRETURNUNDEFINED_H
