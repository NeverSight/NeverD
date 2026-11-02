//===- X86RegistrationSEHRegisters.h - Finally register state -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit architectural data-register transport across shared-frame calls.

#ifndef NEVERD_IR_MED_X86_X86REGISTRATIONSEHREGISTERS_H
#define NEVERD_IR_MED_X86_X86REGISTRATIONSEHREGISTERS_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86/X86Regs.h"

#include <array>

namespace neverd::x86_registration {

enum class SEHRegisterRole : uint8_t {
  CallInput,
  CallbackInput,
  CallbackOutput,
  CallOutput,
};

inline constexpr std::array<uint64_t, 6> SEHDataRegisters = {
    x86reg::RAX, x86reg::RCX, x86reg::RDX,
    x86reg::RBX, x86reg::RSI, x86reg::RDI};

inline bool isSEHRegisterIntrinsic(const MedOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
         Op.Inputs[0].isConst() &&
         (Op.Inputs[0].ConstVal == uint64_t(Intrinsic::X86SEHRegisterRead) ||
          Op.Inputs[0].ConstVal == uint64_t(Intrinsic::X86SEHRegisterWrite));
}

/// ID, role, callback entry, source call/return/entry address, register offset,
/// and (for writes) the current SSA value. Synthetic operations never borrow
/// the original CALL/RETURN's sequence identity.
bool hasValidSEHRegisterShape(const MedOp &Op);

/// Keep unused callback register transport from creating physical function
/// parameters. Run after SSA construction and before ABI recovery.
void discardUnusedSEHCallInputs(MedFunc &Function);

} // namespace neverd::x86_registration

#endif // NEVERD_IR_MED_X86_X86REGISTRATIONSEHREGISTERS_H
