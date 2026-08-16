//===- AArch64Lifter.h - AArch64 instruction lifter ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the AArch64Lifter class that translates decoded AArch64
/// instructions into LowIR operations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_AARCH64LIFTER_H
#define NEVERD_LIFT_AARCH64LIFTER_H

#include "neverd/lift/AArch64Regs.h"

#include <capstone/capstone.h>
#include <vector>

namespace neverd {

class AArch64Lifter {
public:
  explicit AArch64Lifter(Arch TargetArch);

  void lift(const cs_insn *Insn, std::vector<LowOp> &Ops);

  void setStrict(bool S) { Strict = S; }
  bool isStrict() const { return Strict; }

  //===--------------------------------------------------------------------===//
  // Decode-time instruction classification (called by Decoder/FuncDetector)
  //===--------------------------------------------------------------------===//

  /// Correct capstone instruction-id quirks before lifting.  Currently a no-op
  /// for AArch64 (no known capstone decode-id collisions requiring fixup).
  static void fixupDecodedInsn(cs_insn *I);

  /// Whether \p I ends a function's straight-line decode (ret/b/br/eret).
  static bool isFunctionTerminator(const cs_insn *I);

  /// Direct (immediate) call target of \p I, or InvalidVA if \p I is not a
  /// direct call.
  static va_t directCallTarget(const cs_insn *I);

  /// Direct-call (BL) target decoded straight from a 32-bit little-endian
  /// instruction \p Word located at \p Addr, or InvalidVA if \p Word is not a
  /// BL.  AArch64 is fixed-width and 4-byte aligned, so the call-target scan
  /// can classify a BL with a single mask+shift instead of a full capstone
  /// decode.  Matches directCallTarget for the BL case (the only AArch64 direct
  /// call; BLR is indirect).
  static va_t decodeBranchLinkTarget(uint32_t Word, va_t Addr);

  struct LiftState : LiftStateBase {
    using LiftStateBase::LiftStateBase;

    void
    emitIntrinsic(Intrinsic Id, NdVar Out = NdVar::reg(a64reg::X0, 8),
                  std::initializer_list<NdVar> Extra = {},
                  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None) {
      LiftStateBase::emitIntrinsic(Id, Out, Extra, MemoryOrdering);
    }
  };

  NdVar operandRead(LiftState &S, const cs_aarch64_op &Op);
  /// Compute the effective ADDRESS of a memory operand without loading through
  /// it (operandRead emits a LOAD for MEM operands).  For non-memory operands
  /// the operand value itself is returned (register-indirect addressing).
  NdVar operandEffAddr(LiftState &S, const cs_aarch64_op &Op);
  static NdVar operandWrite(const cs_aarch64_op &Op);

  /// Compute the (sign/zero-extended and LSL-scaled) 64-bit contribution of a
  /// memory operand's index register.  AArch64 register-offset addressing may
  /// extend the index via UXTW/SXTW (acting on the 32-bit W view) and/or scale
  /// it by an LSL amount; capstone surfaces the scaled-extend form
  /// (`[Xn, Wm, sxtw #s]`) with both shift.type==LSL and ext==SXTW/UXTW set, so
  /// the extend must be applied to the W view BEFORE the shift.  The caller is
  /// responsible for checking that \p Op has an index register.
  NdVar emitMemIndex(LiftState &S, const cs_aarch64_op &Op);

  /// An extended-register operand (`add w0,w1,w2,sxtb`) is always materialised
  /// by operandRead as a 64-bit value; for a W-form (32-bit) ADD/SUB/CMP/CMN
  /// narrow it to the operation width so the result and the NZCV flags are
  /// computed at 32 bits instead of 64.  No-op when already <= Sz.
  NdVar narrowToWidth(LiftState &S, NdVar V, uint16_t Sz);

  /// Pack NZCV into a GPR (MRS Xn, NZCV) / restore NZCV from a GPR
  /// (MSR NZCV, Xn).  clang spills/reloads the flags this way under register
  /// pressure (e.g. an `adcs` carry kept live across an intervening compare).
  static void emitMrsNzcv(LiftState &S, NdVar Dst);
  static void emitMsrNzcv(LiftState &S, NdVar Src);

private:
  bool liftCore(LiftState &S, const cs_insn *Insn, const cs_aarch64 &ARM64);
  bool liftCoreNEON(LiftState &S, const cs_insn *Insn, const cs_aarch64 &ARM64);
  bool liftControl(LiftState &S, const cs_insn *Insn, const cs_aarch64 &ARM64);
  bool liftMem(LiftState &S, const cs_insn *Insn, const cs_aarch64 &ARM64);
  bool liftAtomic(LiftState &S, const cs_insn *Insn, const cs_aarch64 &ARM64);
  bool liftFP(LiftState &S, const cs_insn *Insn, const cs_aarch64 &ARM64);
  bool liftSIMD(LiftState &S, const cs_insn *Insn, const cs_aarch64 &ARM64);
  bool liftSIMDExt(LiftState &S, const cs_insn *Insn, const cs_aarch64 &ARM64);

  Arch TargetArch;
  bool Strict = true;
};

} // namespace neverd

#endif // NEVERD_LIFT_AARCH64LIFTER_H
