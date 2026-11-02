//===- MedSyscallConventionLinux.cpp - Linux supervisor calls -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Linux enters the kernel with `svc #0`.  AArch64 passes the syscall number
// in x8 and up to six arguments in x0-x5; ARM EABI passes the number in r7
// and up to seven arguments in r0-r6.  Both return the result in the first
// argument register and preserve every other register.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedSyscallConvention.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/ARM/ARMRegs.h"

namespace neverd {

extern const SyscallConvention LinuxAArch64Syscall;
const SyscallConvention LinuxAArch64Syscall = {
    .TheArch = Arch::AArch64,
    .Lifted = Intrinsic::Svc,
    .Immediate = 0,
    .Modeled = Intrinsic::A64LinuxSyscall,
    .NumberRegister = a64reg::X8,
    .ArgumentRegisters = {a64reg::X0, a64reg::X1, a64reg::X2, a64reg::X3,
                          a64reg::X4, a64reg::X5},
    .ArgumentCount = 6,
    .ResultRegister = a64reg::X0,
    .RegisterSize = 8,
};

extern const SyscallConvention LinuxARMSyscall;
const SyscallConvention LinuxARMSyscall = {
    .TheArch = Arch::ARM,
    .Lifted = Intrinsic::ArmSvc,
    .Immediate = 0,
    .Modeled = Intrinsic::ArmLinuxSyscall,
    .NumberRegister = armreg::R7,
    .ArgumentRegisters = {armreg::R0, armreg::R1, armreg::R2, armreg::R3,
                          armreg::R4, armreg::R5, armreg::R6},
    .ArgumentCount = 7,
    .ResultRegister = armreg::R0,
    .RegisterSize = 4,
};

} // namespace neverd
