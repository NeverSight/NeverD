//===- IntrinsicInstructionText.h - The instruction behind one --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The instruction an intrinsic was lifted from, as a listing spells it,
/// where the intrinsic's C spelling leaves out what the instruction carries:
/// a trap's or a breakpoint's code (`brk #0x3E8`, which `__builtin_trap()`
/// does not say), a barrier's option (`dmb ishld`), and the system register
/// or operation of an AArch64 MRS, MSR or SYS (`dc civac`).  The lifters
/// keep that operand as the intrinsic's first input; the C writers print
/// this text beside the call, or as the instruction an `asm` statement runs.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_INTRINSICINSTRUCTIONTEXT_H
#define NEVERD_IR_INTRINSICS_INTRINSICINSTRUCTIONTEXT_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace neverd {

/// Whether \p Id keeps its instruction's operand as its first input: a trap
/// or breakpoint code, a hypervisor or secure-monitor call number, or a
/// barrier option.
bool intrinsicCarriesInstructionOperand(Intrinsic Id);

/// The instruction \p Id with first input \p Operand was lifted from
/// (`brk #0x3E8`, `dsb sy`, `ud1 eax, dword ptr [eax + 0x15]`); empty for an
/// intrinsic that carries no such operand, or without one.
std::string intrinsicInstructionText(Intrinsic Id,
                                     std::optional<uint64_t> Operand);

/// The option name of a DMB, DSB or ISB option field \p CRm (`ish`), or its
/// immediate (`#0x4`).
std::string armBarrierOptionText(uint64_t CRm);

/// The option field a barrier option \p Text spells (`ish` is 0xB, `#0x4`
/// is 4); none for any other text.
std::optional<uint64_t> armBarrierOptionValue(llvm::StringRef Text);

/// The name, as the listing prints it, of the AArch64 system register an
/// MRS reads, or an MSR writes when \p Write, at \p Encoding: op0, op1, CRn,
/// CRm and op2, as bits [20:5] of the instruction hold them (`cntvct_el0` is
/// 0xDF02).  Its generic name, `s3_3_c15_c2_0`, which every assembler and
/// ACLE's __arm_rsr64 accept, for one without a name.
std::string aarch64SystemRegisterName(uint64_t Encoding, bool Write);

/// The assembly of the SVE LDR or STR of a whole Z or P register that
/// \p Word encodes, with `$0` for its base register (`str z0, [$0, #-34, mul
/// vl]`); empty for any other word.  The lift keeps the word as the operand
/// of A64_SveRegisterTransfer: C can name neither the registers nor the
/// length the offset scales by.
std::string aarch64SveRegisterTransferAsm(uint64_t Word);

/// Whether \p Word encodes the SVE LDR or STR of a whole Z or P register.
inline bool isAArch64SveRegisterTransferWord(uint32_t Word) {
  return (Word & 0xFFC0E000) == 0xE5804000 ||
         (Word & 0xFFC0E000) == 0x85804000 ||
         (Word & 0xFFC0E010) == 0xE5800000 || (Word & 0xFFC0E010) == 0x85800000;
}

/// The physical destination clobbered by an SVE register load; empty for
/// stores.
std::string aarch64SveRegisterTransferClobber(uint64_t Word);

/// The assembly of the AArch64 system instruction \p Id (Mrs, Msr, Dc, Ic,
/// Tlbi or At) the lifter kept the op0:op1:CRn:CRm:op2 \p Field of, with
/// `$0` for its output or else its register operand: `mrs $0, cntvct_el0`,
/// `msr tpidr_el0, $0`, `msr daifset, #0x2`, `dc civac, $0`, `ic iallu`,
/// `sysl $0, #3, c15, c0, #1`.  Empty where \p Field and the operands fit no
/// form of \p Id.
std::string aarch64SystemInstructionAsm(Intrinsic Id, uint64_t Field,
                                        bool HasOutput, bool HasRegister);

/// The field an ARM coprocessor register transfer (MRC, MCR, MRRC or MCRR)
/// keeps of its operands, one hex digit each: coprocessor, opc1, CRn, CRm and
/// opc2, so `mrc p15, #0, r0, c13, c0, #3` is 0xF0D03.  MRRC and MCRR have no
/// CRn or opc2, which stay 0: `mrrc p15, #1, r0, r1, c14` is 0xF10E0.
constexpr uint64_t armCoprocessorField(unsigned Coprocessor, unsigned Opc1,
                                       unsigned CRn, unsigned CRm,
                                       unsigned Opc2) {
  return uint64_t(Coprocessor) << 16 | uint64_t(Opc1) << 12 |
         uint64_t(CRn) << 8 | uint64_t(CRm) << 4 | Opc2;
}

/// The operands an armCoprocessorField holds.
struct ArmCoprocessorOperands {
  unsigned Coprocessor;
  unsigned Opc1;
  unsigned CRn;
  unsigned CRm;
  unsigned Opc2;
};

/// The operands of coprocessor transfer \p Id (ArmMrc, ArmMcr, ArmMrrc or
/// ArmMcrr) in \p Field; none for another intrinsic, or a field no such
/// instruction has: an MRC's or MCR's opc1 over 7, or an MRRC's CRn.
std::optional<ArmCoprocessorOperands> armCoprocessorOperands(Intrinsic Id,
                                                             uint64_t Field);

/// The assembly of coprocessor transfer \p Id with \p Field, with `$0` for
/// its register, or `${0:Q}, ${0:R}` for the low and high halves of the
/// 64-bit value an MRRC or MCRR moves: `mrc p15, #0, $0, c13, c0, #3`,
/// `mcrr p15, #1, ${0:Q}, ${0:R}, c14`.  Empty where armCoprocessorOperands
/// has none.
std::string armCoprocessorInstructionAsm(Intrinsic Id, uint64_t Field);

/// The intrinsic and field of coprocessor transfer assembly \p Text as
/// armCoprocessorInstructionAsm writes it; none for any other text.
std::optional<std::pair<Intrinsic, uint64_t>>
armCoprocessorAsmField(llvm::StringRef Text);

/// The status register an ARM MRS (`ArmMrs`) reads, its field.
constexpr uint64_t ArmMrsApsr = 0;
constexpr uint64_t ArmMrsSpsr = 1;

/// The field an ARM MSR (`ArmMsr`) keeps: the fields of its mask (c 1, x 2,
/// s 4, f 8), with ArmMsrSpsr for an SPSR.
constexpr uint64_t ArmMsrSpsr = 0x10;

/// The field an ARM CPS (`ArmCps`) keeps: its imod (2 enables the masks, 3
/// disables them, 0 changes only the mode), the A, I and F masks it changes
/// (4, 2, 1) and, when \p ChangesMode, the mode it enters (`cpsid i` is
/// 0x680, `cps #0x13` 0x33).
constexpr uint64_t armCpsField(unsigned Imod, unsigned Masks, bool ChangesMode,
                               unsigned Mode) {
  return uint64_t(Imod) << 9 | uint64_t(Masks) << 6 |
         uint64_t(ChangesMode) << 5 | Mode;
}

/// The assembly of the ARM status register instruction \p Id (ArmMrs,
/// ArmMsr or ArmCps) with \p Field, with `$0` for its register:
/// `mrs $0, apsr`, `mrs $0, spsr`, `msr cpsr_fc, $0`, `msr spsr_fsxc, $0`,
/// `cpsid if`, `cpsie i, #0x13`, `cps #0x13`.  Empty for a field no such
/// instruction has.
std::string armStatusInstructionAsm(Intrinsic Id, uint64_t Field);

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_INTRINSICINSTRUCTIONTEXT_H
