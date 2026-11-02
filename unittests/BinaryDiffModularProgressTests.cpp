//===- BinaryDiffModularProgressTests.cpp - Integer cycle exits -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check modular orbit coverage against concrete small integers, LLVM guards,
/// adversarial cycles and authenticated replay without freezing source values.
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
#include "llvm/IR/InstrTypes.h"
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
constexpr llvm::StringLiteral Loop = R"(
define i8 @f(i8 %start) {
entry: br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %next, %header ]
 %next = add i8 %i, 3
 %test = icmp ne i8 %next, 0
 br i1 %test, label %header, label %exit
exit: ret i8 %next
})";
constexpr llvm::StringLiteral Funnel = R"(
define i8 @f(i8 %start, i1 %choose, i8 %bound) {
entry: br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %next, %latch ]
 br i1 %choose, label %left, label %right
left:
 %a = add i8 %i, 0
 %ca = icmp ne i8 %a, 0
 br i1 %ca, label %latch, label %exit
right:
 %b = add i8 %i, -2
 %cb = icmp ult i8 %b, 127
 br i1 %cb, label %latch, label %exit
latch:
 %next = add i8 %i, -1
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
 %a = add i8 %i, 3
 %ca = icmp ne i8 %a, 0
 br i1 %ca, label %latch, label %exit
right:
 %b = sub i8 %i, -3
 %cb = icmp ne i8 %b, 0
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
    throw std::runtime_error("missing modular fixture replacement");
  Text.replace(At, From.size(), To.str());
  return Text;
}
std::string width(std::string Text, unsigned Bits) {
  const auto Type = "i" + std::to_string(Bits);
  for (size_t At = 0; (At = Text.find("i8", At)) != std::string::npos;
       At += Type.size())
    Text.replace(At, 2, Type);
  return Text;
}
DataFlowGraph graph(llvm::StringRef Text = Loop,
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
      Old ? buildLLVMBranchProgressDataFlow : buildLLVMModularProgressDataFlow;
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
  EXPECT_EQ(G.Progress->SourceContract, G.Progress->ObservedContract);
  EXPECT_TRUE(G.Progress->Loops.empty());
  EXPECT_EQ(G.Progress->LoopCount, 0u);
  EXPECT_EQ(G.Progress->BackedgeCount, 0u);
}
void proved(llvm::StringRef Text,
            FlowCounterProof Proof = FlowCounterProof::Modular) {
  const auto G = graph(Text);
  ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  ASSERT_FALSE(G.Progress->Loops.empty());
  for (const auto &L : G.Progress->Loops)
    EXPECT_EQ(L.Proof, Proof);
}
std::string twoRanges(unsigned A, unsigned B, unsigned C, unsigned D,
                      unsigned Bits = 4, unsigned Step = 4) {
  auto Text = replace(Funnel.str(), "add i8 %i, -1",
                      "add i8 %i, " + std::to_string(Step));
  Text = replace(Text, "add i8 %i, 0", "sub i8 %i, " + std::to_string(A));
  Text =
      replace(Text, "icmp ne i8 %a, 0", "icmp ult i8 %a, " + std::to_string(B));
  Text = replace(Text, "add i8 %i, -2", "sub i8 %i, " + std::to_string(C));
  Text = replace(Text, "icmp ult i8 %b, 127",
                 "icmp ult i8 %b, " + std::to_string(D));
  return width(Text, Bits);
}
} // namespace

