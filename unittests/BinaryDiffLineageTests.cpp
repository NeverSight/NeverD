//===- BinaryDiffLineageTests.cpp - Cross-version evidence invariants -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent graph oracles, contradictory paths, source identity and bounded
/// transactional admission over authored comparison reports.
//===----------------------------------------------------------------------===//

#include "neverd/diff/Lineage.h"
#include "neverd/diff/LineageJSON.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace neverd::diff;

namespace {
template <typename Operation>
void expectStop(ComparisonState State, Operation Run) {
  try {
    Run();
    ADD_FAILURE() << "Expected a typed comparison stop";
  } catch (const ComparisonInterrupted &Stopped) {
    EXPECT_EQ(Stopped.State, State);
  }
}

Program program(unsigned Version, unsigned Count = 2) {
  ProgramIdentity I;
  I.SHA256 = llvm::utohexstr(Version + 1, true);
  I.SHA256.insert(0, 64 - I.SHA256.size(), '0');
  I.Path = "version-" + std::to_string(Version) + ".bin";
  I.Revision = "owned-lineage-fixture";
  I.Architecture = "x86_64";
  I.Bits = 64;
  I.Endian = "little";
  I.Format = "raw";
  I.Normalization = "fixture-v1";
  I.ImageBase = 0x1000 * (Version + 1);
  ProgramBuilder B(I);
  for (unsigned F = 0; F < Count; ++F) {
    const auto A = I.ImageBase + F * 16;
    B.beginFunction(A, 1, "function-" + std::to_string(F), NameSource::Symbol,
                    0);
    B.addBlock(A);
    const uint8_t Bytes[] = {0xc3};
    B.addInstruction(A, Bytes, "ret", 1, 1, InstructionFlow::Return);
  }
  return B.finish();
}

using Pairs = std::vector<std::pair<unsigned, unsigned>>;
std::string report(const Program &P, const Program &S, const Pairs &Pairs,
                   bool KeepHistory = false) {
  auto R = compare(P, S, {});
  const auto Original = R.Matches;
  for (const auto &M : Original)
    EXPECT_FALSE(
        applyOverride(R, P, S,
                      {OverrideKind::Unmatch, P.Functions[M.Primary].Address,
                       S.Functions[M.Secondary].Address}));
  for (const auto &[A, B] : Pairs)
    EXPECT_FALSE(applyOverride(
        R, P, S,
        {OverrideKind::Match, P.Functions[A].Address, S.Functions[B].Address}));
  if (!KeepHistory)
    R.Overrides.clear();
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  writeComparison(OS, R, P, S);
  return Text;
}

std::string render(llvm::json::Value Value) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << Value;
  return Text;
}

std::string document(const std::vector<std::string> &Reports) {
  llvm::json::Array Rows;
  for (const auto &R : Reports)
    Rows.push_back(R);
  return render(llvm::json::Object{{"schema", LineageSchema},
                                   {"schema_version", LineageSchemaVersion},
                                   {"reports", std::move(Rows)}});
}

std::shared_ptr<const Lineage> load(const std::vector<std::string> &Reports) {
  std::string Error;
  auto L = readLineage(document(Reports), {}, Error);
  EXPECT_TRUE(L) << Error;
  return L;
}

uint32_t node(const Lineage &L, const Program &P, unsigned Function = 0) {
  for (uint32_t V = 0; V < L.versions().size(); ++V)
    if (L.versions()[V].Identity.SHA256 == P.Identity.SHA256)
      return L.node(V, P.Functions[Function].Address).value_or(NoIndex);
  ADD_FAILURE() << "Missing owned snapshot";
  return NoIndex;
}
} // namespace

TEST(BinaryDiffLineage, AllSmallPairGraphsAgreeWithAnIndependentOracle) {
  const Program P[] = {program(0), program(1), program(2)};
  const Pairs Choices[] = {
      {},       {{0, 0}},         {{0, 1}},        {{1, 0}},
      {{1, 1}}, {{0, 0}, {1, 1}}, {{0, 1}, {1, 0}}};
  const std::pair<unsigned, unsigned> Sides[] = {{0, 1}, {1, 2}, {0, 2}};
  std::string Reports[3][7];
  for (unsigned R = 0; R < 3; ++R)
    for (unsigned C = 0; C < 7; ++C)
      Reports[R][C] = report(P[Sides[R].first], P[Sides[R].second], Choices[C]);
  for (unsigned Code = 0; Code < 7 * 7 * 7; ++Code) {
    SCOPED_TRACE(Code);
    unsigned Choice = Code;
    unsigned Distance[6][6];
    for (unsigned A = 0; A < 6; ++A)
      for (unsigned B = 0; B < 6; ++B)
        Distance[A][B] = A == B ? 0 : 7;
    std::vector<std::string> Selected;
    for (unsigned R = 0; R < 3; ++R, Choice /= 7) {
      Selected.push_back(Reports[R][Choice % 7]);
      for (const auto &[A, B] : Choices[Choice % 7]) {
        const auto X = Sides[R].first * 2 + A;
        const auto Y = Sides[R].second * 2 + B;
        Distance[X][Y] = Distance[Y][X] = 1;
      }
    }
    // Floyd-Warshall over six authored coordinates; independent of the
    // production union-find, BFS, ordering and snapshot digests.
    for (unsigned K = 0; K < 6; ++K)
      for (unsigned A = 0; A < 6; ++A)
        for (unsigned B = 0; B < 6; ++B)
          Distance[A][B] =
              std::min(Distance[A][B], Distance[A][K] + Distance[K][B]);
    const auto L = load(Selected);
    ASSERT_TRUE(L);
    ASSERT_EQ(L->nodes().size(), 6u);
    uint32_t Nodes[6];
    for (unsigned I = 0; I < 6; ++I)
      Nodes[I] = node(*L, P[I / 2], I % 2);
    for (unsigned Source = 0; Source < 6; ++Source) {
      const auto Trace = L->trace(Nodes[Source], 1000);
      unsigned Reachable = 0, Conflicts = 0;
      for (unsigned Target = 0; Target < 6; ++Target) {
        const auto It = llvm::find_if(Trace.nodes(), [&](const auto &N) {
          return N.Node == Nodes[Target];
        });
        if (Distance[Source][Target] >= 7) {
          EXPECT_EQ(It, Trace.nodes().end());
          continue;
        }
        ++Reachable;
        ASSERT_NE(It, Trace.nodes().end());
        EXPECT_EQ(It->Distance, Distance[Source][Target]);
        if (Source != Target) {
          ASSERT_LT(It->Edge, L->edges().size());
          const auto &E = L->edges()[It->Edge];
          EXPECT_TRUE(
              (E.Primary == It->Node && E.Secondary == It->Predecessor) ||
              (E.Secondary == It->Node && E.Primary == It->Predecessor));
          const auto Previous =
              llvm::find_if(Trace.nodes(), [&](const auto &N) {
                return N.Node == It->Predecessor;
              });
          ASSERT_NE(Previous, Trace.nodes().end());
          EXPECT_EQ(Previous->Distance + 1, It->Distance);
        }
      }
      for (unsigned V = 0; V < 3; ++V)
        Conflicts +=
            Distance[Source][V * 2] < 7 && Distance[Source][V * 2 + 1] < 7;
      EXPECT_EQ(Trace.nodes().size(), Reachable);
      EXPECT_EQ(Trace.conflicts().size(), Conflicts);
      using DirectEdge = std::tuple<uint32_t, uint32_t, std::string>;
      std::set<DirectEdge> ExpectedEdges, ActualEdges;
      unsigned EdgeChoice = Code;
      for (unsigned R = 0; R < 3; ++R, EdgeChoice /= 7)
        for (const auto &[A, B] : Choices[EdgeChoice % 7]) {
          const auto X = Sides[R].first * 2 + A;
          const auto Y = Sides[R].second * 2 + B;
          if (Distance[Source][X] < 7)
            ExpectedEdges.emplace(Nodes[X], Nodes[Y], Selected[R]);
        }
      std::set<uint32_t> EdgeIndices;
      for (const auto I : Trace.edges()) {
        ASSERT_LT(I, L->edges().size());
        EXPECT_TRUE(EdgeIndices.insert(I).second);
        const auto &E = L->edges()[I];
        ActualEdges.emplace(E.Primary, E.Secondary,
                            L->reports()[E.Report].Evidence->Text);
      }
      EXPECT_EQ(ActualEdges, ExpectedEdges);
      for (const auto C : Trace.conflicts()) {
        EXPECT_EQ(L->conflicts()[C].Kind,
                  LineageConflictKind::MultipleFunctions);
        EXPECT_EQ(L->conflicts()[C].Members.size(), 2u);
        const auto &Conflict = L->conflicts()[C];
        ASSERT_LT(Conflict.Version, L->versions().size());
        const auto &Identity = L->versions()[Conflict.Version].Identity;
        unsigned V = 0;
        while (V < 3 && P[V].Identity.SHA256 != Identity.SHA256)
          ++V;
        ASSERT_LT(V, 3u);
        EXPECT_EQ((std::set<uint32_t>(Conflict.Members.begin(),
                                      Conflict.Members.end())),
                  (std::set<uint32_t>{Nodes[V * 2], Nodes[V * 2 + 1]}));
      }
    }
  }
}

