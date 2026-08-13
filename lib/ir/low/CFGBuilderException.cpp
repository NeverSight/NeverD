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

#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

void CFGBuilder::linkExceptionalSuccessors(LowFunc &Func) {
  for (LowBlock &Block : Func.Blocks) {
    Block.ExceptionalSuccs.clear();
    Block.ExceptionalPreds.clear();
  }
  if (!Func.ExceptionMetadata)
    return;
  const ExceptionFunction &Metadata = *Func.ExceptionMetadata;

  auto TargetBlockId = [&](va_t TargetVA) {
    if (LowBlock *Target = Func.blockFor(TargetVA))
      return Target->Id;
    return -1;
  };
  auto AddEdge = [&](LowBlock &Source, va_t TargetVA, ExceptionalEdgeKind Kind,
                     uint32_t Region, int32_t State) {
    if (TargetVA == 0)
      return;
    ExceptionalEdge Edge;
    Edge.BlockId = TargetBlockId(TargetVA);
    Edge.TargetVA = TargetVA;
    Edge.Kind = Kind;
    Edge.RegionIndex = Region;
    Edge.State = State;
    if (std::find(Source.ExceptionalSuccs.begin(),
                  Source.ExceptionalSuccs.end(),
                  Edge) == Source.ExceptionalSuccs.end())
      Source.ExceptionalSuccs.push_back(Edge);
    if (Edge.BlockId >= 0 &&
        Edge.BlockId < static_cast<int>(Func.Blocks.size())) {
      ExceptionalEdge Pred = Edge;
      Pred.BlockId = Source.Id;
      auto &Preds = Func.Blocks[Edge.BlockId].ExceptionalPreds;
      if (std::find(Preds.begin(), Preds.end(), Pred) == Preds.end())
        Preds.push_back(Pred);
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
      ForProtectedBlocks(Scope.GuardedRange, [&](LowBlock &Block) {
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
    for (LowBlock &Block : Func.Blocks) {
      int32_t State = -1;
      for (const CxxIPState &IPState : Cxx.IPMap) {
        if (IPState.IP > Block.StartAddr)
          break;
        State = IPState.State;
      }
      if (State < 0 || State >= static_cast<int32_t>(Cxx.UnwindMap.size()))
        continue;

      const CxxUnwindAction &Cleanup = Cxx.UnwindMap[State];
      if (Cleanup.ActionVA != 0)
        AddEdge(Block, Cleanup.ActionVA, ExceptionalEdgeKind::CxxCleanup,
                static_cast<uint32_t>(State), State);

      for (size_t I = 0; I < Cxx.TryBlocks.size(); ++I) {
        const CxxTryBlock &Try = Cxx.TryBlocks[I];
        if (State < Try.TryLow || State > Try.TryHigh)
          continue;
        for (const CxxCatchHandler &Catch : Try.Handlers)
          AddEdge(Block, Catch.HandlerVA, ExceptionalEdgeKind::CxxCatch,
                  static_cast<uint32_t>(I), State);
      }
    }
  }

  // x86-32 registration chain.  This table is indexed by the try level the
  // frame holds rather than by address, so a scope guards exactly those blocks
  // that run while its level is current, and the recovered stores are the only
  // record of which those are.  Nesting is by level, not by containment: when
  // an exception arrives the runtime offers it to the current level's scope and
  // then to each enclosing one in turn, so every scope on that chain gets an
  // edge and not just the innermost.
  if (Metadata.Registration && !Metadata.Registration->TryLevelStores.empty()) {
    const RegistrationChainInfo &Chain = *Metadata.Registration;
    const size_t ScopeCount = Chain.Scopes.size();
    for (LowBlock &Block : Func.Blocks) {
      // The store's own block still runs at the outgoing level, so only a store
      // that has completed by the time the block is entered counts.
      int32_t Level = Chain.SeededTryLevel.value_or(-1);
      for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores) {
        if (Store.EndVA > Block.StartAddr)
          break;
        Level = Store.Level;
      }

      // A malformed table could name itself as its own enclosing level; the
      // scope count bounds the walk so such a cycle cannot spin.
      for (size_t Step = 0; Step < ScopeCount; ++Step) {
        if (Level < 0 || static_cast<size_t>(Level) >= ScopeCount)
          break;
        const RegistrationScopeRecord &Scope = Chain.Scopes[Level];
        const uint32_t Region = static_cast<uint32_t>(Level);
        if (Scope.IsFinally) {
          AddEdge(Block, Scope.HandlerVA, ExceptionalEdgeKind::SEHFinally,
                  Region, Level);
        } else {
          AddEdge(Block, Scope.FilterVA, ExceptionalEdgeKind::SEHFilter, Region,
                  Level);
          AddEdge(Block, Scope.HandlerVA, ExceptionalEdgeKind::SEHHandler,
                  Region, Level);
        }
        Level = Scope.EnclosingLevel;
      }
    }
  }

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
        for (size_t Step = 0; Offset && Step <= Itanium.Actions.size(); ++Step) {
          const ItaniumAction *Action = FindAction(*Offset);
          if (!Action)
            break;
          AddClause(Action->isCleanup()   ? ExceptionalEdgeKind::ItaniumCleanupPad
                    : Action->isCatch()   ? ExceptionalEdgeKind::ItaniumCatchPad
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
