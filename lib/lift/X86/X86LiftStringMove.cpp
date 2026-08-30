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

Intrinsic lodsIntrinsic(unsigned Sz) {
  switch (Sz) {
  case 2:
    return Intrinsic::Lodsw;
  case 4:
    return Intrinsic::Lodsd;
  case 8:
    return Intrinsic::Lodsq;
  default:
    return Intrinsic::Lodsb;
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
    const uint16_t AddrSz = S.AddressSize;
    if (hasRepPrefix(X86)) {
      NdVar Df = NdVar::reg(x86reg::DF, 1);
      NdMemoryAddressSpace SourceAddressSpace =
          IsStos ? NdMemoryAddressSpace::Default
                 : stringSourceAddressSpace(X86);
      if (IsStos)
        S.emitIntrinsic(stosIntrinsic(ElemSz), S.makeTemp(AddrSz),
                        {NdVar::reg(x86reg::RDI, AddrSz),
                         NdVar::reg(x86reg::RCX, AddrSz),
                         NdVar::reg(x86reg::RAX, ElemSz), Df});
      else
        S.emitIntrinsic(movsIntrinsic(ElemSz), S.makeTemp(AddrSz),
                        {NdVar::reg(x86reg::RSI, AddrSz),
                         NdVar::reg(x86reg::RDI, AddrSz),
                         NdVar::reg(x86reg::RCX, AddrSz), Df},
                        NdMemoryOrdering::None, SourceAddressSpace);
      // Signed pointer delta = DF ? -(RCX*ElemSz) : +(RCX*ElemSz); RCX -> 0.
      NdVar Bytes = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_MULT, Bytes,
             {NdVar::reg(x86reg::RCX, AddrSz), NdVar::scalar(ElemSz, AddrSz)});
      NdVar NegBytes = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_SUB, NegBytes, {NdVar::scalar(0, AddrSz), Bytes});
      NdVar Delta = S.makeTemp(AddrSz);
      S.emit(NdOp::SELECT, Delta, {Df, NegBytes, Bytes});
      if (!IsStos) {
        NdVar NewSi = S.makeTemp(AddrSz);
        S.emit(NdOp::INT_ADD, NewSi, {NdVar::reg(x86reg::RSI, AddrSz), Delta});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::RSI, AddrSz), {NewSi});
      }
      NdVar NewDi = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_ADD, NewDi, {NdVar::reg(x86reg::RDI, AddrSz), Delta});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RDI, AddrSz), {NewDi});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RCX, AddrSz),
             {NdVar::scalar(0, AddrSz)});
    } else {
      // Single element (no REP): one store/copy; pointer(s) step per DF.
      NdVar Step = L.dirStep(S, ElemSz);
      if (IsStos) {
        S.emit(
            NdOp::STORE, {},
            {NdVar::reg(x86reg::RDI, AddrSz), NdVar::reg(x86reg::RAX, ElemSz)});
      } else {
        NdVar V = S.makeTemp(ElemSz);
        S.emit(NdOp::LOAD, V, {NdVar::reg(x86reg::RSI, AddrSz)},
               NdMemoryOrdering::None, stringSourceAddressSpace(X86));
        S.emit(NdOp::STORE, {}, {NdVar::reg(x86reg::RDI, AddrSz), V});
        NdVar NewSi = S.makeTemp(AddrSz);
        S.emit(NdOp::INT_ADD, NewSi, {NdVar::reg(x86reg::RSI, AddrSz), Step});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::RSI, AddrSz), {NewSi});
      }
      NdVar NewDi = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_ADD, NewDi, {NdVar::reg(x86reg::RDI, AddrSz), Step});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RDI, AddrSz), {NewDi});
    }
    break;
  }

  // --- LODS (load string): AL/AX/EAX/RAX = [RSI]; RSI steps per DF. ---
  // A single LODS is modelled as an ordinary source-address-space LOAD.  REP
  // LODS stays a hardware intrinsic: this preserves the architectural no-load
  // and no-register-write behavior for RCX==0 and handles DF in both
  // directions without a speculative final-element load.
  case X86_INS_LODSB:
  case X86_INS_LODSW:
  case X86_INS_LODSD:
  case X86_INS_LODSQ: {
    unsigned ElemSz = stringElemSize(InsnId);
    const uint16_t AddrSz = S.AddressSize;
    NdVar Rsi = NdVar::reg(x86reg::RSI, AddrSz);
    if (hasRepPrefix(X86)) {
      NdVar Rcx = NdVar::reg(x86reg::RCX, AddrSz);
      NdVar Df = NdVar::reg(x86reg::DF, 1);
      const uint16_t AccumulatorSize = L.targetArch() == Arch::X86 ? 4 : 8;
      S.emitIntrinsic(lodsIntrinsic(ElemSz),
                      NdVar::reg(x86reg::RAX, AccumulatorSize),
                      {Rsi, Rcx,
                       NdVar::reg(x86reg::RAX, AccumulatorSize), Df},
                      NdMemoryOrdering::None, stringSourceAddressSpace(X86));
      NdVar Bytes = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_MULT, Bytes, {Rcx, NdVar::scalar(ElemSz, AddrSz)});
      NdVar NegBytes = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_SUB, NegBytes, {NdVar::scalar(0, AddrSz), Bytes});
      NdVar Delta = S.makeTemp(AddrSz);
      S.emit(NdOp::SELECT, Delta, {Df, NegBytes, Bytes});
      NdVar NewSi = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_ADD, NewSi, {Rsi, Delta});
      S.emit(NdOp::COPY, Rsi, {NewSi});
      S.emit(NdOp::COPY, Rcx, {NdVar::scalar(0, AddrSz)});
    } else {
      S.emit(NdOp::LOAD, NdVar::reg(x86reg::RAX, ElemSz), {Rsi},
             NdMemoryOrdering::None, stringSourceAddressSpace(X86));
      NdVar NewSi = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_ADD, NewSi, {Rsi, L.dirStep(S, ElemSz)});
      S.emit(NdOp::COPY, Rsi, {NewSi});
    }
    break;
  }

  // XLATB: AL = [RBX + AL]
  case X86_INS_XLATB: {
    const uint16_t AddrSz = S.AddressSize;
    NdVar AlExt = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_ZEXT, AlExt, {NdVar::reg(x86reg::RAX, 1)});
    NdVar EA = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_ADD, EA, {NdVar::reg(x86reg::RBX, AddrSz), AlExt});
    S.emit(NdOp::LOAD, NdVar::reg(x86reg::RAX, 1), {EA}, NdMemoryOrdering::None,
           stringSourceAddressSpace(X86));
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
