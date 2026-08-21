//===- X86LiftString.cpp - x86/x64 string & system instruction lifter ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches the x86/x64 string operations, and lifts the compare/scan forms
/// (CMPS/SCAS and the CMPSD/CMPSS disambiguation) here because they reach the
/// private liftRepCmpScas, defined below along with the direction-flag step
/// helper.  MOVS/STOS/LODS/XLATB are in X86LiftStringMove.cpp and the
/// trap/serializing/system-call instructions in X86LiftSystem.cpp.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/Support/Debug.h"

#include <iterator>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

// Element size (bytes) of a MOVS/STOS/LODS/SCAS/CMPS variant.
unsigned stringElemSize(unsigned InsnId) {
  switch (InsnId) {
  case X86_INS_MOVSW:
  case X86_INS_STOSW:
  case X86_INS_LODSW:
  case X86_INS_SCASW:
  case X86_INS_CMPSW:
    return 2;
  case X86_INS_MOVSD:
  case X86_INS_STOSD:
  case X86_INS_LODSD:
  case X86_INS_SCASD:
    return 4;
  case X86_INS_MOVSQ:
  case X86_INS_STOSQ:
  case X86_INS_LODSQ:
  case X86_INS_SCASQ:
  case X86_INS_CMPSQ:
    return 8;
  default:
    return 1;
  }
}

namespace {

// True when a REPNE/REPNZ prefix (0xF2) is attached.  On CMPS/SCAS this selects
// the "repeat while equal fails" loop (`memchr` / `strlen` idioms); REP/REPE
// (0xF3) selects the "repeat while equal" loop (`memcmp`).
bool hasRepnePrefix(const cs_x86 &X86) {
  for (int I = 0; I < 4; ++I)
    if (X86.prefix[I] == X86_PREFIX_REPNE)
      return true;
  return false;
}

// True when any repeat prefix is attached: REP/REPE (0xF3, == X86_PREFIX_REPE)
// or REPNE/REPNZ (0xF2).  CMPS/SCAS use REPE for `==`-terminated loops and
// REPNE for `!=`-terminated loops (memcmp / strlen / memchr idioms).
bool hasRepOrRepnePrefix(const cs_x86 &X86) {
  for (int I = 0; I < 4; ++I)
    if (X86.prefix[I] == X86_PREFIX_REP || X86.prefix[I] == X86_PREFIX_REPNE)
      return true;
  return false;
}

} // namespace

// Direction-aware pointer step for single string ops: +ElemSz when DF=0
// (forward) and -ElemSz when DF=1 (backward).  Folds to a constant whenever the
// direction flag is statically known (e.g. after CLD/STD), so the common
// forward case keeps its original zero-cost lowering.
NdVar X86Lifter::dirStep(LiftState &S, unsigned ElemSz) {
  NdVar Step = S.makeTemp(8);
  S.emit(
      NdOp::SELECT, Step,
      {NdVar::reg(x86reg::DF, 1),
       NdVar::scalar(static_cast<uint64_t>(-static_cast<int64_t>(ElemSz)), 8),
       NdVar::scalar(ElemSz, 8)});
  return Step;
}

