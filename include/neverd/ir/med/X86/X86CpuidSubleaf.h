//===- X86CpuidSubleaf.h - CPUID's ignored sub-leaf -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Drop the ECX operand of a CPUID whose constant leaf ignores it.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_X86CPUIDSUBLEAF_H
#define NEVERD_IR_MED_X86_X86CPUIDSUBLEAF_H

namespace neverd {
struct MedFunc;

/// Replace the ECX operand of each CPUID in \p Func whose leaf is a
/// constant that ignores it (x86CpuidLeafIgnoresSubleaf) by zero, which the
/// processor answers the same way, so the register is read nowhere: Go's
/// `rt0_go` runs `mov eax, 0; cpuid` with ECX as the kernel left it.  The
/// leaf may reach CPUID through copies, zero extensions and low views of
/// the constant.  Requires SSA; returns whether any operand changed.
bool dropIgnoredCpuidSubleaves(MedFunc &Func);
} // namespace neverd

#endif // NEVERD_IR_MED_X86_X86CPUIDSUBLEAF_H
