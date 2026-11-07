//===- ARMLiftDetail.h - ARM32 lifter internals -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared by the ARM32 instruction-lifter translation
/// units: the per-instruction-family handlers that the ARMLifter::lift*
/// members chain through, plus the few helpers used by more than one of them.
///
/// Each handler has exactly the shape of the switch it was carved out of: it
/// returns true when it recognized (and lifted) the instruction and false when
/// the instruction is not one of its cases, so the dispatchers can chain them
/// with `||` and keep the original dispatch order.
///
/// The handlers take the ARMLifter by reference because the operand and flag
/// helpers they call are members; nothing else in ARMLifter's private state is
/// reachable from a per-instruction handler.
///
/// This header is an implementation detail of lib/lift/ARM/ and must NOT be
/// included by code outside that directory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_ARM_ARMLIFTDETAIL_H
#define NEVERD_LIFT_ARM_ARMLIFTDETAIL_H

#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"
#include "neverd/lift/ARM/ARMLifter.h"
#include "neverd/support/ARM/ARMTrap.h"

#include <capstone/capstone.h>
#include <cstdint>
#include <optional>

namespace neverd {

/// The armCoprocessorField of a coprocessor register transfer's operands:
/// `mrc p15, #0, r0, c13, c0, #3`, or `mrrc p15, #1, r0, r1, c14` when
/// \p TwoRegisters.  None for any other shape.
inline std::optional<uint64_t> coprocessorTransferField(const cs_arm &ARM,
                                                        bool TwoRegisters) {
  const cs_arm_op *Op = ARM.operands;
  auto fits = [](const cs_arm_op &Operand, arm_op_type Type, int64_t Max) {
    return Operand.type == Type && Operand.imm >= 0 && Operand.imm <= Max;
  };
  if (ARM.op_count != (TwoRegisters ? 5 : 6) || !fits(Op[0], ARM_OP_PIMM, 15))
    return std::nullopt;
  if (TwoRegisters)
    return fits(Op[1], ARM_OP_IMM, 15) && fits(Op[4], ARM_OP_CIMM, 15)
               ? std::optional<uint64_t>(
                     armCoprocessorField(Op[0].imm, Op[1].imm, 0, Op[4].imm, 0))
               : std::nullopt;
  return fits(Op[1], ARM_OP_IMM, 7) && fits(Op[3], ARM_OP_CIMM, 15) &&
                 fits(Op[4], ARM_OP_CIMM, 15) && fits(Op[5], ARM_OP_IMM, 7)
             ? std::optional<uint64_t>(armCoprocessorField(
                   Op[0].imm, Op[1].imm, Op[3].imm, Op[4].imm, Op[5].imm))
             : std::nullopt;
}

/// Whether \p Op is a register a coprocessor or status register transfer
/// moves: r0-r12, SP or LR.  An MRC to PC is the form that writes the
/// condition flags (`mrc p14, #0, apsr_nzcv, c0, c1, #0`); elsewhere PC is
/// UNPREDICTABLE.
inline bool isTransferRegister(const cs_arm_op &Op) {
  return Op.type == ARM_OP_REG &&
         ((Op.reg >= ARM_REG_R0 && Op.reg <= ARM_REG_R12) ||
          Op.reg == ARM_REG_SP || Op.reg == ARM_REG_LR);
}

/// The field of an MSR's destination \p Op: its mask (c 1, x 2, s 4, f 8),
/// with ArmMsrSpsr for an SPSR.  CPSR, and APSR as A-profile code spells it
/// (`apsr_nzcvq`, `apsr_g`), are one register.  None for a banked register,
/// an M-profile one, or no field.
inline std::optional<uint64_t> msrField(const cs_arm_op &Op) {
  const unsigned Mask = Op.sysop.msr_mask;
  if (Mask == 0 || Mask > 0xF)
    return std::nullopt;
  switch (Op.type) {
  case ARM_OP_SPSR:
    return ArmMsrSpsr | Mask;
  case ARM_OP_CPSR:
    return Mask;
  case ARM_OP_SYSREG:
    switch (Op.sysop.reg.mclasssysreg) {
    case ARM_MCLASSSYSREG_APSR_NZCVQ:
    case ARM_MCLASSSYSREG_APSR_G:
    case ARM_MCLASSSYSREG_APSR_NZCVQG:
      return Mask;
    default:
      return std::nullopt;
    }
  default:
    return std::nullopt;
  }
}

/// The armCpsField of a CPS: `cpsid i` disables IRQs, `cps #0x13` enters
/// Supervisor mode.  None for an UNPREDICTABLE form armStatusInstructionAsm
/// has no text for (`cpsid none, #0x13`).
inline std::optional<uint64_t> cpsField(const cs_arm &ARM) {
  const unsigned Imod = ARM.cps_mode == ARM_CPSMODE_IE   ? 2
                        : ARM.cps_mode == ARM_CPSMODE_ID ? 3
                                                         : 0;
  const unsigned Masks =
      ARM.cps_flag == ARM_CPSFLAG_NONE ? 0 : unsigned(ARM.cps_flag) & 7;
  const bool ChangesMode = ARM.op_count == 1;
  if (ARM.op_count > 1 ||
      (ChangesMode && (ARM.operands[0].type != ARM_OP_IMM ||
                       ARM.operands[0].imm < 0 || ARM.operands[0].imm > 0x1F)))
    return std::nullopt;
  const uint64_t Field =
      armCpsField(Imod, Masks, ChangesMode,
                  ChangesMode ? static_cast<unsigned>(ARM.operands[0].imm) : 0);
  if (armStatusInstructionAsm(Intrinsic::ArmCps, Field).empty())
    return std::nullopt;
  return Field;
}

//===----------------------------------------------------------------------===//
// Helpers shared by more than one handler translation unit
//===----------------------------------------------------------------------===//

/// Lane (element) width in bytes plus the signedness/float-ness of a NEON
/// vector data type.  A zero \c LaneSz means the width could not be recovered.
struct NeonLaneInfo {
  unsigned LaneSz = 0;
  bool IsSigned = false;
  bool IsFloat = false;
};

/// Element info from capstone's \c vector_data.  Defined in
/// ARMLiftSIMDNEON.cpp.
NeonLaneInfo getNeonLaneInfo(arm_vectordata_type VD);

/// Element info parsed from the mnemonic suffix (".8"/".i16"/".s32"/".f32").
/// Defined in ARMLiftSIMDNEON.cpp.
NeonLaneInfo getNeonLaneInfoFromMnemonic(const char *Mnem);

/// Element info from \c vector_data, falling back to the mnemonic suffix when
/// capstone did not populate it.  Defined in ARMLiftSIMDNEON.cpp.
NeonLaneInfo getNeonLaneInfo(arm_vectordata_type VD, const char *Mnem);

/// Whether \p ARM carries a real condition (EQ..LE): AL and the "execute
/// always" sentinel Capstone reports for some unconditional forms do not.
/// Defined in ARMLifter.cpp.
bool isPredicated(const cs_arm &ARM);

/// The immediate a post-indexed single-register LDR/STR adds to its base:
/// Capstone carries it in the operand after \p MemOp, or else in its
/// displacement. Defined in ARMLiftMemAddr.cpp.
int64_t postIndexImmediate(const cs_arm &ARM, const cs_arm_op &MemOp);

/// Whether \p Insn's mnemonic spells \p Alias: the alias itself, with the
/// condition of a predicated instruction after it (`popne`, also inside an IT
/// block), and with the ".w" of a wide Thumb-2 encoding (`popne.w`).  Capstone
/// reports the push/pop aliases this way.  Defined in ARMLiftMem.cpp.
bool isAliasMnemonic(const cs_insn *Insn, const char *Alias);

//===----------------------------------------------------------------------===//
// ARMLifter::liftCore handlers
//===----------------------------------------------------------------------===//

/// HINT and the register/immediate move family: MOV, MOVS, MOVW, MOVT, MVN.
bool liftCoreMove(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                  const cs_arm &ARM);

/// Integer add/subtract with and without carry, plus the flag-only compares.
bool liftCoreArith(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM);

/// Bitwise data processing: AND, ORR, ORN, EOR and BIC.
bool liftCoreLogic(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM);

/// Shift and rotate data processing: LSL, LSR, ASR and ROR.
bool liftCoreShift(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM);

/// Sign/zero extension, bitfield extraction, CLZ, IT and REV.
bool liftCoreBit(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                 const cs_arm &ARM);

//===----------------------------------------------------------------------===//
// ARMLifter::liftCoreExt handlers
//===----------------------------------------------------------------------===//

/// ADR, the bitfield insert/clear pair, the byte/bit reversals, RRX and RSC.
bool liftCoreExtBit(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                    const cs_arm &ARM);

/// The extend-with-accumulate family (SXTAB/UXTAH/SXTB16/...).
bool liftCoreExtExtend(ARMLifter &L, ARMLifter::LiftState &S,
                       const cs_insn *Insn, const cs_arm &ARM);

/// Saturating scalar arithmetic, the GE-setting lane-parallel add/subtract
/// family and SSAT/USAT.
bool liftCoreExtSat(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                    const cs_arm &ARM);

/// Halfword packing, the GE-flag byte select and the SAD instructions.
bool liftCoreExtPack(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM);

/// CRC32, the conditional selects, the MVE long shifts and TBB/TBH.
bool liftCoreExtMisc(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM);

//===----------------------------------------------------------------------===//
// ARMLifter::liftMem handlers
//===----------------------------------------------------------------------===//

/// Single-register LDR/STR (all widths) and the LDRD/STRD pair.
bool liftMemSingle(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM);

/// PUSH/POP and the load/store-multiple family.
bool liftMemMultiple(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM);

/// Exclusive, acquire/release, SWP and unprivileged accesses.
bool liftMemAtomic(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM);

//===----------------------------------------------------------------------===//
// ARMLifter::liftMul handlers
//===----------------------------------------------------------------------===//

/// The 32-bit multiply/divide core and the 64-bit long forms.
bool liftMulBasic(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                  const cs_arm &ARM);

/// The 32-bit accumulating halfword multiplies.
bool liftMulHalfAccum(ARMLifter &L, ARMLifter::LiftState &S,
                      const cs_insn *Insn, const cs_arm &ARM);

/// The 64-bit accumulating halfword multiplies.
bool liftMulHalfLong(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM);

/// The non-accumulating halfword multiplies.
bool liftMulHalfProduct(ARMLifter &L, ARMLifter::LiftState &S,
                        const cs_insn *Insn, const cs_arm &ARM);

/// The most-significant-word multiplies SMMUL/SMMLA/SMMLS.
bool liftMulHigh(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                 const cs_arm &ARM);

//===----------------------------------------------------------------------===//
// ARMLifter::liftSIMD handlers
//===----------------------------------------------------------------------===//

/// Per-lane VFP/NEON arithmetic: add, subtract, multiply, divide, negate,
/// absolute value and square root.
bool liftSIMDArith(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM);

/// The VMOV/VMOVX family: lane insert/extract, GPR pair transfer and the
/// broadcast immediate forms.
bool liftSIMDMove(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                  const cs_arm &ARM);

/// VCMP/VCMPE and the floating-point conversion family.
bool liftSIMDConvert(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM);

/// The fused and unfused multiply-accumulate family.
bool liftSIMDFMA(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                 const cs_arm &ARM);

/// The VSEL family, per-lane min/max, the VRINT roundings and VMRS/VMSR.
bool liftSIMDSelect(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                    const cs_arm &ARM);

/// VFP load/store, the register-list transfers and the lazy-state ops.
bool liftSIMDMem(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                 const cs_arm &ARM);

//===----------------------------------------------------------------------===//
// ARMLifter::liftSIMDNEON handlers
//===----------------------------------------------------------------------===//

/// The NEON structure load/store family VLD1-VLD4 and VST1-VST4.
bool liftSIMDNEONLoadStore(ARMLifter &L, ARMLifter::LiftState &S,
                           const cs_insn *Insn, const cs_arm &ARM);

/// NEON bitwise logic and the bit-select family.
bool liftSIMDNEONLogic(ARMLifter &L, ARMLifter::LiftState &S,
                       const cs_insn *Insn, const cs_arm &ARM);

/// Per-lane NEON add and subtract, including the widening, narrowing,
/// pairwise, halving and rotated-complex forms.
bool liftSIMDNEONArith(ARMLifter &L, ARMLifter::LiftState &S,
                       const cs_insn *Insn, const cs_arm &ARM);

/// Per-lane NEON multiply and multiply-accumulate.
bool liftSIMDNEONMul(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM);

/// Per-lane absolute difference and the mask-producing compares.
bool liftSIMDNEONCompare(ARMLifter &L, ARMLifter::LiftState &S,
                         const cs_insn *Insn, const cs_arm &ARM);

/// Per-lane NEON unary operations, lane widening/narrowing and the
/// reciprocal estimate/step family.
bool liftSIMDNEONConvert(ARMLifter &L, ARMLifter::LiftState &S,
                         const cs_insn *Insn, const cs_arm &ARM);

/// NEON lane movement: swap, interleave, transpose, duplicate, table lookup
/// and extract.
bool liftSIMDNEONPermute(ARMLifter &L, ARMLifter::LiftState &S,
                         const cs_insn *Insn, const cs_arm &ARM);

/// Per-lane NEON shifts, including the widening, insert and narrowing forms.
bool liftSIMDNEONShift(ARMLifter &L, ARMLifter::LiftState &S,
                       const cs_insn *Insn, const cs_arm &ARM);

/// Saturating NEON arithmetic and the doubling multiplies.
bool liftSIMDNEONSat(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                     const cs_arm &ARM);

/// Pairwise min/max, the across-vector reductions, the MVE predicate ops, the
/// dot products and the custom-datapath instructions.
bool liftSIMDNEONMisc(ARMLifter &L, ARMLifter::LiftState &S,
                      const cs_insn *Insn, const cs_arm &ARM);

} // namespace neverd

#endif // NEVERD_LIFT_ARM_ARMLIFTDETAIL_H