TEST(BinaryDiffModularProgress, ProvesRealCounterAndAffineObservationShapes) {
  for (const auto Text : {Loop, Funnel, Selected}) {
    proved(Text);
    EXPECT_EQ(graph(Text, {}, {}, true).Progress->Kind,
              FlowProgressKind::Unproven);
  }
  const auto G = graph(Funnel);
  EXPECT_EQ(G.Profile, LLVMModularProgressDataFlowProfile);
  EXPECT_EQ(dataFlowSchemaVersion(G.Profile), 9u);
  EXPECT_EQ(G.Progress->LoopCount, 1u);
  EXPECT_EQ(G.Progress->BackedgeCount, 1u);
  EXPECT_EQ(G.Progress->Loops.size(), 2u);
  auto Text = replace(Loop.str(), "icmp ne i8 %next, 0", "icmp ne i8 %i, -3");
  proved(Text);
  proved(replace(Text, "add i8 %i, 3", "sub i8 %i, -3"));
  proved(replace(Text, "add i8 %i, 3", "add i8 3, %i"));
  auto Direct = replace(Selected.str(), "[ %next, %latch ]",
                        "[ %a, %left ], [ %b, %right ]");
  Direct = replace(Direct, "label %latch, label %exit",
                   "label %header, label %exit");
  Direct = replace(Direct, "label %latch, label %exit",
                   "label %header, label %exit");
  Direct = replace(Direct,
                   "latch:\n %next = phi i8 [ %a, %left ], [ %b, %right ]\n br "
                   "label %header\n",
                   "");
  const auto Branched = graph(Direct);
  ASSERT_EQ(Branched.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(Branched.Progress->LoopCount, 1u);
  EXPECT_EQ(Branched.Progress->BackedgeCount, 2u);
  for (const auto &L : Branched.Progress->Loops)
    EXPECT_EQ(L.Proof, FlowCounterProof::Modular);
}

TEST(BinaryDiffModularProgress, RequiresEveryResidueAndACommonExit) {
  auto Text = replace(Loop.str(), "add i8 %i, 3", "sub i8 %i, 2");
  unproved(Text); // Odd seeds never reach the single even exit.
  proved(replace(Text, "icmp ne i8 %next, 0", "icmp ugt i8 %next, 1"));
  proved(twoRanges(2, 4, 8, 8));   // Exit residues {0,1} and {2,3}.
  unproved(twoRanges(1, 5, 8, 8)); // Missing residue 1.
  proved(twoRanges(5, 4, 11, 8)); // [3,5) projects across the residue boundary.
  unproved(twoRanges(5, 4, 11, 9)); // Missing residue 3.
  unproved(replace(Selected.str(), "icmp ne i8 %b, 0", "icmp ne i8 %i, 0"));
  unproved(replace(Selected.str(), "sub i8 %i, -3", "sub i8 %i, 3"));
  unproved(replace(Loop.str(), "add i8 %i, 3", "add i8 %i, 0"));
  unproved(replace(Loop.str(), "icmp ne i8 %next, 0", "icmp uge i8 %next, 0"));
  proved(replace(Loop.str(), "icmp ne i8 %next, 0", "icmp ult i8 %next, 0"));
}

TEST(BinaryDiffModularProgress, PreservesUndefPoisonAndExternallyFrozenSeeds) {
  for (llvm::StringRef Seed : {"undef", "poison", "0", "-128"})
    proved(replace(Loop.str(), "[ %start, %entry ]",
                   "[ " + Seed.str() + ", %entry ]"));
  for (llvm::StringRef Seed : {"and i8 undef, 7", "freeze i8 %start"}) {
    auto Text =
        replace(Loop.str(), "entry: br label %header",
                "entry:\n %seed = " + Seed.str() + "\n br label %header");
    proved(replace(Text, "[ %start, %entry ]", "[ %seed, %entry ]"));
  }
  // No noundef is invented on ordinary function arguments.
  const auto G = graph();
  ASSERT_EQ(G.ParameterViews.size(), 1u);
  EXPECT_TRUE(G.ParameterViews.front().Attributes.empty());
}

TEST(BinaryDiffModularProgress, KeepsNowrapPriorityAndNonliteralBounds) {
  proved(replace(Loop.str(), "add i8 %i, 3", "add nsw i8 %i, 3"),
         FlowCounterProof::NoWrap);
  auto Text = replace(Funnel.str(), "add i8 %i, -1", "add nsw i8 %i, -1");
  Text = replace(Text, "icmp ult i8 %b, 127", "icmp ult i8 %b, %bound");
  proved(Text, FlowCounterProof::NoWrap);
  Text = replace(Selected.str(), "add i8 %i, 3", "add nsw i8 %i, 3");
  Text = replace(Text, "sub i8 %i, -3", "sub nsw i8 %i, -1");
  proved(Text, FlowCounterProof::NoWrap);
  unproved(
      replace(Funnel.str(), "icmp ult i8 %b, 127", "icmp ult i8 %b, %bound"));
}

TEST(BinaryDiffModularProgress, RejectsFreezeResetAndUnsupportedObservations) {
  auto Frozen =
      replace(Loop.str(), " %test =", " %frozen = freeze i8 %next\n %test =");
  unproved(replace(Frozen, "icmp ne i8 %next, 0", "icmp ne i8 %frozen, 0"));
  Frozen = replace(Loop.str(), " br i1 %test,",
                   " %frozen = freeze i1 %test\n br i1 %frozen,");
  unproved(Frozen);
  Frozen =
      replace(Loop.str(), " %test =", " %frozen = freeze i8 %next\n %test =");
  unproved(replace(Frozen, "[ %next, %header ]", "[ %frozen, %header ]"));
  unproved(replace(Loop.str(), "[ %next, %header ]", "[ %start, %header ]"));
  unproved(replace(Funnel.str(), "add i8 %i, -2", "sub i8 -2, %i"));
  EXPECT_THROW(graph(replace(Selected.str(), "[ %a, %left ], [ %b, %right ]",
                             "[ %b, %left ], [ %a, %right ]")),
               LLVMDataFlowUnsupported);
  auto Crossed = graph(Selected);
  const auto Latch = Crossed.Progress->Loops.front().Latch;
  auto Merge = std::find_if(
      Crossed.Nodes.begin(), Crossed.Nodes.end(),
      [&](const FlowNode &N) { return N.Phi && N.Block == Latch; });
  ASSERT_NE(Merge, Crossed.Nodes.end());
  std::swap(Merge->Inputs[0].Node, Merge->Inputs[1].Node);
  EXPECT_TRUE(Crossed.validate({}));
  auto G = graph(Selected);
  const auto Counter = G.Progress->Loops.front().Counter;
  G.Nodes[Counter].Inputs.pop_back();
  EXPECT_TRUE(G.validate({}));
}

TEST(BinaryDiffModularProgress, HandlesWholeWidthEndpointsWithoutOverflow) {
  proved(width(Loop.str(), 1));
  proved(width(Loop.str(), 64));
  auto Text = width(Loop.str(), 64);
  Text = replace(Text, "add i64 %i, 3", "add i64 %i, -9223372036854775808");
  unproved(Text);
  proved(replace(Text, "icmp ne i64 %next, 0", "icmp slt i64 %next, 0"));
  proved(replace(Text, "icmp ne i64 %next, 0",
                 "icmp uge i64 %next, 9223372036854775808"));
  Text = replace(width(Loop.str(), 64), "icmp ne i64 %next, 0",
                 "icmp ne i64 %i, 18446744073709551615");
  proved(Text);
  auto Affine = replace(Loop.str(), "add i8 %i, 3", "add i8 %i, 2");
  Affine = replace(Affine, " %test =", " %observed = add i8 %i, 4\n %test =");
  Affine =
      replace(Affine, "icmp ne i8 %next, 0", "icmp uge i8 %observed, %next");
  proved(Affine);
  proved(width(Affine, 64));
  for (llvm::StringRef Seed : {"undef", "poison"})
    proved(replace(Affine, "[ %start, %entry ]",
                   "[ " + Seed.str() + ", %entry ]"));
  unproved(replace(Affine, "icmp uge i8 %observed, %next",
                   "icmp uge i8 %observed, %start"));
  auto Frozen =
      replace(Affine, " %test =", " %frozen = freeze i8 %next\n %test =");
  unproved(replace(Frozen, "icmp uge i8 %observed, %next",
                   "icmp uge i8 %observed, %frozen"));
  auto Foreign =
      replace(Affine, " %next =",
              " %other = phi i8 [ 0, %entry ], [ %othernext, %header ]\n"
              " %othernext = add i8 %other, 2\n %next =");
  unproved(replace(Foreign, "icmp uge i8 %observed, %next",
                   "icmp uge i8 %observed, %othernext"));
}

TEST(BinaryDiffModularProgress,
     RepeatsMixedProofGroupsAndDiscardsResidualCycles) {
  constexpr llvm::StringLiteral Nested = R"(
define i8 @f(i8 %seed) {
entry: br label %outer
outer:
 %o = phi i8 [ 0, %entry ], [ %onext, %latch ]
 br label %inner
inner:
 %i = phi i8 [ %seed, %outer ], [ %inext, %inner ]
 %inext = add i8 %i, 3
 %ic = icmp ne i8 %inext, 0
 br i1 %ic, label %inner, label %latch
latch:
 %onext = add nsw i8 %o, 1
 %oc = icmp ne i8 %onext, 0
 br i1 %oc, label %outer, label %exit
exit: ret i8 %onext
})";
  const auto G = graph(Nested);
  ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(G.Progress->LoopCount, 2u);
  EXPECT_EQ(G.Progress->BackedgeCount, 2u);
  ASSERT_EQ(G.Progress->Loops.size(), 2u);
  EXPECT_NE(G.Progress->Loops[0].Proof, G.Progress->Loops[1].Proof);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
  unproved(replace(Nested.str(), "add i8 %i, 3", "add i8 %i, 2"));
}

