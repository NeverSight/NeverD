//===- X86LiftString.cpp - x86/x64 string & system instruction lifter ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// String-operation, system, and privileged instruction handlers for x86/x64:
/// REP MOVS/STOS/CMPS/SCAS/LODS, XLATB, SYSCALL/INT, and
/// CPUID/RDTSC/fences/prefetch.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

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

// True when a REP/REPE prefix (0xF3) is attached (rep stos/movs/lods).  CMPS
// and SCAS may also carry REPNE (0xF2); those are handled elsewhere.
bool hasRepPrefix(const cs_x86 &X86) {
  for (int I = 0; I < 4; ++I)
    if (X86.prefix[I] == X86_PREFIX_REP)
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

// Direction-aware pointer step for single string ops: +ElemSz when DF=0
// (forward) and -ElemSz when DF=1 (backward).  Folds to a constant whenever the
// direction flag is statically known (e.g. after CLD/STD), so the common
// forward case keeps its original zero-cost lowering.
NdVar X86Lifter::dirStep(LiftState &S, unsigned ElemSz) {
  NdVar Step = S.makeTemp(8);
  S.emit(NdOp::SELECT, Step,
         {NdVar::reg(x86reg::DF, 1),
          NdVar::cst(static_cast<uint64_t>(-static_cast<int64_t>(ElemSz)), 8),
          NdVar::cst(ElemSz, 8)});
  return Step;
}

bool X86Lifter::liftString(LiftState &S, const cs_insn *Insn,
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
        NdVar Src = operandRead(S, X86.operands[1]);
        if (X86.operands[0].type == X86_OP_MEM) {
          // MOVSD stores only the low 64 bits (scalar double)
          if (Src.Size > 8) {
            NdVar Lo = S.makeTemp(8);
            S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
            Src = Lo;
          }
          S.storeToMem(X86.operands[0], Src);
        } else {
          NdVar Dst = operandWrite(X86.operands[0]);
          // SSE MOVSD reg-to-reg merges only the low 64 bits and PRESERVES the
          // destination's upper 64 bits (unlike mem-to-reg which zero-extends).
          // A plain full-width COPY here corrupts the high lane — e.g. clang's
          // SSE2 vectorizer uses `movsd xmm2,xmm1` to splice two halves of a
          // <4 x i32> together, so a full copy drops two lanes.
          if (X86.operands[1].type == X86_OP_REG && Dst.Size >= 16 &&
              Src.Size >= 16) {
            NdVar DstR = operandRead(S, X86.operands[0]);
            NdVar SrcLo = S.makeTemp(8);
            S.emit(NdOp::SUBBYTES, SrcLo, {Src, NdVar::cst(0, 4)});
            NdVar DstHi = S.makeTemp(Dst.Size - 8);
            S.emit(NdOp::SUBBYTES, DstHi, {DstR, NdVar::cst(8, 4)});
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
                        {NdVar::reg(x86reg::RDI, 8),
                         NdVar::reg(x86reg::RCX, 8),
                         NdVar::reg(x86reg::RAX, ElemSz), Df});
      else
        S.emitIntrinsic(movsIntrinsic(ElemSz), S.makeTemp(8),
                        {NdVar::reg(x86reg::RSI, 8),
                         NdVar::reg(x86reg::RDI, 8),
                         NdVar::reg(x86reg::RCX, 8), Df});
      // Signed pointer delta = DF ? -(RCX*ElemSz) : +(RCX*ElemSz); RCX -> 0.
      NdVar Bytes = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Bytes,
             {NdVar::reg(x86reg::RCX, 8), NdVar::cst(ElemSz, 8)});
      NdVar NegBytes = S.makeTemp(8);
      S.emit(NdOp::INT_SUB, NegBytes, {NdVar::cst(0, 8), Bytes});
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
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RCX, 8), {NdVar::cst(0, 8)});
    } else {
      // Single element (no REP): one store/copy; pointer(s) step per DF.
      NdVar Step = dirStep(S, ElemSz);
      if (IsStos) {
        S.emit(
            NdOp::STORE, {},
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
        S.emit(NdOp::SUBBYTES, La, {Dst, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::cst(I * LaneSz, 4)});
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
        S.emit(NdOp::SUBBYTES, Hi, {Dst, NdVar::cst(LaneSz, 4)});
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
      S.emitIntrinsic(Intrinsic::Cmpsd_str);
      for (uint64_t RO : {x86reg::RSI, x86reg::RDI, x86reg::RCX}) {
        NdVar Tmp = S.makeTemp(8);
        S.emit(NdOp::COPY, NdVar::reg(RO, 8), {Tmp});
      }
      S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {NdVar::cst(0, 1)});
    }
    break;
  }

  // --- Privileged / synchronization / debugger-trap instructions ---
  case X86_INS_INT3:
  case X86_INS_UD2:
  case X86_INS_CPUID:
  case X86_INS_XGETBV:
  case X86_INS_RDTSC:
  case X86_INS_RDTSCP:
  case X86_INS_PAUSE:
  case X86_INS_MFENCE:
  case X86_INS_LFENCE:
  case X86_INS_SFENCE:
  case X86_INS_CLFLUSH:
  case X86_INS_PREFETCH:
  case X86_INS_PREFETCHT0:
  case X86_INS_PREFETCHT1:
  case X86_INS_PREFETCHT2:
  case X86_INS_PREFETCHNTA: {
    Intrinsic Id = Intrinsic::None;
    std::vector<std::pair<uint64_t, uint16_t>> Writes;
    std::vector<NdVar> ExtraInputs;
    switch (InsnId) {
    case X86_INS_INT3:
      Id = Intrinsic::Int3;
      break;
    case X86_INS_UD2:
      Id = Intrinsic::Ud2;
      break;
    case X86_INS_CPUID:
      Id = Intrinsic::Cpuid;
      Writes = {{x86reg::RAX, 4},
                {x86reg::RBX, 4},
                {x86reg::RCX, 4},
                {x86reg::RDX, 4}};
      ExtraInputs = {NdVar::reg(x86reg::RAX, 4),
                     NdVar::reg(x86reg::RCX, 4)};
      break;
    case X86_INS_RDTSC:
      Id = Intrinsic::Rdtsc;
      Writes = {{x86reg::RAX, 4}, {x86reg::RDX, 4}};
      break;
    case X86_INS_XGETBV:
      Id = Intrinsic::Xgetbv;
      Writes = {{x86reg::RAX, 4}, {x86reg::RDX, 4}};
      ExtraInputs = {NdVar::reg(x86reg::RCX, 4)};
      break;
    case X86_INS_PAUSE:
      Id = Intrinsic::Pause;
      break;
    case X86_INS_MFENCE:
      Id = Intrinsic::Mfence;
      break;
    case X86_INS_LFENCE:
      Id = Intrinsic::Lfence;
      break;
    case X86_INS_SFENCE:
      Id = Intrinsic::Sfence;
      break;
    case X86_INS_RDTSCP:
      Id = Intrinsic::Rdtscp;
      Writes = {{x86reg::RAX, 4}, {x86reg::RDX, 4}, {x86reg::RCX, 4}};
      break;
    case X86_INS_CLFLUSH:
      Id = Intrinsic::Clflush;
      if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM &&
          X86.operands[0].mem.base != X86_REG_INVALID) {
        auto RI =
            mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].mem.base));
        ExtraInputs = {NdVar::reg(RI.Offset, 8)};
      }
      break;
    default:
      Id = Intrinsic::Prefetch;
      break;
    }
    {
      LowOp LOp;
      LOp.Opcode = NdOp::INTRINSIC;
      LOp.Addr = S.Addr;
      LOp.Seq = S.Seq++;
      // A value-producing intrinsic (CPUID/RDTSC/XGETBV) carries its primary
      // result in RAX; a side-effect-only one (a fence / prefetch / trap, with
      // no Writes) carries NO output, so SSA never bumps RAX for it.  A bare
      // RAX destination on a void fence would shadow a live RAX (mirrors the
      // ARM `dmb` self-loop bug) with the op's never-assigned, zero-defaulted
      // output.
      LOp.Output = Writes.empty() ? NdVar() : NdVar::reg(x86reg::RAX, 8);
      LOp.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
      for (auto &V : ExtraInputs)
        LOp.addInput(V);
      S.Ops.push_back(LOp);
    }
    for (auto &[RegOff, Sz] : Writes) {
      NdVar Tmp = S.makeTemp(Sz);
      S.emit(NdOp::COPY, NdVar::reg(RegOff, Sz), {Tmp});
    }
    break;
  }

  // --- SYSCALL / INT ---
  case X86_INS_SYSCALL:
  case X86_INS_INT: {
    Intrinsic Id =
        (InsnId == X86_INS_SYSCALL) ? Intrinsic::Syscall : Intrinsic::IntN;
    if (InsnId == X86_INS_INT && X86.op_count >= 1 &&
        X86.operands[0].type == X86_OP_IMM && X86.operands[0].imm == 3) {
      Id = Intrinsic::Int3;
    }
    if (InsnId == X86_INS_INT && X86.op_count >= 1 &&
        X86.operands[0].type == X86_OP_IMM) {
      S.emitIntrinsic(
          Id, NdVar::reg(x86reg::RAX, 8),
          {NdVar::cst(static_cast<uint64_t>(X86.operands[0].imm), 1)});
    } else {
      S.emitIntrinsic(Id);
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
      S.emit(NdOp::INT_NOTEQUAL, RcxNZ, {Rcx, NdVar::cst(0, 8)});
      NdVar RcxM1 = S.makeTemp(8);
      S.emit(NdOp::INT_SUB, RcxM1, {Rcx, NdVar::cst(1, 8)});
      NdVar LastOff = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, LastOff, {RcxM1, NdVar::cst(ElemSz, 8)});
      NdVar SafeOff = S.makeTemp(8);
      S.emit(NdOp::SELECT, SafeOff, {RcxNZ, LastOff, NdVar::cst(0, 8)});
      NdVar LastAddr = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, LastAddr, {Rsi, SafeOff});
      NdVar LastVal = S.makeTemp(ElemSz);
      S.emit(NdOp::LOAD, LastVal, {LastAddr});
      NdVar NewAx = S.makeTemp(ElemSz);
      S.emit(NdOp::SELECT, NewAx,
             {RcxNZ, LastVal, NdVar::reg(x86reg::RAX, ElemSz)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RAX, ElemSz), {NewAx});
      NdVar Bytes = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Bytes, {Rcx, NdVar::cst(ElemSz, 8)});
      NdVar NewSi = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, NewSi, {Rsi, Bytes});
      S.emit(NdOp::COPY, Rsi, {NewSi});
      S.emit(NdOp::COPY, Rcx, {NdVar::cst(0, 8)});
    } else {
      S.emit(NdOp::LOAD, NdVar::reg(x86reg::RAX, ElemSz), {Rsi});
      NdVar NewSi = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, NewSi, {Rsi, dirStep(S, ElemSz)});
      S.emit(NdOp::COPY, Rsi, {NewSi});
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
  // the final RCX/RDI/RSI and flags cannot be computed in closed form; they are
  // left as a non-crashing placeholder (compilers virtually never emit them —
  // memcmp/strlen/memchr are open-coded or call the libc routine).
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
    // REP/REPE/REPNE form: data-dependent loop — non-crashing placeholder.
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
    S.emitIntrinsic(Id);
    for (uint64_t RO : {x86reg::RSI, x86reg::RDI, x86reg::RCX}) {
      NdVar Tmp = S.makeTemp(8);
      S.emit(NdOp::COPY, NdVar::reg(RO, 8), {Tmp});
    }
    S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {NdVar::cst(0, 1)});
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
