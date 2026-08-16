//===- PipelineStructReturn.cpp - Aggregate return recovery --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Whole-module recovery of multi-register struct, HFA, and wide-vector
/// returns.
///
//===----------------------------------------------------------------------===//

#include "PipelineReturnModelingDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include <algorithm>
#include <map>
#include <vector>

namespace neverd {

// A small struct returned by value across multiple registers (x86-64 SysV
// eightbytes / AArch64 HFA): the caller-side remodel (modelCallStructReturn,
// run during low->med) rewrote each such direct call to produce a flat
// aggregate temp it SUBBYTES into the field return registers.  Read that
// remodeling back to learn each callee's multi-register return shape, then
// re-type the callee so its RETURN emits the matching LLVM aggregate and the
// backend's ABI lowering places the fields in the right registers.
void recoverStructReturnFromCallers(const BinaryImage &Img,
                                    PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (TRI.PointerSize == 8) {
    std::map<va_t, std::vector<MedReturnReg>> StructRetCallees;
    for (const auto &MF : Result.MedFuncs)
      for (const auto &Blk : MF.Blocks)
        for (size_t I = 0; I < Blk.Ops.size(); ++I) {
          const auto &Op = Blk.Ops[I];
          if (Op.Opcode != NdOp::CALL || Op.Output.Kind != MedVar::Temp ||
              Op.NumInputs < 1 || !Op.Inputs[0].isConst())
            continue;
          std::vector<MedReturnReg> Desc;
          for (size_t J = I + 1; J < Blk.Ops.size(); ++J) {
            const auto &Ex = Blk.Ops[J];
            if (Ex.Opcode != NdOp::SUBBYTES || Ex.NumInputs < 1 ||
                Ex.Inputs[0].Kind != MedVar::Temp ||
                Ex.Inputs[0].Id != Op.Output.Id ||
                Ex.Output.Kind != MedVar::Reg)
              break;
            MedReturnReg RR;
            RR.RegOff = Ex.Output.RegOff;
            RR.Size = Ex.Output.Size;
            RR.IsFP = TRI.isVectorReg(Ex.Output.RegOff);
            Desc.push_back(RR);
          }
          if (Desc.size() >= 2)
            StructRetCallees[Op.Inputs[0].ConstVal] = Desc;
        }
    if (!StructRetCallees.empty())
      for (auto &MF : Result.MedFuncs)
        if (auto It = StructRetCallees.find(MF.Entry);
            It != StructRetCallees.end())
          MF.MultiReturn = It->second;
  }
}

// Struct-return tail-call forwarder shape propagation: `struct S
// f(args){return g(args);}` lowers at -O2 to a lone `b g` (a tail call), so
// f's RETURN forwards exactly g's return value -- f and g have the SAME
// return shape.  The caller-side remodel above learns f's shape from f's own
// direct callers (`main` extracts f's fields), but g -- reached only through
// f's tail call -- is never the target of a struct-return call site, so its
// shape is still unknown here. Propagate f's proven multi-register shape
// FORWARD into such a callee.  Done BEFORE the body-based recovery below and
// only into a callee whose shape is still unknown, so (a) a directly-called
// callee keeps its own caller-proven shape, and (b) a forwarder-reached
// callee takes its forwarder's exact shape rather than the heuristic body
// scan (which can miss a field -- e.g. recover only 2 of a 3-double HFA, or 0
// of a `{x, 2x}` pair whose first field is the passed-through argument).
// Iterated to a fixpoint so a forwarder chain (f -> mid -> g) propagates all
// the way down.  64-bit register struct returns only (the caller-side remodel
// is gated to PointerSize==8).
void propagateStructReturnForwarderShapes(const BinaryImage &Img,
                                          PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (TRI.PointerSize == 8) {
    auto IsCompatibleSubset = [](const std::vector<MedReturnReg> &Subset,
                                 const std::vector<MedReturnReg> &Shape) {
      if (Subset.size() >= Shape.size())
        return false;
      size_t ShapeIdx = 0;
      for (const MedReturnReg &Field : Subset) {
        while (ShapeIdx < Shape.size() &&
               (Field.RegOff != Shape[ShapeIdx].RegOff ||
                Field.Size != Shape[ShapeIdx].Size ||
                Field.IsFP != Shape[ShapeIdx].IsFP))
          ++ShapeIdx;
        if (ShapeIdx == Shape.size())
          return false;
        ++ShapeIdx;
      }
      return true;
    };
    std::map<va_t, MedFunc *> ByEntry;
    for (auto &MF : Result.MedFuncs)
      ByEntry[MF.Entry] = &MF;
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (auto &MF : Result.MedFuncs) {
        if (MF.MultiReturn.size() < 2)
          continue;
        for (const auto &Blk : MF.Blocks) {
          bool Found = false;
          for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
            const auto &Op = Blk.Ops[I];
            if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
                !Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Reg)
              continue;
            const auto &Ret = Blk.Ops[I + 1];
            if (Ret.Opcode != NdOp::RETURN || Ret.NumInputs < 1 ||
                Ret.Inputs[0].Kind != MedVar::Reg ||
                Ret.Inputs[0].RegOff != Op.Output.RegOff)
              continue;
            auto It = ByEntry.find(Op.Inputs[0].ConstVal);
            if (It != ByEntry.end() &&
                (It->second->MultiReturn.empty() ||
                 IsCompatibleSubset(It->second->MultiReturn, MF.MultiReturn))) {
              It->second->MultiReturn = MF.MultiReturn;
              Changed = true;
            }
            Found = true;
            break;
          }
          if (Found)
            break;
        }
      }
    }
  }
}

