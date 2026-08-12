//===- MedSSA.cpp - SSA construction for MedIR -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSA construction: live-in analysis, dominance computation
/// (Cooper-Harvey-Kennedy), dominance frontier calculation, phi insertion,
/// and iterative SSA renaming over the dominator tree.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <queue>
#include <set>

#define DEBUG_TYPE "neverd-med-ssa"

namespace neverd {

void LowToMedConverter::buildSsa(MedFunc &Func) {
  if (Func.Blocks.empty())
    return;
  Func.CallClobbers.clear();

  int N = static_cast<int>(Func.Blocks.size());
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const bool UseItaniumExceptionalFlow =
      Func.ExceptionMetadata && Func.ExceptionMetadata->Itanium &&
      Func.ExceptionMetadata->Itanium->IsCallSiteAddressForm;

  // The machine exceptional edge carries the current frame state into a
  // landing pad just as surely as an ordinary branch carries SSA values.  Keep
  // that edge in the SSA-only graph while leaving MedBlock::Succs untouched:
  // the LLVM emitter will turn the protected call into an invoke later.
  std::vector<std::vector<int>> FlowSuccs(N), FlowPreds(N);
  std::vector<bool> IsExceptionalTarget(N, false);
  auto AddFlowEdge = [&](int From, int To) {
    if (From < 0 || From >= N || To < 0 || To >= N)
      return;
    auto &Succs = FlowSuccs[From];
    if (std::find(Succs.begin(), Succs.end(), To) == Succs.end())
      Succs.push_back(To);
  };
  for (int B = 0; B < N; ++B) {
    for (int S : Func.Blocks[B].Succs)
      AddFlowEdge(B, S);
    if (UseItaniumExceptionalFlow)
      for (const ExceptionalEdge &Edge : Func.Blocks[B].ExceptionalSuccs)
        if (Edge.BlockId >= 0 && Edge.BlockId < N) {
          AddFlowEdge(B, Edge.BlockId);
          IsExceptionalTarget[Edge.BlockId] = true;
        }
  }
  for (int B = 0; B < N; ++B)
    for (int S : FlowSuccs[B])
      FlowPreds[S].push_back(B);

  // Reverse post-order + immediate dominators (Cooper-Harvey-Kennedy), computed
  // up front because both the live-in analysis (Step 0) and the dominance
  // frontiers (Step 2) need them.
  std::vector<int> RPO;
  std::vector<int> RPONum(N, -1);
  std::vector<int> Roots;
  std::vector<int> Component(N, -1);
  {
    std::vector<bool> Visited(N, false);
    auto AppendComponent = [&](int Root) {
      const int ComponentId = static_cast<int>(Roots.size());
      Roots.push_back(Root);
      const size_t RPOBegin = RPO.size();
      std::vector<std::pair<int, size_t>> Stk;
      Stk.reserve(N);
      Visited[Root] = true;
      Component[Root] = ComponentId;
      Stk.push_back({Root, 0});
      while (!Stk.empty()) {
        int B = Stk.back().first;
        size_t I = Stk.back().second;
        auto &Succs = FlowSuccs[B];
        if (I < Succs.size()) {
          Stk.back().second = I + 1;
          int S = Succs[I];
          if (S < 0 || S >= N)
            continue;
          if (!Visited[S]) {
            Visited[S] = true;
            Component[S] = ComponentId;
            Stk.push_back({S, 0});
          }
        } else {
          RPO.push_back(B);
          Stk.pop_back();
        }
      }
      std::reverse(RPO.begin() + static_cast<long>(RPOBegin), RPO.end());
    };

    // Exception handlers and other address-discovered regions can be valid
    // blocks without an ordinary edge from block zero.  Treat every such
    // component as an additional SSA root so its definitions receive unique
    // versions instead of retaining version zero and colliding with values in
    // the entry component.
    AppendComponent(0);
    for (int B = 0; B < N; ++B)
      if (!Visited[B])
        AppendComponent(B);

    for (int I = 0; I < static_cast<int>(RPO.size()); ++I)
      RPONum[RPO[I]] = I;
  }

  std::vector<int> IDom(N, -1);
  for (int Root : Roots)
    IDom[Root] = Root;

  auto Intersect = [&](int B1, int B2) -> int {
    int F1 = B1, F2 = B2;
    while (F1 != F2) {
      while (RPONum[F1] > RPONum[F2])
        F1 = IDom[F1];
      while (RPONum[F2] > RPONum[F1])
        F2 = IDom[F2];
    }
    return F1;
  };

  {
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (int B : RPO) {
        if (IDom[B] == B)
          continue;
        int NewIDom = -1;
        for (int P : FlowPreds[B]) {
          if (P < 0 || P >= N || Component[P] != Component[B])
            continue;
          if (IDom[P] == -1)
            continue;
          if (NewIDom == -1)
            NewIDom = P;
          else
            NewIDom = Intersect(NewIDom, P);
        }
        if (NewIDom != -1 && IDom[B] != NewIDom) {
          IDom[B] = NewIDom;
          Changed = true;
        }
      }
    }
  }

