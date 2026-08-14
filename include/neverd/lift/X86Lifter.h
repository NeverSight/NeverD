//===- X86Lifter.h - x86/x64 instruction lifter -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the X86Lifter class that translates decoded x86/x64 instructions
/// into LowIR operations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86LIFTER_H
#define NEVERD_LIFT_X86LIFTER_H

#include "neverd/lift/X86Regs.h"

#include <capstone/capstone.h>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

class X86Lifter {
public:
  explicit X86Lifter(Arch TargetArch);

  void lift(const cs_insn *Insn, std::vector<LowOp> &Ops);

  void setStrict(bool S) { Strict = S; }
  bool isStrict() const { return Strict; }

  /// x87 stack-top (TOP) accessors.  The ST(i) macro names physical slot
  /// (TOP+i)&7, so TOP advances in lift order; the CFG builder reads it around
  /// each lift to later re-base ST references into control-flow order.
  int getFpuTop() const { return FPUTop; }
  void resetFpuState() {
    FPUTop = 0;
    FpuReset = false;
    FuncRetPopBytes = 0;
  }
  bool fpuDidReset() const { return FpuReset; }

  /// Largest `ret imm` pop seen while lifting the current function (the i386
  /// SysV callee-cleanup byte count, e.g. 4 for the hidden sret pointer); 0 for
  /// an ordinary `ret`.  Reset per function via resetFpuState().
  int getRetPopBytes() const { return FuncRetPopBytes; }

  //===--------------------------------------------------------------------===//
  // Decode-time instruction classification (called by Decoder/FuncDetector)
  //===--------------------------------------------------------------------===//

  /// Correct capstone instruction-id quirks before lifting.  The SSE/SSE2 and
  /// VEX/EVEX FP-compare pseudo-ops decode to ids that collide with unrelated
  /// instructions; re-derive the real id from the opcode / mnemonic.
  static void fixupDecodedInsn(cs_insn *I);

  /// Whether \p I ends a function's straight-line decode (ret/jmp/ud2/...).
  static bool isFunctionTerminator(const cs_insn *I);

  /// Whether \p I is a trap that execution can continue past.
  ///
  /// `int3` is the only one: it raises a breakpoint a debugger routinely
  /// resumes from, and a compiler emits `__debugbreak()` mid-function with the
  /// rest of the body behind it.  `ud2` is the opposite -- it is what a
  /// compiler emits to mark code as unreachable.
  static bool isResumableTrap(const cs_insn *I);

  /// Direct (immediate) call target of \p I, or InvalidVA if \p I is not a
  /// direct call.
  static va_t directCallTarget(const cs_insn *I);

  /// Encoded RET/RETF stack-pop immediate.  The optional preserves the
  /// distinction between no immediate and an explicitly encoded zero.
  static std::optional<uint64_t> returnImmediate(const cs_insn *I);

  /// Target VA of a pure RIP/EIP-relative `lea` (`lea reg, [rip+disp]`), the
  /// x86 address-of idiom that materializes a fixed code address into a
  /// register without a relocation (a same-section function pointer the
  /// assembler resolved).  InvalidVA when \p I is not such a `lea`.  The caller
  /// records a target landing in an executable segment as a code reference so
  /// the emitter symbolizes the resulting constant to `ptrtoint @func`.
  static va_t pcRelCodeRefTarget(const cs_insn *I);

  struct LiftState : LiftStateBase {
    using LiftStateBase::LiftStateBase;

    void emitIntrinsic(Intrinsic Id, NdVar Out = NdVar::reg(x86reg::RAX, 8),
                       std::initializer_list<NdVar> Extra = {}) {
      LiftStateBase::emitIntrinsic(Id, Out, Extra);
    }

    NdVar computeEA(const cs_x86_op &MemOp);
    void storeToMem(const cs_x86_op &MemOp, NdVar Val);
    NdVar emitByteSwap(NdVar Src);
  };

  NdVar operandRead(LiftState &S, const cs_x86_op &Op);
  static NdVar operandWrite(const cs_x86_op &Op);

