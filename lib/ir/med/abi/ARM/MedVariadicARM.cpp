//===- MedVariadicARM.cpp - AAPCS variadic prologue -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The ARM AAPCS variadic prologue recognizer: the GP argument register
/// save area that abuts the entry stack pointer.
///
//===----------------------------------------------------------------------===//

#include "../MedVariadicDetail.h"

namespace neverd {
namespace med_variadic_detail {

/// R0-R3 spilled with R3 just below the entry stack pointer.
bool hasAAPCS32VariadicPrologue(VariadicScan &S) {
  const MedFunc &Func = S.Func;
  const TargetRegInfo &TRI = S.TRI;
  bool Marked = false;
  // ARM32 (softfp) has no FP save area: the GP argument registers are spilled
  // to a save area abutting the entry SP so va_arg walks them then the
  // incoming stack arguments contiguously.  The AAPCS save area is laid out
  // so the LAST parameter register (r3) sits in the slot just below the entry
  // SP (entry_sp - slot), making r0..r3 contiguous with the overflow area at
  // [entry_sp + 0].  Requiring specifically the *last* parameter register
  // there — not just any — distinguishes a real save area from an ordinary
  // -O0 callee that happens to spill its FIRST argument register (r0) into
  // the top frame slot (r0 at entry_sp-slot, r3 at the bottom), which is the
  // reverse order and must NOT be read as variadic (else its incoming stack
  // arguments are dropped; see OptStress312).
  const int LastArgIdx = static_cast<int>(TRI.IntParamRegs.size()) - 1;
  bool AbutsEntry = false;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.Inputs[1].Kind == MedVar::Reg && Op.Inputs[1].SSAVer == 0 &&
          LastArgIdx >= 0 && TRI.regToArgIdx(Op.Inputs[1].RegOff) == LastArgIdx)
        if (auto D = entrySpDelta(S, Op.Inputs[0], 0))
          if (*D == -TRI.PointerSize)
            AbutsEntry = true;
  Marked = AbutsEntry && countParamRegSpills(Func, TRI.IntParamRegs) >= 2;
  return Marked;
}

/// The integer argument register the AAPCS32 save area starts at: the
/// prologue spills each argument register from the first unnamed one through
/// r3, r3 just below the entry stack pointer, so va_arg walks on into the
/// incoming stack arguments.  The registers before it carry named
/// parameters.
std::optional<int> aapcs32FirstVariadicRegister(VariadicScan &S) {
  return firstSavedArgumentRegister(S,
                                    -static_cast<int64_t>(S.TRI.PointerSize));
}

} // namespace med_variadic_detail
} // namespace neverd
