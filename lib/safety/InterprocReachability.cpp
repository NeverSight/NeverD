//===- InterprocReachability.cpp - Known-entry safety reachability --------===//

#include "neverd/safety/InterprocReachability.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/ArgSlicer.h"
#include "neverd/safety/EntryInputPolicy.h"
#include "neverd/safety/SinkCatalog.h"

#include "llvm/ADT/StringSwitch.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <utility>

using namespace neverd;
using namespace neverd::safety;

namespace {

constexpr unsigned kDefaultMaxCallDepth = 64;

bool isApplicationEntry(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define SAFETY_ENTRY_NAME(NAME) .Case(NAME, true)
#include "neverd/safety/SafetyEntryNames.inc"
#undef SAFETY_ENTRY_NAME
      .Case("WinMain", true)
      .Case("wWinMain", true)
      .Default(false);
}

va_t canonical(const BinaryImage *Image, va_t Address) {
  return Image ? normalizeCodeAddress(Address, Image->Arch, Image->Mode)
               : Address;
}

unsigned entryPriority(SafetyEntryKind Kind) {
  switch (Kind) {
  case SafetyEntryKind::Application:
    return 0;
  case SafetyEntryKind::Image:
    return 1;
  case SafetyEntryKind::Export:
    return 2;
  }
  return 3;
}

const MedOp *callOp(const MedFunc &Function, const MedCallInfo &Call) {
  for (const MedBlock &Block : Function.Blocks)
    if (Block.Id == Call.BlockId && Call.OpIdx >= 0 &&
        static_cast<size_t>(Call.OpIdx) < Block.Ops.size())
      return &Block.Ops[static_cast<size_t>(Call.OpIdx)];
  return nullptr;
}

using CallOccurrence = std::pair<int, int>;

struct ReachableCallInventory {
  std::set<CallOccurrence> Calls;
  bool Complete = true;
};

ReachableCallInventory reachableCalls(const MedFunc &Function) {
  ReachableCallInventory Result;
  if (Function.Blocks.empty()) {
    Result.Complete = Function.CallInfos.empty();
    return Result;
  }

  std::map<int, const MedBlock *> Blocks;
  for (const MedBlock &Block : Function.Blocks)
    if (!Blocks.emplace(Block.Id, &Block).second)
      Result.Complete = false;
  if (!Result.Complete)
    return Result;

  std::map<int, std::multiset<int>> NormalPreds;
  std::map<int, std::multiset<int>> ExceptionalPreds;
  for (const auto &[Id, Block] : Blocks) {
    (void)Block;
    NormalPreds.emplace(Id, std::multiset<int>{});
    ExceptionalPreds.emplace(Id, std::multiset<int>{});
  }
  for (const auto &[Id, Block] : Blocks) {
    for (int Succ : Block->Succs) {
      auto It = NormalPreds.find(Succ);
      if (It == NormalPreds.end()) {
        Result.Complete = false;
        continue;
      }
      It->second.insert(Id);
    }
    for (const ExceptionalEdge &Edge : Block->ExceptionalSuccs) {
      if (Edge.BlockId < 0)
        continue;
      auto It = ExceptionalPreds.find(Edge.BlockId);
      if (It == ExceptionalPreds.end()) {
        Result.Complete = false;
        continue;
      }
      It->second.insert(Id);
    }
  }
  for (const auto &[Id, Block] : Blocks) {
    const std::multiset<int> DeclaredNormal(Block->Preds.begin(),
                                            Block->Preds.end());
    std::multiset<int> DeclaredExceptional;
    for (const ExceptionalEdge &Edge : Block->ExceptionalPreds)
      if (Edge.BlockId >= 0)
        DeclaredExceptional.insert(Edge.BlockId);
    if (DeclaredNormal != NormalPreds[Id] ||
        DeclaredExceptional != ExceptionalPreds[Id])
      Result.Complete = false;
  }
  if (!Result.Complete)
    return Result;

  const MedBlock *Entry = nullptr;
  for (const auto &[Id, Block] : Blocks) {
    (void)Id;
    if (Block->StartAddr != Function.Entry)
      continue;
    if (Entry) {
      Result.Complete = false;
      return Result;
    }
    Entry = Block;
  }
  if (!Entry) {
    for (const auto &[Id, Block] : Blocks) {
      if (!NormalPreds[Id].empty() || !ExceptionalPreds[Id].empty())
        continue;
      if (Entry) {
        Result.Complete = false;
        return Result;
      }
      Entry = Block;
    }
  }
  if (!Entry) {
    Result.Complete = false;
    return Result;
  }

  std::set<int> Visited;
  std::deque<int> Queue{Entry->Id};
  while (!Queue.empty()) {
    const int BlockId = Queue.front();
    Queue.pop_front();
    if (!Visited.insert(BlockId).second)
      continue;
    const MedBlock &Block = *Blocks.at(BlockId);
    bool HasNormalContinuation = true;
    for (size_t OpIndex = 0; OpIndex < Block.Ops.size(); ++OpIndex) {
      const MedOp &Op = Block.Ops[OpIndex];
      if (Op.Dead)
        continue;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        Result.Calls.emplace(Block.Id, static_cast<int>(OpIndex));
      if (Op.Opcode == NdOp::RETURN || Op.DoesNotReturn) {
        HasNormalContinuation = false;
        break;
      }
      if (Op.Opcode == NdOp::INDIR_BR) {
        if (Block.Succs.empty() ||
            Function.UnsafeIndirectBranchAddresses.count(Op.Addr) != 0)
          Result.Complete = false;
        break;
      }
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
        if (Block.Succs.empty())
          Result.Complete = false;
        break;
      }
    }
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs)
      if (Edge.BlockId >= 0)
        Queue.push_back(Edge.BlockId);
    if (HasNormalContinuation)
      for (int Succ : Block.Succs)
        Queue.push_back(Succ);
  }
  return Result;
}

