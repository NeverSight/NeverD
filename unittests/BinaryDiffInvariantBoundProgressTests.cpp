//===- BinaryDiffInvariantBoundProgressTests.cpp - Expression exits
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check input-derived loop proofs against concrete counter orbits,
/// source contracts, hostile recurrences and versioned saved evidence.
//===----------------------------------------------------------------------===//

#include "../lib/diff/FlowInvariantBounds.h"
#include "../lib/diff/FlowProgress.h"
#include "../lib/diff/LLVMFlowSemantics.h"

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
entry:
 %basis = xor i8 %bound, 23
 br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %next, %header ]
 %next = add i8 %i, 3
 %test = icmp ne i8 %next, %basis
 br i1 %test, label %header, label %exit
exit: ret i8 %next
})";
constexpr llvm::StringLiteral Funnel = R"(
define i8 @f(i8 %start, i8 %bound, i8 %other, i1 %choose) {
entry:
 %basis = xor i8 %bound, 23
 br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %next, %latch ]
 br i1 %choose, label %left, label %right
left:
 %a = add i8 %i, 3
 %ba = add i8 %basis, 5
 %ca = icmp ne i8 %a, %ba
 br i1 %ca, label %latch, label %exit
right:
 %b = add i8 %i, 5
 %bb = add i8 %basis, 7
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
    throw std::runtime_error("missing invariant-bound fixture replacement");
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
  const auto Build = Legacy ? buildLLVMParameterBoundProgressDataFlow
                            : buildLLVMInvariantBoundProgressDataFlow;
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
            FlowCounterProof Proof = FlowCounterProof::InvariantModular) {
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

TEST(BinaryDiffInvariantBoundProgress,
     MatchesIndependentExpressionAndSeedOrbits) {
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
                    const bool ContinueA =
                        (Value == (Parameter ^ (23 & Mask))) == EqualA;
                    const bool ContinueB =
                        (((Value + Offset) & Mask) ==
                         (((Parameter ^ (23 & Mask)) + BoundOffset) & Mask)) ==
                        EqualB;
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
                      FlowProgressMode::InvariantBound);
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

TEST(BinaryDiffInvariantBoundProgress, CoversWidthsPolarityAndAffineChains) {
  for (unsigned Bits = 1; Bits <= 64; ++Bits) {
    const auto Text = width(Loop.str(), Bits);
    proved(Text);
    EXPECT_EQ(graph(Text, {}, {}, true).Progress->Kind,
              FlowProgressKind::Unproven);
  }
  proved(replace(Loop.str(), "icmp ne i8 %next, %basis",
                 "icmp ne i8 %basis, %next"));
  auto Text = replace(Loop.str(), "icmp ne", "icmp eq");
  proved(replace(Text, "label %header, label %exit",
                 "label %exit, label %header"));
  proved(replace(Loop.str(), "add i8 %i, 3", "sub i8 %i, 3"));
  unproved(replace(Loop.str(), "add i8 %i, 3", "add i8 %i, 2"));
  unproved(replace(Loop.str(), "add i8 %i, 3", "add i8 %i, 0"));
  unproved(replace(Loop.str(), "icmp ne", "icmp ult"));
  proved(Funnel);
  Text = replace(Funnel.str(), " %ba = add i8 %basis, 5",
                 " %ba0 = sub nsw i8 %basis, 120\n"
                 " %ba1 = add nuw i8 127, %ba0\n"
                 " %ba = sub i8 %ba1, 2");
  proved(Text);
  unproved(replace(Text, "sub i8 %ba1, 2", "sub i8 %ba1, 1"));
  auto Selected = replace(Funnel.str(), "add i8 %i, 5", "sub i8 %i, -3");
  Selected = replace(Selected, "add i8 %basis, 7", "add i8 %basis, 5");
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
  const auto G = graph(Direct);
  EXPECT_EQ(G.Progress->Kind, FlowProgressKind::CounterLoops);
  EXPECT_EQ(G.Progress->BackedgeCount, 2u);
}

TEST(BinaryDiffInvariantBoundProgress, AcceptsPureOperationsAndPoisonMasking) {
  for (llvm::StringRef Expr :
       {"add i8 %bound, %start", "sub i8 5, %bound", "mul i8 %bound, 9",
        "udiv i8 %bound, 3", "sdiv i8 %bound, 3", "urem i8 %bound, 3",
        "srem i8 %bound, 3", "shl i8 %bound, 2", "lshr i8 %bound, 2",
        "ashr i8 %bound, 2", "and i8 %bound, 63", "or i8 %bound, 5",
        "select i1 undef, i8 %bound, i8 %start",
        "select i1 false, i8 poison, i8 %bound",
        "select i1 true, i8 %bound, i8 poison", "xor i8 undef, %bound",
        "xor i8 poison, %bound"})
    proved(replace(Loop.str(), "xor i8 %bound, 23", Expr));
  for (llvm::StringRef Cast : {"zext", "sext"})
    proved(replace(Loop.str(), " %basis = xor i8 %bound, 23",
                   " %narrow = trunc i8 %bound to i4\n %basis = " + Cast.str() +
                       " i4 %narrow to i8"));
  auto Compare = replace(width(Loop.str(), 1), "xor i1 %bound, 23",
                         "icmp eq i1 %bound, %start");
  proved(Compare);
  for (llvm::StringRef Op :
       {"bitreverse", "ctpop", "ctlz", "cttz", "abs", "smin", "smax", "umin",
        "umax", "sadd.sat", "uadd.sat", "ssub.sat", "usub.sat", "fshl",
        "fshr"}) {
    const bool Bool = Op == "ctlz" || Op == "cttz" || Op == "abs";
    const bool Unary = Op == "bitreverse" || Op == "ctpop";
    const bool Ternary = Op == "fshl" || Op == "fshr";
    const auto Types = std::string("i8") +
                       (Bool    ? ", i1 immarg"
                        : Unary ? ""
                                : ", i8") +
                       (Ternary ? ", i8" : "");
    const auto Args = std::string("i8 %bound") +
                      (Bool    ? ", i1 true"
                       : Unary ? ""
                               : ", i8 3") +
                      (Ternary ? ", i8 2" : "");
    const auto Name = "llvm." + Op.str() + ".i8";
    proved("declare i8 @" + Name + "(" + Types + ")\n" +
           replace(Loop.str(), "xor i8 %bound, 23",
                   "call i8 @" + Name + "(" + Args + ")"));
  }
  proved("declare i16 @llvm.bswap.i16(i16)\n" +
         replace(width(Loop.str(), 16), "xor i16 %bound, 23",
                 "call i16 @llvm.bswap.i16(i16 %bound)"));
  for (llvm::StringRef Seed : {"undef", "poison", "0", "%bound"})
    proved(replace(Loop.str(), "[ %start, %entry ]",
                   "[ " + Seed.str() + ", %entry ]"));
  const auto G = graph();
  for (const auto &P : G.ParameterViews)
    EXPECT_TRUE(P.Attributes.empty());
}

TEST(BinaryDiffInvariantBoundProgress, RejectsDependenciesAndDifferentBases) {
  for (llvm::StringRef Expr : {"freeze i8 %bound", "freeze i8 undef"})
    unproved(replace(Loop.str(), "xor i8 %bound, 23", Expr));
  auto Text = replace(Loop.str(), " %basis = xor i8 %bound, 23\n", "");
  Text = replace(Text, " %next =", " %basis = xor i8 %i, 23\n %next =");
  unproved(Text);
  Text = replace(Loop.str(), " %basis = xor i8 %bound, 23\n", "");
  Text = replace(
      Text, " %next =",
      " %basis = phi i8 [ %bound, %entry ], [ %basis, %header ]\n %next =");
  unproved(Text);
  Text = replace(Loop.str(), " %basis = xor i8 %bound, 23",
                 " %frozen = freeze i8 %bound\n"
                 " %basis = select i1 true, i8 %bound, i8 %frozen");
  unproved(Text);
  Text = replace(Loop.str(), " %basis = xor i8 %bound, 23\n", "");
  Text = replace(Text, " %next =",
                 " %basis = select i1 false, i8 %i, i8 %bound\n %next =");
  unproved(Text);
  Text =
      replace(Funnel.str(), " %basis = xor i8 %bound, 23",
              " %basis = xor i8 %bound, 23\n %otherbasis = xor i8 %bound, 23");
  unproved(replace(Text, "add i8 %basis, 7", "add i8 %otherbasis, 7"));
  unproved(replace(Funnel.str(), "add i8 %basis, 7", "add i8 %bound, 7"));
  unproved(replace(Funnel.str(), "icmp ne i8 %b, %bb", "icmp ne i8 %b, 7"));
  unproved(replace(Loop.str(), "[ %next, %header ]", "[ %start, %header ]"));
  unproved(replace(Loop.str(), "exit: ret i8 %next", "exit: br label %exit"));
  auto G = graph();
  const auto L = G.Progress->Loops.front();
  const auto Bound = G.Nodes[L.Compare].Inputs[1].Node;
  auto Check = [&] {
    return proveFlowProgress(
        G, G.Progress->ObservedContract, {}, [](uint64_t) {},
        FlowProgressMode::InvariantBound);
  };
  auto Original = G.Nodes[Bound];
  // A structurally representable dependency cycle must not recurse or prove.
  G.Nodes[Bound].Inputs[0] = G.Nodes[L.Compare].Inputs[1];
  EXPECT_EQ(Check().Kind, FlowProgressKind::Unproven);
  for (llvm::StringRef Semantics : {"i8;unknown", "i8;undef;poison", "i8"}) {
    G.Nodes[Bound] = Original;
    G.Nodes[Bound].Inputs[0].Type = FlowOperand::Kind::Unknown;
    G.Nodes[Bound].Inputs[0].Semantics = Semantics.str();
    EXPECT_EQ(Check().Kind, FlowProgressKind::Unproven);
  }
}

TEST(BinaryDiffInvariantBoundProgress, PreservesEveryLegacyCandidateCharge) {
  constexpr llvm::StringLiteral TwoCounters = R"(
define i8 @f(i8 %start, i8 %bound) {
entry: br label %header
header:
 %i = phi i8 [ %start, %entry ], [ %next, %header ]
 %j = phi i8 [ %bound, %entry ], [ %other, %header ]
 %next = add i8 %i, 3
 %other = add nuw i8 %j, 2
 %test = icmp ne i8 %next, %other
 br i1 %test, label %header, label %exit
exit: ret i8 %next
})";
  for (const auto &Text :
       {TwoCounters.str(),
        replace(Loop.str(), "add i8 %i, 3", "add nsw i8 %i, 3"),
        replace(Loop.str(), "icmp ne i8 %next, %basis", "icmp ne i8 %next, 0"),
        replace(Loop.str(), "icmp ne i8 %next, %basis",
                "icmp ne i8 %next, %bound")}) {
    const auto G = graph(Text);
    std::vector<uint64_t> OldCharges, NewCharges;
    const auto Old = proveFlowProgress(
        G, G.Progress->ObservedContract, {},
        [&](uint64_t N) { OldCharges.push_back(N); },
        FlowProgressMode::ParameterBound);
    const auto New = proveFlowProgress(
        G, G.Progress->ObservedContract, {},
        [&](uint64_t N) { NewCharges.push_back(N); },
        FlowProgressMode::InvariantBound);
    ASSERT_EQ(Old.Kind, FlowProgressKind::CounterLoops);
    EXPECT_EQ(Old, New);
    EXPECT_EQ(OldCharges, NewCharges);
    uint64_t Work = 0;
    for (auto N : OldCharges)
      Work += N;
    for (const auto Mode :
         {FlowProgressMode::ParameterBound, FlowProgressMode::InvariantBound}) {
      uint64_t Used = 0;
      EXPECT_EQ(proveFlowProgress(
                    G, G.Progress->ObservedContract, {},
                    [&](uint64_t N) {
                      if (N > Work - Used)
                        throw DataFlowInterrupted(
                            ComparisonState::ResourceLimited);
                      Used += N;
                    },
                    Mode),
                Old);
      EXPECT_EQ(Used, Work);
    }
  }
  proved(TwoCounters, FlowCounterProof::NoWrap);
}

