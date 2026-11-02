//===- BinaryDiffParameterBoundProgressTests.cpp - Relative exits ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check parameter-relative loop proofs against concrete counter orbits,
/// source contracts, hostile recurrences and versioned saved evidence.
//===----------------------------------------------------------------------===//

#include "../lib/diff/FlowProgress.h"

#include "neverd/backend/ParameterSites.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/diff/Control.h"
#include "neverd/diff/DataFlow.h"
#include "neverd/diff/DataFlowJSON.h"
#include "neverd/diff/LLVMDataFlow.h"
#include "neverd/diff/Limits.h"

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
#include <chrono>
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
define i8 @f(i8 %start, i8 %bound) {
entry: br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %next, %header ]
 %next = add i8 %i, 3
 %test = icmp ne i8 %next, %bound
 br i1 %test, label %header, label %exit
exit: ret i8 %next
})";
constexpr llvm::StringLiteral Funnel = R"(
define i8 @f(i8 %start, i8 %bound, i8 %other, i1 %choose) {
entry: br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %next, %latch ]
 br i1 %choose, label %left, label %right
left:
 %a = add i8 %i, 3
 %ba = add i8 %bound, 5
 %ca = icmp ne i8 %a, %ba
 br i1 %ca, label %latch, label %exit
right:
 %b = add i8 %i, 5
 %bb = add i8 %bound, 7
 %cb = icmp ne i8 %b, %bb
 br i1 %cb, label %latch, label %exit
latch:
 %next = add i8 %i, 3
 br label %header
exit: ret i8 %i
})";
std::string replace(std::string Text, llvm::StringRef From,
                    llvm::StringRef To) {
  const auto At = Text.find(From.str());
  if (At == std::string::npos)
    throw std::runtime_error("missing parameter-bound fixture replacement");
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
      Sources.Observations.push_back({0x1000, {Address, 0}, &I});
      Address += 4;
    }
  std::string Before, After;
  llvm::raw_string_ostream Original(Before), Current(After);
  F.print(Original);
  const auto Build = Legacy ? buildLLVMModularProgressDataFlow
                            : buildLLVMParameterBoundProgressDataFlow;
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
void proved(llvm::StringRef Text,
            FlowCounterProof Proof = FlowCounterProof::ParameterModular) {
  const auto G = graph(Text);
  ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops) << Text.str();
  ASSERT_FALSE(G.Progress->Loops.empty());
  for (const auto &L : G.Progress->Loops)
    EXPECT_EQ(L.Proof, Proof);
}
void unproved(llvm::StringRef Text) {
  const auto G = graph(Text);
  EXPECT_EQ(G.Progress->Kind, FlowProgressKind::Unproven) << Text.str();
  EXPECT_EQ(G.Progress->SourceContract, G.Progress->ObservedContract);
  EXPECT_TRUE(G.Progress->Loops.empty());
  EXPECT_EQ(G.Progress->LoopCount, 0u);
  EXPECT_EQ(G.Progress->BackedgeCount, 0u);
}
} // namespace

TEST(BinaryDiffParameterBoundProgress, ProvesEveryWidthAndBothGuardPolarities) {
  for (unsigned Bits = 1; Bits <= 64; ++Bits) {
    const auto Text = width(Loop.str(), Bits);
    proved(Text);
    EXPECT_EQ(graph(Text, {}, {}, true).Progress->Kind,
              FlowProgressKind::Unproven);
  }
  proved(replace(Loop.str(), "icmp ne i8 %next, %bound",
                 "icmp ne i8 %bound, %next"));
  auto Text = replace(Loop.str(), "icmp ne", "icmp eq");
  proved(replace(Text, "label %header, label %exit",
                 "label %exit, label %header"));
  proved(replace(Loop.str(), "add i8 %i, 3", "sub i8 %i, 3"));
  proved(replace(Loop.str(), "add i8 %i, 3", "add i8 3, %i"));
  unproved(replace(Loop.str(), "add i8 %i, 3", "add i8 %i, 2"));
  proved(replace(Text, "add i8 %i, 3", "add i8 %i, 2"));
  proved(replace(width(Text, 64), "add i64 %i, 3",
                 "add i64 %i, -9223372036854775808"));
}

