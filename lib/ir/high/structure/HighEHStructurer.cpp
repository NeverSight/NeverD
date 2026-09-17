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

#include "neverd/Limits.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cctype>
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

bool isCIdentifier(llvm::StringRef Name) {
  if (Name.empty() ||
      (!std::isalpha(static_cast<unsigned char>(Name.front())) &&
       Name.front() != '_'))
    return false;
  return llvm::all_of(Name, [](char Ch) {
    return std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_';
  });
}

/// Read MSVC `TypeDescriptor::{void *vftable; void *spare; char name[]}`.
std::string readMSVCTypeDescriptorName(const BinaryImage *Img,
                                       va_t DescriptorVA) {
  if (!Img || !DescriptorVA)
    return {};
  const size_t PointerSize = Img->is64Bit() ? 8 : 4;
  if (DescriptorVA > InvalidVA - 2 * PointerSize)
    return {};
  const va_t NameVA = DescriptorVA + 2 * PointerSize;
  std::string Name;
  for (size_t I = 0; I < limits::kMaxMsvcTypeDescriptorNameBytes; ++I) {
    if (I > InvalidVA - NameVA)
      return {};
    const uint8_t *Byte = Img->readVA(NameVA + I, 1);
    if (!Byte)
      return {};
    if (*Byte == 0)
      break;
    Name.push_back(static_cast<char>(*Byte));
  }
  if (Name.empty())
    return {};
  llvm::StringRef Mangled(Name);
  if (Mangled.starts_with(".?A") && Mangled.size() > 4) {
    llvm::StringRef Rest = Mangled.drop_front(4);
    const size_t At = Rest.find('@');
    if (At != llvm::StringRef::npos)
      Rest = Rest.take_front(At);
    if (isCIdentifier(Rest))
      return Rest.str();
  }
  return Name;
}

void fillCxxCatchType(HighEHClause &Clause, const BinaryImage *Img) {
  if (!Clause.TypeName.empty() || !Clause.TypeDescriptorVA)
    return;
  Clause.TypeName = readMSVCTypeDescriptorName(Img, Clause.TypeDescriptorVA);
}

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