TEST(BinaryDiffInvariantBoundProgress, RederivesVersionedBasisAndBounds) {
  const auto G = graph(Funnel);
  EXPECT_EQ(G.Profile, "llvm-progress-ssa-v7");
  EXPECT_EQ(readDataFlowGraph(json(G)), G);
  auto Page = dataFlowPageJSON(G, false, 0, 1);
  EXPECT_EQ(Page.getInteger("schema_version"), 11);
  EXPECT_EQ(Page.getObject("progress")->getInteger("guard_count"), 2);
  for (const auto &L : G.Progress->Loops) {
    auto Node = dataFlowNodeJSON(G, G.Blocks[L.Block].Terminator);
    EXPECT_EQ(Node.getObject("progress_loop")->getString("proof"),
              "invariant_modular");
  }
  for (llvm::StringRef Kind :
       {"none", "nowrap", "modular", "parameter_modular", "unsupported"}) {
    auto J = json(G);
    (*J.getAsObject()
          ->getObject("progress")
          ->getArray("loops")
          ->front()
          .getAsObject())["proof"] = Kind;
    EXPECT_THROW(readDataFlowGraph(J), std::invalid_argument);
  }
  auto J = json(G);
  (*J.getAsObject())["profile"] = "llvm-progress-ssa-v6";
  (*J.getAsObject())["schema_version"] = 10;
  EXPECT_THROW(readDataFlowGraph(J), std::invalid_argument);
  auto Forged = G;
  const auto BA =
      Forged.Nodes[Forged.Progress->Loops.front().Compare].Inputs[1].Node;
  const auto Basis = Forged.Nodes[BA].Inputs[0].Node;
  Forged.Nodes[Basis].Inputs[1].Value ^= 1;
  // This remains a valid invariant but must fail fresh native-source binding.
  EXPECT_EQ(readDataFlowGraph(json(Forged)), Forged);
  const auto Old = graph(Funnel, {}, {}, true);
  EXPECT_EQ(Old.Progress->Kind, FlowProgressKind::Unproven);
  EXPECT_EQ(readDataFlowGraph(json(Old)), Old);
}
TEST(BinaryDiffInvariantBoundProgress, PeelsOnlyExactAffineContracts) {
  const auto Original = graph(Funnel);
  const auto L = Original.Progress->Loops.front();
  const auto BA = Original.Nodes[L.Compare].Inputs[1].Node;
  const auto Prefix = Original.Progress->ObservedContract + ";result=i8;flags=";
  for (llvm::StringRef Flags : {"4", "03", "3;extra=1", "-1"}) {
    auto G = Original;
    G.Nodes[BA].Semantics = Prefix + Flags.str();
    // Finite-op admission is not source authentication. An inexact flags
    // encoding cannot be used to unify this bound with the other guard.
    EXPECT_EQ(proveFlowProgress(
                  G, G.Progress->ObservedContract, {}, [](uint64_t) {},
                  FlowProgressMode::InvariantBound)
                  .Kind,
              FlowProgressKind::Unproven);
  }
}

