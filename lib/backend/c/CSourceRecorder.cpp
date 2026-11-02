//===- CSourceRecorder.cpp - Verify source spans without editing C --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CSourceRecorder.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace neverd {

CSourceRecorder::CSourceRecorder(CSourceMap &Map, llvm::StringRef Ordinary,
                                 Recording Mode)
    : Map(Map), Mode(Mode) {
  if ((Mode == Recording::DefinitionsAndInstructions || recordsPositions()) &&
      Map.Recognitions && !Map.Recognitions->empty())
    throw std::invalid_argument(
        "library coverage needs an independent source recording");
  if (recordsPositions() && Map.HighSources &&
      llvm::any_of(*Map.HighSources, [](const auto &Source) {
        return Source.Kind == HighSourceKind::CallExpression;
      }))
    throw std::invalid_argument(
        "call expression coverage needs an independent source recording");
  Map.Anchors.clear();
  if (Mode != Recording::Instructions) {
    Map.Regions.clear();
    Map.Definitions.clear();
    if (Map.Recognitions)
      for (size_t I = 0; I < Map.Recognitions->size(); ++I)
        Map.Regions.push_back({I, {}, false});
  }
  if (recordsPositions())
    return;
  for (size_t Salt = 0;; ++Salt) {
    Prefix = "\x1eND:" + std::to_string(Salt) + ':';
    if (!Ordinary.contains(Prefix))
      break;
  }
}

void CSourceRecorder::prepareHighSources() {
  if (!Map.HighSources)
    return;
  if (recordsInstructions())
    HighStatements.emplace(*Map.HighSources);
  using Key = std::pair<va_t, const HighExpr *>;
  std::map<Key, std::set<sigs::LibraryOccurrence>> Origins;
  std::map<Key, std::shared_ptr<const HighExpr>> Alive;
  std::map<std::pair<va_t, va_t>, std::set<sigs::LibraryOccurrence>> Stores;
  std::set<std::pair<va_t, va_t>> AmbiguousStores;
  std::set<Key> Ambiguous;
  size_t Work = ::neverd::limits::backend::
      kCSourceRecorderCSourceRecorderPrepareHighSourcesWork;
  for (const auto &Source : *Map.HighSources) {
    if (Source.Kind == HighSourceKind::Store) {
      Stores[{Source.Function, Source.Occurrence.Address}].insert(
          Source.Occurrence);
      continue;
    }
    if (Source.Kind == HighSourceKind::StatementExpression)
      continue;
    if (auto Expr = Source.Expression.lock()) {
      Key K{Source.Function, Expr.get()};
      if (recordsInstructions() &&
          Source.Kind == HighSourceKind::CallExpression &&
          Expr->Kind == ExprKind::Call)
        HighCallAnchors[K].insert(Source.Occurrence);
      Origins[K].insert(Source.Occurrence);
      Alive[K] = std::move(Expr);
    }
  }
  if (recordsInstructions() || !Map.Recognitions)
    return;
  for (size_t I = 0; I < Map.Recognitions->size(); ++I) {
    const auto &Match = (*Map.Recognitions)[I];
    if (!Match.Isolated ||
        Match.Scope == sigs::LibraryFeatureScope::WholeFunction)
      continue;
    for (const auto &[K, Origins] : Stores) {
      if (K.first != Match.Function || Origins.size() != 1 ||
          !std::binary_search(Match.Occurrences.begin(),
                              Match.Occurrences.end(), *Origins.begin()))
        continue;
      const auto [At, Added] =
          HighStores.emplace(K, Event{I, {*Origins.begin()}});
      if (!Added && At->second.Region != I)
        AmbiguousStores.insert(K);
    }
    std::map<Key, Event> Candidates;
    std::set<Key> Children;
    for (const auto &[K, Expr] : Alive) {
      if (K.first != Match.Function ||
          !llvm::any_of(Origins[K], [&](const auto &O) {
            return std::binary_search(Match.Occurrences.begin(),
                                      Match.Occurrences.end(), O);
          }))
        continue;
      std::set<sigs::LibraryOccurrence> Covered;
      std::set<const HighExpr *> Seen;
      std::vector<const HighExpr *> Pending{Expr.get()};
      size_t Budget = ::neverd::limits::backend::
          kCSourceRecorderCSourceRecorderPrepareHighSourcesBudget;
      while (!Pending.empty() && Budget) {
        if (!Work) {
          HighRegions.clear();
          HighStores.clear();
          return;
        }
        --Work;
        --Budget;
        const auto *Current = Pending.back();
        Pending.pop_back();
        if (!Seen.insert(Current).second)
          continue;
        if (Current != Expr.get())
          Children.insert({K.first, Current});
        if (auto At = Origins.find({K.first, Current}); At != Origins.end())
          Covered.insert(At->second.begin(), At->second.end());
        Current->forEachChildExpr(
            [&](const auto &Child) { Pending.push_back(Child.get()); });
      }
      if (!Pending.empty())
        continue;
      std::vector<sigs::LibraryOccurrence> Coverage;
      std::set_intersection(Covered.begin(), Covered.end(),
                            Match.Occurrences.begin(), Match.Occurrences.end(),
                            std::back_inserter(Coverage));
      if (!Coverage.empty())
        Candidates.emplace(K, Event{I, std::move(Coverage)});
    }
    for (auto &[K, E] : Candidates)
      if (!Children.contains(K)) {
        const auto [At, Added] = HighRegions.emplace(K, std::move(E));
        if (!Added && At->second.Region != I)
          Ambiguous.insert(K);
      }
  }
  for (const auto &K : Ambiguous)
    HighRegions.erase(K);
  for (const auto &K : AmbiguousStores)
    HighStores.erase(K);
}

