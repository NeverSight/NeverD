//===- MedLLVMControlFlow.cpp - Finite scalar control flow ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Joint executable-edge and finite scalar propagation for native emission.

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd {

void MedLLVMEmitter::refineFiniteControlEdges(
    const std::map<int, const MedBlock *> &Blocks,
    const std::vector<int> &EntryRoots, std::set<std::pair<int, int>> &Edges,
    std::set<int> &LiveBlocks) const {
  // Partial lifting or an unproved indirect transfer cannot establish that
  // the published edge set covers every arrival at a scalar definition.
  if (!CurMedFunc->CompleteLiftCoverage || CurMedFunc->SkippedSSA ||
      !CurMedFunc->UnsafeIndirectBranchAddresses.empty())
    return;
  // Empty is not yet evaluated; nullopt is any runtime value. Finite sets are
  // complete covers, never observations or enumerated prefixes. Only the
  // final fixed point is used to remove an edge.
  using Domain = std::optional<std::vector<uint64_t>>;
  const Domain Unknown;
  struct Use {
    size_t Node;
    bool Dominates = true;
  };
  struct Node {
    MedVar Value;
    const MedOp *Op = nullptr;
    const PhiNode *Phi = nullptr;
    std::vector<Use> Inputs;
    std::vector<size_t> Users;
    std::vector<int> BranchUsers;
    Domain Values = std::vector<uint64_t>{};
    int Block = -1;
    int Position = -1;
    bool Any = false;
    bool Queued = false;
  };
  struct Branch {
    size_t Condition;
    int Taken;
    int Fallthrough;
    int Position;
    bool Dominates = true;
  };
  struct KeyHash {
    size_t operator()(const AddressProvenanceVarKey &K) const {
      return llvm::hash_value(K);
    }
  };
  std::vector<Node> Nodes;
  std::unordered_map<AddressProvenanceVarKey, size_t, KeyHash> Index;
  auto addNode = [&](const MedVar &V) {
    auto [It, Added] = Index.emplace(addressProvenanceVarKey(V), Nodes.size());
    if (Added) {
      Node N;
      N.Value = V;
      Nodes.push_back(std::move(N));
    }
    return It->second;
  };
  std::map<int, Branch> Branches;
  for (const auto &[Id, B] : Blocks) {
    if (!LiveBlocks.count(Id) || B->Succs.size() != 2)
      continue;
    const auto It =
        std::find_if(B->Ops.begin(), B->Ops.end(),
                     [](const MedOp &O) { return O.Opcode == NdOp::COND_BR; });
    if (It == B->Ops.end() || It + 1 != B->Ops.end() || It->NumInputs != 2 ||
        !It->Inputs[0].isConst() || It->Inputs[0].ConstVal == 0 ||
        B->Succs[0] == B->Succs[1])
      continue;
    if (std::any_of(B->Ops.begin(), It, [](const MedOp &O) {
          return O.Opcode == NdOp::BRANCH || O.Opcode == NdOp::INDIR_BR ||
                 O.Opcode == NdOp::RETURN || O.DoesNotReturn;
        }))
      continue;
    // A zero sentinel or a target absent from the successors is not a
    // complete conditional edge identity. It cannot seed a new loop proof.
    std::vector<int> Matching;
    for (int S : B->Succs) {
      auto Found = Blocks.find(S);
      if (Found == Blocks.end())
        continue;
      const auto &Target = *Found->second;
      const va_t Address = Target.StartAddr != 0 || Target.Ops.empty()
                               ? Target.StartAddr
                               : Target.Ops.front().Addr;
      if (Address == It->Inputs[0].ConstVal)
        Matching.push_back(S);
    }
    if (Matching.size() != 1)
      continue;
    const int Taken = Matching.front();
    const size_t Condition = addNode(It->Inputs[1]);
    Branches.emplace(Id,
                     Branch{Condition, Taken,
                            Taken == B->Succs[0] ? B->Succs[1] : B->Succs[0],
                            static_cast<int>(It - B->Ops.begin())});
    Nodes[Condition].BranchUsers.push_back(Id);
  }

  // Build only the SSA slice used by control. Loads, calls, implicit clobbers,
  // and unsupported operations terminate the slice with an unknown value.
  bool HasMerge = false;
  std::unordered_map<const MedOp *, std::vector<size_t>> Operations;
  std::unordered_map<const PhiNode *, std::vector<size_t>> Phis;
  for (size_t I = 0; I != Nodes.size(); ++I) {
    const MedVar V = Nodes[I].Value;
    if (V.Size > 8) {
      Nodes[I].Any = true;
      continue;
    }
    if (V.isConst()) {
      if (auto Bits = traceControlConst(V))
        Nodes[I].Values = std::vector<uint64_t>{*Bits};
      else
        Nodes[I].Any = true;
      continue;
    }
    if (const auto *Phi = lookupPhi(V)) {
      HasMerge = true;
      Nodes[I].Phi = Phi;
      Phis[Phi].push_back(I);
      for (const auto &[Pred, Value] : Phi->Args) {
        const size_t Input = addNode(Value);
        Nodes[I].Inputs.push_back({Input});
      }
      continue;
    }
    const MedOp *Op = lookupDef(V);
    Nodes[I].Op = Op;
    if (!Op) {
      Nodes[I].Any = true;
      continue;
    }
    Operations[Op].push_back(I);
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR:
    case NdOp::BOOL_XOR:
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
    case NdOp::INT_NEGATE:
    case NdOp::INT_NOT:
    case NdOp::INT_NEG2:
    case NdOp::BOOL_NOT:
    case NdOp::SELECT:
      HasMerge |= Op->Opcode == NdOp::SELECT;
      for (unsigned J = 0; J != Op->NumInputs; ++J) {
        const size_t Input = addNode(Op->Inputs[J]);
        Nodes[I].Inputs.push_back({Input});
      }
      break;
    default:
      Nodes[I].Any = true;
      break;
    }
  }
  if (!HasMerge)
    return;

  std::map<int, std::vector<size_t>> BlockNodes, BlockPhis;
  std::map<int, std::set<int>> OrdinaryPredecessors;
  std::set<int> Roots(EntryRoots.begin(), EntryRoots.end());
  std::set<int> InteriorRoots;
  for (const auto &[Id, B] : Blocks) {
    for (int S : B->Succs)
      OrdinaryPredecessors[S].insert(Id);
    // Exception transfers may occur before a definition in the throwing
    // block. Treat every published landing pad as an independent unknown
    // entry for this proof; ordinary SSA dominance alone cannot justify it.
    for (const auto &E : B->ExceptionalSuccs)
      if (LiveBlocks.count(E.BlockId))
        Roots.insert(E.BlockId);
    auto hasInteriorRoot = [&](const std::set<va_t> &Addresses) {
      auto It = Addresses.upper_bound(B->StartAddr);
      return It != Addresses.end() && *It < B->EndAddr;
    };
    if (hasInteriorRoot(CurMedFunc->ModuleAnalysisRoots) ||
        (Img && hasInteriorRoot(Img->CodeRefTargets)))
      InteriorRoots.insert(Id);
    for (const PhiNode &P : B->Phis)
      if (auto It = Phis.find(&P); It != Phis.end()) {
        for (size_t I : It->second) {
          Node &N = Nodes[I];
          N.Block = Id;
          BlockNodes[Id].push_back(I);
          BlockPhis[Id].push_back(I);
        }
      }
    for (size_t J = 0; J != B->Ops.size(); ++J)
      if (auto It = Operations.find(&B->Ops[J]); It != Operations.end()) {
        for (size_t I : It->second) {
          Node &N = Nodes[I];
          N.Block = Id;
          N.Position = static_cast<int>(J);
          BlockNodes[Id].push_back(I);
        }
      }
  }

  // One virtual-root dominator tree authenticates all SSA uses against every
  // independent entry. A source reachable through another root is not a
  // dominating initialization merely because lookupDef found a constant.
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Function> Graph(llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
      llvm::GlobalValue::ExternalLinkage, "control_proof"));
  auto *VirtualRoot = llvm::BasicBlock::Create(Context, "entry", Graph.get());
  std::map<int, llvm::BasicBlock *> GraphBlocks;
  for (int Id : LiveBlocks)
    GraphBlocks.emplace(Id, llvm::BasicBlock::Create(Context, "", Graph.get()));
  auto link = [&](llvm::BasicBlock *From, const std::vector<int> &Targets) {
    if (Targets.empty()) {
      llvm::ReturnInst::Create(Context, From);
      return;
    }
    auto *Switch = llvm::SwitchInst::Create(
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0),
        GraphBlocks.at(Targets.front()), Targets.size() - 1, From);
    for (size_t I = 1; I != Targets.size(); ++I)
      Switch->addCase(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), I),
          GraphBlocks.at(Targets[I]));
  };
  std::vector<int> RootTargets;
  for (int Id : Roots)
    if (LiveBlocks.count(Id))
      RootTargets.push_back(Id);
  link(VirtualRoot, RootTargets);
  std::map<int, std::vector<int>> Successors;
  for (auto [From, To] : Edges)
    Successors[From].push_back(To);
  for (const auto &[Id, BB] : GraphBlocks)
    link(BB, Successors[Id]);
  llvm::DominatorTree Dominators(*Graph);
  auto dominatesUse = [&](const Node &Def, int Block, int Position) {
    if (Def.Block < 0)
      return true; // literal or unknown live-in
    if (!GraphBlocks.count(Def.Block) || !GraphBlocks.count(Block))
      return false;
    if (Def.Block == Block)
      return Def.Position < Position;
    return Dominators.dominates(GraphBlocks.at(Def.Block),
                                GraphBlocks.at(Block));
  };
  for (auto &[Id, B] : Branches)
    B.Dominates = dominatesUse(Nodes[B.Condition], Id, B.Position);
  for (size_t I = 0; I != Nodes.size(); ++I) {
    Node &N = Nodes[I];
    if (InteriorRoots.count(N.Block))
      N.Any = true;
    if (N.Phi) {
      const auto &B = *Blocks.at(N.Block);
      std::set<int> Incoming;
      for (const auto &[Pred, Value] : N.Phi->Args)
        Incoming.insert(Pred);
      if (Roots.count(N.Block) || N.Phi->ExceptionalEntry ||
          !B.ExceptionalPreds.empty() || Incoming.empty() ||
          Incoming.size() != N.Phi->Args.size() ||
          Incoming != OrdinaryPredecessors[N.Block] ||
          B.Preds.size() != Incoming.size() ||
          std::set<int>(B.Preds.begin(), B.Preds.end()) != Incoming)
        N.Any = true;
    }
    for (size_t J = 0; J != N.Inputs.size(); ++J) {
      auto &U = N.Inputs[J];
      const int UseBlock = N.Phi ? N.Phi->Args[J].first : N.Block;
      const int Position = N.Phi ? std::numeric_limits<int>::max() : N.Position;
      U.Dominates = dominatesUse(Nodes[U.Node], UseBlock, Position);
      if (U.Dominates)
        Nodes[U.Node].Users.push_back(I);
    }
  }

  // Cyclic arithmetic can generate the entire machine integer domain. Keep
  // it unknown structurally instead of imposing an enumeration budget.
  // PHIs, SELECTs and width views only transport or discard existing bits;
  // their finite literal seeds therefore reach a fixed point without a cap.
  std::vector<bool> Seen(Nodes.size());
  std::vector<size_t> Finish;
  for (size_t Start = 0; Start != Nodes.size(); ++Start) {
    if (Seen[Start])
      continue;
    std::vector<std::pair<size_t, size_t>> Work{{Start, 0}};
    Seen[Start] = true;
    while (!Work.empty()) {
      auto &[I, J] = Work.back();
      if (J == Nodes[I].Inputs.size()) {
        Finish.push_back(I);
        Work.pop_back();
      } else {
        const auto U = Nodes[I].Inputs[J++];
        if (U.Dominates && !Seen[U.Node]) {
          Seen[U.Node] = true;
          Work.emplace_back(U.Node, 0);
        }
      }
    }
  }
  std::fill(Seen.begin(), Seen.end(), false);
  for (auto It = Finish.rbegin(); It != Finish.rend(); ++It) {
    if (Seen[*It])
      continue;
    std::vector<size_t> Component, Work{*It};
    Seen[*It] = true;
    while (!Work.empty()) {
      const size_t I = Work.back();
      Work.pop_back();
      Component.push_back(I);
      for (size_t U : Nodes[I].Users)
        if (!Seen[U]) {
          Seen[U] = true;
          Work.push_back(U);
        }
    }
    for (size_t I : Component) {
      Node &N = Nodes[I];
      const bool Cyclic = Component.size() > 1 ||
                          std::any_of(N.Inputs.begin(), N.Inputs.end(),
                                      [&](Use U) { return U.Node == I; });
      if (!Cyclic || !N.Op)
        continue;
      switch (N.Op->Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::SUBBYTES:
      case NdOp::SELECT:
        break;
      default:
        N.Any = true;
        break;
      }
    }
  }

  auto merge = [](Domain &Into, const Domain &From) {
    if (!Into)
      return false;
    if (!From) {
      Into.reset();
      return true;
    }
    std::vector<uint64_t> Values;
    std::set_union(Into->begin(), Into->end(), From->begin(), From->end(),
                   std::back_inserter(Values));
    if (Values.size() == Into->size())
      return false;
    Into = std::move(Values);
    return true;
  };
  auto value = [&](Use U) -> const Domain & {
    return U.Dominates ? Nodes[U.Node].Values : Unknown;
  };
  std::set<int> Reachable, QueuedBlocks;
  std::set<std::pair<int, int>> Executable;
  std::vector<int> BlockWork;
  std::vector<size_t> ValueWork;
  auto queueNode = [&](size_t I) {
    if (!Nodes[I].Queued) {
      Nodes[I].Queued = true;
      ValueWork.push_back(I);
    }
  };
  auto queueBlock = [&](int Id) {
    if (QueuedBlocks.insert(Id).second)
      BlockWork.push_back(Id);
  };
  auto activate = [&](int Id) {
    if (!LiveBlocks.count(Id) || !Reachable.insert(Id).second)
      return;
    for (size_t I : BlockNodes[Id])
      queueNode(I);
    queueBlock(Id);
  };
  auto edge = [&](int From, int To) {
    if (!Edges.count({From, To}) || !Executable.insert({From, To}).second)
      return;
    activate(To);
    for (size_t I : BlockPhis[To])
      queueNode(I);
  };
  for (size_t I = 0; I != Nodes.size(); ++I)
    if (Nodes[I].Block < 0)
      queueNode(I);
  for (int Id : Roots)
    activate(Id);

  auto evaluate = [&](const Node &N) -> Domain {
    if (N.Any)
      return std::nullopt;
    if (N.Value.isConst())
      return N.Values;
    Domain Result = std::vector<uint64_t>{};
    auto atOutputWidth = [&]() {
      if (Result) {
        const unsigned Bits = N.Value.Size == 0 ? 64 : N.Value.Size * 8;
        const uint64_t Mask = ~uint64_t(0) >> (64 - Bits);
        for (auto &V : *Result)
          V &= Mask;
        std::sort(Result->begin(), Result->end());
        Result->erase(std::unique(Result->begin(), Result->end()),
                      Result->end());
      }
    };
    if (N.Phi) {
      for (size_t J = 0; J != N.Inputs.size(); ++J)
        if (Executable.count({N.Phi->Args[J].first, N.Block}))
          merge(Result, value(N.Inputs[J]));
      atOutputWidth();
      return Result;
    }
    if (!N.Op)
      return std::nullopt;
    if (N.Op->Opcode == NdOp::SELECT && N.Inputs.size() == 3) {
      const Domain &Condition = value(N.Inputs[0]);
      if (!Condition || std::any_of(Condition->begin(), Condition->end(),
                                    [](uint64_t V) { return V != 0; }))
        merge(Result, value(N.Inputs[1]));
      if (!Condition || std::find(Condition->begin(), Condition->end(), 0) !=
                            Condition->end())
        merge(Result, value(N.Inputs[2]));
      atOutputWidth();
      return Result;
    }
    for (Use U : N.Inputs)
      if (!value(U))
        return std::nullopt;
    for (Use U : N.Inputs)
      if (value(U)->empty())
        return Result;
    ControlValueBindings Bindings;
    std::function<bool(size_t)> Enumerate = [&](size_t J) {
      if (J == N.Inputs.size()) {
        auto V = traceControlConst(N.Value, &Bindings);
        if (!V)
          return false;
        Result->push_back(*V);
        return true;
      }
      const auto &Input = Nodes[N.Inputs[J].Node].Value;
      for (uint64_t V : *value(N.Inputs[J])) {
        Bindings.emplace_back(Input, V);
        const bool Valid = Enumerate(J + 1);
        Bindings.pop_back();
        if (!Valid)
          return false;
      }
      return true;
    };
    if (!Enumerate(0))
      return std::nullopt;
    std::sort(Result->begin(), Result->end());
    Result->erase(std::unique(Result->begin(), Result->end()), Result->end());
    return Result;
  };
  while (true) {
    while (!ValueWork.empty() || !BlockWork.empty()) {
      if (!ValueWork.empty()) {
        const size_t I = ValueWork.back();
        ValueWork.pop_back();
        Node &N = Nodes[I];
        N.Queued = false;
        if (N.Block >= 0 && !Reachable.count(N.Block))
          continue;
        if (merge(N.Values, evaluate(N))) {
          for (size_t U : N.Users)
            queueNode(U);
          for (int B : N.BranchUsers)
            queueBlock(B);
        }
      } else {
        const int Id = BlockWork.back();
        BlockWork.pop_back();
        QueuedBlocks.erase(Id);
        if (!Reachable.count(Id))
          continue;
        const auto &B = *Blocks.at(Id);
        if (auto It = Branches.find(Id); It != Branches.end()) {
          const auto &Branch = It->second;
          const auto &Condition =
              Branch.Dominates ? Nodes[Branch.Condition].Values : Unknown;
          if (!Condition || std::any_of(Condition->begin(), Condition->end(),
                                        [](uint64_t V) { return V != 0; }))
            edge(Id, Branch.Taken);
          if (!Condition || std::find(Condition->begin(), Condition->end(),
                                      0) != Condition->end())
            edge(Id, Branch.Fallthrough);
        } else {
          for (int S : B.Succs)
            edge(Id, S);
        }
        for (const auto &E : B.ExceptionalSuccs)
          edge(Id, E.BlockId);
      }
    }
    // A live unseeded cycle is unknown, not proof of an impossible condition.
    bool Pending = false;
    for (size_t I = 0; I != Nodes.size(); ++I) {
      Node &N = Nodes[I];
      if ((N.Block < 0 || Reachable.count(N.Block)) && N.Values &&
          N.Values->empty()) {
        N.Any = true;
        queueNode(I);
        Pending = true;
      }
    }
    if (!Pending)
      break;
  }
  Edges = std::move(Executable);
  LiveBlocks = std::move(Reachable);
}
} // namespace neverd