struct Edge {
  va_t Caller = 0;
  va_t Callee = 0;
  va_t CallVA = 0;
  bool Indirect = false;
  size_t CallIndex = 0;
};

struct Root {
  va_t Function = 0;
  va_t EntryVA = 0;
  std::string Name;
  SafetyEntryKind Kind = SafetyEntryKind::Image;
};

bool rootLess(const Root &Left, const Root &Right) {
  return std::make_tuple(entryPriority(Left.Kind), Left.Function, Left.EntryVA,
                         Left.Name) <
         std::make_tuple(entryPriority(Right.Kind), Right.Function,
                         Right.EntryVA, Right.Name);
}

ReachabilityWitness witnessOf(const FunctionReachability &Reach) {
  ReachabilityWitness Witness;
  Witness.RootFunctionVA = Reach.RootFunctionVA;
  Witness.EntryVA = Reach.EntryVA;
  Witness.EntryName = Reach.EntryName;
  Witness.Kind = Reach.Kind;
  Witness.CallChain = Reach.CallChain;
  return Witness;
}

bool witnessEndsAt(const ReachabilityWitness &Witness, va_t FunctionEntry) {
  if (Witness.CallChain.empty())
    return Witness.RootFunctionVA == FunctionEntry;
  va_t Current = Witness.RootFunctionVA;
  for (const ReachabilityCall &Call : Witness.CallChain) {
    if (Call.CallerVA != Current)
      return false;
    Current = Call.CalleeVA;
  }
  return Current == FunctionEntry;
}

auto callKey(const ReachabilityCall &Call) {
  return std::tie(Call.CallerVA, Call.CallVA, Call.CalleeVA, Call.Indirect);
}

bool callChainLess(const std::vector<ReachabilityCall> &Left,
                   const std::vector<ReachabilityCall> &Right) {
  return std::lexicographical_compare(
      Left.begin(), Left.end(), Right.begin(), Right.end(),
      [](const ReachabilityCall &A, const ReachabilityCall &B) {
        return callKey(A) < callKey(B);
      });
}