// A function reached ONLY through a function pointer is never the target of a
// direct CALL, so the caller-side struct-return remodel above never learns
// its multi-register return shape (an indirect call site cannot name its
// callee). Recover it from the callee's own body instead: clang's by-value
// small-struct return loads each field into its return register straight-line
// before RETURN
// (`ldr x0,[..]; ldr x1,[..]; ret` for a GP struct; `ldr d0; ldr d1; ret` for
// a 2-double HFA).  A candidate return register (x0:x1, or the HFA v0..v3) is
// a genuine return field when, before a RETURN with no intervening call, it
// is written by a value op (not a live-in self-copy) whose result is LIVE-OUT
// -- not read again before the RETURN.  The live-out test is what separates a
// real field from an FP scratch register: a `double f(double a,double
// b){return a*b;}` reloads b into d1 and then CONSUMES it in the multiply (d1
// is read after its last write), so d1 is not a field; a 2-double HFA leaves
// d0 and d1 untouched after loading them, so both are fields.  AArch64
// returns a small aggregate either all-GP or all-HFA, never mixed (a mixed
// live-out set is a scalar FP return plus the lifter's dead integer
// placeholder), so a mixed set is rejected.  declareFunc then emits the
// aggregate return ({i64,i64} / {double,double} / {float,float}) and the
// indirect call site (modeled by modelCallStructReturn for INDIR_CALL) reads
// every field.  Gated to AArch64 (the x86-64 RAX/RDX overlap with div/mul
// byproducts is handled on the direct-call path's redefine tracking) and to
// functions WITHOUT an already recovered MultiReturn (a directly-called
// struct returner keeps the proven Pipeline- propagated shape).
void recoverStructReturnFromBody(const BinaryImage &Img,
                                 PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  struct RetCand {
    uint64_t RegOff;
    bool IsFP;
  };
  std::vector<RetCand> Cands;
  for (uint64_t R : TRI.IntReturnRegs)
    Cands.push_back({R, false});
  for (uint64_t R : TRI.FPReturnRegs)
    Cands.push_back({R, true});
  if (Img.Arch == Arch::AArch64 && Cands.size() >= 2) {
    for (auto &MF : Result.MedFuncs) {
      if (!MF.MultiReturn.empty())
        continue;

      // A single 128-bit vector returned by value in V0 (a NEON `int32x4` /
      // `float __attribute__((vector_size(16)))`): the callee assembles the
      // whole 16-byte value into V0 with REAL data in the high 64 bits and
      // returns it, unlike a 2..4 element HFA which puts each element in its
      // own D/S register.  The HFA recovery below would pair the wide V0 with
      // a stale V1 (often a reloaded argument register) into a bogus
      // {double,double}, silently corrupting lanes 2-3.  The discriminator is
      // the high-64 content, NOT the bare write width: the lifter models
      // every `ldr d0` (an 8-byte HFA element) as `INT_ZEXT Q0 <- d` -- also
      // a 16-byte V0 write, but a zero-extension whose high 64 bits are 0.  A
      // genuine vector instead reaches V0 through a CONCAT lane-assembly /
      // 16-byte LOAD / NEON op (real high lanes).  Detect the latter before a
      // RETURN with no genuine GP-return-register write (a pure FP/vector
      // return, so not an __int128 returned in X0:X1 that clang stages
      // through V0 at the call site) and type the function as a 16-byte
      // vector return; the emitter's fpAbiType lowers it to a <2 x i64>
      // return carrying all 16 bytes (V0 read whole).
      if (!TRI.FPReturnRegs.empty()) {
        const uint64_t V0 = TRI.FPReturnRegs.front();
        std::map<std::pair<int, int>, const MedOp *> Defs;
        for (const auto &Blk : MF.Blocks)
          for (const auto &O : Blk.Ops)
            if (O.Output.Id >= 0)
              Defs[{O.Output.Id, O.Output.SSAVer}] = &O;
        // True iff \p V (traced through COPY chains) is a genuine >=16-byte
        // value -- a real vector -- rather than a zero-extended <=8-byte
        // scalar (an `INT_ZEXT Qn <- Dn` HFA element / scalar FP).
        auto isWideVec = [&](MedVar V) -> bool {
          for (int Depth = 0; Depth < 8; ++Depth) {
            if (V.Kind == MedVar::Const)
              return false;
            auto It = Defs.find({V.Id, V.SSAVer});
            if (It == Defs.end())
              return V.Size >= 16;
            const MedOp *D = It->second;
            if (D->Opcode == NdOp::INT_ZEXT)
              return false; // zero-extended scalar/HFA element, high 64 = 0
            if (D->Opcode == NdOp::COPY && D->NumInputs >= 1) {
              V = D->Inputs[0];
              continue;
            }
            return D->Output.Size >= 16; // CONCAT / LOAD / NEON real 16 bytes
          }
          return false;
        };
        bool WideVecRet = false;
        for (const auto &Blk : MF.Blocks) {
          int RetIdx = -1;
          for (size_t I = 0; I < Blk.Ops.size(); ++I)
            if (Blk.Ops[I].Opcode == NdOp::RETURN) {
              RetIdx = static_cast<int>(I);
              break;
            }
          if (RetIdx < 0)
            continue;
          const MedOp *V0Write = nullptr;
          bool GPWritten = false;
          bool GPLiveFromCall = false;
          for (int J = RetIdx - 1; J >= 0; --J) {
            const auto &O = Blk.Ops[J];
            if (O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL) {
              // The function's live return may be THIS call's own GP result,
              // left in the GP return register and not overwritten before the
              // RETURN -- a `p = malloc(n); /* fill *p with NEON q-register
              // stores */ return p;` tail.  The V0 writes after the call are
              // then dead memcpy scratch (the loaded init data on its way to
              // memory), not a vector return; without this the pointer return
              // is mistyped <2 x i64> and the caller dereferences garbage.
              if (!GPWritten && O.Output.Kind == MedVar::Reg)
                for (uint64_t IR : TRI.IntReturnRegs)
                  if (O.Output.RegOff == IR)
                    GPLiveFromCall = true;
              break;
            }
            if (O.Output.Kind != MedVar::Reg || O.Output.Size == 0)
              continue;
            bool SelfCopy = O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
                            O.Inputs[0].Kind == MedVar::Reg &&
                            O.Inputs[0].RegOff == O.Output.RegOff;
            if (SelfCopy)
              continue;
            if (O.Output.RegOff == V0 && !V0Write)
              V0Write = &O;
            for (uint64_t IR : TRI.IntReturnRegs)
              if (O.Output.RegOff == IR)
                GPWritten = true;
          }
          if (V0Write && !GPWritten && !GPLiveFromCall &&
              isWideVec(V0Write->Output)) {
            WideVecRet = true;
            break;
          }
        }
        if (WideVecRet) {
          MF.ReturnType = NdType::makeFloat(16);
          continue; // single vector return, not a multi-register HFA
        }
      }

      std::vector<MedReturnReg> Fields;
      for (const auto &Blk : MF.Blocks) {
        int RetIdx = -1;
        for (size_t I = 0; I < Blk.Ops.size(); ++I)
          if (Blk.Ops[I].Opcode == NdOp::RETURN) {
            RetIdx = static_cast<int>(I);
            break;
          }
        if (RetIdx < 0)
          continue;
        std::vector<MedReturnReg> BlkFields;
        for (const auto &C : Cands) {
          // Last genuine (non-self-copy) write to this return register before
          // the RETURN, plus its natural element width.  A Q-register zero
          // extension (size 16) is the lifter's normalization of a D/S write,
          // not the field width, so the element size takes the widest write
          // that still fits a single field (<= 8 bytes).
          int WIdx = -1;
          uint16_t ElemSz = 0;
          for (int J = RetIdx - 1; J >= 0; --J) {
            const auto &O = Blk.Ops[J];
            if (O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL)
              break;
            if (O.Output.Kind != MedVar::Reg || O.Output.Size == 0 ||
                O.Output.RegOff != C.RegOff)
              continue;
            bool SelfCopy = O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
                            O.Inputs[0].Kind == MedVar::Reg &&
                            O.Inputs[0].RegOff == O.Output.RegOff;
            if (SelfCopy)
              continue;
            if (WIdx < 0)
              WIdx = J;
            if (O.Output.Size <= 8)
              ElemSz = std::max(ElemSz, O.Output.Size);
          }
          if (WIdx < 0)
            continue;
          // Live-out test: the last write's value must not be read again
          // before the RETURN (an FP scratch multiplicand is read by its
          // consumer).
          bool Consumed = false;
          for (int J = WIdx + 1; J < RetIdx && !Consumed; ++J) {
            const auto &O = Blk.Ops[J];
            // The lifter emits architectural flag calculations after the
            // value-producing instruction.  Those reads describe side effects
            // of the write; they do not consume the register's returned value.
            if (O.Output.Kind == MedVar::Flag && O.Addr == Blk.Ops[WIdx].Addr)
              continue;
            for (uint8_t K = 0; K < O.NumInputs; ++K)
              if (O.Inputs[K] == Blk.Ops[WIdx].Output) {
                Consumed = true;
                break;
              }
          }
          if (Consumed)
            continue;
          MedReturnReg RR;
          RR.RegOff = C.RegOff;
          RR.IsFP = C.IsFP;
          RR.Size = ElemSz ? (ElemSz >= 8 ? 8 : (ElemSz >= 4 ? 4 : ElemSz)) : 8;
          BlkFields.push_back(RR);
        }
        if (BlkFields.size() >= 2) {
          Fields = std::move(BlkFields);
          break;
        }
      }
      if (Fields.size() < 2)
        continue;
      bool AnyInt = false, AnyFP = false;
      for (const auto &F : Fields)
        (F.IsFP ? AnyFP : AnyInt) = true;
      if (AnyInt && AnyFP)
        continue; // AArch64 never mixes GP and FP fields in a register return
      MF.MultiReturn = std::move(Fields);
    }
  }
}

} // namespace neverd
