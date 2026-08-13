//===- HighEHStructurer.cpp - Conservative EH region structuring --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Converts normalized guarded ranges into explicit HighIR exception nodes.
/// Three native shapes reach this file: a Windows SEH scope table, an MSVC C++
/// state map, and an Itanium LSDA call-site table.  The transform is
/// deliberately interval-conservative: it moves statements only when one
/// contiguous HighIR slice is wholly contained by a validated native range.
/// Crossing or address-less shapes stay in their original order and are
/// reported through the function's unstructured count.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {

enum class RangeClass : uint8_t { Unknown, Inside, Outside, Crossing };

struct AddressFootprint {
  bool HasInside = false;
  bool HasOutside = false;
};

void classifyStatements(const std::vector<HighStmt> &Statements,
                        const ExceptionAddressRange &Range,
                        AddressFootprint &Result);

void classifyStatement(const HighStmt &Stmt, const ExceptionAddressRange &Range,
                       AddressFootprint &Result) {
  auto AddAddress = [&](va_t Address) {
    if (Address == 0 || Address == InvalidVA)
      return;
    if (Range.contains(Address))
      Result.HasInside = true;
    else
      Result.HasOutside = true;
  };

  AddAddress(Stmt.Addr);
  if ((Stmt.Kind == StmtKind::SEHTry || Stmt.Kind == StmtKind::CxxTry ||
       Stmt.Kind == StmtKind::ItaniumTry) &&
      Stmt.EHRange.isValid()) {
    if (Range.contains(Stmt.EHRange))
      Result.HasInside = true;
    else if (Range.overlaps(Stmt.EHRange)) {
      Result.HasInside = true;
      Result.HasOutside = true;
    } else
      Result.HasOutside = true;
  }
  classifyStatements(Stmt.Body, Range, Result);
  classifyStatements(Stmt.ElseBody, Range, Result);
  for (const SwitchCase &Case : Stmt.Cases)
    classifyStatements(Case.Body, Range, Result);
  classifyStatements(Stmt.DefaultBody, Range, Result);
  for (const std::vector<HighStmt> &ClauseBody : Stmt.EHClauseBodies)
    classifyStatements(ClauseBody, Range, Result);
}

void classifyStatements(const std::vector<HighStmt> &Statements,
                        const ExceptionAddressRange &Range,
                        AddressFootprint &Result) {
  for (const HighStmt &Stmt : Statements)
    classifyStatement(Stmt, Range, Result);
}

RangeClass classifyStatement(const HighStmt &Stmt,
                             const ExceptionAddressRange &Range) {
  AddressFootprint Footprint;
  classifyStatement(Stmt, Range, Footprint);
  if (Footprint.HasInside && Footprint.HasOutside)
    return RangeClass::Crossing;
  if (Footprint.HasInside)
    return RangeClass::Inside;
  if (Footprint.HasOutside)
    return RangeClass::Outside;
  return RangeClass::Unknown;
}

bool extractProtectedSlice(std::vector<HighStmt> &Statements,
                           const ExceptionAddressRange &Range,
                           const ExceptionAddressRange &FunctionRange,
                           std::vector<HighStmt> &Body, size_t &InsertAt) {
  std::optional<size_t> First;
  std::optional<size_t> Last;
  std::vector<RangeClass> Classes;
  Classes.reserve(Statements.size());
  for (size_t I = 0; I < Statements.size(); ++I) {
    RangeClass Class = classifyStatement(Statements[I], Range);
    if (Class == RangeClass::Crossing)
      return false;
    Classes.push_back(Class);
    if (Class == RangeClass::Inside) {
      if (!First)
        First = I;
      Last = I;
    }
  }
  if (!First || !Last)
    return false;
  for (size_t I = *First; I <= *Last; ++I)
    if (Classes[I] == RangeClass::Outside)
      return false;

  // Address-less synthetic statements at a native edge belong to the range
  // only when the range itself reaches that function edge.  This captures a
  // synthesized trailing return without swallowing an unrelated neighbour.
  size_t Begin = *First;
  size_t End = *Last + 1;
  if (Range.Begin == FunctionRange.Begin)
    while (Begin != 0 && Classes[Begin - 1] == RangeClass::Unknown)
      --Begin;
  if (Range.End == FunctionRange.End)
    while (End < Classes.size() && Classes[End] == RangeClass::Unknown)
      ++End;

  Body.reserve(End - Begin);
  for (size_t I = Begin; I < End; ++I)
    Body.push_back(std::move(Statements[I]));
  Statements.erase(Statements.begin() + static_cast<ptrdiff_t>(Begin),
                   Statements.begin() + static_cast<ptrdiff_t>(End));
  InsertAt = Begin;
  return true;
}

