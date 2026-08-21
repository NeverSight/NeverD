//===- X86LiftSIMDAVX.cpp - x86/x64 AVX/AVX-512/FMA instruction lifter --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches FMA, AVX-512 opmask and AVX-512 packed integer/float
/// instructions to the per-family handlers in X86LiftSIMDAVX*.cpp, and lifts
/// the VSIB gather/scatter forms here because they reach the private
/// liftVectorGather (defined below).
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool X86Lifter::liftSIMDAVX(LiftState &S, const cs_insn *Insn,
                            const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // Gather — native per-lane conditional load (see liftVectorGather).  The FP
  // forms are bit-identical to the integer VPGATHER forms (the value is only
  // FP-typed), so they share the same lowering.
  // ========================================================================
  case X86_INS_VGATHERDPD:
  case X86_INS_VGATHERDPS:
  case X86_INS_VGATHERQPD:
  case X86_INS_VGATHERQPS: {
    if (!liftVectorGather(S, X86, InsnId) && X86.op_count >= 1) {
      NdVar Dst = operandWrite(X86.operands[0]);
      S.emitIntrinsic(Intrinsic::VGather, Dst);
    }
    break;
  }
  // AVX-512 gather prefetch hints carry no architectural data movement; keep an
  // opaque side effect (clang never emits these for the supported targets).
  case X86_INS_VGATHERPF0DPD:
  case X86_INS_VGATHERPF0DPS:
  case X86_INS_VGATHERPF0QPD:
  case X86_INS_VGATHERPF0QPS:
  case X86_INS_VGATHERPF1DPD:
  case X86_INS_VGATHERPF1DPS:
  case X86_INS_VGATHERPF1QPD:
  case X86_INS_VGATHERPF1QPS: {
    if (X86.op_count >= 1) {
      NdVar Dst = operandWrite(X86.operands[0]);
      S.emitIntrinsic(Intrinsic::VGather, Dst);
    }
    break;
  }
  case X86_INS_VSCATTERDPD:
  case X86_INS_VSCATTERDPS:
  case X86_INS_VSCATTERQPD:
  case X86_INS_VSCATTERQPS:
  case X86_INS_VSCATTERPF0DPD:
  case X86_INS_VSCATTERPF0DPS:
  case X86_INS_VSCATTERPF0QPD:
  case X86_INS_VSCATTERPF0QPS:
  case X86_INS_VSCATTERPF1DPD:
  case X86_INS_VSCATTERPF1DPS:
  case X86_INS_VSCATTERPF1QPD:
  case X86_INS_VSCATTERPF1QPS: {
    if (X86.op_count >= 1) {
      NdVar Dst = operandWrite(X86.operands[0]);
      S.emitIntrinsic(Intrinsic::VScatter, Dst);
    }
    break;
  }

  // VPGATHER{DD,DQ,QD,QQ} — gather load from memory via index vector.
  case X86_INS_VPGATHERDD:
  case X86_INS_VPGATHERDQ:
  case X86_INS_VPGATHERQD:
  case X86_INS_VPGATHERQQ: {
    if (!liftVectorGather(S, X86, InsnId) && X86.op_count >= 2) {
      NdVar Dst = operandWrite(X86.operands[0]);
      NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
      S.emitIntrinsic(Intrinsic::Vpgather, Dst, {Src});
    }
    break;
  }

  default:
    return liftSIMDAVXFMA(*this, S, Insn, X86) ||
           liftSIMDAVXSSE(*this, S, Insn, X86) ||
           liftSIMDAVXMask(*this, S, Insn, X86) ||
           liftSIMDAVXInt(*this, S, Insn, X86) ||
           liftSIMDAVXConvert(*this, S, Insn, X86) ||
           liftSIMDAVXFloat(*this, S, Insn, X86);
  }
  return true;
}

