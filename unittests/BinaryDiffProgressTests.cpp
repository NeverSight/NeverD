//===- BinaryDiffProgressTests.cpp - Source-bound loop progress -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Finite execution is distinct from matching loop bounds or program results.
/// Exercise real LLVM projections, adversarial cycles and saved graph proofs.
//===----------------------------------------------------------------------===//

#include "../lib/diff/FlowProgress.h"

#include "neverd/backend/ParameterSites.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/diff/Control.h"
#include "neverd/diff/DataFlow.h"
#include "neverd/diff/DataFlowJSON.h"
#include "neverd/diff/FlowFragments.h"
#include "neverd/diff/FlowMatch.h"
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
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::diff;
namespace {
DataFlowGraph graph(llvm::StringRef Text, const DataFlowOptions &Options = {},
                    const ComparisonControl &Control = {}, bool Legacy = false,
                    bool MultiBlock = false, bool Nested = false) {
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
      if (!llvm::isa<llvm::TruncInst>(I))
        Sources.Observations.push_back({0x1000, {Address, 0}, &I});
      Address += 4;
    }
  std::string Before, After;
  llvm::raw_string_ostream Old(Before), New(After);
  F.print(Old);
  const auto Build = Legacy       ? buildLLVMParameterDataFlow
                     : Nested     ? buildLLVMNestedProgressDataFlow
                     : MultiBlock ? buildLLVMMultiBlockProgressDataFlow
                                  : buildLLVMProgressDataFlow;
  auto G =
      Build(F, Sources, 0x1000, "owned", "same-abi", true, Options, Control);
  F.print(New);
  EXPECT_EQ(Before, After);
  EXPECT_FALSE(G.validate(Options));
  return G;
}
llvm::json::Value json(const DataFlowGraph &G) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  llvm::json::OStream Out(Stream);
  writeDataFlowGraph(Out, G);
  auto V = llvm::json::parse(Text);
  if (!V)
    throw std::runtime_error(llvm::toString(V.takeError()));
  return std::move(*V);
}
std::string change(std::string Text, llvm::StringRef From, llvm::StringRef To) {
  const auto At = Text.find(From);
  if (At == std::string::npos)
    throw std::runtime_error("missing fixture substitution");
  Text.replace(At, From.size(), To);
  return Text;
}
constexpr llvm::StringLiteral Loop = R"(
define i8 @f(i8 %start) {
entry:
 br label %loop
loop:
 %i = phi i8 [ %start, %entry ], [ %next, %loop ]
 %next = add nsw i8 %i, -1
 %test = icmp eq i8 %next, 0
 br i1 %test, label %exit, label %loop
exit:
 ret i8 %next
})";
constexpr llvm::StringLiteral BodyLoop = R"(
define i32 @f(i32 %start, i32 %x) {
entry:
 br label %loop
loop:
 %i = phi i32 [ %start, %entry ], [ %next, %loop ]
 %v = phi i32 [ %x, %entry ], [ %d, %loop ]
 %next = add nsw i32 %i, -1
 %a = xor i32 %v, 23
 %b = mul i32 %a, 7
 %c = add i32 %b, 9
 %d = xor i32 %c, 31
 %test = icmp eq i32 %next, 0
 br i1 %test, label %exit, label %loop
exit:
 ret i32 %d
})";
constexpr llvm::StringLiteral Unrolled = R"(
define i32 @f(i32 %start, i32 %x) mustprogress willreturn {
 %a0 = xor i32 %x, 23
 %b0 = mul i32 %a0, 7
 %c0 = add i32 %b0, 9
 %d0 = xor i32 %c0, 31
 %a = xor i32 %d0, 23
 %b = mul i32 %a, 7
 %c = add i32 %b, 9
 %d = xor i32 %c, 31
 ret i32 %d
})";
} // namespace