bool parameterFlowLess(const ParameterFlow &Left, const ParameterFlow &Right) {
  if (!Left.Witness)
    return false;
  if (!Right.Witness)
    return true;
  const ReachabilityWitness &A = *Left.Witness;
  const ReachabilityWitness &B = *Right.Witness;
  if (A.CallChain.size() != B.CallChain.size())
    return A.CallChain.size() < B.CallChain.size();
  if (entryPriority(A.Kind) != entryPriority(B.Kind))
    return entryPriority(A.Kind) < entryPriority(B.Kind);
  if (A.RootFunctionVA != B.RootFunctionVA)
    return A.RootFunctionVA < B.RootFunctionVA;
  if (A.EntryVA != B.EntryVA)
    return A.EntryVA < B.EntryVA;
  if (callChainLess(A.CallChain, B.CallChain))
    return true;
  if (callChainLess(B.CallChain, A.CallChain))
    return false;
  return std::tie(A.EntryName, Left.Source) <
         std::tie(B.EntryName, Right.Source);
}

} // namespace

const FunctionReachability *InterprocResult::findFunction(va_t Entry) const {
  auto It = Functions.find(Entry);
  return It == Functions.end() ? nullptr : &It->second;
}

const ParameterFlow *
InterprocResult::findParameter(va_t FunctionEntry,
                               size_t ParameterIndex) const {
  auto It = Parameters.find({FunctionEntry, ParameterIndex});
  return It == Parameters.end() ? nullptr : &It->second;
}

void InterprocResult::annotate(Finding &Record) const {
  Record.Reachability.AttackerControl = Record.Flow;
  const FunctionReachability *Reach = findFunction(Record.FuncEntry);
  if (!Reach) {
    Record.Reachability.Status = ReachabilityStatus::Unknown;
    Record.Reachability.Reason = "function is absent from the call inventory";
    return;
  }

  Record.Reachability.Status = Reach->Status;
  Record.Reachability.EntryVA = Reach->EntryVA;
  Record.Reachability.EntryName = Reach->EntryName;
  Record.Reachability.Kind = Reach->Kind;
  Record.Reachability.CallChain = Reach->CallChain;
  Record.Reachability.Reason = Reach->Reason;
  Record.Reachability.BudgetHit = Reach->BudgetHit;

  if (Record.Flow == ArgFlow::Tainted && Record.AttackerWitness &&
      witnessEndsAt(*Record.AttackerWitness, Record.FuncEntry)) {
    const ReachabilityWitness &Witness = *Record.AttackerWitness;
    Record.Reachability.Status = ReachabilityStatus::Reachable;
    Record.Reachability.EntryVA = Witness.EntryVA;
    Record.Reachability.EntryName = Witness.EntryName;
    Record.Reachability.Kind = Witness.Kind;
    Record.Reachability.CallChain = Witness.CallChain;
    Record.Reachability.Reason.clear();
  }

  if (SummaryBudgetHit) {
    Record.Reachability.BudgetHit = true;
    if (Record.Reachability.Reason.empty())
      Record.Reachability.Reason =
          "interprocedural attacker-control summary budget exhausted";
  }
}

