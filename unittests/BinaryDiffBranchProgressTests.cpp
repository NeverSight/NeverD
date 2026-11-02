//===- BinaryDiffBranchProgressTests.cpp - Shared counter paths -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Progress must hold across every continuation path, including compiler
/// funnels. Exercise real LLVM, alternating cycles and authenticated replay.
//===----------------------------------------------------------------------===//

#include "../lib/diff/FlowProgress.h"

#include "neverd/backend/ParameterSites.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/diff/Control.h"
#include "neverd/diff/DataFlow.h"
#include "neverd/diff/DataFlowJSON.h"
#include "neverd/diff/LLVMDataFlow.h"
#include "neverd/diff/Limits.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::diff;
namespace {
constexpr llvm::StringLiteral Branches = R"(
define i8 @f(i8 %start, i1 %choose) {
entry: br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %a, %left ], [ %b, %right ]
 br i1 %choose, label %left, label %right
left:
 %a = add nsw i8 %i, -1
 %ca = icmp ne i8 %a, 0
 br i1 %ca, label %header, label %exit
right:
 %b = sub nsw i8 %i, 2
 %cb = icmp ne i8 %b, 0
 br i1 %cb, label %header, label %exit
exit: ret i8 %i
})";
constexpr llvm::StringLiteral Funnel = R"(
define i8 @f(i8 %start, i1 %choose) {
entry: br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %next, %latch ]
 br i1 %choose, label %left, label %right
left:
 %ca = icmp ne i8 %i, 0
 br i1 %ca, label %latch, label %exit
right:
 %cb = icmp sgt i8 %i, 1
 br i1 %cb, label %latch, label %exit
latch:
 %next = add nsw i8 %i, -1
 br label %header
exit: ret i8 %i
})";
constexpr llvm::StringLiteral Selected = R"(
define i8 @f(i8 %start, i1 %choose) {
entry: br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %next, %latch ]
 br i1 %choose, label %left, label %right
left:
 %a = add nsw i8 %i, -1
 %ca = icmp ne i8 %a, 0
 br i1 %ca, label %latch, label %exit
right:
 %b = sub nsw i8 %i, 2
 %cb = icmp ne i8 %i, 0
 br i1 %cb, label %latch, label %exit
latch:
 %next = phi i8 [ %a, %left ], [ %b, %right ]
 br label %header
