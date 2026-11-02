//===- BinaryDiffFlowBindingTests.cpp - Authenticated PHI numbering -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Renumbering may preserve source identity; altered facts and guessed PHI
/// correspondences must not. Exercise the shared binding owner and budgets.
//===----------------------------------------------------------------------===//

#include "../lib/diff/FlowBinding.h"

#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/diff/FlowEvidence.h"
#include "neverd/diff/LLVMDataFlow.h"
#include "neverd/diff/Program.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::diff;
namespace {
constexpr llvm::StringLiteral Cycle = R"(
define i8 @f(i8 %seed) {
entry: br label %header
header:
 %counter = phi i8 [ %seed, %entry ], [ %next, %latch ]
 %a = phi i8 [ 1, %entry ], [ %b, %latch ]
 %b = phi i8 [ 2, %entry ], [ %c, %latch ]
 %c = phi i8 [ 3, %entry ], [ %a, %latch ]
 %body = add i8 %a, %b
 %mix = xor i8 %body, %c
 br label %latch
latch:
 %next = add i8 %counter, 3
 %cmp = icmp ne i8 %next, 0
 br i1 %cmp, label %header, label %exit
exit: ret i8 %mix
})";
DataFlowGraph graph(bool Progress = true, bool Dead = false) {
  std::string Text = Cycle.str();
  if (Dead)
    Text.insert(Text.find(" %body ="),
                " %dead = phi i8 [ undef, %entry ], [ %dead, %latch ]\n");
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto M = llvm::parseAssemblyString(Text, Diagnostic, Context);
  if (!M)
    throw std::runtime_error(Diagnostic.getMessage().str());
  auto &F = *M->getFunction("f");
  LLVMSourceMap Sources;
  Sources.Functions[0x1000] = &F;
  for (auto &B : F)
    for (auto &I : B)
      Sources.Observations.push_back({0x1000, {0x1000, 0}, &I});
  const auto Build =
      Progress ? buildLLVMModularProgressDataFlow : buildLLVMDataFlow;
  auto G = Build(F, Sources, 0x1000, "owned", "same-abi", true, {}, {});
  EXPECT_FALSE(G.validate({}));
  return G;
}
DataFlowGraph reorder(const DataFlowGraph &G,
                      std::vector<uint32_t> Order = {}) {
  if (Order.empty()) {
    Order.resize(G.Nodes.size());
    std::iota(Order.begin(), Order.end(), 0);
    std::vector<uint32_t> Phis;
    for (uint32_t I = 0; I < G.Nodes.size(); ++I)
      if (G.Nodes[I].Phi)
        Phis.push_back(I);
    for (size_t I = 0; I < Phis.size(); ++I)
      Order[Phis[I]] = Phis[(I + 1) % Phis.size()];
  }
  std::vector<uint32_t> Inverse(Order.size());
  for (uint32_t I = 0; I < Order.size(); ++I)
    Inverse[Order[I]] = I;
  auto Out = G;
  for (uint32_t I = 0; I < Order.size(); ++I) {
    Out.Nodes[I] = G.Nodes[Order[I]];
    for (auto &Input : Out.Nodes[I].Inputs)
      if (Input.Type == FlowOperand::Kind::Node)
        Input.Node = Inverse[Input.Node];
  }
  for (auto &B : Out.Blocks)
    if (B.Terminator != NoIndex)
      B.Terminator = Inverse[B.Terminator];
  if (Out.Progress)
    for (auto &L : Out.Progress->Loops) {
      L.Counter = Inverse[L.Counter];
      L.Update = Inverse[L.Update];
      L.Compare = Inverse[L.Compare];
    }
  EXPECT_FALSE(Out.validate({}));
  return Out;
}
bool same(const DataFlowGraph &A, const DataFlowGraph &B) {
  return sameLLVMGraphAfterPhiRenumbering(A, B, {}, [](uint64_t) {});
}
std::optional<std::string> bind(const DataFlowGraph &A, const DataFlowGraph &B,
                                const DataFlowOptions &Options = {}) {
  ProgramIdentity Id;
  Id.Architecture = "x86_64";
  Id.ABI = A.ABI;
  Id.Revision = A.Revision;
  Id.SHA256 = std::string(64, 'a');
  Id.Bits = 64;
  Id.Endian = "little";
  ProgramBuilder Builder(Id);
  Builder.beginFunction(0x1000, 1, "", NameSource::Generated, 0);
  Builder.addBlock(0x1000);
  Builder.addInstruction(0x1000, {0x90}, "fixture", 0x1000, 0x1000,
                         InstructionFlow::None);
  auto P = Builder.finish();
  DataFlowSettings Settings;
  Settings.Enabled = true;
  Settings.Profile = A.Profile;
  Settings.Graph = Options;
  DataFlowEvidence Saved, Fresh;
  Saved.Recipe = Fresh.Recipe = "owned";
  Saved.EligibleFunctions = Fresh.EligibleFunctions = 1;
  Saved.Entries = {{0x1000, FlowAvailability::Available, {}, A}};
  Fresh.Entries = {{0x1000, FlowAvailability::Available, {}, B}};
  return bindDataFlowEvidence(&Saved, &Fresh, Settings, P);
}
uint32_t opcode(const DataFlowGraph &G, llvm::StringRef Name) {
  for (uint32_t I = 0; I < G.Nodes.size(); ++I)
    if (G.Nodes[I].Opcode == Name)
      return I;
  throw std::runtime_error("missing binding fixture opcode");
}
} // namespace

