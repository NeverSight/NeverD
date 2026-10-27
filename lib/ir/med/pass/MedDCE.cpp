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

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

namespace {
// DCE indexes only nonnegative definition ids. Reserve negative ids instead
// of DenseMap's default INT_MAX pair, which is a valid id/version here.
struct DCEValueKeyInfo : llvm::DenseMapInfo<std::pair<int, int>> {
  static constexpr std::pair<int, int> getEmptyKey() { return {-1, -1}; }
  static constexpr std::pair<int, int> getTombstoneKey() { return {-2, -2}; }
};
} // namespace

void LowToMedConverter::runDce(MedFunc &Func) {
  llvm::DenseSet<std::pair<int, int>, DCEValueKeyInfo> LiveDefs;

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

  // A nonlocal finally exit consumes the restored parent SP through the
  // runtime, even when no ordinary SSA use survives. Keep the exact source
  // definition and its dependencies for the later native contract check.
  if (const auto Exits = getRegistrationSEHParentExits(Func))
    for (const auto &[Id, Exit] : *Exits)
      MarkLive(Exit.Restore->Output);

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

  // Seed: parameter register assignments before CALL/INDIR_CALL/tail-call.
  // A direct call of a function of the image may also take its arguments in
  // the convention's alternate register order (GCC's i386 regparm EAX, EDX,
  // ECX); an import never does.
  const CallArgumentConvention *Convention =
      callArgumentConvention(TargetArch, TargetFormat);
  const llvm::ArrayRef<uint64_t> AlternateArgumentRegisters =
      Convention ? Convention->AlternateRegisterOrder
                 : llvm::ArrayRef<uint64_t>();
  auto seedParamWrites = [&](const std::vector<MedOp> &Ops, int Before,
                             bool DirectCall) {
    for (int J = Before; J >= 0; --J) {
      const MedOp &Prev = Ops[static_cast<size_t>(J)];
      if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
          Prev.Opcode == NdOp::INTRINSIC)
        break;
      if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0 &&
          (TRI.isParamReg(Prev.Output.RegOff) ||
           TRI.isVectorReg(Prev.Output.RegOff) ||
           (DirectCall && llvm::is_contained(AlternateArgumentRegisters,
                                             Prev.Output.RegOff))))
        MarkLive(Prev.Output);
    }
  };
  // Widen block IDs so every int value stays distinct from the hash sentinels.
  llvm::SmallDenseMap<int64_t, const MedBlock *> Blocks;
  bool UniqueBlockIds = true;
  for (const auto &Block : Func.Blocks)
    if (!Blocks.try_emplace(Block.Id, &Block).second)
      UniqueBlockIds = false;
  auto blockById = [&](int Id) -> const MedBlock * {
    const auto It = Blocks.find(Id);
    return It == Blocks.end() ? nullptr : It->second;
  };
  // Queries only add live definitions, and the graph stays unchanged until
  // every query finishes. Share completed walks across calls and returns.
  // Duplicate block IDs retain the original query-local visitation: a root
  // may differ from the first block that a predecessor ID names.
  // Each call/return prefix is queried once per register. Only predecessor
  // block ends recur, so the shared set needs no record for each call site.
  llvm::DenseSet<std::pair<const MedBlock *, uint64_t>> ReachedEnds;
  llvm::SmallVector<std::pair<const MedBlock *, int>> ReachingWorklist;
  // Keep the definition of register \p RegOff that reaches the point after
  // op \p Before of \p Start on every incoming path: the newest write there,
  // else the register's PHI, else what reaches the end of each predecessor.
  auto seedReachingDef = [&](const MedBlock &Start, int Before,
                             uint64_t RegOff) {
    ReachingWorklist.push_back({&Start, Before});
    std::set<std::pair<int, int>> Visited;
    while (!ReachingWorklist.empty()) {
      const auto [Block, Last] = ReachingWorklist.back();
      ReachingWorklist.pop_back();
      if (UniqueBlockIds) {
        if (Last == static_cast<int>(Block->Ops.size()) - 1 &&
            !ReachedEnds.insert({Block, RegOff}).second)
          continue;
      } else if (!Visited.insert({Block->Id, Last}).second) {
        continue;
      }
      bool Found = false;
      for (int J = Last; J >= 0; --J) {
        const MedVar &Output = Block->Ops[static_cast<size_t>(J)].Output;
        if (Output.Kind != MedVar::Reg || Output.Size == 0 ||
            Output.RegOff != RegOff)
          continue;
        MarkLive(Output);
        Found = true;
        break;
      }
      if (Found)
        continue;
      for (const PhiNode &Phi : Block->Phis)
        if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0 &&
            Phi.Output.RegOff == RegOff) {
          MarkLive(Phi.Output);
          Found = true;
        }
      if (Found)
        continue;
      for (int PredId : Block->Preds)
        if (const MedBlock *Pred = blockById(PredId))
          ReachingWorklist.push_back(
              {Pred, static_cast<int>(Pred->Ops.size()) - 1});
    }
  };
  // A call reads SP to locate outgoing stack arguments, but its implicit input
  // is added only by later ABI recovery.  Keep the reaching definition on every
  // incoming path, including a final POP or an adjustment in a predecessor
  // block; otherwise the call frame can shift after DCE.  Returns read it too.
  auto seedReachingStackPointer = [&](const MedBlock &Start, int Before) {
    seedReachingDef(Start, Before, TRI.StackPointer);
  };
  for (auto &Blk : Func.Blocks) {
    bool HasCall = false;
    for (size_t I = 0; I < Blk.Ops.size(); ++I) {
      bool IsCall = (Blk.Ops[I].Opcode == NdOp::CALL ||
                     Blk.Ops[I].Opcode == NdOp::INDIR_CALL ||
                     Blk.Ops[I].Opcode == NdOp::INTRINSIC);
      bool IsTail = (Blk.Ops[I].Opcode == NdOp::INDIR_BR && Blk.Succs.empty());
      if (!IsCall && !IsTail)
        continue;
      seedReachingStackPointer(Blk, static_cast<int>(I) - 1);
      const MedOp &Call = Blk.Ops[I];
      // A dedicated result-buffer register is an implicit call input too.
      // ABI recovery runs later: deleting AArch64's X8 setup here severs
      // aggregate results whenever constant folding triggers register DCE.
      if (TRI.indirectResultReg() &&
          (Call.Opcode == NdOp::CALL || Call.Opcode == NdOp::INDIR_CALL ||
           IsTail))
        seedReachingDef(Blk, static_cast<int>(I) - 1, TRI.indirectResultReg());
      const bool DirectCall =
          Call.Opcode == NdOp::CALL && Call.NumInputs >= 1 &&
          Call.Inputs[0].isConst() &&
          !(Image && Image->findImportAt(Call.Inputs[0].ConstVal));
      seedParamWrites(Blk.Ops, static_cast<int>(I) - 1, DirectCall);
      // IP-map / EH splits often isolate the CALL. `lea r8; mov edx; lea rcx`
      // then sit in the predecessor and must stay live as call setup.  A
      // conditional choice between two calls does the same: each arm may add
      // `mov edx, eax` before its call while the shared `lea ecx` stays in
      // the dominating block.  Unless another call precedes this one in its
      // block, the predecessors' trailing parameter writes are its setup.
      if (HasCall)
        continue;
      HasCall = IsCall;
      for (int PredId : Blk.Preds)
        if (const MedBlock *Pred = blockById(PredId))
          seedParamWrites(Pred->Ops, static_cast<int>(Pred->Ops.size()) - 1,
                          DirectCall);
    }
  }

  // A return reads state that its operands do not name.  It pops its target
  // through the stack pointer and hands the restored stack pointer back to the
  // caller, and return typing proves a trailing `pop eax` a register restore
  // by that stack advance.  An x87 floating-point return is logical st0, whose
  // physical slot depends on TOP at the return; return typing and the RETURN
  // emitter read the newest x87 write that reaches it, so keep the reaching
  // definition of every slot.  LLVM drops the slots the typed return ignores.
  std::vector<uint64_t> X87ReturnSlots;
  for (const auto &Entry : RegOffVars)
    if (TRI.isX87ReturnReg(Entry.first))
      X87ReturnSlots.push_back(Entry.first);
  for (const MedBlock &Blk : Func.Blocks)
    for (size_t I = 0; I < Blk.Ops.size(); ++I) {
      if (Blk.Ops[I].Opcode != NdOp::RETURN)
        continue;
      const int Before = static_cast<int>(I) - 1;
      seedReachingStackPointer(Blk, Before);
      for (uint64_t Slot : X87ReturnSlots)
        seedReachingDef(Blk, Before, Slot);
    }

  // A stack-pointer write whose value is the stack pointer less a runtime byte
  // count commits a dynamic allocation (`sub esp,eax`, or i386 `sub esi,edx;
  // mov esp,esi`).  From then on the memory between the new and the old stack
  // pointer belongs to this frame, even when nothing reads the stack pointer
  // again because the epilogue restores it from the frame pointer.  The
  // emitter lowers such a write to an alloca, so keep it and its size live.
  llvm::DenseMap<std::pair<int, int>, const MedOp *, DCEValueKeyInfo> ValueDefs;
  llvm::DenseMap<std::pair<int, int>, const PhiNode *, DCEValueKeyInfo>
      ValuePhis;
  for (const MedBlock &Blk : Func.Blocks) {
    for (const PhiNode &Phi : Blk.Phis)
      if (Phi.Output.Id >= 0)
        ValuePhis.try_emplace(std::make_pair(Phi.Output.Id, Phi.Output.SSAVer),
                              &Phi);
    for (const MedOp &Op : Blk.Ops)
      if (Op.Output.Id >= 0 && !(Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                                 Op.Inputs[0].Id == Op.Output.Id &&
                                 Op.Inputs[0].SSAVer == Op.Output.SSAVer))
        ValueDefs.try_emplace(std::make_pair(Op.Output.Id, Op.Output.SSAVer),
                              &Op);
  }
  auto defOf = [&](const MedVar &V) -> const MedOp * {
    if (V.Id < 0)
      return nullptr;
    const auto It = ValueDefs.find({V.Id, V.SSAVer});
    return It == ValueDefs.end() ? nullptr : It->second;
  };
  auto isStackPointerDerived = [&](const MedVar &Start) {
    std::vector<std::pair<MedVar, int>> Pending{{Start, 0}};
    std::set<std::pair<int, int>> Visited;
    while (!Pending.empty()) {
      const auto [V, Depth] = Pending.back();
      Pending.pop_back();
      if (V.isConst() || V.Id < 0 || Depth > limits::kMaxStackPtrTraceDepth ||
          !Visited.insert({V.Id, V.SSAVer}).second)
        continue;
      if (V.Kind == MedVar::Reg && TRI.isStackPointer(V.RegOff))
        return true;
      if (const auto Phi = ValuePhis.find({V.Id, V.SSAVer});
          Phi != ValuePhis.end()) {
        for (const auto &[PredId, Arg] : Phi->second->Args)
          Pending.push_back({Arg, Depth + 1});
        continue;
      }
      const MedOp *Def = defOf(V);
      if (!Def || Def->NumInputs < 1)
        continue;
      switch (Def->Opcode) {
      case NdOp::COPY:
      case NdOp::SUBBYTES:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::INT_AND:
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
        Pending.push_back({Def->Inputs[0], Depth + 1});
        break;
      default:
        break;
      }
    }
    return false;
  };
  auto commitsRuntimeAllocation = [&](const MedOp &Write) {
    const MedOp *Def = &Write;
    for (int Depth = 0; Depth <= limits::kMaxStackPtrTraceDepth; ++Depth) {
      if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs == 2)
        return !Def->Inputs[1].isConst() &&
               isStackPointerDerived(Def->Inputs[0]) &&
               !isStackPointerDerived(Def->Inputs[1]);
      const bool Transport =
          Def->NumInputs >= 1 &&
          (Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
           (Def->Opcode == NdOp::SUBBYTES && Def->NumInputs == 2 &&
            Def->Inputs[1].isConst() && Def->Inputs[1].ConstVal == 0));
      if (!Transport)
        return false;
      Def = defOf(Def->Inputs[0]);
      if (!Def)
        return false;
    }
    return false;
  };
  for (const MedBlock &Blk : Func.Blocks)
    for (const MedOp &Op : Blk.Ops)
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0 &&
          TRI.isStackPointer(Op.Output.RegOff) && commitsRuntimeAllocation(Op))
        MarkLive(Op.Output);

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
  // These bounded leaves never use arithmetic flags to select memory or
  // control effects. Their explicit old flags affect only the returned flags
  // on normal completion. Keeping that dependency conditional prevents a dead
  // flags result from creating an undefined entry flag or source parameter.
  const auto FlagResult = [](const MedOp &Op) -> const MedVar * {
    if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
        !Op.Inputs[0].isConst())
      return nullptr;
    const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
    if (!isX86BoundedLeafIntrinsic(Id) ||
        !x86BoundedLeafShapeIsValid(Id, x86BoundedLeafMedShape(Op)))
      return nullptr;
    return &Op.IntrinsicOutputs.back();
  };
  llvm::DenseMap<std::pair<int, int>, llvm::SmallVector<MedOp *, 1>,
                 DCEValueKeyInfo>
      OpDefs;
  llvm::DenseMap<std::pair<int, int>, llvm::SmallVector<PhiNode *, 1>,
                 DCEValueKeyInfo>
      PhiDefs;
  for (auto &Blk : Func.Blocks) {
    for (auto &Phi : Blk.Phis)
      if (Phi.Output.Id >= 0)
        PhiDefs[{Phi.Output.Id, Phi.Output.SSAVer}].push_back(&Phi);
    for (auto &Op : Blk.Ops) {
      if (Op.Output.Id >= 0)
        OpDefs[{Op.Output.Id, Op.Output.SSAVer}].push_back(&Op);
      if (const auto *Flags = FlagResult(Op))
        OpDefs[{Flags->Id, Flags->SSAVer}].push_back(&Op);
    }
  }
  std::vector<std::pair<int, int>> Worklist(LiveDefs.begin(), LiveDefs.end());
  auto markInputs = [&](const MedOp &Op) {
    for (uint8_t I = 0; I < Op.NumInputs; ++I) {
      if (I == X86BoundedLeafFlagOperand)
        if (const auto *Flags = FlagResult(Op);
            Flags && !LiveDefs.count({Flags->Id, Flags->SSAVer}))
          continue;
      if (Op.Inputs[I].Id >= 0) {
        auto Key = std::make_pair(Op.Inputs[I].Id, Op.Inputs[I].SSAVer);
        if (LiveDefs.insert(Key).second)
          Worklist.push_back(Key);
      }
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
  for (auto &Blk : Func.Blocks) {
    bool EntryCopies = &Blk == &Func.Blocks.front();
    for (auto &Op : Blk.Ops) {
      EntryCopies &= Op.Opcode == NdOp::COPY;
      if (const auto *Flags = FlagResult(Op);
          Flags && !LiveDefs.count({Flags->Id, Flags->SSAVer}))
        Op.Inputs[X86BoundedLeafFlagOperand] = MedVar::makeConst(
            0, X86ArithmeticFlagsBytes, ConstantAddressProvenance::Scalar);
      const bool OutputLive =
          Op.Output.Id >= 0 && LiveDefs.count({Op.Output.Id, Op.Output.SSAVer});
      // Multi-root SSA can number an entry copy after version zero. Copy
      // propagation then leaves readers of its incoming operand while the
      // copy's result is unused. Retain the live-in declaration: stack and
      // callee-save proofs still need its entry provenance.
      const bool IncomingLive =
          EntryCopies && Op.NumInputs == 1 && Op.Output.Kind == MedVar::Reg &&
          Op.Output.Size && Op.Inputs[0].Kind == MedVar::Reg &&
          Op.Inputs[0].SSAVer == 0 && Op.Output.Id == Op.Inputs[0].Id &&
          Op.Output.RegOff == Op.Inputs[0].RegOff &&
          Op.Output.Size == Op.Inputs[0].Size &&
          Op.Output.TheArch == Op.Inputs[0].TheArch &&
          LiveDefs.count({Op.Inputs[0].Id, Op.Inputs[0].SSAVer});
      if (isEssential(Op) || OutputLive || IncomingLive)
        Op.Dead = false;
      else if (Op.Output.Id >= 0)
        Op.Dead = true;
    }
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