// REP/REPE/REPNE CMPS / SCAS.
//
// The loop exits on whichever comes first: RCX hitting zero, or the ZF
// transition the prefix selects (REPE stops on the first mismatch, REPNE on the
// first match).  That termination point is data dependent, so unlike REP
// MOVS/STOS the trip count is not known here; the backend lowers the intrinsic
// to the real `rep cmps/scas` and yields the hardware's leftover RCX.
//
// Everything else follows from that one value.  The loop decrements RCX and
// advances the pointer(s) by one direction step in lockstep, so
//   iterations = RCX_in - RCX_out,  RSI/RDI += iterations * step,
// and the flags are those of the LAST element pair compared, which sits one
// step back from the final pointers.  Recomputing them as an ordinary CMP keeps
// the flags real MedIR values that condition recovery and flag elimination can
// still reason about, instead of opaque asm outputs.  A loop that never ran
// (RCX == 0 on entry) compares nothing and leaves the flags untouched: the
// flag-reconstruction loads are redirected to the live stack frame, and each
// flag falls back to its incoming value.
void X86Lifter::liftRepCmpScas(LiftState &S, Intrinsic Id, unsigned ElemSz,
                               bool IsScas, bool IsRepne) {
  NdVar Rsi = NdVar::reg(x86reg::RSI, 8);
  NdVar Rdi = NdVar::reg(x86reg::RDI, 8);
  NdVar Rcx = NdVar::reg(x86reg::RCX, 8);
  NdVar Df = NdVar::reg(x86reg::DF, 1);
  NdVar RepKind = NdVar::scalar(IsRepne ? 1 : 0, 1);

  // Snapshot the entry count and "the loop body ran at least once" before the
  // count is overwritten.
  NdVar RcxIn = S.makeTemp(8);
  S.emit(NdOp::COPY, RcxIn, {Rcx});
  NdVar RcxNZ = S.makeTemp(1);
  S.emit(NdOp::INT_NOTEQUAL, RcxNZ, {RcxIn, NdVar::scalar(0, 8)});
  NdVar Step = dirStep(S, ElemSz);

  // The intrinsic yields the leftover count.  Note the destination is NOT the
  // default RAX: SCAS reads the accumulator but never writes it, so an RAX
  // destination would clobber a live accumulator.
  NdVar RcxOut = S.makeTemp(8);
  if (IsScas)
    S.emitIntrinsic(Id, RcxOut,
                    {Rdi, Rcx, NdVar::reg(x86reg::RAX, ElemSz), Df, RepKind});
  else
    S.emitIntrinsic(Id, RcxOut, {Rsi, Rdi, Rcx, Df, RepKind});

  NdVar Iters = S.makeTemp(8);
  S.emit(NdOp::INT_SUB, Iters, {RcxIn, RcxOut});
  NdVar Delta = S.makeTemp(8);
  S.emit(NdOp::INT_MULT, Delta, {Iters, Step});

  NdVar NewSi;
  if (!IsScas) {
    NewSi = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, NewSi, {Rsi, Delta});
  }
  NdVar NewDi = S.makeTemp(8);
  S.emit(NdOp::INT_ADD, NewDi, {Rdi, Delta});
  if (!IsScas)
    S.emit(NdOp::COPY, Rsi, {NewSi});
  S.emit(NdOp::COPY, Rdi, {NewDi});
  S.emit(NdOp::COPY, Rcx, {RcxOut});

  // Re-read the last compared pair through the final pointers.
  NdVar Back = S.makeTemp(8);
  S.emit(NdOp::SELECT, Back, {RcxNZ, Step, NdVar::scalar(0, 8)});
  NdVar DiLast = S.makeTemp(8);
  S.emit(NdOp::INT_SUB, DiLast, {NewDi, Back});
  // A MedIR LOAD is unconditional even when its value later feeds the false
  // arm of a SELECT.  REP with an entry count of zero must not touch RSI/RDI,
  // so route that otherwise-dead probe through the live stack frame instead.
  NdVar SafeProbe = NdVar::reg(x86reg::RSP, 8);
  NdVar DiProbe = S.makeTemp(8);
  S.emit(NdOp::SELECT, DiProbe, {RcxNZ, DiLast, SafeProbe});
  NdVar B = S.makeTemp(ElemSz);
  S.emit(NdOp::LOAD, B, {DiProbe});
  NdVar A;
  if (IsScas) {
    A = NdVar::reg(x86reg::RAX, ElemSz);
  } else {
    NdVar SiLast = S.makeTemp(8);
    S.emit(NdOp::INT_SUB, SiLast, {NewSi, Back});
    NdVar SiProbe = S.makeTemp(8);
    S.emit(NdOp::SELECT, SiProbe, {RcxNZ, SiLast, SafeProbe});
    A = S.makeTemp(ElemSz);
    S.emit(NdOp::LOAD, A, {SiProbe});
  }

  static constexpr uint64_t FlagRegs[] = {x86reg::CF, x86reg::PF, x86reg::AF,
                                          x86reg::ZF, x86reg::SF, x86reg::OF};
  NdVar Old[std::size(FlagRegs)];
  for (size_t I = 0; I < std::size(FlagRegs); ++I) {
    Old[I] = S.makeTemp(1);
    S.emit(NdOp::COPY, Old[I], {NdVar::reg(FlagRegs[I], 1)});
  }

  NdVar Res = S.makeTemp(ElemSz);
  S.emit(NdOp::INT_SUB, Res, {A, B});
  emitFlagsArith(S, Res, A, B, /*IsSub=*/true);

  for (size_t I = 0; I < std::size(FlagRegs); ++I) {
    NdVar New = S.makeTemp(1);
    S.emit(NdOp::COPY, New, {NdVar::reg(FlagRegs[I], 1)});
    NdVar Sel = S.makeTemp(1);
    S.emit(NdOp::SELECT, Sel, {RcxNZ, New, Old[I]});
    S.emit(NdOp::COPY, NdVar::reg(FlagRegs[I], 1), {Sel});
  }
}

