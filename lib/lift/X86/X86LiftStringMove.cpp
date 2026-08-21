//===- X86LiftStringMove.cpp - x86/x64 string move/store/load lifter ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The string operations whose trip count is known up front:
/// MOVS, STOS and LODS (with and without a REP prefix) plus
/// XLATB.  The compare/scan forms terminate on a data-dependent
/// ZF transition and stay with the dispatcher in
/// X86LiftString.cpp.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

// True when a REP/REPE prefix (0xF3) is attached (rep stos/movs/lods).  CMPS
// and SCAS may also carry REPNE (0xF2); those are handled elsewhere.
bool hasRepPrefix(const cs_x86 &X86) {
  for (int I = 0; I < 4; ++I)
    if (X86.prefix[I] == X86_PREFIX_REP)
      return true;
  return false;
}

Intrinsic stosIntrinsic(unsigned Sz) {
  switch (Sz) {
  case 2:
    return Intrinsic::Stosw;
  case 4:
    return Intrinsic::Stosd;
  case 8:
    return Intrinsic::Stosq;
  default:
    return Intrinsic::Stosb;
  }
}

Intrinsic movsIntrinsic(unsigned Sz) {
  switch (Sz) {
  case 2:
    return Intrinsic::Movsw;
  case 4:
    return Intrinsic::Movsd;
  case 8:
    return Intrinsic::Movsq;
  default:
    return Intrinsic::Movsb;
  }
}

} // namespace