InterprocResult
neverd::safety::analyzeInterprocedural(const AnalysisInput &In,
                                       const SinkCatalog &Catalog,
                                       const SafetyBudgets &Budgets) {
  InterprocResult Result;
  if (!In.MedFuncs || In.MedFuncs->empty()) {
    Result.GraphComplete = false;
    return Result;
  }

  std::vector<const MedFunc *> Candidates;
  Candidates.reserve(In.MedFuncs->size());
  for (const MedFunc &Function : *In.MedFuncs)
    Candidates.push_back(&Function);
  std::sort(Candidates.begin(), Candidates.end(),
            [](const MedFunc *Left, const MedFunc *Right) {
              return std::tie(Left->Entry, Left->Name) <
                     std::tie(Right->Entry, Right->Name);
            });

  std::map<va_t, const MedFunc *> ByOriginalEntry;
  std::vector<const MedFunc *> Ordered;
  for (const MedFunc *Function : Candidates) {
    if (!ByOriginalEntry.emplace(Function->Entry, Function).second) {
      Result.GraphComplete = false;
      continue;
    }
    Ordered.push_back(Function);
    Result.Functions.emplace(Function->Entry, FunctionReachability{});
  }

  std::map<va_t, va_t> OriginalByCanonicalEntry;
  std::set<va_t> AmbiguousCanonicalEntries;
  for (const MedFunc *Function : Ordered) {
    const va_t Key = canonical(In.Img, Function->Entry);
    if (AmbiguousCanonicalEntries.count(Key) != 0)
      continue;
    auto [It, Inserted] =
        OriginalByCanonicalEntry.emplace(Key, Function->Entry);
    if (!Inserted && It->second != Function->Entry) {
      OriginalByCanonicalEntry.erase(It);
      AmbiguousCanonicalEntries.insert(Key);
      Result.GraphComplete = false;
    }
  }

  std::map<va_t, std::vector<Edge>> Outgoing;
  std::map<va_t, bool> CallerGraphComplete;
  for (const MedFunc *Caller : Ordered) {
    const ReachableCallInventory Inventory = reachableCalls(*Caller);
    bool CallerComplete = Inventory.Complete;
    std::set<CallOccurrence> BoundOccurrences;
    for (size_t Index = 0; Index < Caller->CallInfos.size(); ++Index) {
      const MedCallInfo &Call = Caller->CallInfos[Index];
      const CallOccurrence Occurrence{Call.BlockId, Call.OpIdx};
      if (Inventory.Calls.count(Occurrence) == 0)
        continue;
      const MedOp *Op = callOp(*Caller, Call);
      if (!Op || (Op->Opcode != NdOp::CALL && Op->Opcode != NdOp::INDIR_CALL) ||
          !BoundOccurrences.insert(Occurrence).second) {
        CallerComplete = false;
        continue;
      }
      if (Call.TargetAddr == 0) {
        CallerComplete = false;
        continue;
      }
      const va_t Target = canonical(In.Img, Call.TargetAddr);
      if (AmbiguousCanonicalEntries.count(Target) != 0) {
        CallerComplete = false;
        continue;
      }
      auto Callee = OriginalByCanonicalEntry.find(Target);
      if (Callee == OriginalByCanonicalEntry.end()) {
        const Segment *Segment =
            In.Img ? In.Img->getSegmentFor(Target) : nullptr;
        if (Segment && Segment->isExecutable() &&
            !In.Img->isImportStubAt(Target))
          CallerComplete = false;
        continue;
      }
      Outgoing[Caller->Entry].push_back(
          {Caller->Entry, Callee->second, Op->Addr, Call.IsIndirect, Index});
    }
    for (const CallOccurrence &Occurrence : Inventory.Calls)
      if (BoundOccurrences.count(Occurrence) == 0)
        CallerComplete = false;
    CallerGraphComplete.emplace(Caller->Entry, CallerComplete);

    auto &Edges = Outgoing[Caller->Entry];
    std::sort(Edges.begin(), Edges.end(),
              [](const Edge &Left, const Edge &Right) {
                return std::tie(Left.CallVA, Left.Callee, Left.Indirect,
                                Left.CallIndex) <
                       std::tie(Right.CallVA, Right.Callee, Right.Indirect,
                                Right.CallIndex);
              });
  }

  std::map<va_t, Root> BestRoot;
  const auto offerRoot = [&](const MedFunc &Function, SafetyEntryKind Kind,
                             va_t EntryVA, llvm::StringRef Name) {
    const Root Candidate{Function.Entry, EntryVA, Name.str(), Kind};
    auto It = BestRoot.find(Function.Entry);
    if (It == BestRoot.end() || rootLess(Candidate, It->second))
      BestRoot[Function.Entry] = Candidate;
  };
  for (const MedFunc *Function : Ordered) {
    const va_t FunctionKey = canonical(In.Img, Function->Entry);
    if (AmbiguousCanonicalEntries.count(FunctionKey) != 0)
      continue;
    if (isApplicationEntry(Function->Name))
      offerRoot(*Function, SafetyEntryKind::Application, Function->Entry,
                Function->Name);
    const bool ImageHasEntry =
        In.Img &&
        (In.Img->Entry != 0 || In.Img->Format != BinaryFormat::Unknown);
    if (ImageHasEntry && canonical(In.Img, In.Img->Entry) == FunctionKey)
      offerRoot(*Function, SafetyEntryKind::Image, In.Img->Entry,
                Function->Name);
    if (In.Img)
      for (const Export &Export : In.Img->Exports)
        if (canonical(In.Img, Export.Addr) == FunctionKey)
          offerRoot(*Function, SafetyEntryKind::Export, Export.Addr,
                    Export.Name.empty() ? Function->Name : Export.Name);
  }

  std::vector<Root> Roots;
  Roots.reserve(BestRoot.size());
  for (const auto &[Function, Entry] : BestRoot) {
    (void)Function;
    Roots.push_back(Entry);
  }
  std::sort(Roots.begin(), Roots.end(), rootLess);

  const unsigned MaxDepth =
      Budgets.MaxCallDepth ? Budgets.MaxCallDepth : kDefaultMaxCallDepth;
  std::deque<va_t> Queue;
  for (const Root &Entry : Roots) {
    FunctionReachability &Reach = Result.Functions[Entry.Function];
    if (Reach.Status == ReachabilityStatus::Reachable)
      continue;
    Reach.Status = ReachabilityStatus::Reachable;
    Reach.RootFunctionVA = Entry.Function;
    Reach.EntryVA = Entry.EntryVA;
    Reach.EntryName = Entry.Name;
    Reach.Kind = Entry.Kind;
    Queue.push_back(Entry.Function);
  }

  bool DepthBudgetHit = false;
  while (!Queue.empty()) {
    const va_t Current = Queue.front();
    Queue.pop_front();
    auto Complete = CallerGraphComplete.find(Current);
    if (Complete == CallerGraphComplete.end() || !Complete->second)
      Result.GraphComplete = false;
    const FunctionReachability Parent = Result.Functions[Current];
    for (const Edge &Call : Outgoing[Current]) {
      FunctionReachability &Child = Result.Functions[Call.Callee];
      if (Child.Status == ReachabilityStatus::Reachable)
        continue;
      if (Parent.CallChain.size() >= MaxDepth) {
        DepthBudgetHit = true;
        continue;
      }
      Child.Status = ReachabilityStatus::Reachable;
      Child.RootFunctionVA = Parent.RootFunctionVA;
      Child.EntryVA = Parent.EntryVA;
      Child.EntryName = Parent.EntryName;
      Child.Kind = Parent.Kind;
      Child.CallChain = Parent.CallChain;
      Child.CallChain.push_back(
          {Call.Caller, Call.CallVA, Call.Callee, Call.Indirect});
      Queue.push_back(Call.Callee);
    }
  }

  for (auto &[Function, Reach] : Result.Functions) {
    (void)Function;
    if (Reach.Status == ReachabilityStatus::Reachable)
      continue;
    if (Roots.empty()) {
      Reach.Status = ReachabilityStatus::Unknown;
      Reach.Reason = "no known native entry was recovered";
    } else if (!Result.GraphComplete) {
      Reach.Status = ReachabilityStatus::Unknown;
      Reach.Reason = "internal call graph is incomplete";
    } else if (DepthBudgetHit) {
      Reach.Status = ReachabilityStatus::Unknown;
      Reach.Reason = "interprocedural call-depth budget exhausted";
      Reach.BudgetHit = true;
    } else {
      Reach.Status = ReachabilityStatus::Unreachable;
      Reach.Reason = "no path from a known entry";
    }
  }

  for (const Root &Entry : Roots) {
    if (Entry.Kind != SafetyEntryKind::Application)
      continue;
    const MedFunc *Function = ByOriginalEntry[Entry.Function];
    const FunctionReachability *Reach = Result.findFunction(Entry.Function);
    if (!Function || !Reach || Reach->Status != ReachabilityStatus::Reachable)
      continue;
    for (size_t Index = 0; Index < Function->Params.size(); ++Index) {
      std::optional<llvm::StringRef> Source =
          applicationEntryParameterSource(In, *Function, Index);
      if (!Source)
        continue;
      Result.Parameters[{Function->Entry, Index}] = {
          ArgFlow::Tainted, Source->str(), witnessOf(*Reach)};
    }
  }

  const unsigned DefaultRounds = MaxDepth + 1;
  const unsigned MaxRounds = Budgets.MaxSummaryIterations
                                 ? Budgets.MaxSummaryIterations
                                 : DefaultRounds;
  bool TaintDepthBudgetHit = false;
  const auto transfer = [&](const ParameterFlowMap &Current,
                            ParameterFlowMap &Next, bool RecordDepthBudget) {
    bool Changed = false;
    AnalysisInput FlowInput = In;
    FlowInput.ParameterFlows = &Current;
    for (const MedFunc *Caller : Ordered) {
      const FunctionReachability *CallerReach =
          Result.findFunction(Caller->Entry);
      if (!CallerReach || CallerReach->Status != ReachabilityStatus::Reachable)
        continue;
      for (const Edge &Call : Outgoing[Caller->Entry]) {
        const MedFunc *Callee = ByOriginalEntry[Call.Callee];
        if (!Callee || Call.CallIndex >= Caller->CallInfos.size() ||
            Callee->CC == CallingConv::Unknown)
          continue;
        const MedCallInfo &Info = Caller->CallInfos[Call.CallIndex];
        const size_t Count = std::min(Callee->Params.size(), Info.Args.size());
        for (size_t ArgIndex = 0; ArgIndex < Count; ++ArgIndex) {
          if (Info.VarArgFixedCount >= 0 &&
              ArgIndex >= static_cast<size_t>(Info.VarArgFixedCount))
            continue;
          const MedVar &Param = Callee->Params[ArgIndex];
          const MedVar &Argument = Info.Args[ArgIndex];
          if (Param.Size == 0 || Argument.Size == 0 ||
              Param.Size != Argument.Size)
            continue;
          ArgClassification Flow =
              classifyArgument(FlowInput, Catalog, *Caller, Call.CallIndex,
                               static_cast<int>(ArgIndex));
          if (Flow.Flow != ArgFlow::Tainted)
            continue;

          std::optional<ReachabilityWitness> Witness = Flow.AttackerWitness;
          if (!Witness || !witnessEndsAt(*Witness, Caller->Entry))
            Witness = witnessOf(*CallerReach);
          if (!Witness || !witnessEndsAt(*Witness, Caller->Entry))
            continue;
          const ParameterFlowKey Key{Callee->Entry, ArgIndex};
          if (Witness->CallChain.size() >= MaxDepth) {
            if (RecordDepthBudget && Next.find(Key) == Next.end())
              TaintDepthBudgetHit = true;
            continue;
          }
          Witness->CallChain.push_back(
              {Call.Caller, Call.CallVA, Call.Callee, Call.Indirect});
          ParameterFlow Candidate{ArgFlow::Tainted,
                                  Flow.TaintSource.empty() ? "entry"
                                                           : Flow.TaintSource,
                                  std::move(Witness)};
          auto It = Next.find(Key);
          if (It != Next.end() && !parameterFlowLess(Candidate, It->second))
            continue;
          Next[Key] = std::move(Candidate);
          Changed = true;
        }
      }
    }
    return Changed;
  };

  ParameterFlowMap Current = Result.Parameters;
  bool Converged = false;
  for (unsigned Round = 0; Round < MaxRounds; ++Round) {
    ParameterFlowMap Next = Current;
    if (!transfer(Current, Next, true)) {
      Converged = true;
      break;
    }
    Current = std::move(Next);
  }
  if (!Converged) {
    ParameterFlowMap Probe = Current;
    if (!transfer(Current, Probe, false))
      Converged = true;
  }
  Result.Parameters = std::move(Current);
  if (!Converged || TaintDepthBudgetHit) {
    Result.SummaryBudgetHit = true;
    Result.BudgetHit = true;
  }
  Result.BudgetHit |= DepthBudgetHit;
  return Result;
}