struct RegionCandidate {
  StmtKind Kind = StmtKind::SEHTry;
  ExceptionAddressRange Range;
  std::vector<HighEHClause> Clauses;
  unsigned NativeRegionCount = 0;
};

std::vector<ExceptionAddressRange>
codeRangesForStates(const ExceptionFunction &EH, const CxxExceptionInfo &Cxx,
                    int32_t LowState, int32_t HighState) {
  std::vector<ExceptionAddressRange> Ranges;
  va_t Cursor = EH.CodeRange.Begin;
  int32_t State = -1;
  auto Add = [&](va_t Begin, va_t End, int32_t SegmentState) {
    Begin = std::max(Begin, EH.CodeRange.Begin);
    End = std::min(End, EH.CodeRange.End);
    if (Begin >= End || SegmentState < LowState || SegmentState > HighState)
      return;
    if (!Ranges.empty() && Ranges.back().End == Begin)
      Ranges.back().End = End;
    else
      Ranges.push_back({Begin, End});
  };

  for (const CxxIPState &IPState : Cxx.IPMap) {
    if (IPState.IP < EH.CodeRange.Begin)
      continue;
    if (IPState.IP > EH.CodeRange.End)
      break;
    Add(Cursor, IPState.IP, State);
    Cursor = IPState.IP;
    State = IPState.State;
  }
  Add(Cursor, EH.CodeRange.End, State);
  return Ranges;
}

void addSEHCandidates(const ExceptionFunction &EH,
                      std::vector<RegionCandidate> &Candidates,
                      unsigned &Rejected) {
  if (!EH.SEH)
    return;
  std::map<std::pair<va_t, va_t>, size_t> ByRange;
  for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
    if (Scope.ParseStatus != ExceptionParseStatus::Complete ||
        !Scope.GuardedRange.isValid() ||
        !EH.CodeRange.contains(Scope.GuardedRange)) {
      ++Rejected;
      continue;
    }
    auto Key = std::make_pair(Scope.GuardedRange.Begin, Scope.GuardedRange.End);
    size_t Index = 0;
    if (auto It = ByRange.find(Key); It != ByRange.end()) {
      Index = It->second;
    } else {
      Index = Candidates.size();
      ByRange.emplace(Key, Index);
      RegionCandidate Candidate;
      Candidate.Kind = StmtKind::SEHTry;
      Candidate.Range = Scope.GuardedRange;
      Candidates.push_back(std::move(Candidate));
    }

    HighEHClause Clause;
    Clause.Kind = Scope.Kind == SEHScopeKind::Finally
                      ? HighEHClauseKind::SEHFinally
                      : HighEHClauseKind::SEHExcept;
    Clause.ParseStatus = Scope.ParseStatus;
    Clause.FilterOrActionVA = Scope.FilterOrFinallyVA;
    Clause.HandlerVA = Scope.HandlerVA;
    if (Scope.ContinuationVA)
      Clause.ContinuationVAs.push_back(Scope.ContinuationVA);
    Candidates[Index].Clauses.push_back(std::move(Clause));
    ++Candidates[Index].NativeRegionCount;
  }
}