TEST(BinaryDiffModularProgress,
     RecomputesCanonicalProofAndPreservesOldEncoding) {
  const auto G = graph(Funnel);
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
  for (const auto &L : G.Progress->Loops) {
    auto Page = dataFlowNodeJSON(G, G.Blocks[L.Block].Terminator);
    EXPECT_EQ(Page.getObject("progress_loop")->getString("proof"), "modular");
  }
  for (llvm::StringRef Proof : {"none", "nowrap", "unsupported"}) {
    auto J = json(G);
    auto *P = J.getAsObject()->getObject("progress");
    (*P->getArray("loops"))[0].getAsObject()->operator[]("proof") = Proof;
    EXPECT_THROW(readDataFlowGraph(J), std::invalid_argument);
  }
  auto J = json(G);
  J.getAsObject()
      ->getObject("progress")
      ->getArray("loops")
      ->front()
      .getAsObject()
      ->erase("proof");
  EXPECT_THROW(readDataFlowGraph(J), std::invalid_argument);
  auto Preferred =
      json(graph(replace(Loop.str(), "add i8 %i, 3", "add nsw i8 %i, 3")));
  auto *Witness = Preferred.getAsObject()
                      ->getObject("progress")
                      ->getArray("loops")
                      ->front()
                      .getAsObject();
  EXPECT_EQ(Witness->getString("proof"), "nowrap");
  (*Witness)["proof"] = "modular";
  EXPECT_THROW(readDataFlowGraph(Preferred), std::invalid_argument);
  const auto Old = graph(
      replace(Loop.str(), "add i8 %i, 3", "add nsw i8 %i, 3"), {}, {}, true);
  auto OldJSON = json(Old);
  EXPECT_FALSE(OldJSON.getAsObject()
                   ->getObject("progress")
                   ->getArray("loops")
                   ->front()
                   .getAsObject()
                   ->get("proof"));
  EXPECT_EQ(readDataFlowGraph(OldJSON), Old);
}