exit: ret i8 %i
})";
std::string replace(std::string Text, llvm::StringRef From,
                    llvm::StringRef To) {
  const auto At = Text.find(From.str());
  if (At == std::string::npos)
    throw std::runtime_error("missing fixture replacement");
  Text.replace(At, From.size(), To.str());
  return Text;
}
DataFlowGraph graph(llvm::StringRef Text = Branches,
                    const DataFlowOptions &Options = {},
                    const ComparisonControl &Control = {}, bool Old = false) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto M = llvm::parseAssemblyString(Text, Diagnostic, Context);
  if (!M)
    throw std::runtime_error(Diagnostic.getMessage().str());
  auto &F = *M->getFunction("f");
  std::vector<parameter_sites::Site> Sites;
  for (const auto &A : F.args())
    Sites.push_back({0x10 + 8 * A.getArgNo(), int64_t(A.getArgNo())});
  parameter_sites::setParameterSites(F, Sites);
  LLVMSourceMap Sources;
  Sources.Functions[0x1000] = &F;
  uint64_t Address = 0x1000;
  for (auto &B : F)
    for (auto &I : B) {
      Sources.Observations.push_back({0x1000, {Address, 0}, &I});
      Address += 4;
    }
  std::string Before, After;
  llvm::raw_string_ostream Original(Before), Current(After);
  F.print(Original);
  const auto Build =
      Old ? buildLLVMNestedProgressDataFlow : buildLLVMBranchProgressDataFlow;
  auto G =
      Build(F, Sources, 0x1000, "owned", "same-abi", true, Options, Control);
  F.print(Current);
  EXPECT_EQ(Before, After);
  EXPECT_FALSE(G.validate(Options));
  return G;
}
llvm::json::Value json(const DataFlowGraph &G) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  llvm::json::OStream Out(Stream);
  writeDataFlowGraph(Out, G);
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed)
    throw std::runtime_error(llvm::toString(Parsed.takeError()));
  return std::move(*Parsed);
}
void unproved(llvm::StringRef Text) {
  const auto G = graph(Text);
  EXPECT_EQ(G.Progress->Kind, FlowProgressKind::Unproven);
  EXPECT_EQ(G.Progress->LoopCount, 0u);
  EXPECT_EQ(G.Progress->BackedgeCount, 0u);
  EXPECT_TRUE(G.Progress->Loops.empty());
  EXPECT_EQ(G.Progress->SourceContract, G.Progress->ObservedContract);
}
std::string manyPaths(unsigned Count) {
  std::string Text = "define i8 @f(i8 %start, i32 %select) {\nentry:\n"
                     " switch i32 %select, label %s0 [\n";
  for (unsigned I = 1; I < Count; ++I)
    Text +=
        " i32 " + std::to_string(I) + ", label %s" + std::to_string(I) + "\n";
  Text += " ]\n";
  for (unsigned I = 0; I < Count; ++I)
    Text += "s" + std::to_string(I) + ": br label %header\n";
  Text += "header:\n %i = phi i8 ";
  for (unsigned I = 0; I < Count; ++I) {
    const auto N = std::to_string(I);
    Text += (I ? ", " : "") + std::string("[ %start, %s") + N + " ], [ %u" + N +
            ", %l" + N + " ]";
  }
  Text += "\n switch i32 %select, label %l0 [\n";
  for (unsigned I = 1; I < Count; ++I)
    Text +=
        " i32 " + std::to_string(I) + ", label %l" + std::to_string(I) + "\n";
  Text += " ]\n";
  for (unsigned I = 0; I < Count; ++I) {
    const auto N = std::to_string(I);
    Text += "l" + N + ":\n %u" + N + " = add nsw i8 %i, -1\n %c" + N +
            " = icmp ne i8 %u" + N + ", 0\n br i1 %c" + N +
            ", label %header, label %exit\n";
  }
  return Text + "exit: ret i8 %i\n}";
}
} // namespace

TEST(BinaryDiffBranchProgress, DistinguishesLoopsBackedgesAndGuardWitnesses) {
  for (const auto Text : {Branches, Funnel, Selected}) {
    const auto G = graph(Text);
    ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
    EXPECT_EQ(G.Profile, LLVMBranchProgressDataFlowProfile);
    EXPECT_EQ(G.Progress->LoopCount, 1u);
    EXPECT_EQ(G.Progress->BackedgeCount, Text == Branches ? 2u : 1u);
    ASSERT_EQ(G.Progress->Loops.size(), 2u);
    EXPECT_LT(G.Progress->Loops[0].Block, G.Progress->Loops[1].Block);
    for (const auto &L : G.Progress->Loops) {
      EXPECT_EQ(L.Header, 1u);
      EXPECT_EQ(L.Latch, Text == Branches ? L.Block : 4u);
    }
    EXPECT_EQ(readDataFlowGraph(json(G)), G);
    const auto J = json(G);
    EXPECT_EQ(J.getAsObject()->getInteger("schema_version"), 8);
    const auto *P = J.getAsObject()->getObject("progress");
    EXPECT_EQ(P->getInteger("loop_count"), 1);
    EXPECT_EQ(P->getInteger("backedge_count"), Text == Branches ? 2 : 1);
    EXPECT_EQ(P->getInteger("guard_count"), 2);
    EXPECT_EQ(graph(Text, {}, {}, true).Progress->Kind,
              FlowProgressKind::Unproven);
  }
}

