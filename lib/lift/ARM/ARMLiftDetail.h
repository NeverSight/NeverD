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

#include "neverd/lift/ARMLifter.h"

#include <capstone/capstone.h>

namespace neverd {

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

/// Element info from capstone's \c vector_data.  Defined in ARMLiftSIMDNEON.cpp.
NeonLaneInfo getNeonLaneInfo(arm_vectordata_type VD);

/// Element info parsed from the mnemonic suffix (".8"/".i16"/".s32"/".f32").
/// Defined in ARMLiftSIMDNEON.cpp.
NeonLaneInfo getNeonLaneInfoFromMnemonic(const char *Mnem);

/// Element info from \c vector_data, falling back to the mnemonic suffix when
/// capstone did not populate it.  Defined in ARMLiftSIMDNEON.cpp.
NeonLaneInfo getNeonLaneInfo(arm_vectordata_type VD, const char *Mnem);

/// Whether \p Insn's mnemonic is \p Alias or its ".w" (wide Thumb-2) spelling.
/// Capstone reports the push/pop aliases this way.  Defined in ARMLiftMem.cpp.
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
bool liftSIMDNEONMul(ARMLifter &L, ARMLifter::LiftState &S,
                     const cs_insn *Insn, const cs_arm &ARM);

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
bool liftSIMDNEONSat(ARMLifter &L, ARMLifter::LiftState &S,
                     const cs_insn *Insn, const cs_arm &ARM);

/// Pairwise min/max, the across-vector reductions, the MVE predicate ops, the
/// dot products and the custom-datapath instructions.
bool liftSIMDNEONMisc(ARMLifter &L, ARMLifter::LiftState &S,
                      const cs_insn *Insn, const cs_arm &ARM);

} // namespace neverd

#endif // NEVERD_LIFT_ARM_ARMLIFTDETAIL_H