TEST(BinaryDiffFlowBinding, AnchorsCyclicPhisWithoutChangingEitherSnapshot) {
  for (bool Progress : {false, true}) {
    const auto A = graph(Progress), B = reorder(A), OriginalA = A,
               OriginalB = B;
    ASSERT_NE(A, B);
    EXPECT_TRUE(same(A, B));
    EXPECT_TRUE(same(B, A));
    EXPECT_FALSE(bind(A, B));
    EXPECT_FALSE(bind(B, A));
    EXPECT_EQ(A, OriginalA);
    EXPECT_EQ(B, OriginalB);
  }
}

TEST(BinaryDiffFlowBinding, RefusesAlteredFactsAndConflictingDefinitions) {
  const auto A = graph(), B = reorder(A);
  for (unsigned Change = 0; Change < 7; ++Change) {
    auto Bad = B;
    if (Change == 0)
      Bad.Nodes[opcode(Bad, "xor")].Inputs[1].Node =
          Bad.Nodes[opcode(Bad, "add")].Inputs[0].Node;
    else if (Change == 1)
      std::swap(Bad.Nodes[opcode(Bad, "add")].Inputs[0],
                Bad.Nodes[opcode(Bad, "add")].Inputs[1]);
    else if (Change == 2)
      ++Bad.Nodes[opcode(Bad, "xor")].Origins.front().Sequence;
    else if (Change == 3)
      Bad.ABI = "other-abi";
    else if (Change == 4) {
      for (auto &N : Bad.Nodes)
        if (N.Phi && N.Inputs.front().Type == FlowOperand::Kind::Constant) {
          ++N.Inputs.front().Value;
          break;
        }
    } else if (Change == 5)
      Bad.Progress->Loops.front().Proof = FlowCounterProof::NoWrap;
    else
      Bad.Progress->SourceContract = Bad.Progress->ObservedContract;
    if (Change < 5)
      ASSERT_FALSE(Bad.validate({})) << Change;
    EXPECT_FALSE(same(A, Bad)) << Change;
    EXPECT_TRUE(bind(A, Bad)) << Change;
  }
}