TEST(BinaryDiffBranchProgress,
     IntersectsDomainsWithoutRequiringIdenticalFlags) {
  const std::vector<std::pair<std::string, std::string>> Updates = {
      {"add nsw i8 %i, 1", "sub nsw i8 %i, -2"},
      {"add nuw i8 %i, 1", "add nuw i8 %i, -1"},
      {"sub nuw i8 %i, 1", "sub nuw i8 %i, -1"},
      {"add nsw nuw i8 %i, 1", "add nsw i8 %i, 2"}};
  for (const auto &[A, B] : Updates) {
    auto Text = replace(Branches.str(), "add nsw i8 %i, -1", A);
    Text = replace(Text, "sub nsw i8 %i, 2", B);
    EXPECT_EQ(graph(Text).Progress->Kind, FlowProgressKind::CounterLoops);
  }
  for (const auto Width : {1, 64}) {
    auto Text = Branches.str();
    size_t At = 0;
    const auto Type = "i" + std::to_string(Width);
    while ((At = Text.find("i8", At)) != std::string::npos) {
      Text.replace(At, 2, Type);
      At += Type.size();
    }
    Text = replace(Text, "add nsw " + Type + " %i, -1",
                   "add nsw " + Type + " %i, " +
                       (Width == 64 ? "-9223372036854775808" : "-1"));
    Text = replace(Text, "sub nsw " + Type + " %i, 2",
                   "add nsw " + Type + " %i, -1");
    EXPECT_EQ(graph(Text).Progress->Kind, FlowProgressKind::CounterLoops);
  }
}

TEST(BinaryDiffBranchProgress,
     RejectsDefinedAlternatingDirectionAndDomainCycles) {
  auto Text =
      replace(Branches.str(), "br i1 %choose, label %left, label %right",
              "%pick = icmp slt i8 %i, 0\n"
              " br i1 %pick, label %left, label %right");
  Text = replace(Text, "add nsw i8 %i, -1", "add nsw i8 %i, 1");
  Text = replace(Text, "icmp ne i8 %a, 0", "icmp ne i8 %a, 42");
  Text = replace(Text, "icmp ne i8 %b, 0", "icmp ne i8 %b, 42");
  // -1 -> 0 under NSW, then 0 -> 255 under NUW, indefinitely and defined.
  unproved(replace(Text, "sub nsw i8 %i, 2", "add nuw i8 %i, -1"));
  Text = replace(Text, "icmp slt i8 %i, 0", "icmp eq i8 %i, 0");
  // 0 -> 1 -> 0 under NSW is a separate direction counterexample.
  unproved(replace(Text, "sub nsw i8 %i, 2", "add nsw i8 %i, -1"));
}

TEST(BinaryDiffBranchProgress,
     TriesBothCounterCandidatesAndUsesTheExactPhiArm) {
  auto Text = replace(Branches.str(), " br i1 %choose",
                      " %j = phi i8 [ %start, %entry ], [ %ja, %left ], "
                      "[ %start, %right ]\n br i1 %choose");
  Text = replace(Text, " %ca = icmp ne i8 %a, 0",
                 " %ja = add nsw i8 %j, -1\n %ca = icmp ne i8 %ja, %a");
  EXPECT_EQ(graph(Text).Progress->Kind, FlowProgressKind::CounterLoops);
  auto SameCounter = replace(Branches.str(), " %ca = icmp ne i8 %a, 0",
                             " %extra = add nsw i8 %i, -3\n"
                             " %ca = icmp ne i8 %extra, %a");
  const auto G = graph(SameCounter);
  EXPECT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(G.Nodes[G.Progress->Loops.front().Update].Inputs.back().Value,
            255u);
}

TEST(BinaryDiffBranchProgress,
     KeepsPoisonObservableAcrossUnconditionalFunnels) {
  for (auto Text : {Funnel.str(), Selected.str()}) {
    Text = replace(Text, "[ %start, %entry ]", "[ undef, %entry ]");
    EXPECT_EQ(graph(Text).Progress->Kind, FlowProgressKind::CounterLoops);
  }
  auto Hoisted = replace(Funnel.str(), " %next = add nsw i8 %i, -1\n", "");
  Hoisted = replace(Hoisted, " br i1 %choose",
                    " %next = add nsw i8 %i, -1\n br i1 %choose");
  Hoisted = replace(Hoisted, "icmp ne i8 %i, 0", "icmp ne i8 %next, 0");
  EXPECT_EQ(graph(Hoisted).Progress->Kind, FlowProgressKind::CounterLoops);
  auto FrozenSeed =
      replace(Selected.str(), "entry: br label %header",
              "entry:\n %seed = freeze i8 %start\n br label %header");
  FrozenSeed = replace(FrozenSeed, "[ %start, %entry ]", "[ %seed, %entry ]");
  EXPECT_EQ(graph(FrozenSeed).Progress->Kind, FlowProgressKind::CounterLoops);
}