  std::map<uint64_t, std::set<int>> RegOffToIds;
  std::map<int, MedVar> RegVarOfId;
  for (const MedBlock &Blk : Func.Blocks) {
    for (const MedOp &Op : Blk.Ops) {
      auto AddReg = [&](const MedVar &V) {
        if (V.Kind != MedVar::Reg || V.Id < 0)
          return;
        RegOffToIds[V.RegOff].insert(V.Id);
        RegVarOfId.emplace(V.Id, V);
      };
      AddReg(Op.Output);
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        AddReg(Op.Inputs[I]);
    }
  }

  // An Itanium landing pad overwrites the target ABI's first two integer
  // result registers with {exception object, selector}.  Define those values
  // at every exceptional target before liveness/SSA construction so they kill
  // the protected call's ordinary register values, while all preserved frame
  // registers continue to flow over the exceptional edge.
  const uint64_t EHSelectorReg =
      TRI.IntReturnRegs.size() > 1 ? TRI.IntReturnRegs[1]
                                  : TRI.IntReturnReg2;
  auto InsertEHDefs = [&](MedBlock &Block, uint64_t RegOff,
                          MedVar::VarKind Kind) {
    auto IdsIt = RegOffToIds.find(RegOff);
    if (IdsIt == RegOffToIds.end())
      return;
    std::vector<MedOp> Defs;
    for (int Id : IdsIt->second) {
      auto VarIt = RegVarOfId.find(Id);
      if (VarIt == RegVarOfId.end())
        continue;
      MedOp Def;
      Def.Opcode = NdOp::COPY;
      Def.Output = VarIt->second;
      MedVar Input = VarIt->second;
      Input.Kind = Kind;
      Input.Id = -1;
      Input.SSAVer = 0;
      Def.addInput(Input);
      Def.Addr = Block.StartAddr;
      Defs.push_back(Def);
    }
    Block.Ops.insert(Block.Ops.begin(), Defs.begin(), Defs.end());
  };
  if (UseItaniumExceptionalFlow) {
    for (int B = 0; B < N; ++B) {
      if (!IsExceptionalTarget[B])
        continue;
      // Insert selector first so the exception definition remains the first
      // recovered register copy; neither order is semantically significant.
      if (EHSelectorReg != 0)
        InsertEHDefs(Func.Blocks[B], EHSelectorReg, MedVar::EHSelector);
      InsertEHDefs(Func.Blocks[B], TRI.IntReturnReg, MedVar::EHException);
    }
  }