TEST(BinaryDiffLineage, RejectionsBelongToTheirReportAndSurviveOtherPaths) {
  const auto A = program(0), B = program(1), C = program(2);
  const auto Denied = report(A, C, {}, true);
  const auto L = load({report(A, B, {{0, 0}}), report(B, C, {{0, 0}}), Denied});
  ASSERT_TRUE(L);
  const auto Trace = L->trace(node(*L, A), 1000);
  ASSERT_EQ(Trace.conflicts().size(), 1u);
  const auto &Conflict = L->conflicts()[Trace.conflicts()[0]];
  EXPECT_EQ(Conflict.Kind, LineageConflictKind::RejectedPair);
  ASSERT_LT(Conflict.Report, L->reports().size());
  EXPECT_EQ(L->reports()[Conflict.Report].Evidence->Text, Denied);
  // A direct accepted edge in another report is additional conflicting
  // evidence. It cannot rewrite the rejected report's history.
  const auto Both = load({Denied, report(A, C, {{0, 0}})});
  ASSERT_TRUE(Both);
  EXPECT_EQ(Both->trace(node(*Both, A), 1000).conflicts().size(), 1u);
  const auto Cleared = load({report(A, C, {{0, 0}}, true)});
  ASSERT_TRUE(Cleared);
  EXPECT_TRUE(Cleared->trace(node(*Cleared, A), 1000).conflicts().empty());
}

TEST(BinaryDiffLineage, ReorderedAndRepeatedReportsRetainExactEvidence) {
  const auto A = program(0), B = program(1), C = program(2);
  const auto AB = report(A, B, {{0, 0}}), BC = report(B, C, {{0, 0}});
  const auto First = load({AB, BC});
  const auto Again = load({BC, AB, BC});
  ASSERT_TRUE(First && Again);
  EXPECT_EQ(First->revision(), Again->revision());
  EXPECT_EQ(First->reports().size(), 2u);
  EXPECT_EQ(First->reportBytes(), AB.size() + BC.size());
  const auto Saved = writeLineage(*First);
  std::string Error;
  auto Reopened = readLineage(Saved, {}, Error);
  ASSERT_TRUE(Reopened) << Error;
  EXPECT_EQ(writeLineage(*Reopened), Saved);
  auto Metadata = llvm::json::parse(Saved);
  ASSERT_TRUE(bool(Metadata));
  auto *Versions = Metadata->getAsObject()->getArray("versions");
  (*Versions)[0].getAsObject()->operator[]("label") = "Release 1.0";
  (*Versions)[0].getAsObject()->operator[]("branch") = "stable";
  Reopened = readLineage(render(std::move(*Metadata)), {}, Error);
  ASSERT_TRUE(Reopened) << Error;
  EXPECT_NE(First->revision(), Reopened->revision());
  EXPECT_EQ(Reopened->versions()[0].Branch, "stable");
  EXPECT_EQ(Reopened->edges().size(), First->edges().size());
}

TEST(BinaryDiffLineage, SameSnapshotRequiresTheSameInventory) {
  auto A = program(0), B = program(1);
  const auto Original = report(A, B, {{0, 0}});
  B.Functions[0].Name = "inconsistent saved inventory";
  std::string Error;
  EXPECT_FALSE(
      readLineage(document({Original, report(A, B, {{0, 0}})}), {}, Error));
  EXPECT_NE(Error.find("inventory"), std::string::npos);
  B.Identity.Revision = "explicitly reanalyzed";
  const auto Separate = load({Original, report(A, B, {{0, 0}})});
  ASSERT_TRUE(Separate);
  EXPECT_EQ(Separate->versions().size(), 3u);
}

TEST(BinaryDiffLineage,
     CanonicalIdentityPreservesEveryAnalysisFieldExceptPath) {
  const auto Original = program(0).Identity;
  const auto Key = programAnalysisIdentity(Original);
  auto Changed = Original;
  Changed.Path = "moved/file.bin";
  EXPECT_EQ(programAnalysisIdentity(Changed), Key);
  for (auto Member :
       {&ProgramIdentity::SHA256, &ProgramIdentity::ProjectInputName,
        &ProgramIdentity::ProjectStateSHA256, &ProgramIdentity::Format,
        &ProgramIdentity::Architecture, &ProgramIdentity::ABI,
        &ProgramIdentity::Normalization, &ProgramIdentity::Revision,
        &ProgramIdentity::Endian, &ProgramIdentity::Analysis}) {
    Changed = Original;
    Changed.*Member += ":different";
    EXPECT_NE(programAnalysisIdentity(Changed), Key);
  }
  for (const auto &Change : std::vector<std::function<void(ProgramIdentity &)>>{
           [](auto &I) { ++I.Bits; }, [](auto &I) { ++I.ImageBase; },
           [](auto &I) { ++I.EntryPoint; },
           [](auto &I) { I.DiscoveryComplete = false; }}) {
    Changed = Original;
    Change(Changed);
    EXPECT_NE(programAnalysisIdentity(Changed), Key);
  }
  auto Other = Original;
  Changed = Original;
  Changed.ABI = "a:b";
  Changed.Normalization = "c";
  Other.ABI = "a";
  Other.Normalization = "b:c";
  EXPECT_NE(programAnalysisIdentity(Changed), programAnalysisIdentity(Other));
}

TEST(BinaryDiffLineage, BudgetsAndStopsDoNotPublishPartialGraphs) {
  const auto A = program(0), B = program(1);
  const auto Input = document({report(A, B, {{0, 0}, {1, 1}})});
  std::string Error;
  const auto L = readLineage(Input, {}, Error);
  ASSERT_TRUE(L) << Error;
  LineageSettings Settings;
  Settings.MaxWork = L->work();
  EXPECT_TRUE(readLineage(Input, Settings, Error));
  --Settings.MaxWork;
  expectStop(ComparisonState::ResourceLimited,
             [&] { readLineage(Input, Settings, Error); });
  Settings = {};
  Settings.MaxNodes = 3;
  expectStop(ComparisonState::ResourceLimited,
             [&] { readLineage(Input, Settings, Error); });
  Settings = {};
  Settings.MaxEdges = 1;
  expectStop(ComparisonState::ResourceLimited,
             [&] { readLineage(Input, Settings, Error); });
  Settings = {};
  Settings.MaxSourceBytes = L->sourceBytes() - 1;
  expectStop(ComparisonState::ResourceLimited,
             [&] { readLineage(Input, Settings, Error); });
  Settings.MaxSourceBytes = 2 * L->sourceBytes() - L->reportBytes();
  bool Reparsed = false;
  ComparisonControl Control;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    Reparsed |= Phase == "parse";
  };
  const auto Cached = readLineage(Input, Settings, Error, Control, L.get());
  ASSERT_TRUE(Cached) << Error;
  EXPECT_FALSE(Reparsed);
  EXPECT_EQ(Cached->reports()[0].Evidence, L->reports()[0].Evidence);
  Settings.MaxNodes = 4;
  expectStop(ComparisonState::ResourceLimited,
             [&] { readLineage(Input, Settings, Error, {}, L.get()); });
  const auto Trace = L->trace(node(*L, A), 1000);
  EXPECT_NO_THROW(L->trace(node(*L, A), Trace.work()));
  expectStop(ComparisonState::ResourceLimited,
             [&] { L->trace(node(*L, A), Trace.work() - 1); });
  const auto Before = writeLineage(*L);
  for (const auto *Phase : {"lineage:parse", "lineage:reports", "lineage:graph",
                            "lineage:publish"}) {
    std::atomic<bool> Cancel = false;
    Control.Cancel = &Cancel;
    Control.Progress = [&](llvm::StringRef Name, uint64_t, uint64_t) {
      if (Name == Phase)
        Cancel = true;
    };
    expectStop(ComparisonState::Cancelled,
               [&] { readLineage(Input, {}, Error, Control, L.get()); });
    EXPECT_TRUE(Cancel);
    EXPECT_EQ(writeLineage(*L), Before);
  }
  Control = {};
  Control.Deadline = std::chrono::steady_clock::now();
  expectStop(ComparisonState::TimedOut,
             [&] { readLineage(Input, {}, Error, Control); });
  expectStop(ComparisonState::TimedOut,
             [&] { L->trace(node(*L, A), 1000, Control); });
}

