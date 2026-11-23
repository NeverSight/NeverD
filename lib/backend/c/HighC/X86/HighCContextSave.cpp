//===- HighCContextSave.cpp - x86-64 register file saves -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The registers an x86-64 context save stores, and how the source reads one.
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd {

HighCWriter::X64MachineRegister
HighCWriter::x64MachineRegister(const MedVar &V) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  if (TRI.isVectorReg(V.RegOff))
    return V.Size == TRI.FPABIRegWidth ? X64MachineRegister::Vector
                                       : X64MachineRegister::None;
  return V.Size == TRI.PointerSize && TRI.isGeneralReg(V.RegOff)
             ? X64MachineRegister::General
             : X64MachineRegister::None;
}

bool HighCWriter::x64SavesUngivenRegister(
    llvm::function_ref<X64EntryUse(uint64_t)> Use) {
  // System V and Win64 pass no argument in RAX, R10 or R11 and keep none of
  // them over a call, and RAX holds nothing at the entry of a function that
  // is not variadic.  One that stores the RAX it was entered with, and reads
  // none of the three, saves registers, as Go's asyncPreempt and debugCall
  // do for the code they interrupt: the argument registers stored beside it
  // are saved, not taken.  The stores of R10 and R11 may be gone by now, as
  // dead ones are.  Without RAX, four stored arguments are a constructor.
  return Use(x86reg::RAX) == X64EntryUse::Stored &&
         Use(x86reg::R10) != X64EntryUse::Read &&
         Use(x86reg::R11) != X64EntryUse::Read;
}

std::string HighCWriter::x64MachineRegisterText(const MedVar &V) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  const X64MachineRegister Machine = x64MachineRegister(V);
  if (V.Kind != MedVar::Reg || Machine == X64MachineRegister::None ||
      !TRI.GetRegName)
    return {};
  // GetRegName reuses one buffer. Copy it before any other call.
  const std::string Name =
      llvm::StringRef(TRI.GetRegName(V.RegOff, V.Size)).lower();
  if (Name.empty())
    return {};
  // GNU assembly alternatives support both -masm=att and -masm=intel.
  if (Machine == X64MachineRegister::Vector)
    return "({ unsigned __int128 __entry; __asm__ volatile(\"movdqu {%%" +
           Name + ", %0|%0, " + Name + "}\" : \"=m\"(__entry)); __entry; })";
  return "({ uint64_t __entry; __asm__ volatile(\"mov{q %%" + Name +
         ", %0| %0, " + Name + "}\" : \"=r\"(__entry)); __entry; })";
}

} // namespace neverd
