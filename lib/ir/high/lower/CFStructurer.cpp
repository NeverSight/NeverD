//===- CFStructurer.cpp - Control-flow structuring ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Control-flow structuring for HighIR: block-level dispatch loop and PHI
/// copy insertion.  Individual NdOp lowering helpers live in:
///   NdOpLowering.cpp         — STORE, CALL, INTRINSIC, COND_BR, BRANCH,
///                               generic-assign
///   NdOpCallIndLowering.cpp  — INDIR_CALL target resolution and lowering
///   NdOpReturnLowering.cpp   — RETURN value recovery
///   NdOpSwitchRecovery.cpp   — INDIR_BR / jump-table switch recovery
///
//===----------------------------------------------------------------------===//

#include "CompareTreeSwitch.h"

#include "neverd/Limits.h"
#include "neverd/ir/high/MedToHigh.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>

namespace neverd {

//===----------------------------------------------------------------------===//
// insertPhiCopies
//===----------------------------------------------------------------------===//

std::vector<HighStmt>
MedToHighConverter::phiCopiesForEdge(int From, int To, va_t At,
                                     const PhiCopyMap &PhiCopies) {
  std::vector<HighStmt> Copies;
  // A shared dispatch's selector, which no PHI reads, is set last.
  const auto Selector = DispatchSelectorCopies.find({From, To});
  auto It = PhiCopies.find({From, To});
  if (It == PhiCopies.end()) {
    if (Selector != DispatchSelectorCopies.end())
      Copies = Selector->second;
    return Copies;
  }
  VarKeySet Destinations;
  for (const auto &[Output, Argument] : It->second)
    Destinations.insert(varKey(Output));
  std::vector<HighStmt> Writes;
  for (const auto &[Output, Argument] : It->second) {
    if (Output == Argument)
      continue;
    HighStmt Copy;
    Copy.Kind = StmtKind::Assign;
    Copy.Addr = At;
    Copy.IsPhiCopy = true;
    Copy.Dst = HighExpr::makeVar(Output);
    // A join PHI's incoming value reads as in any other statement: a
    // definition only the PHI uses is inlined, and a value read more than
    // once stays the variable its assignment names.  Inlining that one too
    // would repeat a shared expression once per read.
    Copy.Val = medvarToExpr(Argument);
    bool ReadsDestination = false;
    std::set<const HighExpr *> Seen;
    std::function<void(const ExprPtr &)> Visit =
        [&](const ExprPtr &Expression) {
          if (!Expression || !Seen.insert(Expression.get()).second)
            return;
          if (Expression->Kind == ExprKind::Var &&
              Destinations.count(varKey(Expression->Var)))
            ReadsDestination = true;
          for (const auto &Operand : Expression->Operands)
            Visit(Operand);
        };
    Visit(Copy.Val);
    if (ReadsDestination) {
      // PHIs read their predecessor values simultaneously. Capture any
      // expression using an overwritten PHI before publishing edge writes;
      // this also handles cycles such as a <- b, b <- a.
      MedVar Snapshot;
      Snapshot.Kind = MedVar::Temp;
      Snapshot.Id = NextHighTempId++;
      Snapshot.Size = Output.Size;
      HighStmt Capture = Copy;
      Capture.Dst = HighExpr::makeVar(Snapshot, Copy.Val->Type);
      Copies.push_back(Capture);
      Copy.Val = Capture.Dst;
    }
    Writes.push_back(std::move(Copy));
  }
  Copies.insert(Copies.end(), Writes.begin(), Writes.end());
  if (Selector != DispatchSelectorCopies.end())
    Copies.insert(Copies.end(), Selector->second.begin(),
                  Selector->second.end());
  return Copies;
}

void MedToHighConverter::insertPhiCopies(HighFunc &Func,
                                         const MedBlock &CurBlock, int BlkIdx,
                                         size_t BlkBodyStart,
                                         const PhiCopyMap &PhiCopies) {
  const va_t CopyAddr =
      CurBlock.Ops.empty() ? CurBlock.StartAddr : CurBlock.Ops.back().Addr;
  auto CopiesFor = [&](int Successor) {
    return phiCopiesForEdge(BlkIdx, Successor, CopyAddr, PhiCopies);
  };

  size_t BranchIndex = Func.Body.size();
  for (size_t I = Func.Body.size(); I > BlkBodyStart; --I)
    if (Func.Body[I - 1].Kind == StmtKind::Goto ||
        Func.Body[I - 1].Kind == StmtKind::If ||
        Func.Body[I - 1].Kind == StmtKind::Switch) {
      BranchIndex = I - 1;
      break;
    }
  for (int Successor : CurBlock.Succs) {
    if (!CurMed || Successor < 0 || Successor >= int(CurMed->Blocks.size()))
      continue;
    auto Copies = CopiesFor(Successor);
    if (Copies.empty())
      continue;
    const auto &TargetBlock = CurMed->Blocks[Successor];
    va_t Target = TargetBlock.StartAddr;
    if (!Target && !TargetBlock.Ops.empty())
      Target = TargetBlock.Ops.front().Addr;
    va_t TargetEnd = TargetBlock.EndAddr;
    if (!TargetEnd)
      TargetEnd = TargetBlock.Ops.empty() ? Target + 1
                                          : TargetBlock.Ops.back().Addr + 1;
    auto gotoHitsSuccessor = [&](va_t GotoTarget) {
      if (!GotoTarget || GotoTarget == InvalidVA)
        return false;
      if (GotoTarget == Target)
        return true;
      return Target && GotoTarget >= Target && GotoTarget < TargetEnd;
    };
    if (BranchIndex < Func.Body.size()) {
      auto &Branch = Func.Body[BranchIndex];
      if (Branch.Kind == StmtKind::Switch) {
        auto Insert = [&](std::vector<HighStmt> &Body) {
          if (!Body.empty() && Body.back().Kind == StmtKind::Goto &&
              gotoHitsSuccessor(Body.back().GotoTarget)) {
            Body.insert(Body.end() - 1, Copies.begin(), Copies.end());
            return true;
          }
          return false;
        };
        bool Found = Insert(Branch.DefaultBody);
        for (auto &Case : Branch.Cases)
          Found |= Insert(Case.Body);
        if (!Found) {
          // A CFG edge with no dispatch binding cannot execute its PHIs on
          // an arbitrary case. Retain an explicit unresolved transfer so
          // source validation rejects the incomplete switch.
          HighStmt Unresolved;
          Unresolved.Kind = StmtKind::Goto;
          Unresolved.GotoTarget = InvalidVA;
          Branch.DefaultBody = {std::move(Unresolved)};
        }
        continue;
      }
      if (Branch.Kind == StmtKind::If && !Branch.Body.empty() &&
          Branch.Body.back().Kind == StmtKind::Goto &&
          gotoHitsSuccessor(Branch.Body.back().GotoTarget)) {
        Branch.Body.insert(Branch.Body.end() - 1, Copies.begin(), Copies.end());
        continue;
      }
      if (Branch.Kind == StmtKind::Goto &&
          gotoHitsSuccessor(Branch.GotoTarget)) {
        Func.Body.insert(Func.Body.begin() + BranchIndex, Copies.begin(),
                         Copies.end());
        BranchIndex += Copies.size();
        continue;
      }
    }
    // The untaken conditional edge (or the sole fallthrough edge) executes
    // its copies after the branch. No loop-backedge write occurs on an exit.
    Func.Body.insert(Func.Body.end(), Copies.begin(), Copies.end());
  }
}

//===----------------------------------------------------------------------===//
// lowerCompareTreeSwitch
//===----------------------------------------------------------------------===//

void MedToHighConverter::lowerCompareTreeSwitch(HighFunc &Func,
                                                const MedFunc &Med,
                                                const CompareTreeSwitch &Tree,
                                                const PhiCopyMap &PhiCopies,
                                                const VarKeySet &PhiArgVars) {
  // Everything the dispatch evaluates stays at the root's branch, inside the
  // root block, wherever the folded blocks lie.
  const va_t At = Med.Blocks[Tree.Root].Ops.back().Addr;
  for (int Id : Tree.Interior) {
    const MedBlock &Block = Med.Blocks[Id];
    const size_t First = Func.Body.size();
    for (size_t OpIdx = 0; OpIdx + 1 < Block.Ops.size(); ++OpIdx)
      if (Block.Ops[OpIdx].Opcode != NdOp::NOP)
        lowerGenericAssign(Func, Block.Ops[OpIdx], PhiArgVars);
    for (size_t K = First; K < Func.Body.size(); ++K)
      Func.Body[K].Addr = At;
  }
  auto Transfer = [&](const CompareTreeEdge &Edge) {
    std::vector<HighStmt> Body =
        phiCopiesForEdge(Edge.From, Edge.To, At, PhiCopies);
    const MedBlock &Target = Med.Blocks[Edge.To];
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = Target.StartAddr     ? Target.StartAddr
                      : Target.Ops.empty() ? 0
                                           : Target.Ops.front().Addr;
    Body.push_back(std::move(Jump));
    return Body;
  };
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.Addr = At;
  Switch.SwitchExpr = medvarToExpr(Tree.Selector);
  for (const CompareTreeEdge &Edge : Tree.Cases)
    for (size_t I = 0; I < Edge.Values.size(); ++I) {
      SwitchCase Case;
      Case.Value = Edge.Values[I];
      // Values with one target share its body: `case A: case B: ...`.
      if (I + 1 < Edge.Values.size())
        Case.FallsThrough = true;
      else
        Case.Body = Transfer(Edge);
      Switch.Cases.push_back(std::move(Case));
    }
  Switch.DefaultBody = Transfer(Tree.Default);
  Func.Body.push_back(std::move(Switch));
}

//===----------------------------------------------------------------------===//
// pullCompareTreeCases
//===----------------------------------------------------------------------===//

void MedToHighConverter::pullCompareTreeCases(HighFunc &Func,
                                              const MedFunc &Med) {
  if (CaseRegions.empty())
    return;
  // The block holding each address.
  std::vector<std::tuple<va_t, va_t, int>> Ranges;
  for (const MedBlock &Block : Med.Blocks) {
    const va_t Start = Block.StartAddr     ? Block.StartAddr
                       : Block.Ops.empty() ? 0
                                           : Block.Ops.front().Addr;
    const va_t End = Block.EndAddr       ? Block.EndAddr
                     : Block.Ops.empty() ? Start
                                         : Block.Ops.back().Addr + 1;
    if (Start && End > Start)
      Ranges.emplace_back(Start, End, Block.Id);
  }
  std::sort(Ranges.begin(), Ranges.end());
  auto BlockAt = [&](va_t Addr) {
    auto It = std::upper_bound(
        Ranges.begin(), Ranges.end(), Addr,
        [](va_t A, const auto &R) { return A < std::get<0>(R); });
    if (It == Ranges.begin())
      return -1;
    --It;
    return Addr < std::get<1>(*It) ? std::get<2>(*It) : -1;
  };
  auto Addressed = [](va_t A) { return A != 0 && A != InvalidVA; };
  // The list holding the switch at \p At, and its index there.
  std::function<std::vector<HighStmt> *(std::vector<HighStmt> &, va_t,
                                        size_t &)>
      Find = [&](std::vector<HighStmt> &L, va_t At,
                 size_t &Index) -> std::vector<HighStmt> * {
    for (size_t I = 0; I < L.size(); ++I) {
      if (L[I].Kind == StmtKind::Switch && L[I].Addr == At) {
        Index = I;
        return &L;
      }
      for (auto *List : {&L[I].Body, &L[I].ElseBody, &L[I].DefaultBody})
        if (auto *Found = Find(*List, At, Index))
          return Found;
      for (auto &C : L[I].Cases)
        if (auto *Found = Find(C.Body, At, Index))
          return Found;
    }
    return nullptr;
  };
  for (const auto &[At, Regions] : CaseRegions) {
    size_t I = 0;
    std::vector<HighStmt> *Found = Find(Func.Body, At, I);
    if (!Found)
      continue;
    std::vector<HighStmt> &L = *Found;
    // Which case region each block belongs to.
    std::map<int, size_t> RegionOf;
    for (size_t R = 0; R < Regions.size(); ++R)
      for (int Block : Regions[R].second)
        RegionOf[Block] = R;
    // The region of each statement: its own address, else the one before.
    // A statement holding addresses of two regions, or of no block, makes
    // the move ambiguous; the switch then keeps its jumps.
    constexpr size_t None = SIZE_MAX;
    std::vector<size_t> Owner(L.size(), None);
    bool Ambiguous = false;
    size_t Prev = None;
    for (size_t K = 0; K < L.size() && !Ambiguous; ++K) {
      size_t Mine = Prev;
      bool Seen = false;
      std::function<void(const HighStmt &)> Check = [&](const HighStmt &S) {
        if (Addressed(S.Addr)) {
          const int Block = BlockAt(S.Addr);
          if (Block < 0) {
            Ambiguous = true;
            return;
          }
          auto It = RegionOf.find(Block);
          const size_t R = It == RegionOf.end() ? None : It->second;
          if (!Seen) {
            Mine = R;
            Seen = true;
          } else if (R != Mine) {
            Ambiguous = true;
          }
        }
        for (const auto *List : {&S.Body, &S.ElseBody, &S.DefaultBody})
          for (const HighStmt &T : *List)
            Check(T);
        for (const auto &C : S.Cases)
          for (const HighStmt &T : C.Body)
            Check(T);
      };
      if (K != I)
        Check(L[K]);
      else
        Mine = None;
      Owner[K] = Mine;
      Prev = Mine;
    }
    if (Ambiguous)
      continue;
    HighStmt &Switch = L[I];
    std::vector<std::vector<HighStmt> *> Units;
    for (SwitchCase &C : Switch.Cases)
      if (!C.FallsThrough)
        Units.push_back(&C.Body);
    Units.push_back(&Switch.DefaultBody);
    // Build every case body first; nothing changes unless all succeed.
    std::vector<std::pair<std::vector<HighStmt> *, std::vector<size_t>>> Moves;
    bool Failed = false;
    for (size_t R = 0; R < Regions.size() && !Failed; ++R) {
      const va_t Entry = Regions[R].first;
      std::vector<HighStmt> *Unit = nullptr;
      for (std::vector<HighStmt> *B : Units)
        if (!B->empty() && B->back().Kind == StmtKind::Goto &&
            B->back().GotoTarget == Entry)
          Unit = B;
      std::vector<size_t> Indices;
      for (size_t K = 0; K < L.size(); ++K)
        if (Owner[K] == R)
          Indices.push_back(K);
      if (!Unit || Indices.empty())
        continue;
      // A run that falls off its end continues at the next statement left
      // in place, which needs its own address to be jumped to.
      for (size_t N = 0; N < Indices.size() && !Failed; ++N) {
        const size_t K = Indices[N];
        if (N + 1 < Indices.size() && Indices[N + 1] == K + 1)
          continue;
        const StmtKind Last = L[K].Kind;
        if (Last == StmtKind::Goto || Last == StmtKind::Return ||
            Last == StmtKind::Break || Last == StmtKind::Continue)
          continue;
        if (K + 1 >= L.size() || !Addressed(L[K + 1].Addr) ||
            L[K + 1].Addr == L[K].Addr)
          Failed = true;
      }
      Moves.push_back({Unit, std::move(Indices)});
    }
    if (Failed || Moves.empty())
      continue;
    std::vector<bool> Moved(L.size(), false);
    for (auto &[Unit, Indices] : Moves) {
      const va_t Entry = Unit->back().GotoTarget;
      // The case enters its code at the target, which need not come first.
      if (L[Indices.front()].Addr == Entry)
        Unit->pop_back();
      for (size_t N = 0; N < Indices.size(); ++N) {
        const size_t K = Indices[N];
        Moved[K] = true;
        Unit->push_back(L[K]);
        if (N + 1 < Indices.size() && Indices[N + 1] == K + 1)
          continue;
        const StmtKind Last = L[K].Kind;
        if (Last == StmtKind::Goto || Last == StmtKind::Return ||
            Last == StmtKind::Break || Last == StmtKind::Continue)
          continue;
        HighStmt Jump;
        Jump.Kind = StmtKind::Goto;
        Jump.GotoTarget = L[K + 1].Addr;
        Unit->push_back(std::move(Jump));
      }
    }
    std::vector<HighStmt> Kept;
    Kept.reserve(L.size());
    for (size_t K = 0; K < L.size(); ++K)
      if (!Moved[K])
        Kept.push_back(std::move(L[K]));
    L = std::move(Kept);
  }
}

//===----------------------------------------------------------------------===//
// Block layout
//===----------------------------------------------------------------------===//

static va_t blockEntry(const MedBlock &Block) {
  return Block.StartAddr     ? Block.StartAddr
         : Block.Ops.empty() ? 0
                             : Block.Ops.front().Addr;
}

/// The block \p Block runs into when it does not branch: -1 when it never
/// does, nullopt when its edges do not say which successor that is.
static std::optional<int> fallThroughOf(const MedFunc &Med,
                                        const MedBlock &Block) {
  const auto Valid = [&](int Id) {
    return Id >= 0 && Id < static_cast<int>(Med.Blocks.size()) &&
           Med.Blocks[Id].Id == Id;
  };
  if (!std::all_of(Block.Succs.begin(), Block.Succs.end(), Valid))
    return std::nullopt;
  if (!Block.Ops.empty()) {
    const MedOp &Last = Block.Ops.back();
    switch (Last.Opcode) {
    case NdOp::RETURN:
    case NdOp::BRANCH:
    case NdOp::INDIR_BR:
      return -1;
    case NdOp::COND_BR: {
      if (Last.NumInputs < 2 || !Last.Inputs[0].isConst())
        return std::nullopt;
      const va_t Taken = Last.Inputs[0].ConstVal;
      // Both edges reach one block only when the branch targets the
      // instruction after it.
      if (Block.Succs.size() == 1)
        return blockEntry(Med.Blocks[Block.Succs[0]]) == Taken &&
                       Block.EndAddr == Taken
                   ? std::optional<int>(Block.Succs[0])
                   : std::nullopt;
      if (Block.Succs.size() != 2)
        return std::nullopt;
      int Other = -1, Hits = 0;
      for (int Successor : Block.Succs) {
        if (blockEntry(Med.Blocks[Successor]) == Taken)
          ++Hits;
        else
          Other = Successor;
      }
      return Hits == 1 && Other >= 0 ? std::optional<int>(Other) : std::nullopt;
    }
    default:
      break;
    }
  }
  if (Block.Succs.empty())
    return -1;
  if (Block.Succs.size() == 1)
    return Block.Succs.front();
  return std::nullopt;
}

/// Whether \p EH describes regions that structureExceptionRegions wraps by
/// address, so the blocks must stay in address order.  A frame that only
/// unwinds -- the plain stack-cookie check, or a personality NeverD does not
/// know and so reads no table for -- has nothing to wrap.
static bool hasExceptionRegions(const ExceptionFunction &EH) {
  if (EH.PersonalityVA == 0)
    return false;
  const bool UnwindOnly =
      (EH.Personality == ExceptionPersonality::Unknown ||
       EH.Personality == ExceptionPersonality::GSHandlerCheck) &&
      (!EH.SEH || EH.SEH->Scopes.empty()) && !EH.Cxx && !EH.Itanium;
  return !UnwindOnly;
}

/// The emission order that follows block addresses.  MedIR numbers blocks by
/// address, except that a block split out of the guest instruction ending its
/// only predecessor (an ARM predicated effect, MedPredicatedEffects.cpp) is
/// appended last, and its entry is that instruction's address.  A jump to it
/// would name the instruction, whose first statement belongs to the
/// predecessor, so the split block follows its predecessor instead.
static std::vector<int> addressOrderLayout(const MedFunc &Med) {
  const int Count = static_cast<int>(Med.Blocks.size());
  std::vector<int> Follower(Count, -1);
  std::vector<bool> Split(Count, false);
  for (int I = 0; I < Count; ++I) {
    const MedBlock &Block = Med.Blocks[I];
    if (Block.Id != I || Block.Preds.size() != 1 || Block.StartAddr == 0)
      continue;
    const int Pred = Block.Preds.front();
    if (Pred < 0 || Pred >= Count || Pred == I || Follower[Pred] != -1)
      continue;
    const MedBlock &Guard = Med.Blocks[Pred];
    if (Guard.Ops.empty() || Guard.Ops.back().Addr != Block.StartAddr)
      continue;
    Follower[Pred] = I;
    Split[I] = true;
  }
  std::vector<int> Order;
  Order.reserve(Count);
  std::vector<bool> Placed(Count, false);
  for (int I = 0; I < Count; ++I) {
    if (Split[I])
      continue;
    for (int B = I; B != -1 && !Placed[B]; B = Follower[B]) {
      Placed[B] = true;
      Order.push_back(B);
    }
  }
  // A split block whose guard is itself unplaced would close a cycle; keep
  // it in index order rather than drop it.
  for (int I = 0; I < Count; ++I)
    if (!Placed[I])
      Order.push_back(I);
  return Order;
}

std::vector<int> highBlockLayout(const MedFunc &Med,
                                 const std::set<int> &Dispatched,
                                 bool SEHReversePostorder) {
  const int Count = static_cast<int>(Med.Blocks.size());
  const std::vector<int> AddressOrder = addressOrderLayout(Med);
  const ExceptionFunction *EH =
      Med.ExceptionMetadata ? &*Med.ExceptionMetadata : nullptr;
  const bool SEHOnly = SEHReversePostorder && EH && EH->SEH &&
                       !EH->SEH->Scopes.empty() && !EH->Cxx && !EH->Itanium &&
                       !EH->Registration;
  if (Count < 3 ||
      Count > static_cast<int>(limits::kMaxStructurableMedBlocks) ||
      (EH && hasExceptionRegions(*EH) && !SEHOnly))
    return AddressOrder;
  for (int I = 0; I < Count; ++I) {
    const MedBlock &Block = Med.Blocks[I];
    if (Block.Id != I || (!SEHOnly && !Block.ExceptionalSuccs.empty()) ||
        (!Dispatched.count(I) && !fallThroughOf(Med, Block)))
      return AddressOrder;
  }
  if (blockEntry(Med.Blocks[0]) != Med.Entry)
    return AddressOrder;

  std::vector<std::vector<int>> Successors(Count);
  for (int I = 0; I < Count; ++I) {
    Successors[I] = Med.Blocks[I].Succs;
    std::sort(Successors[I].begin(), Successors[I].end(), [&](int A, int B) {
      const va_t X = blockEntry(Med.Blocks[A]), Y = blockEntry(Med.Blocks[B]);
      return X != Y ? X > Y : A > B;
    });
    Successors[I].erase(std::unique(Successors[I].begin(), Successors[I].end()),
                        Successors[I].end());
  }
  std::vector<int> Post;
  Post.reserve(Count);
  std::vector<bool> Seen(Count);
  std::vector<std::pair<int, size_t>> Stack{{0, 0}};
  Seen[0] = true;
  while (!Stack.empty()) {
    auto &[Block, Next] = Stack.back();
    if (Next < Successors[Block].size()) {
      const int Successor = Successors[Block][Next++];
      if (!Seen[Successor]) {
        Seen[Successor] = true;
        Stack.push_back({Successor, 0});
      }
      continue;
    }
    Post.push_back(Block);
    Stack.pop_back();
  }
  std::vector<int> Order(Post.rbegin(), Post.rend());
  for (int I = 0; I < Count; ++I)
    if (!Seen[I])
      Order.push_back(I);
  if (SEHOnly) {
    // A __try wraps one run of statements, so each guarded range's blocks
    // must stay next to each other.
    std::vector<int> Position(Count);
    for (int P = 0; P < Count; ++P)
      Position[Order[P]] = P;
    for (const SEHScopeRecord &Scope : EH->SEH->Scopes) {
      if (!Scope.GuardedRange.isValid())
        return AddressOrder;
      int First = Count, Last = -1, Members = 0;
      for (int I = 0; I < Count; ++I)
        if (Scope.GuardedRange.contains(blockEntry(Med.Blocks[I]))) {
          First = std::min(First, Position[I]);
          Last = std::max(Last, Position[I]);
          ++Members;
        }
      if (Members && Last - First + 1 != Members)
        return AddressOrder;
    }
  }
  return Order;
}

//===----------------------------------------------------------------------===//
// structureControlFlow — block-level dispatch
//===----------------------------------------------------------------------===//

void MedToHighConverter::structureControlFlow(HighFunc &Func,
                                              const MedFunc &Med) {
  PhiCopyMap PhiCopies;
  for (auto &Block : Med.Blocks)
    for (auto &Phi : Block.Phis)
      for (auto &[PredId, Arg] : Phi.Args)
        PhiCopies[{PredId, Block.Id}].push_back({Phi.Output, Arg});

  VarKeySet PhiArgVars;
  for (auto &Block : Med.Blocks)
    for (auto &Phi : Block.Phis)
      for (auto &[PredId, Arg] : Phi.Args)
        if (Arg.Id >= 0)
          PhiArgVars.insert(varKey(Arg));

  std::vector<std::pair<size_t, va_t>> MissingEntries;

  // A function too large to structure keeps its branches: a switch there
  // only trades them for as many case jumps.
  size_t OpCount = 0;
  for (const MedBlock &Block : Med.Blocks)
    OpCount += Block.Ops.size();
  const std::vector<CompareTreeSwitch> Trees =
      OpCount > limits::kMaxStructuredHighStmts
          ? std::vector<CompareTreeSwitch>()
          : findCompareTreeSwitches(Med);
  std::map<int, const CompareTreeSwitch *> TreeAt;
  std::set<int> TreeInterior;
  CaseRegions.clear();
  auto EntryOf = [](const MedBlock &Block) -> va_t {
    return Block.StartAddr     ? Block.StartAddr
           : Block.Ops.empty() ? 0
                               : Block.Ops.front().Addr;
  };
  for (const CompareTreeSwitch &Tree : Trees) {
    TreeAt.emplace(Tree.Root, &Tree);
    TreeInterior.insert(Tree.Interior.begin(), Tree.Interior.end());
    auto &Regions = CaseRegions[Med.Blocks[Tree.Root].Ops.back().Addr];
    for (const CompareTreeEdge &Edge : Tree.Cases)
      if (!Edge.Region.empty())
        Regions.push_back({EntryOf(Med.Blocks[Edge.To]), Edge.Region});
    if (!Tree.Default.Region.empty())
      Regions.push_back(
          {EntryOf(Med.Blocks[Tree.Default.To]), Tree.Default.Region});
  }

  planDispatchSelectors(Med);

  std::set<int> Dispatched = TreeInterior;
  for (const auto &[Root, Tree] : TreeAt)
    Dispatched.insert(Root);
  const std::vector<int> Order =
      highBlockLayout(Med, Dispatched, !SEHAddressOrder);
  // The block emitted after each position: a fall-through edge to any other
  // block needs an explicit jump.
  std::vector<int> NextEmitted(Order.size(), -1);
  for (size_t Pos = Order.size(); Pos-- > 1;)
    NextEmitted[Pos - 1] =
        TreeInterior.count(Order[Pos]) ? NextEmitted[Pos] : Order[Pos];

  for (size_t Pos = 0; Pos < Order.size(); ++Pos) {
    const int BlkIdx = Order[Pos];
    // A compare tree's interior blocks run at its root, inside its switch.
    if (TreeInterior.count(BlkIdx))
      continue;
    const int Next = NextEmitted[Pos];
    auto &CurBlock = Med.Blocks[BlkIdx];
    auto TreeIt = TreeAt.find(BlkIdx);
    const CompareTreeSwitch *Tree =
        TreeIt == TreeAt.end() ? nullptr : TreeIt->second;
    size_t BlkBodyStart = Func.Body.size();
    const size_t OpEnd = CurBlock.Ops.size() - (Tree ? 1 : 0);
    for (size_t OpIdx = 0; OpIdx < OpEnd; ++OpIdx) {
      auto &CurOp = CurBlock.Ops[OpIdx];
      if (ReturnProtectionOps.count(&CurOp) ||
          AArch64CallAuthentications.contains(&CurOp))
        continue;
      const size_t StatementBegin = Func.Body.size();
      switch (CurOp.Opcode) {
      case NdOp::STORE:
        // A prologue save keeps the caller's register, which no source reads,
        // and a get-PC call pushes the return address only for its pop, as a
        // spill of a PC-rooted value is stored only for its reloads.
        if (!CurOp.PrologueSave &&
            !I386PcRootedStores.count({CurOp.Addr, CurOp.OriginSeq}))
          lowerStore(Func, CurOp);
        break;
      case NdOp::CALL:
        lowerCall(Func, CurBlock, CurOp);
        break;
      case NdOp::INTRINSIC:
        lowerIntrinsic(Func, CurBlock, CurOp, OpIdx);
        break;
      case NdOp::INDIR_CALL:
        lowerCallInd(Func, CurBlock, CurOp);
        break;
      case NdOp::RETURN:
        lowerReturn(Func, CurBlock, CurOp, Med);
        break;
      case NdOp::COND_BR:
        lowerCBranch(Func, CurOp);
        break;
      case NdOp::BRANCH:
        lowerBranch(Func, CurOp);
        break;
      case NdOp::INDIR_BR:
        lowerBranchInd(Func, CurBlock, CurOp, Med);
        break;
      case NdOp::NOP:
        break;
      default:
        lowerGenericAssign(Func, CurOp, PhiArgVars);
        break;
      }
      if (StatementObserver && CurOp.Addr != InvalidVA && CurOp.OriginSeq >= 0)
        for (size_t I = StatementBegin; I < Func.Body.size(); ++I)
          if (Func.Body[I].Addr == CurOp.Addr && !Func.Body[I].IsPhiCopy)
            StatementObserver(CurOp, Func.Body[I]);
    }

    if (Tree)
      lowerCompareTreeSwitch(Func, Med, *Tree, PhiCopies, PhiArgVars);
    else
      insertPhiCopies(Func, CurBlock, BlkIdx, BlkBodyStart, PhiCopies);
    // The MedIR CFG may thread an empty branch block out of the false edge.
    // Its successor then need not be the next block in source order. Preserve
    // that transfer after the false-edge PHI copies, using the explicit taken
    // target to identify the other edge rather than relying on Succs order.
    if (!Tree && !CurBlock.Ops.empty() && CurBlock.Succs.size() == 2) {
      const auto &Terminator = CurBlock.Ops.back();
      if (Terminator.Opcode == NdOp::COND_BR && Terminator.NumInputs >= 2 &&
          Terminator.Inputs[0].isConst()) {
        int Taken = -1, Other = -1;
        bool Complete = true;
        for (int Successor : CurBlock.Succs) {
          if (Successor < 0 ||
              Successor >= static_cast<int>(Med.Blocks.size()) ||
              Med.Blocks[Successor].Id != Successor) {
            Complete = false;
            break;
          }
          const auto &Target = Med.Blocks[Successor];
          const va_t Address =
              Target.StartAddr
                  ? Target.StartAddr
                  : (Target.Ops.empty() ? 0 : Target.Ops.front().Addr);
          if (Address == Terminator.Inputs[0].ConstVal) {
            Complete &= Taken == -1;
            Taken = Successor;
          } else {
            Complete &= Other == -1;
            Other = Successor;
          }
        }
        if (Complete && Taken >= 0 && Other >= 0 && Other != Next) {
          const auto &Target = Med.Blocks[Other];
          HighStmt Transfer;
          Transfer.Kind = StmtKind::Goto;
          Transfer.GotoTarget =
              Target.StartAddr
                  ? Target.StartAddr
                  : (Target.Ops.empty() ? 0 : Target.Ops.front().Addr);
          Func.Body.push_back(std::move(Transfer));
        }
      }
    }
    // A block without a terminator falls through to its sole successor.
    // Threading an empty branch block out of that edge (cold code ending in a
    // `jmp` back to the hot path) leaves a successor that need not be the next
    // block in source order; transfer to it explicitly.
    // A conditional branch to the instruction after it also falls through
    // there.
    if (!Tree && CurBlock.Succs.size() == 1) {
      const int Successor = CurBlock.Succs.front();
      const bool Terminated =
          !CurBlock.Ops.empty() &&
          (CurBlock.Ops.back().Opcode == NdOp::BRANCH ||
           (CurBlock.Ops.back().Opcode == NdOp::COND_BR &&
            fallThroughOf(Med, CurBlock) != std::optional<int>(Successor)) ||
           CurBlock.Ops.back().Opcode == NdOp::INDIR_BR ||
           CurBlock.Ops.back().Opcode == NdOp::RETURN);
      if (!Terminated && Successor != Next && Successor >= 0 &&
          Successor < static_cast<int>(Med.Blocks.size()) &&
          Med.Blocks[Successor].Id == Successor) {
        HighStmt Transfer;
        Transfer.Kind = StmtKind::Goto;
        Transfer.GotoTarget = EntryOf(Med.Blocks[Successor]);
        Func.Body.push_back(std::move(Transfer));
      }
    }
    const va_t Entry =
        CurBlock.StartAddr
            ? CurBlock.StartAddr
            : (CurBlock.Ops.empty() ? 0 : CurBlock.Ops.front().Addr);
    if (Entry && Entry != InvalidVA &&
        std::none_of(Func.Body.begin() + BlkBodyStart, Func.Body.end(),
                     [Entry](const HighStmt &S) { return S.Addr == Entry; }))
      MissingEntries.emplace_back(BlkBodyStart, Entry);
  }

  std::set<va_t> BranchEntries;
  walkStmts(Func.Body, [&](const HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Goto)
      BranchEntries.insert(Statement.GotoTarget);
  });
  // A block no branch reaches is entered from outside the CFG (an __except
  // handler runs from the exception dispatcher); it needs the same anchor.
  for (size_t B = 1; B < Med.Blocks.size(); ++B)
    if (Med.Blocks[B].Preds.empty())
      BranchEntries.insert(Med.Blocks[B].StartAddr);
  for (auto It = MissingEntries.rbegin(); It != MissingEntries.rend(); ++It) {
    if (!BranchEntries.count(It->second))
      continue;
    // Inlining a block's initial argument copies must not erase its branch
    // entry. An empty block carries that exact label through DCE and emission,
    // even when the surviving call lies after a return in physical layout.
    HighStmt Label;
    Label.Kind = StmtKind::Block;
    Label.Addr = It->second;
    Func.Body.insert(Func.Body.begin() + It->first, std::move(Label));
  }
}

} // namespace neverd
