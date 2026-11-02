//===- MedCallConventionSysV.cpp - System V x86-64 call arguments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// System V x86-64, the convention of ELF and Mach-O images alike (Apple's
// x86-64 convention is the System V one): RDI, RSI, RDX, RCX, R8 and R9.  A
// variadic callee tests AL (the vector registers its caller used) and spills
// every argument register to its register save area, so its summary lists no
// parameters.
// Its stack arguments start at the return address with no home area, which
// the stack-argument summary does not count.  RAX, R10 and R11 hold nothing
// at entry, so a function that only aligns the stack for a call with a push
// of RAX passes no stack argument.  A libc import with a fixed prototype
// reads exactly its parameters, through its PLT stub or its GOT slot alike,
// and a variadic one its fixed parameters, which the variadic arguments a
// call passes follow in the argument registers.
// A function's register parameters are those its callers pass: the ones its
// summary lists, not a register whose old value only a byte write keeps
// (`setg cl` merges into RCX).  The routines of the Itanium C++ ABI's runtime
// and unwinder take the parameters it declares.
//
//===----------------------------------------------------------------------===//

#include "../MedCallConventionItanium.h"

#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd {

namespace {
/// Reading exactly AL at entry is the variadic prologue's `test al, al`: no
/// System V parameter is passed in RAX.  A wider read is no such test: an
/// alignment `push rax` before a call reads the whole register, and
/// `sete al; mov r14d, eax` copies bytes of RAX no caller set.
bool readsVectorCount(const GPRReadWidths &Reads) {
  return Reads[x86reg::RAX / 8] == 1;
}
} // namespace

extern const CallArgumentConvention SysVX64CallArguments;
const CallArgumentConvention SysVX64CallArguments = {
    .TheArch = Arch::X64,
    .Format = BinaryFormat::Unknown,
    .RegisterArgumentsFromCalleeSummary = true,
    .VectorArgumentsFromCalleeSummary = true,
    .ImportArgumentsFromPrototype = true,
    .SummaryListsNoParameters = readsVectorCount,
    .VariadicVectorCountRegister = x86reg::RAX,
    .VariadicImportArgumentsInRegisters = true,
    .StackArgumentSummary = true,
    .UndefinedIncomingScratchRegisters = true,
    .IndirectCallsTakePrecedingSetup = true,
    .ParametersFromIncomingReads = true,
    .PrototypeArgCount = itaniumRuntimeArgCount,
    .FormattedCallArguments = true,
};

} // namespace neverd
