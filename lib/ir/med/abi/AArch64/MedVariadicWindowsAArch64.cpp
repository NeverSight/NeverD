//===- MedVariadicWindowsAArch64.cpp - Windows ARM64 va_start ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The Windows ARM64 variadic prologue recognizer.  Its va_list is a plain
/// pointer: va_start spills x(first) through x7 to consecutive doublewords
/// that end at the entry stack pointer and hands on the address of the first,
/// so va_arg walks the saved registers and then the caller's stack arguments
/// as one array.
///
//===----------------------------------------------------------------------===//

#include "../MedVariadicDetail.h"

#include "llvm/ADT/STLExtras.h"

#include <cstdint>
#include <optional>
#include <set>

namespace neverd {
namespace med_variadic_detail {

namespace {
/// Whether \p Op passes its operand \p Input on rather than only addressing
/// memory with it: stored as a value, or an operand of a call or a return.
bool passesOn(const MedOp &Op, unsigned Input) {
  switch (Op.Opcode) {
  case NdOp::STORE:
    return Input == 1;
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
    return true;
  default:
    return false;
  }
}
} // namespace

std::optional<int> windowsAArch64FirstVariadicRegister(VariadicScan &S) {
  const TargetRegInfo &TRI = S.TRI;
  const int Registers = static_cast<int>(TRI.IntParamRegs.size());
  const int64_t Slot = TRI.PointerSize;
  if (!Registers || !Slot)
    return std::nullopt;
  // x7 in the doubleword just below the entry stack pointer, each register
  // before it in the doubleword below the next's.
  const int Saved = firstSavedArgumentRegister(S, -Slot);
  if (Saved >= Registers)
    return std::nullopt;
  // The va_list: the address of the first unnamed register's slot, passed
  // on.  A named parameter homed where it continues the run keeps its
  // address in the function, as an -O0 function homing its parameters in
  // that layout keeps all of theirs.
  std::set<int64_t> PassedOn;
  const auto Note = [&](const MedVar &V) {
    if (const auto Delta = entrySpDelta(S, V, 0))
      PassedOn.insert(*Delta);
  };
  for (const MedBlock &Block : S.Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      // Formed in an argument register (`add x3, sp, #off`) for a call.
      if (Op.Output.Kind == MedVar::Reg &&
          llvm::is_contained(TRI.IntParamRegs, Op.Output.RegOff))
        Note(Op.Output);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (passesOn(Op, I))
          Note(Op.Inputs[I]);
    }
  for (int First = Saved; First < Registers; ++First)
    if (PassedOn.count(-Slot * (Registers - First)))
      return First;
  return std::nullopt;
}

} // namespace med_variadic_detail
} // namespace neverd
