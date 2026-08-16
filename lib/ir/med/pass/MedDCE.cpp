//===- MedDCE.cpp - Dead code elimination for MedIR --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Register-level dead code elimination: seeds liveness from returns,
/// calls, and PHI nodes, then propagates backward through the def-use
/// chain to remove provably dead operations.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd {

void LowToMedConverter::runDce(MedFunc &Func) {
  std::set<std::pair<int, int>> LiveDefs;

  std::map<uint64_t, std::vector<std::pair<int, uint16_t>>> RegOffVars;
  for (auto &Blk : Func.Blocks) {
    for (auto &Op : Blk.Ops) {
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Id >= 0)
        RegOffVars[Op.Output.RegOff].push_back({Op.Output.Id, Op.Output.Size});
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        if (Op.Inputs[I].Kind == MedVar::Reg && Op.Inputs[I].Id >= 0)
          RegOffVars[Op.Inputs[I].RegOff].push_back(
              {Op.Inputs[I].Id, Op.Inputs[I].Size});
      }
    }
  }
  for (const MedCallClobber &Clobber : Func.CallClobbers) {
    if (Clobber.Value.Kind == MedVar::Reg && Clobber.Value.Id >= 0)
      RegOffVars[Clobber.Value.RegOff].push_back(
          {Clobber.Value.Id, Clobber.Value.Size});
    if (Clobber.PreservedPrefixSize > 0 &&
        Clobber.PreservedInput.Kind == MedVar::Reg &&
        Clobber.PreservedInput.Id >= 0)
      RegOffVars[Clobber.PreservedInput.RegOff].push_back(
          {Clobber.PreservedInput.Id, Clobber.PreservedInput.Size});
  }

  auto MarkLive = [&](const MedVar &V) {
    if (V.Id >= 0) {
      LiveDefs.insert({V.Id, V.SSAVer});
      if (V.Kind == MedVar::Reg) {
        for (auto &[AliasId, AliasSz] : RegOffVars[V.RegOff]) {
          if (AliasId != V.Id)
            LiveDefs.insert({AliasId, V.SSAVer});
        }
      }
    }
  };

  const auto &TRI = getTargetRegInfo(TargetArch);

  // Seed: return value registers in blocks containing RETURN
  for (auto &Blk : Func.Blocks) {
    bool HasReturn = false;
    for (auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        HasReturn = true;
        break;
      }
    if (!HasReturn)
      continue;

    for (auto &Phi : Blk.Phis) {
      if (Phi.Output.Kind == MedVar::Reg &&
          TRI.isReturnReg(Phi.Output.RegOff) && Phi.Output.Size > 0) {
        MarkLive(Phi.Output);
        for (auto &[PredId, Arg] : Phi.Args)
          MarkLive(Arg);
      }
    }

    // Mark both the last-in-program-order write to the return register *and*
    // the widest write.  The MedLLVMEmitter RETURN handler selects the widest
    // write as the return value (bug #152/#157), so the widest must stay live
    // even if an independent narrow sub-register write follows it.
    for (size_t I = 0; I < Blk.Ops.size(); ++I) {
      if (Blk.Ops[I].Opcode != NdOp::RETURN)
        continue;
      bool FoundInt = false, FoundFP = false, FoundHi = false;
      const MedVar *WidestInt = nullptr, *WidestFP = nullptr,
                   *WidestHi = nullptr;
      for (int J = static_cast<int>(I) - 1; J >= 0; --J) {
        auto &Prev = Blk.Ops[J];
        if (Prev.Output.Kind != MedVar::Reg || Prev.Output.Size == 0)
          continue;
        if (Prev.Output.RegOff == TRI.IntReturnReg) {
          if (!FoundInt) {
            MarkLive(Prev.Output);
            FoundInt = true;
          }
          if (!WidestInt || Prev.Output.Size > WidestInt->Size)
            WidestInt = &Prev.Output;
        }
        // High-half integer return register (i386 EDX, ARM32 R1): a 64-bit
        // return pairs it with the low half.  Keep its last/widest write live
        // so type inference and the RETURN emitter can splice both halves; a
        // dead high half (an i32 return) is removed later by LLVM.
        if (TRI.IntReturnReg2 != 0 && Prev.Output.RegOff == TRI.IntReturnReg2) {
          if (!FoundHi) {
            MarkLive(Prev.Output);
            FoundHi = true;
          }
          if (!WidestHi || Prev.Output.Size > WidestHi->Size)
            WidestHi = &Prev.Output;
        }
        if (TRI.hasFPReturnReg() && Prev.Output.RegOff == TRI.FPReturnReg) {
          if (!FoundFP) {
            MarkLive(Prev.Output);
            FoundFP = true;
          }
          if (!WidestFP || Prev.Output.Size > WidestFP->Size)
            WidestFP = &Prev.Output;
        }
      }
      if (WidestInt)
        MarkLive(*WidestInt);
      if (WidestHi)
        MarkLive(*WidestHi);
      if (WidestFP)
        MarkLive(*WidestFP);
    }

    // Keep the secondary multi-register return registers alive too (x86-64
    // XMM1, AArch64 V1..V3 / X1): a small struct returned by value across
    // multiple registers writes them, but the pass that re-types such a callee
    // to return the aggregate runs after DCE — so seed every candidate return
    // register's last + widest write here so the later aggregate RETURN can
    // find each field.  A genuinely dead extra write (a non-struct return) is
    // removed by LLVM afterwards, so this only ever keeps real struct fields
    // alive.
    for (size_t I = 0; I < Blk.Ops.size(); ++I) {
      if (Blk.Ops[I].Opcode != NdOp::RETURN)
        continue;
      auto seedReg = [&](uint64_t RegOff) {
        const MedVar *Last = nullptr, *Widest = nullptr;
        for (int J = static_cast<int>(I) - 1; J >= 0; --J) {
          auto &Prev = Blk.Ops[J];
          if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0 &&
              Prev.Output.RegOff == RegOff) {
            if (!Last)
              Last = &Prev.Output;
            if (!Widest || Prev.Output.Size > Widest->Size)
              Widest = &Prev.Output;
          }
        }
        if (Last)
          MarkLive(*Last);
        if (Widest)
          MarkLive(*Widest);
      };
      for (uint64_t R : TRI.FPReturnRegs)
        seedReg(R);
      for (uint64_t R : TRI.IntReturnRegs)
        seedReg(R);
    }

    for (int PredId : Blk.Preds) {
      if (PredId < 0 || PredId >= static_cast<int>(Func.Blocks.size()))
        continue;
      auto &Pred = Func.Blocks[PredId];
      const MedVar *Last = nullptr, *Widest = nullptr;
      for (auto Rit = Pred.Ops.rbegin(); Rit != Pred.Ops.rend(); ++Rit) {
        if (Rit->Output.Kind == MedVar::Reg &&
            TRI.isReturnReg(Rit->Output.RegOff) && Rit->Output.Size > 0) {
          if (!Last)
            Last = &Rit->Output;
          if (!Widest || Rit->Output.Size > Widest->Size)
            Widest = &Rit->Output;
        }
      }
      if (Last)
        MarkLive(*Last);
      if (Widest)
        MarkLive(*Widest);
    }
  }

  // Seed: parameter register assignments before CALL/INDIR_CALL/tail-call
  for (auto &Blk : Func.Blocks) {
    for (size_t I = 0; I < Blk.Ops.size(); ++I) {
      bool IsCall = (Blk.Ops[I].Opcode == NdOp::CALL ||
                     Blk.Ops[I].Opcode == NdOp::INDIR_CALL ||
                     Blk.Ops[I].Opcode == NdOp::INTRINSIC);
      bool IsTail = (Blk.Ops[I].Opcode == NdOp::INDIR_BR && Blk.Succs.empty());
      if (!IsCall && !IsTail)
        continue;
      for (int J = static_cast<int>(I) - 1; J >= 0; --J) {
        auto &Prev = Blk.Ops[J];
        if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
            Prev.Opcode == NdOp::INTRINSIC)
          break;
        if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0) {
          if (TRI.isParamReg(Prev.Output.RegOff) ||
              TRI.isVectorReg(Prev.Output.RegOff))
            MarkLive(Prev.Output);
        }
      }
    }
  }

  // Seed: all PHI arguments
  for (auto &Blk : Func.Blocks) {
    for (auto &Phi : Blk.Phis) {
      MarkLive(Phi.Output);
      for (auto &[PredId, Arg] : Phi.Args)
        MarkLive(Arg);
    }
  }

  // A partial register clobber is also an implicit use of its pre-call value:
  // AAPCS64 preserves the low 64 bits of v8-v15 even when their upper half is
  // overwritten.  Keep that hidden input's def-use chain alive.
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Clobber.PreservedPrefixSize > 0)
      MarkLive(Clobber.PreservedInput);

  // Propagate liveness
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &Blk : Func.Blocks) {
      for (auto &Phi : Blk.Phis) {
        if (LiveDefs.count({Phi.Output.Id, Phi.Output.SSAVer})) {
          for (auto &[PredId, Arg] : Phi.Args) {
            if (Arg.Id >= 0) {
              auto Key = std::make_pair(Arg.Id, Arg.SSAVer);
              if (LiveDefs.insert(Key).second)
                Changed = true;
            }
          }
        }
      }

      for (auto &Op : Blk.Ops) {
        bool IsEssential =
            Op.Opcode == NdOp::STORE || Op.Opcode == NdOp::ATOMIC_XCHG ||
            Op.Opcode == NdOp::ATOMIC_ADD ||
            Op.Opcode == NdOp::ATOMIC_CMPXCHG || Op.Opcode == NdOp::CALL ||
            Op.Opcode == NdOp::INDIR_CALL || Op.Opcode == NdOp::INTRINSIC ||
            Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::BRANCH ||
            Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::INDIR_BR ||
            Op.MemoryOrdering != NdMemoryOrdering::None;

        bool OutputLive = Op.Output.Id >= 0 &&
                          LiveDefs.count({Op.Output.Id, Op.Output.SSAVer});

        if (IsEssential || OutputLive) {
          Op.Dead = false;
          for (uint8_t I = 0; I < Op.NumInputs; ++I) {
            if (Op.Inputs[I].Id >= 0) {
              auto Key = std::make_pair(Op.Inputs[I].Id, Op.Inputs[I].SSAVer);
              if (LiveDefs.insert(Key).second)
                Changed = true;
            }
          }
        } else if (!IsEssential && Op.Output.Id >= 0 &&
                   !LiveDefs.count({Op.Output.Id, Op.Output.SSAVer})) {
          Op.Dead = true;
        }
      }
    }
  }

  for (auto &Blk : Func.Blocks) {
    Blk.Ops.erase(std::remove_if(Blk.Ops.begin(), Blk.Ops.end(),
                                 [](const MedOp &Op) { return Op.Dead; }),
                  Blk.Ops.end());
  }
}

} // namespace neverd