void addCxxCandidates(const ExceptionFunction &EH,
                      std::vector<RegionCandidate> &Candidates,
                      unsigned &Rejected) {
  if (!EH.Cxx)
    return;
  const CxxExceptionInfo &Cxx = *EH.Cxx;
  for (const CxxTryBlock &Try : Cxx.TryBlocks) {
    std::vector<ExceptionAddressRange> Ranges =
        codeRangesForStates(EH, Cxx, Try.TryLow, Try.TryHigh);
    if (Ranges.size() != 1 || !Ranges.front().isValid()) {
      ++Rejected;
      continue;
    }

    RegionCandidate Candidate;
    Candidate.Kind = StmtKind::CxxTry;
    Candidate.Range = Ranges.front();
    Candidate.NativeRegionCount = 1;
    for (const CxxCatchHandler &Catch : Try.Handlers) {
      HighEHClause Clause;
      Clause.Kind = HighEHClauseKind::CxxCatch;
      Clause.HandlerVA = Catch.HandlerVA;
      Clause.TypeDescriptorVA = Catch.TypeDescriptorVA;
      Clause.Adjectives = Catch.Adjectives;
      Clause.CatchObjectOffset = Catch.CatchObjectOffset;
      Clause.ParentFrameOffset = Catch.ParentFrameOffset;
      Clause.ContinuationVAs = Catch.ContinuationVAs;
      Candidate.Clauses.push_back(std::move(Clause));
    }
    for (int32_t State = Try.TryLow; State <= Try.TryHigh; ++State) {
      if (State < 0 || State >= static_cast<int32_t>(Cxx.UnwindMap.size()))
        continue;
      const CxxUnwindAction &Action = Cxx.UnwindMap[State];
      if (Action.ActionVA == 0)
        continue;
      HighEHClause Clause;
      Clause.Kind = HighEHClauseKind::CxxCleanup;
      Clause.FilterOrActionVA = Action.ActionVA;
      Clause.State = State;
      Clause.UnwindActionKind = Action.Kind;
      Clause.UnwindObjectOffset = Action.ObjectOffset;
      Candidate.Clauses.push_back(std::move(Clause));
    }
    if (Candidate.Clauses.empty()) {
      ++Rejected;
      continue;
    }
    Candidates.push_back(std::move(Candidate));
  }
}

//===----------------------------------------------------------------------===//
// Itanium
//===----------------------------------------------------------------------===//

const ItaniumAction *findAction(const ItaniumEHInfo &LSDA, uint64_t Offset) {
  for (const ItaniumAction &Action : LSDA.Actions)
    if (Action.TableOffset == Offset)
      return &Action;
  return nullptr;
}

const ItaniumTypeEntry *findTypeEntry(const ItaniumEHInfo &LSDA,
                                      uint64_t Index) {
  for (const ItaniumTypeEntry &Entry : LSDA.TypeTable)
    if (Entry.Index == Index)
      return &Entry;
  return nullptr;
}

const ItaniumExceptionSpec *findExceptionSpec(const ItaniumEHInfo &LSDA,
                                              uint64_t Index) {
  for (const ItaniumExceptionSpec &Spec : LSDA.ExceptionSpecs)
    if (Spec.Index == Index)
      return &Spec;
  return nullptr;
}

/// The action records one call site names, in the order the personality tests
/// them.  The chain is a linked list inside a table the decoder already
/// bounded, so the action count is both a cycle breaker and a bound that no
/// well-formed chain can exceed.
std::vector<uint64_t> actionChain(const ItaniumEHInfo &LSDA,
                                  const ItaniumCallSite &Site) {
  std::vector<uint64_t> Chain;
  std::optional<uint64_t> Offset = Site.FirstActionOffset;
  for (size_t Step = 0; Offset && Step <= LSDA.Actions.size(); ++Step) {
    const ItaniumAction *Action = findAction(LSDA, *Offset);
    if (!Action)
      break;
    if (std::find(Chain.begin(), Chain.end(), Action->TableOffset) !=
        Chain.end())
      break;
    Chain.push_back(Action->TableOffset);
    Offset = Action->NextActionOffset;
  }
  return Chain;
}