TEST(BinaryDiffFlowBinding, RejectsOrdinaryReorderingAndUnanchoredCycles) {
  const auto A = graph();
  std::vector<uint32_t> Order(A.Nodes.size());
  std::iota(Order.begin(), Order.end(), 0);
  std::swap(Order[opcode(A, "add")], Order[opcode(A, "xor")]);
  const auto B = reorder(A, Order);
  EXPECT_FALSE(same(A, B));
  EXPECT_TRUE(bind(A, B));
  const auto Dead = graph(false, true), Permuted = reorder(Dead);
  EXPECT_FALSE(same(Dead, Permuted));
  EXPECT_TRUE(bind(Dead, Permuted));
  // Existing exact binding remains valid even with an unanchored PHI cycle.
  EXPECT_FALSE(bind(Dead, Dead));
}

TEST(BinaryDiffFlowBinding, ValidatesFreshReferencesBeforeFollowingThem) {
  const auto A = graph(), B = reorder(A);
  for (unsigned Change = 0; Change < 4; ++Change) {
    auto Bad = B;
    if (Change == 0)
      Bad.Nodes[opcode(Bad, "xor")].Inputs[0].Node = Bad.Nodes.size();
    else if (Change == 1)
      Bad.Blocks.front().Terminator = Bad.Nodes.size();
    else if (Change == 2)
      Bad.Nodes[opcode(Bad, "phi")].Inputs[0].Predecessor = Bad.Blocks.size();
    else
      Bad.Progress->Loops.front().Counter = Bad.Nodes.size();
    EXPECT_FALSE(same(A, Bad));
    EXPECT_TRUE(bind(A, Bad));
  }
}

TEST(BinaryDiffFlowBinding, ChargesExactWorkAndEveryCancellationPoint) {
  const auto A = graph(false), B = reorder(A);
  uint64_t Total = 0;
  ASSERT_TRUE(sameLLVMGraphAfterPhiRenumbering(
      A, B, {}, [&](uint64_t N) { Total += N; }));
  DataFlowOptions Options;
  Options.MaxWork = Total;
  ASSERT_FALSE(A.validate(Options));
  ASSERT_FALSE(B.validate(Options));
  EXPECT_FALSE(bind(A, B, Options));
  --Options.MaxWork;
  const auto Error = bind(A, B, Options);
  ASSERT_TRUE(Error);
  EXPECT_EQ(*Error, "saved dataflow PHI binding exceeds its work allowance");
  for (uint64_t Stop = 0; Stop < Total; ++Stop) {
    uint64_t Work = 0;
    EXPECT_THROW(sameLLVMGraphAfterPhiRenumbering(
                     A, B, {},
                     [&](uint64_t N) {
                       Work += N;
                       if (Work > Stop)
                         throw DataFlowInterrupted(ComparisonState::Cancelled);
                     }),
                 DataFlowInterrupted);
  }
}

TEST(BinaryDiffFlowBinding, RestoreControlStopsInsideExactBindingCharges) {
  const auto A = graph(false), B = reorder(A);
  const auto OriginalA = A, OriginalB = B;
  uint64_t Total = 0;
  ASSERT_TRUE(sameLLVMGraphAfterPhiRenumbering(
      A, B, {}, [&](uint64_t N) { Total += N; }));
  ASSERT_GT(Total, 2u);
  for (const uint64_t Boundary : {uint64_t(1), Total / 2, Total - 1}) {
    std::atomic<bool> Cancel = false;
    ComparisonControl Control;
    Control.Cancel = &Cancel;
    uint64_t Charged = 0;
    EXPECT_THROW(sameLLVMGraphAfterPhiRenumbering(
                     A, B, {},
                     [&](uint64_t N) {
                       Charged += N;
                       if (Charged > Boundary)
                         Cancel = true;
                     },
                     Control),
                 ComparisonInterrupted);
    EXPECT_GT(Charged, Boundary);
    EXPECT_EQ(A, OriginalA);
    EXPECT_EQ(B, OriginalB);
  }
}