TEST(BinaryDiffParameterBoundProgress,
     PreservesUndefPoisonAndSourceAttributes) {
  for (llvm::StringRef Seed : {"undef", "poison", "0", "%bound"})
    proved(replace(Loop.str(), "[ %start, %entry ]",
                   "[ " + Seed.str() + ", %entry ]"));
  auto Text = replace(Loop.str(), "entry: br label %header",
                      "entry:\n %seed = freeze i8 %start\n br label %header");
  proved(replace(Text, "[ %start, %entry ]", "[ %seed, %entry ]"));
  const auto G = graph();
  ASSERT_EQ(G.ParameterViews.size(), 2u);
  EXPECT_TRUE(G.ParameterViews[0].Attributes.empty());
  EXPECT_TRUE(G.ParameterViews[1].Attributes.empty());
  proved(replace(Loop.str(), "i8 %bound)", "i8 noundef %bound)"));
  for (llvm::StringRef Flags : {"nsw ", "nuw ", "nsw nuw "}) {
    Text = replace(Funnel.str(), "add i8 %bound, 5",
                   "add " + Flags.str() + "i8 %bound, 5");
    proved(Text);
  }
  // A fresh defined choice may select either guard. It does not reset their
  // shared counter or input parameter, so all exits must still be common.
  proved(replace(Funnel.str(), " br i1 %choose,",
                 " %pick = freeze i1 undef\n br i1 %pick,"));
}