  void emitFlagsArith(LiftState &S, NdVar Result, NdVar A, NdVar B,
                      bool IsSub);
  static void emitPF(LiftState &S, NdVar Result);
  static void emitZSPF(LiftState &S, NdVar Result);
  // AF (auxiliary carry) = carry/borrow out of bit 3 = bit 4 of (A ^ B ^
  // Result).
  static void emitAF(LiftState &S, NdVar Result, NdVar A, NdVar B);
  static void emitFlagsLogic(LiftState &S, NdVar Result);
  // Extract bit \p BitPos of \p Val as a 1-byte 0/1 boolean.
  static NdVar extractBit(LiftState &S, NdVar Val, unsigned BitPos);
  // Direction-aware pointer step for single string ops: +ElemSz when DF=0
  // (forward), -ElemSz when DF=1 (backward).  Folds to a constant when the
  // direction flag is statically known.
  static NdVar dirStep(LiftState &S, unsigned ElemSz);
  // x86 shifts/rotates define OF only for a 1-bit count: write \p OfBit to OF
  // when \p Cnt == 1, otherwise leave OF unchanged (count 0 preserves the
  // flags; for count > 1 OF is architecturally undefined).
  static void emitShiftRotateOF(LiftState &S, NdVar Cnt, NdVar OfBit);
  // x86 shifts/rotates leave the flags unchanged for a zero (post-mask) count.
  // Restore each listed (flag register, pre-instruction snapshot) pair when
  // \p Cnt == 0; otherwise keep the freshly computed flag.  Snapshot the flags
  // (1-byte COPYs) before the handler overwrites them.
  static void
  emitZeroCountFlagGuard(LiftState &S, NdVar Cnt,
                         std::initializer_list<std::pair<int, NdVar>> Flags);

private:
  bool liftCore(LiftState &S, const cs_insn *Insn, const cs_x86 &X86);
  bool liftControl(LiftState &S, const cs_insn *Insn, const cs_x86 &X86);
  bool liftAtomic(LiftState &S, const cs_insn *Insn, const cs_x86 &X86);
  bool liftString(LiftState &S, const cs_insn *Insn, const cs_x86 &X86);

  /// Lift a REP/REPE/REPNE-prefixed CMPS or SCAS.  The loop terminates on a
  /// data-dependent ZF transition, so the hardware instruction the backend
  /// emits for the intrinsic returns its leftover count.  The lifter derives
  /// the pointer advances from that count and recomputes the status flags in
  /// MedIR from the last element pair the loop compared.
  void liftRepCmpScas(LiftState &S, Intrinsic Id, unsigned ElemSz, bool IsScas,
                      bool IsRepne);
  bool liftFPU(LiftState &S, const cs_insn *Insn, const cs_x86 &X86);
  bool liftExt(LiftState &S, const cs_insn *Insn, const cs_x86 &X86);
  bool liftSIMD(LiftState &S, const cs_insn *Insn, const cs_x86 &X86);
  bool liftSIMDAVX(LiftState &S, const cs_insn *Insn, const cs_x86 &X86);
  bool liftSIMDLegacy(LiftState &S, const cs_insn *Insn, const cs_x86 &X86);

  /// AVX2 VSIB gather (VPGATHER{DD,DQ,QD,QQ} / VGATHER{DPS,DPD,QPS,QPD}): lift
  /// natively to a per-lane conditional load (the index vector + scale + base
  /// give each element's address; the mask sign bit selects load vs the source
  /// lane) so the recompiled code is plain scalar loads — no opaque intrinsic.
  /// \returns false when the operand shape is unexpected (caller keeps a stub).
  bool liftVectorGather(LiftState &S, const cs_x86 &X86, unsigned InsnId);

  Arch TargetArch;
  bool Strict = true;
  int FPUTop = 0;

  /// Set when the instruction just lifted reset TOP to an absolute value
  /// (FNINIT/FNCLEX) rather than a relative push/pop; cleared at each lift.
  bool FpuReset = false;

  /// Accumulated `ret imm` pop bytes for the current function (see
  /// getRetPopBytes); reset per function in resetFpuState.
  int FuncRetPopBytes = 0;

  /// Cross-instruction state for CQO/CDQ/CWD + IDIV/DIV pattern.
  /// Set by CQO/CDQ/CWD, cleared after IDIV/DIV or any other RDX write.
  enum class RdxState {
    Unknown,
    SignExtRAX,
    Zero
  } LastRdxState = RdxState::Unknown;
  uint16_t LastRdxSize = 0;

  /// Cross-instruction state for the i386 PIC get-PC thunk `call $+5; pop reg`.
  /// The CALL handler arms \c GetPcPending with the next-instruction address;
  /// if the very next instruction is a `pop`, the POP handler resolves the
  /// popped register to that constant PC (so the constant pool / GOT base folds
  /// to a known VA the emitter can redirect to rodata).
  bool GetPcPending = false;
  bool GetPcArmedThisInsn = false;
  uint64_t GetPcValue = 0;
};

} // namespace neverd

#endif // NEVERD_LIFT_X86LIFTER_H