TEST(BinaryDiffLineage, LargeConflictGroupsDoNotExpandEveryPairOrPath) {
  constexpr unsigned Count = 256;
  const auto A = program(0, Count), B = program(1, Count);
  Pairs Straight, Shifted;
  for (unsigned I = 0; I < Count; ++I) {
    Straight.emplace_back(I, I);
    Shifted.emplace_back(I, (I + 1) % Count);
  }
  const auto L = load({report(A, B, Straight), report(A, B, Shifted)});
  ASSERT_TRUE(L);
  ASSERT_EQ(L->conflicts().size(), 2u);
  const auto Trace = L->trace(node(*L, A), 10000);
  EXPECT_EQ(Trace.nodes().size(), 2 * Count);
  EXPECT_EQ(Trace.edges().size(), 2 * Count);
  EXPECT_EQ(Trace.conflicts().size(), 2u);
  for (const auto &Conflict : L->conflicts())
    EXPECT_EQ(Conflict.Members.size(), Count);
  // Include grouped conflict-member projection as well as the graph walk.
  EXPECT_LT(Trace.work(), 16 * Count);
}

TEST(BinaryDiffLineage, DeepChainsAndReverseEvidenceRemainIterative) {
  constexpr unsigned Count = 2000;
  std::vector<std::string> Reports;
  for (unsigned I = 0; I + 1 < Count; ++I) {
    auto A = program(I, 1), B = program(I + 1, 1);
    // Reverse every other report; orientation does not erase the association.
    if (I % 2)
      std::swap(A, B);
    Reports.push_back(report(A, B, {{0, 0}}));
  }
  const auto L = load(Reports);
  ASSERT_TRUE(L);
  const auto Trace = L->trace(node(*L, program(0, 1)), 10 * Count);
  ASSERT_EQ(Trace.nodes().size(), Count);
  EXPECT_EQ(Trace.nodes().back().Distance, Count - 1);
  EXPECT_TRUE(Trace.conflicts().empty());
  std::atomic<bool> Cancel = false;
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t Done, uint64_t) {
    if (Phase == "lineage:trace" && Done == Count / 2)
      Cancel = true;
  };
  expectStop(ComparisonState::Cancelled,
             [&] { L->trace(node(*L, program(0, 1)), 10 * Count, Control); });
  EXPECT_TRUE(Cancel);
  EXPECT_EQ(L->nodes().size(), Count);
}

TEST(BinaryDiffLineage, WrapperValidationAndHistoricalStopsAreExplicit) {
  const auto A = program(0), B = program(1);
  const auto Saved = report(A, B, {{0, 0}});
  const auto Input = document({Saved});
  std::string Error;
  for (const auto &Mutate :
       std::vector<std::function<void(llvm::json::Object &)>>{
           [](auto &O) { O["schema_version"] = 3; },
           [](auto &O) { O["trust_me"] = true; },
           [](auto &O) { O["reports"] = llvm::json::Array{}; },
           [](auto &O) { O["reports"] = llvm::json::Array{42}; },
           [](auto &O) {
             O["versions"] =
                 llvm::json::Array{llvm::json::Object{{"id", "unknown"},
                                                      {"label", "Release"},
                                                      {"branch", "stable"}}};
           }}) {
    auto Value = llvm::json::parse(Input);
    ASSERT_TRUE(bool(Value));
    Mutate(*Value->getAsObject());
    EXPECT_FALSE(readLineage(render(std::move(*Value)), {}, Error));
    EXPECT_FALSE(Error.empty());
  }
  for (const auto *Field : {"sha256", "revision"}) {
    auto Value = llvm::json::parse(Saved);
    ASSERT_TRUE(bool(Value));
    (*Value->getAsObject()->getObject("primary"))[Field] = "";
    EXPECT_FALSE(readLineage(document({render(std::move(*Value))}), {}, Error));
    EXPECT_NE(Error.find("hashes and analysis revisions"), std::string::npos);
  }
  for (const auto State :
       {ComparisonState::Cancelled, ComparisonState::TimedOut,
        ComparisonState::ResourceLimited}) {
    auto Value = llvm::json::parse(Saved);
    ASSERT_TRUE(bool(Value));
    (*Value->getAsObject())["state"] = stateName(State);
    const auto L = load({render(std::move(*Value))});
    ASSERT_TRUE(L);
    EXPECT_EQ(L->reports()[0].Evidence->Saved.State, State);
    EXPECT_EQ(L->trace(node(*L, A), 1000).nodes().size(), 2u);
  }
}

TEST(BinaryDiffLineage,
     ParseCompletionConsumesBothSuccessAndFailureBeforeStop) {
  const auto Input = document({report(program(0), program(1), {{0, 0}})});
  for (const auto &Text : {Input, std::string("{invalid")}) {
    std::atomic<bool> Cancel = false;
    bool Completed = false;
    ComparisonControl Control;
    Control.Cancel = &Cancel;
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                           uint64_t Total) {
      if (Phase == "lineage:parse" && Done && Done == Total) {
        Completed = true;
        Cancel = true;
      }
    };
    std::string Error;
    expectStop(ComparisonState::Cancelled,
               [&] { readLineage(Text, {}, Error, Control); });
    EXPECT_TRUE(Completed);
  }
}

TEST(BinaryDiffLineage, SnapshotIdentityEncodingIsAPersistenceContract) {
  ProgramIdentity I;
  I.SHA256 = std::string(64, 'a');
  I.ProjectInputName = "member:bin";
  I.ProjectStateSHA256 = std::string(64, 'b');
  I.Format = "ELF";
  I.Architecture = "x86_64";
  I.ABI = "";
  I.Normalization = "native:v1";
  I.Revision = "r:1";
  I.Endian = "little";
  I.Analysis = "names=0";
  I.Bits = 64;
  I.ImageBase = std::numeric_limits<uint64_t>::max();
  I.EntryPoint = 0;
  I.DiscoveryComplete = false;
  EXPECT_EQ(
      programAnalysisIdentity(I),
      "neverd.bindiff.analysis-v1:"
      "64:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "10:member:bin"
      "64:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
      "3:ELF6:x86_640:9:native:v13:r:16:little7:names=0"
      "2:6420:184467440737095516151:01:0");
}

TEST(BinaryDiffLineage, ParallelReportsKeepTheirSeparateDirectEvidence) {
  const auto A = program(0), B = program(1);
  const auto First = report(A, B, {{0, 0}});
  auto Other = llvm::json::parse(First);
  ASSERT_TRUE(bool(Other));
  auto *Warnings = Other->getAsObject()->getArray("warnings");
  ASSERT_TRUE(Warnings);
  Warnings->push_back("Independent analyst source");
  const auto Second = render(std::move(*Other));
  const auto L = load({First, Second});
  ASSERT_TRUE(L);
  const auto Trace = L->trace(node(*L, A), 1000);
  EXPECT_EQ(Trace.nodes().size(), 2u);
  ASSERT_EQ(Trace.edges().size(), 2u);
  EXPECT_TRUE(Trace.conflicts().empty());
  std::set<std::string> Sources;
  for (const auto E : Trace.edges()) {
    const auto &Edge = L->edges()[E];
    EXPECT_EQ(Edge.Primary, node(*L, A));
    EXPECT_EQ(Edge.Secondary, node(*L, B));
    Sources.insert(L->reports()[Edge.Report].Evidence->Text);
  }
  EXPECT_EQ(Sources, (std::set<std::string>{First, Second}));
}