bool X86Lifter::liftString(LiftState &S, const cs_insn *Insn,
                           const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // CMPSD/CMPSS (string variant vs SSE scalar disambiguation)
  case X86_INS_CMPSD:
  case X86_INS_CMPSS: {
    bool HasXmm = false;
    for (uint8_t N = 0; N < X86.op_count; ++N) {
      if (X86.operands[N].type == X86_OP_REG &&
          X86.operands[N].reg >= X86_REG_XMM0 &&
          X86.operands[N].reg <= X86_REG_XMM15) {
        HasXmm = true;
        break;
      }
    }
    if (HasXmm) {
      if (X86.op_count < 2)
        break;
      NdVar Dst = operandWrite(X86.operands[0]);
      NdVar Src = operandRead(S, X86.operands[1]);
      uint8_t Pred = 0;
      bool FoundImm = false;
      for (uint8_t N = 0; N < X86.op_count; ++N)
        if (X86.operands[N].type == X86_OP_IMM) {
          Pred = static_cast<uint8_t>(X86.operands[N].imm) & 7;
          FoundImm = true;
        }
      if (!FoundImm && Insn->size >= 1)
        Pred = Insn->bytes[Insn->size - 1] & 7;
      // CMPSS/CMPSD are scalar: compare only lane 0, preserve the dst high
      // half. (Packed CMPPS/CMPPD route to the SIMD lifter, never reach here.)
      bool IsPacked = false;
      bool IsDouble = (InsnId == X86_INS_CMPSD);
      unsigned LaneSz = IsDouble ? 8 : 4;
      unsigned NLanes = IsPacked ? (Dst.Size / LaneSz) : 1;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        // Always extract the operand lane: the scalar form must compare only
        // the low LaneSz bytes, not the full XMM register.
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {Dst, NdVar::scalar(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::scalar(I * LaneSz, 4)});
        NdVar CmpRes = S.makeTemp(1);
        bool Neg = false;
        switch (Pred) {
        default:
        case 0:
          S.emit(NdOp::FLOAT_EQUAL, CmpRes, {La, Lb});
          break;
        case 1:
          S.emit(NdOp::FLOAT_LESS, CmpRes, {La, Lb});
          break;
        case 2:
          S.emit(NdOp::FLOAT_LESSEQUAL, CmpRes, {La, Lb});
          break;
        case 3: { // UNORD: isNaN(a) || isNaN(b) — must inspect BOTH operands
          NdVar NanA = S.makeTemp(1), NanB = S.makeTemp(1);
          S.emit(NdOp::FLOAT_ISNAN, NanA, {La});
          S.emit(NdOp::FLOAT_ISNAN, NanB, {Lb});
          S.emit(NdOp::BOOL_OR, CmpRes, {NanA, NanB});
          break;
        }
        case 4:
          S.emit(NdOp::FLOAT_NOTEQUAL, CmpRes, {La, Lb});
          break;
        case 5:
          S.emit(NdOp::FLOAT_LESS, CmpRes, {La, Lb});
          Neg = true;
          break;
        case 6:
          S.emit(NdOp::FLOAT_LESSEQUAL, CmpRes, {La, Lb});
          Neg = true;
          break;
        case 7: { // ORD: !(isNaN(a) || isNaN(b)) — must inspect BOTH operands
          NdVar NanA = S.makeTemp(1), NanB = S.makeTemp(1);
          S.emit(NdOp::FLOAT_ISNAN, NanA, {La});
          S.emit(NdOp::FLOAT_ISNAN, NanB, {Lb});
          S.emit(NdOp::BOOL_OR, CmpRes, {NanA, NanB});
          Neg = true;
          break;
        }
        }
        if (Neg) {
          NdVar NotCmp = S.makeTemp(1);
          S.emit(NdOp::BOOL_NOT, NotCmp, {CmpRes});
          CmpRes = NotCmp;
        }
        NdVar Mask = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_ZEXT, Mask, {CmpRes});
        NdVar AllOnes = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, AllOnes, {Mask});
        if (I == 0)
          Acc = AllOnes;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {AllOnes, Acc});
          Acc = Next;
        }
      }
      if (!IsPacked && Dst.Size > LaneSz) {
        NdVar Hi = S.makeTemp(Dst.Size - LaneSz);
        S.emit(NdOp::SUBBYTES, Hi, {Dst, NdVar::scalar(LaneSz, 4)});
        S.emit(NdOp::CONCAT, Dst, {Hi, Acc});
      } else if (Acc.Size < Dst.Size) {
        NdVar Wide = S.makeTemp(Dst.Size);
        S.emit(NdOp::INT_ZEXT, Wide, {Acc});
        S.emit(NdOp::COPY, Dst, {Wide});
      } else {
        S.emit(NdOp::COPY, Dst, {Acc});
      }
    } else if (!hasRepOrRepnePrefix(X86)) {
      // String CMPSD (AT&T `cmpsl`, no XMM operand): compare DS:[RSI] with
      // ES:[RDI] as a 4-byte CMP and step both pointers per DF.
      // Single form only; the REP form falls to the placeholder below.
      unsigned ElemSz = 4;
      NdVar SiVal = S.makeTemp(ElemSz);
      S.emit(NdOp::LOAD, SiVal, {NdVar::reg(x86reg::RSI, 8)});
      NdVar DiVal = S.makeTemp(ElemSz);
      S.emit(NdOp::LOAD, DiVal, {NdVar::reg(x86reg::RDI, 8)});
      NdVar Res = S.makeTemp(ElemSz);
      S.emit(NdOp::INT_SUB, Res, {SiVal, DiVal});
      emitFlagsArith(S, Res, SiVal, DiVal, /*IsSub=*/true);
      NdVar Step = dirStep(S, ElemSz);
      NdVar NewSi = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, NewSi, {NdVar::reg(x86reg::RSI, 8), Step});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RSI, 8), {NewSi});
      NdVar NewDi = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, NewDi, {NdVar::reg(x86reg::RDI, 8), Step});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RDI, 8), {NewDi});
    } else {
      // String CMPSD (`cmpsl`) under REP/REPE/REPNE.  stringElemSize cannot
      // classify X86_INS_CMPSD (it doubles as the SSE scalar compare), so the
      // 4-byte element size is passed explicitly.
      liftRepCmpScas(S, Intrinsic::Cmpsd_str, /*ElemSz=*/4, /*IsScas=*/false,
                     hasRepnePrefix(X86));
    }
    break;
  }

  // --- Compare/scan string ops (CMPS/SCAS) ---
  // SCAS compares the accumulator (AL/AX/EAX/RAX) with ES:[RDI]; CMPS compares
  // DS:[RSI] with ES:[RDI].  Both set the status flags exactly like CMP (a SUB
  // whose result is discarded) and advance the pointer(s) by the element size.
  //
  // The single (non-repeated) form has no memory write and a deterministic
  // pointer step, so the whole effect is modelled directly in MedIR (like
  // LODS) — reusing emitFlagsArith makes the flags bit-identical to a real CMP.
  // Pointers step +/-ElemSz per the direction flag (see dirStep), matching the
  // single MOVS/STOS/LODS handlers above.
  //
  // The REP/REPE/REPNE forms terminate on a data-dependent ZF transition, so
  // the final RCX/RDI/RSI come back from the hardware loop the backend emits
  // for the intrinsic; see liftRepCmpScas.
  case X86_INS_CMPSB:
  case X86_INS_CMPSW:
  case X86_INS_CMPSQ:
  case X86_INS_SCASB:
  case X86_INS_SCASW:
  case X86_INS_SCASD:
  case X86_INS_SCASQ: {
    bool IsScas = (InsnId == X86_INS_SCASB || InsnId == X86_INS_SCASW ||
                   InsnId == X86_INS_SCASD || InsnId == X86_INS_SCASQ);
    if (!hasRepOrRepnePrefix(X86)) {
      unsigned ElemSz = stringElemSize(InsnId);
      NdVar DiVal = S.makeTemp(ElemSz);
      S.emit(NdOp::LOAD, DiVal, {NdVar::reg(x86reg::RDI, 8)});
      NdVar CmpA, CmpB = DiVal;
      if (IsScas) {
        // result = AL/AX/EAX/RAX - ES:[RDI]
        CmpA = NdVar::reg(x86reg::RAX, ElemSz);
      } else {
        // result = DS:[RSI] - ES:[RDI]
        NdVar SiVal = S.makeTemp(ElemSz);
        S.emit(NdOp::LOAD, SiVal, {NdVar::reg(x86reg::RSI, 8)});
        CmpA = SiVal;
      }
      NdVar Res = S.makeTemp(ElemSz);
      S.emit(NdOp::INT_SUB, Res, {CmpA, CmpB});
      emitFlagsArith(S, Res, CmpA, CmpB, /*IsSub=*/true);
      // Step the pointer(s) by the element size per DF (forward/backward).
      NdVar Step = dirStep(S, ElemSz);
      if (!IsScas) {
        NdVar NewSi = S.makeTemp(8);
        S.emit(NdOp::INT_ADD, NewSi, {NdVar::reg(x86reg::RSI, 8), Step});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::RSI, 8), {NewSi});
      }
      NdVar NewDi = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, NewDi, {NdVar::reg(x86reg::RDI, 8), Step});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RDI, 8), {NewDi});
      break;
    }
    // REP/REPE/REPNE form: data-dependent loop, lowered to the hardware
    // instruction (see liftRepCmpScas).
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_CMPSB:
      Id = Intrinsic::Cmpsb;
      break;
    case X86_INS_CMPSW:
      Id = Intrinsic::Cmpsw;
      break;
    case X86_INS_CMPSQ:
      Id = Intrinsic::Cmpsq;
      break;
    case X86_INS_SCASB:
      Id = Intrinsic::Scasb;
      break;
    case X86_INS_SCASW:
      Id = Intrinsic::Scasw;
      break;
    case X86_INS_SCASD:
      Id = Intrinsic::Scasd;
      break;
    default:
      Id = Intrinsic::Scasq;
      break;
    }
    liftRepCmpScas(S, Id, stringElemSize(InsnId), IsScas, hasRepnePrefix(X86));
    break;
  }

  default:
    return liftStringMove(*this, S, Insn, X86) ||
           liftSystem(*this, S, Insn, X86);
  }
  return true;
}

} // namespace neverd