namespace {
void checkConcreteOrbits(bool TwoAffine) {
  // The oracle enumerates concrete states and compares integers directly.
  // It shares neither interval construction nor residue projection with proof.
  for (unsigned Bits = 1; Bits <= 4; ++Bits) {
    const unsigned Size = 1u << Bits, Mask = Size - 1;
    auto Text =
        replace(Loop.str(), " %test =", " %observed = add i8 %i, 0\n %test =");
    Text = replace(Text, "icmp ne i8 %next, 0", "icmp ne i8 %observed, 0");
    if (TwoAffine) {
      Text = replace(Text, " %test =", " %bound = add i8 %i, 3\n %test =");
      Text = replace(Text, "icmp ne i8 %observed, 0",
                     "icmp ult i8 %observed, %bound");
    }
    auto G = graph(width(Text, Bits));
    ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
    const auto Loop = G.Progress->Loops.front();
    const auto Observed = G.Nodes[Loop.Compare].Inputs[0].Node;
    const auto Other = G.Nodes[Loop.Compare].Inputs[1].Node;
    const auto Semantics =
        G.Progress->ObservedContract + ";result=i1;flags=0;predicate=";
    const auto Signed = [&](unsigned V) {
      return int(V) - (V >= Size / 2 ? int(Size) : 0);
    };
    for (unsigned Pred = llvm::CmpInst::FIRST_ICMP_PREDICATE;
         Pred <= llvm::CmpInst::LAST_ICMP_PREDICATE; ++Pred)
      for (unsigned Step = 1; Step < Size; ++Step)
        for (unsigned Offset = 0; Offset < Size; ++Offset)
          for (unsigned Bound = 0; Bound < Size; ++Bound) {
            G.Nodes[Loop.Update].Inputs[1].Value = Step;
            G.Nodes[Observed].Inputs[1].Value = Offset;
            if (TwoAffine)
              G.Nodes[Other].Inputs[1].Value = Bound;
            else
              G.Nodes[Loop.Compare].Inputs[1].Value = Bound;
            G.Nodes[Loop.Compare].Semantics = Semantics + std::to_string(Pred);
            const auto Compare = [&](unsigned A, unsigned B) {
              switch (Pred) {
              case llvm::CmpInst::ICMP_EQ:
                return A == B;
              case llvm::CmpInst::ICMP_NE:
                return A != B;
              case llvm::CmpInst::ICMP_UGT:
                return A > B;
              case llvm::CmpInst::ICMP_UGE:
                return A >= B;
              case llvm::CmpInst::ICMP_ULT:
                return A < B;
              case llvm::CmpInst::ICMP_ULE:
                return A <= B;
              case llvm::CmpInst::ICMP_SGT:
                return Signed(A) > Signed(B);
              case llvm::CmpInst::ICMP_SGE:
                return Signed(A) >= Signed(B);
              case llvm::CmpInst::ICMP_SLT:
                return Signed(A) < Signed(B);
              case llvm::CmpInst::ICMP_SLE:
                return Signed(A) <= Signed(B);
              default:
                throw std::runtime_error("unexpected predicate");
              }
            };
            for (bool Reverse : {false, true}) {
              if (Reverse)
                std::swap(G.Nodes[Loop.Compare].Inputs[0],
                          G.Nodes[Loop.Compare].Inputs[1]);
              for (bool Invert : {false, true}) {
                if (Invert)
                  std::swap(G.Blocks[Loop.Block].Successors[0],
                            G.Blocks[Loop.Block].Successors[1]);
                bool AllExit = true;
                for (unsigned Seed = 0; Seed < Size; ++Seed) {
                  unsigned Value = Seed, Visits = 0;
                  for (; Visits < Size; ++Visits) {
                    const auto Observation = (Value + Offset) & Mask;
                    const auto Limit =
                        TwoAffine ? (Value + Bound) & Mask : Bound;
                    const bool Condition = Reverse
                                               ? Compare(Limit, Observation)
                                               : Compare(Observation, Limit);
                    if (Condition == Invert)
                      break;
                    Value = (Value + Step) & Mask;
                  }
                  AllExit &= Visits < Size;
                }
                const auto P = proveFlowProgress(
                    G, G.Progress->ObservedContract, {}, [](uint64_t) {},
                    FlowProgressMode::Modular);
                ASSERT_EQ(P.Kind == FlowProgressKind::CounterLoops, AllExit)
                    << TwoAffine << ' ' << Bits << ' ' << Pred << ' ' << Step
                    << ' ' << Offset << ' ' << Bound << ' ' << Reverse << ' '
                    << Invert;
                if (Invert)
                  std::swap(G.Blocks[Loop.Block].Successors[0],
                            G.Blocks[Loop.Block].Successors[1]);
              }
              if (Reverse)
                std::swap(G.Nodes[Loop.Compare].Inputs[0],
                          G.Nodes[Loop.Compare].Inputs[1]);
            }
          }
  }
}
} // namespace

