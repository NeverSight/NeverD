//===- ARMLiftSIMDNEONLoadStore.cpp - ARM32 NEON structure load/store lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The NEON register-list transfers VLD1-VLD4 and VST1-VST4,
/// including the de-interleaving, all-lanes and single-lane forms.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftSIMDNEONLoadStore(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
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
    // operand to L.operandEffAddr(which adds mem.index) made `[r0], r1` load
    // from r0+r1 and shifted every overlapping window by one element.
    auto &MemBR = ARM.operands[MemIdx].mem;
    NdVar Base;
    if (MemBR.base != ARM_REG_INVALID) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(MemBR.base));
      Base = S.makeTemp(4);
      S.emit(NdOp::COPY, Base, {NdVar::reg(RI.Offset, 4)});
    } else {
      Base = L.operandEffAddr(S, ARM.operands[MemIdx]);
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
            S.emit(NdOp::COPY, L.operandWrite(RegOp), {Acc});
          } else {
            NdVar Cur = L.operandRead(S, RegOp);
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
        NdVar Reg = L.operandWrite(RegOp);
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
      NdVar Reg = L.operandWrite(RegOp);
      // `vld1/vst1 {dN[idx]}, [rM]` transfers ONE element, not the whole
      // register.  Previously the indexed form fell into the whole-register
      // path: a `.32` lane load read 8 bytes into dN (clobbering the other
      // lane) instead of 4 bytes into lane idx.
      if (Lane >= 0 && LaneElemSz > 0 && LaneElemSz < Reg.Size) {
        unsigned ByteOff = static_cast<unsigned>(Lane) * LaneElemSz;
        if (ByteOff + LaneElemSz > Reg.Size)
          ByteOff = 0;
        if (IsLoad) {
          NdVar Cur = L.operandRead(S, RegOp);
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
          NdVar Cur = L.operandRead(S, RegOp);
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
        NdVar Src = L.operandRead(S, RegOp);
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

  default:
    return false;
  }
  return true;
}

} // namespace neverd