TEST(BinaryDiffLineage, PagesRetainWitnessesConflictsAndCacheIdentity) {
  const auto A = program(0), B = program(1), C = program(2);
  const auto L = load(
      {report(A, B, {{0, 0}}), report(B, C, {{0, 0}}), report(A, C, {{0, 1}})});
  ASSERT_TRUE(L);
  const auto Source = node(*L, A);
  const auto Version = L->versions()[L->nodes()[Source].Version].ID;
  const auto Query = [&](llvm::StringRef Kind, uint64_t Offset = 0) {
    return readLineageQuery(llvm::json::Object{{"kind", Kind},
                                               {"revision", L->revision()},
                                               {"version", Version},
                                               {"address", "0x1000"},
                                               {"offset", Offset},
                                               {"limit", 1}});
  };
  auto First = lineagePage(*L, Query("trace_nodes"));
  ASSERT_TRUE(First.Trace);
  EXPECT_EQ(First.JSON.getInteger("total"), 4);
  std::set<uint32_t> Nodes;
  for (unsigned I = 0; I < 4; ++I) {
    const auto Page = lineagePage(*L, Query("trace_nodes", I), {}, First.Trace);
    EXPECT_EQ(Page.Trace, First.Trace);
    const auto *Rows = Page.JSON.getArray("items");
    ASSERT_TRUE(Rows && Rows->size() == 1);
    Nodes.insert(*(*Rows)[0].getAsObject()->getInteger("node"));
    if (I == 3)
      EXPECT_EQ(Page.JSON.get("next_offset")->kind(), llvm::json::Value::Null);
    else
      EXPECT_EQ(Page.JSON.getInteger("next_offset"), I + 1);
  }
  EXPECT_EQ(Nodes, (std::set<uint32_t>{node(*L, A), node(*L, B), node(*L, C),
                                       node(*L, C, 1)}));
  const auto Edges = lineagePage(*L, Query("trace_edges"), {}, First.Trace);
  EXPECT_EQ(Edges.Trace, First.Trace);
  EXPECT_EQ(Edges.JSON.getInteger("total"), 3);
  const auto Conflicts =
      lineagePage(*L, Query("trace_conflicts"), {}, First.Trace);
  EXPECT_EQ(Conflicts.JSON.getInteger("total"), 1);
  auto TooSmall = Query("trace_nodes");
  TooSmall.MaxWork = First.Trace->work() - 1;
  expectStop(ComparisonState::ResourceLimited,
             [&] { lineagePage(*L, TooSmall, {}, First.Trace); });
  auto Stale = Query("trace_nodes");
  Stale.Revision = "stale";
  EXPECT_THROW(lineagePage(*L, Stale), std::invalid_argument);
  EXPECT_THROW(lineagePage(*L, Query("trace_nodes", 5)), std::invalid_argument);
}

TEST(BinaryDiffLineage, ManualAssessmentsRemainReportLocalAndDoNotPropagate) {
  const auto A = program(0), B = program(1), C = program(2);
  auto R = compare(A, B, {});
  ReviewDraft Draft;
  Draft.Primary = A.Functions[0].Address;
  Draft.Secondary = B.Functions[0].Address;
  Draft.Case = "case-42";
  Draft.Author = "Analyst";
  Draft.Note = "Manual assessment only";
  Draft.Assessment = ReviewAssessment::PatchPresent;
  ASSERT_FALSE(appendReview(R, A, B, Draft, ""));
  Draft.Assessment = ReviewAssessment::Inconclusive;
  ASSERT_FALSE(appendReview(R, A, B, Draft, R.Reviews.head()));
  const auto Old = R.Reviews.Records[0].ID;
  std::string Saved;
  llvm::raw_string_ostream OS(Saved);
  writeComparison(OS, R, A, B);
  const auto L = load({Saved, report(B, C, {{0, 0}})});
  ASSERT_TRUE(L);
  auto Q = readLineageQuery(
      llvm::json::Object{{"kind", "reviews"}, {"revision", L->revision()}});
  const auto Page = lineagePage(*L, Q);
  ASSERT_EQ(Page.JSON.getInteger("total"), 1);
  const auto &Row = *Page.JSON.getArray("items")->front().getAsObject();
  EXPECT_EQ(Row.getString("assessment"), "inconclusive");
  EXPECT_EQ(Row.getString("supersedes"), Old);
  EXPECT_EQ(Row.getBoolean("active_pair"), true);
  EXPECT_EQ(Row.getBoolean("computed_patch_evidence"), false);
  Q.Latest = false;
  EXPECT_EQ(lineagePage(*L, Q).JSON.getInteger("total"), 2);
  EXPECT_EQ(lineageSummary(*L).getInteger("reviews"), 2);
}

TEST(BinaryDiffLineage,
     QueryAdmissionIsStrictAndStopsKeepTheProjectionPrivate) {
  const auto L = load({report(program(0), program(1), {{0, 0}})});
  ASSERT_TRUE(L);
  const llvm::json::Object Original{{"kind", "versions"},
                                    {"revision", L->revision()}};
  for (const auto &Mutate :
       std::vector<std::function<void(llvm::json::Object &)>>{
           [](auto &O) { O["kind"] = "guessed_ancestry"; },
           [](auto &O) { O["limit"] = 0; }, [](auto &O) { O["offset"] = -1; },
           [](auto &O) { O["max_work"] = true; },
           [](auto &O) { O["address"] = "0x1000"; },
           [](auto &O) { O["latest"] = false; },
           [](auto &O) { O["index"] = 0; }, [](auto &O) { O["search"] = ""; },
           [](auto &O) { O["unknown"] = 1; },
           [](auto &O) { O.erase("revision"); }}) {
    auto O = Original;
    Mutate(O);
    EXPECT_THROW(readLineageQuery(O), std::invalid_argument);
  }
  const auto Q = readLineageQuery(Original);
  for (const auto *Phase : {"lineage:query", "lineage:query_publish"}) {
    std::atomic<bool> Cancel = false;
    ComparisonControl Control;
    Control.Cancel = &Cancel;
    Control.Progress = [&](llvm::StringRef Name, uint64_t, uint64_t) {
      if (Name == Phase)
        Cancel = true;
    };
    expectStop(ComparisonState::Cancelled,
               [&] { lineagePage(*L, Q, Control); });
    EXPECT_TRUE(Cancel);
  }
  EXPECT_EQ(lineagePage(*L, Q).JSON.getInteger("total"), 2);
}

TEST(BinaryDiffLineage, SourceRegionPagesDoNotBecomeAcceptedAssociations) {
  const auto A = program(0), B = program(1);
  auto Result = compare(A, B, {});
  const auto Original = Result.Matches;
  for (const auto &M : Original)
    ASSERT_FALSE(
        applyOverride(Result, A, B,
                      {OverrideKind::Unmatch, A.Functions[M.Primary].Address,
                       B.Functions[M.Secondary].Address}));
  Result.Settings.Regions.Enabled = true;
  Result.Regions.State = ComparisonState::Complete;
  Result.Regions.Anchors.push_back({{0, 0, 0, 1}, {1, 0, 0, 1}, true});
  RegionHypothesis H;
  H.Id = "owned-source-region";
  H.Anchors = {0};
  H.PrimaryFunctions = {0};
  H.SecondaryFunctions = {1};
  H.Explanations = {RegionKind::Shared, RegionKind::Clone};
  H.Confidence = 0.75;
  Result.Regions.Hypotheses.push_back(H);
  const auto Save = [&] {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    writeComparison(OS, Result, A, B);
    return Text;
  };
  for (const bool Calls : {false, true}) {
    Result.Settings.Regions.Calls = Calls;
    Result.Regions.Calls = Calls;
    Result.Regions.Hypotheses[0].Anchors = {0};
    const auto L = load({Save()});
    ASSERT_TRUE(L);
    EXPECT_TRUE(L->edges().empty());
    auto Query = readLineageQuery(
        llvm::json::Object{{"kind", "native_regions"},
                           {"revision", L->revision()},
                           {"report", L->reports()[0].Evidence->ID}});
    auto Page = lineagePage(*L, Query, {});
    const auto &Row = *Page.JSON.getArray("items")->front().getAsObject();
    EXPECT_EQ(Row.getNumber("confidence"), 0.75);
    EXPECT_EQ(Row.getBoolean("native_authenticated"), false);
    EXPECT_EQ(Row.getInteger("anchor_count"), 1);
    EXPECT_EQ(Row.getString("profile"), regionProfile(Calls));
    if (Calls)
      EXPECT_EQ(Row.getString("call_evidence"),
                "current_function_correspondence");
    else
      EXPECT_FALSE(Row.get("call_evidence"));
    std::string Error;
    const auto Saved = writeLineage(*L);
    const auto Reopened = readLineage(Saved, {}, Error);
    ASSERT_TRUE(Reopened) << Error;
    EXPECT_EQ(writeLineage(*Reopened), Saved);
    const auto ReopenedPage = lineagePage(*Reopened, Query, {});
    const auto &ReopenedRow =
        *ReopenedPage.JSON.getArray("items")->front().getAsObject();
    EXPECT_EQ(ReopenedRow.getString("profile"), Row.getString("profile"));
    EXPECT_EQ(ReopenedRow.getString("call_evidence"),
              Row.getString("call_evidence"));
    Query.Kind = "native_region_anchors";
    Page = lineagePage(*L, Query, {});
    const auto &Anchor = *Page.JSON.getArray("items")->front().getAsObject();
    EXPECT_EQ(Anchor.getInteger("primary_function_index"), 0);
    EXPECT_EQ(Anchor.getInteger("secondary_function_index"), 1);
    EXPECT_FALSE(Anchor.get("primary_address"));
    EXPECT_EQ(Anchor.getString("profile"), regionProfile(Calls));
    EXPECT_EQ(Anchor.getBoolean("native_authenticated"), false);
    EXPECT_EQ(Anchor.getString("semantic_validation"), "not_performed");
    if (Calls)
      EXPECT_EQ(Anchor.getString("call_evidence"),
                "current_function_correspondence");
    else
      EXPECT_FALSE(Anchor.get("call_evidence"));
    Query.Index = 1;
    EXPECT_THROW((void)lineagePage(*L, Query, {}), std::invalid_argument);
    // Detached native evidence has no fresh binding. Invalid stored coordinates
    // must fail explicitly when queried rather than indexing unchecked memory.
    Result.Regions.Hypotheses[0].Anchors = {999};
    const auto Invalid = load({Save()});
    ASSERT_TRUE(Invalid);
    Query.Revision = Invalid->revision().str();
    Query.Report = Invalid->reports()[0].Evidence->ID;
    Query.Index = 0;
    EXPECT_THROW((void)lineagePage(*Invalid, Query, {}), std::invalid_argument);
  }
}