void CSourceRecorder::prepareLLVMSources(const LLVMSourceMap &Sources) {
  for (const auto &[Entry, Handle] : Sources.Functions)
    if (auto *Function = llvm::dyn_cast_or_null<llvm::Function>(Handle))
      LLVMFunctions.emplace(Function, Entry);
  std::set<const llvm::Value *> Ambiguous;
  size_t Work = ::neverd::limits::backend::
      kCSourceRecorderCSourceRecorderPrepareLLVMSourcesWork;
  for (const auto &Observation : Sources.Observations) {
    auto *Instruction =
        llvm::dyn_cast_or_null<llvm::Instruction>(Observation.Value);
    if (!Instruction)
      continue;
    auto Function = LLVMFunctions.find(Instruction->getFunction());
    if (Function == LLVMFunctions.end() ||
        Function->second != Observation.Function)
      continue;
    if (recordsInstructions()) {
      auto &Anchor = LLVMAnchors[Instruction];
      Anchor.first = Observation.Function;
      Anchor.second.insert(Observation.Occurrence);
      continue;
    }
    if (!Map.Recognitions)
      continue;
    for (size_t I = 0; I < Map.Recognitions->size(); ++I) {
      if (!Work) {
        LLVMRegions.clear();
        return;
      }
      --Work;
      const auto &Match = (*Map.Recognitions)[I];
      if (Match.Function != Observation.Function || !Match.Isolated ||
          Match.Scope == sigs::LibraryFeatureScope::WholeFunction ||
          !std::binary_search(Match.Occurrences.begin(),
                              Match.Occurrences.end(), Observation.Occurrence))
        continue;
      auto [At, Added] = LLVMRegions.try_emplace(Instruction, Event{I, {}});
      if (At->second.Region != I)
        Ambiguous.insert(Instruction);
      else
        At->second.Coverage.push_back(Observation.Occurrence);
    }
  }
  for (const auto *Value : Ambiguous)
    LLVMRegions.erase(Value);
  // Prefer the outer event when it already carries every origin of a pure
  // child. Nested delimiters must not interfere with the emitter's cast and
  // condition spelling; memory operations remain independent boundaries.
  for (auto &[Value, E] : LLVMRegions) {
    (void)Value;
    std::sort(E.Coverage.begin(), E.Coverage.end());
    E.Coverage.erase(std::unique(E.Coverage.begin(), E.Coverage.end()),
                     E.Coverage.end());
  }
  std::set<const llvm::Value *> Nested;
  for (const auto &[Value, Parent] : LLVMRegions) {
    const auto *I = llvm::dyn_cast<llvm::Instruction>(Value);
    if (!I)
      continue;
    std::vector<const llvm::Instruction *> Pending;
    for (const auto &Operand : I->operands())
      if (auto *Child = llvm::dyn_cast<llvm::Instruction>(Operand))
        Pending.push_back(Child);
    std::set<const llvm::Instruction *> Seen;
    while (!Pending.empty()) {
      if (!Work) {
        LLVMRegions.clear();
        return;
      }
      --Work;
      const auto *Child = Pending.back();
      Pending.pop_back();
      if (!Seen.insert(Child).second || Child->getParent() != I->getParent() ||
          (!Child->isBinaryOp() && !Child->isCast() &&
           !llvm::isa<llvm::CmpInst, llvm::SelectInst, llvm::GetElementPtrInst>(
               Child)))
        continue;
      if (auto At = LLVMRegions.find(Child);
          At != LLVMRegions.end() && At->second.Region == Parent.Region &&
          std::includes(Parent.Coverage.begin(), Parent.Coverage.end(),
                        At->second.Coverage.begin(), At->second.Coverage.end()))
        Nested.insert(Child);
      for (const auto &Operand : Child->operands())
        if (auto *Next = llvm::dyn_cast<llvm::Instruction>(Operand))
          Pending.push_back(Next);
    }
  }
  for (const auto *Value : Nested)
    LLVMRegions.erase(Value);
}

