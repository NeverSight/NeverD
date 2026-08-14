//===- ARMLifter.h - ARM32 instruction lifter ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the ARMLifter class that translates decoded ARM32 instructions
/// into LowIR operations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_ARMLIFTER_H
#define NEVERD_LIFT_ARMLIFTER_H

#include "neverd/lift/ARMRegs.h"

#include <capstone/capstone.h>
#include <vector>

namespace neverd {

class ARMLifter {
public:
  explicit ARMLifter(Arch TargetArch);

  void lift(const cs_insn *Insn, std::vector<LowOp> &Ops);

  void setStrict(bool S) { Strict = S; }
  bool isStrict() const { return Strict; }

  //===--------------------------------------------------------------------===//
  // Decode-time instruction classification (called by Decoder/FuncDetector)
  //===--------------------------------------------------------------------===//

  /// Correct capstone instruction-id quirks before lifting.  Currently a no-op
  /// for ARM (no known capstone decode-id collisions requiring fixup).
  static void fixupDecodedInsn(cs_insn *I);

  /// Whether \p I ends a function's straight-line decode.  Operand-dependent
  /// PC writes require capstone detail to be enabled.
  static bool isFunctionTerminator(const cs_insn *I);

  /// Direct (immediate) call target of \p I, or InvalidVA if \p I is not a
  /// direct call.
  static va_t directCallTarget(const cs_insn *I);

  enum class ControlKind : uint8_t {
    None,
    DirectBranch,
    DirectCall,
    IndirectBranch,
    IndirectCall,
    Return,
    ExceptionReturn,
  };

  /// Operand-aware control description shared by lifting, recursive decode,
  /// and instruction-boundary provenance.  ExceptionReturn is deliberately a
  /// distinct kind because its destination mode comes from restored processor
  /// state rather than the branch target.
  struct ControlInfo {
    ControlKind Kind = ControlKind::None;
    LowInstructionTargetMode TargetMode = LowInstructionTargetMode::Preserve;
    bool IsConditional = false;

    bool isControl() const { return Kind != ControlKind::None; }
  };

  static ControlInfo classifyControl(const cs_insn *I,
                                     InstructionMode SourceMode);

  /// Describe ARM/Thumb interworking without interpreting target values in a
  /// later IR consumer.  This is a projection of classifyControl(), not an
  /// independent instruction-id table.
  static LowInstructionTargetMode controlTargetMode(const cs_insn *I,
                                                    InstructionMode SourceMode);

  struct LiftState : LiftStateBase {
    using LiftStateBase::LiftStateBase;

    void emitIntrinsic(Intrinsic Id, NdVar Out = NdVar::reg(armreg::R0, 4),
                       std::initializer_list<NdVar> Extra = {}) {
      LiftStateBase::emitIntrinsic(Id, Out, Extra);
    }
  };

  NdVar operandRead(LiftState &S, const cs_arm_op &Op);
  NdVar operandEffAddr(LiftState &S, const cs_arm_op &Op);
  static NdVar operandWrite(const cs_arm_op &Op);

  /// Apply an *immediate* barrel-shift/rotate (LSL/LSR/ASR/ROR/RRX) to a 32-bit
  /// value and return the result.  Shared by register-offset memory addressing
  /// (`[Rn, Rm, ror #k]`) and shifted data-processing operands: the addressing
  /// paths previously emitted a plain left shift for ROR, and RRX (rotate right
  /// through carry by one) was silently dropped everywhere.  Returns \p Val
  /// unchanged for a zero-amount non-RRX shift or an unrecognised type.
  static NdVar emitImmShift(LiftState &S, NdVar Val, unsigned ShType,
                            unsigned ShVal);

  /// Compute the LDRD/STRD access effective address from a memory operand,
  /// mirroring the single-register LDR/STR path: base + (optionally
  /// scaled/shifted/subtracted) index register + signed displacement.  For the
  /// post-indexed form the displacement is the post-increment (applied to the
  /// base afterward) and is NOT part of the access address, so pass
  /// \p PostIndex=true to exclude it.
  static NdVar emitLdrdStrdEA(LiftState &S, const cs_arm_op &MemOp,
                              bool PostIndex);

  /// Apply LDRD/STRD base-register writeback (pre-/post-indexed).  \p EA is the
  /// access address (used as the new base for the pre-indexed form).
  static void emitLdrdStrdWriteback(LiftState &S, const cs_insn *Insn,
                                    const cs_arm &ARM, const cs_arm_op &MemOp,
                                    NdVar EA);

  /// Compute the access address for a single-register LDR/STR memory operand
  /// and perform base writeback.  Handles offset / pre-index / post-index
  /// uniformly with an immediate OR a (shifted, signed) register offset: the
  /// post-indexed form accesses the UNMODIFIED base and folds the whole offset
  /// into the writeback.  Returns the access address (a fresh temp).
  static NdVar emitSingleMemAddr(LiftState &S, const cs_insn *Insn,
                                 const cs_arm &ARM, const cs_arm_op &MemOp);

  static NdVar buildCondCode(ARMCC_CondCodes CC, LiftState &S);
  static void emitNZCV(LiftState &S, NdVar Result, NdVar A, NdVar B,
                       bool IsSub);
  static void emitNZ(LiftState &S, NdVar Result);

  /// Pack NZCV into a GPR (MRS APSR) / restore NZCV from a GPR (MSR APSR_nzcv).
  static void emitMrsNzcv(LiftState &S, NdVar Dst);
  static void emitMsrNzcv(LiftState &S, NdVar Src);

  // Snapshot a source operand into a temp when it aliases the destination
  // register, so flag computation reads the pre-write value (e.g. `adds
  // rd, rd, rm`).  Returns the operand unchanged when there is no aliasing.
  static NdVar snapForFlags(LiftState &S, const NdVar &Dst, const NdVar &Op);

  // ARM shifter carry-out for a register (Rs[7:0]) or immediate shift amount.
  // Sets CFLAG for flag-setting LSLS/LSRS/ASRS/RORS.  ShType: 0=LSL 1=LSR
  // 2=ASR 3=ROR.  A zero amount leaves C unchanged (constant amounts fold).
  static void emitRegShifterCarry(LiftState &S, unsigned ShType, NdVar Src,
                                  NdVar Amt);

  // Set C for a flag-setting logical/move op (ANDS/ORRS/EORS/BICS/ORN/TST/TEQ/
  // MOVS/MVNS) from operand \p Op: the barrel-shifter carry-out for a shifted
  // register, or bit 31 of a rotated modified immediate.  A bare register or a
  // non-rotated immediate leaves C unchanged.  Call before the result writes
  // the destination (a shifted source may alias it).
  static void emitLogicalOpCarry(LiftState &S, const cs_insn *Insn,
                                 const cs_arm_op &Op);

private:
  bool liftCore(LiftState &S, const cs_insn *Insn, const cs_arm &ARM);
  bool liftCoreExt(LiftState &S, const cs_insn *Insn, const cs_arm &ARM);
  bool liftControl(LiftState &S, const cs_insn *Insn, const cs_arm &ARM);
  bool liftMem(LiftState &S, const cs_insn *Insn, const cs_arm &ARM);
  bool liftMul(LiftState &S, const cs_insn *Insn, const cs_arm &ARM);
  bool liftSIMD(LiftState &S, const cs_insn *Insn, const cs_arm &ARM);
  bool liftSIMDNEON(LiftState &S, const cs_insn *Insn, const cs_arm &ARM);

  Arch TargetArch;
  bool Strict = true;
};

} // namespace neverd

#endif // NEVERD_LIFT_ARMLIFTER_H