TEST(BinaryDiffLineage, UngroupedNativeAnchorsRetainEveryCoordinateAndPage) {
  const auto A = program(0), B = program(1);
  auto R = compare(A, B, {});
  R.Settings.Regions.Enabled = true;
  R.Settings.Regions.Calls = true;
  R.Regions.Calls = true;
  R.Regions.State = ComparisonState::ResourceLimited;
  R.Regions.StoppedPhase = "regions:relationships";
  for (uint32_t I = 0; I != 601; ++I)
    R.Regions.Anchors.push_back(
        {{I % 2, 0, 0, 1}, {(I + 1) % 2, 0, 0, 1}, false});
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  writeComparison(OS, R, A, B);
  const auto L = load({Text});
  ASSERT_TRUE(L);
  EXPECT_EQ(lineageSummary(*L).getBoolean("supports_all_region_anchors"), true);
  auto Q = readLineageQuery(
      llvm::json::Object{{"kind", "native_anchors"},
                         {"revision", L->revision()},
                         {"report", L->reports()[0].Evidence->ID},
                         {"offset", 599},
                         {"limit", 2}});
  auto Page = lineagePage(*L, Q).JSON;
  EXPECT_EQ(Page.getInteger("total"), 601);
  ASSERT_EQ(Page.getArray("items")->size(), 2);
  const auto &Row = *Page.getArray("items")->back().getAsObject();
  EXPECT_EQ(Row.getInteger("index"), 600);
  EXPECT_EQ(Row.getInteger("primary_function_index"), 0);
  EXPECT_EQ(Row.getInteger("secondary_function_index"), 1);
  EXPECT_EQ(Row.getString("profile"), "native-call-runs-v1");
  EXPECT_EQ(Row.getString("call_evidence"), "current_function_correspondence");
  EXPECT_EQ(Row.getBoolean("native_authenticated"), false);
  EXPECT_EQ(Row.getString("report"), L->reports()[0].Evidence->ID);
  EXPECT_FALSE(Row.get("hypothesis_id"));
  EXPECT_FALSE(Row.get("hypothesis_index"));
  EXPECT_TRUE(Page.get("next_offset")->getAsNull());
  const auto Work = *Page.getInteger("work");
  Q.MaxWork = Work;
  EXPECT_EQ(lineagePage(*L, Q).JSON.getInteger("total"), 601);
  Q.MaxWork = Work - 1;
  expectStop(ComparisonState::ResourceLimited, [&] { lineagePage(*L, Q); });
  Q.MaxWork = Work;
  Q.Offset = 602;
  EXPECT_THROW((void)lineagePage(*L, Q), std::invalid_argument);
  Q.Offset = 601;
  Page = lineagePage(*L, Q).JSON;
  EXPECT_EQ(Page.getInteger("total"), 601);
  EXPECT_TRUE(Page.getArray("items")->empty());
  std::atomic<bool> Cancel = true;
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  expectStop(ComparisonState::Cancelled, [&] { lineagePage(*L, Q, Control); });
  std::string Error;
  const auto Reopened = readLineage(writeLineage(*L), {}, Error);
  ASSERT_TRUE(Reopened) << Error;
  Q.Offset = 599;
  EXPECT_EQ(lineagePage(*Reopened, Q).JSON.getInteger("total"), 601);
  EXPECT_EQ(Reopened->edges().size(), L->edges().size());
}

TEST(BinaryDiffLineage, HistoricalDecisionsOutsideInventoryRemainWarnings) {
  const auto A = program(0), B = program(1);
  auto Value = llvm::json::parse(report(A, B, {{0, 0}}));
  ASSERT_TRUE(bool(Value));
  auto *Overrides = Value->getAsObject()->getArray("overrides");
  ASSERT_TRUE(Overrides);
  Overrides->push_back(llvm::json::Object{
      {"kind", "unmatch"}, {"primary", "0xdead"}, {"secondary", "0x2000"}});
  const auto L = load({render(std::move(*Value))});
  ASSERT_TRUE(L);
  EXPECT_EQ(L->nodes().size(), 4);
  EXPECT_EQ(L->edges().size(), 1);
  EXPECT_EQ(lineageSummary(*L).getInteger("historical_decision_warnings"), 1);
  auto Q = readLineageQuery(
      llvm::json::Object{{"kind", "decisions"},
                         {"revision", L->revision()},
                         {"report", L->reports()[0].Evidence->ID}});
  const auto Decisions = lineagePage(*L, Q, {}).JSON;
  const auto &D = *Decisions.getArray("items")->back().getAsObject();
  EXPECT_EQ(D.getBoolean("primary_in_inventory"), false);
  EXPECT_EQ(D.getBoolean("secondary_in_inventory"), true);
  Q.Kind = "warnings";
  const auto Warnings = lineagePage(*L, Q, {}).JSON;
  const auto &W = *Warnings.getArray("items")->back().getAsObject();
  EXPECT_EQ(W.getString("source"), "decision_history");
  EXPECT_EQ(W.getString("primary"), "0xdead");
  EXPECT_EQ(W.getInteger("decision_index"), 0);
}

