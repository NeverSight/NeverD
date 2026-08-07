//===- ARMLiftSIMDNEON.cpp - ARM32 NEON instruction lifter ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NEON (Advanced SIMD) instruction handlers for ARM32: vector load/store,
/// arithmetic, logic, shifts, conversions, table lookups, zip/unzip, and
/// transpose.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

namespace {
struct NeonLaneInfo {
  unsigned LaneSz = 0;
  bool IsSigned = false;
  bool IsFloat = false;
};

NeonLaneInfo getNeonLaneInfo(arm_vectordata_type VD) {
  NeonLaneInfo I;
  switch (VD) {
  case ARM_VECTORDATA_S8:
    I.LaneSz = 1;
    I.IsSigned = true;
    break;
  case ARM_VECTORDATA_U8:
  case ARM_VECTORDATA_I8:
    I.LaneSz = 1;
    break;
  case ARM_VECTORDATA_S16:
    I.LaneSz = 2;
    I.IsSigned = true;
    break;
  case ARM_VECTORDATA_U16:
  case ARM_VECTORDATA_I16:
    I.LaneSz = 2;
    break;
  case ARM_VECTORDATA_S32:
    I.LaneSz = 4;
    I.IsSigned = true;
    break;
  case ARM_VECTORDATA_U32:
  case ARM_VECTORDATA_I32:
    I.LaneSz = 4;
    break;
  case ARM_VECTORDATA_S64:
    I.LaneSz = 8;
    I.IsSigned = true;
    break;
  case ARM_VECTORDATA_U64:
  case ARM_VECTORDATA_I64:
    I.LaneSz = 8;
    break;
  case ARM_VECTORDATA_F32:
    I.LaneSz = 4;
    I.IsFloat = true;
    break;
  case ARM_VECTORDATA_F64:
    I.LaneSz = 8;
    I.IsFloat = true;
    break;
  default:
    break;
  }
  return I;
}

// Capstone leaves `vector_data` == ARM_VECTORDATA_INVALID for several NEON
// instructions (notably VTST), so the only reliable element-width source is the
// mnemonic suffix (".8"/".16"/".i16"/".s32"/".u8"/".f32").  Parse it.
NeonLaneInfo getNeonLaneInfoFromMnemonic(const char *Mnem) {
  NeonLaneInfo I;
  if (!Mnem)
    return I;
  llvm::StringRef M(Mnem);
  size_t Dot = M.rfind('.');
  if (Dot == llvm::StringRef::npos)
    return I;
  llvm::StringRef Suf = M.substr(Dot + 1);
  if (!Suf.empty()) {
    char C = Suf[0];
    if (C == 's')
      I.IsSigned = true;
    else if (C == 'f')
      I.IsFloat = true;
    if (C == 'i' || C == 's' || C == 'u' || C == 'f' || C == 'p')
      Suf = Suf.drop_front();
  }
  unsigned Bits = 0;
  if (!Suf.getAsInteger(10, Bits) && Bits >= 8)
    I.LaneSz = Bits / 8;
  return I;
}

// Element info from vector_data, falling back to the mnemonic suffix when
// capstone did not populate vector_data.
NeonLaneInfo getNeonLaneInfo(arm_vectordata_type VD, const char *Mnem) {
  NeonLaneInfo I = getNeonLaneInfo(VD);
  if (I.LaneSz == 0)
    I = getNeonLaneInfoFromMnemonic(Mnem);
  return I;
}
} // namespace

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

namespace {
// NEON vector compares (VCEQ/VCGT/VTST/...) set *each lane* to all-ones when
// the per-lane predicate holds and all-zeros otherwise.  Emitting a single
// full-width compare (the old behaviour) collapses the whole register to a
// 0/1 boolean, which is only accidentally correct when callers read lane 0 in
// scenarios where the wide result happens to coincide.  This helper performs
// the comparison lane-by-lane and writes an explicit all-ones/all-zeros mask.
void emitPerLaneCmpMask(ARMLifter::LiftState &S, NdVar Dst, NdVar A,
                        NdVar B, NdOp CmpOp, unsigned LaneSz) {
  if (LaneSz == 0 || Dst.Size <= LaneSz) {
    // Unknown lane size or scalar width: fall back to a full-width compare so
    // we never regress relative to the previous behaviour.
    S.emit(CmpOp, Dst, {A, B});
    return;
  }
  unsigned NLanes = Dst.Size / LaneSz;
  uint64_t AllOnes = (LaneSz >= 8) ? ~0ULL : ((1ULL << (LaneSz * 8)) - 1);
  NdVar Acc = S.makeTemp(0);
  for (unsigned I = 0; I < NLanes; ++I) {
    NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
    S.emit(NdOp::SUBBYTES, La,
           {A, NdVar::cst(static_cast<uint64_t>(I) * LaneSz, 4)});
    S.emit(NdOp::SUBBYTES, Lb,
           {B, NdVar::cst(static_cast<uint64_t>(I) * LaneSz, 4)});
    NdVar Cmp = S.makeTemp(1);
    S.emit(CmpOp, Cmp, {La, Lb});
    NdVar Mask = S.makeTemp(LaneSz);
    S.emit(NdOp::SELECT, Mask,
           {Cmp, NdVar::cst(AllOnes, LaneSz), NdVar::cst(0, LaneSz)});
    if (I == 0)
      Acc = Mask;
    else {
      NdVar Next = S.makeTemp(Acc.Size + LaneSz);
      S.emit(NdOp::CONCAT, Next, {Mask, Acc});
      Acc = Next;
    }
  }
  S.emit(NdOp::COPY, Dst, {Acc});
}
} // namespace

