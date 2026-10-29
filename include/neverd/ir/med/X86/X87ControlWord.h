//===- X87ControlWord.h - The x87 control word register ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The x87 control word as MedIR names it.  The value a function reads before
/// writing it is the one it was entered with: state the unit itself holds,
/// which source output reads back with FNSTCW instead of leaving unavailable.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_X87CONTROLWORD_H
#define NEVERD_IR_MED_X86_X87CONTROLWORD_H

#include "neverd/Common.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd {

/// Whether \p V is the x87 control word of an \p TheArch function.
inline bool isX87ControlWord(Arch TheArch, const MedVar &V) {
  return (TheArch == Arch::X86 || TheArch == Arch::X64) &&
         V.Kind == MedVar::Reg && V.RegOff == x86reg::FPU_CW && V.Size == 2;
}

} // namespace neverd

#endif // NEVERD_IR_MED_X86_X87CONTROLWORD_H