size_t CSourceRecorder::event(std::optional<size_t> Region,
                              std::vector<sigs::LibraryOccurrence> Coverage,
                              std::optional<va_t> Function) {
  Events.push_back({Region, std::move(Coverage), Function});
  return Events.size() - 1;
}

std::string CSourceRecorder::begin(size_t Event) const {
  return Prefix + std::to_string(Event) + "B\x1f";
}

std::string CSourceRecorder::end(size_t Event) const {
  return Prefix + std::to_string(Event) + "E\x1f";
}

llvm::StringRef CSourceRecorder::plain(llvm::StringRef Text,
                                       std::string &Storage) const {
  if (recordsPositions())
    return Text;
  size_t At = Text.find(Prefix);
  if (At == llvm::StringRef::npos)
    return Text;
  Storage.clear();
  for (; At != llvm::StringRef::npos; At = Text.find(Prefix)) {
    Storage.append(Text.data(), At);
    // A delimiter is the prefix, an event and its kind, and a unit separator.
    Text = Text.drop_front(At + Prefix.size()).split('\x1f').second;
  }
  Storage.append(Text.data(), Text.size());
  return Storage;
}

std::string CSourceRecorder::definition(std::optional<va_t> Entry) {
  if (Mode == Recording::Instructions)
    return {};
  DefinitionEntries.push_back(Entry);
  return Prefix + std::to_string(DefinitionEntries.size() - 1) + "D\x1f";
}

std::string CSourceRecorder::definition(const llvm::Function &Function) {
  auto I = LLVMFunctions.find(&Function);
  return definition(I == LLVMFunctions.end() ? std::nullopt
                                             : std::optional(I->second));
}

std::string CSourceRecorder::expression(va_t Function, const HighExpr &Expr,
                                        std::string Text) {
  if (recordsInstructions()) {
    auto At = HighCallAnchors.find({Function, &Expr});
    if (Expr.Kind == ExprKind::Call && At != HighCallAnchors.end() &&
        At->second.size() == 1) {
      auto ID = event(std::nullopt, {*At->second.begin()}, Function);
      return begin(ID) + Text + end(ID);
    }
    return Text;
  }
  auto It = HighRegions.find({Function, &Expr});
  if (It == HighRegions.end())
    return Text;
  size_t Event = event(It->second.Region, It->second.Coverage);
  return begin(Event) + Text + end(Event);
}

std::optional<size_t> CSourceRecorder::function(va_t Entry) {
  if (recordsInstructions())
    return std::nullopt;
  if (Map.Recognitions)
    for (size_t I = 0; I < Map.Recognitions->size(); ++I) {
      const auto &Match = (*Map.Recognitions)[I];
      if (Match.Function == Entry && Match.Isolated &&
          Match.Scope == sigs::LibraryFeatureScope::WholeFunction)
        return event(I, Match.Occurrences);
    }
  return std::nullopt;
}

