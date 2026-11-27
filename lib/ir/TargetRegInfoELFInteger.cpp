//===- TargetRegInfoELFInteger.cpp - ELF integer register conventions -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// RISC-V ILP32/LP64 and classic PowerPC System V integer carriers.
///
//===----------------------------------------------------------------------===//

#include "TargetRegInfoDetail.h"

#include "neverd/ArchSupport.h"
#include "neverd/lift/IntegerRegisters.h"
#include "neverd/loader/ELF/PowerPCABI.h"

#include <array>
#include <cstddef>

namespace neverd {
namespace {
using integerreg::gpr;
template <unsigned First, size_t Count> constexpr auto registers() {
  std::array<uint64_t, Count> Result{};
  for (unsigned I = 0; I < Count; ++I)
    Result[I] = gpr(First + I);
  return Result;
}
constexpr auto GPRs = registers<0, 32>();
constexpr auto RVArgs = registers<10, 8>();
using integerreg::riscv::fpr;
constexpr std::array RVFPArgs = {fpr(10), fpr(11), fpr(12), fpr(13),
                                 fpr(14), fpr(15), fpr(16), fpr(17)};
constexpr auto PPCArgs = registers<3, 8>();
// gp and tp are fixed throughout ordinary psABI calls.
constexpr uint64_t RVSaved[] = {gpr(3),  gpr(4),  gpr(8),  gpr(9),  gpr(18),
                                gpr(19), gpr(20), gpr(21), gpr(22), gpr(23),
                                gpr(24), gpr(25), gpr(26), gpr(27)};
constexpr auto PPCSaved = registers<14, 18>();
constexpr const char *RVNames[] = {
    "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
    "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
    "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
constexpr const char *PPCNames[] = {
    "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10",
    "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21",
    "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31"};
constexpr const char *RVFPNames[] = {
    "ft0", "ft1", "ft2",  "ft3",  "ft4", "ft5", "ft6",  "ft7",
    "fs0", "fs1", "fa0",  "fa1",  "fa2", "fa3", "fa4",  "fa5",
    "fa6", "fa7", "fs2",  "fs3",  "fs4", "fs5", "fs6",  "fs7",
    "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"};
const char *rvName(uint64_t Off, uint16_t) {
  if (Off >= fpr(0) && Off < fpr(32) && Off % 8 == 0)
    return RVFPNames[(Off - fpr(0)) / 8];
  if (Off == integerreg::PC)
    return "pc";
  return Off >= gpr(0) && Off < gpr(32) && Off % 8 == 0 ? RVNames[Off / 8 - 1]
                                                        : "unknown";
}
const char *ppcName(uint64_t Off, uint16_t) {
  using namespace integerreg;
  if (Off == PC)
    return "pc";
  if (Off == ppc::LR)
    return "lr";
  if (Off == ppc::CTR)
    return "ctr";
  if (Off == ppc::XER)
    return "xer";
  static constexpr const char *CRNames[] = {
      "cr0_lt", "cr0_gt", "cr0_eq", "cr0_so", "cr1_lt", "cr1_gt", "cr1_eq",
      "cr1_so", "cr2_lt", "cr2_gt", "cr2_eq", "cr2_so", "cr3_lt", "cr3_gt",
      "cr3_eq", "cr3_so", "cr4_lt", "cr4_gt", "cr4_eq", "cr4_so", "cr5_lt",
      "cr5_gt", "cr5_eq", "cr5_so", "cr6_lt", "cr6_gt", "cr6_eq", "cr6_so",
      "cr7_lt", "cr7_gt", "cr7_eq", "cr7_so"};
  if (Off >= ppc::crBit(0) && Off < ppc::crBit(32))
    return CRNames[Off - ppc::crBit(0)];
  return Off >= gpr(0) && Off < gpr(32) && Off % 8 == 0 ? PPCNames[Off / 8 - 1]
                                                        : "unknown";
}

TargetRegInfo integerInfo(Arch A) {
  TargetRegInfo Info;
  const bool RV = isRISCV(A);
  Info.TheArch = A;
  Info.FullRegWidth = Info.PointerSize = isRISCV64(A) || isPowerPC64(A) ? 8 : 4;
  Info.MinInsnAlign = RV ? 2 : 4;
  Info.IntParamRegs = RV ? llvm::ArrayRef(RVArgs) : llvm::ArrayRef(PPCArgs);
  Info.IntReturnRegs = Info.IntParamRegs.take_front(2);
  Info.IntReturnReg = Info.IntReturnRegs[0];
  Info.IntReturnReg2 = Info.IntReturnRegs[1];
  Info.IntegerReturnPairHighFirst = !archLittleEndian(A);
  Info.StackPointer = gpr(RV ? 2 : 1);
  Info.FramePointer = gpr(RV ? 8 : 31);
  Info.ProgramCounter = integerreg::PC;
  Info.LinkRegister = RV ? gpr(1) : integerreg::ppc::LR;
  Info.CalleeSaveRegs = RV ? llvm::ArrayRef(RVSaved) : llvm::ArrayRef(PPCSaved);
  Info.GeneralRegs =
      RV ? llvm::ArrayRef(GPRs).drop_front() : llvm::ArrayRef(GPRs);
  Info.GetRegName = RV ? rvName : ppcName;
  if (RV) {
    Info.VecRegBase = fpr(0);
    Info.VecRegCount = 32;
    Info.VecRegStride = Info.VecRegWidth = 8;
    if (hasRISCVDoubleABI(A)) {
      Info.FPABIRegWidth = 8;
      Info.FPParamRegs = RVFPArgs;
      Info.FPReturnRegs = Info.FPParamRegs.take_front(2);
      Info.FPReturnReg = fpr(10);
    }
  }
  if (!RV)
    Info.CallStackArgumentBase =
        isPowerPC64ELFv1(A) ? ppc_elf::ELFv1StackArgumentBase
        : isPowerPC64(A)    ? ppc_elf::ELFv2StackArgumentBase
                            : ppc_elf::PPC32StackArgumentBase;
  return Info;
}
} // namespace
const TargetRegInfo &getELFIntegerRegInfo(Arch Target) {
  switch (Target) {
#define NEVERD_ELF_INTEGER_TARGET(Name, LLVMArch, Spelling)                    \
  case Arch::Name: {                                                           \
    static const TargetRegInfo Info = integerInfo(Arch::Name);                 \
    return Info;                                                               \
  }
#include "neverd/ELFIntegerTargets.def"

  default:
    llvm_unreachable("unexpected integer ELF target");
  }
}
} // namespace neverd
