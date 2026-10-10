//===- CFGBuilderException.cpp - Exceptional CFG edges -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Links exceptional successors and predecessors from normalized platform and
/// language-runtime exception metadata.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

void CFGBuilder::closeRegistrationCxxContinuations(const BinaryImage &Img,
                                                   Decoder &Dec,
                                                   LowFunc &Func) {
  if (Img.Arch != Arch::X86 || !Func.ExceptionMetadata ||
      !Func.ExceptionMetadata->Registration || !Func.ExceptionMetadata->Cxx ||
      !Func.RegistrationStates)
    return;

  std::set<va_t> AddedRoots;
  std::set<va_t> AddedPersistent;
  std::set<va_t> Quarantined;
  size_t WorkUsed = 0;
  bool Failed = false;
  auto Charge = [&](size_t Amount) {
    if (Amount > limits::kMaxRegistrationEHStateWork - WorkUsed) {
      Failed = true;
      return false;
    }
    WorkUsed += Amount;
    return true;
  };
  for (;;) {
    const auto Candidates = Func.RegistrationStates->CxxContinuations;
    if (!Charge(Candidates.size() + Func.Blocks.size() + 1))
      break;
    std::set<va_t> ProvenTargets;
    bool Grew = false;
    for (const RegistrationCxxContinuation &Continuation : Candidates) {
      const va_t Target = Continuation.TargetVA;
      if (Quarantined.count(Target) || !isOwnedInteriorTarget(Img, Target)) {
        Failed = true;
        continue;
      }
      ProvenTargets.insert(Target);
      if (!DurableCFGRoots.count(Target) || !BlockStarts.count(Target) ||
          !Insns.count(Target)) {
        if (!AddedRoots.count(Target) &&
            AddedRoots.size() == limits::kMaxRegistrationEHRecords) {
          Failed = true;
          break;
        }
        BlockStarts.insert(Target);
        if (DurableCFGRoots.insert(Target).second)
          AddedRoots.insert(Target);
        if (PersistentCFGRoots.insert(Target).second)
          AddedPersistent.insert(Target);
        // Runtime resumption supplies a reaching frame/state; this is not an
        // independent ordinary entry with unknown incoming registers.
        if (!ExploredAddrs.count(Target))
          explore(Img, Dec, Target);
        Grew = true;
      }
      if (!Insns.count(Target)) {
        Failed = true;
        Quarantined.insert(Target);
        ProvenTargets.erase(Target);
      }
    }
    bool Withdrew = false;
    for (auto It = AddedRoots.begin(); It != AddedRoots.end();) {
      if (ProvenTargets.count(*It)) {
        ++It;
        continue;
      }
      if (AddedPersistent.erase(*It))
        PersistentCFGRoots.erase(*It);
      DurableCFGRoots.erase(*It);
      Quarantined.insert(*It);
      It = AddedRoots.erase(It);
      Withdrew = Failed = true;
    }
    if (!Grew && !Withdrew)
      break;
    splitBlocks();
    rebuildBlocks(Func);
    multiStageResolve(Img, Dec, Func);
    convertIndirectTailCalls(Func);
  }
  if (Failed) {
    auto &State = *Func.RegistrationStates;
    State.Complete = State.CallbackStatesComplete =
        State.CxxContinuationsComplete = State.RegistrationLifetimeComplete =
            State.ChainOperationsComplete = State.ImageReadsComplete = false;
    State.ChainAccesses.clear();
    State.Diagnostics.push_back(
        "C++ continuation ownership or CFG closure is not proven");
  }
}