TEST(BinaryDiffModularProgress, MatchesExhaustiveSmallIntegerOrbits) {
  checkConcreteOrbits(false);
}

TEST(BinaryDiffModularProgress, MatchesExhaustiveSameCounterObservations) {
  checkConcreteOrbits(true);
}

TEST(BinaryDiffModularProgress, MatchesExhaustiveTwoGuardChoices) {
  // Either guard may continue on each visit. Independently enumerate whether
  // any orbit can always pick a continuing guard, including wrapped ranges.
  constexpr unsigned Bits = 3, Size = 1u << Bits, Mask = Size - 1;
  auto Text = replace(
      twoRanges(0, 1, 0, 1, Bits, 1),
      " br i1 %choose, label %left, label %right",
      " %pick = freeze i1 undef\n br i1 %pick, label %left, label %right");
  auto G = graph(Text);
  ASSERT_EQ(G.Progress->Loops.size(), 2u);
  const auto Left = G.Progress->Loops[0], Right = G.Progress->Loops[1];
  const auto A = G.Nodes[Left.Compare].Inputs[0].Node;
  const auto B = G.Nodes[Right.Compare].Inputs[0].Node;
  for (unsigned Step = 1; Step < Size; ++Step)
    for (unsigned OffsetA = 0; OffsetA < Size; ++OffsetA)
      for (unsigned OffsetB = 0; OffsetB < Size; ++OffsetB)
        for (unsigned BoundA = 0; BoundA < Size; ++BoundA)
          for (unsigned BoundB = 0; BoundB < Size; ++BoundB) {
            G.Nodes[Left.Update].Inputs[1].Value = Step;
            G.Nodes[A].Inputs[1].Value = OffsetA;
            G.Nodes[B].Inputs[1].Value = OffsetB;
            G.Nodes[Left.Compare].Inputs[1].Value = BoundA;
            G.Nodes[Right.Compare].Inputs[1].Value = BoundB;
            bool AllExit = true;
            for (unsigned Seed = 0; Seed < Size; ++Seed) {
              unsigned Value = Seed, Visits = 0;
              for (; Visits < Size; ++Visits) {
                if (((Value - OffsetA) & Mask) >= BoundA &&
                    ((Value - OffsetB) & Mask) >= BoundB)
                  break;
                Value = (Value + Step) & Mask;
              }
              AllExit &= Visits < Size;
            }
            const auto P = proveFlowProgress(
                G, G.Progress->ObservedContract, {}, [](uint64_t) {},
                FlowProgressMode::Modular);
            ASSERT_EQ(P.Kind == FlowProgressKind::CounterLoops, AllExit)
                << Step << ' ' << OffsetA << ' ' << OffsetB << ' ' << BoundA
                << ' ' << BoundB;
          }
}

