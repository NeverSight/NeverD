//===- BinaryDiffProgressTests.cpp - Source-bound loop progress -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Finite execution is distinct from matching loop bounds or program results.
/// Exercise real LLVM projections, adversarial cycles and saved graph proofs.
//===----------------------------------------------------------------------===//

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
                    const ComparisonControl &Control = {},
                    bool Legacy = false) {
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
  const auto Build =
      Legacy ? buildLLVMParameterDataFlow : buildLLVMProgressDataFlow;
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