TEST(BinaryDiffInvariantBoundProgress, ChargesBoundsAndStopsTransactionally) {
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
      FlowProgressMode::InvariantBound);
  for (uint64_t Stop = 0; Stop < Calls; ++Stop) {
    uint64_t Current = 0;
    auto Retained = Proof;
    EXPECT_THROW(Retained = proveFlowProgress(
                     G, G.Progress->ObservedContract, {},
                     [&](uint64_t) {
                       if (Current++ == Stop)
                         throw DataFlowInterrupted(ComparisonState::Cancelled);
                     },
                     FlowProgressMode::InvariantBound),
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
TEST(BinaryDiffInvariantBoundProgress, MemoizesDeepSharedValidAndInvalidDAGs) {
  auto G = graph();
  const auto L = G.Progress->Loops.front();
  const auto Root = G.Nodes[L.Compare].Inputs[1];
  auto Template = G.Nodes[Root.Node];
  const auto Constant = Template.Inputs[1];
  auto Current = Root;
  constexpr unsigned Depth = 10000;
  for (unsigned I = 0; I < Depth; ++I) {
    auto N = Template;
    // A shared diamond would be exponential without memoized traversal.
    N.Opcode = "add";
    N.Inputs = {Current, Current};
    N.Origins.clear();
    Current.Node = uint32_t(G.Nodes.size());
    G.Nodes.push_back(std::move(N));
  }
  EXPECT_FALSE(
      validateFiniteLLVMFlow(G, G.Progress->ObservedContract, [](uint64_t) {}));
  uint64_t Calls = 0, Work = 0;
  const auto Charge = [&](uint64_t N) {
    ++Calls;
    Work += N;
  };
  const auto LinearAllowance = [&] {
    uint64_t Payload = G.Nodes.size();
    for (const auto &N : G.Nodes) {
      Payload += 1 + N.Opcode.size() + N.Semantics.size();
      for (const auto &V : N.Inputs)
        Payload += 1 + V.Semantics.size();
    }
    return 12 * Payload;
  };
  FlowInvariantBounds Bounds(G, G.Progress->ObservedContract, Charge);
  ASSERT_TRUE(Bounds.observe(Current));
  EXPECT_LT(Calls, 10u * G.Nodes.size());
  EXPECT_LT(Work, LinearAllowance());
  const auto Before = Calls;
  for (unsigned I = 0; I < Depth; ++I)
    ASSERT_TRUE(Bounds.observe(Current));
  EXPECT_EQ(Calls - Before, Depth);
  // Appending affine translations must cache the normalized offset too.
  for (unsigned I = 0; I < Depth; ++I) {
    auto N = Template;
    N.Opcode = "add";
    N.Inputs = {Current, Constant};
    N.Origins.clear();
    Current.Node = uint32_t(G.Nodes.size());
    G.Nodes.push_back(std::move(N));
  }
  Calls = Work = 0;
  FlowInvariantBounds Affine(G, G.Progress->ObservedContract, Charge);
  const auto Result = Affine.observe(Current);
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->Offset, (Depth * Constant.Value) & 255u);
  EXPECT_LT(Calls, 12u * G.Nodes.size());
  EXPECT_LT(Work, LinearAllowance());
  G.Nodes[Root.Node].Opcode = "freeze";
  G.Nodes[Root.Node].Inputs.resize(1);
  Calls = Work = 0;
  FlowInvariantBounds Rejected(G, G.Progress->ObservedContract, Charge);
  EXPECT_FALSE(Rejected.observe(Current));
  EXPECT_LT(Calls, 8u * G.Nodes.size());
  EXPECT_LT(Work, LinearAllowance());
  const auto RejectedBefore = Calls;
  for (unsigned I = 0; I < Depth; ++I)
    EXPECT_FALSE(Rejected.observe(Current));
  EXPECT_EQ(Calls - RejectedBefore, Depth);
}

TEST(BinaryDiffInvariantBoundProgress, CompletesNestedProofsAtomically) {
  constexpr llvm::StringLiteral Nested = R"(
define i8 @f(i8 %seed, i8 %bound) {
entry:
 %basis = xor i8 %bound, 23
 br label %outer
outer:
 %o = phi i8 [ 0, %entry ], [ %onext, %latch ]
 br label %inner
inner:
 %i = phi i8 [ %seed, %outer ], [ %inext, %inner ]
 %inext = add i8 %i, 3
 %ic = icmp ne i8 %inext, %basis
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
  EXPECT_EQ(G.Progress->Loops[0].Proof, FlowCounterProof::InvariantModular);
  EXPECT_EQ(G.Progress->Loops[1].Proof, FlowCounterProof::NoWrap);
  unproved(replace(Nested.str(), "add i8 %i, 3", "add i8 %i, 2"));
}