TEST(BinaryDiffBranchProgress, RefusesResetFreezeBypassAndUnsupportedArms) {
  for (const auto &Text :
       {replace(Branches.str(), "sub nsw i8 %i, 2", "sub i8 %i, 2"),
        replace(Branches.str(), "sub nsw i8 %i, 2", "sub nsw i8 %i, 0"),
        replace(Branches.str(), "[ %b, %right ]", "[ %start, %right ]"),
        replace(Branches.str(), "br i1 %cb, label %header, label %exit",
                "br label %header"),
        replace(Funnel.str(), "br i1 %cb, label %latch, label %exit",
                "br label %latch"),
        replace(Funnel.str(), "br i1 %cb, label %latch, label %exit",
                "br i1 %cb, label %latch, label %latch"),
        replace(Selected.str(), "[ %b, %right ]", "[ %start, %right ]"),
        replace(Selected.str(), " %cb = icmp ne i8 %i, 0",
                " %frozen = freeze i8 %i\n %cb = icmp ne i8 %frozen, 0"),
        replace(Funnel.str(), "br i1 %ca, label %latch, label %exit",
                "%frozen = freeze i1 %ca\n"
                " br i1 %frozen, label %latch, label %exit")}) {
    SCOPED_TRACE(Text);
    unproved(Text);
  }
  auto Frozen =
      replace(Selected.str(), "[ %next, %latch ]", "[ %frozen, %latch ]");
  Frozen = replace(Frozen, " br label %header\nexit:",
                   " %frozen = freeze i8 %next\n br label %header\nexit:");
  unproved(Frozen);
  unproved(replace(Funnel.str(), "exit: ret i8 %i",
                   "exit: ret i8 %i\nspin: br label %spin"));
}

TEST(BinaryDiffBranchProgress,
     RequiresCompleteUniquePredecessorsAndArmBinding) {
  for (const auto Text : {Branches, Selected}) {
    const auto G = graph(Text);
    const auto Counter = G.Progress->Loops.front().Counter;
    auto Bad = G;
    Bad.Nodes[Counter].Inputs.pop_back();
    EXPECT_TRUE(Bad.validate({}));
    EXPECT_THROW(readDataFlowGraph(json(Bad)), std::invalid_argument);
    Bad = G;
    Bad.Nodes[Counter].Inputs.back() = Bad.Nodes[Counter].Inputs.front();
    EXPECT_TRUE(Bad.validate({}));
  }
  const auto G = graph(Selected);
  const auto &First = G.Progress->Loops.front();
  const auto Counter = First.Counter;
  const auto Merge = G.Nodes[Counter].Inputs.back().Node;
  auto Bad = G;
  std::swap(Bad.Nodes[Merge].Inputs[0].Node, Bad.Nodes[Merge].Inputs[1].Node);
  EXPECT_TRUE(Bad.validate({}));
  Bad = G;
  Bad.Nodes[Merge].Inputs.pop_back();
  EXPECT_TRUE(Bad.validate({}));
  Bad = G;
  auto &Compare = Bad.Nodes[First.Compare];
  Compare.Inputs.front().Node = G.Progress->Loops.back().Update;
  EXPECT_TRUE(Bad.validate({}));
}