TEST(BinaryDiffLineage, SSARegionPagesKeepOriginalAnchorsAndRespectStops) {
  auto A = program(0), B = program(1);
  // Authored retained SSA observations; lineage deliberately does not bind
  // these reports to a fresh native analysis or derive new region semantics.
  for (auto *P : {&A, &B}) {
    P->Identity.ABI = "owned-lineage-abi";
    auto Evidence = std::make_shared<DataFlowEvidence>();
    Evidence->Recipe = "owned-lineage-ssa";
    Evidence->EligibleFunctions = P->Functions.size();
    for (const auto &F : P->Functions) {
      DataFlowGraph G;
      G.Revision = P->Identity.Revision;
      G.ABI = P->Identity.ABI;
      G.Function = F.Address;
      G.EntryBlock = 0;
      G.Blocks.push_back({F.Address, F.Address + 1});
      for (uint32_t I = 0; I < 4; ++I) {
        FlowNode N;
        N.Opcode = "INT_ADD";
        N.ResultBytes = 8;
        N.Block = 0;
        N.Address = F.Address;
        N.OriginSequence = 0;
        FlowOperand Input;
        Input.Type = I ? FlowOperand::Kind::Node : FlowOperand::Kind::Parameter;
        Input.Node = I ? I - 1 : NoIndex;
        Input.Bytes = 8;
        N.Inputs.push_back(Input);
        Input.Type = FlowOperand::Kind::Constant;
        Input.Node = NoIndex;
        Input.Value = I + 3;
        N.Inputs.push_back(Input);
        G.Nodes.push_back(std::move(N));
      }
      Evidence->Entries.push_back(
          {F.Address, FlowAvailability::Available, {}, std::move(G)});
    }
    P->DataFlow = std::move(Evidence);
  }
  ComparisonSettings Settings;
  Settings.DataFlow.Enabled = true;
  auto R = compare(A, B, Settings);
  R.Settings.FlowRegions.Enabled = true;
  R.FlowRegions.State = ComparisonState::Complete;
  FlowRegionAnchor Anchor;
  Anchor.PrimaryFunction = 0x1000;
  Anchor.SecondaryFunction = 0x2010;
  Anchor.PrimaryRoot = 0;
  Anchor.SecondaryRoot = 2;
  Anchor.Nodes = {{0, 2}};
  Anchor.PrimaryOrigins = Anchor.SecondaryOrigins = 1;
  Anchor.OpenControlFrontier = true;
  R.FlowRegions.Anchors.push_back(Anchor);
  Anchor.PrimaryRoot = 1;
  Anchor.SecondaryRoot = 3;
  Anchor.Nodes = {{1, 3}};
  R.FlowRegions.Anchors.push_back(Anchor);
  FlowRegionHypothesis H;
  H.Id = "owned-ssa-region";
  H.Anchors = {0, 1};
  H.PrimaryFunctions = {0x1000};
  H.SecondaryFunctions = {0x2010};
  H.Explanations = {RegionKind::Shared};
  H.PrimaryNodes = H.SecondaryNodes = 2;
  H.OpenControlFrontier = true;
  R.FlowRegions.Hypotheses.push_back(H);
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  writeComparison(OS, R, A, B);
  const auto L = load({Text});
  ASSERT_TRUE(L);
  const auto Edges = L->edges().size();
  auto Q = readLineageQuery(
      llvm::json::Object{{"kind", "flow_regions"},
                         {"revision", L->revision()},
                         {"report", L->reports()[0].Evidence->ID}});
  auto Page = lineagePage(*L, Q, {}).JSON;
  ASSERT_EQ(Page.getArray("items")->size(), 1);
  const auto &Region = *Page.getArray("items")->front().getAsObject();
  EXPECT_EQ(Region.getInteger("anchor_count"), 2);
  EXPECT_EQ(Region.getBoolean("native_authenticated"), false);
  Q = readLineageQuery(
      llvm::json::Object{{"kind", "flow_region_anchors"},
                         {"revision", L->revision()},
                         {"report", L->reports()[0].Evidence->ID},
                         {"index", 0},
                         {"limit", 1}});
  Page = lineagePage(*L, Q, {}).JSON;
  EXPECT_EQ(Page.getInteger("total"), 2);
  EXPECT_EQ(Page.getArray("items")->front().getAsObject()->getString("report"),
            L->reports()[0].Evidence->ID);
  EXPECT_EQ(
      Page.getArray("items")->front().getAsObject()->getString("hypothesis_id"),
      "owned-ssa-region");
  EXPECT_EQ(Page.getInteger("next_offset"), 1);
  EXPECT_EQ(
      Page.getArray("items")->front().getAsObject()->getInteger("primary_root"),
      0);
  Q.Offset = 1;
  Page = lineagePage(*L, Q, {}).JSON;
  EXPECT_EQ(
      Page.getArray("items")->front().getAsObject()->getInteger("primary_root"),
      1);
  EXPECT_TRUE(Page.get("next_offset")->getAsNull());
  R.FlowRegions.Hypotheses.clear();
  std::string UngroupedText;
  llvm::raw_string_ostream UngroupedOS(UngroupedText);
  writeComparison(UngroupedOS, R, A, B);
  const auto Ungrouped = load({UngroupedText});
  ASSERT_TRUE(Ungrouped);
  const auto AllQuery = readLineageQuery(
      llvm::json::Object{{"kind", "flow_anchors"},
                         {"revision", Ungrouped->revision()},
                         {"report", Ungrouped->reports()[0].Evidence->ID},
                         {"offset", 1},
                         {"limit", 1}});
  const auto AllPage = lineagePage(*Ungrouped, AllQuery).JSON;
  EXPECT_EQ(AllPage.getInteger("total"), 2);
  const auto &AllRow = *AllPage.getArray("items")->front().getAsObject();
  EXPECT_EQ(AllRow.getInteger("index"), 1);
  EXPECT_EQ(AllRow.getInteger("primary_root"), 1);
  EXPECT_EQ(AllRow.getBoolean("open_control_frontier"), true);
  EXPECT_EQ(AllRow.getString("profile"), R.FlowRegions.Profile);
  EXPECT_EQ(AllRow.getBoolean("native_authenticated"), false);
  EXPECT_FALSE(AllRow.get("hypothesis_id"));
  EXPECT_FALSE(AllRow.get("hypothesis_index"));
  EXPECT_EQ(Ungrouped->edges().size(), Edges);
  Q.MaxWork = 1;
  expectStop(ComparisonState::ResourceLimited,
             [&] { (void)lineagePage(*L, Q, {}); });
  Q.MaxWork = 10000;
  std::atomic<bool> Stop{true};
  ComparisonControl Control;
  Control.Cancel = &Stop;
  expectStop(ComparisonState::Cancelled,
             [&] { (void)lineagePage(*L, Q, Control); });
  EXPECT_EQ(L->edges().size(), Edges);
  EXPECT_EQ(L->reports()[0].Evidence->Text, Text);
}

namespace {
llvm::json::Object releaseRecord(llvm::StringRef ID, const Lineage &L,
                                 llvm::json::Array Parents = {},
                                 llvm::StringRef Date = {}) {
  return llvm::json::Object{
      {"id", ID.str()},
      {"label", ID.str()},
      {"branch", "stable"},
      {"version", L.versions().front().ID},
      {"published_at", Date.empty() ? llvm::json::Value(nullptr)
                                    : llvm::json::Value(Date.str())},
      {"parents", std::move(Parents)}};
}
std::shared_ptr<const Lineage> releases(const Lineage &L,
                                        llvm::json::Array Rows) {
  std::string Error;
  auto Result =
      editLineageReleases(L,
                          llvm::json::Object{{"revision", L.revision()},
                                             {"upsert", std::move(Rows)}},
                          {}, Error);
  EXPECT_TRUE(Result) << Error;
  return Result;
}
llvm::json::Object releasePage(const Lineage &L, llvm::json::Object Query) {
  Query["revision"] = L.revision();
  return lineagePage(L, readLineageQuery(Query)).JSON;
}
} // namespace

TEST(BinaryDiffLineage, ReleaseAliasesKeepSnapshotAndSourceIdentities) {
  const auto L = load({report(program(0), program(1), {{0, 0}})});
  ASSERT_TRUE(L);
  const auto Original = writeLineage(*L);
  auto R = releases(
      *L, llvm::json::Array{
              releaseRecord("merge", *L, llvm::json::Array{"stable", "preview"},
                            "2026-10-25T08:00:00Z"),
              releaseRecord("stable", *L, llvm::json::Array{"base"},
                            "2026-10-24T08:00:00Z"),
              releaseRecord("base", *L),
              releaseRecord("preview", *L, llvm::json::Array{"base"})});
  ASSERT_TRUE(R);
  EXPECT_EQ(R->schemaVersion(), 2u);
  EXPECT_EQ(R->releases().size(), 4u);
  EXPECT_EQ(R->releaseLinks(), 4u);
  EXPECT_EQ(R->versions().size(), L->versions().size());
  EXPECT_EQ(R->nodes().size(), L->nodes().size());
  EXPECT_EQ(R->edges().size(), L->edges().size());
  EXPECT_EQ(R->reports()[0].Evidence->Text, L->reports()[0].Evidence->Text);
  for (const auto &Release : R->releases())
    EXPECT_EQ(Release.Version, 0u);
  std::vector<std::string> Timeline;
  for (const auto I : R->releaseTimeline())
    Timeline.push_back(R->releases()[I].ID);
  EXPECT_EQ(Timeline,
            (std::vector<std::string>{"base", "stable", "preview", "merge"}));
  const auto Parents = releasePage(
      *R, llvm::json::Object{
              {"kind", "release_parents"}, {"release", "merge"}, {"limit", 1}});
  EXPECT_EQ(Parents.getInteger("total"), 2);
  EXPECT_EQ(Parents.getInteger("next_offset"), 1);
  EXPECT_EQ(Parents.getArray("items")->front().getAsObject()->getString("id"),
            "preview");
  const auto Children =
      releasePage(*R, llvm::json::Object{{"kind", "release_children"},
                                         {"release", "base"}});
  EXPECT_EQ(Children.getInteger("total"), 2);
  EXPECT_EQ(writeLineage(*L), Original);
  std::string Error;
  const auto Restored = readLineage(writeLineage(*R), {}, Error);
  ASSERT_TRUE(Restored) << Error;
  EXPECT_EQ(Restored->revision(), R->revision());
  EXPECT_EQ(writeLineage(*Restored), writeLineage(*R));
}