/// Recover try regions from an Itanium call-site table.
///
/// The table is flat and sorted: it says which landing pad each stretch of
/// code reaches, never which stretches belong to one source-level `try`.  What
/// carries that is the action chain, because the compiler builds one chain per
/// try nest — an inner clause first, then the clauses of every enclosing try.
/// So the region a clause guards is the run of call sites naming its action,
/// and an enclosing clause, being named by more of them, comes out as a region
/// that contains the inner one.
///
/// A run is broken by any call site that does not name the action.  That
/// distinction is what keeps two adjacent try blocks apart: a compiler emits
/// an explicit entry for a stretch that can throw and reaches no handler here,
/// and emits nothing at all for one that cannot throw.  Merging across the
/// second but not the first covers the straight-line code between two calls in
/// one try without swallowing the code between two separate ones.
///
/// That break is also the limit of what this recovers.  Where the compiler
/// laid an inner handler out *between* two stretches the enclosing clause
/// guards, the enclosing run breaks there too, and the one source-level try
/// comes back as two regions that each list the enclosing clause.  Rejoining
/// them would mean deciding that the gap holds only handler code, and the
/// table says nothing that separates that case from a stretch which genuinely
/// escapes the frame — so the clause is reported against the stretches it was
/// proven to guard rather than against a hull that was guessed.
///
/// Cleanup actions deliberately produce no clause.  The pad runs them before
/// it tests anything, so they are not arms of the region, and a frame whose
/// only actions are cleanups is a scope with destructors rather than a `try`.
void addItaniumCandidates(const ExceptionFunction &EH,
                          std::vector<RegionCandidate> &Candidates,
                          unsigned &Rejected) {
  if (!EH.Itanium)
    return;
  const ItaniumEHInfo &LSDA = *EH.Itanium;
  // The SJLJ form's call-site "ranges" are indices the compiler handed out,
  // not addresses, so there is no interval here to lay over the body.
  if (!LSDA.IsCallSiteAddressForm) {
    Rejected += static_cast<unsigned>(LSDA.CallSites.size());
    return;
  }

  std::vector<size_t> Order;
  Order.reserve(LSDA.CallSites.size());
  for (size_t I = 0; I < LSDA.CallSites.size(); ++I)
    if (LSDA.CallSites[I].GuardedRange.isValid())
      Order.push_back(I);
  std::stable_sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
    return LSDA.CallSites[A].GuardedRange.Begin <
           LSDA.CallSites[B].GuardedRange.Begin;
  });

  std::vector<std::vector<uint64_t>> Chains(LSDA.CallSites.size());
  for (size_t I : Order)
    Chains[I] = actionChain(LSDA, LSDA.CallSites[I]);

  // Every action a chain dispatches on, visited in table order.  The order a
  // region's clauses end up in is fixed afterwards from their depth in that
  // region's own chain, which is the only order that means anything: an
  // action's depth differs between chains, so no global ordering of the table
  // can stand for the order the personality tests one region's clauses in.
  std::vector<uint64_t> Dispatching;
  for (const ItaniumAction &Action : LSDA.Actions)
    if (!Action.isCleanup())
      Dispatching.push_back(Action.TableOffset);
  std::sort(Dispatching.begin(), Dispatching.end());
  Dispatching.erase(std::unique(Dispatching.begin(), Dispatching.end()),
                    Dispatching.end());

  std::map<std::pair<va_t, va_t>, size_t> ByRange;
  std::vector<RegionCandidate> Regions;

  for (uint64_t ActionOffset : Dispatching) {
    const ItaniumAction *Action = findAction(LSDA, ActionOffset);
    if (!Action)
      continue;

    auto Flush = [&](size_t RunBegin, size_t RunEnd) {
      const ItaniumCallSite &First = LSDA.CallSites[Order[RunBegin]];
      const ItaniumCallSite &Last = LSDA.CallSites[Order[RunEnd - 1]];
      ExceptionAddressRange Range{First.GuardedRange.Begin,
                                  Last.GuardedRange.End};
      unsigned SiteCount = static_cast<unsigned>(RunEnd - RunBegin);
      if (!Range.isValid() || !EH.CodeRange.contains(Range)) {
        Rejected += SiteCount;
        return;
      }

      auto Key = std::make_pair(Range.Begin, Range.End);
      auto It = ByRange.find(Key);
      if (It == ByRange.end()) {
        It = ByRange.emplace(Key, Regions.size()).first;
        RegionCandidate Fresh;
        Fresh.Kind = StmtKind::ItaniumTry;
        Fresh.Range = Range;
        Regions.push_back(std::move(Fresh));
      }
      RegionCandidate &Region = Regions[It->second];
      // Several clauses share one region, so the native records it stands for
      // are its call sites counted once rather than once per clause.
      Region.NativeRegionCount = std::max(Region.NativeRegionCount, SiteCount);

      HighEHClause Clause;
      Clause.TypeFilter = Action->TypeFilter;
      const std::vector<uint64_t> &Chain = Chains[Order[RunBegin]];
      Clause.ChainDepth = static_cast<uint32_t>(
          std::find(Chain.begin(), Chain.end(), ActionOffset) - Chain.begin());
      for (size_t K = RunBegin; K < RunEnd; ++K) {
        va_t Pad = LSDA.CallSites[Order[K]].LandingPadVA;
        if (Pad == 0)
          continue;
        if (std::find(Clause.LandingPadVAs.begin(), Clause.LandingPadVAs.end(),
                      Pad) == Clause.LandingPadVAs.end())
          Clause.LandingPadVAs.push_back(Pad);
      }
      if (!Clause.LandingPadVAs.empty())
        Clause.HandlerVA = Clause.LandingPadVAs.front();

      if (Action->isCatch()) {
        Clause.Kind = HighEHClauseKind::ItaniumCatch;
        const ItaniumTypeEntry *Type =
            findTypeEntry(LSDA, static_cast<uint64_t>(Action->TypeFilter));
        if (!Type) {
          Clause.ParseStatus = ExceptionParseStatus::Partial;
        } else {
          Clause.TypeDescriptorVA = Type->TypeInfoVA;
          Clause.TypeName = Type->TypeName;
        }
      } else {
        Clause.Kind = HighEHClauseKind::ItaniumSpec;
        const ItaniumExceptionSpec *Spec =
            findExceptionSpec(LSDA, static_cast<uint64_t>(-Action->TypeFilter));
        if (!Spec) {
          Clause.ParseStatus = ExceptionParseStatus::Partial;
        } else {
          for (uint64_t Index : Spec->TypeIndices) {
            const ItaniumTypeEntry *Type = findTypeEntry(LSDA, Index);
            if (!Type || Type->TypeName.empty()) {
              Clause.ParseStatus = ExceptionParseStatus::Partial;
              continue;
            }
            Clause.SpecTypeNames.push_back(Type->TypeName);
          }
        }
      }
      Region.Clauses.push_back(std::move(Clause));
    };

    std::optional<size_t> RunBegin;
    for (size_t K = 0; K < Order.size(); ++K) {
      const std::vector<uint64_t> &Chain = Chains[Order[K]];
      if (std::find(Chain.begin(), Chain.end(), ActionOffset) != Chain.end()) {
        if (!RunBegin)
          RunBegin = K;
        continue;
      }
      if (RunBegin) {
        Flush(*RunBegin, K);
        RunBegin.reset();
      }
    }
    if (RunBegin)
      Flush(*RunBegin, Order.size());
  }

  for (RegionCandidate &Region : Regions)
    std::stable_sort(Region.Clauses.begin(), Region.Clauses.end(),
                     [](const HighEHClause &A, const HighEHClause &B) {
                       return A.ChainDepth < B.ChainDepth;
                     });
  Candidates.insert(Candidates.end(), std::make_move_iterator(Regions.begin()),
                    std::make_move_iterator(Regions.end()));
}