TEST(BinaryDiffProgress, CounterProofRetainsSourceAndDoesNotMutateLLVM) {
  const auto G = graph(Loop);
  ASSERT_TRUE(G.Progress);
  const auto &P = *G.Progress;
  EXPECT_EQ(P.Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(P.SourceContract.find("willreturn"), std::string::npos);
  EXPECT_NE(P.ObservedContract.find("mustprogress"), std::string::npos);
  EXPECT_NE(P.ObservedContract.find("willreturn"), std::string::npos);
  ASSERT_EQ(P.Loops.size(), 1u);
  const auto &L = P.Loops[0];
  EXPECT_EQ(L.Block, 1u);
  EXPECT_EQ(G.Nodes[L.Counter].Opcode, "phi");
  EXPECT_EQ(G.Nodes[L.Update].Opcode, "add");
  EXPECT_EQ(G.Nodes[L.Compare].Opcode, "icmp");
  EXPECT_EQ(G.retainedItems(), G.Nodes.size() + G.ParameterViews.size() + 2);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
  const auto Page = dataFlowPageJSON(G, false, L.Update, 1);
  EXPECT_EQ(Page.getInteger("schema_version"), 5);
  ASSERT_TRUE(Page.getObject("progress"));
  EXPECT_FALSE(Page.getObject("progress")->get("loops"));
  const auto Node = dataFlowNodeJSON(G, L.Update);
  ASSERT_TRUE(Node.getObject("progress_loop"));
  EXPECT_EQ(Node.getObject("progress_loop")->getInteger("counter"), L.Counter);
}

TEST(BinaryDiffProgress,
     SupportsExactNowrapDirectionsWidthsAndUnderdefinedSeeds) {
  for (llvm::StringRef Update :
       {"add nsw i8 %i, 1", "sub nsw i8 %i, 1", "sub nsw i8 %i, -1",
        "add nuw i8 %i, 1", "sub nuw i8 %i, 1", "add nsw i8 -1, %i",
        "add nsw i8 %i, 2", "add nsw i8 %i, -2", "sub nsw i8 %i, 2",
        "sub nsw i8 %i, -2", "add nuw i8 %i, 2", "sub nuw i8 %i, 2",
        "add nuw i8 %i, -1", "sub nuw i8 %i, -1", "add nsw i8 %i, -128",
        "sub nsw i8 %i, -128", "add nsw i8 %i, 127", "add nuw nsw i8 %i, 2"}) {
    SCOPED_TRACE(Update.str());
    EXPECT_EQ(
        graph(change(Loop.str(), "add nsw i8 %i, -1", Update)).Progress->Kind,
        FlowProgressKind::CounterLoops);
  }
  for (llvm::StringRef Seed : {"undef", "poison", "0", "-128"})
    EXPECT_EQ(graph(change(Loop.str(), "[ %start, %entry ]",
                           "[ " + Seed.str() + ", %entry ]"))
                  .Progress->Kind,
              FlowProgressKind::CounterLoops);
  for (llvm::StringRef Type : {"i1", "i64"}) {
    auto Text = Loop.str();
    for (size_t At = 0; (At = Text.find("i8", At)) != std::string::npos;
         At += Type.size())
      Text.replace(At, 2, Type);
    EXPECT_EQ(graph(Text).Progress->Kind, FlowProgressKind::CounterLoops);
    if (Type == "i64")
      for (llvm::StringRef Update : {"add nsw i64 %i, -9223372036854775808",
                                     "sub nsw i64 %i, -9223372036854775808",
                                     "add nsw i64 %i, 9223372036854775807",
                                     "add nuw i64 %i, 18446744073709551615",
                                     "sub nuw i64 %i, 18446744073709551615"}) {
        SCOPED_TRACE(Update.str());
        EXPECT_EQ(
            graph(change(Text, "add nsw i64 %i, -1", Update)).Progress->Kind,
            FlowProgressKind::CounterLoops);
      }
  }
  // Even steps miss this exit. Nowrap exhaustion still proves progress;
  // the otherwise identical wrapping counter can run forever.
  auto Missed = change(Loop.str(), "[ %start, %entry ]", "[ 0, %entry ]");
  Missed = change(Missed, "add nsw i8 %i, -1", "add nsw i8 %i, 2");
  Missed = change(Missed, "icmp eq i8 %next, 0", "icmp eq i8 %next, 1");
  EXPECT_EQ(graph(Missed).Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(graph(change(Missed, "add nsw", "add")).Progress->Kind,
            FlowProgressKind::Unproven);
}

TEST(BinaryDiffProgress, DirectComparisonsConsumeCounterPoisonInEitherOperand) {
  for (llvm::StringRef Predicate :
       {"eq", "ne", "ugt", "uge", "ult", "ule", "sgt", "sge", "slt", "sle"})
    for (llvm::StringRef Bound : {"0", "%start"})
      for (bool Reverse : {false, true})
        for (bool SelfOnTrue : {false, true}) {
          const auto Operands =
              Reverse ? Bound.str() + ", %next" : "%next, " + Bound.str();
          auto Text = change(Loop.str(), "icmp eq i8 %next, 0",
                             "icmp " + Predicate.str() + " i8 " + Operands);
          if (SelfOnTrue)
            Text = change(Text, "label %exit, label %loop",
                          "label %loop, label %exit");
          SCOPED_TRACE(Text);
          const auto G = graph(Text);
          EXPECT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
          EXPECT_EQ(readDataFlowGraph(json(G)), G);
        }
  for (llvm::StringRef Bound : {"undef", "poison", "%next"})
    EXPECT_EQ(graph(change(Loop.str(), "icmp eq i8 %next, 0",
                           "icmp eq i8 %next, " + Bound.str()))
                  .Progress->Kind,
              FlowProgressKind::CounterLoops);
  auto Other = change(Loop.str(),
                      " %test =", " %other = add nsw i8 %start, 1\n %test =");
  Other = change(Other, "icmp eq i8 %next, 0", "icmp slt i8 %other, %next");
  EXPECT_EQ(graph(Other).Progress->Kind, FlowProgressKind::CounterLoops);
}

TEST(BinaryDiffProgress,
     UnprovedLoopsKeepTheirOriginalContractsAndRemainAvailable) {
  for (llvm::StringRef Update :
       {"add i8 %i, -1", "add i8 %i, 2", "add nsw i8 %i, 0", "sub nuw i8 %i, 0",
        "sub nsw i8 1, %i", "add nsw i8 %i, %start", "add nsw i8 %i, undef",
        "add nsw i8 %i, poison"}) {
    SCOPED_TRACE(Update.str());
    const auto G = graph(change(Loop.str(), "add nsw i8 %i, -1", Update));
    ASSERT_TRUE(G.Progress);
    EXPECT_EQ(G.Progress->Kind, FlowProgressKind::Unproven);
    EXPECT_EQ(G.Progress->SourceContract, G.Progress->ObservedContract);
    EXPECT_TRUE(G.Progress->Loops.empty());
    EXPECT_EQ(readDataFlowGraph(json(G)), G);
  }
  for (llvm::StringRef Comparison :
       {"icmp eq i8 %start, 0", "icmp eq i8 %i, 0"})
    EXPECT_EQ(graph(change(Loop.str(), "icmp eq i8 %next, 0", Comparison))
                  .Progress->Kind,
              FlowProgressKind::Unproven);
}

TEST(BinaryDiffProgress, FreezeAndResetCannotReplenishTheCounterDomain) {
  auto Frozen =
      change(Loop.str(), " %test =", " %frozen = freeze i8 %next\n %test =");
  for (auto Text :
       {change(Frozen, "icmp eq i8 %next", "icmp eq i8 %frozen"),
        change(Frozen, "[ %next, %loop ]", "[ %frozen, %loop ]"),
        change(Loop.str(), "[ %next, %loop ]", "[ %start, %loop ]")})
    EXPECT_EQ(graph(Text).Progress->Kind, FlowProgressKind::Unproven);
  const auto Cycle = graph(R"(
define void @f() {
entry: br label %a
a:
 %i = phi i8 [ 4, %entry ], [ %x, %a ], [ 4, %b ]
 %x = sub nsw i8 %i, 1
 %cx = icmp eq i8 %x, 0
 br i1 %cx, label %b, label %a
b:
 %j = phi i8 [ 4, %a ], [ %y, %b ]
 %y = sub nsw i8 %j, 1
 %cy = icmp eq i8 %y, 0
 br i1 %cy, label %a, label %b
})");
  EXPECT_EQ(Cycle.Progress->Kind, FlowProgressKind::Unproven);
  EXPECT_TRUE(Cycle.Progress->Loops.empty());
  const auto Irreducible = graph(R"(
define void @f(i1 %choose) {
entry: br i1 %choose, label %a, label %b
a: br label %b
b: br label %a
})");
  EXPECT_EQ(Irreducible.Progress->Kind, FlowProgressKind::Unproven);
}

TEST(BinaryDiffProgress, SourcePromisesNeverSubstituteForAnIndependentProof) {
  auto Text = change(Loop.str(), "add nsw", "add");
  Text = change(Text, "%start) {", "%start) mustprogress willreturn {");
  const auto G = graph(Text);
  EXPECT_EQ(G.Progress->Kind, FlowProgressKind::Unproven);
  EXPECT_EQ(G.Progress->SourceContract, G.Progress->ObservedContract);
  const auto Attributes = graph(change(
      Loop.str(), "%start) {",
      "%start) \"example\"=\"mustprogress;flags=0;result=i8;willreturn\" {"));
  EXPECT_EQ(Attributes.Progress->Kind, FlowProgressKind::CounterLoops);
  for (const auto *Contract : {&Attributes.Progress->SourceContract,
                               &Attributes.Progress->ObservedContract})
    EXPECT_NE(Contract->find(
                  "\"example\"=\"mustprogress;flags=0;result=i8;willreturn\""),
              std::string::npos);
}

TEST(BinaryDiffProgress, MatchesSharedComputationWithoutEquatingLoopBounds) {
  const auto A = graph(BodyLoop), B = graph(Unrolled);
  EXPECT_EQ(A.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(B.Progress->Kind, FlowProgressKind::Acyclic);
  const auto Compare = [](const DataFlowGraph &A, const DataFlowGraph &B) {
    const auto AK = fingerprintFlowFragments(A),
               BK = fingerprintFlowFragments(B);
    return matchFlowFragment(FlowFragmentInput(A, AK), 7,
                             FlowFragmentInput(B, BK), 7, 4);
  };
  const auto M = Compare(A, B);
  EXPECT_EQ(M.Outcome, FlowMatchOutcome::Matched);
  EXPECT_FALSE(M.Frontier.empty());
  EXPECT_EQ(
      Compare(graph(BodyLoop, {}, {}, true), graph(Unrolled, {}, {}, true))
          .Outcome,
      FlowMatchOutcome::Different);
  EXPECT_EQ(
      Compare(A, graph(change(Unrolled.str(), "%c, 31", "%c, 33"))).Outcome,
      FlowMatchOutcome::Different);
  const auto Bound = graph(change(BodyLoop.str(), "%next, 0", "%next, 5"));
  EXPECT_EQ(Bound.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_NE(json(A), json(Bound));
  EXPECT_EQ(Compare(Bound, B).Outcome, FlowMatchOutcome::Matched);
}

TEST(BinaryDiffProgress, LowBitElisionPrecedesAndRemapsProofWitnesses) {
  auto Text =
      change(BodyLoop.str(), "i32 %start, i32 %x", "i32 %start, i64 %raw");
  Text = change(Text, "entry:\n", "entry:\n %x = trunc i64 %raw to i32\n");
  const auto G = graph(Text);
  ASSERT_EQ(G.ParameterViews.size(), 2u);
  EXPECT_EQ(G.ParameterViews[1].ElidedTruncations, 1u);
  EXPECT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(G.Progress->Loops[0].Update, 3u);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
}

TEST(BinaryDiffProgress,
     SavedProofsRejectForgedWitnessesAndNonfiniteOperations) {
  const auto G = graph(Loop);
  for (llvm::StringRef Suffix :
       {";result=i1;flags=0;predicate=31", ";result=i1;flags=0;predicate=42",
        ";result=i1;flags=0;predicate=032", ";result=i1;flags=1;predicate=32",
        ";result=i1;flags=0;predicate=32;unknown"}) {
    auto Forged = G;
    Forged.Nodes[G.Progress->Loops[0].Compare].Semantics =
        G.Progress->ObservedContract + Suffix.str();
    EXPECT_TRUE(Forged.validate({}));
    EXPECT_THROW(readDataFlowGraph(json(Forged)), std::invalid_argument);
  }
  for (llvm::StringRef Field : {"block", "counter", "update", "compare"}) {
    auto V = json(G);
    auto *P = V.getAsObject()->getObject("progress");
    (*P->getArray("loops"))[0].getAsObject()->operator[](Field) = 0;
    EXPECT_THROW(readDataFlowGraph(V), std::invalid_argument);
  }
  auto V = json(G);
  (*V.getAsObject()->getObject("progress"))["kind"] = "acyclic";
  EXPECT_THROW(readDataFlowGraph(V), std::invalid_argument);
  V = json(G);
  (*V.getAsObject()->getObject("progress"))["loop_count"] = 0;
  EXPECT_THROW(readDataFlowGraph(V), std::invalid_argument);
  V = json(G);
  (*V.getAsObject())["extra_proof"] = true;
  EXPECT_THROW(readDataFlowGraph(V), std::invalid_argument);
  auto Legacy = json(graph(Loop, {}, {}, true));
  (*Legacy.getAsObject())["progress"] = *json(G).getAsObject()->get("progress");
  EXPECT_THROW(readDataFlowGraph(Legacy), std::invalid_argument);
  for (llvm::StringRef Opcode : {"call", "unknown", "load", "llvm.fshl.i128"}) {
    auto Invalid = graph(Unrolled);
    Invalid.Nodes[0].Opcode = Opcode.str();
    EXPECT_TRUE(Invalid.validate({}));
  }
}

TEST(BinaryDiffProgress, ProofRowsAndWorkShareProjectionLimits) {
  const auto G = graph(Loop);
  DataFlowOptions O;
  O.MaxNodes = G.retainedItems();
  EXPECT_EQ(graph(Loop, O), G);
  --O.MaxNodes;
  EXPECT_THROW(graph(Loop, O), DataFlowInterrupted);
  EXPECT_THROW(readDataFlowGraph(json(G), O), std::invalid_argument);
  O = {};
  O.MaxWork = 1;
  EXPECT_THROW(graph(Loop, O), DataFlowInterrupted);
  uint64_t Low = 1, High = O.MaxWork = limits::diff::kDefaultFlowMaxWork;
  while (Low < High) {
    O.MaxWork = Low + (High - Low) / 2;
    try {
      graph(Loop, O);
      High = O.MaxWork;
    } catch (const DataFlowInterrupted &E) {
      EXPECT_EQ(E.State, ComparisonState::ResourceLimited);
      Low = O.MaxWork + 1;
    }
  }
  O.MaxWork = Low;
  EXPECT_EQ(graph(Loop, O), G);
  O.MaxWork = Low - 1;
  EXPECT_THROW(graph(Loop, O), DataFlowInterrupted);
  std::atomic<bool> Cancel{true};
  ComparisonControl C;
  C.Cancel = &Cancel;
  EXPECT_THROW(graph(Loop, {}, C), DataFlowInterrupted);
}

namespace {
constexpr llvm::StringLiteral MultiBlockLoop = R"(
define i8 @f(i8 %start, i8 %x, i1 %choose) {
entry: br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %next, %latch ]
 %v = phi i8 [ %x, %entry ], [ %value, %latch ]
 br i1 %choose, label %left, label %right
left:
 %a = xor i8 %v, 23
 br label %latch
right:
 %b = mul i8 %v, 7
 br label %latch
latch:
 %value = phi i8 [ %a, %left ], [ %b, %right ]
 %next = add nsw i8 %i, -1
 %test = icmp eq i8 %next, 0
 br i1 %test, label %exit, label %header
exit:
 ret i8 %value
})";
DataFlowGraph multiGraph(llvm::StringRef Text = MultiBlockLoop,
                         const DataFlowOptions &Options = {},
                         const ComparisonControl &Control = {}) {
  return graph(Text, Options, Control, false, true);
}
} // namespace

TEST(BinaryDiffMultiBlockProgress, VersionsTheProofAndNamesHeaderAndLatch) {
  const auto Old = graph(MultiBlockLoop), G = multiGraph();
  EXPECT_EQ(Old.Progress->Kind, FlowProgressKind::Unproven);
  EXPECT_EQ(Old.Progress->SourceContract, Old.Progress->ObservedContract);
  EXPECT_EQ(G.Profile, "llvm-progress-ssa-v2");
  ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  ASSERT_EQ(G.Progress->Loops.size(), 1u);
  const auto &L = G.Progress->Loops[0];
  EXPECT_EQ(L.Header, 1u);
  EXPECT_EQ(L.Block, 4u);
  EXPECT_EQ(G.Nodes[L.Counter].Block, L.Header);
  EXPECT_EQ(G.Nodes[L.Update].Block, L.Block);
  EXPECT_EQ(G.Nodes[L.Compare].Block, L.Block);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
  const auto Row = dataFlowNodeJSON(G, L.Update);
  ASSERT_TRUE(Row.getObject("progress_loop"));
  EXPECT_EQ(Row.getObject("progress_loop")->getInteger("header"), L.Header);
  const auto Page = dataFlowPageJSON(G, false, L.Update, 1);
  EXPECT_EQ(Page.getInteger("schema_version"), 6);
  EXPECT_FALSE(Page.getObject("progress")->get("loops"));
  EXPECT_EQ(G.retainedItems(), G.Nodes.size() + G.ParameterViews.size() + 2);
  const auto Self = multiGraph(Loop);
  EXPECT_EQ(Self.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(Self.Progress->Loops[0].Header, Self.Progress->Loops[0].Block);
  EXPECT_EQ(graph(Loop).Progress->Loops[0].Header, NoIndex);
}

TEST(BinaryDiffMultiBlockProgress,
     HandlesEarlyExitsSeedsAndNonTopologicalLayout) {
  auto Early =
      change(MultiBlockLoop.str(), " %a = xor i8 %v, 23\n br label %latch",
             " %a = xor i8 %v, 23\n br i1 %choose, label %exit, label %latch");
  Early = change(Early, "ret i8 %value", "ret i8 %x");
  EXPECT_EQ(multiGraph(Early).Progress->Kind, FlowProgressKind::CounterLoops);
  auto Seeds = change(MultiBlockLoop.str(), "entry: br label %header", R"(entry:
 br i1 %choose, label %first, label %second
first: br label %header
second: br label %header)");
  Seeds =
      change(Seeds, "[ %start, %entry ]", "[ %start, %first ], [ 0, %second ]");
  Seeds = change(Seeds, "[ %x, %entry ]", "[ %x, %first ], [ 1, %second ]");
  EXPECT_EQ(multiGraph(Seeds).Progress->Kind, FlowProgressKind::CounterLoops);
  auto Reordered = MultiBlockLoop.str();
  const auto Begin = Reordered.find("latch:"), End = Reordered.find("exit:");
  const auto Latch = Reordered.substr(Begin, End - Begin);
  Reordered.erase(Begin, End - Begin);
  Reordered.insert(Reordered.find("header:"), Latch);
  const auto G = multiGraph(Reordered);
  EXPECT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_GT(G.Progress->Loops[0].Header, G.Progress->Loops[0].Block);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
}

TEST(BinaryDiffMultiBlockProgress, HeaderUpdatesDominateContinuedIterations) {
  auto Hoisted =
      change(MultiBlockLoop.str(), " %next = add nsw i8 %i, -1\n", "");
  Hoisted = change(
      Hoisted, " br i1 %choose, label %left, label %right",
      " %next = add nsw i8 %i, -1\n br i1 %choose, label %left, label %right");
  auto Early =
      change(Hoisted, " %a = xor i8 %v, 23\n br label %latch",
             " %a = xor i8 %v, 23\n br i1 %choose, label %exit, label %latch");
  Early = change(Early, "ret i8 %value", "ret i8 %x");
  for (const auto &Text : {Hoisted, Early}) {
    const auto G = multiGraph(Text);
    ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
    const auto &L = G.Progress->Loops[0];
    EXPECT_EQ(G.Nodes[L.Update].Block, L.Header);
    EXPECT_NE(L.Header, L.Block);
    EXPECT_EQ(readDataFlowGraph(json(G)), G);
    EXPECT_EQ(graph(Text).Progress->Kind, FlowProgressKind::Unproven);
  }
  auto Wide = Hoisted;
  for (size_t At = 0; (At = Wide.find("i8", At)) != std::string::npos; At += 3)
    Wide.replace(At, 2, "i64");
  Wide = change(Wide, "add nsw i64 %i, -1",
                "add nsw i64 %i, -9223372036854775808");
  EXPECT_EQ(multiGraph(Wide).Progress->Kind, FlowProgressKind::CounterLoops);
}

TEST(BinaryDiffMultiBlockProgress, RetainsNowrapAndDirectGuardRequirements) {
  for (llvm::StringRef Update : {"add nsw i8 %i, 2", "sub nuw i8 %i, 2",
                                 "add nuw i8 -1, %i", "sub nsw i8 %i, -128"})
    EXPECT_EQ(
        multiGraph(change(MultiBlockLoop.str(), "add nsw i8 %i, -1", Update))
            .Progress->Kind,
        FlowProgressKind::CounterLoops);
  for (llvm::StringRef Update : {"add i8 %i, -1", "add nsw i8 %i, 0",
                                 "sub nsw i8 1, %i", "add nsw i8 %i, %start"}) {
    const auto G =
        multiGraph(change(MultiBlockLoop.str(), "add nsw i8 %i, -1", Update));
    EXPECT_EQ(G.Progress->Kind, FlowProgressKind::Unproven);
    EXPECT_EQ(G.Progress->SourceContract, G.Progress->ObservedContract);
  }
  auto Other = change(MultiBlockLoop.str(),
                      " %test =", " %other = add nsw i8 %x, 1\n %test =");
  Other = change(Other, "icmp eq i8 %next, 0", "icmp slt i8 %other, %next");
  EXPECT_EQ(multiGraph(Other).Progress->Kind, FlowProgressKind::CounterLoops);
  const auto Frozen = change(
      MultiBlockLoop.str(), " %test =", " %frozen = freeze i8 %next\n %test =");
  for (const auto &Text :
       {change(Frozen, "icmp eq i8 %next", "icmp eq i8 %frozen"),
        change(Frozen, "[ %next, %latch ]", "[ %frozen, %latch ]"),
        change(MultiBlockLoop.str(), "[ %next, %latch ]", "[ %start, %latch ]"),
        change(MultiBlockLoop.str(), "icmp eq i8 %next, 0",
               "icmp eq i8 %i, 0")})
    EXPECT_EQ(multiGraph(Text).Progress->Kind, FlowProgressKind::Unproven);
}

TEST(BinaryDiffMultiBlockProgress, RejectsBypassResetsNestedCyclesAndReentry) {
  auto Bypass = change(
      MultiBlockLoop.str(), " %a = xor i8 %v, 23\n br label %latch",
      " %a = xor i8 %v, 23\n br i1 %choose, label %header, label %latch");
  Bypass =
      change(Bypass, "[ %next, %latch ]", "[ %next, %latch ], [ 0, %left ]");
  Bypass =
      change(Bypass, "[ %value, %latch ]", "[ %value, %latch ], [ %a, %left ]");
  auto Nested =
      change(MultiBlockLoop.str(), " %b = mul i8 %v, 7\n br label %latch",
             " %b = mul i8 %v, 7\n br label %inner\ninner:\n br i1 %choose, "
             "label %inner, label %latch");
  Nested = change(Nested, "[ %b, %right ]", "[ %b, %inner ]");
  auto Reentry =
      change(MultiBlockLoop.str(), "ret i8 %value", "br label %header");
  Reentry =
      change(Reentry, "[ %next, %latch ]", "[ %next, %latch ], [ 0, %exit ]");
  Reentry =
      change(Reentry, "[ %value, %latch ]", "[ %value, %latch ], [ 0, %exit ]");
  auto Duplicate = change(MultiBlockLoop.str(), "label %exit, label %header",
                          "label %header, label %header");
  Duplicate = change(Duplicate, "[ %next, %latch ]",
                     "[ %next, %latch ], [ %next, %latch ]");
  Duplicate = change(Duplicate, "[ %value, %latch ]",
                     "[ %value, %latch ], [ %value, %latch ]");
  for (const auto &Text : {Bypass, Nested, Reentry, Duplicate}) {
    SCOPED_TRACE(Text);
    const auto G = multiGraph(Text);
    EXPECT_EQ(G.Progress->Kind, FlowProgressKind::Unproven);
    EXPECT_TRUE(G.Progress->Loops.empty());
    EXPECT_EQ(G.Progress->SourceContract, G.Progress->ObservedContract);
    EXPECT_EQ(readDataFlowGraph(json(G)), G);
  }
  EXPECT_EQ(multiGraph(R"(
define void @f(i1 %choose) {
entry: br i1 %choose, label %a, label %b
a: br label %b
b: br label %a
})")
                .Progress->Kind,
            FlowProgressKind::Unproven);
  EXPECT_EQ(multiGraph(R"(
define void @f() {
entry: ret void
a: br label %b
b: br label %a
})")
                .Progress->Kind,
            FlowProgressKind::Unproven);
}

TEST(BinaryDiffMultiBlockProgress, ProvesSequentialComponentsIndependently) {
  const auto G = multiGraph(R"(
define i8 @f(i8 %start) {
entry: br label %first
first:
 %a = phi i8 [ %start, %entry ], [ %x, %step1 ]
 br label %step1
step1:
 %x = add nsw i8 %a, 1
 %cx = icmp eq i8 %x, 5
 br i1 %cx, label %second, label %first
second:
 %b = phi i8 [ %x, %step1 ], [ %y, %step2 ]
 br label %step2
step2:
 %y = sub nuw i8 %b, 1
 %cy = icmp ne i8 %y, 0
 br i1 %cy, label %second, label %exit
exit: ret i8 %y
})");
  EXPECT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  ASSERT_EQ(G.Progress->Loops.size(), 2u);
  EXPECT_EQ(G.Progress->Loops[0].Header, 1u);
  EXPECT_EQ(G.Progress->Loops[1].Header, 3u);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
}

TEST(BinaryDiffMultiBlockProgress,
     RejectsForgedHeadersAndCrossVersionWitnesses) {
  const auto G = multiGraph();
  for (const auto Header : {0u, G.Progress->Loops[0].Block, NoIndex}) {
    auto Forged = G;
    Forged.Progress->Loops[0].Header = Header;
    EXPECT_TRUE(Forged.validate({}));
    EXPECT_THROW(readDataFlowGraph(json(Forged)), std::invalid_argument);
  }
  auto V = json(G);
  (*V.getAsObject())["schema_version"] = 5;
  EXPECT_THROW(readDataFlowGraph(V), std::invalid_argument);
  V = json(G);
  V.getAsObject()
      ->getObject("progress")
      ->getArray("loops")
      ->front()
      .getAsObject()
      ->erase("header");
  EXPECT_THROW(readDataFlowGraph(V), std::invalid_argument);
  V = json(graph(Loop));
  auto *L = V.getAsObject()
                ->getObject("progress")
                ->getArray("loops")
                ->front()
                .getAsObject();
  (*L)["header"] = *L->getInteger("block");
  EXPECT_THROW(readDataFlowGraph(V), std::invalid_argument);
  auto Legacy = graph(Loop);
  Legacy.Progress->Loops[0].Header = Legacy.Progress->Loops[0].Block;
  EXPECT_TRUE(Legacy.validate({}));
}

TEST(BinaryDiffMultiBlockProgress, SharesStorageWorkAndCooperativeStop) {
  const auto G = multiGraph();
  DataFlowOptions O;
  O.MaxNodes = G.retainedItems();
  EXPECT_EQ(multiGraph(MultiBlockLoop, O), G);
  --O.MaxNodes;
  EXPECT_THROW(multiGraph(MultiBlockLoop, O), DataFlowInterrupted);
  EXPECT_THROW(readDataFlowGraph(json(G), O), std::invalid_argument);
  uint64_t Work = 0;
  const auto P = proveFlowProgress(
      G, G.Progress->ObservedContract, {}, [&](uint64_t N) { Work += N; },
      FlowProgressMode::MultiBlock);
  EXPECT_EQ(P.Kind, G.Progress->Kind);
  EXPECT_EQ(P.Loops, G.Progress->Loops);
  for (const auto StopAt : {Work / 2, Work - 1}) {
    uint64_t Done = 0;
    EXPECT_THROW(proveFlowProgress(
                     G, G.Progress->ObservedContract, {},
                     [&](uint64_t N) {
                       Done += N;
                       if (Done >= StopAt)
                         throw DataFlowInterrupted(ComparisonState::Cancelled);
                     },
                     FlowProgressMode::MultiBlock),
                 DataFlowInterrupted);
    EXPECT_EQ(G.Progress->Loops, P.Loops);
  }
  O = {};
  O.MaxWork = 1;
  EXPECT_THROW(multiGraph(MultiBlockLoop, O), DataFlowInterrupted);
  uint64_t Low = 1, High = limits::diff::kDefaultFlowMaxWork;
  while (Low < High) {
    O.MaxWork = Low + (High - Low) / 2;
    try {
      multiGraph(MultiBlockLoop, O);
      High = O.MaxWork;
    } catch (const DataFlowInterrupted &E) {
      EXPECT_EQ(E.State, ComparisonState::ResourceLimited);
      Low = O.MaxWork + 1;
    }
  }
  O.MaxWork = Low;
  EXPECT_EQ(multiGraph(MultiBlockLoop, O), G);
  --O.MaxWork;
  EXPECT_THROW(multiGraph(MultiBlockLoop, O), DataFlowInterrupted);
  std::atomic<bool> Cancel{true};
  ComparisonControl C;
  C.Cancel = &Cancel;
  EXPECT_THROW(multiGraph(MultiBlockLoop, {}, C), DataFlowInterrupted);
}

TEST(BinaryDiffMultiBlockProgress, RejectsManyLatchesWithoutRepeatedSeedScans) {
  const auto Measure = [](unsigned Count) {
    std::string Text = "define void @f(i32 %which, i8 %start) {\nentry:\n "
                       "switch i32 %which, label %seed0 [\n";
    for (unsigned I = 1; I < Count; ++I)
      Text += "i32 " + std::to_string(I) + ", label %seed" + std::to_string(I) +
              "\n";
    Text += "]\n";
    for (unsigned I = 0; I < Count; ++I)
      Text += "seed" + std::to_string(I) + ": br label %header\n";
    Text += "header:\n %i = phi i8 ";
    for (unsigned I = 0; I < Count; ++I) {
      if (I)
        Text += ", ";
      Text += "[ %start, %seed" + std::to_string(I) + " ], [ %next" +
              std::to_string(I) + ", %latch" + std::to_string(I) + " ]";
    }
    Text += "\n switch i32 %which, label %latch0 [\n";
    for (unsigned I = 1; I < Count; ++I)
      Text += "i32 " + std::to_string(I) + ", label %latch" +
              std::to_string(I) + "\n";
    Text += "]\n";
    for (unsigned I = 0; I < Count; ++I) {
      const auto N = std::to_string(I);
      Text += "latch" + N + ":\n %next" + N + " = add nsw i8 %i, 1\n %test" +
              N + " = icmp eq i8 %next" + N + ", 0\n br i1 %test" + N +
              ", label %exit, label %header\n";
    }
    Text += "exit: ret void\n}";
    DataFlowOptions Options;
    Options.MaxBlocks = Count * 2 + 3;
    const auto G = multiGraph(Text, Options);
    EXPECT_EQ(G.Progress->Kind, FlowProgressKind::Unproven);
    uint64_t Work = 0;
    const auto P = proveFlowProgress(
        G, G.Progress->ObservedContract, {}, [&](uint64_t N) { Work += N; },
        FlowProgressMode::MultiBlock);
    EXPECT_EQ(P.Kind, FlowProgressKind::Unproven);
    return Work;
  };
  // Eight times as many outside seeds and candidate latches must not yield
  // their cross product in charged proof work.
  const auto Small = Measure(64), Large = Measure(512);
  EXPECT_GT(Large, Small);
  EXPECT_LT(Large, Small * 10);
}

namespace {
constexpr llvm::StringLiteral NestedLoop = R"(
define i8 @f(i8 %start, i8 %seed, i1 %choose) {
entry: br label %outer
outer:
 %i = phi i8 [ %start, %entry ], [ %next, %latch ]
 br label %inner
inner:
 %j = phi i8 [ %seed, %outer ], [ %step, %inner ]
 %step = add nsw i8 %j, -1
 %guard = icmp ne i8 %step, 0
 br i1 %guard, label %inner, label %latch
latch:
 %next = add nsw i8 %i, -1
 %test = icmp eq i8 %next, 0
 br i1 %test, label %exit, label %outer
exit: ret i8 %next
})";

DataFlowGraph nestedGraph(llvm::StringRef Text = NestedLoop,
                          const DataFlowOptions &Options = {},
                          const ComparisonControl &Control = {}) {
  return graph(Text, Options, Control, false, false, true);
}

std::string nestedCounters(unsigned Depth) {
  std::string Text = "define i32 @f(i32 %start) {\nentry: br label %h0\n";
  for (unsigned I = 0; I < Depth; ++I) {
    const auto N = std::to_string(I);
    const auto Previous = I ? std::to_string(I - 1) : std::string();
    Text += "h" + N + ":\n %i" + N + " = phi i32 [ " +
            (I ? "%i" + Previous + ", %h" + Previous : "%start, %entry") +
            " ], [ %up" + N + ", %l" + N + " ]\n br label %" +
            (I + 1 == Depth ? "l" + N : "h" + std::to_string(I + 1)) + "\n";
  }
  for (unsigned I = Depth; I-- > 0;) {
    const auto N = std::to_string(I);
    Text += "l" + N + ":\n %up" + N + " = add nsw i32 %i" + N + ", -1\n %cmp" +
            N + " = icmp eq i32 %up" + N + ", 0\n br i1 %cmp" + N +
            ", label %" + (I ? "l" + std::to_string(I - 1) : "exit") +
            ", label %h" + N + "\n";
  }
  return Text + "exit: ret i32 %up0\n}";
}
} // namespace

TEST(BinaryDiffNestedProgress,
     ProvesReseededInnerCounterWithoutChangingLegacy) {
  for (const auto &Text :
       {NestedLoop.str(),
        change(NestedLoop.str(), "[ %seed, %outer ]", "[ %i, %outer ]")}) {
    const auto G = nestedGraph(Text);
    ASSERT_TRUE(G.Progress);
    EXPECT_EQ(G.Profile, LLVMNestedProgressDataFlowProfile);
    EXPECT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
    ASSERT_EQ(G.Progress->Loops.size(), 2u);
    // Outer is proved first, but paged witnesses remain ordered by latch.
    EXPECT_EQ(G.Progress->Loops[0].Block, 2u);
    EXPECT_EQ(G.Progress->Loops[0].Header, 2u);
    EXPECT_EQ(G.Progress->Loops[1].Block, 3u);
    EXPECT_EQ(G.Progress->Loops[1].Header, 1u);
    EXPECT_NE(G.Progress->SourceContract, G.Progress->ObservedContract);
    EXPECT_EQ(readDataFlowGraph(json(G)), G);
    EXPECT_EQ(graph(Text).Progress->Kind, FlowProgressKind::Unproven);
    EXPECT_EQ(multiGraph(Text).Progress->Kind, FlowProgressKind::Unproven);
  }
  EXPECT_EQ(nestedGraph("define void @f() { ret void }").Progress->Kind,
            FlowProgressKind::Acyclic);
  EXPECT_EQ(nestedGraph(Loop).Progress->Loops.size(), 1u);
  EXPECT_EQ(nestedGraph(MultiBlockLoop).Progress->Loops.size(), 1u);
}

TEST(BinaryDiffNestedProgress,
     ProvesHeaderUpdatesEarlyExitsAndSequentialInnerLoops) {
  auto Hoisted = change(NestedLoop.str(), " %next = add nsw i8 %i, -1\n", "");
  Hoisted = change(Hoisted, " br label %inner",
                   " %next = add nsw i8 %i, -1\n br label %inner");
  const auto Early = change(Hoisted, " br label %inner",
                            " br i1 %choose, label %exit, label %inner");
  auto InnerEarly =
      change(NestedLoop.str(), "[ %step, %inner ]", "[ %step, %inner_latch ]");
  InnerEarly = change(InnerEarly, " %step =",
                      " br i1 %choose, label %latch, label %inner_latch\n"
                      "inner_latch:\n %step =");
  auto Sequential =
      change(NestedLoop.str(), "br i1 %guard, label %inner, label %latch",
             "br i1 %guard, label %inner, label %second\n"
             "second:\n"
             " %k = phi i8 [ %step, %inner ], [ %more, %second ]\n"
             " %more = sub nuw i8 %k, 1\n"
             " %done = icmp eq i8 %more, 0\n"
             " br i1 %done, label %latch, label %second");
  for (const auto &Text : {Hoisted, Early, InnerEarly, Sequential}) {
    SCOPED_TRACE(Text);
    const auto G = nestedGraph(Text);
    EXPECT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
    EXPECT_EQ(G.Progress->Loops.size(), Text == Sequential ? 3u : 2u);
    EXPECT_EQ(readDataFlowGraph(json(G)), G);
  }
}

TEST(BinaryDiffNestedProgress, KeepsProofIndependentOfNestedBlockLayout) {
  const auto Text = NestedLoop.str();
  const auto Outer = Text.find("outer:\n"), Inner = Text.find("inner:\n"),
             Latch = Text.find("latch:\n"), Exit = Text.find("exit:");
  const auto Reordered = Text.substr(0, Outer) +
                         Text.substr(Latch, Exit - Latch) +
                         Text.substr(Inner, Latch - Inner) +
                         Text.substr(Outer, Inner - Outer) + Text.substr(Exit);
  const auto G = nestedGraph(Reordered);
  ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  ASSERT_EQ(G.Progress->Loops.size(), 2u);
  EXPECT_EQ(G.Progress->Loops[0].Block, 1u);
  EXPECT_EQ(G.Progress->Loops[0].Header, 3u);
  EXPECT_EQ(G.Progress->Loops[1].Block, 2u);
  EXPECT_EQ(G.Progress->Loops[1].Header, 2u);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
}

TEST(BinaryDiffNestedProgress,
     DiscardsEveryWitnessWhenAnyLayerRemainsUnproven) {
  const auto Frozen = change(
      NestedLoop.str(), " %guard =", " %frozen = freeze i8 %step\n %guard =");
  for (const auto &Text :
       {change(NestedLoop.str(), "add nsw i8 %i, -1", "add i8 %i, -1"),
        change(NestedLoop.str(), "add nsw i8 %j, -1", "add i8 %j, -1"),
        change(NestedLoop.str(), "add nsw i8 %j, -1", "add nsw i8 %j, 0"),
        change(NestedLoop.str(), "[ %step, %inner ]", "[ %seed, %inner ]"),
        change(Frozen, "icmp ne i8 %step, 0", "icmp ne i8 %frozen, 0"),
        change(NestedLoop.str(), "br i1 %guard, label %inner, label %latch",
               "br i1 %choose, label %inner, label %latch"),
        change(NestedLoop.str(), "exit: ret i8 %next",
               "exit: ret i8 %next\nunreachable_cycle: br label "
               "%unreachable_cycle")}) {
    SCOPED_TRACE(Text);
    for (const auto &Body :
         {Text, change(Text, "i1 %choose) {",
                       "i1 %choose) mustprogress willreturn {")}) {
      const auto G = nestedGraph(Body);
      EXPECT_EQ(G.Progress->Kind, FlowProgressKind::Unproven);
      EXPECT_TRUE(G.Progress->Loops.empty());
      EXPECT_EQ(G.Progress->SourceContract, G.Progress->ObservedContract);
      EXPECT_EQ(readDataFlowGraph(json(G)), G);
    }
  }
}

TEST(BinaryDiffNestedProgress,
     RefusesOuterResetsDuplicateEdgesAndIrreducibleEntries) {
  auto Reset = change(NestedLoop.str(), "[ %next, %latch ]",
                      "[ %next, %latch ], [ %start, %reset ]");
  Reset = change(Reset, "br i1 %guard, label %inner, label %latch",
                 "br i1 %guard, label %inner, label %check\ncheck:\n"
                 " br i1 %choose, label %reset, label %latch\n"
                 "reset: br label %outer");
  auto Duplicate = change(NestedLoop.str(), "label %exit, label %outer",
                          "label %outer, label %outer");
  Duplicate = change(Duplicate, "[ %next, %latch ]",
                     "[ %next, %latch ], [ %next, %latch ]");
  for (const auto &Text : {Reset, Duplicate, std::string(R"(
define i8 @f(i8 %start, i1 %choose) {
entry: br i1 %choose, label %a, label %b
a:
 %i = phi i8 [ %start, %entry ], [ %up, %a ], [ 0, %b ]
 %up = add nsw i8 %i, 1
 %ci = icmp eq i8 %up, 0
 br i1 %ci, label %b, label %a
b:
 %j = phi i8 [ %start, %entry ], [ %down, %b ], [ 0, %a ]
 %down = sub nuw i8 %j, 1
 %cj = icmp eq i8 %down, 0
 br i1 %cj, label %a, label %b
})")}) {
    const auto G = nestedGraph(Text);
    EXPECT_EQ(G.Progress->Kind, FlowProgressKind::Unproven);
    EXPECT_TRUE(G.Progress->Loops.empty());
  }
}

TEST(BinaryDiffNestedProgress, ValidatesAllLayersFromScratchAndBindsSchema) {
  const auto G = nestedGraph(nestedCounters(3));
  ASSERT_EQ(G.Progress->Loops.size(), 3u);
  for (unsigned I = 0; I < G.Progress->Loops.size(); ++I) {
    auto Forged = G;
    Forged.Progress->Loops[I].Header = G.Progress->Loops[I].Block;
    EXPECT_TRUE(Forged.validate({}));
    EXPECT_THROW(readDataFlowGraph(json(Forged)), std::invalid_argument);
    Forged = G;
    Forged.Progress->Loops.erase(Forged.Progress->Loops.begin() + I);
    EXPECT_TRUE(Forged.validate({}));
  }
  auto Reordered = G;
  std::swap(Reordered.Progress->Loops.front(),
            Reordered.Progress->Loops.back());
  EXPECT_TRUE(Reordered.validate({}));
  auto V = json(G);
  EXPECT_EQ(V.getAsObject()->getInteger("schema_version"), 7);
  (*V.getAsObject())["schema_version"] = 6;
  EXPECT_THROW(readDataFlowGraph(V), std::invalid_argument);
  auto Old = G;
  Old.Profile = LLVMMultiBlockProgressDataFlowProfile.str();
  EXPECT_TRUE(Old.validate({}));
  EXPECT_THROW(readDataFlowGraph(json(Old)), std::invalid_argument);
  V = json(G);
  V.getAsObject()
      ->getObject("progress")
      ->getArray("loops")
      ->front()
      .getAsObject()
      ->erase("header");
  EXPECT_THROW(readDataFlowGraph(V), std::invalid_argument);
}

TEST(BinaryDiffNestedProgress, KeepsNestedTraversalBoundedAndInterruptible) {
  const auto G = nestedGraph(nestedCounters(64));
  ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(G.Progress->Loops.size(), 64u);
  uint64_t OneRound = 0, AllRounds = 0;
  const auto One = proveFlowProgress(
      G, G.Progress->ObservedContract, {}, [&](uint64_t N) { OneRound += N; },
      FlowProgressMode::MultiBlock);
  EXPECT_EQ(One.Kind, FlowProgressKind::Unproven);
  const auto All = proveFlowProgress(
      G, G.Progress->ObservedContract, {}, [&](uint64_t N) { AllRounds += N; },
      FlowProgressMode::Nested);
  EXPECT_EQ(All.Loops, G.Progress->Loops);
  EXPECT_GT(AllRounds, OneRound);
  for (const auto StopAt : {OneRound + 1, AllRounds - 1}) {
    uint64_t Work = 0;
    EXPECT_THROW(proveFlowProgress(
                     G, G.Progress->ObservedContract, {},
                     [&](uint64_t N) {
                       Work += N;
                       if (Work >= StopAt)
                         throw DataFlowInterrupted(ComparisonState::Cancelled);
                     },
                     FlowProgressMode::Nested),
                 DataFlowInterrupted);
    EXPECT_EQ(G.Progress->Loops, All.Loops);
  }
  DataFlowOptions O;
  O.MaxWork = OneRound;
  EXPECT_THROW(nestedGraph(nestedCounters(64), O), DataFlowInterrupted);
  std::atomic<bool> Cancel{true};
  ComparisonControl C;
  C.Cancel = &Cancel;
  EXPECT_THROW(nestedGraph(NestedLoop, {}, C), DataFlowInterrupted);
}

TEST(BinaryDiffNestedProgress, SharesExactRetainedAndProjectionAllowances) {
  const auto G = nestedGraph();
  DataFlowOptions O;
  O.MaxNodes = G.retainedItems();
  EXPECT_EQ(nestedGraph(NestedLoop, O), G);
  --O.MaxNodes;
  EXPECT_THROW(nestedGraph(NestedLoop, O), DataFlowInterrupted);
  EXPECT_THROW(readDataFlowGraph(json(G), O), std::invalid_argument);
  O = {};
  uint64_t Low = 1, High = limits::diff::kDefaultFlowMaxWork;
  while (Low < High) {
    O.MaxWork = Low + (High - Low) / 2;
    try {
      nestedGraph(NestedLoop, O);
      High = O.MaxWork;
    } catch (const DataFlowInterrupted &E) {
      EXPECT_EQ(E.State, ComparisonState::ResourceLimited);
      Low = O.MaxWork + 1;
    }
  }
  O.MaxWork = Low;
  EXPECT_EQ(nestedGraph(NestedLoop, O), G);
  --O.MaxWork;
  EXPECT_THROW(nestedGraph(NestedLoop, O), DataFlowInterrupted);
}