bool liftStringMove(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // --- MOVS / STOS / REP prefix ---
  case X86_INS_MOVSB:
  case X86_INS_MOVSW:
  case X86_INS_MOVSD:
  case X86_INS_MOVSQ:
  case X86_INS_STOSB:
  case X86_INS_STOSW:
  case X86_INS_STOSD:
  case X86_INS_STOSQ: {
    bool HasXmm = false;
    for (uint8_t N = 0; N < X86.op_count; ++N) {
      if (X86.operands[N].type == X86_OP_REG &&
          X86.operands[N].reg >= X86_REG_XMM0 &&
          X86.operands[N].reg <= X86_REG_XMM15) {
        HasXmm = true;
        break;
      }
    }
    if (HasXmm && InsnId == X86_INS_MOVSD) {
      if (X86.op_count >= 2) {
        NdVar Src = L.operandRead(S, X86.operands[1]);
        if (X86.operands[0].type == X86_OP_MEM) {
          // MOVSD stores only the low 64 bits (scalar double)
          if (Src.Size > 8) {
            NdVar Lo = S.makeTemp(8);
            S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::scalar(0, 4)});
            Src = Lo;
          }
          S.storeToMem(X86.operands[0], Src);
        } else {
          NdVar Dst = L.operandWrite(X86.operands[0]);
          // SSE MOVSD reg-to-reg merges only the low 64 bits and PRESERVES the
          // destination's upper 64 bits (unlike mem-to-reg which zero-extends).
          // A plain full-width COPY here corrupts the high lane — e.g. clang's
          // SSE2 vectorizer uses `movsd xmm2,xmm1` to splice two halves of a
          // <4 x i32> together, so a full copy drops two lanes.
          if (X86.operands[1].type == X86_OP_REG && Dst.Size >= 16 &&
              Src.Size >= 16) {
            NdVar DstR = L.operandRead(S, X86.operands[0]);
            NdVar SrcLo = S.makeTemp(8);
            S.emit(NdOp::SUBBYTES, SrcLo, {Src, NdVar::scalar(0, 4)});
            NdVar DstHi = S.makeTemp(Dst.Size - 8);
            S.emit(NdOp::SUBBYTES, DstHi, {DstR, NdVar::scalar(8, 4)});
            S.emit(NdOp::CONCAT, Dst, {DstHi, SrcLo});
          } else if (X86.operands[1].type == X86_OP_MEM &&
                     Src.Size < Dst.Size) {
            // MOVSD m64->xmm zero-extends bits [127:64]; make it explicit so a
            // later full-width read (e.g. a movdqa spill) does not see stale
            // upper bytes from a plain narrow COPY.
            NdVar Ext = S.makeTemp(Dst.Size);
            S.emit(NdOp::INT_ZEXT, Ext, {Src});
            S.emit(NdOp::COPY, Dst, {Ext});
          } else {
            S.emit(NdOp::COPY, Dst, {Src});
          }
        }
      }
      break;
    }
    // REP MOVS/STOS perform the actual memory copy/fill; the emitter lowers the
    // intrinsic to a real `rep movs/stos` with RSI/RDI/RCX/AL wired through
    // register constraints.  The register side effects (pointers advanced,
    // count zeroed) are modelled directly in MedIR so they survive DCE
    // independently of the inline-asm result.  The direction flag is threaded
    // through as a final intrinsic input so the emitter runs the hardware `rep`
    // in the matching direction, and the pointer model advances by
    // +/-(RCX*ElemSz) per DF.  DF folds to a constant after a preceding
    // CLD/STD, so the common forward case keeps its plain +RCX*ElemSz lowering.
    bool IsStos = (InsnId == X86_INS_STOSB || InsnId == X86_INS_STOSW ||
                   InsnId == X86_INS_STOSD || InsnId == X86_INS_STOSQ);
    unsigned ElemSz = stringElemSize(InsnId);
    if (hasRepPrefix(X86)) {
      NdVar Df = NdVar::reg(x86reg::DF, 1);
      if (IsStos)
        S.emitIntrinsic(stosIntrinsic(ElemSz), S.makeTemp(8),
                        {NdVar::reg(x86reg::RDI, 8), NdVar::reg(x86reg::RCX, 8),
                         NdVar::reg(x86reg::RAX, ElemSz), Df});
      else
        S.emitIntrinsic(movsIntrinsic(ElemSz), S.makeTemp(8),
                        {NdVar::reg(x86reg::RSI, 8), NdVar::reg(x86reg::RDI, 8),
                         NdVar::reg(x86reg::RCX, 8), Df});
      // Signed pointer delta = DF ? -(RCX*ElemSz) : +(RCX*ElemSz); RCX -> 0.
      NdVar Bytes = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Bytes,
             {NdVar::reg(x86reg::RCX, 8), NdVar::scalar(ElemSz, 8)});
      NdVar NegBytes = S.makeTemp(8);
      S.emit(NdOp::INT_SUB, NegBytes, {NdVar::scalar(0, 8), Bytes});
      NdVar Delta = S.makeTemp(8);
      S.emit(NdOp::SELECT, Delta, {Df, NegBytes, Bytes});
      if (!IsStos) {
        NdVar NewSi = S.makeTemp(8);
        S.emit(NdOp::INT_ADD, NewSi, {NdVar::reg(x86reg::RSI, 8), Delta});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::RSI, 8), {NewSi});
      }
      NdVar NewDi = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, NewDi, {NdVar::reg(x86reg::RDI, 8), Delta});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RDI, 8), {NewDi});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RCX, 8), {NdVar::scalar(0, 8)});
    } else {
      // Single element (no REP): one store/copy; pointer(s) step per DF.
      NdVar Step = L.dirStep(S, ElemSz);
      if (IsStos) {
        S.emit(NdOp::STORE, {},
               {NdVar::reg(x86reg::RDI, 8), NdVar::reg(x86reg::RAX, ElemSz)});
      } else {
        NdVar V = S.makeTemp(ElemSz);
        S.emit(NdOp::LOAD, V, {NdVar::reg(x86reg::RSI, 8)});
        S.emit(NdOp::STORE, {}, {NdVar::reg(x86reg::RDI, 8), V});
        NdVar NewSi = S.makeTemp(8);
        S.emit(NdOp::INT_ADD, NewSi, {NdVar::reg(x86reg::RSI, 8), Step});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::RSI, 8), {NewSi});
      }
      NdVar NewDi = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, NewDi, {NdVar::reg(x86reg::RDI, 8), Step});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RDI, 8), {NewDi});
    }
    break;
  }

  // --- LODS (load string): AL/AX/EAX/RAX = [RSI]; RSI steps per DF. ---
  // LODS has no memory write, so the entire effect is modelled in MedIR (no
  // inline asm needed).  The single form steps RSI by +/-ElemSz per the
  // direction flag; the REP form below stays forward (DF=0).
  case X86_INS_LODSB:
  case X86_INS_LODSW:
  case X86_INS_LODSD:
  case X86_INS_LODSQ: {
    unsigned ElemSz = stringElemSize(InsnId);
    NdVar Rsi = NdVar::reg(x86reg::RSI, 8);
    if (hasRepPrefix(X86)) {
      // REP LODS loops over RCX elements; only the last one survives in the
      // accumulator.  Guard the load address against RCX==0 (where nothing
      // happens) so the recompiled code never dereferences a wild pointer.
      NdVar Rcx = NdVar::reg(x86reg::RCX, 8);
      NdVar RcxNZ = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, RcxNZ, {Rcx, NdVar::scalar(0, 8)});
      NdVar RcxM1 = S.makeTemp(8);
      S.emit(NdOp::INT_SUB, RcxM1, {Rcx, NdVar::scalar(1, 8)});
      NdVar LastOff = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, LastOff, {RcxM1, NdVar::scalar(ElemSz, 8)});
      NdVar SafeOff = S.makeTemp(8);
      S.emit(NdOp::SELECT, SafeOff, {RcxNZ, LastOff, NdVar::scalar(0, 8)});
      NdVar LastAddr = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, LastAddr, {Rsi, SafeOff});
      NdVar LastVal = S.makeTemp(ElemSz);
      S.emit(NdOp::LOAD, LastVal, {LastAddr});
      NdVar NewAx = S.makeTemp(ElemSz);
      S.emit(NdOp::SELECT, NewAx,
             {RcxNZ, LastVal, NdVar::reg(x86reg::RAX, ElemSz)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RAX, ElemSz), {NewAx});
      NdVar Bytes = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Bytes, {Rcx, NdVar::scalar(ElemSz, 8)});
      NdVar NewSi = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, NewSi, {Rsi, Bytes});
      S.emit(NdOp::COPY, Rsi, {NewSi});
      S.emit(NdOp::COPY, Rcx, {NdVar::scalar(0, 8)});
    } else {
      S.emit(NdOp::LOAD, NdVar::reg(x86reg::RAX, ElemSz), {Rsi});
      NdVar NewSi = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, NewSi, {Rsi, L.dirStep(S, ElemSz)});
      S.emit(NdOp::COPY, Rsi, {NewSi});
    }
    break;
  }

  // XLATB: AL = [RBX + AL]
  case X86_INS_XLATB: {
    NdVar AlExt = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, AlExt, {NdVar::reg(x86reg::RAX, 1)});
    NdVar EA = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA, {NdVar::reg(x86reg::RBX, 8), AlExt});
    S.emit(NdOp::LOAD, NdVar::reg(x86reg::RAX, 1), {EA});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