std::optional<size_t>
CSourceRecorder::function(const llvm::Function &Function) {
  auto I = LLVMFunctions.find(&Function);
  return I == LLVMFunctions.end() ? std::nullopt : function(I->second);
}

std::optional<size_t>
CSourceRecorder::instruction(const llvm::Instruction &Value) {
  auto I = LLVMRegions.find(&Value);
  auto Anchor = LLVMAnchors.find(&Value);
  if (recordsInstructions() && Anchor != LLVMAnchors.end())
    return event(std::nullopt,
                 {Anchor->second.second.begin(), Anchor->second.second.end()},
                 Anchor->second.first);
  return I == LLVMRegions.end() ? std::nullopt
                                : std::optional<size_t>(event(
                                      I->second.Region, I->second.Coverage));
}

std::optional<size_t> CSourceRecorder::statement(va_t Function,
                                                 const HighStmt &Stmt) {
  const auto At = HighStores.find({Function, Stmt.Addr});
  if (recordsInstructions() && HighStatements)
    if (const auto Origin = HighStatements->lookup(Function, Stmt))
      return event(std::nullopt, {*Origin}, Function);
  if (Stmt.Kind != StmtKind::Store)
    return std::nullopt;
  return At == HighStores.end()
             ? std::nullopt
             : std::optional(event(At->second.Region, At->second.Coverage));
}

std::string CSourceRecorder::expression(const llvm::Instruction &Value,
                                        std::string Text) {
  if (recordsInstructions() &&
      !(Map.RecordCallExpressions && llvm::isa<llvm::CallBase>(Value)))
    return Text;
  auto Event = instruction(Value);
  return Event ? begin(*Event) + Text + end(*Event) : Text;
}

bool CSourceRecorder::finish(llvm::StringRef Annotated,
                             llvm::StringRef Ordinary) {
  if (recordsPositions())
    return false;
  std::vector<CSourceRegion> Candidate =
      recordsInstructions() ? std::vector<CSourceRegion>() : Map.Regions;
  std::vector<std::set<sigs::LibraryOccurrence>> Covered(Candidate.size());
  std::string Clean;
  struct Frame {
    size_t Event;
    size_t Begin;
  };
  std::vector<Frame> Stack;
  std::vector<CSourceDefinition> Definitions;
  std::vector<CSourceAnchor> Anchors;
  while (!Annotated.empty()) {
    size_t At = Annotated.find(Prefix);
    if (At == llvm::StringRef::npos) {
      Clean.append(Annotated.data(), Annotated.size());
      break;
    }
    Clean.append(Annotated.data(), At);
    Annotated = Annotated.drop_front(At + Prefix.size());
    size_t Stop = Annotated.find('\x1f');
    if (Stop == llvm::StringRef::npos || Stop < 2)
      return false;
    llvm::StringRef Token = Annotated.take_front(Stop);
    size_t Event = 0;
    if (Token.drop_back().getAsInteger(10, Event))
      return false;
    if (Token.back() == 'D') {
      if (Event >= DefinitionEntries.size())
        return false;
      Definitions.push_back({DefinitionEntries[Event], Clean.size()});
    } else if (Event >= Events.size()) {
      return false;
    } else if (Token.back() == 'B') {
      Stack.push_back({Event, Clean.size()});
    } else if (Token.back() == 'E') {
      if (Stack.empty() || Stack.back().Event != Event)
        return false;
      const auto Open = Stack.back();
      Stack.pop_back();
      if (Open.Begin != Clean.size()) {
        const auto &Target = Events[Event];
        if (Target.Region) {
          Candidate[*Target.Region].Spans.push_back({Open.Begin, Clean.size()});
          Covered[*Target.Region].insert(Target.Coverage.begin(),
                                         Target.Coverage.end());
        }
        if (Target.Function)
          Anchors.push_back(
              {*Target.Function, {Open.Begin, Clean.size()}, Target.Coverage});
      }
    } else {
      return false;
    }
    Annotated = Annotated.drop_front(Stop + 1);
  }
  if (!Stack.empty() || Clean != Ordinary)
    return false;
  for (auto &Region : Candidate) {
    auto &Spans = Region.Spans;
    std::sort(Spans.begin(), Spans.end(), [](auto A, auto B) {
      return std::tie(A.Begin, A.End) < std::tie(B.Begin, B.End);
    });
    Spans.erase(std::unique(Spans.begin(), Spans.end()), Spans.end());
    const auto &Required = (*Map.Recognitions)[Region.Recognition].Occurrences;
    const auto &Found = Covered[Region.Recognition];
    Region.Mapped =
        !Spans.empty() && std::includes(Found.begin(), Found.end(),
                                        Required.begin(), Required.end());
    std::vector<CSourceSpan> Merged;
    for (auto Span : Spans) {
      if (!Merged.empty() && Span.Begin <= Merged.back().End)
        Merged.back().End = std::max(Merged.back().End, Span.End);
      else
        Merged.push_back(Span);
    }
    Spans = std::move(Merged);
    if (!Region.Mapped)
      Spans.clear();
  }
  for (size_t I = 0; I < Candidate.size(); ++I)
    for (size_t J = I + 1; J < Candidate.size(); ++J)
      for (auto A : Candidate[I].Spans)
        for (auto B : Candidate[J].Spans)
          if (A.Begin < B.End && B.Begin < A.End)
            Candidate[I].Mapped = Candidate[J].Mapped = false;
  for (auto &Region : Candidate)
    if (!Region.Mapped)
      Region.Spans.clear();
  if (recordsInstructions())
    Map.Anchors = std::move(Anchors);
  else
    Map.Regions = std::move(Candidate);
  if (Mode != Recording::Instructions)
    Map.Definitions = std::move(Definitions);
  return true;
}