bool ARMLifter::liftSIMDNEON(LiftState &S, const cs_insn *Insn,
                             const cs_arm &ARM) {
  switch (Insn->id) {

  // ========================================================================
  // NEON (Advanced SIMD) — large batch
  // ========================================================================
  // NEON structure load/store
  case ARM_INS_VLD1:
  case ARM_INS_VLD2:
  case ARM_INS_VLD3:
  case ARM_INS_VLD4:
  case ARM_INS_VLD20:
  case ARM_INS_VLD21:
  case ARM_INS_VLD40:
  case ARM_INS_VLD41:
  case ARM_INS_VLD42:
  case ARM_INS_VLD43:
  case ARM_INS_VST1:
  case ARM_INS_VST2:
  case ARM_INS_VST3:
  case ARM_INS_VST4:
  case ARM_INS_VST20:
  case ARM_INS_VST21:
  case ARM_INS_VST40:
  case ARM_INS_VST41:
  case ARM_INS_VST42:
  case ARM_INS_VST43: {
    // `vld1/vst1 {dA, dB, ...}, [rN]{!}` move a *list* of register-sized
    // chunks to/from consecutive memory.  The register list precedes the
    // single memory operand; an optional post-index register follows it.
    // Previously only the first register was handled, so a 16-byte
    // `{d18,d19}` transfer moved only 8 bytes and left half of an array
    // uninitialized.  Walk the whole list and use the effective address
    // (not a LOAD of memory).
    bool IsLoad = (Insn->id == ARM_INS_VLD1 || Insn->id == ARM_INS_VLD2 ||
                   Insn->id == ARM_INS_VLD3 || Insn->id == ARM_INS_VLD4 ||
                   Insn->id == ARM_INS_VLD20 || Insn->id == ARM_INS_VLD21 ||
                   Insn->id == ARM_INS_VLD40 || Insn->id == ARM_INS_VLD41 ||
                   Insn->id == ARM_INS_VLD42 || Insn->id == ARM_INS_VLD43);
    int MemIdx = -1;
    for (int I = 0; I < ARM.op_count; ++I)
      if (ARM.operands[I].type == ARM_OP_MEM) {
        MemIdx = I;
        break;
      }
    if (MemIdx < 1)
      break;
    // NEON list addressing is always `[Rn]{:align}`: the access EA is just the
    // base register.  Any index/disp present is a POST-index writeback amount
    // (`[Rn], Rm` / `[Rn], #imm`), NOT part of the address — feeding the whole
    // operand to operandEffAddr (which adds mem.index) made `[r0], r1` load
    // from r0+r1 and shifted every overlapping window by one element.
    auto &MemBR = ARM.operands[MemIdx].mem;
    NdVar Base;
    if (MemBR.base != ARM_REG_INVALID) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(MemBR.base));
      Base = S.makeTemp(4);
      S.emit(NdOp::COPY, Base, {NdVar::reg(RI.Offset, 4)});
    } else {
      Base = operandEffAddr(S, ARM.operands[MemIdx]);
    }
    // Single-element element size for the indexed `{dN[idx]}` form.
    unsigned LaneElemSz =
        getNeonLaneInfo(ARM.vector_data, Insn->mnemonic).LaneSz;
    uint32_t Offset = 0;

    // VLD2/VLD3/VLD4 (and VST*) DE-INTERLEAVE consecutive memory elements
    // across the register list: element i goes to register (i % StructN), lane
    // (i / StructN).  Previously these were treated like VLD1 (contiguous),
    // so `vld2.32 {d18,d19,d20,d21}` loaded q9={v0,v1,v2,v3} instead of the
    // even lanes {v0,v2,v4,v6} — corrupting stride-2 pooling / de-interleave.
    // (ARM32 analogue of the AArch64 LD2/3/4 fix.)
    unsigned StructN =
        (Insn->id == ARM_INS_VLD2 || Insn->id == ARM_INS_VST2)   ? 2
        : (Insn->id == ARM_INS_VLD3 || Insn->id == ARM_INS_VST3) ? 3
        : (Insn->id == ARM_INS_VLD4 || Insn->id == ARM_INS_VST4) ? 4
                                                                 : 1;
    bool Deinterleaved = false;
    if (StructN >= 2 && LaneElemSz > 0 && (8u % LaneElemSz) == 0) {
      std::vector<int> RegOpIdx;
      for (int I = 0; I < MemIdx; ++I)
        if (ARM.operands[I].type == ARM_OP_REG)
          RegOpIdx.push_back(I);
      unsigned NRegsInList = static_cast<unsigned>(RegOpIdx.size());
      if (NRegsInList >= StructN && NRegsInList % StructN == 0) {
        unsigned ElemSz = LaneElemSz;
        unsigned RegsPerTarget = NRegsInList / StructN;
        unsigned ElemsPerD = 8u / ElemSz; // lanes per D register
        auto addrAt = [&](unsigned ElemIdx) -> NdVar {
          if (ElemIdx == 0)
            return Base;
          NdVar A = S.makeTemp(4);
          S.emit(
              NdOp::INT_ADD, A,
              {Base, NdVar::cst(static_cast<uint64_t>(ElemIdx) * ElemSz, 4)});
          return A;
        };
        for (unsigned d = 0; d < NRegsInList; ++d) {
          unsigned g = d / RegsPerTarget; // target stream (even/odd/...)
          unsigned p = d % RegsPerTarget; // D position inside the stream
          auto &RegOp = ARM.operands[RegOpIdx[d]];
          if (IsLoad) {
            NdVar Acc = S.makeTemp(0);
            bool First = true;
            for (unsigned j = 0; j < ElemsPerD; ++j) {
              unsigned L = p * ElemsPerD + j; // lane within the stream
              unsigned MemElem = L * StructN + g;
              NdVar El = S.makeTemp(ElemSz);
              S.emit(NdOp::LOAD, El, {addrAt(MemElem)});
              if (First) {
                Acc = El;
                First = false;
              } else {
                NdVar Next = S.makeTemp(Acc.Size + ElemSz);
                S.emit(NdOp::CONCAT, Next, {El, Acc});
                Acc = Next;
              }
            }
            S.emit(NdOp::COPY, operandWrite(RegOp), {Acc});
          } else {
            NdVar Cur = operandRead(S, RegOp);
            for (unsigned j = 0; j < ElemsPerD; ++j) {
              unsigned L = p * ElemsPerD + j;
              unsigned MemElem = L * StructN + g;
              NdVar El = S.makeTemp(ElemSz);
              S.emit(NdOp::SUBBYTES, El, {Cur, NdVar::cst(j * ElemSz, 4)});
              S.emit(NdOp::STORE, {}, {addrAt(MemElem), El});
            }
          }
        }
        Offset = NRegsInList * 8u;
        Deinterleaved = true;
      }
    }

    // VLD1 (single element to ALL lanes): `vld1.N {dN[], dM[]}, [rA]{!}` loads
    // ONE element and replicates it into every lane of each listed register.
    // capstone exposes no lane index for the `[]` (all-lanes) form
    // (vector_index/neon_lane == -1, identical to a whole-register transfer),
    // so the only signal is the `[]` marker in the operand text; without it the
    // list read as a contiguous multi-register load (wrong element per lane and
    // a wrong list-sized writeback stride — broke clang's matmul scalar
    // splats).
    bool AllLanes = !Deinterleaved && IsLoad && LaneElemSz > 0 &&
                    llvm::StringRef(Insn->op_str).contains("[]");
    if (AllLanes) {
      NdVar El = S.makeTemp(LaneElemSz);
      S.emit(NdOp::LOAD, El, {Base});
      for (int I = 0; I < MemIdx; ++I) {
        auto &RegOp = ARM.operands[I];
        if (RegOp.type != ARM_OP_REG)
          continue;
        NdVar Reg = operandWrite(RegOp);
        unsigned NLanes = Reg.Size / LaneElemSz;
        NdVar Acc = El;
        for (unsigned L = 1; L < NLanes; ++L) {
          NdVar Next = S.makeTemp(Acc.Size + LaneElemSz);
          S.emit(NdOp::CONCAT, Next, {El, Acc});
          Acc = Next;
        }
        S.emit(NdOp::COPY, Reg, {Acc});
      }
      // Only ONE element is transferred, so a `!`/register writeback advances
      // by a single element, not by the register-list byte count.
      Offset = LaneElemSz;
      Deinterleaved = true;
    }

    for (int I = 0; !Deinterleaved && I < MemIdx; ++I) {
      auto &RegOp = ARM.operands[I];
      if (RegOp.type != ARM_OP_REG)
        continue;
      NdVar Addr = Base;
      if (Offset > 0) {
        Addr = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Addr, {Base, NdVar::cst(Offset, 4)});
      }
      int Lane = RegOp.neon_lane >= 0 ? RegOp.neon_lane : RegOp.vector_index;
      NdVar Reg = operandWrite(RegOp);
      // `vld1/vst1 {dN[idx]}, [rM]` transfers ONE element, not the whole
      // register.  Previously the indexed form fell into the whole-register
      // path: a `.32` lane load read 8 bytes into dN (clobbering the other
      // lane) instead of 4 bytes into lane idx.
      if (Lane >= 0 && LaneElemSz > 0 && LaneElemSz < Reg.Size) {
        unsigned ByteOff = static_cast<unsigned>(Lane) * LaneElemSz;
        if (ByteOff + LaneElemSz > Reg.Size)
          ByteOff = 0;
        if (IsLoad) {
          NdVar Cur = operandRead(S, RegOp);
          NdVar Val = S.makeTemp(LaneElemSz);
          S.emit(NdOp::LOAD, Val, {Addr});
          // Read-modify-write: splice Val into [ByteOff, ByteOff+LaneElemSz).
          if (ByteOff == 0) {
            NdVar Hi = S.makeTemp(Reg.Size - LaneElemSz);
            S.emit(NdOp::SUBBYTES, Hi, {Cur, NdVar::cst(LaneElemSz, 4)});
            S.emit(NdOp::CONCAT, Reg, {Hi, Val});
          } else if (ByteOff + LaneElemSz == Reg.Size) {
            NdVar Lo = S.makeTemp(ByteOff);
            S.emit(NdOp::SUBBYTES, Lo, {Cur, NdVar::cst(0, 4)});
            S.emit(NdOp::CONCAT, Reg, {Val, Lo});
          } else {
            NdVar Lo = S.makeTemp(ByteOff);
            S.emit(NdOp::SUBBYTES, Lo, {Cur, NdVar::cst(0, 4)});
            NdVar Hi = S.makeTemp(Reg.Size - ByteOff - LaneElemSz);
            S.emit(NdOp::SUBBYTES, Hi,
                   {Cur, NdVar::cst(ByteOff + LaneElemSz, 4)});
            NdVar Mid = S.makeTemp(ByteOff + LaneElemSz);
            S.emit(NdOp::CONCAT, Mid, {Val, Lo});
            S.emit(NdOp::CONCAT, Reg, {Hi, Mid});
          }
        } else {
          NdVar Cur = operandRead(S, RegOp);
          NdVar Val = S.makeTemp(LaneElemSz);
          S.emit(NdOp::SUBBYTES, Val, {Cur, NdVar::cst(ByteOff, 4)});
          S.emit(NdOp::STORE, {}, {Addr, Val});
        }
        Offset += LaneElemSz;
        continue;
      }
      if (IsLoad) {
        S.emit(NdOp::LOAD, Reg, {Addr});
        Offset += Reg.Size;
      } else {
        NdVar Src = operandRead(S, RegOp);
        S.emit(NdOp::STORE, {}, {Addr, Src});
        Offset += Src.Size;
      }
    }
    // Address writeback: `[rN]!` adds the transferred byte count; `[rN], rM`
    // adds register rM.  The post-index register rM may be encoded either as
    // `mem.index` (current capstone) or as a trailing REG operand (older
    // capstone); handle both.  Only the `[rN]!` (no register) form increments
    // by the transferred byte count.
    if (MemBR.base != ARM_REG_INVALID) {
      auto BI = mapCapstoneReg(static_cast<arm_reg>(MemBR.base));
      if (BI.Size != 0) {
        NdVar BaseReg = NdVar::reg(BI.Offset, 4);
        arm_reg PostReg = ARM_REG_INVALID;
        if (ARM.post_index && MemBR.index != ARM_REG_INVALID)
          PostReg = static_cast<arm_reg>(MemBR.index);
        else if (MemIdx + 1 < ARM.op_count &&
                 ARM.operands[MemIdx + 1].type == ARM_OP_REG)
          PostReg = static_cast<arm_reg>(ARM.operands[MemIdx + 1].reg);
        if (PostReg != ARM_REG_INVALID) {
          auto PI = mapCapstoneReg(PostReg);
          if (PI.Size != 0)
            S.emit(NdOp::INT_ADD, BaseReg,
                   {BaseReg, NdVar::reg(PI.Offset, 4)});
        } else if (ARM.post_index && MemBR.disp != 0) {
          S.emit(NdOp::INT_ADD, BaseReg,
                 {BaseReg, NdVar::cst(static_cast<uint64_t>(
                                            static_cast<uint32_t>(MemBR.disp)),
                                        4)});
        } else if (Insn->detail->writeback && Offset > 0) {
          S.emit(NdOp::INT_ADD, BaseReg, {BaseReg, NdVar::cst(Offset, 4)});
        }
      }
    }
    break;
  }

  // NEON data processing — binary ops
  case ARM_INS_VAND:
  case ARM_INS_VORR:
  case ARM_INS_VEOR: {
    // Immediate form: `vorr.iN dD, #imm` (dD |= imm) and `vand.iN dD, #imm`
    // (dD &= imm) carry only two operands (dst + immediate).  The old code
    // required 3 operands and silently dropped them, so e.g. clang's
    // `(x & 0x7e) | 1` lost its `| 1` — the divisor became even/zero and the
    // recompiled float divide produced inf/NaN (VectorAlgo8 arm32 fdiv).  Like
    // VBIC's immediate form (and AArch64 #220), broadcast the per-lane
    // immediate and OR/AND it into every lane.  VEOR has no immediate form.
    if ((Insn->id == ARM_INS_VORR || Insn->id == ARM_INS_VAND) &&
        ARM.op_count == 2 && ARM.operands[1].type == ARM_OP_IMM) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar DstIn = operandRead(S, ARM.operands[0]);
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned LaneSz = LI.LaneSz ? LI.LaneSz : 4;
      uint64_t LaneMask = (LaneSz >= 8) ? ~0ULL : ((1ULL << (LaneSz * 8)) - 1);
      uint64_t Imm = static_cast<uint64_t>(ARM.operands[1].imm) & LaneMask;
      NdOp ImmOpc = (Insn->id == ARM_INS_VORR) ? NdOp::INT_OR : NdOp::INT_AND;
      if (DstIn.Size > LaneSz) {
        unsigned NLanes = DstIn.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {DstIn, NdVar::cst(I * LaneSz, 4)});
          NdVar R = S.makeTemp(LaneSz);
          S.emit(ImmOpc, R, {La, NdVar::cst(Imm, LaneSz)});
          if (I == 0)
            Acc = R;
          else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {R, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(ImmOpc, Dst, {DstIn, NdVar::cst(Imm, DstIn.Size)});
      }
      break;
    }
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdOp Opc = NdOp::INT_AND;
    if (Insn->id == ARM_INS_VORR)
      Opc = NdOp::INT_OR;
    else if (Insn->id == ARM_INS_VEOR)
      Opc = NdOp::INT_XOR;
    S.emit(Opc, Dst, {A, B});
    break;
  }
  case ARM_INS_VORN: {
    // VORN: Dst = a | ~b
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    S.emit(NdOp::INT_OR, Dst, {A, NB});
    break;
  }
  case ARM_INS_VBIC: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    // Immediate form: `vbic.iN dD, #imm`  =>  dD &= ~imm (imm broadcast/lane).
    // The previous code required 3 operands and silently dropped this form, so
    // the masking it performs (e.g. clearing the high byte of each 16-bit lane
    // when widening bytes) was a no-op and the value was left unchanged.
    if (ARM.op_count == 2 && ARM.operands[1].type == ARM_OP_IMM) {
      NdVar DstIn = operandRead(S, ARM.operands[0]);
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned LaneSz = LI.LaneSz ? LI.LaneSz : 2;
      uint64_t LaneMask = (LaneSz >= 8) ? ~0ULL : ((1ULL << (LaneSz * 8)) - 1);
      uint64_t NotImm =
          (~static_cast<uint64_t>(ARM.operands[1].imm)) & LaneMask;
      if (DstIn.Size > LaneSz) {
        unsigned NLanes = DstIn.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {DstIn, NdVar::cst(I * LaneSz, 4)});
          NdVar R = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_AND, R, {La, NdVar::cst(NotImm, LaneSz)});
          if (I == 0)
            Acc = R;
          else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {R, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::INT_AND, Dst, {DstIn, NdVar::cst(NotImm, DstIn.Size)});
      }
      break;
    }
    // Register form: Dst = a & ~b
    if (ARM.op_count < 3)
      break;
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    S.emit(NdOp::INT_AND, Dst, {A, NB});
    break;
  }
  case ARM_INS_VBSL: {
    // VBSL: Dst = (op1 & dst_old) | (op2 & ~dst_old)
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Mask = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {A, Mask});
    NdVar NMask = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NMask, {Mask});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {B, NMask});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }
  case ARM_INS_VBIT: {
    // VBIT: Dst = (op1 & op2) | (dst_old & ~op2)
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {A, B});
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {NdVar::reg(Dst.Offset, Dst.Size), NB});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }
  case ARM_INS_VBIF: {
    // VBIF: Dst = (dst_old & op2) | (op1 & ~op2)
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {NdVar::reg(Dst.Offset, Dst.Size), B});
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {A, NB});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }

  // NEON widening add: per-lane sign/zero extend then add
  case ARM_INS_VADDL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    if (LI.LaneSz > 0 && A.Size >= LI.LaneSz && Dst.Size > A.Size) {
      unsigned NLanes = A.Size / LI.LaneSz;
      unsigned WLaneSz = LI.LaneSz * 2;
      auto ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdVar Acc = NdVar::cst(0, 0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar LB = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar WA = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WA, {LA});
        NdVar WB = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WB, {LB});
        NdVar Sum = S.makeTemp(WLaneSz);
        S.emit(NdOp::INT_ADD, Sum, {WA, WB});
        if (I == 0) {
          Acc = Sum;
        } else {
          NdVar P = S.makeTemp(Acc.Size + WLaneSz);
          S.emit(NdOp::CONCAT, P, {Sum, Acc});
          Acc = P;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }

  // NEON pairwise add: d[i] = a[2i] + a[2i+1], d[N/2+i] = b[2i] + b[2i+1]
  case ARM_INS_VPADD: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    if (LI.LaneSz > 0 && A.Size >= 2 * LI.LaneSz) {
      unsigned NPairs = A.Size / LI.LaneSz / 2;
      NdVar Acc = NdVar::cst(0, 0);
      auto doAdd = [&](const NdVar &Src, unsigned PairIdx) {
        for (unsigned P = 0; P < NPairs; ++P) {
          NdVar Lo = S.makeTemp(LI.LaneSz);
          S.emit(
              NdOp::SUBBYTES, Lo,
              {Src, NdVar::cst(static_cast<uint64_t>(P) * 2 * LI.LaneSz, 4)});
          NdVar Hi = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(
                           (static_cast<uint64_t>(P) * 2 + 1) * LI.LaneSz, 4)});
          NdVar Sum = S.makeTemp(LI.LaneSz);
          if (LI.IsFloat)
            S.emit(NdOp::FLOAT_ADD, Sum, {Lo, Hi});
          else
            S.emit(NdOp::INT_ADD, Sum, {Lo, Hi});
          if (PairIdx == 0 && P == 0) {
            Acc = Sum;
          } else {
            NdVar Prev = S.makeTemp(Acc.Size + LI.LaneSz);
            S.emit(NdOp::CONCAT, Prev, {Sum, Acc});
            Acc = Prev;
          }
        }
      };
      doAdd(A, 0);
      doAdd(B, 1);
      if (Acc.Size == Dst.Size)
        S.emit(NdOp::COPY, Dst, {Acc});
      else if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::SUBBYTES, Dst, {Acc, NdVar::cst(0, 4)});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }

  // VADDW: wide += widen(narrow), per lane.  operand[1] is already wide (Q),
  // operand[2] is the narrow source (D) which is sign/zero extended.
  case ARM_INS_VADDW: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcLaneSz = LI.LaneSz;
    if (SrcLaneSz > 0 && SrcLaneSz < 8) {
      unsigned DstLaneSz = SrcLaneSz * 2;
      unsigned NLanes = Dst.Size / DstLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar AW = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SUBBYTES, AW, {A, NdVar::cst(I * DstLaneSz, 4)});
        NdVar BN = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, BN, {B, NdVar::cst(I * SrcLaneSz, 4)});
        NdVar BW = S.makeTemp(DstLaneSz);
        S.emit(LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, BW, {BN});
        NdVar R = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_ADD, R, {AW, BW});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }
  // VADDHN/VRADDHN: add two wide vectors, return the high half of each lane
  // (rounding variant adds half an ULP first).
  case ARM_INS_VADDHN:
  case ARM_INS_VRADDHN: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    bool IsRound = (Insn->id == ARM_INS_VRADDHN);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned WideLaneSz = LI.LaneSz;
    if (WideLaneSz >= 2 && A.Size >= WideLaneSz) {
      unsigned NarrowLaneSz = WideLaneSz / 2;
      unsigned NLanes = A.Size / WideLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar AL = S.makeTemp(WideLaneSz), BL = S.makeTemp(WideLaneSz);
        S.emit(NdOp::SUBBYTES, AL, {A, NdVar::cst(I * WideLaneSz, 4)});
        S.emit(NdOp::SUBBYTES, BL, {B, NdVar::cst(I * WideLaneSz, 4)});
        NdVar Sum = S.makeTemp(WideLaneSz);
        S.emit(NdOp::INT_ADD, Sum, {AL, BL});
        if (IsRound) {
          NdVar Sum1 = S.makeTemp(WideLaneSz);
          S.emit(
              NdOp::INT_ADD, Sum1,
              {Sum, NdVar::cst(1ULL << (NarrowLaneSz * 8 - 1), WideLaneSz)});
          Sum = Sum1;
        }
        NdVar Hi = S.makeTemp(NarrowLaneSz);
        S.emit(NdOp::SUBBYTES, Hi, {Sum, NdVar::cst(NarrowLaneSz, 4)});
        if (I == 0) {
          Acc = Hi;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + NarrowLaneSz);
          S.emit(NdOp::CONCAT, Next, {Hi, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }
  // VSUBHN/VRSUBHN: subtract two wide vectors, return the high half of each
  // lane (rounding variant adds half an ULP first).  Mirror of VADDHN; was a
  // full-width INT_SUB placeholder (no narrowing, no per-lane).
  case ARM_INS_VSUBHN:
  case ARM_INS_VRSUBHN: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    bool IsRound = (Insn->id == ARM_INS_VRSUBHN);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned WideLaneSz = LI.LaneSz;
    if (WideLaneSz >= 2 && A.Size >= WideLaneSz) {
      unsigned NarrowLaneSz = WideLaneSz / 2;
      unsigned NLanes = A.Size / WideLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar AL = S.makeTemp(WideLaneSz), BL = S.makeTemp(WideLaneSz);
        S.emit(NdOp::SUBBYTES, AL, {A, NdVar::cst(I * WideLaneSz, 4)});
        S.emit(NdOp::SUBBYTES, BL, {B, NdVar::cst(I * WideLaneSz, 4)});
        NdVar Diff = S.makeTemp(WideLaneSz);
        S.emit(NdOp::INT_SUB, Diff, {AL, BL});
        if (IsRound) {
          NdVar Diff1 = S.makeTemp(WideLaneSz);
          S.emit(
              NdOp::INT_ADD, Diff1,
              {Diff, NdVar::cst(1ULL << (NarrowLaneSz * 8 - 1), WideLaneSz)});
          Diff = Diff1;
        }
        NdVar Hi = S.makeTemp(NarrowLaneSz);
        S.emit(NdOp::SUBBYTES, Hi, {Diff, NdVar::cst(NarrowLaneSz, 4)});
        if (I == 0) {
          Acc = Hi;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + NarrowLaneSz);
          S.emit(NdOp::CONCAT, Next, {Hi, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  // Per-lane halving add: dst[i] = (a[i]+b[i](+1)) >> 1.  VHADD truncates,
  // VRHADD rounds; sign/zero from the data type.  The old code did a single
  // full-width INT_ADD (no halving, no per-lane).
  case ARM_INS_VHADD:
  case ARM_INS_VRHADD: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    bool IsRound = (Insn->id == ARM_INS_VRHADD);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz;
    if (LaneSz > 0 && LaneSz <= 4 && Dst.Size >= LaneSz) {
      unsigned WideSz = LaneSz * 2;
      unsigned NLanes = Dst.Size / LaneSz;
      NdOp ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdOp ShOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(ExtOp, Aw, {Al});
        S.emit(ExtOp, Bw, {Bl});
        NdVar Sum = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Sum, {Aw, Bw});
        if (IsRound) {
          NdVar Sum1 = S.makeTemp(WideSz);
          S.emit(NdOp::INT_ADD, Sum1, {Sum, NdVar::cst(1, WideSz)});
          Sum = Sum1;
        }
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(ShOp, Sh, {Sum, NdVar::cst(1, WideSz)});
        NdVar Res = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Res, {Sh, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }
  // Per-lane halving subtract: dst[i] = (a[i]-b[i]) >> 1 (no rounding form).
  // Was grouped with VSUBHN/VCADD as a full-width INT_SUB placeholder (no
  // halving, cross-lane borrow).
  case ARM_INS_VHSUB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz;
    if (LaneSz > 0 && LaneSz <= 4 && Dst.Size >= LaneSz) {
      unsigned WideSz = LaneSz * 2;
      unsigned NLanes = Dst.Size / LaneSz;
      NdOp ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdOp ShOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(ExtOp, Aw, {Al});
        S.emit(ExtOp, Bw, {Bl});
        NdVar Diff = S.makeTemp(WideSz);
        S.emit(NdOp::INT_SUB, Diff, {Aw, Bw});
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(ShOp, Sh, {Diff, NdVar::cst(1, WideSz)});
        NdVar Res = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Res, {Sh, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  // VPADDL: pairwise add adjacent lanes of one source and widen to 2x lane
  // (Dst[i] = widen(Src[2i]) + widen(Src[2i+1])).  VPADAL additionally adds
  // the prior Dst lane.  The old placeholder just COPYed the source, dropping
  // both the pairwise sum and the widening — breaking every vcnt+vpaddl
  // popcount reduction clang emits.
  case ARM_INS_VPADDL:
  case ARM_INS_VPADAL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    bool IsAccum = (Insn->id == ARM_INS_VPADAL);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz;
    if (LaneSz > 0 && LaneSz < 8 && Dst.Size >= LaneSz * 2) {
      unsigned WLaneSz = LaneSz * 2;
      unsigned NDst = Dst.Size / WLaneSz;
      auto ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdVar OldDst;
      if (IsAccum)
        OldDst = operandRead(S, ARM.operands[0]);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NDst; ++I) {
        NdVar A = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, A, {Src, NdVar::cst((2 * I) * LaneSz, 4)});
        NdVar B = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, B, {Src, NdVar::cst((2 * I + 1) * LaneSz, 4)});
        NdVar WA = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WA, {A});
        NdVar WB = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WB, {B});
        NdVar Sum = S.makeTemp(WLaneSz);
        S.emit(NdOp::INT_ADD, Sum, {WA, WB});
        if (IsAccum) {
          NdVar Old = S.makeTemp(WLaneSz);
          S.emit(NdOp::SUBBYTES, Old, {OldDst, NdVar::cst(I * WLaneSz, 4)});
          NdVar Sum2 = S.makeTemp(WLaneSz);
          S.emit(NdOp::INT_ADD, Sum2, {Sum, Old});
          Sum = Sum2;
        }
        if (I == 0) {
          Acc = Sum;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + WLaneSz);
          S.emit(NdOp::CONCAT, Next, {Sum, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VSUBL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    if (LI.LaneSz > 0 && A.Size >= LI.LaneSz && Dst.Size > A.Size) {
      unsigned NLanes = A.Size / LI.LaneSz;
      unsigned WLaneSz = LI.LaneSz * 2;
      auto ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdVar Acc = NdVar::cst(0, 0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar LB = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar WA = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WA, {LA});
        NdVar WB = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WB, {LB});
        NdVar Diff = S.makeTemp(WLaneSz);
        S.emit(NdOp::INT_SUB, Diff, {WA, WB});
        if (I == 0) {
          Acc = Diff;
        } else {
          NdVar P = S.makeTemp(Acc.Size + WLaneSz);
          S.emit(NdOp::CONCAT, P, {Diff, Acc});
          Acc = P;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  // VSUBW: wide -= widen(narrow), per lane (mirror of VADDW).  The generic
  // full-width INT_SUB both skipped per-lane widening of the narrow D source
  // and propagated the borrow across the 64-bit lanes.
  case ARM_INS_VSUBW: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcLaneSz = LI.LaneSz;
    if (SrcLaneSz > 0 && SrcLaneSz < 8) {
      unsigned DstLaneSz = SrcLaneSz * 2;
      unsigned NLanes = Dst.Size / DstLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar AW = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SUBBYTES, AW, {A, NdVar::cst(I * DstLaneSz, 4)});
        NdVar BN = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, BN, {B, NdVar::cst(I * SrcLaneSz, 4)});
        NdVar BW = S.makeTemp(DstLaneSz);
        S.emit(LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, BW, {BN});
        NdVar R = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_SUB, R, {AW, BW});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  case ARM_INS_VHCADD:
  // VCADD — rotated complex floating-point add (AArch32 FEAT_FCMA).  Per pair:
  //   rot 90:  re = Vn.re - Vm.im;  im = Vn.im + Vm.re
  //   rot 270: re = Vn.re + Vm.im;  im = Vn.im - Vm.re
  // The old code was a whole-register INT_SUB (integer op on FP, no rotation).
  case ARM_INS_VCADD: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Vn = operandRead(S, ARM.operands[1]);
    NdVar Vm = operandRead(S, ARM.operands[2]);
    int64_t Rot = ARM.operands[3].imm;
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned ES = LI.LaneSz;
    if (!LI.IsFloat || (ES != 4 && ES != 8) || Dst.Size < 2 * ES) {
      S.emit(NdOp::INT_SUB, Dst, {Vn, Vm});
      break;
    }
    auto lane = [&](NdVar V, unsigned Idx) {
      NdVar T = S.makeTemp(ES);
      S.emit(NdOp::SUBBYTES, T,
             {V, NdVar::cst(static_cast<uint64_t>(Idx) * ES, 4)});
      return T;
    };
    NdVar Acc = NdVar::cst(0, 0);
    bool First = true;
    auto append = [&](NdVar L) {
      if (First) {
        Acc = L;
        First = false;
        return;
      }
      NdVar N = S.makeTemp(Acc.Size + ES);
      S.emit(NdOp::CONCAT, N, {L, Acc});
      Acc = N;
    };
    unsigned NLanes = Dst.Size / ES;
    for (unsigned K = 0; K < NLanes / 2; ++K) {
      NdVar VnRe = lane(Vn, 2 * K), VnIm = lane(Vn, 2 * K + 1);
      NdVar VmRe = lane(Vm, 2 * K), VmIm = lane(Vm, 2 * K + 1);
      NdVar OutRe = S.makeTemp(ES), OutIm = S.makeTemp(ES);
      if (Rot == 270) {
        S.emit(NdOp::FLOAT_ADD, OutRe, {VnRe, VmIm});
        S.emit(NdOp::FLOAT_SUB, OutIm, {VnIm, VmRe});
      } else {
        S.emit(NdOp::FLOAT_SUB, OutRe, {VnRe, VmIm});
        S.emit(NdOp::FLOAT_ADD, OutIm, {VnIm, VmRe});
      }
      append(OutRe);
      append(OutIm);
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_VMULL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    // vmull.p8 — polynomial (carry-less) widening multiply (8 i8 pairs -> 8
    // i16), NOT integer multiply; map to the ARM NEON intrinsic.
    if (ARM.vector_data == ARM_VECTORDATA_P8) {
      S.emitIntrinsic(Intrinsic::ArmVmullp, Dst, {A, B});
      break;
    }
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    if (LI.LaneSz > 0 && A.Size >= LI.LaneSz && Dst.Size > A.Size) {
      unsigned NLanes = A.Size / LI.LaneSz;
      unsigned WLaneSz = LI.LaneSz * 2;
      auto ExtOp = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      // By-scalar `vmull.s16 q,d,d[idx]` broadcasts one Dm lane; operandRead
      // ignores vector_index and returns the whole Dm, so detect the lane index
      // and splat it (a per-lane SUBBYTES would walk d[0..N] instead).  Same
      // fix as #386 for VMUL/VMLA.
      int BLane = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                                 : ARM.operands[2].vector_index;
      NdVar ScalarB;
      if (BLane >= 0) {
        ScalarB = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, ScalarB,
               {B, NdVar::cst(static_cast<uint64_t>(BLane) * LI.LaneSz, 4)});
      }
      NdVar Acc = NdVar::cst(0, 0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar LB = ScalarB;
        if (BLane < 0) {
          LB = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * LI.LaneSz, 4)});
        }
        NdVar WA = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WA, {LA});
        NdVar WB = S.makeTemp(WLaneSz);
        S.emit(ExtOp, WB, {LB});
        NdVar Prod = S.makeTemp(WLaneSz);
        S.emit(NdOp::INT_MULT, Prod, {WA, WB});
        if (I == 0) {
          Acc = Prod;
        } else {
          NdVar P = S.makeTemp(Acc.Size + WLaneSz);
          S.emit(NdOp::CONCAT, P, {Prod, Acc});
          Acc = P;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_MULT, Dst, {A, B});
    }
    break;
  }
  case ARM_INS_VMULLB:
  case ARM_INS_VMULLT:
  case ARM_INS_VMULH:
  case ARM_INS_VRMULH: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }
  // VMLA/VMLAS/VMLS — same-width multiply-accumulate.  Must be per-lane: a
  // single full-width INT_MULT/FLOAT_MULT multiplies the whole register and a
  // full-width add/sub carries across lane boundaries.  Supports the scalar
  // (`vmla.i32 q, q, d[idx]`) form where the multiplier is a broadcast.
  case ARM_INS_VMLA:
  case ARM_INS_VMLAS:
  case ARM_INS_VMLS: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSub = (Insn->id == ARM_INS_VMLS);
    bool IsFP = ARM.vector_data == ARM_VECTORDATA_F32 ||
                ARM.vector_data == ARM_VECTORDATA_F64;
    NdOp MulOp = IsFP ? NdOp::FLOAT_MULT : NdOp::INT_MULT;
    NdOp AccOp = IsFP ? (IsSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD)
                      : (IsSub ? NdOp::INT_SUB : NdOp::INT_ADD);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz;
    if (IsFP && LaneSz == 0)
      LaneSz = (ARM.vector_data == ARM_VECTORDATA_F64) ? 8 : 4;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      // `vmla.iN/.fN qD, qN, dM[idx]` — the multiplier is ONE broadcast scalar
      // lane.  operandRead returns the whole Dm register, so detect the indexed
      // form via the operand's lane index and splat that lane; size-based
      // detection never fired (Dm reads back 8 bytes) and a per-lane SUBBYTES
      // walked dM[0],dM[1],… past the register.
      int BLane = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                                 : ARM.operands[2].vector_index;
      NdVar ScalarB;
      if (BLane >= 0) {
        ScalarB = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, ScalarB,
               {B, NdVar::cst(static_cast<uint64_t>(BLane) * LaneSz, 4)});
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = ScalarB;
        if (BLane < 0) {
          Lb = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        }
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {OldDst, NdVar::cst(I * LaneSz, 4)});
        NdVar P = S.makeTemp(LaneSz);
        S.emit(MulOp, P, {La, Lb});
        NdVar R = S.makeTemp(LaneSz);
        S.emit(AccOp, R, {Ld, P});
        if (I == 0)
          Acc = R;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(MulOp, Prod, {A, B});
      S.emit(AccOp, Dst, {OldDst, Prod});
    }
    break;
  }
  // VMLAL/VMLSL — widening multiply-accumulate: Qd[i] +=
  // widen(Dn[i])*widen(Dm[i]). Source lanes are narrow (InSz), destination
  // lanes are double width (OutSz). A full-width multiply produces a single
  // 64x64->128 product, not per-lane.
  case ARM_INS_VMLAL:
  case ARM_INS_VMLSL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSub = (Insn->id == ARM_INS_VMLSL);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned InSz = LI.LaneSz;
    NdOp Ext = LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
    if (InSz > 0 && InSz <= 4) {
      unsigned OutSz = InSz * 2;
      unsigned NLanes = Dst.Size / OutSz;
      // By-scalar `vmlal.s16 q,d,d[idx]` broadcasts one Dm lane; operandRead
      // ignores vector_index (returns the whole 8-byte Dm) so the old
      // `B.Size<=InSz` test never fired — detect the lane index and splat it.
      int BLane = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                                 : ARM.operands[2].vector_index;
      NdVar ScalarB;
      if (BLane >= 0) {
        ScalarB = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, ScalarB,
               {B, NdVar::cst(static_cast<uint64_t>(BLane) * InSz, 4)});
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * InSz, 4)});
        NdVar Lb = ScalarB;
        if (BLane < 0) {
          Lb = S.makeTemp(InSz);
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * InSz, 4)});
        }
        NdVar Wa = S.makeTemp(OutSz);
        S.emit(Ext, Wa, {La});
        NdVar Wb = S.makeTemp(OutSz);
        S.emit(Ext, Wb, {Lb});
        NdVar P = S.makeTemp(OutSz);
        S.emit(NdOp::INT_MULT, P, {Wa, Wb});
        NdVar Ld = S.makeTemp(OutSz);
        S.emit(NdOp::SUBBYTES, Ld, {OldDst, NdVar::cst(I * OutSz, 4)});
        NdVar R = S.makeTemp(OutSz);
        S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, R, {Ld, P});
        if (I == 0)
          Acc = R;
        else {
          NdVar Next = S.makeTemp(Acc.Size + OutSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {A, B});
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Dst, {OldDst, Prod});
    }
    break;
  }
  case ARM_INS_VABD:
  case ARM_INS_VABDL:
  case ARM_INS_VABA:
  case ARM_INS_VABAL: {
    // VABD: Vd = |Vn - Vm|;  VABA: Vd += |Vn - Vm|  (the "L" forms widen the
    // narrow input lanes to double width).  Both must be per-lane: the previous
    // implementation did a single full-width compare+subtract+select over the
    // whole register, collapsing all lanes to one predicate and propagating
    // borrows across lane boundaries (broke SAD: `vaba.u32`/`vabd.u32`).
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    bool IsAccum = (Insn->id == ARM_INS_VABA || Insn->id == ARM_INS_VABAL);
    bool IsWiden = (Insn->id == ARM_INS_VABDL || Insn->id == ARM_INS_VABAL);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned InSz = LI.LaneSz; // narrow (source) lane width
    bool IsSigned = LI.IsSigned;
    if (InSz > 0) {
      unsigned OutSz = IsWiden ? InSz * 2 : InSz;
      unsigned NLanes = Dst.Size / OutSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * InSz, 4)});
        NdVar Lb = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * InSz, 4)});
        // Compute |a-b| in the (possibly widened) output lane width.
        NdVar Wa = La, Wb = Lb;
        if (OutSz != InSz) {
          Wa = S.makeTemp(OutSz);
          Wb = S.makeTemp(OutSz);
          NdOp Ext = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
          S.emit(Ext, Wa, {La});
          S.emit(Ext, Wb, {Lb});
        }
        NdVar Lt = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, Lt, {Wa, Wb});
        NdVar DiffPos = S.makeTemp(OutSz);
        S.emit(NdOp::INT_SUB, DiffPos, {Wa, Wb});
        NdVar DiffNeg = S.makeTemp(OutSz);
        S.emit(NdOp::INT_SUB, DiffNeg, {Wb, Wa});
        NdVar Abs = S.makeTemp(OutSz);
        S.emit(NdOp::SELECT, Abs, {Lt, DiffNeg, DiffPos});
        if (IsAccum) {
          NdVar Old = S.makeTemp(OutSz);
          S.emit(
              NdOp::SUBBYTES, Old,
              {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(I * OutSz, 4)});
          NdVar Sum = S.makeTemp(OutSz);
          S.emit(NdOp::INT_ADD, Sum, {Old, Abs});
          Abs = Sum;
        }
        if (I == 0)
          Acc = Abs;
        else {
          NdVar Next = S.makeTemp(Acc.Size + OutSz);
          S.emit(NdOp::CONCAT, Next, {Abs, Acc});
          Acc = Next;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Unknown lane width: fall back to the (scalar) absolute difference.
      NdVar GE = S.makeTemp(1);
      S.emit(NdOp::INT_LESSEQUAL, GE, {B, A});
      NdVar DiffPos = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, DiffPos, {A, B});
      NdVar DiffNeg = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_SUB, DiffNeg, {B, A});
      NdVar AbsDiff = S.makeTemp(Dst.Size);
      S.emit(NdOp::SELECT, AbsDiff, {GE, DiffPos, DiffNeg});
      if (IsAccum)
        S.emit(NdOp::INT_ADD, Dst,
               {NdVar::reg(Dst.Offset, Dst.Size), AbsDiff});
      else
        S.emit(NdOp::COPY, Dst, {AbsDiff});
    }
    break;
  }
  case ARM_INS_VABAV: {
    // VABAV: scalar += sum of |Vn - Vm| across all lanes (reduce).
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned InSz = LI.LaneSz;
    bool IsSigned = LI.IsSigned;
    if (InSz > 0 && A.Size >= InSz) {
      unsigned NLanes = A.Size / InSz;
      NdVar Sum = NdVar::reg(Dst.Offset, Dst.Size);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * InSz, 4)});
        NdVar Lb = S.makeTemp(InSz);
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * InSz, 4)});
        NdVar Lt = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, Lt, {La, Lb});
        NdVar DiffPos = S.makeTemp(InSz);
        S.emit(NdOp::INT_SUB, DiffPos, {La, Lb});
        NdVar DiffNeg = S.makeTemp(InSz);
        S.emit(NdOp::INT_SUB, DiffNeg, {Lb, La});
        NdVar Abs = S.makeTemp(InSz);
        S.emit(NdOp::SELECT, Abs, {Lt, DiffNeg, DiffPos});
        NdVar AbsW = Abs;
        if (Dst.Size != InSz) {
          AbsW = S.makeTemp(Dst.Size);
          S.emit(NdOp::INT_ZEXT, AbsW, {Abs});
        }
        NdVar NewSum = S.makeTemp(Dst.Size);
        S.emit(NdOp::INT_ADD, NewSum, {Sum, AbsW});
        Sum = NewSum;
      }
      S.emit(NdOp::COPY, Dst, {Sum});
    }
    break;
  }

  // NEON compare
  case ARM_INS_VCEQ: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp Op = LI.IsFloat ? NdOp::FLOAT_EQUAL : NdOp::INT_EQUAL;
    emitPerLaneCmpMask(S, Dst, A, B, Op, LI.LaneSz);
    break;
  }
  case ARM_INS_VCGE:
  case ARM_INS_VACGE: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    // a >= b  <=>  b <= a (operands swapped).
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp Op = LI.IsFloat    ? NdOp::FLOAT_LESSEQUAL
              : LI.IsSigned ? NdOp::INT_SLESSEQUAL
                            : NdOp::INT_LESSEQUAL;
    emitPerLaneCmpMask(S, Dst, B, A, Op, LI.LaneSz);
    break;
  }
  case ARM_INS_VCGT:
  case ARM_INS_VACGT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    // a > b  <=>  b < a (operands swapped).
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp Op = LI.IsFloat    ? NdOp::FLOAT_LESS
              : LI.IsSigned ? NdOp::INT_SLESS
                            : NdOp::INT_LESS;
    emitPerLaneCmpMask(S, Dst, B, A, Op, LI.LaneSz);
    break;
  }
  case ARM_INS_VCLE: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp Op = LI.IsFloat    ? NdOp::FLOAT_LESSEQUAL
              : LI.IsSigned ? NdOp::INT_SLESSEQUAL
                            : NdOp::INT_LESSEQUAL;
    emitPerLaneCmpMask(S, Dst, A, B, Op, LI.LaneSz);
    break;
  }
  case ARM_INS_VCLT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp Op = LI.IsFloat    ? NdOp::FLOAT_LESS
              : LI.IsSigned ? NdOp::INT_SLESS
                            : NdOp::INT_LESS;
    emitPerLaneCmpMask(S, Dst, A, B, Op, LI.LaneSz);
    break;
  }
  case ARM_INS_VTST: {
    // VTST sets each lane to all-ones when (a & b) != 0 in that lane, else 0.
    // The bitwise AND is identical full-width or per-lane, but the != 0 test
    // and the all-ones result must be computed *per lane*.
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Anded = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, Anded, {A, B});
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    emitPerLaneCmpMask(S, Dst, Anded, NdVar::cst(0, A.Size),
                       NdOp::INT_NOTEQUAL, LI.LaneSz);
    break;
  }

  // NEON misc unary: operations with correct NdOp mapping
  case ARM_INS_VMVN: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      const auto &SrcOp = ARM.operands[1];
      // vmvn.iN dN/qN, #imm — broadcast ~imm across ALL lanes.  Previously this
      // form did `INT_NOT Dst(16), imm(4)` which only set lane 0 and left the
      // other lanes undefined/zero (e.g. a broadcast -4096 only landed in
      // lane 0, so `(x & mask) - 4096` was applied to one lane in four).
      if (SrcOp.type == ARM_OP_IMM && Dst.Size > 4) {
        auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
        unsigned LaneSz = LI.LaneSz ? LI.LaneSz : 4;
        uint64_t Imm = static_cast<uint64_t>(SrcOp.imm);
        uint64_t LMask = (LaneSz >= 8) ? ~0ULL : ((1ULL << (LaneSz * 8)) - 1);
        uint64_t NotImm = (~Imm) & LMask;
        uint64_t Lo = 0, Hi = 0;
        for (unsigned I = 0; I * LaneSz < 8 && I * LaneSz < Dst.Size; ++I)
          Lo |= NotImm << (I * LaneSz * 8);
        for (unsigned I = 0; I * LaneSz < Dst.Size; ++I)
          if (I * LaneSz >= 8)
            Hi |= NotImm << ((I * LaneSz - 8) * 8);
        if (Dst.Size <= 8) {
          S.emit(NdOp::COPY, Dst, {NdVar::cst(Lo, Dst.Size)});
        } else {
          NdVar LoV = S.makeTemp(8);
          S.emit(NdOp::COPY, LoV, {NdVar::cst(Lo, 8)});
          NdVar HiV = S.makeTemp(Dst.Size - 8);
          S.emit(NdOp::COPY, HiV,
                 {NdVar::cst(Hi, static_cast<uint16_t>(Dst.Size - 8))});
          S.emit(NdOp::CONCAT, Dst, {HiV, LoV});
        }
        break;
      }
      NdVar Src = operandRead(S, ARM.operands[1]);
      S.emit(NdOp::INT_NOT, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VCLZ: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      // `vclz.iN` counts leading zeros PER LANE; a single LZCOUNT on the whole
      // register counts zeros of the full width and breaks the reduction.
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned LaneSz = LI.LaneSz;
      if (LaneSz > 0 && Dst.Size > LaneSz) {
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Src, NdVar::cst(I * LaneSz, 4)});
          NdVar Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::LZCOUNT, Lr, {La});
          if (I == 0) {
            Acc = Lr;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Lr, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::LZCOUNT, Dst, {Src});
      }
    }
    break;
  }
  case ARM_INS_VCNT: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      unsigned NBytes = Src.Size;
      if (NBytes > 1) {
        NdVar Acc = NdVar::cst(0, 0);
        for (unsigned I = 0; I < NBytes; ++I) {
          NdVar B = S.makeTemp(1);
          S.emit(NdOp::SUBBYTES, B, {Src, NdVar::cst(I, 4)});
          NdVar Pop = S.makeTemp(1);
          S.emit(NdOp::POPCOUNT, Pop, {B});
          if (I == 0) {
            Acc = Pop;
          } else {
            NdVar P = S.makeTemp(Acc.Size + 1);
            S.emit(NdOp::CONCAT, P, {Pop, Acc});
            Acc = P;
          }
        }
        if (Acc.Size < Dst.Size)
          S.emit(NdOp::INT_ZEXT, Dst, {Acc});
        else
          S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::POPCOUNT, Dst, {Src});
      }
    }
    break;
  }
  case ARM_INS_VCLS: {
    // Count leading sign bits PER LANE: cls(x) = clz(x ^ (x >>s 1)) - 1.
    // Was a COPY placeholder (did nothing).
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned LaneSz = LI.LaneSz;
      if (LaneSz > 0 && Dst.Size > LaneSz) {
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Src, NdVar::cst(I * LaneSz, 4)});
          NdVar Sh = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_ASHR, Sh, {La, NdVar::cst(1, LaneSz)});
          NdVar Xr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_XOR, Xr, {La, Sh});
          NdVar Cz = S.makeTemp(LaneSz);
          S.emit(NdOp::LZCOUNT, Cz, {Xr});
          NdVar R = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_SUB, R, {Cz, NdVar::cst(1, LaneSz)});
          if (I == 0) {
            Acc = R;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {R, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
    }
    break;
  }
  case ARM_INS_VPNOT:
  case ARM_INS_VCTP: {
    // MVE (M-profile Helium) predicate ops — not exercised by A-profile NEON
    // tests; left as a placeholder COPY.
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VMOVL: {
    // Widen each narrow lane of the D source to a wide lane of the Q dest.
    // A single full-width INT_ZEXT (the old behaviour) only zero-extends the
    // whole 64-bit value into the low half of the 128-bit dest, which is wrong
    // for every lane above lane 0.
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned SrcLaneSz = LI.LaneSz;
      if (SrcLaneSz > 0 && SrcLaneSz < 8 && Src.Size >= SrcLaneSz) {
        unsigned DstLaneSz = SrcLaneSz * 2;
        unsigned NLanes = Src.Size / SrcLaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar SLane = S.makeTemp(SrcLaneSz);
          S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * SrcLaneSz, 4)});
          NdVar WLane = S.makeTemp(DstLaneSz);
          S.emit(LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WLane, {SLane});
          if (I == 0) {
            Acc = WLane;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
            S.emit(NdOp::CONCAT, Next, {WLane, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::INT_ZEXT, Dst, {Src});
      }
    }
    break;
  }
  case ARM_INS_VMOVLB:
  case ARM_INS_VMOVLT: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      S.emit(NdOp::INT_ZEXT, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VMOVN: {
    // Narrow each element of the Q source to half-width in the D dest.  Taking
    // the low half of the whole register (the old behaviour) only keeps the
    // first half of the lanes instead of truncating every lane.
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned SrcLaneSz = LI.LaneSz;
      if (SrcLaneSz >= 2 && Src.Size >= SrcLaneSz) {
        unsigned DstLaneSz = SrcLaneSz / 2;
        unsigned NLanes = Src.Size / SrcLaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar SLane = S.makeTemp(SrcLaneSz);
          S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * SrcLaneSz, 4)});
          NdVar NLane = S.makeTemp(DstLaneSz);
          S.emit(NdOp::SUBBYTES, NLane, {SLane, NdVar::cst(0, 4)});
          if (I == 0) {
            Acc = NLane;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
            S.emit(NdOp::CONCAT, Next, {NLane, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
      }
    }
    break;
  }
  case ARM_INS_VMOVNB:
  case ARM_INS_VMOVNT: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
    }
    break;
  }
  case ARM_INS_VREV16:
  case ARM_INS_VREV32:
  case ARM_INS_VREV64: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      // VREV<group>.<elem> reverses the order of <elem>-bit elements within
      // each <group>-bit container.  The container size is encoded by the
      // opcode (16/32/64); the element size is the data-type suffix.  The
      // previous handler simply COPY'd Src→Dst for VREV32/VREV64, so the
      // reversal silently never happened (e.g. `vrev64.32 q,q` left lanes in
      // place).  This went unnoticed because the only tests summed both lanes,
      // which is invariant under reversal.  Do a real per-element shuffle.
      unsigned GroupSz = (Insn->id == ARM_INS_VREV16)   ? 2
                         : (Insn->id == ARM_INS_VREV32) ? 4
                                                        : 8;
      unsigned ElemSz = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic).LaneSz;
      if (ElemSz == 0 || ElemSz >= GroupSz || (GroupSz % ElemSz) != 0 ||
          Dst.Size < GroupSz || (Dst.Size % GroupSz) != 0) {
        // Cannot determine a sensible element/container layout: preserve old
        // behaviour rather than emit something worse.
        S.emit(NdOp::COPY, Dst, {Src});
        break;
      }
      unsigned ElemsPerGroup = GroupSz / ElemSz;
      unsigned NumGroups = Dst.Size / GroupSz;
      // Collect reversed elements in lane order (lane 0 = lowest bytes).
      std::vector<NdVar> Elems;
      for (unsigned G = 0; G < NumGroups; ++G) {
        for (unsigned E = 0; E < ElemsPerGroup; ++E) {
          unsigned SrcElem = ElemsPerGroup - 1 - E;
          unsigned SrcByteOff = G * GroupSz + SrcElem * ElemSz;
          NdVar L = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, L, {Src, NdVar::cst(SrcByteOff, 4)});
          Elems.push_back(L);
        }
      }
      NdVar Acc = Elems[0];
      for (unsigned I = 1; I < Elems.size(); ++I) {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {Elems[I], Acc});
        Acc = Next;
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // NEON reciprocal estimate/step: use intrinsics
  case ARM_INS_VRECPE: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      S.emitIntrinsic(Intrinsic::ArmVrecpe, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VRECPS: {
    // VRECPS/VRSQRTS are BINARY (`vrecps.f32 Qd,Qn,Qm` = per-lane
    // Newton-Raphson step `2 - Qn*Qm`).  The old code read only operands[1] and
    // dropped the second source, so the refinement step was wrong.  Read both
    // sources; the 2-operand destructive form (Qd = Qd · Qm) uses Qd as the
    // first source.
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                      : operandRead(S, ARM.operands[0]);
      NdVar B = operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
      S.emitIntrinsic(Intrinsic::ArmVrecps, Dst, {A, B});
    }
    break;
  }
  case ARM_INS_VRSQRTE: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      S.emitIntrinsic(Intrinsic::ArmVrsqrte, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VRSQRTS: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar A = (ARM.op_count >= 3) ? operandRead(S, ARM.operands[1])
                                      : operandRead(S, ARM.operands[0]);
      NdVar B = operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
      S.emitIntrinsic(Intrinsic::ArmVrsqrts, Dst, {A, B});
    }
    break;
  }
  // VSWP swaps the full contents of two D/Q registers.  The old handler emitted
  // an unhandled intrinsic (silently 0) into operand 0 and never wrote operand
  // 1, so `vswp d0,d1` left d0=0 and d1 untouched.  Snapshot operand 0's old
  // value before the writes so the exchange is correct even when the registers
  // alias.
  case ARM_INS_VSWP: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Other = operandWrite(ARM.operands[1]);
      NdVar A = operandRead(S, ARM.operands[0]);
      NdVar B = operandRead(S, ARM.operands[1]);
      NdVar ATmp = S.makeTemp(A.Size);
      S.emit(NdOp::COPY, ATmp, {A});
      S.emit(NdOp::COPY, Dst, {B});
      S.emit(NdOp::COPY, Other, {ATmp});
    }
    break;
  }
  case ARM_INS_VTRN:
  case ARM_INS_VUZP:
  case ARM_INS_VZIP: {
    if (ARM.op_count < 2)
      break;
    // VZIP/VUZP/VTRN read AND write BOTH register operands.  The previous code
    // only wrote operands[0], leaving the second register (which receives the
    // high-half interleave / odd elements) stale — silently corrupting any
    // later use of that register (e.g. clang's byte-widening sequences).
    NdVar DdIn = operandRead(S, ARM.operands[0]);
    NdVar DmIn = operandRead(S, ARM.operands[1]);
    NdVar DdOut = operandWrite(ARM.operands[0]);
    NdVar DmOut = operandWrite(ARM.operands[1]);
    // Element size must come from the data type; capstone often leaves
    // vector_data INVALID for `vzip.8`/`vuzp.8`/`vtrn.8`, in which case the
    // old default of 4 made `vzip.8` interleave 4-byte groups instead of
    // bytes — silently shuffling at the wrong granularity.
    unsigned LaneSz = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic).LaneSz;
    if (LaneSz == 0)
      LaneSz = 4;

    unsigned N = DdOut.Size / LaneSz; // lanes per register
    if (N < 2) {
      S.emit(NdOp::COPY, DdOut, {DdIn});
      break;
    }

    auto getLane = [&](NdVar Reg, unsigned Idx) {
      NdVar L = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, L,
             {Reg, NdVar::cst(static_cast<uint64_t>(Idx) * LaneSz, 4)});
      return L;
    };
    auto assemble = [&](NdVar Out, const std::vector<NdVar> &Elems) {
      if (Elems.empty())
        return;
      NdVar Acc = Elems[0];
      for (unsigned I = 1; I < Elems.size(); ++I) {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Elems[I], Acc});
        Acc = Next;
      }
      S.emit(NdOp::COPY, Out, {Acc});
    };

    std::vector<NdVar> DdElems, DmElems;
    if (Insn->id == ARM_INS_VZIP) {
      // Interleave: combined[g] = even ? Dd[g/2] : Dm[g/2]; first N -> Dd,
      // next N -> Dm.
      for (unsigned G = 0; G < 2 * N; ++G) {
        NdVar E = (G % 2 == 0) ? getLane(DdIn, G / 2) : getLane(DmIn, G / 2);
        (G < N ? DdElems : DmElems).push_back(E);
      }
    } else if (Insn->id == ARM_INS_VUZP) {
      // De-interleave concat(Dd,Dm): Dd gets even indices, Dm gets odd indices.
      auto concat = [&](unsigned J) {
        return J < N ? getLane(DdIn, J) : getLane(DmIn, J - N);
      };
      for (unsigned K = 0; K < N; ++K)
        DdElems.push_back(concat(2 * K));
      for (unsigned K = 0; K < N; ++K)
        DmElems.push_back(concat(2 * K + 1));
    } else { // VTRN — transpose pairs
      for (unsigned K = 0; 2 * K + 1 < N; ++K) {
        DdElems.push_back(getLane(DdIn, 2 * K));
        DdElems.push_back(getLane(DmIn, 2 * K));
        DmElems.push_back(getLane(DdIn, 2 * K + 1));
        DmElems.push_back(getLane(DmIn, 2 * K + 1));
      }
    }
    assemble(DdOut, DdElems);
    assemble(DmOut, DmElems);
    break;
  }
  case ARM_INS_VDUP: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[ARM.op_count - 1]);
      // The broadcast element size comes from the data type (.8/.16/.32), not
      // from the source register width.  `vdup.16 dN, rM` must replicate only
      // the low 16 bits of the core register, not the whole 32-bit register.
      // Capstone often leaves vector_data INVALID for the `vdup.N dN, rM`
      // (GPR→vector) form, so fall back to the mnemonic suffix; otherwise
      // ElemSz defaulted to the 4-byte GPR width and `vdup.8` wrongly
      // broadcast the whole 32-bit register (1 valid byte per 4).
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned ElemSz = LI.LaneSz ? LI.LaneSz : Src.Size;
      if (ElemSz == 0)
        ElemSz = Src.Size;
      // `vdup.32 dN, dM[idx]` broadcasts a specific *lane* of the source
      // vector, not lane 0.  operandRead returns the whole register, so honour
      // the source lane index here — otherwise a reduction's `vdup d,dM[1]`
      // wrongly splatted lane 0 and doubled the wrong partial sum.
      auto &VdupSrcOp = ARM.operands[ARM.op_count - 1];
      int VdupSrcLane = VdupSrcOp.neon_lane >= 0 ? VdupSrcOp.neon_lane
                                                 : VdupSrcOp.vector_index;
      NdVar Elem = Src;
      if (ElemSz < Src.Size) {
        unsigned ByteOff = 0;
        if (VdupSrcLane > 0) {
          ByteOff = static_cast<unsigned>(VdupSrcLane) * ElemSz;
          if (ByteOff + ElemSz > Src.Size)
            ByteOff = 0;
        }
        Elem = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(ByteOff, 4)});
      }
      if (ElemSz > 0 && Dst.Size > ElemSz) {
        unsigned NLanes = Dst.Size / ElemSz;
        NdVar Acc = Elem;
        for (unsigned I = 1; I < NLanes; ++I) {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {Elem, Acc});
          Acc = Next;
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::COPY, Dst, {Elem});
      }
    }
    break;
  }
  case ARM_INS_VINS: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[ARM.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  // NEON VTBL/VTBX — per-byte table lookup via a SELECT chain.
  //   VTBL: d[i] = (idx[i] < table_len) ? table[idx[i]] : 0
  //   VTBX: d[i] = (idx[i] < table_len) ? table[idx[i]] : old_d[i]
  // The table spans 1-4 consecutive D registers, which capstone expands into
  // separate operands; the index is the last operand and the table registers
  // are everything between the destination and the index.
  case ARM_INS_VTBL:
  case ARM_INS_VTBX: {
    if (ARM.op_count < 3)
      break;
    bool IsTbx = (Insn->id == ARM_INS_VTBX);
    NdVar Dst = operandWrite(ARM.operands[0]);
    unsigned IdxOp = static_cast<unsigned>(ARM.op_count) - 1;
    NdVar Idx = operandRead(S, ARM.operands[IdxOp]);
    std::vector<NdVar> TblBytes;
    for (unsigned R = 1; R < IdxOp; ++R) {
      NdVar T = operandRead(S, ARM.operands[R]);
      for (unsigned J = 0; J < T.Size; ++J) {
        NdVar B = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, B, {T, NdVar::cst(J, 4)});
        TblBytes.push_back(B);
      }
    }
    unsigned TblLen = static_cast<unsigned>(TblBytes.size());
    unsigned NBytes = Dst.Size;
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NBytes; ++I) {
      NdVar IdxByte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, IdxByte, {Idx, NdVar::cst(I, 4)});
      NdVar Res = S.makeTemp(1);
      if (IsTbx)
        S.emit(NdOp::SUBBYTES, Res, {OldDst, NdVar::cst(I, 4)});
      else
        S.emit(NdOp::COPY, Res, {NdVar::cst(0, 1)});
      for (unsigned J = 0; J < TblLen; ++J) {
        NdVar IsJ = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsJ, {IdxByte, NdVar::cst(J, 1)});
        NdVar NewRes = S.makeTemp(1);
        S.emit(NdOp::SELECT, NewRes, {IsJ, TblBytes[J], Res});
        Res = NewRes;
      }
      if (I == 0) {
        Acc = Res;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 1);
        S.emit(NdOp::CONCAT, Next, {Res, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_VEXT: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    unsigned ImmVal = 0;
    if (ARM.operands[3].type == ARM_OP_IMM)
      ImmVal = ARM.operands[3].imm;
    // The VEXT extract index is in ELEMENTS for the `.16`/`.32` mnemonic forms
    // (capstone reports `vext.32 ...,#3` as 3, not the 12-byte offset).  Scale
    // by the element size to get the byte offset; `.8` has elemSize 1 (the
    // historical byte-accurate behavior is preserved).
    {
      unsigned ElemSz = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic).LaneSz;
      if (ElemSz > 1)
        ImmVal *= ElemSz;
    }
    unsigned TotalBytes = Dst.Size;
    if (ImmVal == 0) {
      S.emit(NdOp::COPY, Dst, {A});
    } else if (ImmVal >= TotalBytes) {
      S.emit(NdOp::COPY, Dst, {B});
    } else {
      NdVar Hi = S.makeTemp(TotalBytes - ImmVal);
      S.emit(NdOp::SUBBYTES, Hi, {A, NdVar::cst(ImmVal, 4)});
      NdVar Lo = S.makeTemp(ImmVal);
      S.emit(NdOp::SUBBYTES, Lo, {B, NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Lo, Hi});
    }
    break;
  }

  // VSHLL (vector shift left long): widen each narrow D-source lane to a
  // double-width Q-dest lane (sign/zero-extend per `.s`/`.u`), then shift left
  // by the immediate.  Sharing the plain-VSHL path wrongly treated it as a
  // same-width per-lane shift: it read past the 8-byte source and never
  // widened, corrupting every i64-from-i32 construction.
  case ARM_INS_VSHLL: {
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcLaneSz = LI.LaneSz;
    uint64_t Imm = (ARM.op_count >= 3 && ARM.operands[2].type == ARM_OP_IMM)
                       ? static_cast<uint64_t>(ARM.operands[2].imm)
                       : 0;
    if (SrcLaneSz > 0 && SrcLaneSz < 8 && Src.Size >= SrcLaneSz) {
      unsigned DstLaneSz = SrcLaneSz * 2;
      unsigned NLanes = Src.Size / SrcLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar SLane = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * SrcLaneSz, 4)});
        NdVar WLane = S.makeTemp(DstLaneSz);
        S.emit(LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WLane, {SLane});
        NdVar Shifted = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_LEFT, Shifted, {WLane, NdVar::cst(Imm, DstLaneSz)});
        if (I == 0) {
          Acc = Shifted;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {Shifted, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {Src, NdVar::cst(Imm, Dst.Size)});
    }
    break;
  }

  // NEON saturating / rounding variable shift left (VQSHL/VQSHLU register or
  // immediate, VQRSHL register, VRSHL register).  Map to the ARM NEON intrinsic
  // (per-lane signed shift amount, negative = right; immediate forms splat
  // +imm).  Was wrongly folded into the plain VSHL handler — VQ* lost
  // saturation and VRSHL lost the rounding bias.  The intrinsic is the hardware
  // instruction, so rounding is done in wide precision (no lane overflow).
  case ARM_INS_VQSHL:
  case ARM_INS_VQSHLU:
  case ARM_INS_VRSHL:
  case ARM_INS_VQRSHL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned ElemSz = LI.LaneSz ? LI.LaneSz : Dst.Size;
    unsigned NLanes = ElemSz ? Dst.Size / ElemSz : 1;
    NdVar ShiftVec;
    if (ARM.operands[2].type == ARM_OP_IMM) {
      NdVar C =
          NdVar::cst(static_cast<uint64_t>(ARM.operands[2].imm), ElemSz);
      NdVar Acc = C;
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {C, Acc});
        Acc = Next;
      }
      ShiftVec = Acc;
    } else {
      ShiftVec = operandRead(S, ARM.operands[2]);
    }
    Intrinsic II;
    if (Insn->id == ARM_INS_VQSHLU)
      II = Intrinsic::ArmVqshiftsu;
    else if (Insn->id == ARM_INS_VQRSHL)
      II = LI.IsSigned ? Intrinsic::ArmVqrshifts : Intrinsic::ArmVqrshiftu;
    else if (Insn->id == ARM_INS_VRSHL)
      II = LI.IsSigned ? Intrinsic::ArmVrshifts : Intrinsic::ArmVrshiftu;
    else
      II = LI.IsSigned ? Intrinsic::ArmVqshifts : Intrinsic::ArmVqshiftu;
    S.emitIntrinsic(II, Dst, {Src, ShiftVec, NdVar::cst(ElemSz, 4)});
    break;
  }
  // NEON shift (non-rounding; VRSHL is handled above via its rounding
  // intrinsic)
  case ARM_INS_VSHL:
  case ARM_INS_VSHLC:
  case ARM_INS_VSHLLB:
  case ARM_INS_VSHLLT: {
    if (ARM.op_count < 3) {
      if (ARM.op_count >= 2) {
        NdVar Dst = operandWrite(ARM.operands[0]);
        NdVar Src = operandRead(S, ARM.operands[1]);
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    if (LI.LaneSz > 0 && Dst.Size > LI.LaneSz) {
      unsigned NLanes = Dst.Size / LI.LaneSz;
      // `vshl.iN dD, dM, #imm` shifts every lane by the SAME scalar immediate;
      // operandRead widens the immediate past the lane width, so testing only
      // `B.Size <= LaneSz` wrongly SUBBYTES'd it per-lane (lane 0 shifted, the
      // rest by 0).  Treat an immediate shift amount as a scalar broadcast.
      // (Same fix as VSHR — was missing here, breaking the s8-saturating-add
      //  idiom `vshl.i16 #8; vqadd.s16; vshr.s16 #8`.)
      bool BImm = (ARM.operands[2].type == ARM_OP_IMM);
      bool BScalar = BImm || (B.Size <= LI.LaneSz);
      // VSHL/VRSHL/VQSHL/VQRSHL by a *register* use a per-lane SIGNED shift
      // amount (low byte of each Vm lane): >=0 left-shifts, <0 right-shifts by
      // the magnitude (arithmetic for `.s`, logical for `.u`).  The previous
      // unconditional INT_LEFT turned a negative amount into a huge unsigned
      // left shift (poison/0) — clang emits `vneg; vshl` to express `a >> n`.
      bool IsVarReg = !BScalar && (Insn->id == ARM_INS_VSHL);
      NdOp RightOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
      NdVar BSc = B;
      if (BImm)
        BSc =
            NdVar::cst(static_cast<uint64_t>(ARM.operands[2].imm), LI.LaneSz);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar Lb = BScalar ? BSc : S.makeTemp(LI.LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar R = S.makeTemp(LI.LaneSz);
        if (IsVarReg) {
          // Sign of the shift amount comes from the low byte of the lane.
          NdVar ShByte = S.makeTemp(1);
          S.emit(NdOp::SUBBYTES, ShByte, {Lb, NdVar::cst(0, 4)});
          NdVar ShAmt = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::INT_SEXT, ShAmt, {ShByte});
          NdVar IsNeg = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, IsNeg, {ShAmt, NdVar::cst(0, LI.LaneSz)});
          NdVar NegAmt = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::INT_NEG2, NegAmt, {ShAmt});
          NdVar RightR = S.makeTemp(LI.LaneSz);
          S.emit(RightOp, RightR, {La, NegAmt});
          NdVar LeftR = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::INT_LEFT, LeftR, {La, ShAmt});
          S.emit(NdOp::SELECT, R, {IsNeg, RightR, LeftR});
        } else {
          S.emit(NdOp::INT_LEFT, R, {La, Lb});
        }
        if (I == 0)
          Acc = R;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LI.LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {A, B});
    }
    break;
  }
  // VSLI/VSRI — per-lane shift-left/right and insert, keeping the bits the
  // shift vacates from the old destination.  A full-width shift would bleed
  // bits across lanes; previously these were folded into the plain shift
  // handlers.
  case ARM_INS_VSLI:
  case ARM_INS_VSRI: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSli = (Insn->id == ARM_INS_VSLI);
    unsigned Sh = static_cast<unsigned>(ARM.operands[2].imm);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz ? LI.LaneSz : Dst.Size;
    unsigned NLanes = LaneSz ? Dst.Size / LaneSz : 1;
    unsigned LaneBits = LaneSz * 8;
    uint64_t InsMask;
    if (IsSli)
      InsMask = (Sh >= LaneBits) ? 0 : (~0ULL << Sh);
    else
      InsMask = (Sh >= LaneBits)
                    ? 0
                    : ((LaneBits >= 64) ? (~0ULL >> Sh)
                                        : (((1ULL << LaneBits) - 1) >> Sh));
    NdVar ShCst = NdVar::cst(Sh, LaneSz);
    NdVar InsC = NdVar::cst(InsMask, LaneSz);
    NdVar KeepC = NdVar::cst(~InsMask, LaneSz);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar SLane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * LaneSz, 4)});
      NdVar OLane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, OLane, {OldDst, NdVar::cst(I * LaneSz, 4)});
      NdVar Shifted = S.makeTemp(LaneSz);
      S.emit(IsSli ? NdOp::INT_LEFT : NdOp::INT_RIGHT, Shifted, {SLane, ShCst});
      NdVar Ins = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_AND, Ins, {Shifted, InsC});
      NdVar Kept = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_AND, Kept, {OLane, KeepC});
      NdVar Res = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_OR, Res, {Ins, Kept});
      if (I == 0) {
        Acc = Res;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Res, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // VSHR / VRSHR — same-width per-lane right shift by a scalar immediate (or a
  // per-lane register count).  Source and destination element widths are equal.
  case ARM_INS_VSHR:
  case ARM_INS_VRSHR: {
    if (ARM.op_count < 3) {
      if (ARM.op_count >= 2) {
        NdVar Dst = operandWrite(ARM.operands[0]);
        NdVar Src = operandRead(S, ARM.operands[1]);
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp ShOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
    // VRSHR adds a 1<<(n-1) rounding bias before the shift (in wider
    // precision).
    bool IsRound = (Insn->id == ARM_INS_VRSHR);
    unsigned ImmAmt = (ARM.operands[2].type == ARM_OP_IMM)
                          ? (unsigned)ARM.operands[2].imm
                          : 0;
    if (LI.LaneSz > 0 && Dst.Size > LI.LaneSz) {
      unsigned NLanes = Dst.Size / LI.LaneSz;
      // `vshr.N dD, dM, #imm` shifts every lane by the SAME scalar immediate.
      // operandRead widens the immediate past the lane width, so the old
      // `B.Size <= LaneSz` test wrongly treated it as a per-lane vector and
      // SUBBYTES'd it — giving 0 for every lane but lane 0.
      bool BImm = (ARM.operands[2].type == ARM_OP_IMM);
      bool BScalar = BImm || (B.Size <= LI.LaneSz);
      bool Round = IsRound && BImm && ImmAmt > 0 && ImmAmt <= LI.LaneSz * 8;
      NdVar BSc = B;
      if (BImm)
        BSc =
            NdVar::cst(static_cast<uint64_t>(ARM.operands[2].imm), LI.LaneSz);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar Lb = BScalar ? BSc : S.makeTemp(LI.LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar R = Round ? S.emitRoundedShr(La, LI.LaneSz, ImmAmt, LI.IsSigned)
                          : S.makeTemp(LI.LaneSz);
        if (!Round)
          S.emit(ShOp, R, {La, Lb});
        if (I == 0)
          Acc = R;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LI.LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else if (IsRound && ImmAmt > 0 && LI.LaneSz > 0 &&
               ImmAmt <= Dst.Size * 8) {
      NdVar R = S.emitRoundedShr(A, Dst.Size, ImmAmt, LI.IsSigned);
      S.emit(NdOp::COPY, Dst, {R});
    } else {
      S.emit(ShOp, Dst, {A, B});
    }
    break;
  }
  // VSHRN / VRSHRN — vector shift right and NARROW: each source lane (Q, width
  // 2*DstLane) is shifted right by #imm and truncated to its low half, packed
  // into the D destination.  This used to share the same-width VSHR body above:
  // for the narrowing form `Dst.Size > LaneSz` is false (Dst is the D half,
  // LaneSz is the wide source element), so it shifted the whole 128-bit source
  // and kept only the low 64 bits — correct for lane 0, wrong for the high
  // lane. Narrow truncation makes the right shift sign-agnostic (logical is
  // correct).
  case ARM_INS_VSHRN:
  case ARM_INS_VRSHRN: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcLaneSz = LI.LaneSz;
    uint64_t Imm = (ARM.operands[2].type == ARM_OP_IMM)
                       ? static_cast<uint64_t>(ARM.operands[2].imm)
                       : 0;
    bool IsRound = (Insn->id == ARM_INS_VRSHRN);
    if (SrcLaneSz >= 2 && A.Size >= SrcLaneSz) {
      unsigned DstLaneSz = SrcLaneSz / 2;
      unsigned NLanes = A.Size / SrcLaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar SLane = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, SLane, {A, NdVar::cst(I * SrcLaneSz, 4)});
        NdVar Shifted = SLane;
        if (Imm > 0) {
          if (IsRound) {
            NdVar Rounded = S.makeTemp(SrcLaneSz);
            S.emit(NdOp::INT_ADD, Rounded,
                   {SLane, NdVar::cst(1ULL << (Imm - 1), SrcLaneSz)});
            SLane = Rounded;
          }
          Shifted = S.makeTemp(SrcLaneSz);
          S.emit(NdOp::INT_RIGHT, Shifted,
                 {SLane, NdVar::cst(Imm, SrcLaneSz)});
        }
        NdVar NLane = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SUBBYTES, NLane, {Shifted, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = NLane;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {NLane, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::SUBBYTES, Dst, {A, NdVar::cst(0, 4)});
    }
    break;
  }
  // MVE bottom/top narrowing variants: keep the previous same-width behaviour
  // (M-profile Helium, not generated for A32 NEON targets, not
  // roundtrip-tested).
  case ARM_INS_VSHRNB:
  case ARM_INS_VSHRNT:
  case ARM_INS_VRSHRNB:
  case ARM_INS_VRSHRNT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp ShOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
    S.emit(ShOp, Dst, {A, B});
    break;
  }
  // VSRA/VRSRA — vector shift right and accumulate: Dd += (Dn >> #imm) per
  // lane. Must be per-lane: a full-width shift crosses lane boundaries and a
  // full-width add carries across lanes.  VRSRA adds the 1<<(n-1) rounding bias
  // before the shift in wider precision (see emitRoundedShr).
  case ARM_INS_VSRA:
  case ARM_INS_VRSRA: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    NdOp ShOp = LI.IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
    // VRSRA adds a 1<<(n-1) rounding bias before the shift (in wider
    // precision).
    bool IsRound = (Insn->id == ARM_INS_VRSRA);
    unsigned ImmAmt = (ARM.operands[2].type == ARM_OP_IMM)
                          ? (unsigned)ARM.operands[2].imm
                          : 0;
    if (LI.LaneSz > 0 && Dst.Size > LI.LaneSz) {
      unsigned NLanes = Dst.Size / LI.LaneSz;
      bool BImm = (ARM.operands[2].type == ARM_OP_IMM);
      NdVar BSc = B;
      if (BImm)
        BSc =
            NdVar::cst(static_cast<uint64_t>(ARM.operands[2].imm), LI.LaneSz);
      bool BScalar = BImm || (B.Size <= LI.LaneSz);
      bool Round = IsRound && BImm && ImmAmt > 0 && ImmAmt <= LI.LaneSz * 8;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar Lb = BScalar ? BSc : S.makeTemp(LI.LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar Sh = Round
                         ? S.emitRoundedShr(La, LI.LaneSz, ImmAmt, LI.IsSigned)
                         : S.makeTemp(LI.LaneSz);
        if (!Round)
          S.emit(ShOp, Sh, {La, Lb});
        NdVar Ld = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {OldDst, NdVar::cst(I * LI.LaneSz, 4)});
        NdVar R = S.makeTemp(LI.LaneSz);
        S.emit(NdOp::INT_ADD, R, {Ld, Sh});
        if (I == 0)
          Acc = R;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LI.LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Sh;
      if (IsRound && ImmAmt > 0 && LI.LaneSz > 0 && ImmAmt <= Dst.Size * 8)
        Sh = S.emitRoundedShr(A, Dst.Size, ImmAmt, LI.IsSigned);
      else {
        Sh = S.makeTemp(Dst.Size);
        S.emit(ShOp, Sh, {A, B});
      }
      S.emit(NdOp::INT_ADD, Dst, {OldDst, Sh});
    }
    break;
  }
  // Narrowing saturating shift-right by immediate.  Map the A32/NEON forms to
  // the ARM NEON intrinsic (wide source + immediate -> narrow, with
  // saturation); the MVE bottom/top (B/T) variants keep the simple fallback.
  // Was a plain full-width INT_RIGHT (no narrowing/saturation/per-lane).
  case ARM_INS_VQSHRN:
  case ARM_INS_VQSHRNB:
  case ARM_INS_VQSHRNT:
  case ARM_INS_VQSHRUN:
  case ARM_INS_VQSHRUNB:
  case ARM_INS_VQSHRUNT:
  case ARM_INS_VQRSHRN:
  case ARM_INS_VQRSHRNB:
  case ARM_INS_VQRSHRNT:
  case ARM_INS_VQRSHRUN:
  case ARM_INS_VQRSHRUNB:
  case ARM_INS_VQRSHRUNT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    unsigned Imm = static_cast<unsigned>(ARM.operands[2].imm);
    bool IsBT =
        (Insn->id == ARM_INS_VQSHRNB || Insn->id == ARM_INS_VQSHRNT ||
         Insn->id == ARM_INS_VQSHRUNB || Insn->id == ARM_INS_VQSHRUNT ||
         Insn->id == ARM_INS_VQRSHRNB || Insn->id == ARM_INS_VQRSHRNT ||
         Insn->id == ARM_INS_VQRSHRUNB || Insn->id == ARM_INS_VQRSHRUNT);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned WideElem = LI.LaneSz;
    if (IsBT || WideElem < 2 || Src.Size != 16) {
      S.emit(NdOp::INT_RIGHT, Dst, {Src, NdVar::cst(Imm, Dst.Size)});
      break;
    }
    unsigned NarrowElem = WideElem / 2;
    bool Round = (Insn->id == ARM_INS_VQRSHRN || Insn->id == ARM_INS_VQRSHRUN);
    bool UnsResult =
        (Insn->id == ARM_INS_VQSHRUN || Insn->id == ARM_INS_VQRSHRUN);
    Intrinsic II;
    if (UnsResult)
      II = Round ? Intrinsic::ArmVqrshiftnsu : Intrinsic::ArmVqshiftnsu;
    else if (LI.IsSigned)
      II = Round ? Intrinsic::ArmVqrshiftns : Intrinsic::ArmVqshiftns;
    else
      II = Round ? Intrinsic::ArmVqrshiftnu : Intrinsic::ArmVqshiftnu;
    S.emitIntrinsic(II, Dst,
                    {Src, NdVar::cst(Imm, 4), NdVar::cst(NarrowElem, 4)});
    break;
  }

  // NEON saturating ops
  // VQABS/VQNEG — per-lane signed saturating absolute / negate.  Only the
  // INT_MIN lane saturates (|MIN| and -MIN both -> MAX).  Previously these were
  // wrongly mapped to the Vqmovn placeholder (which returned 0).
  case ARM_INS_VQABS:
  case ARM_INS_VQNEG: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[ARM.op_count - 1]);
    bool IsAbs = (Insn->id == ARM_INS_VQABS);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned LaneSz = LI.LaneSz;
    if (LaneSz > 0 && LaneSz <= 8 && Dst.Size >= LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      unsigned Bits = LaneSz * 8;
      uint64_t MinV = (Bits >= 64) ? (1ULL << 63) : (1ULL << (Bits - 1));
      uint64_t MaxV = (Bits >= 64) ? ~(1ULL << 63) : ((1ULL << (Bits - 1)) - 1);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar L = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, L, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Neg = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Neg, {L});
        NdVar R = Neg;
        if (IsAbs) {
          NdVar IsNeg = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, IsNeg, {L, NdVar::cst(0, LaneSz)});
          R = S.makeTemp(LaneSz);
          S.emit(NdOp::SELECT, R, {IsNeg, Neg, L});
        }
        NdVar IsMin = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsMin, {L, NdVar::cst(MinV, LaneSz)});
        NdVar Sat = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Sat, {IsMin, NdVar::cst(MaxV, LaneSz), R});
        if (I == 0)
          Acc = Sat;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Sat, Acc});
          Acc = Next;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Neg = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NEG2, Neg, {Src});
      if (IsAbs) {
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Src, NdVar::cst(0, Src.Size)});
        S.emit(NdOp::SELECT, Dst, {IsNeg, Neg, Src});
      } else {
        S.emit(NdOp::COPY, Dst, {Neg});
      }
    }
    break;
  }
  // VQMOVN narrows each wide lane to half width with *saturation* into the
  // narrow lane's signed/unsigned range (per the .s/.u suffix).  The old
  // handler plain-truncated, silently dropping the saturation for out-of-range
  // lanes.
  case ARM_INS_VQMOVN:
  case ARM_INS_VQMOVNB:
  case ARM_INS_VQMOVNT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[ARM.op_count - 1]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    bool IsSigned = LI.IsSigned;
    unsigned SrcLaneSz = LI.LaneSz;
    if (SrcLaneSz >= 2 && Src.Size > SrcLaneSz) {
      unsigned DstLaneSz = SrcLaneSz / 2;
      unsigned NLanes = Src.Size / SrcLaneSz;
      unsigned DstBits = DstLaneSz * 8;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Wide = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, Wide, {Src, NdVar::cst(I * SrcLaneSz, 4)});
        // Narrowing saturate: trunc to DstLaneSz, extend back, compare with
        // original.  If equal the value fits; otherwise pick max/min based on
        // sign.  Avoids the fork's InstCombine mis-fold on INT_SLESS+SELECT
        // and @llvm.smax/@llvm.smin clamp chains.
        NdVar Narrow = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SUBBYTES, Narrow, {Wide, NdVar::cst(0, 4)});
        NdVar BackWide = S.makeTemp(SrcLaneSz);
        if (IsSigned)
          S.emit(NdOp::INT_SEXT, BackWide, {Narrow});
        else
          S.emit(NdOp::INT_ZEXT, BackWide, {Narrow});
        NdVar Fits = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, Fits, {Wide, BackWide});
        int64_t MaxV = IsSigned ? (1LL << (DstBits - 1)) - 1
                                : ((DstBits >= 64) ? (int64_t)~0ULL
                                                   : (1LL << DstBits) - 1);
        int64_t MinV = IsSigned ? -(1LL << (DstBits - 1)) : 0;
        NdVar IsPos = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, IsPos,
               {NdVar::cst(0, SrcLaneSz), Wide});
        NdVar OverflowVal = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SELECT, OverflowVal,
               {IsPos, NdVar::cst((uint64_t)MaxV, DstLaneSz),
                NdVar::cst((uint64_t)MinV, DstLaneSz)});
        NdVar NarrowResult = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SELECT, NarrowResult, {Fits, Narrow, OverflowVal});
        if (I == 0)
          Acc = NarrowResult;
        else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {NarrowResult, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VQADD:
  case ARM_INS_VQSUB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    // capstone may leave vector_data INVALID -> recover from the mnemonic.
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    bool IsSigned = LI.IsSigned;
    bool IsAdd = (Insn->id == ARM_INS_VQADD);
    unsigned LaneSz = LI.LaneSz;
    if (LaneSz > 0 && LaneSz <= 4 && Dst.Size > LaneSz) {
      unsigned WideSz = LaneSz * 2;
      unsigned NLanes = Dst.Size / LaneSz;
      unsigned Bits = LaneSz * 8;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Wa = S.makeTemp(WideSz);
        NdVar Wb = S.makeTemp(WideSz);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Wa, {La});
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Wb, {Lb});
        NdVar Wide = S.makeTemp(WideSz);
        S.emit(IsAdd ? NdOp::INT_ADD : NdOp::INT_SUB, Wide, {Wa, Wb});
        // Saturate the wide result to the lane's range before truncating.
        NdVar Clamped = S.makeTemp(LaneSz);
        if (IsSigned) {
          int64_t MaxV = (1LL << (Bits - 1)) - 1;
          int64_t MinV = -(1LL << (Bits - 1));
          NdVar TooHi = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, TooHi,
                 {NdVar::cst((uint64_t)MaxV, WideSz), Wide});
          NdVar C1 = S.makeTemp(WideSz);
          S.emit(NdOp::SELECT, C1,
                 {TooHi, NdVar::cst((uint64_t)MaxV, WideSz), Wide});
          NdVar TooLo = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, TooLo,
                 {C1, NdVar::cst((uint64_t)MinV, WideSz)});
          NdVar C2 = S.makeTemp(WideSz);
          S.emit(NdOp::SELECT, C2,
                 {TooLo, NdVar::cst((uint64_t)MinV, WideSz), C1});
          S.emit(NdOp::SUBBYTES, Clamped, {C2, NdVar::cst(0, 4)});
        } else {
          uint64_t MaxV = (1ULL << Bits) - 1;
          NdVar C1 = S.makeTemp(WideSz);
          if (IsAdd) {
            NdVar TooHi = S.makeTemp(1);
            S.emit(NdOp::INT_LESS, TooHi,
                   {NdVar::cst(MaxV, WideSz), Wide});
            S.emit(NdOp::SELECT, C1,
                   {TooHi, NdVar::cst(MaxV, WideSz), Wide});
          } else {
            // Detect unsigned underflow from the widened operands.  Testing
            // the wrapped subtraction result after an upper clamp loses the
            // sign and incorrectly saturates underflow to MaxV instead of 0.
            NdVar Underflow = S.makeTemp(1);
            S.emit(NdOp::INT_LESS, Underflow, {Wa, Wb});
            S.emit(NdOp::SELECT, C1,
                   {Underflow, NdVar::cst(0, WideSz), Wide});
          }
          S.emit(NdOp::SUBBYTES, Clamped, {C1, NdVar::cst(0, 4)});
        }
        if (I == 0)
          Acc = Clamped;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Clamped, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emitIntrinsic(IsAdd ? Intrinsic::ArmVqadd : Intrinsic::ArmVqsub, Dst,
                      {A, B});
    }
    break;
  }
  // VQMOVUN narrows each *signed* wide lane to an *unsigned* half-width lane
  // with saturation (negatives clamp to 0, large values clamp to the unsigned
  // max). The old handler emitted an unhandled intrinsic that silently returned
  // 0.
  case ARM_INS_VQMOVUN:
  case ARM_INS_VQMOVUNB:
  case ARM_INS_VQMOVUNT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[ARM.op_count - 1]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcLaneSz = LI.LaneSz;
    if (SrcLaneSz >= 2 && Src.Size > SrcLaneSz) {
      unsigned DstLaneSz = SrcLaneSz / 2;
      unsigned NLanes = Src.Size / SrcLaneSz;
      unsigned DstBits = DstLaneSz * 8;
      uint64_t MaxV = (DstBits >= 64) ? ~0ULL : ((1ULL << DstBits) - 1);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Wide = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, Wide, {Src, NdVar::cst(I * SrcLaneSz, 4)});
        NdVar TooHi = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, TooHi, {NdVar::cst(MaxV, SrcLaneSz), Wide});
        NdVar C1 = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SELECT, C1, {TooHi, NdVar::cst(MaxV, SrcLaneSz), Wide});
        NdVar Neg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, Neg, {C1, NdVar::cst(0, SrcLaneSz)});
        NdVar C2 = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SELECT, C2, {Neg, NdVar::cst(0, SrcLaneSz), C1});
        NdVar Narrow = S.makeTemp(DstLaneSz);
        S.emit(NdOp::SUBBYTES, Narrow, {C2, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Narrow;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
    }
    break;
  }
  // Saturating doubling multiply (high half VQDMULH/VQRDMULH; widening
  // VQDMULL). Map to the ARM NEON intrinsic; was a plain full-width INT_MULT
  // placeholder. The MVE bottom/top widening forms keep the fallback.
  case ARM_INS_VQDMULH:
  case ARM_INS_VQRDMULH:
  case ARM_INS_VQDMULL:
  case ARM_INS_VQDMULLB:
  case ARM_INS_VQDMULLT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned SrcElem = LI.LaneSz;
    bool MveBT = (Insn->id == ARM_INS_VQDMULLB || Insn->id == ARM_INS_VQDMULLT);
    if (SrcElem == 0 || MveBT) {
      S.emit(NdOp::INT_MULT, Dst, {A, B});
      break;
    }
    bool Widen = (Insn->id == ARM_INS_VQDMULL);
    unsigned DstElem = Widen ? SrcElem * 2 : SrcElem;
    // By-scalar `vqdmulh.s16 q,q,d[idx]` / `vqdmull.s16 q,d,d[idx]`:
    // operandRead ignores vector_index, so broadcast the selected Dm lane
    // across A.Size before the per-lane intrinsic (otherwise it multiplied
    // lane-by-lane).
    int BLane = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                               : ARM.operands[2].vector_index;
    if (BLane >= 0 && A.Size >= SrcElem) {
      NdVar Lane = S.makeTemp(SrcElem);
      S.emit(NdOp::SUBBYTES, Lane,
             {B, NdVar::cst(static_cast<uint64_t>(BLane) * SrcElem, 4)});
      NdVar Bcast = Lane;
      for (unsigned I = 1; I < A.Size / SrcElem; ++I) {
        NdVar Next = S.makeTemp(Bcast.Size + SrcElem);
        S.emit(NdOp::CONCAT, Next, {Lane, Bcast});
        Bcast = Next;
      }
      B = Bcast;
    }
    Intrinsic II = (Insn->id == ARM_INS_VQDMULH)    ? Intrinsic::ArmVqdmulh
                   : (Insn->id == ARM_INS_VQRDMULH) ? Intrinsic::ArmVqrdmulh
                                                    : Intrinsic::ArmVqdmull;
    S.emitIntrinsic(II, Dst, {A, B, NdVar::cst(DstElem, 4)});
    break;
  }
  // VQDMLAL/VQDMLSL — signed saturating doubling multiply-accumulate long:
  //   Vd[i] = SignedSat(Vd[i] +/- SignedSat(2 * Vn[i] * Vm[i]))
  // Narrow signed source lanes (s16/s32) widen to double-width dest lanes; the
  // doubled product saturates, then the accumulate saturates.  Was a bare
  // full-width INT_MULT + INT_ADD placeholder (no widening/doubling/saturation,
  // and VQDMLSL even accumulated with INT_ADD).  Mirrors AArch64
  // SQDMLAL/SQDMLSL.
  case ARM_INS_VQDMLAL:
  case ARM_INS_VQDMLSL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSub = (Insn->id == ARM_INS_VQDMLSL);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned InSz = LI.LaneSz;
    if (InSz == 0 || InSz > 4 || Dst.Size <= InSz) {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {A, B});
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Dst, {OldDst, Prod});
      break;
    }
    unsigned DstLane = InSz * 2;
    unsigned NLanes = Dst.Size / DstLane;
    // Indexed form `vqdmlal.s16 q,d,d[idx]` broadcasts one B lane; operandRead
    // returns the whole Dm, so detect the lane index (same fix as #386 VMLA).
    int BLane = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                               : ARM.operands[2].vector_index;
    // Signed saturating add/sub at the dest lane width: overflow is detected
    // from operand/result signs and the lane forced to MIN/MAX (no i128
    // needed).
    auto satAddSub = [&](NdVar AVal, NdVar BVal, unsigned W,
                         bool Sub) -> NdVar {
      unsigned WBits = W * 8;
      uint64_t MaxV =
          (WBits >= 64) ? 0x7FFFFFFFFFFFFFFFULL : ((1ULL << (WBits - 1)) - 1);
      uint64_t MinV =
          (WBits >= 64) ? 0x8000000000000000ULL : (1ULL << (WBits - 1));
      NdVar Res = S.makeTemp(W);
      S.emit(Sub ? NdOp::INT_SUB : NdOp::INT_ADD, Res, {AVal, BVal});
      NdVar ANeg = S.makeTemp(1), BNeg = S.makeTemp(1), RNeg = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, ANeg, {AVal, NdVar::cst(0, W)});
      S.emit(NdOp::INT_SLESS, BNeg, {BVal, NdVar::cst(0, W)});
      S.emit(NdOp::INT_SLESS, RNeg, {Res, NdVar::cst(0, W)});
      NdVar SignCond = S.makeTemp(1);
      if (Sub)
        S.emit(NdOp::BOOL_XOR, SignCond, {ANeg, BNeg});
      else
        S.emit(NdOp::INT_EQUAL, SignCond, {ANeg, BNeg});
      NdVar SignFlip = S.makeTemp(1);
      S.emit(NdOp::BOOL_XOR, SignFlip, {ANeg, RNeg});
      NdVar Ovf = S.makeTemp(1);
      S.emit(NdOp::INT_AND, Ovf, {SignCond, SignFlip});
      NdVar SatVal = S.makeTemp(W);
      S.emit(NdOp::SELECT, SatVal,
             {ANeg, NdVar::cst(MinV, W), NdVar::cst(MaxV, W)});
      NdVar Out = S.makeTemp(W);
      S.emit(NdOp::SELECT, Out, {Ovf, SatVal, Res});
      return Out;
    };
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar NarrA = S.makeTemp(InSz);
      S.emit(NdOp::SUBBYTES, NarrA, {A, NdVar::cst(I * InSz, 4)});
      NdVar NarrB = S.makeTemp(InSz);
      uint64_t BOff = (BLane >= 0) ? static_cast<uint64_t>(BLane) * InSz
                                   : static_cast<uint64_t>(I) * InSz;
      S.emit(NdOp::SUBBYTES, NarrB, {B, NdVar::cst(BOff, 4)});
      NdVar WA = S.makeTemp(DstLane), WB = S.makeTemp(DstLane);
      S.emit(NdOp::INT_SEXT, WA, {NarrA});
      S.emit(NdOp::INT_SEXT, WB, {NarrB});
      NdVar Prod = S.makeTemp(DstLane);
      S.emit(NdOp::INT_MULT, Prod, {WA, WB});
      if (DstLane <= 4) {
        // Double in a 2x-wide temp and clamp to the dest signed range;
        // saturates only when both narrow lanes are INT_MIN (2*MIN*MIN > MAX).
        unsigned Wide = DstLane * 2, Bits = DstLane * 8;
        int64_t MaxV = (1LL << (Bits - 1)) - 1, MinV = -(1LL << (Bits - 1));
        NdVar P2 = S.makeTemp(Wide);
        S.emit(NdOp::INT_SEXT, P2, {Prod});
        NdVar Dbl = S.makeTemp(Wide);
        S.emit(NdOp::INT_LEFT, Dbl, {P2, NdVar::cst(1, Wide)});
        NdVar TooHi = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, TooHi,
               {NdVar::cst((uint64_t)MaxV, Wide), Dbl});
        NdVar C1 = S.makeTemp(Wide);
        S.emit(NdOp::SELECT, C1,
               {TooHi, NdVar::cst((uint64_t)MaxV, Wide), Dbl});
        NdVar TooLo = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, TooLo,
               {C1, NdVar::cst((uint64_t)MinV, Wide)});
        NdVar C2 = S.makeTemp(Wide);
        S.emit(NdOp::SELECT, C2,
               {TooLo, NdVar::cst((uint64_t)MinV, Wide), C1});
        NdVar Narrowed = S.makeTemp(DstLane);
        S.emit(NdOp::SUBBYTES, Narrowed, {C2, NdVar::cst(0, 4)});
        Prod = Narrowed;
      } else {
        // 64-bit dest: 2*MIN32*MIN32 == 2^63 overflows i64; saturate that lane
        // to INT64_MAX (an i128 clamp cannot represent the bound).
        NdVar Dbl = S.makeTemp(DstLane);
        S.emit(NdOp::INT_LEFT, Dbl, {Prod, NdVar::cst(1, DstLane)});
        unsigned NBits = InSz * 8;
        uint64_t NMin = 1ULL << (NBits - 1);
        NdVar AMin = S.makeTemp(1), BMin = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, AMin, {NarrA, NdVar::cst(NMin, InSz)});
        S.emit(NdOp::INT_EQUAL, BMin, {NarrB, NdVar::cst(NMin, InSz)});
        NdVar Both = S.makeTemp(1);
        S.emit(NdOp::INT_AND, Both, {AMin, BMin});
        NdVar Sat = S.makeTemp(DstLane);
        S.emit(NdOp::SELECT, Sat,
               {Both, NdVar::cst(0x7FFFFFFFFFFFFFFFULL, DstLane), Dbl});
        Prod = Sat;
      }
      NdVar LaneDst = S.makeTemp(DstLane);
      S.emit(NdOp::SUBBYTES, LaneDst, {OldDst, NdVar::cst(I * DstLane, 4)});
      NdVar Lr = satAddSub(LaneDst, Prod, DstLane, IsSub);
      if (I == 0) {
        Acc = Lr;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + DstLane);
        S.emit(NdOp::CONCAT, Next, {Lr, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_VQRDMLAH:
  case ARM_INS_VQRDMLASH:
  case ARM_INS_VQRDMLSH:
  case ARM_INS_VQDMLADH:
  case ARM_INS_VQDMLADHX:
  case ARM_INS_VQDMLAH:
  case ARM_INS_VQDMLASH:
  case ARM_INS_VQDMLSDH:
  case ARM_INS_VQDMLSDHX:
  case ARM_INS_VQRDMLADH:
  case ARM_INS_VQRDMLADHX:
  case ARM_INS_VQRDMLSDH:
  case ARM_INS_VQRDMLSDHX: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {NdVar::reg(Dst.Offset, Dst.Size), Prod});
    break;
  }

  // NEON pairwise min/max: d[i]=op(a[2i],a[2i+1]), d[N/2+i]=op(b[2i],b[2i+1])
  case ARM_INS_VPMAX:
  case ARM_INS_VPMIN: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    bool IsMin = (Insn->id == ARM_INS_VPMIN);
    if (LI.LaneSz > 0 && A.Size >= 2 * LI.LaneSz) {
      unsigned NPairs = A.Size / LI.LaneSz / 2;
      NdOp CmpOp = LI.IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
      // VPMIN/VPMAX float lanes are IEEE minimum/maximum (NaN-propagating,
      // -0 < +0); a FLOAT_LESS+SELECT got NaN and signed zeros wrong.
      NdOp FMM = IsMin ? NdOp::FLOAT_MIN : NdOp::FLOAT_MAX;
      NdVar Acc = NdVar::cst(0, 0);
      auto doCmp = [&](const NdVar &Src, unsigned SetIdx) {
        for (unsigned P = 0; P < NPairs; ++P) {
          NdVar Lo = S.makeTemp(LI.LaneSz);
          S.emit(
              NdOp::SUBBYTES, Lo,
              {Src, NdVar::cst(static_cast<uint64_t>(P) * 2 * LI.LaneSz, 4)});
          NdVar Hi = S.makeTemp(LI.LaneSz);
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(
                           (static_cast<uint64_t>(P) * 2 + 1) * LI.LaneSz, 4)});
          NdVar Sel = S.makeTemp(LI.LaneSz);
          if (LI.IsFloat) {
            S.emit(FMM, Sel, {Lo, Hi});
          } else {
            NdVar Cond = S.makeTemp(1);
            if (IsMin)
              S.emit(CmpOp, Cond, {Lo, Hi});
            else
              S.emit(CmpOp, Cond, {Hi, Lo});
            S.emit(NdOp::SELECT, Sel, {Cond, Lo, Hi});
          }
          if (SetIdx == 0 && P == 0) {
            Acc = Sel;
          } else {
            NdVar Prev = S.makeTemp(Acc.Size + LI.LaneSz);
            S.emit(NdOp::CONCAT, Prev, {Sel, Acc});
            Acc = Prev;
          }
        }
      };
      doCmp(A, 0);
      doCmp(B, 1);
      if (Acc.Size == Dst.Size)
        S.emit(NdOp::COPY, Dst, {Acc});
      else if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::SUBBYTES, Dst, {Acc, NdVar::cst(0, 4)});
    } else {
      S.emit(NdOp::COPY, Dst, {A});
    }
    break;
  }

  // NEON reduction
  case ARM_INS_VMAXAV:
  case ARM_INS_VMAXNMAV:
  case ARM_INS_VMAXNMV:
  case ARM_INS_VMAXV:
  case ARM_INS_VMINAV:
  case ARM_INS_VMINNMAV:
  case ARM_INS_VMINNMV:
  case ARM_INS_VMINV:
  case ARM_INS_VADDV:
  case ARM_INS_VADDVA:
  case ARM_INS_VADDLV:
  case ARM_INS_VADDLVA:
  case ARM_INS_VMLADAV:
  case ARM_INS_VMLADAVA:
  case ARM_INS_VMLADAVAX:
  case ARM_INS_VMLADAVX:
  case ARM_INS_VMLALDAV:
  case ARM_INS_VMLALDAVA:
  case ARM_INS_VMLALDAVAX:
  case ARM_INS_VMLALDAVX:
  case ARM_INS_VMLSDAV:
  case ARM_INS_VMLSDAVA:
  case ARM_INS_VMLSDAVAX:
  case ARM_INS_VMLSDAVX:
  case ARM_INS_VMLSLDAV:
  case ARM_INS_VMLSLDAVA:
  case ARM_INS_VMLSLDAVAX:
  case ARM_INS_VMLSLDAVX:
  case ARM_INS_VRMLALDAVH:
  case ARM_INS_VRMLALDAVHA:
  case ARM_INS_VRMLALDAVHAX:
  case ARM_INS_VRMLALDAVHX:
  case ARM_INS_VRMLSLDAVH:
  case ARM_INS_VRMLSLDAVHA:
  case ARM_INS_VRMLSLDAVHAX:
  case ARM_INS_VRMLSLDAVHX: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[ARM.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // MVE predicate
  case ARM_INS_VPST:
    S.emitIntrinsic(Intrinsic::ArmVpst);
    break;
  case ARM_INS_VPT:
    S.emitIntrinsic(Intrinsic::ArmVpt);
    break;
  case ARM_INS_VPSEL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    S.emit(NdOp::COPY, Dst, {A});
    break;
  }
  case ARM_INS_VCMUL:
  // VCMLA — rotated complex floating-point multiply-accumulate (AArch32
  // FEAT_FCMA).  Same #rot table as AArch64 FCMLA; the old code was a whole-
  // register INT_MULT+INT_ADD (integer ops on FP, no rotation, no per-lane).
  case ARM_INS_VCMLA: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Vn = operandRead(S, ARM.operands[1]);
    NdVar Vm = operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    int64_t Rot = ARM.operands[3].imm;
    int VecIdx = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                                : ARM.operands[2].vector_index;
    auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
    unsigned ES = LI.LaneSz;
    if (!LI.IsFloat || (ES != 4 && ES != 8) || Dst.Size < 2 * ES) {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {Vn, Vm});
      S.emit(NdOp::INT_ADD, Dst, {OldDst, Prod});
      break;
    }
    bool UseVnIm = (Rot == 90 || Rot == 270);
    bool ReUseVmIm = (Rot == 90 || Rot == 270);
    bool ImUseVmIm = (Rot == 0 || Rot == 180);
    bool ReSub = (Rot == 90 || Rot == 180);
    bool ImSub = (Rot == 180 || Rot == 270);
    auto lane = [&](NdVar V, unsigned Idx) {
      NdVar T = S.makeTemp(ES);
      S.emit(NdOp::SUBBYTES, T,
             {V, NdVar::cst(static_cast<uint64_t>(Idx) * ES, 4)});
      return T;
    };
    NdVar Acc = NdVar::cst(0, 0);
    bool First = true;
    auto append = [&](NdVar L) {
      if (First) {
        Acc = L;
        First = false;
        return;
      }
      NdVar N = S.makeTemp(Acc.Size + ES);
      S.emit(NdOp::CONCAT, N, {L, Acc});
      Acc = N;
    };
    unsigned NLanes = Dst.Size / ES;
    for (unsigned K = 0; K < NLanes / 2; ++K) {
      unsigned BPair = (VecIdx >= 0) ? (unsigned)VecIdx : K;
      NdVar VnEl = lane(Vn, 2 * K + (UseVnIm ? 1 : 0));
      NdVar VmRe = lane(Vm, 2 * BPair);
      NdVar VmIm = lane(Vm, 2 * BPair + 1);
      NdVar AccRe = lane(OldDst, 2 * K);
      NdVar AccIm = lane(OldDst, 2 * K + 1);
      NdVar PRe = S.makeTemp(ES), PIm = S.makeTemp(ES);
      S.emit(NdOp::FLOAT_MULT, PRe, {VnEl, ReUseVmIm ? VmIm : VmRe});
      S.emit(NdOp::FLOAT_MULT, PIm, {VnEl, ImUseVmIm ? VmIm : VmRe});
      NdVar OutRe = S.makeTemp(ES), OutIm = S.makeTemp(ES);
      S.emit(ReSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD, OutRe, {AccRe, PRe});
      S.emit(ImSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD, OutIm, {AccIm, PIm});
      append(OutRe);
      append(OutIm);
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_VBRSR: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }
  case ARM_INS_VDDUP:
  case ARM_INS_VDWDUP:
  case ARM_INS_VIDUP:
  case ARM_INS_VIWDUP: {
    if (ARM.op_count >= 2) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Src = operandRead(S, ARM.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VADC:
  case ARM_INS_VADCI: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    S.emit(NdOp::INT_ADD, Dst, {A, B});
    break;
  }
  case ARM_INS_VSBC:
  case ARM_INS_VSBCI: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    S.emit(NdOp::INT_SUB, Dst, {A, B});
    break;
  }
  // VSDOT/VUDOT — byte dot-product accumulate: each 32-bit lane adds the sum of
  // four byte products (signed/unsigned).  The indexed form broadcasts one
  // 4-byte group of B.  Was a full-width INT_MULT+INT_ADD placeholder (whole
  // register as one integer — no per-lane reduction, no byte widening).
  case ARM_INS_VSDOT:
  case ARM_INS_VUDOT: {
    if (ARM.op_count < 3)
      break;
    bool IsSigned = (Insn->id == ARM_INS_VSDOT);
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    unsigned NLanes = Dst.Size / 4;
    int VecIdx = ARM.operands[2].neon_lane >= 0 ? ARM.operands[2].neon_lane
                                                : ARM.operands[2].vector_index;
    auto ExtOp = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
    NdVar Acc = NdVar::cst(0, 0);
    for (unsigned I = 0; I < NLanes; ++I) {
      unsigned BBase = (VecIdx >= 0 ? (unsigned)VecIdx : I) * 4;
      NdVar Lane = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Lane, {OldDst, NdVar::cst(I * 4, 4)});
      for (unsigned K = 0; K < 4; ++K) {
        NdVar Ba = S.makeTemp(1), Bb = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, Ba, {A, NdVar::cst(I * 4 + K, 4)});
        S.emit(NdOp::SUBBYTES, Bb, {B, NdVar::cst(BBase + K, 4)});
        NdVar Ea = S.makeTemp(4), Eb = S.makeTemp(4);
        S.emit(ExtOp, Ea, {Ba});
        S.emit(ExtOp, Eb, {Bb});
        NdVar Pr = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, Pr, {Ea, Eb});
        NdVar Nl = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Nl, {Lane, Pr});
        Lane = Nl;
      }
      if (I == 0) {
        Acc = Lane;
      } else {
        NdVar P = S.makeTemp(Acc.Size + 4);
        S.emit(NdOp::CONCAT, P, {Lane, Acc});
        Acc = P;
      }
    }
    if (Acc.Size < Dst.Size)
      S.emit(NdOp::INT_ZEXT, Dst, {Acc});
    else
      S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // VUSDOT/VSUDOT (mixed sign, i8mm) and VDOT (alias) — keep the simple
  // placeholder; not validated against Unicorn.
  case ARM_INS_VUSDOT:
  case ARM_INS_VSUDOT:
  case ARM_INS_VDOT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {NdVar::reg(Dst.Offset, Dst.Size), Prod});
    break;
  }
  case ARM_INS_VMMLA:
  case ARM_INS_VSMMLA:
  case ARM_INS_VUMMLA:
  case ARM_INS_VUSMMLA: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {NdVar::reg(Dst.Offset, Dst.Size), Prod});
    break;
  }
  case ARM_INS_VCX1:
    S.emitIntrinsic(Intrinsic::ArmVcx1);
    break;
  case ARM_INS_VCX1A:
    S.emitIntrinsic(Intrinsic::ArmVcx1a);
    break;
  case ARM_INS_VCX2:
    S.emitIntrinsic(Intrinsic::ArmVcx2);
    break;
  case ARM_INS_VCX2A:
    S.emitIntrinsic(Intrinsic::ArmVcx2a);
    break;
  case ARM_INS_VCX3:
    S.emitIntrinsic(Intrinsic::ArmVcx3);
    break;
  case ARM_INS_VCX3A:
    S.emitIntrinsic(Intrinsic::ArmVcx3a);
    break;

  case ARM_INS___BRKDIV0:
    S.emitIntrinsic(Intrinsic::ArmBkpt);
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
