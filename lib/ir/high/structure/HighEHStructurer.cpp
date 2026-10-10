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
/// That slice may sit in a nested if/else/try list after `structureIfElse`;
/// a crossing parent is not a reason to drop the inner range.  Attached
/// cleanup funclets are not part of a try's address footprint.  Crossing or
/// address-less shapes stay in their original order and are reported through
/// the function's unstructured count.
///
//===----------------------------------------------------------------------===//

#include "HighCFSimplifyDetail.h"
#include "X86RegistrationTry.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/ir/med/X86RegistrationFrame.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <set>
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
  if (const std::string Spelling = msvcRttiTypeSpelling(Name);
      !Spelling.empty())
    return Spelling;
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

HighEHClause makeCxxCatchClause(const CxxCatchHandler &Catch,
                                const BinaryImage *Img,
                                const RegistrationStateAnalysis *Registration,
                                uint32_t TryIndex, uint32_t CatchIndex) {
  HighEHClause Clause;
  Clause.Kind = HighEHClauseKind::CxxCatch;
  Clause.HandlerVA = Catch.HandlerVA;
  Clause.TypeDescriptorVA = Catch.TypeDescriptorVA;
  Clause.Adjectives = Catch.Adjectives;
  Clause.CatchObjectOffset = Catch.CatchObjectOffset;
  Clause.ParentFrameOffset = Catch.ParentFrameOffset;
  Clause.ContinuationVAs = Catch.ContinuationVAs;
  if (Registration && Registration->CxxContinuationsComplete &&
      Registration->CxxContinuations.size() <=
          limits::kMaxRegistrationEHRecords) {
    for (const auto &Resume : Registration->CxxContinuations)
      if (Resume.TryIndex == TryIndex && Resume.CatchIndex == CatchIndex)
        Clause.ContinuationVAs.push_back(Resume.TargetVA);
    std::sort(Clause.ContinuationVAs.begin(), Clause.ContinuationVAs.end());
    Clause.ContinuationVAs.erase(std::unique(Clause.ContinuationVAs.begin(),
                                             Clause.ContinuationVAs.end()),
                                 Clause.ContinuationVAs.end());
  }
  fillCxxCatchType(Clause, Img);
  return Clause;
}

struct AddressFootprint {
  bool HasInside = false;
  bool HasOutside = false;
};

struct AddressSet {
  ExceptionAddressRange Span;
  std::vector<ExceptionAddressRange> Parts;
  bool RequireCall = false;
  /// The parts are ranges of one split scope: a plain union, not the two
  /// arms of a C++ cleanup diamond.
  bool SplitScope = false;

  AddressSet(ExceptionAddressRange R) : Span(R) {}
  AddressSet(ExceptionAddressRange R, std::vector<ExceptionAddressRange> P)
      : Span(R), Parts(std::move(P)), RequireCall(!P.empty()) {}

  bool contains(va_t Address) const {
    if (Parts.empty())
      return Span.contains(Address);
    for (const ExceptionAddressRange &P : Parts)
      if (P.contains(Address))
        return true;
    return false;
  }
  bool contains(const ExceptionAddressRange &Other) const {
    if (Parts.empty())
      return Span.contains(Other);
    return Other.isValid() && contains(Other.Begin) &&
           (Other.End <= Other.Begin + 1 || contains(Other.End - 1));
  }
  bool overlaps(const ExceptionAddressRange &Other) const {
    if (Parts.empty())
      return Span.overlaps(Other);
    for (const ExceptionAddressRange &P : Parts)
      if (P.overlaps(Other))
        return true;
    return false;
  }
};

void classifyStatements(const std::vector<HighStmt> &Statements,
                        const AddressSet &Range, AddressFootprint &Result);

void classifyStatement(const HighStmt &Stmt, const AddressSet &Range,
                       AddressFootprint &Result) {
  auto AddAddress = [&](va_t Address) {
    if (Address == 0 || Address == InvalidVA)
      return;
    if (Range.contains(Address))
      Result.HasInside = true;
    else
      Result.HasOutside = true;
  };

  // IfElse: the cond can sit in the previous IP state (FH3 delays the
  // state bump until the first call in an arm).  Arms decide the
  // footprint so a live parent can wrap the whole diamond.  A lone If
  // still uses Stmt.Addr — its fallthrough is not ElseBody, and
  // ignoring the cond would swallow the if when only the then-arm is
  // in range.
  if (Stmt.Kind != StmtKind::IfElse)
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
  // Cleanup / catch funclets live at ActionVA, outside the protected IP
  // range.  Walking them here makes an inner try Crossing for every parent
  // that does not also cover the funclet, so the parent cannot wrap it.
}

void classifyStatements(const std::vector<HighStmt> &Statements,
                        const AddressSet &Range, AddressFootprint &Result) {
  for (const HighStmt &Stmt : Statements)
    classifyStatement(Stmt, Range, Result);
}

