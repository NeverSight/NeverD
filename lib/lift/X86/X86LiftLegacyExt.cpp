//===- X86LiftLegacyExt.cpp - x86 minor ISA extension lifter --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Small, mostly vendor-specific extensions: 3DNow!, TBM,
/// CET, MOVDIRI/MOVDIR64B, PTWRITE, VIA PadLock, the 64-bit
/// xsave variants, GFNI, LWP, CLAC/STAC, CLZERO, the bare
/// prefix opcodes, MMX/SSE4a data moves and the remaining
/// system and MXCSR instructions.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftLegacyExt(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // 3DNow! — AMD's deprecated SIMD (PF* prefix). Approximate as FLOAT_*.
  // ========================================================================
  case X86_INS_PFADD:
  case X86_INS_PFACC:
  case X86_INS_PFNACC:
  case X86_INS_PFPNACC: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_ADD, Dst, {Dst, Src});
    break;
  }
  case X86_INS_PFSUB:
  case X86_INS_PFSUBR: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_SUB, Dst, {Dst, Src});
    break;
  }
  case X86_INS_PFMUL: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_MULT, Dst, {Dst, Src});
    break;
  }
  case X86_INS_PFMAX:
  case X86_INS_PFMIN: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case X86_INS_PFCMPEQ:
  case X86_INS_PFCMPGE:
  case X86_INS_PFCMPGT: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_EQUAL, Dst, {Dst, Src});
    break;
  }
  case X86_INS_PFRCP:
  case X86_INS_PFRCPIT1:
  case X86_INS_PFRCPIT2:
  case X86_INS_PFRSQIT1:
  case X86_INS_PFRSQRT: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_SQRT, Dst, {Src});
    break;
  }
  case X86_INS_PF2ID:
  case X86_INS_PF2IW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_TRUNC, Dst, {Src});
    break;
  }
  case X86_INS_PI2FD:
  case X86_INS_PI2FW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::FLOAT_INT2FLOAT, Dst, {Src});
    break;
  }
  case X86_INS_PSWAPD:
  case X86_INS_PMULHRW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // TBM — trailing bit manipulation (AMD). INT_AND/OR/XOR approximations.
  // ========================================================================
  case X86_INS_BLCFILL:
  case X86_INS_BLCI:
  case X86_INS_BLCIC:
  case X86_INS_BLCMSK:
  case X86_INS_BLCS:
  case X86_INS_BLSFILL:
  case X86_INS_BLSIC:
  case X86_INS_T1MSKC:
  case X86_INS_TZMSK: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    // All TBM ops are of the form: Dst = f(Src, Src±1). Approximate as
    // the dominant operation pattern (AND/OR with adjacents).
    switch (InsnId) {
    case X86_INS_BLCFILL: {
      // Dst = Src & (Src + 1)
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_AND, Dst, {Src, Inc});
      break;
    }
    case X86_INS_BLCI: {
      // Dst = Src | ~(Src + 1)
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      NdVar NotInc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, NotInc, {Inc});
      S.emit(NdOp::INT_OR, Dst, {Src, NotInc});
      break;
    }
    case X86_INS_BLCIC: {
      // Dst = ~Src & (Src + 1)
      NdVar NotSrc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, NotSrc, {Src});
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_AND, Dst, {NotSrc, Inc});
      break;
    }
    case X86_INS_BLCMSK: {
      // Dst = Src ^ (Src + 1)
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_XOR, Dst, {Src, Inc});
      break;
    }
    case X86_INS_BLCS: {
      // Dst = Src | (Src + 1)
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_OR, Dst, {Src, Inc});
      break;
    }
    case X86_INS_BLSFILL: {
      // Dst = Src | (Src - 1)
      NdVar Dec = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, Dec, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_OR, Dst, {Src, Dec});
      break;
    }
    case X86_INS_BLSIC: {
      // Dst = ~Src | (Src - 1)
      NdVar NotSrc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, NotSrc, {Src});
      NdVar Dec = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, Dec, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_OR, Dst, {NotSrc, Dec});
      break;
    }
    case X86_INS_T1MSKC: {
      // Dst = ~Src | (Src + 1)
      NdVar NotSrc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, NotSrc, {Src});
      NdVar Inc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ADD, Inc, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_OR, Dst, {NotSrc, Inc});
      break;
    }
    case X86_INS_TZMSK: {
      // Dst = ~Src & (Src - 1)
      NdVar NotSrc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, NotSrc, {Src});
      NdVar Dec = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, Dec, {Src, NdVar::cst(1, Dst.Size)});
      S.emit(NdOp::INT_AND, Dst, {NotSrc, Dec});
      break;
    }
    default:
      break;
    }
    break;
  }

  // ========================================================================
  // CET — Control-flow Enforcement (ENDBR, shadow stack, etc.).
  // ========================================================================
  case X86_INS_ENDBR32:
  case X86_INS_ENDBR64:
    S.emit(NdOp::NOP, {}, {});
    break;

  case X86_INS_INCSSPD:
  case X86_INS_INCSSPQ:
    S.emitIntrinsic(Intrinsic::CetIncSsp);
    break;
  case X86_INS_RDSSPD:
  case X86_INS_RDSSPQ:
    S.emitIntrinsic(Intrinsic::CetRdSsp);
    if (X86.op_count >= 1) {
      NdVar Dst = L.operandWrite(X86.operands[0]);
      S.emit(NdOp::COPY, Dst, {S.makeTemp(Dst.Size)});
    }
    break;
  case X86_INS_SAVEPREVSSP:
    S.emitIntrinsic(Intrinsic::CetSaveprevssp);
    break;
  case X86_INS_RSTORSSP:
    S.emitIntrinsic(Intrinsic::CetRstorssp);
    break;
  case X86_INS_WRSSD:
  case X86_INS_WRSSQ:
    S.emitIntrinsic(Intrinsic::CetWrss);
    break;
  case X86_INS_WRUSSD:
  case X86_INS_WRUSSQ:
    S.emitIntrinsic(Intrinsic::CetWruss);
    break;
  case X86_INS_SETSSBSY:
    S.emitIntrinsic(Intrinsic::CetSetssbsy);
    break;
  case X86_INS_CLRSSBSY:
    S.emitIntrinsic(Intrinsic::CetClrssbsy);
    break;

  // MOVDIRI / MOVDIR64B — direct store.
  case X86_INS_MOVDIRI:
  case X86_INS_MOVDIR64B: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Src);
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // PT — Processor Trace.
  case X86_INS_PTWRITE:
    S.emitIntrinsic(Intrinsic::Ptwrite);
    break;

  // ========================================================================
  // VIA PadLock — hardware crypto acceleration (XCRYPT*, XSHA*, MONTMUL,
  // XSTORE).
  // ========================================================================
  case X86_INS_XCRYPTECB:
  case X86_INS_XCRYPTCBC:
  case X86_INS_XCRYPTCTR:
  case X86_INS_XCRYPTCFB:
  case X86_INS_XCRYPTOFB:
  case X86_INS_XSHA1:
  case X86_INS_XSHA256:
  case X86_INS_MONTMUL:
  case X86_INS_XSTORE: {
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_XCRYPTECB:
      Id = Intrinsic::Xcryptecb;
      break;
    case X86_INS_XCRYPTCBC:
      Id = Intrinsic::Xcryptcbc;
      break;
    case X86_INS_XCRYPTCTR:
      Id = Intrinsic::Xcryptctr;
      break;
    case X86_INS_XCRYPTCFB:
      Id = Intrinsic::Xcryptcfb;
      break;
    case X86_INS_XCRYPTOFB:
      Id = Intrinsic::Xcryptofb;
      break;
    case X86_INS_XSHA1:
      Id = Intrinsic::Xsha1;
      break;
    case X86_INS_XSHA256:
      Id = Intrinsic::Xsha256;
      break;
    case X86_INS_MONTMUL:
      Id = Intrinsic::Montmul;
      break;
    case X86_INS_XSTORE:
      Id = Intrinsic::Xstore;
      break;
    default:
      Id = Intrinsic::Xcryptecb;
      break;
    }
    S.emitIntrinsic(Id);
    for (uint64_t RO : {x86reg::RSI, x86reg::RDI, x86reg::RCX}) {
      S.emit(NdOp::COPY, NdVar::reg(RO, 8), {S.makeTemp(8)});
    }
    break;
  }

  // ========================================================================
  // xsave 64-bit variants.
  // ========================================================================
  case X86_INS_XSAVE64:
  case X86_INS_XRSTOR64:
  case X86_INS_XSAVES64:
  case X86_INS_XRSTORS64:
  case X86_INS_XSAVEC64:
  case X86_INS_XSAVEOPT64: {
    Intrinsic Id = Intrinsic::Xsave64;
    switch (InsnId) {
    case X86_INS_XRSTOR64:
      Id = Intrinsic::Xrstor64;
      break;
    case X86_INS_XSAVES64:
      Id = Intrinsic::Xsaves64;
      break;
    case X86_INS_XRSTORS64:
      Id = Intrinsic::Xrstors64;
      break;
    case X86_INS_XSAVEC64:
      Id = Intrinsic::Xsavec64;
      break;
    case X86_INS_XSAVEOPT64:
      Id = Intrinsic::Xsaveopt64;
      break;
    default:
      break;
    }
    if (X86.op_count < 1 ||
        !S.emitMemoryIntrinsic(
            Id, X86.operands[0],
            {NdVar::reg(x86reg::RAX, 4), NdVar::reg(x86reg::RDX, 4)}))
      return false;
    break;
  }

  // x87 misc — FNINIT/FNCLEX already handled above in x87 block.

  // ========================================================================
  // GFNI — Galois Field instructions.
  // ========================================================================
  case X86_INS_GF2P8AFFINEINVQB:
  case X86_INS_GF2P8AFFINEQB:
  case X86_INS_GF2P8MULB:
  case X86_INS_VGF2P8AFFINEINVQB:
  case X86_INS_VGF2P8AFFINEQB:
  case X86_INS_VGF2P8MULB: {
    const bool IsVex = InsnId == X86_INS_VGF2P8AFFINEINVQB ||
                       InsnId == X86_INS_VGF2P8AFFINEQB ||
                       InsnId == X86_INS_VGF2P8MULB;
    const bool IsMul =
        InsnId == X86_INS_GF2P8MULB || InsnId == X86_INS_VGF2P8MULB;
    const unsigned RequiredOps = IsMul ? (IsVex ? 3 : 2) : (IsVex ? 4 : 3);
    if (X86.op_count < RequiredOps)
      break;

    NdVar Dst = L.operandWrite(X86.operands[0]);
    const unsigned SrcIdx = IsVex ? 1 : 0;
    NdVar Src1 = L.operandRead(S, X86.operands[SrcIdx]);
    NdVar Src2 = L.operandRead(S, X86.operands[SrcIdx + 1]);

    if (IsMul) {
      S.emitIntrinsic(Intrinsic::Gf2p8MulB, Dst, {Src1, Src2});
      break;
    }

    if (X86.operands[X86.op_count - 1].type != X86_OP_IMM)
      break;
    uint8_t Imm = static_cast<uint8_t>(X86.operands[X86.op_count - 1].imm);
    Intrinsic IC = (InsnId == X86_INS_GF2P8AFFINEINVQB ||
                    InsnId == X86_INS_VGF2P8AFFINEINVQB)
                       ? Intrinsic::Gf2p8AffineInvQb
                       : Intrinsic::Gf2p8AffineQb;
    S.emitIntrinsic(IC, Dst, {Src1, Src2, NdVar::cst(Imm, 1)});
    break;
  }

  // ========================================================================
  // LWP — Lightweight Profiling (AMD).
  // ========================================================================
  case X86_INS_LLWPCB:
    S.emitIntrinsic(Intrinsic::Llwpcb);
    break;
  case X86_INS_SLWPCB:
    S.emitIntrinsic(Intrinsic::Slwpcb);
    break;
  case X86_INS_LWPINS:
    S.emitIntrinsic(Intrinsic::Lwpins);
    break;
  case X86_INS_LWPVAL:
    S.emitIntrinsic(Intrinsic::Lwpval);
    break;

  // CLAC / STAC — supervisor mode access control.
  case X86_INS_CLAC:
    S.emitIntrinsic(Intrinsic::Clac);
    break;
  case X86_INS_STAC:
    S.emitIntrinsic(Intrinsic::Stac);
    break;

  // CLZERO — zero cache line (AMD Zen).
  case X86_INS_CLZERO:
    S.emitIntrinsic(Intrinsic::Clzero);
    break;

  // XTEST already handled in TSX block.

  // VPCLMUL variants that are not the base VPCLMULQDQ are rare but exist
  // in some Capstone builds. Already covered via VPCLMULQDQ above.

  // ========================================================================
  // Prefixes — not standalone instructions; emit NOP.
  // ========================================================================
  case X86_INS_DATA16:
  case X86_INS_LOCK:
  case X86_INS_REP:
  case X86_INS_REPNE:
  case X86_INS_REX64:
    S.emit(NdOp::NOP, {}, {});
    break;

  // Legacy FPU handled in x87 block above.
  case X86_INS_FCMOVNP:
    S.emitIntrinsic(Intrinsic::X87Op);
    break;

  // ========================================================================
  // SSE/MMX data moves — treat as COPY to preserve dataflow.
  // ========================================================================
  case X86_INS_MOVDQ2Q:
  case X86_INS_MOVQ2DQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case X86_INS_PSHUFW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Pshufw, Dst, {Src});
    break;
  }

  // ========================================================================
  // SSE4a (AMD): EXTRQ / INSERTQ → intrinsic (hard to model per-bit).
  // ========================================================================
  case X86_INS_EXTRQ:
    S.emitIntrinsic(Intrinsic::Extrq);
    break;
  case X86_INS_INSERTQ:
    S.emitIntrinsic(Intrinsic::Insertq);
    break;

  // ========================================================================
  // 3DNow! — rare; COPY-based dataflow preservation.
  // ========================================================================
  case X86_INS_PAVGUSB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // System instructions.
  // ========================================================================
  case X86_INS_CLDEMOTE:
    S.emitIntrinsic(Intrinsic::Cldemote);
    break;
  case X86_INS_INVLPGA:
    S.emitIntrinsic(Intrinsic::Invlpga);
    break;
  case X86_INS_PCONFIG:
    S.emitIntrinsic(Intrinsic::Pconfig);
    break;
  case X86_INS_RDPKRU:
    S.emitIntrinsic(Intrinsic::Rdpkru);
    S.emit(NdOp::COPY, NdVar::reg(x86reg::RAX, 4), {S.makeTemp(4)});
    break;
  case X86_INS_WRPKRU:
    S.emitIntrinsic(Intrinsic::Wrpkru);
    break;
  case X86_INS_SYSEXITQ:
    S.emitIntrinsic(Intrinsic::Sysexitq);
    break;
  case X86_INS_SYSRETQ:
    S.emitIntrinsic(Intrinsic::Sysretq);
    break;
  case X86_INS_WBNOINVD:
    S.emitIntrinsic(Intrinsic::Wbnoinvd);
    break;

  // ========================================================================
  // AVX/AVX-512 control: VLDMXCSR / VSTMXCSR.
  // ========================================================================
  case X86_INS_VLDMXCSR:
  case X86_INS_VSTMXCSR:
    if (X86.op_count < 1 ||
        !S.emitMemoryIntrinsic(InsnId == X86_INS_VLDMXCSR
                                   ? Intrinsic::Ldmxcsr
                                   : Intrinsic::Stmxcsr,
                               X86.operands[0]))
      return false;
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