bool X86Lifter::liftVectorGather(LiftState &S, const cs_x86 &X86,
                                 unsigned InsnId) {
  // Index element size (D=4 / Q=8) and value element size, derived from the
  // mnemonic: VxGATHER<idx><val>.  The FP forms share the integer sizes.
  uint16_t IdxSz, ValSz;
  switch (InsnId) {
  case X86_INS_VPGATHERDD:
  case X86_INS_VGATHERDPS:
    IdxSz = 4;
    ValSz = 4;
    break;
  case X86_INS_VPGATHERDQ:
  case X86_INS_VGATHERDPD:
    IdxSz = 4;
    ValSz = 8;
    break;
  case X86_INS_VPGATHERQD:
  case X86_INS_VGATHERQPS:
    IdxSz = 8;
    ValSz = 4;
    break;
  case X86_INS_VPGATHERQQ:
  case X86_INS_VGATHERQPD:
    IdxSz = 8;
    ValSz = 8;
    break;
  default:
    return false;
  }

  // Operands: dst = operands[0]; the VSIB memory operand carries the base GPR,
  // the vector index register, scale, and disp; the remaining register operand
  // is the mask (read for sign bits, zeroed afterward).
  const cs_x86_op *Mem = nullptr;
  int MaskIdx = -1;
  for (int I = 0; I < X86.op_count; ++I) {
    if (X86.operands[I].type == X86_OP_MEM)
      Mem = &X86.operands[I];
    else if (I != 0 && X86.operands[I].type == X86_OP_REG)
      MaskIdx = I;
  }
  if (!Mem || MaskIdx < 0 || X86.operands[0].type != X86_OP_REG ||
      Mem->mem.index == X86_REG_INVALID)
    return false;

  NdVar Dst = operandWrite(X86.operands[0]);
  uint16_t DstBytes = Dst.Size;
  RegInfo IdxRI = mapCapstoneReg(static_cast<x86_reg>(Mem->mem.index));
  NdVar IdxVec = NdVar::reg(IdxRI.Offset, IdxRI.Size);
  NdVar MaskVec = operandRead(S, X86.operands[MaskIdx]);
  NdVar DstOld = operandRead(S, X86.operands[0]);

  uint16_t IdxLanes = IdxRI.Size / IdxSz;
  uint16_t ValLanes = DstBytes / ValSz;
  uint16_t NumElems = IdxLanes < ValLanes ? IdxLanes : ValLanes;
  if (NumElems == 0 || ValLanes > 8)
    return false;

  // baseAddr = base + disp (index is per-lane, added below).
  NdVar BaseAddr = S.makeTemp(8);
  bool First = true;
  auto Acc = [&](NdVar V) {
    if (First) {
      S.emit(NdOp::COPY, BaseAddr, {V});
      First = false;
    } else {
      S.emit(NdOp::INT_ADD, BaseAddr, {BaseAddr, V});
    }
  };
  if (Mem->mem.base != X86_REG_INVALID) {
    RegInfo BaseRI = mapCapstoneReg(static_cast<x86_reg>(Mem->mem.base));
    Acc(NdVar::reg(BaseRI.Offset, 8));
  }
  if (S.RelocatedDisplacement)
    Acc(*S.RelocatedDisplacement);
  else if (Mem->mem.disp != 0)
    Acc(NdVar::scalar(static_cast<uint64_t>(Mem->mem.disp), 8));
  if (First)
    Acc(NdVar::scalar(0, 8));
  unsigned Scale = Mem->mem.scale ? Mem->mem.scale : 1;

  NdVar Lanes[8];
  for (uint16_t I = 0; I < ValLanes; ++I) {
    if (I >= NumElems) {
      Lanes[I] =
          NdVar::scalar(0, ValSz); // SDM: lanes past the gather count = 0
      continue;
    }
    // addr = baseAddr + sext(index[I]) * scale.
    NdVar IdxElem = S.makeTemp(IdxSz);
    S.emit(NdOp::SUBBYTES, IdxElem,
           {IdxVec, NdVar::scalar(static_cast<uint64_t>(I) * IdxSz, 4)});
    NdVar Idx64 = IdxElem;
    if (IdxSz < 8) {
      Idx64 = S.makeTemp(8);
      S.emit(NdOp::INT_SEXT, Idx64, {IdxElem});
    }
    NdVar Off = Idx64;
    if (Scale > 1) {
      Off = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Off, {Idx64, NdVar::scalar(Scale, 8)});
    }
    NdVar Addr = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, Addr, {BaseAddr, Off});
    NdVar Loaded = S.makeTemp(ValSz);
    S.emit(NdOp::LOAD, Loaded, {Addr});
    // The mask element's sign bit gates the load; a clear sign keeps the source
    // lane (operands[0] is also the merge source for masked-off elements).
    NdVar MaskElem = S.makeTemp(ValSz);
    S.emit(NdOp::SUBBYTES, MaskElem,
           {MaskVec, NdVar::scalar(static_cast<uint64_t>(I) * ValSz, 4)});
    NdVar SignSet = S.makeTemp(1);
    S.emit(NdOp::INT_SLESS, SignSet, {MaskElem, NdVar::scalar(0, ValSz)});
    NdVar OldLane = S.makeTemp(ValSz);
    S.emit(NdOp::SUBBYTES, OldLane,
           {DstOld, NdVar::scalar(static_cast<uint64_t>(I) * ValSz, 4)});
    NdVar Res = S.makeTemp(ValSz);
    S.emit(NdOp::SELECT, Res, {SignSet, Loaded, OldLane});
    Lanes[I] = Res;
  }

  // Power-of-two CONCAT tree merges the lanes low->high into the destination.
  uint16_t Count = ValLanes, Sz = ValSz;
  while (Count > 1) {
    uint16_t Half = Count / 2;
    uint16_t NewSz = static_cast<uint16_t>(Sz * 2);
    for (uint16_t K = 0; K < Half; ++K) {
      NdVar Out = (Half == 1) ? Dst : S.makeTemp(NewSz);
      S.emit(NdOp::CONCAT, Out, {Lanes[2 * K + 1], Lanes[2 * K]});
      Lanes[K] = Out;
    }
    Count = Half;
    Sz = NewSz;
  }

  // The gather clears the entire mask register on completion.
  NdVar MaskOut = operandWrite(X86.operands[MaskIdx]);
  S.emit(NdOp::COPY, MaskOut, {NdVar::scalar(0, MaskVec.Size)});
  return true;
}

} // namespace neverd