bool hasCrossingRegions(const std::vector<RegionCandidate> &Candidates,
                        size_t Index) {
  const ExceptionAddressRange &A = Candidates[Index].Range;
  for (size_t I = 0; I < Candidates.size(); ++I) {
    if (I == Index)
      continue;
    const ExceptionAddressRange &B = Candidates[I].Range;
    if (!A.overlaps(B) || A.contains(B) || B.contains(A))
      continue;
    return true;
  }
  return false;
}

} // anonymous namespace

void MedToHighConverter::structureExceptionRegions(HighFunc &Func,
                                                   const MedFunc &Med) {
  (void)Med;
  if (!Func.ExceptionMetadata)
    return;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;

  std::vector<RegionCandidate> Candidates;
  unsigned Rejected = 0;
  if (EH.ParseStatus == ExceptionParseStatus::Complete) {
    addSEHCandidates(EH, Candidates, Rejected);
    addCxxCandidates(EH, Candidates, Rejected);
    addItaniumCandidates(EH, Candidates, Rejected);
  } else {
    Rejected += EH.SEH ? static_cast<unsigned>(EH.SEH->Scopes.size()) : 0;
    Rejected += EH.Cxx ? static_cast<unsigned>(EH.Cxx->TryBlocks.size()) : 0;
    Rejected +=
        EH.Itanium ? static_cast<unsigned>(EH.Itanium->CallSites.size()) : 0;
  }

  // Inner-first makes a nested structured node an indivisible statement when
  // its enclosing range is processed.  The address classifier understands the
  // explicit EHRange, so nesting never relies on incidental statement order.
  std::stable_sort(Candidates.begin(), Candidates.end(),
                   [](const RegionCandidate &A, const RegionCandidate &B) {
                     return std::make_tuple(A.Range.size(), A.Range.Begin,
                                            A.Range.End, A.Kind) <
                            std::make_tuple(B.Range.size(), B.Range.Begin,
                                            B.Range.End, B.Kind);
                   });

  // Several native try-map records may share one code interval (for example,
  // distinct ordered catch clauses).  Keep one HighIR try node and retain the
  // native-record count for completeness accounting.
  std::vector<RegionCandidate> Merged;
  for (RegionCandidate &Candidate : Candidates) {
    if (!Merged.empty() && Merged.back().Kind == Candidate.Kind &&
        Merged.back().Range.Begin == Candidate.Range.Begin &&
        Merged.back().Range.End == Candidate.Range.End) {
      Merged.back().Clauses.insert(
          Merged.back().Clauses.end(),
          std::make_move_iterator(Candidate.Clauses.begin()),
          std::make_move_iterator(Candidate.Clauses.end()));
      Merged.back().NativeRegionCount += Candidate.NativeRegionCount;
      continue;
    }
    Merged.push_back(std::move(Candidate));
  }
  Candidates = std::move(Merged);

  for (size_t I = 0; I < Candidates.size(); ++I) {
    RegionCandidate &Candidate = Candidates[I];
    if (hasCrossingRegions(Candidates, I)) {
      Rejected += Candidate.NativeRegionCount;
      continue;
    }
    std::vector<HighStmt> ProtectedBody;
    size_t InsertAt = 0;
    if (!extractProtectedSlice(Func.Body, Candidate.Range, EH.CodeRange,
                               ProtectedBody, InsertAt)) {
      Rejected += Candidate.NativeRegionCount;
      continue;
    }

    HighStmt Try;
    Try.Kind = Candidate.Kind;
    Try.Addr = Candidate.Range.Begin;
    Try.Body = std::move(ProtectedBody);
    Try.EHRange = Candidate.Range;
    Try.EHClauses = std::move(Candidate.Clauses);
    Try.EHClauseBodies.resize(Try.EHClauses.size());
    Try.EHIsReducible = true;
    Func.Body.insert(Func.Body.begin() + static_cast<ptrdiff_t>(InsertAt),
                     std::move(Try));
    Func.StructuredExceptionRegions += Candidate.NativeRegionCount;
  }
  Func.UnstructuredExceptionRegions += Rejected;
}

} // namespace neverd