TEST(BinaryDiffLineage, ReleaseSchemaUpgradePreservesLegacyDocuments) {
  const auto L = load({report(program(0), program(1), {{0, 0}})});
  ASSERT_TRUE(L);
  const auto Old = writeLineage(*L);
  std::string Error;
  const auto Reopened = readLineage(Old, {}, Error);
  ASSERT_TRUE(Reopened);
  EXPECT_EQ(Reopened->schemaVersion(), 1u);
  EXPECT_EQ(Reopened->revision(), L->revision());
  EXPECT_EQ(writeLineage(*Reopened), Old);
  const auto Upgraded = releases(*L, llvm::json::Array{});
  ASSERT_TRUE(Upgraded);
  EXPECT_EQ(Upgraded->schemaVersion(), 2u);
  EXPECT_NE(Upgraded->revision(), L->revision());
  const auto Empty = readLineage(writeLineage(*Upgraded), {}, Error);
  ASSERT_TRUE(Empty);
  EXPECT_EQ(Empty->schemaVersion(), 2u);
  EXPECT_EQ(Empty->revision(), Upgraded->revision());
  auto Invalid = llvm::json::parse(Old);
  ASSERT_TRUE(bool(Invalid));
  (*Invalid->getAsObject())["releases"] = llvm::json::Array{};
  EXPECT_FALSE(readLineage(render(std::move(*Invalid)), {}, Error));
  EXPECT_NE(Error.find("schema version 2"), std::string::npos);
}

TEST(BinaryDiffLineage, ReleaseMetadataRejectsInvalidDatesAndAncestry) {
  const auto L = load({report(program(0), program(1), {{0, 0}})});
  ASSERT_TRUE(L);
  std::string Error;
  const auto Reject = [&](llvm::json::Array Rows) {
    EXPECT_FALSE(
        editLineageReleases(*L,
                            llvm::json::Object{{"revision", L->revision()},
                                               {"upsert", std::move(Rows)}},
                            {}, Error));
    EXPECT_FALSE(Error.empty());
  };
  for (const auto Date :
       {"2025-02-29T00:00:00Z", "2024-04-31T00:00:00Z", "0000-01-01T00:00:00Z",
        "2024-01-01T24:00:00Z", "2024-01-01T00:60:00Z", "2024-01-01T00:00:60Z",
        "2024-01-01T00:00:00+00:00", "2024-01-01", "2024-01-01T00:00:0xZ"})
    Reject(
        llvm::json::Array{releaseRecord("a", *L, llvm::json::Array{}, Date)});
  ASSERT_TRUE(releases(
      *L, llvm::json::Array{releaseRecord("leap", *L, llvm::json::Array{},
                                          "2024-02-29T23:59:59Z")}));
  Reject(llvm::json::Array{releaseRecord("a", *L), releaseRecord("a", *L)});
  Reject(
      llvm::json::Array{releaseRecord("a", *L, llvm::json::Array{"missing"})});
  Reject(llvm::json::Array{releaseRecord("a", *L, llvm::json::Array{"a"})});
  Reject(
      llvm::json::Array{releaseRecord("a", *L),
                        releaseRecord("b", *L, llvm::json::Array{"a", "a"})});
  Reject(llvm::json::Array{releaseRecord("a", *L, llvm::json::Array{"b"}),
                           releaseRecord("b", *L, llvm::json::Array{"a"})});
  Reject(llvm::json::Array{
      releaseRecord("late", *L, llvm::json::Array{}, "2030-01-01T00:00:00Z"),
      releaseRecord("unknown", *L, llvm::json::Array{"late"}),
      releaseRecord("early", *L, llvm::json::Array{"unknown"},
                    "2020-01-01T00:00:00Z")});
  auto Unknown = releaseRecord("a", *L);
  Unknown["version"] = "missing";
  Reject(llvm::json::Array{std::move(Unknown)});
  auto Empty = releaseRecord(" ", *L);
  Reject(llvm::json::Array{std::move(Empty)});
}

TEST(BinaryDiffLineage, ReleaseTimelineMatchesEverySmallForwardDAG) {
  const auto L = load({report(program(0), program(1), {{0, 0}})});
  ASSERT_TRUE(L);
  for (unsigned Mask = 0; Mask < 64; ++Mask) {
    llvm::json::Array Rows;
    bool Edges[4][4]{};
    unsigned Bit = 0;
    for (unsigned Parent = 0; Parent < 4; ++Parent)
      for (unsigned Child = Parent + 1; Child < 4; ++Child)
        Edges[Parent][Child] = Mask & (1u << Bit++);
    for (unsigned I = 0; I < 4; ++I) {
      llvm::json::Array Parents;
      for (unsigned P = 0; P < I; ++P)
        if (Edges[P][I])
          Parents.push_back(std::to_string(P));
      Rows.push_back(releaseRecord(std::to_string(I), *L, std::move(Parents)));
    }
    const auto R = releases(*L, Rows);
    ASSERT_TRUE(R);
    unsigned Position[4]{};
    for (unsigned I = 0; I < 4; ++I)
      Position[R->releaseTimeline()[I]] = I;
    unsigned Links = 0;
    for (unsigned P = 0; P < 4; ++P)
      for (unsigned C = P + 1; C < 4; ++C)
        if (Edges[P][C]) {
          ++Links;
          EXPECT_LT(Position[P], Position[C]) << Mask;
        }
    EXPECT_EQ(R->releaseLinks(), Links);
    std::reverse(Rows.begin(), Rows.end());
    for (auto &Row : Rows) {
      auto *Parents = Row.getAsObject()->getArray("parents");
      std::reverse(Parents->begin(), Parents->end());
    }
    const auto Reordered = releases(*L, std::move(Rows));
    ASSERT_TRUE(Reordered);
    EXPECT_EQ(Reordered->revision(), R->revision()) << Mask;
  }
}

TEST(BinaryDiffLineage, ReleaseLabelEditsKeepParentsBeyondOnePage) {
  const auto L = load({report(program(0), program(1), {{0, 0}})});
  ASSERT_TRUE(L);
  llvm::json::Array Rows, Parents;
  for (unsigned I = 0; I < 257; ++I) {
    const auto ID = "parent-" + std::to_string(I);
    Parents.push_back(ID);
    Rows.push_back(releaseRecord(ID, *L));
  }
  Rows.push_back(releaseRecord("child", *L, std::move(Parents)));
  const auto R = releases(*L, std::move(Rows));
  ASSERT_TRUE(R);
  auto Rename = releaseRecord("child", *R);
  Rename["label"] = "Renamed without reading all parents";
  Rename.erase("parents");
  const auto Renamed = releases(*R, llvm::json::Array{std::move(Rename)});
  ASSERT_TRUE(Renamed);
  EXPECT_EQ(Renamed->releases()[*Renamed->release("child")].Parents.size(),
            257u);
  const auto Page =
      releasePage(*Renamed, llvm::json::Object{{"kind", "release_parents"},
                                               {"release", "child"},
                                               {"offset", 200}});
  EXPECT_EQ(Page.getInteger("total"), 257);
  EXPECT_EQ(Page.getArray("items")->size(), 57u);
  EXPECT_FALSE(Page.getArray("items")->front().getAsObject()->get("parents"));
  std::string Error;
  const auto Edited = editLineageReleases(
      *Renamed,
      llvm::json::Object{
          {"revision", Renamed->revision()},
          {"upsert", llvm::json::Array{releaseRecord("new-parent", *Renamed)}},
          {"remove", llvm::json::Array{"parent-256"}},
          {"parent_updates", llvm::json::Array{llvm::json::Object{
                                 {"release", "child"},
                                 {"remove", llvm::json::Array{"parent-256"}},
                                 {"add", llvm::json::Array{"new-parent"}}}}}},
      {}, Error);
  ASSERT_TRUE(Edited) << Error;
  EXPECT_FALSE(Edited->release("parent-256"));
  const auto &Child = Edited->releases()[*Edited->release("child")];
  EXPECT_EQ(Child.Parents.size(), 257u);
  EXPECT_TRUE(
      llvm::is_contained(Child.Parents, *Edited->release("new-parent")));
  EXPECT_FALSE(editLineageReleases(
      *Edited,
      llvm::json::Object{{"revision", Edited->revision()},
                         {"remove", llvm::json::Array{"new-parent"}}},
      {}, Error));
  EXPECT_NE(Error.find("unknown parent"), std::string::npos);
  EXPECT_EQ(Edited->releaseLinks(), 257u);
}