bool extractAddressSlice(std::vector<HighStmt> &Statements,
                         const ExceptionAddressRange &Range,
                         const ExceptionAddressRange &FunctionRange,
                         std::vector<HighStmt> &Body, size_t &InsertAt,
                         bool IncludeFunctionEdgeUnknown = true) {
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
  if (IncludeFunctionEdgeUnknown && Range.Begin == FunctionRange.Begin)
    while (Begin != 0 && Classes[Begin - 1] == RangeClass::Unknown)
      --Begin;
  if (IncludeFunctionEdgeUnknown && Range.End == FunctionRange.End)
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
  /// Native C++ try-map states.  Nested tries can collapse to the same IP
  /// interval when inner-only states appear in the function body; those must
  /// stay separate HighIR tries, not sibling `catch` clauses.
  int32_t TryLow = 0;
  int32_t TryHigh = 0;
  bool HasTryStates = false;
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

void addSEHCandidates(const ExceptionFunction &EH, Arch TargetArch,
                      std::vector<RegionCandidate> &Candidates,
                      unsigned &Rejected) {
  if (!EH.SEH)
    return;
  std::map<std::pair<va_t, va_t>, size_t> ByRange;
  for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
    const std::optional<ExceptionAddressRange> SemanticRange =
        getSemanticSEHGuardedRange(Scope, TargetArch, EH.CodeRange);
    if (Scope.ParseStatus != ExceptionParseStatus::Complete ||
        !SemanticRange) {
      ++Rejected;
      continue;
    }
    auto Key = std::make_pair(SemanticRange->Begin, SemanticRange->End);
    size_t Index = 0;
    if (auto It = ByRange.find(Key); It != ByRange.end()) {
      Index = It->second;
    } else {
      Index = Candidates.size();
      ByRange.emplace(Key, Index);
      RegionCandidate Candidate;
      Candidate.Kind = StmtKind::SEHTry;
      Candidate.Range = *SemanticRange;
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

void addRegistrationCandidates(const ExceptionFunction &EH,
                               std::vector<RegionCandidate> &Candidates,
                               unsigned &Rejected) {
  if (!EH.Registration)
    return;
  const RegistrationChainInfo &Chain = *EH.Registration;
  if (Chain.Scopes.empty())
    return;
  if (Chain.TryLevelStores.empty()) {
    Rejected += static_cast<unsigned>(Chain.Scopes.size());
    return;
  }

  struct Interval {
    va_t Begin = 0;
    va_t End = 0;
  };
  std::vector<std::vector<Interval>> PerScope(Chain.Scopes.size());
  int32_t Level = Chain.SeededTryLevel.value_or(-1);
  va_t Cursor = EH.CodeRange.Begin;
  auto Flush = [&](va_t End) {
    if (End <= Cursor)
      return;
    int32_t Walk = Level;
    for (size_t Step = 0; Step < Chain.Scopes.size(); ++Step) {
      if (Walk < 0 || static_cast<size_t>(Walk) >= Chain.Scopes.size())
        break;
      PerScope[static_cast<size_t>(Walk)].push_back({Cursor, End});
      Walk = Chain.Scopes[static_cast<size_t>(Walk)].EnclosingLevel;
    }
  };
  for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores) {
    va_t Cut = Store.EndVA;
    if (Cut < EH.CodeRange.Begin)
      Cut = EH.CodeRange.Begin;
    if (Cut > EH.CodeRange.End)
      Cut = EH.CodeRange.End;
    Flush(Cut);
    Cursor = Cut;
    Level = Store.Level;
  }
  Flush(EH.CodeRange.End);

  for (size_t I = 0; I < Chain.Scopes.size(); ++I) {
    std::vector<Interval> &Iv = PerScope[I];
    if (Iv.empty()) {
      ++Rejected;
      continue;
    }
    std::sort(Iv.begin(), Iv.end(),
              [](const Interval &A, const Interval &B) {
                return A.Begin < B.Begin;
              });
    ExceptionAddressRange Range{Iv.front().Begin, Iv.front().End};
    bool Contiguous = true;
    for (size_t K = 1; K < Iv.size(); ++K) {
      if (Iv[K].Begin <= Range.End)
        Range.End = std::max(Range.End, Iv[K].End);
      else {
        Contiguous = false;
        break;
      }
    }
    if (!Contiguous || !Range.isValid()) {
      ++Rejected;
      continue;
    }

    const RegistrationScopeRecord &Scope = Chain.Scopes[I];
    RegionCandidate Candidate;
    Candidate.Kind = StmtKind::SEHTry;
    Candidate.Range = Range;
    Candidate.NativeRegionCount = 1;
    HighEHClause Clause;
    Clause.Kind = Scope.IsFinally ? HighEHClauseKind::SEHFinally
                                  : HighEHClauseKind::SEHExcept;
    Clause.FilterOrActionVA = Scope.FilterVA;
    Clause.HandlerVA = Scope.HandlerVA;
    Candidate.Clauses.push_back(std::move(Clause));
    Candidates.push_back(std::move(Candidate));
  }
}

void addCxxCandidates(const ExceptionFunction &EH, const BinaryImage *Img,
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
    Candidate.TryLow = Try.TryLow;
    Candidate.TryHigh = Try.TryHigh;
    Candidate.HasTryStates = true;
    for (const CxxCatchHandler &Catch : Try.Handlers) {
      HighEHClause Clause;
      Clause.Kind = HighEHClauseKind::CxxCatch;
      Clause.HandlerVA = Catch.HandlerVA;
      Clause.TypeDescriptorVA = Catch.TypeDescriptorVA;
      Clause.Adjectives = Catch.Adjectives;
      Clause.CatchObjectOffset = Catch.CatchObjectOffset;
      Clause.ParentFrameOffset = Catch.ParentFrameOffset;
      Clause.ContinuationVAs = Catch.ContinuationVAs;
      fillCxxCatchType(Clause, Img);
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

std::optional<va_t> windowsClauseBodyTarget(const HighEHClause &Clause) {
  if (Clause.Kind == HighEHClauseKind::SEHExcept ||
      Clause.Kind == HighEHClauseKind::CxxCatch)
    return Clause.HandlerVA;
  if (Clause.Kind == HighEHClauseKind::SEHFinally ||
      Clause.Kind == HighEHClauseKind::CxxCleanup)
    return Clause.FilterOrActionVA;
  return std::nullopt;
}

std::optional<ExceptionAddressRange>
uniqueHandlerBlockRange(const MedFunc &Med, const ExceptionFunction &EH,
                        va_t Target,
                        const std::vector<RegionCandidate> &ProtectedRegions) {
  if (Target == 0 || Target == InvalidVA)
    return std::nullopt;

  const MedBlock *Match = nullptr;
  for (const MedBlock &Block : Med.Blocks) {
    if (Target < Block.StartAddr || Target >= Block.EndAddr)
      continue;
    if (Match && Match != &Block)
      return std::nullopt;
    Match = &Block;
  }
  if (!Match)
    return std::nullopt;

  ExceptionAddressRange Range{Match->StartAddr, Match->EndAddr};
  if (!Range.isValid() || !EH.CodeRange.contains(Range))
    return std::nullopt;
  if (std::any_of(ProtectedRegions.begin(), ProtectedRegions.end(),
                  [&](const RegionCandidate &Candidate) {
                    return Range.overlaps(Candidate.Range);
                  }))
    return std::nullopt;
  return Range;
}

} // anonymous namespace

void MedToHighConverter::structureExceptionRegions(HighFunc &Func,
                                                   const MedFunc &Med) {
  if (!Func.ExceptionMetadata)
    return;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;

  std::vector<RegionCandidate> Candidates;
  unsigned Rejected = 0;
  if (EH.ParseStatus == ExceptionParseStatus::Complete) {
    addSEHCandidates(EH, TargetArch, Candidates, Rejected);
    addRegistrationCandidates(EH, Candidates, Rejected);
    addCxxCandidates(EH, Image, Candidates, Rejected);
    addItaniumCandidates(EH, Candidates, Rejected);
  } else {
    Rejected += EH.SEH ? static_cast<unsigned>(EH.SEH->Scopes.size()) : 0;
    Rejected +=
        EH.Registration ? static_cast<unsigned>(EH.Registration->Scopes.size())
                        : 0;
    Rejected += EH.Cxx ? static_cast<unsigned>(EH.Cxx->TryBlocks.size()) : 0;
    Rejected +=
        EH.Itanium ? static_cast<unsigned>(EH.Itanium->CallSites.size()) : 0;
  }

  // Inner-first makes a nested structured node an indivisible statement when
  // its enclosing range is processed.  The address classifier understands the
  // explicit EHRange, so nesting never relies on incidental statement order.
  std::stable_sort(Candidates.begin(), Candidates.end(),
                   [](const RegionCandidate &A, const RegionCandidate &B) {
                     const auto Span = [](const RegionCandidate &C) {
                       return C.HasTryStates ? (C.TryHigh - C.TryLow) : 0;
                     };
                     return std::make_tuple(A.Range.size(), Span(A),
                                            A.Range.Begin, A.Range.End,
                                            A.Kind) <
                            std::make_tuple(B.Range.size(), Span(B),
                                            B.Range.Begin, B.Range.End,
                                            B.Kind);
                   });

  // Several native try-map records may share one code interval (for example,
  // distinct ordered catch clauses).  Keep one HighIR try node and retain the
  // native-record count for completeness accounting.  Nested C++ tries that
  // collapse to the same IPs keep separate nodes so inner `try` stays nested.
  std::vector<RegionCandidate> Merged;
  for (RegionCandidate &Candidate : Candidates) {
    if (!Merged.empty() && Merged.back().Kind == Candidate.Kind &&
        Merged.back().Range.Begin == Candidate.Range.Begin &&
        Merged.back().Range.End == Candidate.Range.End) {
      const bool DistinctCxxTries =
          Candidate.Kind == StmtKind::CxxTry &&
          (Merged.back().HasTryStates != Candidate.HasTryStates ||
           Merged.back().TryLow != Candidate.TryLow ||
           Merged.back().TryHigh != Candidate.TryHigh);
      if (!DistinctCxxTries) {
        Merged.back().Clauses.insert(
            Merged.back().Clauses.end(),
            std::make_move_iterator(Candidate.Clauses.begin()),
            std::make_move_iterator(Candidate.Clauses.end()));
        Merged.back().NativeRegionCount += Candidate.NativeRegionCount;
        continue;
      }
    }
    Merged.push_back(std::move(Candidate));
  }
  Candidates = std::move(Merged);

  std::map<va_t, unsigned> WindowsTargetUses;
  for (const RegionCandidate &Candidate : Candidates)
    for (const HighEHClause &Clause : Candidate.Clauses)
      if (std::optional<va_t> Target = windowsClauseBodyTarget(Clause);
          Target && *Target != 0 && *Target != InvalidVA)
        ++WindowsTargetUses[*Target];

  for (size_t I = 0; I < Candidates.size(); ++I) {
    RegionCandidate &Candidate = Candidates[I];
    if (hasCrossingRegions(Candidates, I)) {
      Rejected += Candidate.NativeRegionCount;
      continue;
    }
    std::vector<HighStmt> ProtectedBody;
    size_t InsertAt = 0;
    if (!extractAddressSlice(Func.Body, Candidate.Range, EH.CodeRange,
                             ProtectedBody, InsertAt)) {
      Rejected += Candidate.NativeRegionCount;
      continue;
    }

    HighStmt Try;
    Try.Kind = Candidate.Kind;
    Try.Addr = Candidate.Range.Begin;
    Try.Body = std::move(ProtectedBody);
    Try.EHRange = Candidate.Range;
    std::vector<std::optional<va_t>> ClauseTargets;
    ClauseTargets.reserve(Candidate.Clauses.size());
    for (const HighEHClause &Clause : Candidate.Clauses)
      ClauseTargets.push_back(windowsClauseBodyTarget(Clause));
    Try.EHClauses = std::move(Candidate.Clauses);
    Try.EHClauseBodies.resize(Try.EHClauses.size());
    Try.EHIsReducible = true;
    Func.Body.insert(Func.Body.begin() + static_cast<ptrdiff_t>(InsertAt),
                     std::move(Try));

    std::vector<std::vector<HighStmt>> ClauseBodies(ClauseTargets.size());
    for (size_t ClauseIndex = 0; ClauseIndex < ClauseTargets.size();
         ++ClauseIndex) {
      std::optional<va_t> Target = ClauseTargets[ClauseIndex];
      if (!Target || WindowsTargetUses[*Target] != 1)
        continue;
      std::optional<ExceptionAddressRange> HandlerRange =
          uniqueHandlerBlockRange(Med, EH, *Target, Candidates);
      if (!HandlerRange)
        continue;
      size_t HandlerAt = 0;
      std::vector<HighStmt> HandlerBody;
      if (!extractAddressSlice(Func.Body, *HandlerRange, EH.CodeRange,
                               HandlerBody, HandlerAt,
                               /*IncludeFunctionEdgeUnknown=*/false))
        continue;
      ClauseBodies[ClauseIndex] = std::move(HandlerBody);
    }

    auto InsertedTry = std::find_if(
        Func.Body.begin(), Func.Body.end(), [&](const HighStmt &Stmt) {
          return Stmt.Kind == Candidate.Kind &&
                 Stmt.EHRange.Begin == Candidate.Range.Begin &&
                 Stmt.EHRange.End == Candidate.Range.End;
        });
    if (InsertedTry != Func.Body.end())
      InsertedTry->EHClauseBodies = std::move(ClauseBodies);

    // Filter thunks live in the same function as x86 registration EH but are
    // called only by the personality.  Drop them from the C body; the except
    // header already names the filter.
    if (InsertedTry != Func.Body.end()) {
      for (const HighEHClause &Clause : InsertedTry->EHClauses) {
        if (Clause.Kind != HighEHClauseKind::SEHExcept ||
            Clause.FilterOrActionVA == 0 ||
            Clause.FilterOrActionVA == Clause.HandlerVA)
          continue;
        std::optional<ExceptionAddressRange> FilterRange =
            uniqueHandlerBlockRange(Med, EH, Clause.FilterOrActionVA,
                                    Candidates);
        if (!FilterRange)
          continue;
        size_t FilterAt = 0;
        std::vector<HighStmt> FilterBody;
        extractAddressSlice(Func.Body, *FilterRange, EH.CodeRange, FilterBody,
                            FilterAt, /*IncludeFunctionEdgeUnknown=*/false);
      }
    }

    Func.StructuredExceptionRegions += Candidate.NativeRegionCount;
  }
  Func.UnstructuredExceptionRegions += Rejected;

  // x86 registration and VC6 C++ often have no IP map.  Still surface a
  // readable try around the recovered body rather than leaving a flat listing.
  if (Func.StructuredExceptionRegions == 0 && !Func.Body.empty() &&
      (EH.SEH || EH.Cxx || EH.Registration)) {
    HighStmt Try;
    Try.Kind = (EH.Cxx && !EH.Cxx->TryBlocks.empty()) ? StmtKind::CxxTry
                                                      : StmtKind::SEHTry;
    Try.EHRange = EH.CodeRange;
    Try.EHIsReducible = false;
    Try.Body = std::move(Func.Body);
    if (Try.Kind == StmtKind::CxxTry) {
      for (const CxxTryBlock &Block : EH.Cxx->TryBlocks) {
        for (const CxxCatchHandler &Catch : Block.Handlers) {
          HighEHClause Clause;
          Clause.Kind = HighEHClauseKind::CxxCatch;
          Clause.HandlerVA = Catch.HandlerVA;
          Clause.TypeDescriptorVA = Catch.TypeDescriptorVA;
          Clause.Adjectives = Catch.Adjectives;
          fillCxxCatchType(Clause, Image);
          Try.EHClauses.push_back(std::move(Clause));
          Try.EHClauseBodies.emplace_back();
        }
      }
    } else if (EH.Registration) {
      for (const RegistrationScopeRecord &Scope : EH.Registration->Scopes) {
        HighEHClause Clause;
        Clause.Kind = Scope.IsFinally ? HighEHClauseKind::SEHFinally
                                      : HighEHClauseKind::SEHExcept;
        Clause.FilterOrActionVA = Scope.FilterVA;
        Clause.HandlerVA = Scope.HandlerVA;
        Try.EHClauses.push_back(std::move(Clause));
        Try.EHClauseBodies.emplace_back();
      }
    } else if (EH.SEH) {
      for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
        HighEHClause Clause;
        Clause.Kind = Scope.Kind == SEHScopeKind::Finally
                          ? HighEHClauseKind::SEHFinally
                          : HighEHClauseKind::SEHExcept;
        Clause.FilterOrActionVA = Scope.FilterOrFinallyVA;
        Clause.HandlerVA = Scope.HandlerVA;
        Try.EHClauses.push_back(std::move(Clause));
        Try.EHClauseBodies.emplace_back();
      }
    }
    if (Try.EHClauses.empty()) {
      HighEHClause Clause;
      Clause.Kind = Try.Kind == StmtKind::CxxTry ? HighEHClauseKind::CxxCatch
                                                 : HighEHClauseKind::SEHExcept;
      Try.EHClauses.push_back(std::move(Clause));
      Try.EHClauseBodies.emplace_back();
    }
    Func.Body.clear();
    Func.Body.push_back(std::move(Try));
    if (Func.UnstructuredExceptionRegions == 0)
      Func.UnstructuredExceptionRegions = 1;
  }
}

} // namespace neverd