TEST(BinaryDiffBranchProgress, RepeatsNestedGroupsAndDiscardsPartialProgress) {
  auto Text =
      replace(Branches.str(), " br i1 %choose, label %left, label %right",
              " br label %inner\ninner:\n"
              " %j = phi i8 [ %i, %header ], [ %step, %inner ]\n"
              " %step = add nsw i8 %j, -1\n"
              " %test = icmp ne i8 %step, 0\n"
              " br i1 %test, label %inner, label %dispatch\n"
              "dispatch: br i1 %choose, label %left, label %right");
  const auto G = graph(Text);
  ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(G.Progress->LoopCount, 2u);
  EXPECT_EQ(G.Progress->BackedgeCount, 3u);
  EXPECT_EQ(G.Progress->Loops.size(), 3u);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
  unproved(replace(Text, "add nsw i8 %j, -1", "add i8 %j, -1"));
  const auto Left = Text.find("left:\n"), Right = Text.find("right:\n"),
             Exit = Text.find("exit:");
  Text = Text.substr(0, Left) + Text.substr(Right, Exit - Right) +
         Text.substr(Left, Right - Left) + Text.substr(Exit);
  const auto Reordered = graph(Text);
  EXPECT_EQ(Reordered.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_TRUE(std::is_sorted(
      Reordered.Progress->Loops.begin(), Reordered.Progress->Loops.end(),
      [](const auto &A, const auto &B) { return A.Block < B.Block; }));
}

TEST(BinaryDiffBranchProgress, RecomputesEveryGuardLatchAndCountOnRestore) {
  const auto G = graph(Selected);
  for (unsigned I = 0; I < G.Progress->Loops.size(); ++I) {
    auto Bad = G;
    Bad.Progress->Loops[I].Latch = Bad.Progress->Loops[I].Block;
    EXPECT_TRUE(Bad.validate({}));
    EXPECT_THROW(readDataFlowGraph(json(Bad)), std::invalid_argument);
    Bad = G;
    Bad.Progress->Loops[I].Update = G.Progress->Loops[1 - I].Update;
    EXPECT_TRUE(Bad.validate({}));
  }
  for (const auto Field : {"loop_count", "backedge_count", "guard_count"}) {
    auto J = json(G);
    (*J.getAsObject()->getObject("progress"))[Field] = 99;
    EXPECT_THROW(readDataFlowGraph(J), std::invalid_argument);
  }
  auto Bad = G;
  std::swap(Bad.Progress->Loops[0], Bad.Progress->Loops[1]);
  EXPECT_TRUE(Bad.validate({}));
  Bad.Profile = LLVMNestedProgressDataFlowProfile.str();
  EXPECT_TRUE(Bad.validate({}));
  auto J = json(G);
  (*J.getAsObject())["schema_version"] = 7;
  EXPECT_THROW(readDataFlowGraph(J), std::invalid_argument);
}

TEST(BinaryDiffBranchProgress,
     SharesExactStorageWorkAndCooperativeCancellation) {
  const auto G = graph(Selected);
  DataFlowOptions O;
  O.MaxNodes = G.retainedItems();
  EXPECT_EQ(graph(Selected, O), G);
  --O.MaxNodes;
  EXPECT_THROW(graph(Selected, O), DataFlowInterrupted);
  EXPECT_THROW(readDataFlowGraph(json(G), O), std::invalid_argument);
  O = {};
  uint64_t Low = 1, High = limits::diff::kDefaultFlowMaxWork;
  while (Low < High) {
    O.MaxWork = Low + (High - Low) / 2;
    try {
      graph(Selected, O);
      High = O.MaxWork;
    } catch (const DataFlowInterrupted &E) {
      EXPECT_EQ(E.State, ComparisonState::ResourceLimited);
      Low = O.MaxWork + 1;
    }
  }
  O.MaxWork = Low;
  EXPECT_EQ(graph(Selected, O), G);
  --O.MaxWork;
  EXPECT_THROW(graph(Selected, O), DataFlowInterrupted);
  uint64_t Total = 0;
  const auto P = proveFlowProgress(
      G, G.Progress->ObservedContract, {}, [&](uint64_t N) { Total += N; },
      FlowProgressMode::Branched);
  auto Expected = *G.Progress;
  Expected.SourceContract = Expected.ObservedContract;
  EXPECT_EQ(P, Expected);
  for (const auto Stop : {Total / 2, Total - 1}) {
    uint64_t Work = 0;
    EXPECT_THROW(proveFlowProgress(
                     G, G.Progress->ObservedContract, {},
                     [&](uint64_t N) {
                       Work += N;
                       if (Work >= Stop)
                         throw DataFlowInterrupted(ComparisonState::Cancelled);
                     },
                     FlowProgressMode::Branched),
                 DataFlowInterrupted);
  }
  std::atomic<bool> Cancel{true};
  ComparisonControl C;
  C.Cancel = &Cancel;
  EXPECT_THROW(graph(Selected, {}, C), DataFlowInterrupted);
}

TEST(BinaryDiffBranchProgress, WideGroupsDoNotRescanSeedsForEveryLatch) {
  const auto Measure = [](unsigned Count) {
    const auto G = graph(manyPaths(Count));
    EXPECT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
    EXPECT_EQ(G.Progress->LoopCount, 1u);
    EXPECT_EQ(G.Progress->BackedgeCount, Count);
    EXPECT_EQ(G.Progress->Loops.size(), Count);
    uint64_t Work = 0;
    const auto P = proveFlowProgress(
        G, G.Progress->ObservedContract, {}, [&](uint64_t N) { Work += N; },
        FlowProgressMode::Branched);
    auto Expected = *G.Progress;
    Expected.SourceContract = Expected.ObservedContract;
    EXPECT_EQ(P, Expected);
    return Work;
  };
  const auto Small = Measure(32), Large = Measure(128);
  EXPECT_LT(Large, Small * 6);
}

TEST(BinaryDiffBranchProgress, RetriesAfterASelectedCandidatePopulatesScratch) {
  auto Text = replace(Selected.str(), " br i1 %choose",
                      " %j = phi i8 [ %start, %entry ], [ %other, %latch ]\n"
                      " br i1 %choose");
  Text = replace(Text, " %ca = icmp ne i8 %a, 0",
                 " %ja = add nsw i8 %j, -1\n %ca = icmp ne i8 %ja, %a");
  Text = replace(Text, " %cb = icmp ne i8 %i, 0",
                 " %jb = add nsw i8 %j, 1\n %cb = icmp ne i8 %jb, %b");
  Text = replace(Text, "latch:\n",
                 "latch:\n %other = phi i8 [ %ja, %left ], [ %jb, %right ]\n");
  const auto G = graph(Text);
  EXPECT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(G.Progress->LoopCount, 1u);
  EXPECT_EQ(G.Progress->BackedgeCount, 1u);
  EXPECT_EQ(G.Progress->Loops.size(), 2u);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
}

TEST(BinaryDiffBranchProgress, GroupsSeveralFunnelsAndDirectPathsAtomically) {
  const auto G = graph(R"(
define i8 @f(i8 %start, i1 %choose, i1 %route) {
entry: br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %u, %latch1 ], [ %v, %latch2 ], [ %w, %direct ]
 br i1 %choose, label %direct, label %fork
fork: br i1 %route, label %first, label %second
first: br i1 %choose, label %a, label %b
second: br i1 %route, label %c, label %d
a:
 %ca = icmp ne i8 %i, 0
 br i1 %ca, label %latch1, label %exit
b:
 %cb = icmp sgt i8 %i, 1
 br i1 %cb, label %latch1, label %exit
latch1:
 %u = add nsw i8 %i, -1
 br label %header
c:
 %uc = add nsw i8 %i, -2
 %cc = icmp ne i8 %uc, 0
 br i1 %cc, label %latch2, label %exit
d:
 %ud = add nsw i8 %i, -3
 %cd = icmp ne i8 %ud, 0
 br i1 %cd, label %latch2, label %exit
latch2:
 %v = phi i8 [ %uc, %c ], [ %ud, %d ]
 br label %header
direct:
 %w = sub nsw i8 %i, 4
 %cw = icmp ne i8 %w, 0
 br i1 %cw, label %header, label %exit
exit: ret i8 %i
})");
  ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(G.Progress->LoopCount, 1u);
  EXPECT_EQ(G.Progress->BackedgeCount, 3u);
  EXPECT_EQ(G.Progress->Loops.size(), 5u);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
  const auto Counter = G.Progress->Loops.front().Counter;
  uint32_t Merge = NoIndex;
  for (const auto &Input : G.Nodes[Counter].Inputs)
    if (Input.Type == FlowOperand::Kind::Node && G.Nodes[Input.Node].Phi)
      Merge = Input.Node;
  ASSERT_NE(Merge, NoIndex);
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    auto Bad = G;
    auto &Inputs = Bad.Nodes[Merge].Inputs;
    if (!Mutation)
      Inputs.pop_back();
    else if (Mutation == 1)
      Inputs.back() = Inputs.front();
    else
      std::swap(Inputs[0].Node, Inputs[1].Node);
    EXPECT_TRUE(Bad.validate({}));
    const auto P = proveFlowProgress(
        Bad, Bad.Progress->ObservedContract, {}, [](uint64_t) {},
        FlowProgressMode::Branched);
    EXPECT_EQ(P.Kind, FlowProgressKind::Unproven);
    EXPECT_TRUE(P.Loops.empty());
    EXPECT_EQ(P.LoopCount, 0u);
    EXPECT_EQ(P.BackedgeCount, 0u);
  }
}