  auto CallClobberedIds = [&](const MedOp &Op) {
    std::set<int> Result;
    if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
        Op.PreservesCallerSaved)
      return Result;
    for (const auto &[RegOff, Ids] : RegOffToIds) {
      if (TRI.isFrameOrLinkReg(RegOff) || TRI.isStackPointer(RegOff) ||
          (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff))
        continue;
      for (int Id : Ids) {
        auto It = RegVarOfId.find(Id);
        if (It == RegVarOfId.end() ||
            TRI.callPreservedPrefixSize(RegOff, It->second.Size) <
                It->second.Size)
          Result.insert(Id);
      }
    }
    return Result;
  };

  // Step 0: Insert implicit definitions for live-in variables in the entry
  // block.  A variable is live-in to the function when some path from entry
  // uses it before any definition — exactly the values the caller supplies.
  // This is a standard backward liveness fixpoint: a plain "defined earlier in
  // reverse post-order" test is unsound, since a definition in a sibling or
  // return block reaches no use here yet would mask a genuine live-in (e.g. a
  // pointer parameter first read in a loop preheader while a `n==0` exit block
  // also clears that register).
  {
    // Per-block upward-exposed uses (read before any local definition) and
    // kills (definitions, alias-expanded), plus a representative MedVar per Id
    // for materialising the self-copy.
    std::vector<std::set<int>> UEVar(N), VarKill(N);
    std::map<int, MedVar> VarOfId;
    for (int B = 0; B < N; ++B) {
      std::set<int> &Kill = VarKill[B];
      for (auto &Op : Func.Blocks[B].Ops) {
        for (uint8_t I = 0; I < Op.NumInputs; ++I) {
          const auto &Inp = Op.Inputs[I];
          if (Inp.Id >= 0 && !Kill.count(Inp.Id)) {
            UEVar[B].insert(Inp.Id);
            VarOfId.emplace(Inp.Id, Inp);
          }
        }
        // A partial call clobber reads the pre-call value to retain its ABI-
        // preserved low prefix.  Account for that hidden use before applying
        // the call's kill, otherwise a first-use v8-v15 Q view would lack its
        // live-in definition.
        for (int Id : CallClobberedIds(Op)) {
          auto It = RegVarOfId.find(Id);
          if (It == RegVarOfId.end())
            continue;
          uint16_t Prefix =
              TRI.callPreservedPrefixSize(It->second.RegOff, It->second.Size);
          if (Prefix > 0 && Prefix < It->second.Size && !Kill.count(Id)) {
            UEVar[B].insert(Id);
            VarOfId.emplace(Id, It->second);
          }
        }
        if (Op.Output.Id >= 0 && Op.Output.Size > 0) {
          Kill.insert(Op.Output.Id);
          if (Op.Output.Kind == MedVar::Reg) {
            auto It = RegOffToIds.find(Op.Output.RegOff);
            if (It != RegOffToIds.end())
              Kill.insert(It->second.begin(), It->second.end());
          }
        }
        std::set<int> Clobbered = CallClobberedIds(Op);
        Kill.insert(Clobbered.begin(), Clobbered.end());
      }
    }

    std::vector<std::set<int>> LiveIn(N), LiveOut(N);
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (auto It = RPO.rbegin(); It != RPO.rend(); ++It) {
        int B = *It;
        std::set<int> NewOut;
        for (int S : Func.Blocks[B].Succs)
          if (S >= 0 && S < N)
            NewOut.insert(LiveIn[S].begin(), LiveIn[S].end());
        std::set<int> NewIn = UEVar[B];
        for (int V : NewOut)
          if (!VarKill[B].count(V))
            NewIn.insert(V);
        if (NewIn != LiveIn[B] || NewOut != LiveOut[B]) {
          LiveIn[B] = std::move(NewIn);
          LiveOut[B] = std::move(NewOut);
          Changed = true;
        }
      }
    }

    for (int Root : Roots) {
      std::vector<MedOp> InitOps;
      const bool IsItaniumEHRoot =
          !Func.Blocks[Root].ExceptionalPreds.empty() &&
          Func.ExceptionMetadata && Func.ExceptionMetadata->Itanium &&
          Func.ExceptionMetadata->Itanium->IsCallSiteAddressForm;
      for (int Id : LiveIn[Root]) {
        auto VIt = VarOfId.find(Id);
        if (VIt == VarOfId.end())
          continue;
        MedOp Init;
        Init.Opcode = NdOp::COPY;
        Init.Output = VIt->second;
        MedVar Input = VIt->second;
        if (IsItaniumEHRoot && Input.Kind == MedVar::Reg &&
            Input.RegOff == TRI.IntReturnReg) {
          Input.Kind = MedVar::EHException;
          Input.Id = -1;
          Input.SSAVer = 0;
        } else if (IsItaniumEHRoot && EHSelectorReg != 0 &&
                   Input.Kind == MedVar::Reg &&
                   Input.RegOff == EHSelectorReg) {
          Input.Kind = MedVar::EHSelector;
          Input.Id = -1;
          Input.SSAVer = 0;
        }
        Init.addInput(Input);
        Init.Addr = Func.Blocks[Root].StartAddr;
        InitOps.push_back(Init);
        LLVM_DEBUG(llvm::dbgs() << "  live-in: " << VIt->second.display()
                                << " (id=" << Id << ")\n");
      }
      auto &RootOps = Func.Blocks[Root].Ops;
      RootOps.insert(RootOps.begin(), InitOps.begin(), InitOps.end());
    }
  }

  // Step 2: Compute dominance frontiers
  std::vector<std::set<int>> DF(N);
  for (int B = 0; B < N; ++B) {
    if (FlowPreds[B].size() < 2)
      continue;
    for (int P : FlowPreds[B]) {
      if (P < 0 || P >= N || Component[P] != Component[B])
        continue;
      int Runner = P;
      while (Runner != IDom[B] && Runner != -1) {
        DF[Runner].insert(B);
        Runner = IDom[Runner];
      }
    }
  }

  // Step 3: Collect all variable definitions per block
  std::map<int, std::set<int>> VarDefs;
  for (int B = 0; B < N; ++B) {
    for (auto &Op : Func.Blocks[B].Ops) {
      if (Op.Output.Id >= 0 && Op.Output.Size > 0)
        VarDefs[Op.Output.Id].insert(B);
      for (int Id : CallClobberedIds(Op))
        VarDefs[Id].insert(B);
    }
  }

  // Step 4: Insert phi nodes
  std::map<int, MedVar> VarIdToVar = RegVarOfId;
  for (auto &Blk : Func.Blocks)
    for (auto &Op : Blk.Ops)
      if (Op.Output.Id >= 0 && Op.Output.Size > 0)
        VarIdToVar.emplace(Op.Output.Id, Op.Output);

  for (auto &[VarId, DefBlocks] : VarDefs) {
    std::set<int> PhiBlocks;
    std::queue<int> Worklist;
    for (int B : DefBlocks)
      Worklist.push(B);

    while (!Worklist.empty()) {
      int B = Worklist.front();
      Worklist.pop();
      for (int D : DF[B]) {
        if (PhiBlocks.insert(D).second) {
          auto VIt = VarIdToVar.find(VarId);
          MedVar PhiVar = (VIt != VarIdToVar.end()) ? VIt->second : MedVar{};

          PhiNode Phi;
          Phi.Output = PhiVar;
          for (int P : FlowPreds[D])
            Phi.Args.push_back({P, PhiVar});
          Func.Blocks[D].Phis.push_back(Phi);

          Worklist.push(D);
        }
      }
    }
  }

  // Step 5: SSA renaming (assign version numbers)
  std::map<int, int> VarCounter;
  std::map<int, std::vector<int>> VarStack;

  auto GetVersion = [&](int VarId) -> int {
    if (VarStack[VarId].empty())
      return 0;
    return VarStack[VarId].back();
  };
  auto NewVersion = [&](int VarId) -> int {
    int V = VarCounter[VarId]++;
    VarStack[VarId].push_back(V);
    return V;
  };

  std::vector<std::vector<int>> DomChildren(N);
  for (int C = 0; C < N; ++C) {
    if (C != 0 && IDom[C] != -1 && IDom[C] != C)
      DomChildren[IDom[C]].push_back(C);
  }

  struct Frame {
    int B;
    size_t ChildIdx;
    std::map<int, int> SavedSizes;
  };
  std::vector<Frame> Stk;
  Stk.reserve(N);

  auto ProcessBlock = [&](Frame &F) {
    if (F.B < 0 || F.B >= N)
      return;
    auto &Blk = Func.Blocks[F.B];
    for (auto &Phi : Blk.Phis) {
      Phi.Output.SSAVer = NewVersion(Phi.Output.Id);
      F.SavedSizes[Phi.Output.Id]++;
    }
    for (auto &Op : Blk.Ops) {
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        if (Op.Inputs[I].Id >= 0)
          Op.Inputs[I].SSAVer = GetVersion(Op.Inputs[I].Id);
      }
      if (Op.Output.Id >= 0 && Op.Output.Size > 0) {
        Op.Output.SSAVer = NewVersion(Op.Output.Id);
        F.SavedSizes[Op.Output.Id]++;
      }
      for (int Id : CallClobberedIds(Op)) {
        auto It = RegVarOfId.find(Id);
        MedVar PreservedInput;
        uint16_t PreservedPrefixSize = 0;
        if (It != RegVarOfId.end()) {
          PreservedPrefixSize =
              TRI.callPreservedPrefixSize(It->second.RegOff, It->second.Size);
          if (PreservedPrefixSize > 0 &&
              PreservedPrefixSize < It->second.Size) {
            PreservedInput = It->second;
            PreservedInput.SSAVer = GetVersion(Id);
          } else {
            PreservedPrefixSize = 0;
          }
        }
        int Version = NewVersion(Id);
        F.SavedSizes[Id]++;
        if (It != RegVarOfId.end()) {
          MedVar Clobber = It->second;
          Clobber.SSAVer = Version;
          Func.CallClobbers.push_back(
              {Clobber, Op.CallSiteId, PreservedInput, PreservedPrefixSize});
        }
      }
    }
    for (int S : FlowSuccs[F.B]) {
      if (S < 0 || S >= N)
        continue;
      for (auto &Phi : Func.Blocks[S].Phis) {
        for (auto &[Pred, Arg] : Phi.Args) {
          if (Pred == F.B)
            Arg.SSAVer = GetVersion(Arg.Id);
        }
      }
    }
  };

  for (int Root : Roots) {
    Stk.push_back({Root, 0, {}});
    ProcessBlock(Stk.back());

    while (!Stk.empty()) {
      auto &F = Stk.back();
      auto &Children = DomChildren[F.B];
      if (F.ChildIdx < Children.size()) {
        int C = Children[F.ChildIdx++];
        Stk.push_back({C, 0, {}});
        ProcessBlock(Stk.back());
      } else {
        for (auto &[VId, Cnt] : F.SavedSizes) {
          for (int I = 0; I < Cnt; ++I)
            VarStack[VId].pop_back();
        }
        Stk.pop_back();
      }
    }
  }
}

} // namespace neverd