TEST(BinaryDiffModularProgress,
     SharesExactStorageWorkAndEveryCancellationPoint) {
  const auto Text = twoRanges(5, 4, 11, 8);
  const auto G = graph(Text);
  DataFlowOptions O;
  O.MaxNodes = G.retainedItems();
  EXPECT_EQ(graph(Text, O), G);
  --O.MaxNodes;
  EXPECT_THROW(graph(Text, O), DataFlowInterrupted);
  O = {};
  uint64_t Low = 1, High = limits::diff::kDefaultFlowMaxWork;
  while (Low < High) {
    O.MaxWork = Low + (High - Low) / 2;
    try {
      graph(Text, O);
      High = O.MaxWork;
    } catch (const DataFlowInterrupted &E) {
      EXPECT_EQ(E.State, ComparisonState::ResourceLimited);
      Low = O.MaxWork + 1;
    }
  }
  O.MaxWork = Low;
  EXPECT_EQ(graph(Text, O), G);
  --O.MaxWork;
  EXPECT_THROW(graph(Text, O), DataFlowInterrupted);
  uint64_t Total = 0;
  const auto P = proveFlowProgress(
      G, G.Progress->ObservedContract, {}, [&](uint64_t N) { Total += N; },
      FlowProgressMode::Modular);
  EXPECT_EQ(P.Kind, FlowProgressKind::CounterLoops);
  for (uint64_t Stop = 0; Stop < Total; ++Stop) {
    uint64_t Work = 0;
    EXPECT_THROW(proveFlowProgress(
                     G, G.Progress->ObservedContract, {},
                     [&](uint64_t N) {
                       Work += N;
                       if (Work > Stop)
                         throw DataFlowInterrupted(ComparisonState::Cancelled);
                     },
                     FlowProgressMode::Modular),
                 DataFlowInterrupted);
  }
  std::atomic<bool> Cancel{true};
  ComparisonControl C;
  C.Cancel = &Cancel;
  EXPECT_THROW(graph(Text, {}, C), DataFlowInterrupted);
}
