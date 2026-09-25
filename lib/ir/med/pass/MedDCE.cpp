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

  // Distinct variable ids that name each register offset (its aliases).
  std::map<uint64_t, std::set<int>> RegOffVars;
  for (auto &Blk : Func.Blocks) {
    for (auto &Op : Blk.Ops) {
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Id >= 0)
        RegOffVars[Op.Output.RegOff].insert(Op.Output.Id);
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        if (Op.Inputs[I].Kind == MedVar::Reg && Op.Inputs[I].Id >= 0)
          RegOffVars[Op.Inputs[I].RegOff].insert(Op.Inputs[I].Id);
      }
    }
  }
  for (const MedCallClobber &Clobber : Func.CallClobbers) {
    if (Clobber.Value.Kind == MedVar::Reg && Clobber.Value.Id >= 0)
      RegOffVars[Clobber.Value.RegOff].insert(Clobber.Value.Id);
    if (Clobber.PreservedPrefixSize > 0 &&
        Clobber.PreservedInput.Kind == MedVar::Reg &&
        Clobber.PreservedInput.Id >= 0)
      RegOffVars[Clobber.PreservedInput.RegOff].insert(
          Clobber.PreservedInput.Id);
  }

  auto MarkLive = [&](const MedVar &V) {
    if (V.Id >= 0) {
      LiveDefs.insert({V.Id, V.SSAVer});
      if (V.Kind == MedVar::Reg) {
        for (int AliasId : RegOffVars[V.RegOff]) {
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

  auto isFlagVar = [&](const MedVar &V) {
    if (V.Kind == MedVar::Flag)
      return true;
    if (V.Kind != MedVar::Reg)
      return false;
    if (TRI.isFlag(V.RegOff, V.Size ? V.Size : 1))
      return true;
    return V.RegOff == TRI.FlagCF || V.RegOff == TRI.FlagZF ||
           V.RegOff == TRI.FlagNF || V.RegOff == TRI.FlagVF ||
           (TRI.FlagPF != 0 && V.RegOff == TRI.FlagPF) ||
           (TRI.FlagDF != 0 && V.RegOff == TRI.FlagDF);
  };

  // Seed PHIs that can be implicit ABI live-ins (call args, returns,
  // callee-saves, SP/FP).  Flag and temp PHIs stay live only when a later
  // essential op reads them; otherwise CMP/TEST leftover PF popcount and
  // flag SSA survive at joins (cookie / seh_probe LLVM-to-C).
  for (auto &Blk : Func.Blocks) {
    for (auto &Phi : Blk.Phis) {
      const MedVar &V = Phi.Output;
      if (isFlagVar(V) || V.Kind != MedVar::Reg)
        continue;
      if (!(TRI.isParamReg(V.RegOff) || TRI.isReturnReg(V.RegOff) ||
            TRI.isCalleeSaveReg(V.RegOff) || TRI.isStackPointer(V.RegOff) ||
            TRI.isFramePointer(V.RegOff) || TRI.isLinkRegister(V.RegOff) ||
            TRI.isVectorReg(V.RegOff)))
        continue;
      MarkLive(V);
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

  // Propagate liveness with a worklist over definitions: a live value makes
  // its defining operation (or PHI) live, which makes their inputs live.
  auto isEssential = [](const MedOp &Op) {
    return Op.Opcode == NdOp::STORE || Op.Opcode == NdOp::ATOMIC_XCHG ||
           Op.Opcode == NdOp::ATOMIC_ADD || Op.Opcode == NdOp::ATOMIC_CMPXCHG ||
           Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
           Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::RETURN ||
           Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
           Op.Opcode == NdOp::INDIR_BR ||
           Op.MemoryOrdering != NdMemoryOrdering::None ||
           Op.MemoryAddressSpace != NdMemoryAddressSpace::Default;
  };
  std::map<std::pair<int, int>, std::vector<MedOp *>> OpDefs;
  std::map<std::pair<int, int>, std::vector<PhiNode *>> PhiDefs;
  for (auto &Blk : Func.Blocks) {
    for (auto &Phi : Blk.Phis)
      if (Phi.Output.Id >= 0)
        PhiDefs[{Phi.Output.Id, Phi.Output.SSAVer}].push_back(&Phi);
    for (auto &Op : Blk.Ops)
      if (Op.Output.Id >= 0)
        OpDefs[{Op.Output.Id, Op.Output.SSAVer}].push_back(&Op);
  }
  std::vector<std::pair<int, int>> Worklist(LiveDefs.begin(), LiveDefs.end());
  auto markInputs = [&](const MedOp &Op) {
    for (uint8_t I = 0; I < Op.NumInputs; ++I)
      if (Op.Inputs[I].Id >= 0) {
        auto Key = std::make_pair(Op.Inputs[I].Id, Op.Inputs[I].SSAVer);
        if (LiveDefs.insert(Key).second)
          Worklist.push_back(Key);
      }
  };
  for (auto &Blk : Func.Blocks)
    for (auto &Op : Blk.Ops)
      if (isEssential(Op))
        markInputs(Op);
  while (!Worklist.empty()) {
    const auto Key = Worklist.back();
    Worklist.pop_back();
    if (auto It = PhiDefs.find(Key); It != PhiDefs.end())
      for (PhiNode *Phi : It->second)
        for (auto &[PredId, Arg] : Phi->Args)
          if (Arg.Id >= 0) {
            auto ArgKey = std::make_pair(Arg.Id, Arg.SSAVer);
            if (LiveDefs.insert(ArgKey).second)
              Worklist.push_back(ArgKey);
          }
    if (auto It = OpDefs.find(Key); It != OpDefs.end())
      for (MedOp *Op : It->second)
        markInputs(*Op);
  }
  for (auto &Blk : Func.Blocks)
    for (auto &Op : Blk.Ops) {
      const bool OutputLive =
          Op.Output.Id >= 0 && LiveDefs.count({Op.Output.Id, Op.Output.SSAVer});
      if (isEssential(Op) || OutputLive)
        Op.Dead = false;
      else if (Op.Output.Id >= 0)
        Op.Dead = true;
    }

  for (auto &Blk : Func.Blocks) {
    Blk.Ops.erase(std::remove_if(Blk.Ops.begin(), Blk.Ops.end(),
                                 [](const MedOp &Op) { return Op.Dead; }),
                  Blk.Ops.end());
    Blk.Phis.erase(std::remove_if(Blk.Phis.begin(), Blk.Phis.end(),
                                  [&](const PhiNode &Phi) {
                                    if (Phi.Output.Id < 0)
                                      return false;
                                    return !LiveDefs.count(
                                        {Phi.Output.Id, Phi.Output.SSAVer});
                                  }),
                   Blk.Phis.end());
  }
}

} // namespace neverd