TEST(BinaryDiffParameterBoundProgress, GroupsAffineBoundsAndEveryBackedge) {
  const auto G = graph(Funnel);
  ASSERT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(G.Progress->LoopCount, 1u);
  EXPECT_EQ(G.Progress->BackedgeCount, 1u);
  EXPECT_EQ(G.Progress->Loops.size(), 2u);
  proved(replace(Funnel.str(), "add i8 %bound, 5", "add i8 5, %bound"));
  proved(replace(Funnel.str(), "add i8 %bound, 5", "sub i8 %bound, -5"));
  auto Selected = replace(Funnel.str(), "add i8 %i, 5", "sub i8 %i, -3");
  Selected = replace(Selected, "add i8 %bound, 7", "add i8 %bound, 5");
  Selected = replace(Selected, "%next = add i8 %i, 3",
                     "%next = phi i8 [ %a, %left ], [ %b, %right ]");
  proved(Selected);
  unproved(replace(Selected, "sub i8 %i, -3", "sub i8 %i, -5"));
  auto Direct =
      replace(Selected, "[ %next, %latch ]", "[ %a, %left ], [ %b, %right ]");
  for (unsigned I = 0; I < 2; ++I)
    Direct = replace(Direct, "label %latch, label %exit",
                     "label %header, label %exit");
  Direct = replace(Direct,
                   "latch:\n %next = phi i8 [ %a, %left ], [ %b, %right ]\n"
                   " br label %header\n",
                   "");
  const auto Branched = graph(Direct);
  ASSERT_EQ(Branched.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(Branched.Progress->BackedgeCount, 2u);
}

TEST(BinaryDiffParameterBoundProgress, RefusesDifferentCoordinatesAndResets) {
  unproved(replace(Funnel.str(), "add i8 %bound, 7", "add i8 %other, 7"));
  unproved(replace(Funnel.str(), "add i8 %bound, 7", "add i8 %bound, 8"));
  unproved(replace(Funnel.str(), "add i8 %bound, 5", "sub i8 5, %bound"));
  unproved(replace(Funnel.str(), "icmp ne i8 %a, %ba", "icmp ne i8 %a, 5"));
  unproved(replace(Funnel.str(), "icmp ne i8 %b, %bb", "icmp ne i8 %b, 7"));
  unproved(replace(Loop.str(), "icmp ne", "icmp ule"));
  unproved(replace(Loop.str(), "[ %next, %header ]", "[ %start, %header ]"));
  unproved(replace(Loop.str(), "add i8 %i, 3", "add i8 %i, 0"));
  auto Text =
      replace(Loop.str(), " %test =", " %frozen = freeze i8 %bound\n %test =");
  unproved(
      replace(Text, "icmp ne i8 %next, %bound", "icmp ne i8 %next, %frozen"));
  unproved(replace(replace(Text, "freeze i8 %bound", "freeze i8 undef"),
                   "icmp ne i8 %next, %bound", "icmp ne i8 %next, %frozen"));
  Text =
      replace(Loop.str(), " %test =", " %frozen = freeze i8 %next\n %test =");
  unproved(
      replace(Text, "icmp ne i8 %next, %bound", "icmp ne i8 %frozen, %bound"));
  unproved(replace(Text, "[ %next, %header ]", "[ %frozen, %header ]"));
  Text = replace(Loop.str(), " br i1 %test,",
                 " %frozen = freeze i1 %test\n br i1 %frozen,");
  unproved(Text);
  Text = replace(Loop.str(), " %test =", " %moving = add i8 %i, 2\n %test =");
  unproved(
      replace(Text, "icmp ne i8 %next, %bound", "icmp ne i8 %next, %moving"));
}

TEST(BinaryDiffParameterBoundProgress, RetainsPriorityAndRequiresAResidualDAG) {
  proved(replace(Loop.str(), "add i8 %i, 3", "add nsw i8 %i, 3"),
         FlowCounterProof::NoWrap);
  proved(replace(Loop.str(), "icmp ne i8 %next, %bound", "icmp ne i8 %next, 0"),
         FlowCounterProof::Modular);
  constexpr llvm::StringLiteral Nested = R"(
define i8 @f(i8 %seed, i8 %bound) {
entry: br label %outer
outer:
 %o = phi i8 [ 0, %entry ], [ %onext, %latch ]
 br label %inner
inner:
 %i = phi i8 [ %seed, %outer ], [ %inext, %inner ]
 %inext = add i8 %i, 3
 %ic = icmp ne i8 %inext, %bound
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
  ASSERT_EQ(G.Progress->Loops.size(), 2u);
  EXPECT_EQ(G.Progress->Loops[0].Proof, FlowCounterProof::ParameterModular);
  EXPECT_EQ(G.Progress->Loops[1].Proof, FlowCounterProof::NoWrap);
  unproved(replace(Nested.str(), "add i8 %i, 3", "add i8 %i, 2"));
  unproved(replace(Loop.str(), "exit: ret i8 %next", "exit: br label %exit"));
}

TEST(BinaryDiffParameterBoundProgress, AuthenticatesVersionBasisAndParameter) {
  const auto G = graph(Funnel);
  EXPECT_EQ(G.Profile, "llvm-progress-ssa-v6");
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
  for (const auto &L : G.Progress->Loops) {
    auto Page = dataFlowNodeJSON(G, G.Blocks[L.Block].Terminator);
    EXPECT_EQ(Page.getObject("progress_loop")->getString("proof"),
              "parameter_modular");
  }
  auto Page = dataFlowPageJSON(G, false, 0, 1);
  EXPECT_EQ(Page.getInteger("schema_version"), 10);
  EXPECT_EQ(Page.getObject("progress")->getInteger("guard_count"), 2);
  EXPECT_FALSE(Page.getObject("progress")->get("loops"));
  for (llvm::StringRef Kind : {"none", "nowrap", "modular", "unsupported"}) {
    auto J = json(G);
    (*J.getAsObject()
          ->getObject("progress")
          ->getArray("loops")
          ->front()
          .getAsObject())["proof"] = Kind;
    EXPECT_THROW(readDataFlowGraph(J), std::invalid_argument);
  }
  auto J = json(G);
  (*J.getAsObject())["profile"] = "llvm-progress-ssa-v5";
  (*J.getAsObject())["schema_version"] = 9;
  EXPECT_THROW(readDataFlowGraph(J), std::invalid_argument);
  auto Forged = G;
  const auto Bound =
      Forged.Nodes[Forged.Progress->Loops[0].Compare].Inputs[1].Node;
  Forged.Nodes[Bound].Inputs[0].Value = 2;
  EXPECT_TRUE(Forged.validate({}));
  EXPECT_THROW(readDataFlowGraph(json(Forged)), std::invalid_argument);
  Forged = G;
  Forged.ParameterViews[1].ViewBits = 7;
  EXPECT_TRUE(Forged.validate({}));
  // Changing every bound consistently can preserve the proof. The SDK's
  // separate native-source binding test must reject this valid altered graph.
  Forged = G;
  auto Parameter = Forged.Nodes[Forged.Progress->Loops[0].Counter].Inputs[0];
  ASSERT_EQ(Parameter.Type, FlowOperand::Kind::Parameter);
  Parameter.Predecessor = NoIndex;
  for (const auto &L : Forged.Progress->Loops) {
    const auto BoundNode = Forged.Nodes[L.Compare].Inputs[1].Node;
    Forged.Nodes[BoundNode].Inputs[0] = Parameter;
  }
  EXPECT_FALSE(Forged.validate({}));
  EXPECT_EQ(readDataFlowGraph(json(Forged)), Forged);
  const auto Old = graph(Funnel, {}, {}, true);
  EXPECT_EQ(Old.Progress->Kind, FlowProgressKind::Unproven);
  EXPECT_EQ(readDataFlowGraph(json(Old)), Old);
}

TEST(BinaryDiffParameterBoundProgress,
     MatchesExhaustiveParameterAndSeedOrbits) {
  // Direct arithmetic enumerates every parameter, seed and orbit. It shares
  // neither relative coordinates nor residue/interval construction with proof.
  for (unsigned Bits = 1; Bits <= 4; ++Bits) {
    const unsigned Size = 1u << Bits, Mask = Size - 1;
    const auto Text = replace(Funnel.str(), " br i1 %choose,",
                              " %pick = freeze i1 undef\n br i1 %pick,");
    auto G = graph(width(Text, Bits));
    ASSERT_EQ(G.Progress->Loops.size(), 2u);
    const auto L = G.Progress->Loops[0], R = G.Progress->Loops[1];
    const auto A = G.Nodes[L.Compare].Inputs[0].Node;
    const auto B = G.Nodes[R.Compare].Inputs[0].Node;
    const auto BA = G.Nodes[L.Compare].Inputs[1].Node;
    const auto BB = G.Nodes[R.Compare].Inputs[1].Node;
    G.Nodes[A].Inputs[1].Value = 0;
    G.Nodes[BA].Inputs[1].Value = 0;
    for (unsigned Step = 1; Step < Size; ++Step)
      for (unsigned Offset = 0; Offset < Size; ++Offset)
        for (unsigned BoundOffset = 0; BoundOffset < Size; ++BoundOffset)
          for (bool EqualA : {false, true})
            for (bool EqualB : {false, true}) {
              G.Nodes[L.Update].Inputs[1].Value = Step;
              G.Nodes[B].Inputs[1].Value = Offset;
              G.Nodes[BB].Inputs[1].Value = BoundOffset;
              const auto Prefix = G.Progress->ObservedContract +
                                  ";result=i1;flags=0;predicate=";
              bool AllExit = true;
              for (unsigned Parameter = 0; Parameter < Size && AllExit;
                   ++Parameter)
                for (unsigned Seed = 0; Seed < Size && AllExit; ++Seed) {
                  unsigned Value = Seed, Visits = 0;
                  for (; Visits < Size; ++Visits) {
                    const bool ContinueA = (Value == Parameter) == EqualA;
                    const bool ContinueB =
                        (((Value + Offset) & Mask) ==
                         ((Parameter + BoundOffset) & Mask)) == EqualB;
                    if (!ContinueA && !ContinueB)
                      break;
                    Value = (Value + Step) & Mask;
                  }
                  AllExit = Visits < Size;
                }
              for (bool Reverse : {false, true})
                for (bool Invert : {false, true}) {
                  G.Nodes[L.Compare].Semantics =
                      Prefix + std::to_string(EqualA != Invert
                                                  ? llvm::CmpInst::ICMP_EQ
                                                  : llvm::CmpInst::ICMP_NE);
                  G.Nodes[R.Compare].Semantics =
                      Prefix + std::to_string(EqualB != Invert
                                                  ? llvm::CmpInst::ICMP_EQ
                                                  : llvm::CmpInst::ICMP_NE);
                  for (const auto &Witness : {L, R}) {
                    if (Reverse)
                      std::swap(G.Nodes[Witness.Compare].Inputs[0],
                                G.Nodes[Witness.Compare].Inputs[1]);
                    if (Invert)
                      std::swap(G.Blocks[Witness.Block].Successors[0],
                                G.Blocks[Witness.Block].Successors[1]);
                  }
                  const auto P = proveFlowProgress(
                      G, G.Progress->ObservedContract, {}, [](uint64_t) {},
                      FlowProgressMode::ParameterBound);
                  ASSERT_EQ(P.Kind == FlowProgressKind::CounterLoops, AllExit)
                      << Bits << ' ' << Step << ' ' << Offset << ' '
                      << BoundOffset << ' ' << EqualA << ' ' << EqualB << ' '
                      << Reverse << ' ' << Invert;
                  for (const auto &Witness : {L, R}) {
                    if (Reverse)
                      std::swap(G.Nodes[Witness.Compare].Inputs[0],
                                G.Nodes[Witness.Compare].Inputs[1]);
                    if (Invert)
                      std::swap(G.Blocks[Witness.Block].Successors[0],
                                G.Blocks[Witness.Block].Successors[1]);
                  }
                }
            }
  }
}

TEST(BinaryDiffParameterBoundProgress, ChargesBoundsAndStopsTransactionally) {
  const auto G = graph(Funnel);
  DataFlowOptions O;
  O.MaxNodes = G.retainedItems();
  EXPECT_EQ(graph(Funnel, O), G);
  --O.MaxNodes;
  EXPECT_THROW(graph(Funnel, O), DataFlowInterrupted);
  O = {};
  uint64_t Low = 1, High = limits::diff::kDefaultFlowMaxWork;
  while (Low < High) {
    O.MaxWork = Low + (High - Low) / 2;
    try {
      graph(Funnel, O);
      High = O.MaxWork;
    } catch (const DataFlowInterrupted &E) {
      EXPECT_EQ(E.State, ComparisonState::ResourceLimited);
      Low = O.MaxWork + 1;
    }
  }
  O.MaxWork = Low;
  EXPECT_EQ(graph(Funnel, O), G);
  --O.MaxWork;
  EXPECT_THROW(graph(Funnel, O), DataFlowInterrupted);
  uint64_t Calls = 0;
  const auto Proof = proveFlowProgress(
      G, G.Progress->ObservedContract, {}, [&](uint64_t) { ++Calls; },
      FlowProgressMode::ParameterBound);
  for (uint64_t Stop = 0; Stop < Calls; ++Stop) {
    uint64_t Current = 0;
    auto Retained = Proof;
    EXPECT_THROW(Retained = proveFlowProgress(
                     G, G.Progress->ObservedContract, {},
                     [&](uint64_t) {
                       if (Current++ == Stop)
                         throw DataFlowInterrupted(ComparisonState::Cancelled);
                     },
                     FlowProgressMode::ParameterBound),
                 DataFlowInterrupted);
    EXPECT_EQ(Retained, Proof);
  }
  std::atomic<bool> Cancel{true};
  ComparisonControl C;
  C.Cancel = &Cancel;
  EXPECT_THROW(graph(Funnel, {}, C), DataFlowInterrupted);
  EXPECT_THROW(readDataFlowGraph(json(G), {}, C), ComparisonInterrupted);
  C.Cancel = nullptr;
  C.Deadline = std::chrono::steady_clock::now();
  EXPECT_THROW(graph(Funnel, {}, C), DataFlowInterrupted);
  EXPECT_EQ(graph(Funnel), G);
}