RangeClass classifyStatement(const HighStmt &Stmt, const AddressSet &Range) {
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

bool isCallStmt(const HighStmt &Stmt) {
  return Stmt.Kind == StmtKind::Call ||
         (Stmt.Kind == StmtKind::Assign && Stmt.Val &&
          Stmt.Val->Kind == ExprKind::Call);
}

bool stmtHasCoveredCall(const HighStmt &Stmt, const AddressSet &Range) {
  bool Found = false;
  std::function<void(const HighStmt &)> Walk = [&](const HighStmt &S) {
    if (Found)
      return;
    if (isCallStmt(S) && Range.contains(S.Addr)) {
      Found = true;
      return;
    }
    for (const HighStmt &C : S.Body)
      Walk(C);
    for (const HighStmt &C : S.ElseBody)
      Walk(C);
    for (const HighStmt &C : S.DefaultBody)
      Walk(C);
    for (const auto &Case : S.Cases)
      for (const HighStmt &C : Case.Body)
        Walk(C);
  };
  Walk(Stmt);
  return Found;
}

bool armHasCoveredCall(const std::vector<HighStmt> &Arm,
                       const AddressSet &Range) {
  for (const HighStmt &S : Arm)
    if (stmtHasCoveredCall(S, Range))
      return true;
  return false;
}

bool isCoverDiamondIfElse(const HighStmt &Stmt, const AddressSet &Range) {
  return Stmt.Kind == StmtKind::IfElse && armHasCoveredCall(Stmt.Body, Range) &&
         armHasCoveredCall(Stmt.ElseBody, Range);
}

unsigned coverPartsTouched(const HighStmt &Stmt, const AddressSet &Range) {
  unsigned Bits = 0;
  const unsigned N = static_cast<unsigned>(Range.Parts.size());
  for (unsigned I = 0; I < N && I < 32; ++I) {
    AddressSet One{Range.Parts[I]};
    if (stmtHasCoveredCall(Stmt, One))
      Bits |= 1u << I;
  }
  return Bits;
}

bool extractAddressSlice(std::vector<HighStmt> &Statements,
                         const AddressSet &Range,
                         const ExceptionAddressRange &FunctionRange,
                         std::vector<HighStmt> &Body, size_t &InsertAt,
                         bool IncludeFunctionEdgeUnknown,
                         std::vector<HighStmt> **Host) {
  std::optional<size_t> First;
  std::optional<size_t> Last;
  std::vector<RangeClass> Classes;
  Classes.reserve(Statements.size());
  bool Crossing = false;
  for (size_t I = 0; I < Statements.size(); ++I) {
    RangeClass Class = classifyStatement(Statements[I], Range);
    Classes.push_back(Class);
    if (Class == RangeClass::Crossing)
      Crossing = true;
    if (Class == RangeClass::Inside) {
      if (!First)
        First = I;
      Last = I;
    }
  }
  bool Contiguous = First && Last && !Crossing;
  if (Contiguous) {
    for (size_t I = *First; I <= *Last; ++I)
      if (Classes[I] == RangeClass::Outside)
        Contiguous = false;
  }
  if (Contiguous && !Range.Parts.empty() && !Range.SplitScope) {
    // A split cleanup IP set is the two arms of one diamond.  A lone
    // if-goto that merely sits in one fragment is not.
    if (*Last != *First || !isCoverDiamondIfElse(Statements[*First], Range))
      Contiguous = false;
  }
  if (Contiguous) {
    // Address-less synthetic statements at a native edge belong to the range
    // only when the range itself reaches that function edge.  This captures a
    // synthesized trailing return without swallowing an unrelated neighbour.
    size_t Begin = *First;
    size_t End = *Last + 1;
    if (IncludeFunctionEdgeUnknown && Range.Span.Begin == FunctionRange.Begin)
      while (Begin != 0 && Classes[Begin - 1] == RangeClass::Unknown)
        --Begin;
    if (IncludeFunctionEdgeUnknown && Range.Span.End == FunctionRange.End)
      while (End < Classes.size() && Classes[End] == RangeClass::Unknown)
        ++End;

    Body.reserve(End - Begin);
    for (size_t I = Begin; I < End; ++I)
      Body.push_back(std::move(Statements[I]));
    Statements.erase(Statements.begin() + static_cast<ptrdiff_t>(Begin),
                     Statements.begin() + static_cast<ptrdiff_t>(End));
    InsertAt = Begin;
    if (Host)
      *Host = &Statements;
    return true;
  }

  // Split-cover cleanup is either one IfElse whose arms each contain a
  // covered call, or an if/goto plus the later sibling that holds the
  // other arm.  A lone if-goto in one fragment has no covered call.
  if (!Range.Parts.empty() && !Range.SplitScope) {
    std::optional<size_t> FirstCall;
    std::optional<size_t> LastCall;
    unsigned SeenParts = 0;
    bool Hole = false;
    bool AfterCall = false;
    for (size_t I = 0; I < Statements.size(); ++I) {
      const unsigned Parts = coverPartsTouched(Statements[I], Range);
      if (Parts == 0) {
        if (AfterCall && Classes[I] == RangeClass::Outside)
          AfterCall = false;
        continue;
      }
      if (!AfterCall && FirstCall)
        Hole = true;
      if (!FirstCall)
        FirstCall = I;
      LastCall = I;
      SeenParts |= Parts;
      AfterCall = true;
    }
    if (FirstCall && LastCall && !Hole && (SeenParts & (SeenParts - 1)) != 0 &&
        (*FirstCall != *LastCall ||
         isCoverDiamondIfElse(Statements[*FirstCall], Range))) {
      bool SpanOk = true;
      for (size_t I = *FirstCall; I <= *LastCall; ++I)
        if (Classes[I] == RangeClass::Outside)
          SpanOk = false;
      if (SpanOk) {
        const size_t Begin = *FirstCall;
        const size_t End = *LastCall + 1;
        Body.reserve(End - Begin);
        for (size_t I = Begin; I < End; ++I)
          Body.push_back(std::move(Statements[I]));
        Statements.erase(Statements.begin() + static_cast<ptrdiff_t>(Begin),
                         Statements.begin() + static_cast<ptrdiff_t>(End));
        InsertAt = Begin;
        if (Host)
          *Host = &Statements;
        return true;
      }
    }
  }

  // `structureIfElse` often nests a cleanup IP range inside `if` / `else`.
  // A crossing parent is not unstructured; the contiguous slice is inner.
  // A try the range holds is never such a parent: placing the range in its
  // body would nest the two the wrong way round, so an exception would
  // reach their handlers in the wrong order.
  for (HighStmt &Stmt : Statements) {
    auto Recurse = [&](std::vector<HighStmt> &Child) {
      return !Child.empty() &&
             extractAddressSlice(Child, Range, FunctionRange, Body, InsertAt,
                                 /*IncludeFunctionEdgeUnknown=*/false, Host);
    };
    const bool HeldTry =
        (Stmt.Kind == StmtKind::SEHTry || Stmt.Kind == StmtKind::CxxTry ||
         Stmt.Kind == StmtKind::ItaniumTry) &&
        Stmt.EHRange.isValid() && Range.contains(Stmt.EHRange);
    if ((!HeldTry && Recurse(Stmt.Body)) || Recurse(Stmt.ElseBody) ||
        Recurse(Stmt.DefaultBody))
      return true;
    for (SwitchCase &Case : Stmt.Cases)
      if (Recurse(Case.Body))
        return true;
  }
  return false;
}

bool extractAddressSlice(std::vector<HighStmt> &Statements,
                         const AddressSet &Range,
                         const ExceptionAddressRange &FunctionRange,
                         std::vector<HighStmt> &Body, size_t &InsertAt,
                         bool IncludeFunctionEdgeUnknown = true) {
  std::vector<HighStmt> *Host = nullptr;
  return extractAddressSlice(Statements, Range, FunctionRange, Body, InsertAt,
                             IncludeFunctionEdgeUnknown, &Host);
}

struct RegionCandidate {
  StmtKind Kind = StmtKind::SEHTry;
  ExceptionAddressRange Range;
  std::vector<ExceptionAddressRange> Cover;
  std::vector<HighEHClause> Clauses;
  unsigned NativeRegionCount = 0;
  /// Native C++ try-map states.  Nested tries can collapse to the same IP
  /// interval when inner-only states appear in the function body; those must
  /// stay separate HighIR tries, not sibling `catch` clauses.
  int32_t TryLow = 0;
  int32_t TryHigh = 0;
  bool HasTryStates = false;
};

bool cxxStateOnUnwindChain(const CxxExceptionInfo &Cxx, int32_t Current,
                           int32_t Target) {
  unsigned Guard = 0;
  const unsigned Limit = static_cast<unsigned>(Cxx.UnwindMap.size()) + 1;
  while (Current >= 0 && Guard++ < Limit) {
    if (static_cast<size_t>(Current) >= Cxx.UnwindMap.size())
      return false;
    if (Current == Target)
      return true;
    Current = Cxx.UnwindMap[Current].ToState;
  }
  return false;
}

template <typename Pred>
std::vector<ExceptionAddressRange>
codeRangesMatching(const ExceptionFunction &EH, const CxxExceptionInfo &Cxx,
                   Pred Live,
                   const RegistrationStateAnalysis *Registration = nullptr) {
  if (EH.Registration) {
    if (!Registration || (EH.Registration->hasCxxCallbackStack() &&
                          !cxxRegistrationFrameCoordinate(EH, Registration)))
      return {};
    auto Ranges = registrationRangesWhere(*Registration, Live);
    return Ranges ? std::move(*Ranges) : std::vector<ExceptionAddressRange>{};
  }
  std::vector<ExceptionAddressRange> Ranges;
  va_t Cursor = EH.CodeRange.Begin;
  int32_t State = -1;
  auto Add = [&](va_t Begin, va_t End, int32_t SegmentState) {
    Begin = std::max(Begin, EH.CodeRange.Begin);
    End = std::min(End, EH.CodeRange.End);
    if (Begin >= End || !Live(SegmentState))
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

std::vector<ExceptionAddressRange>
codeRangesForStates(const ExceptionFunction &EH, const CxxExceptionInfo &Cxx,
                    int32_t LowState, int32_t HighState,
                    const RegistrationStateAnalysis *Registration = nullptr) {
  return codeRangesMatching(
      EH, Cxx,
      [&](int32_t SegmentState) {
        return SegmentState >= LowState && SegmentState <= HighState;
      },
      Registration);
}

/// IPs that would run this cleanup, including child states.  A parent
/// object whose exact IP fragments are split by nested states still has
/// one live interval.
std::vector<ExceptionAddressRange> codeRangesWhereStateIsLive(
    const ExceptionFunction &EH, const CxxExceptionInfo &Cxx, int32_t Target,
    const RegistrationStateAnalysis *Registration = nullptr) {
  return codeRangesMatching(
      EH, Cxx,
      [&](int32_t SegmentState) {
        return cxxStateOnUnwindChain(Cxx, SegmentState, Target);
      },
      Registration);
}

/// A try whose IP states are interrupted by `state=-1` holes yields more than
/// one interval. The trailing fragment is a continuation, not a second try.
ExceptionAddressRange
primaryCxxTryRange(const std::vector<ExceptionAddressRange> &Ranges) {
  ExceptionAddressRange Best;
  for (const ExceptionAddressRange &Range : Ranges)
    if (Range.isValid() && Range.size() > Best.size())
      Best = Range;
  return Best;
}

void addSEHCandidates(const ExceptionFunction &EH, Arch TargetArch,
                      std::vector<RegionCandidate> &Candidates,
                      unsigned &Rejected) {
  if (!EH.SEH)
    return;
  std::map<std::pair<va_t, va_t>, size_t> ByRange;
  for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
    const std::optional<ExceptionAddressRange> SemanticRange =
        getSemanticSEHGuardedRange(Scope, TargetArch, EH);
    if (Scope.ParseStatus != ExceptionParseStatus::Complete || !SemanticRange) {
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
                               const RegistrationStateAnalysis *States,
                               std::vector<RegionCandidate> &Candidates,
                               unsigned &Rejected) {
  if (!EH.Registration)
    return;
  const RegistrationChainInfo &Chain = *EH.Registration;
  if (Chain.Scopes.empty())
    return;
  if (!States || !States->Complete) {
    Rejected += static_cast<unsigned>(Chain.Scopes.size());
    return;
  }

  for (size_t I = 0; I < Chain.Scopes.size(); ++I) {
    auto Ranges = registrationRangesWhere(*States, [&](int32_t Level) {
      for (size_t Step = 0; Step < Chain.Scopes.size(); ++Step) {
        if (Level < 0 || static_cast<size_t>(Level) >= Chain.Scopes.size())
          break;
        if (static_cast<size_t>(Level) == I)
          return true;
        Level = Chain.Scopes[Level].EnclosingLevel;
      }
      return false;
    });
    if (!Ranges || Ranges->size() != 1) {
      ++Rejected;
      continue;
    }

    const RegistrationScopeRecord &Scope = Chain.Scopes[I];
    RegionCandidate Candidate;
    Candidate.Kind = StmtKind::SEHTry;
    Candidate.Range = Ranges->front();
    Candidate.NativeRegionCount = 1;
    HighEHClause Clause;
    Clause.Kind = Scope.IsFinally ? HighEHClauseKind::SEHFinally
                                  : HighEHClauseKind::SEHExcept;
    Clause.FilterOrActionVA =
        Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA;
    Clause.HandlerVA = Scope.HandlerVA;
    Candidate.Clauses.push_back(std::move(Clause));
    Candidates.push_back(std::move(Candidate));
  }
}

void addCxxCandidates(const ExceptionFunction &EH, const MedFunc &Med,
                      const BinaryImage *Img,
                      const RegistrationStateAnalysis *Registration,
                      std::vector<RegionCandidate> &Candidates,
                      unsigned &Rejected) {
  if (!EH.Cxx)
    return;
  const CxxExceptionInfo &Cxx = *EH.Cxx;
  for (uint32_t TryIndex = 0; TryIndex < Cxx.TryBlocks.size(); ++TryIndex) {
    const CxxTryBlock &Try = Cxx.TryBlocks[TryIndex];
    if (EH.Registration && EH.Registration->hasCxxCallbackStack() &&
        llvm::any_of(Try.Handlers, [&](const auto &Catch) {
          return !registrationCallbackRegion(Med, Catch.HandlerVA);
        })) {
      ++Rejected;
      continue;
    }
    auto Ranges =
        codeRangesForStates(EH, Cxx, Try.TryLow, Try.TryHigh, Registration);
    // Catch continuations may merge protected and unprotected source states
    // after the ordinary component has terminated. In synchronous C++ that
    // join does not prevent a lexical try around the independently checked
    // terminal component. Use the same ownership proof during extraction.
    bool TerminalRegistration = false;
    if (EH.Registration && EH.Registration->hasCxxCallbackStack()) {
      auto Terminal =
          terminalRegistrationTryRanges(Med, Try.TryLow, Try.TryHigh);
      if (!Terminal.empty()) {
        Ranges = std::move(Terminal);
        TerminalRegistration = true;
      }
    }
    const bool SplitRegistration =
        EH.Registration && (Ranges.size() > 1 || TerminalRegistration);
    const ExceptionAddressRange Range =
        SplitRegistration
            ? ExceptionAddressRange{Ranges.front().Begin, Ranges.back().End}
            : primaryCxxTryRange(Ranges);
    if (!Range.isValid()) {
      ++Rejected;
      continue;
    }

    RegionCandidate Candidate;
    Candidate.Kind = StmtKind::CxxTry;
    Candidate.Range = Range;
    if (SplitRegistration)
      Candidate.Cover = std::move(Ranges);
    Candidate.NativeRegionCount = 1;
    Candidate.TryLow = Try.TryLow;
    Candidate.TryHigh = Try.TryHigh;
    Candidate.HasTryStates = true;
    for (uint32_t CatchIndex = 0; CatchIndex < Try.Handlers.size();
         ++CatchIndex) {
      const CxxCatchHandler &Catch = Try.Handlers[CatchIndex];
      HighEHClause Clause =
          makeCxxCatchClause(Catch, Img, Registration, TryIndex, CatchIndex);
      if (!EH.Registration)
        for (const auto &Entry : Med.CxxContinuationEntries)
          if (Entry.SourceEntry == Catch.HandlerVA &&
              !llvm::is_contained(Clause.ContinuationVAs, Entry.Target))
            Clause.ContinuationVAs.push_back(Entry.Target);
      Candidate.Clauses.push_back(std::move(Clause));
    }
    for (int32_t State = Try.TryLow; State <= Try.TryHigh; ++State) {
      if (State < 0 || State >= static_cast<int32_t>(Cxx.UnwindMap.size()))
        continue;
      bool OwnedByInner = false;
      for (const CxxTryBlock &Other : Cxx.TryBlocks) {
        if (Other.TryLow == Try.TryLow && Other.TryHigh == Try.TryHigh)
          continue;
        if (Other.TryLow >= Try.TryLow && Other.TryHigh <= Try.TryHigh &&
            State >= Other.TryLow && State <= Other.TryHigh) {
          OwnedByInner = true;
          break;
        }
      }
      if (OwnedByInner)
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

/// FH3/FH4 frames that only construct locals still have an UnwindMap and IP
/// states, but no TryBlock.  That is a destructor scope, not SEH.  Hex-Rays
/// prints `__wind`/`__unwind`; HighC keeps `try` + `/* unwind cleanup */`
/// plus the attached funclet body, which is already the catch-with-dtor shape.
void addCxxCleanupOnlyCandidates(const ExceptionFunction &EH,
                                 const RegistrationStateAnalysis *Registration,
                                 std::vector<RegionCandidate> &Candidates,
                                 unsigned &Rejected) {
  if (!EH.Cxx)
    return;
  const CxxExceptionInfo &Cxx = *EH.Cxx;
  auto coveredByTry = [&](int32_t State) {
    for (const CxxTryBlock &Try : Cxx.TryBlocks)
      if (State >= Try.TryLow && State <= Try.TryHigh)
        return true;
    return false;
  };

  std::map<std::pair<va_t, va_t>, size_t> ByRange;
  for (int32_t State = 0; State < static_cast<int32_t>(Cxx.UnwindMap.size());
       ++State) {
    const CxxUnwindAction &Action = Cxx.UnwindMap[State];
    if (Action.ActionVA == 0 || coveredByTry(State))
      continue;
    // A cleanup needs its own entry/return ABI proof before it can supply a
    // callback clause body. Catch projection does not establish that ABI.
    if (EH.Registration && EH.Registration->hasCxxCallbackStack()) {
      ++Rejected;
      continue;
    }
    std::vector<ExceptionAddressRange> Ranges =
        codeRangesForStates(EH, Cxx, State, State, Registration);
    if (Ranges.size() != 1 || !Ranges.front().isValid()) {
      std::vector<ExceptionAddressRange> Live =
          codeRangesWhereStateIsLive(EH, Cxx, State, Registration);
      if (Live.size() == 1 && Live.front().isValid())
        Ranges = std::move(Live);
    }
    if ((Ranges.size() != 1 || !Ranges.front().isValid()) && !EH.Registration &&
        Cxx.TryBlocks.empty() && Cxx.IPMap.empty() && EH.CodeRange.isValid())
      Ranges = {EH.CodeRange};
    std::vector<ExceptionAddressRange> Cover;
    if (Ranges.size() > 1) {
      ExceptionAddressRange BBox;
      bool AllValid = true;
      for (const ExceptionAddressRange &R : Ranges) {
        if (!R.isValid()) {
          AllValid = false;
          break;
        }
        if (!BBox.isValid())
          BBox = R;
        else {
          BBox.Begin = std::min(BBox.Begin, R.Begin);
          BBox.End = std::max(BBox.End, R.End);
        }
      }
      if (!AllValid || !BBox.isValid()) {
        ++Rejected;
        continue;
      }
      Cover = Ranges;
      Ranges = {BBox};
    }
    if (Ranges.size() != 1 || !Ranges.front().isValid()) {
      ++Rejected;
      continue;
    }

    HighEHClause Clause;
    Clause.Kind = HighEHClauseKind::CxxCleanup;
    Clause.FilterOrActionVA = Action.ActionVA;
    Clause.State = State;
    Clause.UnwindActionKind = Action.Kind;
    Clause.UnwindObjectOffset = Action.ObjectOffset;

    const auto Key = std::make_pair(Ranges.front().Begin, Ranges.front().End);
    if (const auto It = ByRange.find(Key); It != ByRange.end()) {
      Candidates[It->second].Clauses.push_back(std::move(Clause));
      Candidates[It->second].TryHigh = State;
      ++Candidates[It->second].NativeRegionCount;
      continue;
    }

    RegionCandidate Candidate;
    Candidate.Kind = StmtKind::CxxTry;
    Candidate.Range = Ranges.front();
    Candidate.Cover = std::move(Cover);
    Candidate.NativeRegionCount = 1;
    Candidate.TryLow = State;
    Candidate.TryHigh = State;
    Candidate.HasTryStates = true;
    Candidate.Clauses.push_back(std::move(Clause));
    ByRange.emplace(Key, Candidates.size());
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

/// Whether a jump in \p Outside lands on a statement of \p Slice other than
/// the region's entry. Machine code may enter a protected range anywhere; a C
/// `__try` block only at its top, where the entry label precedes it.
bool jumpEntersSlice(const std::vector<HighStmt> &Outside,
                     const std::vector<HighStmt> &Slice, va_t Entry) {
  std::set<va_t> Labels;
  auto Collect = [&](auto &&Self, const std::vector<HighStmt> &List) -> void {
    for (const HighStmt &S : List) {
      if (S.Addr && S.Addr != InvalidVA && S.Addr != Entry)
        Labels.insert(S.Addr);
      Self(Self, S.Body);
      Self(Self, S.ElseBody);
      for (const auto &Case : S.Cases)
        Self(Self, Case.Body);
      Self(Self, S.DefaultBody);
      for (const auto &Clause : S.EHClauseBodies)
        Self(Self, Clause);
    }
  };
  Collect(Collect, Slice);
  bool Entered = false;
  auto Scan = [&](auto &&Self, const std::vector<HighStmt> &List) -> void {
    for (const HighStmt &S : List) {
      if (Entered)
        return;
      Entered = S.Kind == StmtKind::Goto && Labels.count(S.GotoTarget);
      Self(Self, S.Body);
      Self(Self, S.ElseBody);
      for (const auto &Case : S.Cases)
        Self(Self, Case.Body);
      Self(Self, S.DefaultBody);
      for (const auto &Clause : S.EHClauseBodies)
        Self(Self, Clause);
    }
  };
  Scan(Scan, Outside);
  return Entered;
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

/// Whether \p Stmts, outside the slice taken for a try over \p Range, still
/// hold a statement of that range that may raise an exception.
bool guardedStatementLeftOut(const std::vector<HighStmt> &Stmts,
                             const ExceptionAddressRange &Range) {
  for (const HighStmt &S : Stmts) {
    if (S.Addr && S.Addr != InvalidVA && Range.contains(S.Addr) &&
        highStmtMayFault(S))
      return true;
    for (const auto *List : {&S.Body, &S.ElseBody, &S.DefaultBody})
      if (guardedStatementLeftOut(*List, Range))
        return true;
    for (const SwitchCase &Case : S.Cases)
      if (guardedStatementLeftOut(Case.Body, Range))
        return true;
    for (const std::vector<HighStmt> &ClauseBody : S.EHClauseBodies)
      if (guardedStatementLeftOut(ClauseBody, Range))
        return true;
  }
  return false;
}

/// The list and index of the try statement of \p Kind over \p Range.  A
/// slice extraction moves statements, so callers look it up again after one.
std::optional<std::pair<std::vector<HighStmt> *, size_t>>
findTryStmt(std::vector<HighStmt> &List, StmtKind Kind,
            const ExceptionAddressRange &Range) {
  for (size_t I = 0; I < List.size(); ++I) {
    HighStmt &S = List[I];
    if (S.Kind == Kind && S.EHRange.Begin == Range.Begin &&
        S.EHRange.End == Range.End)
      return std::make_pair(&List, I);
    for (std::vector<HighStmt> *Child : {&S.Body, &S.ElseBody, &S.DefaultBody})
      if (auto Found = findTryStmt(*Child, Kind, Range))
        return Found;
    for (SwitchCase &Case : S.Cases)
      if (auto Found = findTryStmt(Case.Body, Kind, Range))
        return Found;
    for (std::vector<HighStmt> &ClauseBody : S.EHClauseBodies)
      if (auto Found = findTryStmt(ClauseBody, Kind, Range))
        return Found;
  }
  return std::nullopt;
}

/// Whether a break or continue in \p List leaves it.
bool loopJumpLeaves(const std::vector<HighStmt> &List, bool BreakOwned = false,
                    bool ContinueOwned = false) {
  for (const HighStmt &S : List) {
    if ((S.Kind == StmtKind::Break && !BreakOwned) ||
        (S.Kind == StmtKind::Continue && !ContinueOwned))
      return true;
    const bool Loop = S.Kind == StmtKind::While ||
                      S.Kind == StmtKind::DoWhile || S.Kind == StmtKind::For;
    const bool Breaks = BreakOwned || Loop || S.Kind == StmtKind::Switch;
    const bool Continues = ContinueOwned || Loop;
    if (loopJumpLeaves(S.Body, Breaks, Continues) ||
        loopJumpLeaves(S.ElseBody, Breaks, Continues) ||
        loopJumpLeaves(S.DefaultBody, Breaks, Continues))
      return true;
    for (const SwitchCase &Case : S.Cases)
      if (loopJumpLeaves(Case.Body, Breaks, Continues))
        return true;
    for (const std::vector<HighStmt> &ClauseBody : S.EHClauseBodies)
      if (loopJumpLeaves(ClauseBody, BreakOwned, ContinueOwned))
        return true;
  }
  return false;
}

/// MSVC can split one __try into ranges sharing its clauses: parts of the
/// protected body it placed after the handler.  Move the statements of
/// \p Part to the end of the try of \p Kind over \p TryRange when only that
/// try's body enters them and neither runs into the other; a try body that
/// would fall off its end first gets an explicit jump to what follows it.
bool absorbSplitTryPart(std::vector<HighStmt> &Body, StmtKind Kind,
                        const ExceptionAddressRange &TryRange,
                        const ExceptionAddressRange &Part,
                        const ExceptionAddressRange &FunctionRange) {
  std::vector<HighStmt> Slice;
  size_t SliceAt = 0;
  std::vector<HighStmt> *SliceHost = nullptr;
  if (!extractAddressSlice(Body, Part, FunctionRange, Slice, SliceAt,
                           /*IncludeFunctionEdgeUnknown=*/false, &SliceHost) ||
      !SliceHost || Slice.empty())
    return false;
  // Nothing falls into the part, and the part falls into nothing.
  const bool Isolated =
      SliceAt != 0 && highStmtEndsItsBlock((*SliceHost)[SliceAt - 1]) &&
      highStmtEndsItsBlock(Slice.back()) && !loopJumpLeaves(Slice);
  auto Restore = [&] {
    SliceHost->insert(SliceHost->begin() + static_cast<ptrdiff_t>(SliceAt),
                      std::make_move_iterator(Slice.begin()),
                      std::make_move_iterator(Slice.end()));
    return false;
  };
  const auto TrySite = findTryStmt(Body, Kind, TryRange);
  if (!Isolated || !TrySite)
    return Restore();
  std::vector<HighStmt> &TryHost = *TrySite->first;
  const size_t TryAt = TrySite->second;

  // Only the try's own body may jump into the part.
  std::set<va_t> Labels;
  walkStmts(Slice, [&](const HighStmt &S) {
    if (S.Addr != 0 && S.Addr != InvalidVA)
      Labels.insert(S.Addr);
  });
  bool Entered = false;
  std::map<va_t, unsigned> LabelStarts;
  std::function<void(const std::vector<HighStmt> &, bool)> Scan =
      [&](const std::vector<HighStmt> &List, bool InTry) {
        for (size_t I = 0; I < List.size(); ++I) {
          const HighStmt &S = List[I];
          if (S.Addr != 0 && S.Addr != InvalidVA &&
              (I == 0 || List[I - 1].Addr != S.Addr))
            ++LabelStarts[S.Addr];
          Entered |=
              !InTry && S.Kind == StmtKind::Goto && Labels.count(S.GotoTarget);
          const bool Protected = InTry || &S == &TryHost[TryAt];
          Scan(S.Body, Protected);
          Scan(S.ElseBody, InTry);
          Scan(S.DefaultBody, InTry);
          for (const SwitchCase &Case : S.Cases)
            Scan(Case.Body, InTry);
          for (const std::vector<HighStmt> &ClauseBody : S.EHClauseBodies)
            Scan(ClauseBody, InTry);
        }
      };
  Scan(Body, false);
  if (Entered)
    return Restore();

  HighStmt &Try = TryHost[TryAt];
  if (Try.Body.empty() || !highStmtEndsItsBlock(Try.Body.back())) {
    // Falling off the try body continues after the try statement.
    if (TryAt + 1 >= TryHost.size())
      return Restore();
    const va_t Next = TryHost[TryAt + 1].Addr;
    if (Next == 0 || Next == InvalidVA || Next == Try.Addr ||
        LabelStarts[Next] != 1)
      return Restore();
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = Next;
    Try.Body.push_back(std::move(Jump));
  }
  Try.Body.insert(Try.Body.end(), std::make_move_iterator(Slice.begin()),
                  std::make_move_iterator(Slice.end()));
  return true;
}

/// The value an x86 filter thunk returns when that is all it computes:
/// `return E;`, or `t = E; return t;`.  E may read only memory, constants and
/// the frame, the one register the personality enters a filter with; each
/// frame register becomes the entry stack pointer plus its proven offset, the
/// way the function's own frame accesses are written.
ExprPtr x86FilterValue(const std::vector<HighStmt> &Body, const MedFunc &Med) {
  std::vector<const HighStmt *> Live;
  for (const HighStmt &S : Body)
    if (S.Kind != StmtKind::Nop &&
        !(S.Kind == StmtKind::Block && S.Body.empty()))
      Live.push_back(&S);
  if (Live.empty() || Live.size() > 2 ||
      Live.back()->Kind != StmtKind::Return || !Live.back()->RetVal)
    return nullptr;
  ExprPtr Value = Live.back()->RetVal;
  if (Live.size() == 2) {
    const HighStmt &Def = *Live.front();
    if (Def.Kind != StmtKind::Assign || !Def.Dst || !Def.Val ||
        Def.Dst->Kind != ExprKind::Var || Value->Kind != ExprKind::Var ||
        varKey(Def.Dst->Var) != varKey(Value->Var))
      return nullptr;
    Value = Def.Val;
  }
  size_t Budget = 256;
  std::map<const HighExpr *, ExprPtr> Rebuilt;
  std::function<ExprPtr(const ExprPtr &)> Rebuild =
      [&](const ExprPtr &E) -> ExprPtr {
    if (!E || !Budget--)
      return nullptr;
    if (auto It = Rebuilt.find(E.get()); It != Rebuilt.end())
      return It->second;
    ExprPtr Result;
    switch (E->Kind) {
    case ExprKind::Const:
      Result = E;
      break;
    case ExprKind::Var: {
      const TargetRegInfo &TRI = getTargetRegInfo(E->Var.TheArch);
      if (E->Var.Kind != MedVar::Reg || E->Var.Size != TRI.PointerSize ||
          (E->Var.RegOff != TRI.StackPointer &&
           E->Var.RegOff != TRI.FramePointer))
        return nullptr;
      const std::optional<int64_t> Offset =
          entryStackOffset(Med, E->Var, E->Var.TheArch, BinaryFormat::COFF);
      if (!Offset)
        return nullptr;
      MedVar EntrySP;
      EntrySP.Kind = MedVar::Reg;
      EntrySP.TheArch = E->Var.TheArch;
      EntrySP.RegOff = TRI.StackPointer;
      EntrySP.Size = TRI.PointerSize;
      Result = HighExpr::makeVar(EntrySP, E->Type);
      if (*Offset != 0) {
        const bool Down = *Offset < 0;
        Result = HighExpr::makeBinop(
            Down ? NdOp::INT_SUB : NdOp::INT_ADD, Result,
            HighExpr::makeConst(
                static_cast<uint64_t>(Down ? -*Offset : *Offset),
                TRI.PointerSize, ConstantAddressProvenance::Scalar));
        Result->Type = E->Type;
      }
      break;
    }
    case ExprKind::Load:
      if (E->MemoryOrdering != NdMemoryOrdering::None ||
          E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return nullptr;
      [[fallthrough]];
    case ExprKind::BinOp:
    case ExprKind::UnaryOp:
    case ExprKind::Cast:
    case ExprKind::BitCast: {
      auto Copy = std::make_shared<HighExpr>(*E);
      for (ExprPtr &Operand : Copy->Operands)
        if (!(Operand = Rebuild(Operand)))
          return nullptr;
      Result = Copy;
      break;
    }
    default:
      return nullptr;
    }
    Rebuilt.emplace(E.get(), Result);
    return Result;
  };
  return Rebuild(Value);
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

/// The native body at \p Target when it can leave the function body: a
/// checked registration callback CFG, or a single block entered only by the
/// exception dispatcher. It may not overlap a
/// guarded range, except that with \p TryRange (the Windows SEH try the block
/// becomes a clause body of) every SEH range must hold both the block and the
/// try, or neither: a clause body is guarded by the trys around its statement,
/// so the block keeps exactly the protection it had.
std::optional<AddressSet>
uniqueHandlerBlockRange(const MedFunc &Med, const ExceptionFunction &EH,
                        va_t Target,
                        const std::vector<RegionCandidate> &ProtectedRegions,
                        const ExceptionAddressRange *TryRange = nullptr) {
  if (Target == 0 || Target == InvalidVA)
    return std::nullopt;

  std::optional<RegistrationCallbackRegion> Callback;
  if (EH.Registration && EH.Cxx)
    Callback = registrationCallbackRegion(Med, Target);
  std::vector<ExceptionAddressRange> Ranges;
  if (Callback) {
    Ranges = std::move(Callback->Ranges);
  } else {
    if (EH.Registration && EH.Registration->hasCxxCallbackStack())
      return std::nullopt;
    const MedBlock *Match = nullptr;
    for (const MedBlock &Block : Med.Blocks) {
      if (Target < Block.StartAddr || Target >= Block.EndAddr)
        continue;
      if (Match && Match != &Block)
        return std::nullopt;
      Match = &Block;
    }
    // Normal flow may share the handler's code (RtlGuardIsValidStackPointer
    // falls into its `xor eax, eax`).  Moving that block into the __except
    // body would take it away from the ordinary path.
    if (!Match || !Match->Preds.empty())
      return std::nullopt;

    Ranges.push_back({Match->StartAddr, Match->EndAddr});
  }
  for (const auto &Range : Ranges) {
    if (!Range.isValid() || !EH.ownsCode(Range))
      return std::nullopt;
    for (const RegionCandidate &Candidate : ProtectedRegions) {
      const bool SameGuard = TryRange && Candidate.Kind == StmtKind::SEHTry &&
                             Candidate.Cover.empty();
      // The try's own range and its split parts hold its body, never a clause.
      const bool AroundTry = SameGuard && Candidate.Range.contains(*TryRange) &&
                             (Candidate.Range.Begin != TryRange->Begin ||
                              Candidate.Range.End != TryRange->End);
      const bool HasRegistrationParts =
          EH.Registration && Candidate.Kind == StmtKind::CxxTry &&
          Candidate.HasTryStates && !Candidate.Cover.empty();
      const bool Overlaps =
          !HasRegistrationParts
              ? Range.overlaps(Candidate.Range)
              : llvm::any_of(Candidate.Cover,
                             [&](const auto &P) { return Range.overlaps(P); });
      if (AroundTry ? !Candidate.Range.contains(Range) : Overlaps)
        return std::nullopt;
    }
  }
  if (!Callback)
    return AddressSet(Ranges.front());
  const ExceptionAddressRange Span{Ranges.front().Begin, Ranges.back().End};
  AddressSet Result(Span, std::move(Ranges));
  Result.SplitScope = true;
  return Result;
}

/// Whether \p S is a plain statement with no address.  Nothing jumps to it,
/// so it runs only after the statement before it.  Loop exits are not plain:
/// the list they would move into need not sit in the same loop.
bool isAddresslessPlainStmt(const HighStmt &S) {
  if (S.Addr != 0 && S.Addr != InvalidVA)
    return false;
  switch (S.Kind) {
  case StmtKind::Assign:
  case StmtKind::ExprStmt:
  case StmtKind::Store:
  case StmtKind::Call:
  case StmtKind::Nop:
  case StmtKind::Return:
  case StmtKind::Goto:
    return true;
  default:
    return false;
  }
}

/// The statement that runs once the statements of \p List end, \p List
/// being nested in \p Stmts and \p After running once \p Stmts end: the
/// statement after the if/else, block or __except-only try owning \p List, or
/// what runs after that statement's own list.  nullptr when nothing here is
/// known to run then, such as after a loop body, a case, a __finally or the
/// function.  std::nullopt when \p List is not nested in \p Stmts.
std::optional<const HighStmt *>
continuationOf(const std::vector<HighStmt> &Stmts,
               const std::vector<HighStmt> *List, const HighStmt *After) {
  if (&Stmts == List)
    return After;
  for (size_t I = 0; I < Stmts.size(); ++I) {
    const HighStmt &S = Stmts[I];
    const HighStmt *Next = I + 1 < Stmts.size() ? &Stmts[I + 1] : After;
    const bool Arms = S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
                      S.Kind == StmtKind::Block;
    const bool ExceptOnly =
        S.Kind == StmtKind::SEHTry && !S.EHClauses.empty() &&
        llvm::all_of(S.EHClauses, [](const HighEHClause &Clause) {
          return Clause.Kind == HighEHClauseKind::SEHExcept;
        });
    if (auto Found =
            continuationOf(S.Body, List, Arms || ExceptOnly ? Next : nullptr))
      return Found;
    if (auto Found = continuationOf(S.ElseBody, List, Arms ? Next : nullptr))
      return Found;
    for (const SwitchCase &Case : S.Cases)
      if (auto Found = continuationOf(Case.Body, List, nullptr))
        return Found;
    if (auto Found = continuationOf(S.DefaultBody, List, nullptr))
      return Found;
    for (size_t K = 0; K < S.EHClauseBodies.size(); ++K) {
      const bool Except = S.Kind == StmtKind::SEHTry &&
                          K < S.EHClauses.size() &&
                          S.EHClauses[K].Kind == HighEHClauseKind::SEHExcept;
      if (auto Found = continuationOf(S.EHClauseBodies[K], List,
                                      Except ? Next : nullptr))
        return Found;
    }
  }
  return std::nullopt;
}

/// How many statements of \p Stmts a jump to \p Addr could land on: those
/// beginning an address group, which C labels.  A first child sharing its
/// parent's address prints under the parent's label, so it is no start of
/// its own.  \p TargetStarts is set when \p Target is one of them.
unsigned statementsStarting(const std::vector<HighStmt> &Stmts, va_t Addr,
                            va_t Parent, const HighStmt *Target,
                            bool &TargetStarts) {
  unsigned Count = 0;
  for (size_t I = 0; I < Stmts.size(); ++I) {
    const HighStmt &S = Stmts[I];
    if (S.Addr == Addr && Addr != (I == 0 ? Parent : Stmts[I - 1].Addr)) {
      ++Count;
      TargetStarts |= &S == Target;
    }
    Count += statementsStarting(S.Body, Addr, S.Addr, Target, TargetStarts);
    Count += statementsStarting(S.ElseBody, Addr, 0, Target, TargetStarts);
    for (const SwitchCase &Case : S.Cases)
      Count += statementsStarting(Case.Body, Addr, 0, Target, TargetStarts);
    Count += statementsStarting(S.DefaultBody, Addr, 0, Target, TargetStarts);
    for (const std::vector<HighStmt> &ClauseBody : S.EHClauseBodies)
      Count += statementsStarting(ClauseBody, Addr, 0, Target, TargetStarts);
  }
  return Count;
}

} // anonymous namespace

void reportUnprotectedGuardedCode(const HighFunc &Func, const char *Stage) {
  if (!std::getenv("NEVERD_HIGH_FLOW_ORACLE"))
    return;
  std::vector<const HighStmt *> Trys;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::SEHTry && S.EHIsReducible && S.EHRange.isValid())
      Trys.push_back(&S);
  });
  for (const HighStmt *Try : Trys) {
    std::set<const HighStmt *> Inside;
    walkStmts(Try->Body, [&](const HighStmt &S) { Inside.insert(&S); });
    unsigned Outside = 0;
    va_t First = 0;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Addr && S.Addr != InvalidVA && Try->EHRange.contains(S.Addr) &&
          !Inside.count(&S) && highStmtMayFault(S)) {
        if (!Outside++)
          First = S.Addr;
      }
    });
    if (Outside)
      llvm::errs() << "FLOWPROTECT stage=" << Stage << " entry="
                   << llvm::utohexstr(Func.Entry, /*LowerCase=*/true)
                   << " try=[" << llvm::utohexstr(Try->EHRange.Begin, true)
                   << "," << llvm::utohexstr(Try->EHRange.End, true)
                   << ") outside=" << Outside
                   << " first=" << llvm::utohexstr(First, true) << "\n";
  }
}

void MedToHighConverter::structureExceptionRegions(HighFunc &Func,
                                                   const MedFunc &Med) {
  if (!Func.ExceptionMetadata)
    return;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;

  std::vector<RegionCandidate> Candidates;
  unsigned Rejected = 0;
  if (EH.ParseStatus == ExceptionParseStatus::Complete) {
    const RegistrationStateAnalysis *Registration =
        Med.RegistrationStates ? &*Med.RegistrationStates : nullptr;
    addSEHCandidates(EH, TargetArch, Candidates, Rejected);
    addRegistrationCandidates(EH, Registration, Candidates, Rejected);
    addCxxCandidates(EH, Med, Image, Registration, Candidates, Rejected);
    addCxxCleanupOnlyCandidates(EH, Registration, Candidates, Rejected);
    addItaniumCandidates(EH, Candidates, Rejected);
  } else {
    Rejected += EH.SEH ? static_cast<unsigned>(EH.SEH->Scopes.size()) : 0;
    Rejected += EH.Registration
                    ? static_cast<unsigned>(EH.Registration->Scopes.size())
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
                                            B.Range.Begin, B.Range.End, B.Kind);
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

  // SEH ranges with the same clauses are one __try split by the compiler.
  // The largest range anchors the try and the others join its body; a part
  // waits for its anchor and stands alone only if it cannot join.
  std::vector<std::optional<size_t>> SplitAnchor(Candidates.size());
  std::vector<std::vector<size_t>> SplitParts(Candidates.size());
  {
    using ClauseKey = std::tuple<HighEHClauseKind, va_t, va_t>;
    std::map<std::vector<ClauseKey>, std::vector<size_t>> Groups;
    for (size_t I = 0; I < Candidates.size(); ++I) {
      const RegionCandidate &Candidate = Candidates[I];
      if (Candidate.Kind != StmtKind::SEHTry || !Candidate.Cover.empty() ||
          Candidate.Clauses.empty())
        continue;
      std::vector<ClauseKey> Key;
      for (const HighEHClause &Clause : Candidate.Clauses)
        Key.emplace_back(Clause.Kind, Clause.FilterOrActionVA,
                         Clause.HandlerVA);
      Groups[std::move(Key)].push_back(I);
    }
    for (const auto &[Key, Members] : Groups) {
      if (Members.size() < 2)
        continue;
      bool Disjoint = true;
      for (size_t A = 0; A < Members.size(); ++A)
        for (size_t B = A + 1; B < Members.size(); ++B)
          Disjoint &= !Candidates[Members[A]].Range.overlaps(
              Candidates[Members[B]].Range);
      if (!Disjoint)
        continue;
      const size_t Anchor = *std::max_element(
          Members.begin(), Members.end(), [&](size_t A, size_t B) {
            const ExceptionAddressRange &L = Candidates[A].Range;
            const ExceptionAddressRange &R = Candidates[B].Range;
            return L.size() != R.size() ? L.size() < R.size()
                                        : L.Begin > R.Begin;
          });
      for (size_t Member : Members)
        if (Member != Anchor) {
          SplitAnchor[Member] = Anchor;
          SplitParts[Anchor].push_back(Member);
        }
    }
  }
  std::vector<size_t> Order(Candidates.size());
  std::iota(Order.begin(), Order.end(), size_t{0});
  std::vector<bool> Processed(Candidates.size(), false);
  std::vector<bool> Absorbed(Candidates.size(), false);
  size_t SplitCopyBudget = limits::kMaxRegistrationEHStateWork;
  for (size_t K = 0; K < Order.size(); ++K) {
    const size_t I = Order[K];
    if (Absorbed[I])
      continue;
    if (SplitAnchor[I] && !Processed[*SplitAnchor[I]]) {
      Order.push_back(I);
      continue;
    }
    Processed[I] = true;
    RegionCandidate &Candidate = Candidates[I];
    if (hasCrossingRegions(Candidates, I)) {
      Rejected += Candidate.NativeRegionCount;
      continue;
    }
    // A PE32 try can surround its out-of-line catch in address order. Its
    // checked state intervals form one lexical body only after that callback
    // is moved into its clause. Work transactionally: an ambiguous handler,
    // an ordinary predecessor or an intervening unprotected statement must
    // leave the original listing intact.
    const bool SplitRegistrationCxx =
        EH.Registration && Candidate.Kind == StmtKind::CxxTry &&
        Candidate.HasTryStates && !Candidate.Cover.empty();
    const bool SeparateRegistrationCxx =
        SplitRegistrationCxx ||
        (EH.Registration && EH.Registration->hasCxxCallbackStack() &&
         Candidate.Kind == StmtKind::CxxTry);
    std::optional<std::vector<HighStmt>> OriginalBody;
    bool RegionInstalled = false;
    auto RestoreBody = llvm::scope_exit([&] {
      if (OriginalBody && !RegionInstalled)
        Func.Body = std::move(*OriginalBody);
    });
    std::vector<std::vector<HighStmt>> SeparatedBodies(
        Candidate.Clauses.size());
    if (SeparateRegistrationCxx) {
      bool Exhausted = SplitCopyBudget == 0;
      if (!Exhausted)
        walkStmts(Func.Body, [&](const HighStmt &) {
          if (SplitCopyBudget)
            --SplitCopyBudget;
          else
            Exhausted = true;
        });
      if (Exhausted) {
        Rejected += Candidate.NativeRegionCount;
        continue;
      }
      OriginalBody = Func.Body;
      bool Complete = true;
      for (size_t C = 0; C < Candidate.Clauses.size(); ++C) {
        const auto &Clause = Candidate.Clauses[C];
        if (Clause.Kind != HighEHClauseKind::CxxCatch)
          continue;
        const auto Range =
            uniqueHandlerBlockRange(Med, EH, Clause.HandlerVA, Candidates);
        size_t At = 0;
        if (WindowsTargetUses[Clause.HandlerVA] != 1 || !Range ||
            !extractAddressSlice(Func.Body, *Range, EH.CodeRange,
                                 SeparatedBodies[C], At,
                                 /*IncludeFunctionEdgeUnknown=*/false) ||
            SeparatedBodies[C].empty() ||
            !highStmtEndsItsBlock(SeparatedBodies[C].back())) {
          Complete = false;
          break;
        }
        // A closed native CFG is not enough if earlier structuring scattered
        // its statements into several lists. Move the complete callback from
        // its actual entry, or restore the original function transactionally.
        if (EH.Registration->hasCxxCallbackStack()) {
          bool Left = SeparatedBodies[C].front().Addr != Clause.HandlerVA;
          walkStmts(Func.Body, [&](const HighStmt &Stmt) {
            Left |= Stmt.Addr != 0 && Stmt.Addr != InvalidVA &&
                    Range->contains(Stmt.Addr);
          });
          if (Left) {
            Complete = false;
            break;
          }
        }
      }
      if (!Complete) {
        Rejected += Candidate.NativeRegionCount;
        continue;
      }
    }
    std::vector<HighStmt> ProtectedBody;
    size_t InsertAt = 0;
    std::vector<HighStmt> *Host = nullptr;
    // Split parts the anchor's statements contain come along with them,
    // such as a part that is the arm of a branch in the anchor range.
    std::vector<size_t> NestedParts;
    AddressSet Union(Candidate.Range);
    Union.SplitScope = true;
    for (size_t Part : SplitParts[I])
      if (!Processed[Part]) {
        Union.Parts.push_back(Candidates[Part].Range);
        NestedParts.push_back(Part);
      }
    if (!NestedParts.empty()) {
      Union.Parts.push_back(Candidate.Range);
      if (extractAddressSlice(Func.Body, Union, EH.CodeRange, ProtectedBody,
                              InsertAt, /*IncludeFunctionEdgeUnknown=*/true,
                              &Host) &&
          Host) {
        bool Stray = false;
        walkStmts(Func.Body, [&](const HighStmt &S) {
          for (size_t Part : NestedParts)
            Stray |= S.Addr != 0 && Candidates[Part].Range.contains(S.Addr);
        });
        if (Stray) {
          Host->insert(Host->begin() + static_cast<ptrdiff_t>(InsertAt),
                       std::make_move_iterator(ProtectedBody.begin()),
                       std::make_move_iterator(ProtectedBody.end()));
          ProtectedBody.clear();
          Host = nullptr;
        }
      } else {
        Host = nullptr;
      }
      if (!Host)
        NestedParts.clear();
    }
    AddressSet ProtectedAddresses{Candidate.Range, Candidate.Cover};
    ProtectedAddresses.SplitScope = SplitRegistrationCxx;
    const bool TerminalRegistration =
        !Host && SplitRegistrationCxx &&
        extractTerminalRegistrationTry(Func, Med, Candidate.TryLow,
                                       Candidate.TryHigh, ProtectedBody,
                                       InsertAt);
    if (TerminalRegistration)
      Host = &Func.Body;
    if ((!Host &&
         !extractAddressSlice(Func.Body, ProtectedAddresses, EH.CodeRange,
                              ProtectedBody, InsertAt,
                              /*IncludeFunctionEdgeUnknown=*/true, &Host)) ||
        !Host) {
      Rejected += Candidate.NativeRegionCount;
      continue;
    }
    // A try joined from split parts may start with a part's statements: its
    // entry, and the label it carries, is its first statement's address.
    va_t Entry = TerminalRegistration ? Med.Entry : Candidate.Range.Begin;
    if (!NestedParts.empty())
      for (const HighStmt &S : ProtectedBody)
        if (S.Addr && S.Addr != InvalidVA) {
          Entry = S.Addr;
          break;
        }
    // A jump from outside into the protected statements has no C spelling;
    // keep that range unstructured instead of emitting one.  So is a guarded
    // statement left outside the slice that may raise: in C it would run
    // unprotected.
    bool LeftOut = Candidate.Kind == StmtKind::SEHTry &&
                   guardedStatementLeftOut(Func.Body, Candidate.Range);
    if (SplitRegistrationCxx)
      for (const auto &Part : Candidate.Cover)
        LeftOut |= guardedStatementLeftOut(Func.Body, Part);
    for (size_t Part : NestedParts)
      LeftOut |= Candidate.Kind == StmtKind::SEHTry &&
                 guardedStatementLeftOut(Func.Body, Candidates[Part].Range);
    bool Entered = jumpEntersSlice(Func.Body, ProtectedBody, Entry);
    for (const auto &Body : SeparatedBodies)
      Entered |= jumpEntersSlice(Body, ProtectedBody, Entry);
    if (Entered || LeftOut) {
      Host->insert(Host->begin() + static_cast<ptrdiff_t>(InsertAt),
                   std::make_move_iterator(ProtectedBody.begin()),
                   std::make_move_iterator(ProtectedBody.end()));
      Rejected += Candidate.NativeRegionCount;
      continue;
    }

    HighStmt Try;
    Try.Kind = Candidate.Kind;
    Try.Addr = Entry;
    Try.Body = std::move(ProtectedBody);
    Try.EHRange = Candidate.Range;
    std::vector<std::optional<va_t>> ClauseTargets;
    ClauseTargets.reserve(Candidate.Clauses.size());
    for (const HighEHClause &Clause : Candidate.Clauses)
      ClauseTargets.push_back(windowsClauseBodyTarget(Clause));
    Try.EHClauses = std::move(Candidate.Clauses);
    Try.EHClauseBodies.resize(Try.EHClauses.size());
    Try.EHIsReducible = true;
    Host->insert(Host->begin() + static_cast<ptrdiff_t>(InsertAt),
                 std::move(Try));

    // The parts that join this try no longer use its clauses on their own.
    unsigned JoinedParts = 0;
    for (size_t Part : NestedParts) {
      Processed[Part] = Absorbed[Part] = true;
      Candidate.NativeRegionCount += Candidates[Part].NativeRegionCount;
      ++JoinedParts;
    }
    for (size_t Part : SplitParts[I]) {
      if (Processed[Part] ||
          !absorbSplitTryPart(Func.Body, Candidate.Kind, Candidate.Range,
                              Candidates[Part].Range, EH.CodeRange))
        continue;
      Processed[Part] = Absorbed[Part] = true;
      Candidate.NativeRegionCount += Candidates[Part].NativeRegionCount;
      ++JoinedParts;
    }
    if (JoinedParts != NestedParts.size()) {
      const auto TrySite =
          findTryStmt(Func.Body, Candidate.Kind, Candidate.Range);
      if (!TrySite)
        llvm::report_fatal_error("structured try vanished while joining "
                                 "its split ranges");
      Host = TrySite->first;
    }

    auto FindInsertedTry = [&]() -> HighStmt * {
      for (HighStmt &Stmt : *Host)
        if (Stmt.Kind == Candidate.Kind &&
            Stmt.EHRange.Begin == Candidate.Range.Begin &&
            Stmt.EHRange.End == Candidate.Range.End)
          return &Stmt;
      return nullptr;
    };
    // A clause that runs off its end continues after the try statement,
    // which need not be where its handler block ran on to.  What runs next
    // is the following statement, past empty anchors, or the target of the
    // jump that statement is.
    auto AfterInsertedTry = [&]() {
      std::set<va_t> AfterTry;
      const HighStmt *InsertedTry = FindInsertedTry();
      for (size_t K = 0; InsertedTry && K < Host->size(); ++K) {
        if (&(*Host)[K] != InsertedTry)
          continue;
        for (size_t M = K + 1; M < Host->size(); ++M) {
          const HighStmt &Next = (*Host)[M];
          AfterTry.insert(Next.Addr);
          if (Next.Kind == StmtKind::Goto)
            AfterTry.insert(Next.GotoTarget);
          const bool EmptyAnchor =
              Next.Kind == StmtKind::Nop ||
              (Next.Kind == StmtKind::Block && Next.Body.empty());
          if (!EmptyAnchor)
            break;
        }
        break;
      }
      AfterTry.erase(0);
      AfterTry.erase(InvalidVA);
      return AfterTry;
    };

    // The statements a clause running off its end reaches before anything
    // runs: the empty anchors after the try statement and the statement
    // after them.
    auto FollowInsertedTry = [&]() {
      std::vector<const HighStmt *> Follow;
      const HighStmt *InsertedTry = FindInsertedTry();
      for (size_t K = 0; InsertedTry && K < Host->size(); ++K) {
        if (&(*Host)[K] != InsertedTry)
          continue;
        for (size_t M = K + 1; M < Host->size(); ++M) {
          const HighStmt &Next = (*Host)[M];
          Follow.push_back(&Next);
          if (Next.Kind != StmtKind::Nop &&
              (Next.Kind != StmtKind::Block || !Next.Body.empty()))
            break;
        }
        break;
      }
      return Follow;
    };

    auto ClauseBodies = std::move(SeparatedBodies);
    // Where a handler block that runs off its end continued: the statement
    // after it.  As a clause body it continues after the try statement.
    std::vector<std::optional<va_t>> ClauseFallTo(ClauseTargets.size());
    for (size_t ClauseIndex = 0; ClauseIndex < ClauseTargets.size();
         ++ClauseIndex) {
      if (!ClauseBodies[ClauseIndex].empty())
        continue;
      std::optional<va_t> Target = ClauseTargets[ClauseIndex];
      if (!Target || WindowsTargetUses[*Target] - JoinedParts != 1)
        continue;
      const auto HandlerRange = uniqueHandlerBlockRange(
          Med, EH, *Target, Candidates,
          Candidate.Kind == StmtKind::SEHTry ? &Candidate.Range : nullptr);
      if (!HandlerRange)
        continue;
      size_t HandlerAt = 0;
      std::vector<HighStmt> HandlerBody;
      std::vector<HighStmt> *HandlerHost = nullptr;
      if (!extractAddressSlice(
              Func.Body, *HandlerRange, EH.CodeRange, HandlerBody, HandlerAt,
              /*IncludeFunctionEdgeUnknown=*/false, &HandlerHost))
        continue;
      if (!HandlerBody.empty() && !highStmtEndsItsBlock(HandlerBody.back())) {
        if (!HandlerHost)
          llvm::report_fatal_error("handler slice extracted without its list");
        // The handler block ran on.  Statements without an address that
        // follow it run only after it, so they come along until one leaves;
        // past them is the statement it ran into or, at the end of its list,
        // what runs after that list.
        size_t RunEnd = HandlerAt;
        bool Leaves = false;
        while (!Leaves && RunEnd < HandlerHost->size() &&
               isAddresslessPlainStmt((*HandlerHost)[RunEnd]))
          Leaves = highStmtEndsItsBlock((*HandlerHost)[RunEnd++]);
        const bool InList = !Leaves && RunEnd < HandlerHost->size();
        const HighStmt *Cont = nullptr;
        if (!Leaves && !InList)
          Cont =
              continuationOf(Func.Body, HandlerHost, nullptr).value_or(nullptr);
        if (!Leaves && !InList && !Cont) {
          // Without the statement it ran into, the clause could not keep
          // that path: leave the handler where it is, entered by a jump.
          HandlerHost->insert(HandlerHost->begin() +
                                  static_cast<ptrdiff_t>(HandlerAt),
                              std::make_move_iterator(HandlerBody.begin()),
                              std::make_move_iterator(HandlerBody.end()));
          continue;
        }
        HandlerBody.insert(
            HandlerBody.end(),
            std::make_move_iterator(HandlerHost->begin() +
                                    static_cast<ptrdiff_t>(HandlerAt)),
            std::make_move_iterator(HandlerHost->begin() +
                                    static_cast<ptrdiff_t>(RunEnd)));
        HandlerHost->erase(
            HandlerHost->begin() + static_cast<ptrdiff_t>(HandlerAt),
            HandlerHost->begin() + static_cast<ptrdiff_t>(RunEnd));
        if (InList)
          Cont = &(*HandlerHost)[HandlerAt];
        if (!Leaves) {
          // A clause running on to the statement after the try reaches it
          // with no jump.  Any other continuation needs one, which must land
          // on that statement alone once the handler code is in the clause.
          const std::vector<const HighStmt *> Follow = FollowInsertedTry();
          const bool FallsThrough =
              ClauseTargets.size() == 1 && llvm::is_contained(Follow, Cont);
          const va_t Next = Cont->Addr;
          bool ContStarts = false;
          unsigned Starts = 0;
          if (Next != 0 && Next != InvalidVA) {
            Starts =
                statementsStarting(Func.Body, Next, 0, Cont, ContStarts) +
                statementsStarting(HandlerBody, Next, 0, nullptr, ContStarts);
            for (const std::vector<HighStmt> &Filled : ClauseBodies)
              Starts +=
                  statementsStarting(Filled, Next, 0, nullptr, ContStarts);
          }
          if (!FallsThrough && (Starts != 1 || !ContStarts)) {
            HandlerHost->insert(HandlerHost->begin() +
                                    static_cast<ptrdiff_t>(HandlerAt),
                                std::make_move_iterator(HandlerBody.begin()),
                                std::make_move_iterator(HandlerBody.end()));
            continue;
          }
          if (!FallsThrough)
            ClauseFallTo[ClauseIndex] = Next;
        }
      }
      ClauseBodies[ClauseIndex] = std::move(HandlerBody);
    }

    if (HighStmt *InsertedTry = FindInsertedTry()) {
      const std::set<va_t> AfterTry = AfterInsertedTry();
      InsertedTry->EHClauseBodies = std::move(ClauseBodies);
      for (size_t C = 0; C < ClauseFallTo.size(); ++C) {
        if (!ClauseFallTo[C] || AfterTry.count(*ClauseFallTo[C]))
          continue;
        HighStmt Jump;
        Jump.Kind = StmtKind::Goto;
        Jump.GotoTarget = *ClauseFallTo[C];
        InsertedTry->EHClauseBodies[C].push_back(std::move(Jump));
      }
    }

    // Filter thunks live in the same function as x86 registration EH but are
    // called only by the personality.  Drop them from the C body; the except
    // header names the filter, or prints the value it returns.
    if (HighStmt *InsertedTry = FindInsertedTry()) {
      for (HighEHClause &Clause : InsertedTry->EHClauses) {
        if (Clause.Kind != HighEHClauseKind::SEHExcept ||
            Clause.FilterOrActionVA == 0 ||
            Clause.FilterOrActionVA == Clause.HandlerVA)
          continue;
        const auto FilterRange = uniqueHandlerBlockRange(
            Med, EH, Clause.FilterOrActionVA, Candidates);
        if (!FilterRange)
          continue;
        size_t FilterAt = 0;
        std::vector<HighStmt> FilterBody;
        if (extractAddressSlice(Func.Body, *FilterRange, EH.CodeRange,
                                FilterBody, FilterAt,
                                /*IncludeFunctionEdgeUnknown=*/false) &&
            EH.Registration)
          Clause.FilterValue = x86FilterValue(FilterBody, Med);
      }
    }

    Func.StructuredExceptionRegions += Candidate.NativeRegionCount;
    RegionInstalled = true;
  }
  Func.UnstructuredExceptionRegions += Rejected;

  // x86 registration and VC6 C++ often have no IP map.  Still surface a
  // readable try around the recovered body rather than leaving a flat listing.
  // Cleanup-only C++ (UnwindMap, no TryBlocks) is a destructor scope, not SEH.
  if (Func.StructuredExceptionRegions == 0 && !Func.Body.empty() &&
      (EH.SEH || EH.Cxx || EH.Registration)) {
    HighStmt Try;
    Try.Kind = EH.Cxx ? StmtKind::CxxTry : StmtKind::SEHTry;
    Try.EHRange = EH.CodeRange;
    Try.EHIsReducible = false;
    Try.Body = std::move(Func.Body);
    if (Try.Kind == StmtKind::CxxTry) {
      const auto *Registration =
          Med.RegistrationStates ? &*Med.RegistrationStates : nullptr;
      for (uint32_t TryIndex = 0; TryIndex < EH.Cxx->TryBlocks.size();
           ++TryIndex) {
        const auto &Block = EH.Cxx->TryBlocks[TryIndex];
        for (uint32_t CatchIndex = 0; CatchIndex < Block.Handlers.size();
             ++CatchIndex) {
          Try.EHClauses.push_back(makeCxxCatchClause(Block.Handlers[CatchIndex],
                                                     Image, Registration,
                                                     TryIndex, CatchIndex));
          Try.EHClauseBodies.emplace_back();
        }
      }
      if (Try.EHClauses.empty()) {
        for (int32_t State = 0;
             State < static_cast<int32_t>(EH.Cxx->UnwindMap.size()); ++State) {
          const CxxUnwindAction &Action = EH.Cxx->UnwindMap[State];
          if (Action.ActionVA == 0)
            continue;
          HighEHClause Clause;
          Clause.Kind = HighEHClauseKind::CxxCleanup;
          Clause.FilterOrActionVA = Action.ActionVA;
          Clause.State = State;
          Clause.UnwindActionKind = Action.Kind;
          Clause.UnwindObjectOffset = Action.ObjectOffset;
          Try.EHClauses.push_back(std::move(Clause));
          Try.EHClauseBodies.emplace_back();
        }
      }
    } else if (EH.Registration) {
      for (const RegistrationScopeRecord &Scope : EH.Registration->Scopes) {
        HighEHClause Clause;
        Clause.Kind = Scope.IsFinally ? HighEHClauseKind::SEHFinally
                                      : HighEHClauseKind::SEHExcept;
        Clause.FilterOrActionVA =
            Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA;
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
  reportUnprotectedGuardedCode(Func, "after-exceptions");
  // Invert-skip of `if (c) goto L; work; L:` inside a try body is blocked
  // before wrapping: Med blocks in the try have ExceptionalPreds. After the
  // handler is a clause, the try list is a closed HighIR run.
  invertSkipGotos(Func);
}

} // namespace neverd
