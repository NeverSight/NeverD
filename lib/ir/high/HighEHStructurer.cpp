//===- HighEHStructurer.cpp - Conservative Windows EH structuring -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Converts normalized Windows guarded ranges/state maps into explicit HighIR
/// exception nodes.  The transform is deliberately interval-conservative: it
/// moves statements only when one contiguous HighIR slice is wholly contained
/// by a validated native range.  Crossing or address-less shapes stay in their
/// original order and are reported through the function's unstructured count.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

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
  if ((Stmt.Kind == StmtKind::SEHTry || Stmt.Kind == StmtKind::CxxTry) &&
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
  } else {
    Rejected += EH.SEH ? static_cast<unsigned>(EH.SEH->Scopes.size()) : 0;
    Rejected += EH.Cxx ? static_cast<unsigned>(EH.Cxx->TryBlocks.size()) : 0;
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