void CSourceRecorder::positionDefinition(std::optional<va_t> Entry,
                                         uint64_t Offset) {
  if (!recordsPositions() || Offset > std::numeric_limits<size_t>::max()) {
    PositionsValid = false;
    return;
  }
  PositionDefinitions.push_back({Entry, static_cast<size_t>(Offset)});
}

void CSourceRecorder::positionStatement(size_t Event, uint64_t Begin,
                                        uint64_t End) {
  if (!recordsPositions() || Event >= Events.size() ||
      !Events[Event].Function || Begin > End ||
      End > std::numeric_limits<size_t>::max()) {
    PositionsValid = false;
    return;
  }
  // A suppressed statement emits no text and must not gain a navigation span.
  if (Begin != End)
    PositionAnchors.push_back(
        {*Events[Event].Function,
         {static_cast<size_t>(Begin), static_cast<size_t>(End)},
         Events[Event].Coverage});
}

void CSourceRecorder::appendPositionBuffer(size_t First, uint64_t Size,
                                           uint64_t Offset) {
  if (!recordsPositions() || First > PositionAnchors.size() ||
      Offset > std::numeric_limits<size_t>::max() ||
      Size > std::numeric_limits<size_t>::max() - Offset) {
    PositionsValid = false;
    return;
  }
  for (size_t I = First; I < PositionAnchors.size(); ++I) {
    auto &Span = PositionAnchors[I].Span;
    if (Span.Begin > Span.End || Span.End > Size) {
      PositionsValid = false;
      return;
    }
  }
  for (size_t I = First; I < PositionAnchors.size(); ++I) {
    PositionAnchors[I].Span.Begin += Offset;
    PositionAnchors[I].Span.End += Offset;
  }
}

bool CSourceRecorder::finishPositions(llvm::StringRef Ordinary) {
  if (!recordsPositions() || !PositionsValid)
    return false;
  for (const auto &Definition : PositionDefinitions)
    if (Definition.Begin > Ordinary.size())
      return false;
  for (const auto &Anchor : PositionAnchors)
    if (Anchor.Span.Begin >= Anchor.Span.End ||
        Anchor.Span.End > Ordinary.size())
      return false;
  Map.Definitions = std::move(PositionDefinitions);
  Map.Anchors = std::move(PositionAnchors);
  return true;
}

} // namespace neverd
