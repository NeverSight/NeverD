//===- X86DebugRegisterRules.h - Debug register transfers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared nontransactional MOV-DR access and register normalization rules.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_X86_X86DEBUGREGISTERRULES_H
#define NEVERD_IR_X86_X86DEBUGREGISTERRULES_H

#include "neverd/ir/X86/X86DebugRules.h"

#include <cstdint>

namespace neverd {

struct X86DebugRegisterAccess {
  uint8_t Debug, General;
  bool Write, InvalidOpcode;
};

enum class X86DebugRegisterResult : uint8_t {
  Completed,
  UndefinedOpcode,
  GeneralProtection,
  GeneralDetect,
  UnsupportedControl,
};

/// Requires DebugRegisters or Watchpoints. The target preserves the six
/// architecturally undefined arithmetic flags, has no RTM/bus-lock debug
/// extensions. Comparator admission follows the selected profile. A valid
/// write requesting unbound controls refuses the environment; it is not #GP.
/// Call after authenticated fetch. Value has the execution mode's GPR width.
template <typename Bank, typename Word>
inline X86DebugRegisterResult
x86AccessDebugRegister(Bank &State, Word &Value, X86DebugRegisterAccess Access,
                       uint64_t CR4, uint8_t CPL,
                       X86InstructionDebugProfile Profile =
                           X86InstructionDebugProfile::DebugRegisters) {
  using Result = X86DebugRegisterResult;
  const bool Alias = Access.Debug == 4 || Access.Debug == 5;
  if (Access.InvalidOpcode || (Alias && (CR4 & 8)))
    return Result::UndefinedOpcode;
  if (CPL)
    return Result::GeneralProtection;
  if (State.DR7 & 0x2000)
    return Result::GeneralDetect;
  const unsigned Index = Alias ? Access.Debug + 2 : Access.Debug;
  if (Access.Write) {
    if (Index >= 6 && (uint64_t(Value) >> 32))
      return Result::GeneralProtection;
    if (Index < 4) {
      State.DR[Index] = Value;
    } else if (Index == 6) {
      State.DR6 = (Value & 0xe00f) | 0xffff0ff0;
    } else {
      const uint64_t Next = (Value & 0xffff2bff) | 0x400;
      if (!x86InstructionDebugValid(State.DR6, Next, State.DebugCTL, Profile,
                                    CR4))
        return Result::UnsupportedControl;
      State.DR7 = Next;
    }
  } else {
    Value = Index < 4 ? State.DR[Index] : Index == 6 ? State.DR6 : State.DR7;
  }
  return Result::Completed;
}

} // namespace neverd
#endif // NEVERD_IR_X86_X86DEBUGREGISTERRULES_H