void CFGBuilder::linkExceptionalSuccessors(LowFunc &Func) {
  for (LowBlock &Block : Func.Blocks) {
    Block.ExceptionalSuccs.clear();
    Block.ExceptionalPreds.clear();
  }
  if (!Func.ExceptionMetadata)
    return;
  const ExceptionFunction &Metadata = *Func.ExceptionMetadata;
  Func.RegistrationStates.reset();
  if (Metadata.Registration) {
    std::optional<std::vector<RegistrationCalleeFrameContract>> Callees;
    std::optional<std::vector<RegistrationCleanupFrameContract>> Cleanups;
    std::optional<std::vector<RegistrationCalleeStackContract>> Stacks;
    const bool CheckCalls = CurrentImg && Metadata.Cxx.has_value();
    RegistrationCallCalleeIndex *CalleeIndex = nullptr;
    if (CurrentImg) {
      if (BorrowedRegistrationCallees &&
          BorrowedRegistrationCallees->ownsImage(*CurrentImg))
        CalleeIndex = BorrowedRegistrationCallees;
      else {
        if (!RegistrationCallees)
          RegistrationCallees =
              std::make_shared<RegistrationCallCalleeIndex>(*CurrentImg);
        CalleeIndex = RegistrationCallees.get();
      }
      Stacks = CalleeIndex->stackContracts(Func);
    }
    if (CheckCalls) {
      Callees = CalleeIndex->contracts(Func);
      Cleanups = CalleeIndex->cleanupContracts(Func);
    }
    va_t CookieCheckVA = 0;
    if (CurrentImg &&
        Metadata.Personality == ExceptionPersonality::ExceptHandler4 &&
        Metadata.Registration->GSCookieOffset != -2)
      if (auto Check = coff_loader::getCheckedX86EH4CookieCheck(
              *CurrentImg, Metadata.PersonalityVA);
          Check &&
          coff_loader::hasCheckedX86CookieCheckSuccessPath(*CurrentImg, *Check))
        CookieCheckVA = *Check;
    Func.RegistrationStates = analyzeRegistrationStates(
        Func,
        CurrentImg && CurrentImg->DynInfo.SecurityCookieRVA
            ? CurrentImg->Base + CurrentImg->DynInfo.SecurityCookieRVA
            : 0,
        CookieCheckVA, CheckCalls && Callees ? &*Callees : nullptr,
        CheckCalls && Cleanups ? &*Cleanups : nullptr,
        Stacks ? &*Stacks : nullptr);
    if (CheckCalls && (!Callees || !Cleanups))
      Func.RegistrationStates->Diagnostics.push_back(
          "registration callee proof budget exhausted");
  }

  std::map<va_t, LowBlock *> BlocksByAddress;
  std::map<int, LowBlock *> BlocksById;
  for (LowBlock &Block : Func.Blocks) {
    BlocksByAddress.emplace(Block.StartAddr, &Block);
    BlocksById.emplace(Block.Id, &Block);
  }
  auto TargetBlockId = [&](va_t TargetVA) {
    auto It = BlocksByAddress.upper_bound(TargetVA);
    if (It != BlocksByAddress.begin()) {
      const LowBlock &Block = *std::prev(It)->second;
      if (TargetVA < Block.EndAddr)
        return Block.Id;
    }
    return -1;
  };
  std::set<std::tuple<int, va_t, ExceptionalEdgeKind, uint32_t, int32_t, va_t>>
      Edges;
  size_t RegistrationEdgeWork = 0;
  bool RegistrationEdgesExhausted = false;
  auto ChargeRegistrationEdge = [&] {
    if (!Metadata.Registration)
      return true;
    if (RegistrationEdgeWork == limits::kMaxRegistrationEHStateWork) {
      RegistrationEdgesExhausted = true;
      return false;
    }
    ++RegistrationEdgeWork;
    return true;
  };
  auto RejectExhaustedRegistrationEdges = [&] {
    if (!RegistrationEdgesExhausted)
      return false;
    for (LowBlock &Block : Func.Blocks) {
      Block.ExceptionalSuccs.clear();
      Block.ExceptionalPreds.clear();
    }
    Func.RegistrationStates->Complete = false;
    Func.RegistrationStates->RegistrationLifetimeComplete = false;
    Func.RegistrationStates->Blocks.clear();
    Func.RegistrationStates->Diagnostics.push_back(
        "registration exceptional-edge expansion budget exhausted");
    return true;
  };
  auto AddEdge = [&](LowBlock &Source, va_t TargetVA, ExceptionalEdgeKind Kind,
                     uint32_t Region, int32_t State,
                     va_t SourceVA = InvalidVA) {
    if (TargetVA == 0 || !ChargeRegistrationEdge() ||
        !Edges.emplace(Source.Id, TargetVA, Kind, Region, State, SourceVA)
             .second)
      return;
    ExceptionalEdge Edge;
    Edge.BlockId = TargetBlockId(TargetVA);
    Edge.TargetVA = TargetVA;
    Edge.Kind = Kind;
    Edge.RegionIndex = Region;
    Edge.State = State;
    Edge.SourceVA = SourceVA;
    Source.ExceptionalSuccs.push_back(Edge);
    if (auto It = BlocksById.find(Edge.BlockId); It != BlocksById.end()) {
      ExceptionalEdge Pred = Edge;
      Pred.BlockId = Source.Id;
      It->second->ExceptionalPreds.push_back(Pred);
    }
  };
  auto ForProtectedBlocks = [&](const ExceptionAddressRange &Range, auto &&Fn) {
    for (LowBlock &Block : Func.Blocks)
      if (Block.StartAddr < Range.End && Block.EndAddr > Range.Begin)
        Fn(Block);
  };

  if (Metadata.SEH) {
    for (size_t I = 0; I < Metadata.SEH->Scopes.size(); ++I) {
      const SEHScopeRecord &Scope = Metadata.SEH->Scopes[I];
      const std::optional<ExceptionAddressRange> SemanticRange =
          getSemanticSEHGuardedRange(
              Scope, CurrentImg ? CurrentImg->Arch : Arch::Unknown, Metadata);
      if (!SemanticRange)
        continue;
      ForProtectedBlocks(*SemanticRange, [&](LowBlock &Block) {
        switch (Scope.Kind) {
        case SEHScopeKind::Filter:
          AddEdge(Block, Scope.FilterOrFinallyVA,
                  ExceptionalEdgeKind::SEHFilter, static_cast<uint32_t>(I), -1);
          AddEdge(Block, Scope.HandlerVA, ExceptionalEdgeKind::SEHHandler,
                  static_cast<uint32_t>(I), -1);
          break;
        case SEHScopeKind::CatchAll:
          AddEdge(Block, Scope.HandlerVA, ExceptionalEdgeKind::SEHHandler,
                  static_cast<uint32_t>(I), -1);
          break;
        case SEHScopeKind::Finally:
          AddEdge(Block, Scope.FilterOrFinallyVA,
                  ExceptionalEdgeKind::SEHFinally, static_cast<uint32_t>(I),
                  -1);
          break;
        }
      });
    }
  }

  if (Metadata.Cxx) {
    const CxxExceptionInfo &Cxx = *Metadata.Cxx;
    auto AddCxxEdges = [&](LowBlock &Block, va_t IP, va_t SourceVA) {
      int32_t State = -1;
      for (const CxxIPState &IPState : Cxx.IPMap) {
        if (IPState.IP > IP)
          break;
        State = IPState.State;
      }
      if (State < 0 || State >= static_cast<int32_t>(Cxx.UnwindMap.size()))
        return;

      const CxxUnwindAction &Cleanup = Cxx.UnwindMap[State];
      if (Cleanup.ActionVA != 0)
        AddEdge(Block, Cleanup.ActionVA, ExceptionalEdgeKind::CxxCleanup,
                static_cast<uint32_t>(State), State, SourceVA);

      for (size_t I = 0; I < Cxx.TryBlocks.size(); ++I) {
        const CxxTryBlock &Try = Cxx.TryBlocks[I];
        if (State < Try.TryLow || State > Try.TryHigh)
          continue;
        for (const CxxCatchHandler &Catch : Try.Handlers)
          AddEdge(Block, Catch.HandlerVA, ExceptionalEdgeKind::CxxCatch,
                  static_cast<uint32_t>(I), State, SourceVA);
      }
    };
    for (LowBlock &Block : Func.Blocks) {
      if (RegistrationEdgesExhausted)
        break;
      if (Metadata.Registration) {
        if (!Func.RegistrationStates)
          continue;
        auto It = std::find_if(Func.RegistrationStates->Blocks.begin(),
                               Func.RegistrationStates->Blocks.end(),
                               [&](const RegistrationBlockState &State) {
                                 return State.BlockId == Block.Id;
                               });
        if (It == Func.RegistrationStates->Blocks.end() || !It->CanDispatch)
          continue;
        for (int32_t State : It->Levels) {
          if (!ChargeRegistrationEdge())
            break;
          int32_t Walk = State;
          for (size_t Step = 0; Step < Cxx.UnwindMap.size(); ++Step) {
            if (!ChargeRegistrationEdge())
              break;
            if (Walk < 0 || static_cast<size_t>(Walk) >= Cxx.UnwindMap.size())
              break;
            const CxxUnwindAction &Cleanup = Cxx.UnwindMap[Walk];
            AddEdge(Block, Cleanup.ActionVA, ExceptionalEdgeKind::CxxCleanup,
                    static_cast<uint32_t>(Walk), State);
            Walk = Cleanup.ToState;
          }
          for (size_t I = 0; I < Cxx.TryBlocks.size(); ++I) {
            if (!ChargeRegistrationEdge())
              break;
            const CxxTryBlock &Try = Cxx.TryBlocks[I];
            if (Try.TryLow < It->CxxMinimumTryLevel || State < Try.TryLow ||
                State > Try.TryHigh)
              continue;
            for (const CxxCatchHandler &Catch : Try.Handlers) {
              if (!ChargeRegistrationEdge())
                break;
              AddEdge(Block, Catch.HandlerVA, ExceptionalEdgeKind::CxxCatch,
                      static_cast<uint32_t>(I), State);
            }
          }
        }
        continue;
      }
      if (Cxx.IsSynchronous && CurrentImg && CurrentImg->Arch == Arch::X64) {
        // x64 FH3/FH4 state lookup uses the saved return PC. Clang can put
        // an IP-map boundary one byte past a label, inside an instruction;
        // it is not necessarily a basic-block boundary. Preserve each call's
        // state and source instead of borrowing the state at block entry.
        for (const LowInstructionBoundary &Insn : Block.InstructionBoundaries)
          if (Insn.Control == LowInstructionControl::Call && Insn.Size &&
              Insn.Address <= InvalidVA - Insn.Size)
            AddCxxEdges(Block, Insn.Address + Insn.Size, Insn.Address);
      } else {
        AddCxxEdges(Block, Block.StartAddr, InvalidVA);
      }
    }
  }
  if (RejectExhaustedRegistrationEdges())
    return;

  // x86-32 registration chain.  This table is indexed by the try level the
  // frame holds rather than by address, so a scope guards exactly those blocks
  // that run while its level is current, and the recovered stores are the only
  // record of which those are.  Nesting is by level, not by containment: when
  // an exception arrives the runtime offers it to the current level's scope and
  // then to each enclosing one in turn, so every scope on that chain gets an
  // edge and not just the innermost.
  if (Metadata.Registration && Func.RegistrationStates) {
    const RegistrationChainInfo &Chain = *Metadata.Registration;
    const size_t ScopeCount = Chain.Scopes.size();
    for (LowBlock &Block : Func.Blocks) {
      if (RegistrationEdgesExhausted)
        break;
      auto It = std::find_if(Func.RegistrationStates->Blocks.begin(),
                             Func.RegistrationStates->Blocks.end(),
                             [&](const RegistrationBlockState &State) {
                               return State.BlockId == Block.Id;
                             });
      if (It == Func.RegistrationStates->Blocks.end() || !It->CanDispatch)
        continue;
      for (int32_t Level : It->Levels) {
        if (!ChargeRegistrationEdge())
          break;
        // The scope count bounds the walk even for manually supplied cycles.
        for (size_t Step = 0; Step < ScopeCount; ++Step) {
          if (!ChargeRegistrationEdge())
            break;
          if (Level < 0 || static_cast<size_t>(Level) >= ScopeCount)
            break;
          const RegistrationScopeRecord &Scope = Chain.Scopes[Level];
          const uint32_t Region = static_cast<uint32_t>(Level);
          if (Scope.IsFinally) {
            AddEdge(Block, Scope.HandlerVA, ExceptionalEdgeKind::SEHFinally,
                    Region, Level);
          } else {
            AddEdge(Block, Scope.FilterVA, ExceptionalEdgeKind::SEHFilter,
                    Region, Level);
            AddEdge(Block, Scope.HandlerVA, ExceptionalEdgeKind::SEHHandler,
                    Region, Level);
          }
          Level = Scope.EnclosingLevel;
        }
      }
    }
  }
  if (RejectExhaustedRegistrationEdges())
    return;

  // Itanium.  A Rust frame is deliberately not walked separately: its landing
  // pads are a reclassification of these same call sites (or, on MSVC targets,
  // of the `Cxx` maps handled above), so reading both would double every edge.
  if (Metadata.Itanium && Metadata.Itanium->IsCallSiteAddressForm) {
    const ItaniumEHInfo &Itanium = *Metadata.Itanium;
    auto FindAction = [&](uint64_t Offset) -> const ItaniumAction * {
      for (const ItaniumAction &Action : Itanium.Actions)
        if (Action.TableOffset == Offset)
          return &Action;
      return nullptr;
    };
    for (size_t I = 0; I < Itanium.CallSites.size(); ++I) {
      const ItaniumCallSite &Site = Itanium.CallSites[I];
      // A zero landing pad is how the table spells "no local handler": the
      // exception leaves the frame instead of entering it, so there is no edge.
      if (Site.LandingPadVA == 0)
        continue;

      // One clause per distinct (kind, filter) the chain names.  A chain that
      // repeats a kind describes one pad entry, not several.
      llvm::SmallVector<std::pair<ExceptionalEdgeKind, int32_t>, 4> Clauses;
      auto AddClause = [&](ExceptionalEdgeKind Kind, int64_t Filter) {
        auto Clause = std::make_pair(Kind, static_cast<int32_t>(Filter));
        if (std::find(Clauses.begin(), Clauses.end(), Clause) == Clauses.end())
          Clauses.push_back(Clause);
      };
      if (!Site.FirstActionOffset) {
        // The ABI defines a landing pad with no action record as an
        // unconditional cleanup, which is the shape every destructor-only
        // frame has.
        AddClause(ExceptionalEdgeKind::ItaniumCleanupPad, 0);
      } else {
        std::optional<uint64_t> Offset = Site.FirstActionOffset;
        // The chain is a linked list inside a table the decoder already
        // bounded, so a step budget of the action count both terminates a
        // cycle and cannot cut a well-formed chain short.
        for (size_t Step = 0; Offset && Step <= Itanium.Actions.size();
             ++Step) {
          const ItaniumAction *Action = FindAction(*Offset);
          if (!Action)
            break;
          AddClause(Action->isCleanup() ? ExceptionalEdgeKind::ItaniumCleanupPad
                    : Action->isCatch() ? ExceptionalEdgeKind::ItaniumCatchPad
                                        : ExceptionalEdgeKind::ItaniumSpecPad,
                    Action->TypeFilter);
          Offset = Action->NextActionOffset;
        }
      }
      ForProtectedBlocks(Site.GuardedRange, [&](LowBlock &Block) {
        for (const auto &[Kind, Filter] : Clauses)
          AddEdge(Block, Site.LandingPadVA, Kind, static_cast<uint32_t>(I),
                  Filter);
      });
    }
  }

  // Delphi.  A `TExcFrame` has no scope table and no per-scope range: one
  // frame guards one region, which runs from the instruction that linked the
  // record onto the chain up to the descriptor.  The descriptor bounds it
  // because Delphi lays the dispatch code and the handler bodies out after the
  // guarded body — the same layout the decoder already reads the arms from.
  if (Metadata.Delphi) {
    const DelphiFrameInfo &Delphi = *Metadata.Delphi;
    ExceptionAddressRange Guarded;
    Guarded.Begin = Metadata.CodeRange.contains(Delphi.ChainInstallVA)
                        ? Delphi.ChainInstallVA
                        : Metadata.CodeRange.Begin;
    Guarded.End = Metadata.CodeRange.contains(Delphi.DescriptorVA) &&
                          Delphi.DescriptorVA > Guarded.Begin
                      ? Delphi.DescriptorVA
                      : Metadata.CodeRange.End;
    if (Guarded.isValid())
      ForProtectedBlocks(Guarded, [&](LowBlock &Block) {
        switch (Delphi.Kind) {
        case DelphiHandlerKind::Finally:
          AddEdge(Block, Delphi.FinallyBodyVA,
                  ExceptionalEdgeKind::DelphiFinally, 0, -1);
          break;
        case DelphiHandlerKind::AnyException:
        case DelphiHandlerKind::AutoException:
          AddEdge(Block, Delphi.ExceptBodyVA, ExceptionalEdgeKind::DelphiExcept,
                  0, -1);
          break;
        case DelphiHandlerKind::OnException:
          for (size_t I = 0; I < Delphi.OnExceptions.size(); ++I)
            AddEdge(Block, Delphi.OnExceptions[I].HandlerVA,
                    ExceptionalEdgeKind::DelphiOnException,
                    static_cast<uint32_t>(I), -1);
          break;
        case DelphiHandlerKind::Unknown:
          break;
        }
      });
  }

  // Delphi on x86-64, where the frame does carry a scope table and so names
  // the exact range each handler guards.
  if (Metadata.DelphiScopes) {
    const std::vector<DelphiScopeRecord> &Scopes =
        Metadata.DelphiScopes->Scopes;
    for (size_t I = 0; I < Scopes.size(); ++I) {
      const DelphiScopeRecord &Scope = Scopes[I];
      ForProtectedBlocks(Scope.GuardedRange, [&](LowBlock &Block) {
        switch (Scope.Kind) {
        case DelphiScopeKind::Finally:
          AddEdge(Block, Scope.TargetVA, ExceptionalEdgeKind::DelphiFinally,
                  static_cast<uint32_t>(I), -1);
          break;
        case DelphiScopeKind::SafecallCatch:
        case DelphiScopeKind::CatchAll:
          AddEdge(Block, Scope.TargetVA, ExceptionalEdgeKind::DelphiExcept,
                  static_cast<uint32_t>(I), -1);
          break;
        case DelphiScopeKind::OnException:
          for (size_t J = 0; J < Scope.OnExceptions.size(); ++J)
            AddEdge(Block, Scope.OnExceptions[J].HandlerVA,
                    ExceptionalEdgeKind::DelphiOnException,
                    static_cast<uint32_t>(I), static_cast<int32_t>(J));
          break;
        }
      });
    }
  }

  // Go.  The runtime resumes a panicking frame at `deferreturn` to run what
  // the frame deferred, so that address is entered without any branch reaching
  // it.  The sites that can start that transfer are the ones that register a
  // defer and the ones that raise, which is what the frame's own metadata
  // names; a `recover` site gets no edge here because recovery resumes the
  // frame that deferred rather than the deferred frame the call sits in.
  if (Metadata.Go && Metadata.Go->DeferReturnOffset) {
    va_t DeferReturn =
        Metadata.CodeRange.Begin + *Metadata.Go->DeferReturnOffset;
    if (Metadata.CodeRange.contains(DeferReturn)) {
      auto AddGoEdge = [&](va_t SiteVA) {
        if (LowBlock *Block = Func.blockFor(SiteVA))
          AddEdge(*Block, DeferReturn, ExceptionalEdgeKind::GoDeferReturn, 0,
                  -1);
      };
      for (const GoDeferSite &Defer : Metadata.Go->Defers)
        AddGoEdge(Defer.CallVA);
      for (const GoPanicSite &Panic : Metadata.Go->Panics)
        AddGoEdge(Panic.CallVA);
    }
  }
}

} // namespace neverd