TEST(BinaryDiffLineage, ReleaseFocusLocatesSelectionAfterTimelineMoves) {
  const auto L = load({report(program(0), program(1), {{0, 0}})});
  ASSERT_TRUE(L);
  const auto R = releases(
      *L,
      llvm::json::Array{
          releaseRecord("a", *L, llvm::json::Array{}, "2025-01-01T00:00:00Z"),
          releaseRecord("b", *L, llvm::json::Array{}, "2025-02-01T00:00:00Z"),
          releaseRecord("c", *L, llvm::json::Array{}, "2025-03-01T00:00:00Z"),
          releaseRecord("d", *L, llvm::json::Array{}, "2025-04-01T00:00:00Z")});
  ASSERT_TRUE(R);
  auto Page = releasePage(
      *R,
      llvm::json::Object{{"kind", "releases"}, {"focus", "d"}, {"limit", 2}});
  EXPECT_EQ(Page.getInteger("offset"), 2);
  auto Moved =
      releaseRecord("d", *R, llvm::json::Array{}, "2024-01-01T00:00:00Z");
  Moved.erase("parents");
  const auto Changed = releases(*R, llvm::json::Array{std::move(Moved)});
  ASSERT_TRUE(Changed);
  Page = releasePage(
      *Changed,
      llvm::json::Object{{"kind", "releases"}, {"focus", "d"}, {"limit", 2}});
  EXPECT_EQ(Page.getInteger("offset"), 0);
  EXPECT_EQ(Page.getInteger("total"), 4);
  EXPECT_EQ(Page.getArray("items")->front().getAsObject()->getString("id"),
            "d");
  EXPECT_THROW(releasePage(*Changed, llvm::json::Object{{"kind", "releases"},
                                                        {"focus", "d"},
                                                        {"offset", 0}}),
               std::invalid_argument);
  EXPECT_THROW(
      releasePage(*Changed, llvm::json::Object{{"kind", "releases"},
                                               {"focus", "d"},
                                               {"search", "does-not-exist"}}),
      std::invalid_argument);
}

TEST(BinaryDiffLineage, ReleaseEditsDoNotRevisitRetainedSourceBytes) {
  const auto Text = report(program(0), program(1), {{0, 0}});
  const auto Small = load({Text});
  const auto Padded = load({Text + std::string(4 * 1024 * 1024, ' ')});
  ASSERT_TRUE(Small);
  ASSERT_TRUE(Padded);
  EXPECT_EQ(Padded->reports()[0].Evidence->EncodedBytes,
            Small->reports()[0].Evidence->EncodedBytes + 4 * 1024 * 1024);
  const auto First =
      releases(*Small, llvm::json::Array{releaseRecord("a", *Small)});
  ASSERT_TRUE(First);
  LineageSettings Settings;
  Settings.MaxWork = First->work();
  ComparisonControl Control;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    EXPECT_NE(Phase, "parse");
    EXPECT_NE(Phase, "lineage:parse");
    EXPECT_NE(Phase, "lineage:reports");
  };
  std::string Error;
  const auto Next = editLineageReleases(
      *Padded,
      llvm::json::Object{
          {"revision", Padded->revision()},
          {"upsert", llvm::json::Array{releaseRecord("a", *Padded)}}},
      Settings, Error, Control);
  ASSERT_TRUE(Next) << Error;
  EXPECT_EQ(Next->work(), First->work());
  EXPECT_EQ(Next->reports()[0].Evidence, Padded->reports()[0].Evidence);
  EXPECT_EQ(Next->reports()[0].Evidence->Text,
            Text + std::string(4 * 1024 * 1024, ' '));
  const auto Reopened = readLineage(writeLineage(*Next), {}, Error);
  ASSERT_TRUE(Reopened) << Error;
  EXPECT_EQ(Reopened->revision(), Next->revision());
  EXPECT_EQ(writeLineage(*Reopened), writeLineage(*Next));
}

TEST(BinaryDiffLineage, ReleaseEditsRetainEveryGraphProjection) {
  const auto A = program(0), B = program(1), C = program(2);
  const auto L =
      load({report(A, B, {{0, 0}, {1, 1}}), report(B, C, {{0, 0}, {1, 1}}),
            report(C, A, {{0, 1}, {1, 0}}), report(A, C, {}, true)});
  ASSERT_TRUE(L);
  ASSERT_FALSE(L->conflicts().empty());
  const auto R = releases(
      *L,
      llvm::json::Array{releaseRecord("base", *L),
                        releaseRecord("head", *L, llvm::json::Array{"base"})});
  ASSERT_TRUE(R);
  const auto Compare = [&](llvm::json::Object Query) {
    auto Before = releasePage(*L, Query);
    auto After = releasePage(*R, Query);
    for (auto &Row : *Before.getArray("items")) {
      EXPECT_EQ(Row.getAsObject()->getString("lineage_revision"),
                L->revision());
      Row.getAsObject()->erase("lineage_revision");
    }
    for (auto &Row : *After.getArray("items")) {
      EXPECT_EQ(Row.getAsObject()->getString("lineage_revision"),
                R->revision());
      Row.getAsObject()->erase("lineage_revision");
    }
    EXPECT_EQ(render(*Before.get("items")), render(*After.get("items")))
        << render(llvm::json::Value(std::move(Query)));
    EXPECT_EQ(Before.getInteger("total"), After.getInteger("total"));
  };
  for (const auto Kind : {"versions", "reports", "edges", "conflicts"})
    Compare(llvm::json::Object{{"kind", Kind}});
  for (const auto &V : L->versions()) {
    Compare(llvm::json::Object{{"kind", "functions"}, {"version", V.ID}});
    for (const auto &F : L->functions(*L->version(V.ID)))
      for (const auto Kind : {"trace_nodes", "trace_edges", "trace_conflicts"})
        Compare(
            llvm::json::Object{{"kind", Kind},
                               {"version", V.ID},
                               {"address", "0x" + llvm::utohexstr(F.Address)}});
  }
  for (uint32_t I = 0; I < L->conflicts().size(); ++I)
    Compare(llvm::json::Object{{"kind", "conflict_members"}, {"index", I}});
  for (uint32_t I = 0; I < L->reports().size(); ++I)
    EXPECT_EQ(R->reports()[I].Evidence, L->reports()[I].Evidence);
}

TEST(BinaryDiffLineage, ReleaseEditsHonorBudgetsAndCancellation) {
  const auto L = load({report(program(0), program(1), {{0, 0}})});
  ASSERT_TRUE(L);
  const llvm::json::Object Edit{
      {"revision", L->revision()},
      {"upsert",
       llvm::json::Array{releaseRecord("a", *L),
                         releaseRecord("b", *L, llvm::json::Array{"a"})}}};
  const auto Before = writeLineage(*L);
  std::string Error;
  const auto R = editLineageReleases(*L, Edit, {}, Error);
  ASSERT_TRUE(R) << Error;
  LineageSettings Exact;
  Exact.MaxWork = R->work();
  const auto AtBudget = editLineageReleases(*L, Edit, Exact, Error);
  ASSERT_TRUE(AtBudget) << Error;
  EXPECT_EQ(AtBudget->work(), R->work());
  EXPECT_EQ(AtBudget->revision(), R->revision());
  LineageSettings TooSmall;
  TooSmall.MaxWork = 1;
  bool Preparing = false;
  ComparisonControl Admission;
  Admission.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    Preparing |= Phase == "lineage:release_edit";
  };
  expectStop(ComparisonState::ResourceLimited, [&] {
    (void)editLineageReleases(*L, Edit, TooSmall, Error, Admission);
  });
  EXPECT_FALSE(Preparing);
  for (unsigned Kind = 0; Kind < 4; ++Kind) {
    LineageSettings S;
    if (Kind == 0)
      S.MaxNodes = L->nodes().size() * 2 + 1;
    else if (Kind == 1)
      S.MaxEdges = L->edges().size() * 2;
    else if (Kind == 2)
      S.MaxWork = R->work() - 1;
    else
      S.MaxSourceBytes = L->sourceBytes();
    expectStop(ComparisonState::ResourceLimited,
               [&] { (void)editLineageReleases(*L, Edit, S, Error); });
  }
  for (const auto *Phase :
       {"lineage:release_edit", "lineage:reuse_graph", "lineage:publish"}) {
    std::atomic<bool> Stop{false};
    ComparisonControl C;
    C.Cancel = &Stop;
    C.Progress = [&](llvm::StringRef Current, uint64_t, uint64_t) {
      if (Current == Phase)
        Stop = true;
    };
    expectStop(ComparisonState::Cancelled,
               [&] { (void)editLineageReleases(*L, Edit, {}, Error, C); });
    EXPECT_TRUE(Stop);
  }
  EXPECT_EQ(writeLineage(*L), Before);
  auto Stale = Edit;
  Stale["revision"] = "old";
  EXPECT_FALSE(editLineageReleases(*L, Stale, {}, Error));
  EXPECT_NE(Error.find("stale"), std::string::npos);
}
