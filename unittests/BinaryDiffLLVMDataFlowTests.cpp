//===- BinaryDiffLLVMDataFlowTests.cpp - Typed optimized SSA contracts
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Correspondence, explicit refusal and provenance without executing inputs.
//===----------------------------------------------------------------------===//

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/diff/DataFlowJSON.h"
#include "neverd/diff/FlowMatch.h"
#include "neverd/diff/LLVMDataFlow.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include <gtest/gtest.h>

using namespace neverd;
using namespace neverd::diff;
namespace {
DataFlowGraph graph(llvm::StringRef Text, const DataFlowOptions &Options = {},
                    const ComparisonControl &Control = {}) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(Text, Diagnostic, Context);
  if (!Module)
    throw std::runtime_error(Diagnostic.getMessage().str());
  auto *F = Module->getFunction("f");
  LLVMSourceMap Sources;
  Sources.Functions[0x1000] = F;
  uint64_t Address = 0x1000;
  for (auto &B : *F)
    for (auto &I : B) {
      Sources.Observations.push_back({0x1000, {Address, 0}, &I});
      Sources.Observations.push_back({0x1000, {Address + 0x100, 1}, &I});
      Sources.Observations.push_back({0x1000, {Address + 0x200, 2}, &I, true});
      Address += 4;
    }
  return buildLLVMDataFlow(*F, Sources, 0x1000, "owned", "same-abi", true,
                           Options, Control);
}
FlowNeighborhoodMatch match(const DataFlowGraph &A, unsigned AI,
                            const DataFlowGraph &B, unsigned BI) {
  DataFlowOptions O;
  O.Rounds = 16;
  return matchDataFlowNeighborhood(A, AI, fingerprintDataFlow(A, O), B, BI,
                                   fingerprintDataFlow(B, O), O);
}
constexpr llvm::StringLiteral Arithmetic = R"(
define i32 @f(i32 %x, i32 %y) {
 %a = add i32 %x, 23
 %b = mul i32 %y, 7
 %c = xor i32 %a, %b
 ret i32 %c
})";
} // namespace

TEST(BinaryDiffLLVMDataFlow, SchedulingNamesAndPluralOriginsRemainIndependent) {
  auto A = graph(Arithmetic);
  auto B = graph(R"(
define i32 @f(i32 %left, i32 %right) {
 %first = mul i32 %right, 7
 %second = add i32 %left, 23
 %result = xor i32 %first, %second
 ret i32 %result
})");
  EXPECT_EQ(match(A, 2, B, 2).Outcome, FlowMatchOutcome::Matched);
  for (const auto &N : A.Nodes) {
    ASSERT_EQ(N.Origins.size(), 2u);
    EXPECT_EQ(N.Address, 0u);
    EXPECT_EQ(N.OriginSequence, -1);
    EXPECT_EQ(N.Origins[0].Sequence, 0);
    EXPECT_EQ(N.Origins[1].Sequence, 1);
  }
  const auto Slice = sliceDataFlow(A, {2}, SliceDirection::Backward, true);
  EXPECT_EQ(Slice.State, ComparisonState::Complete);
  EXPECT_EQ(Slice.Nodes, (std::vector<uint32_t>{0, 1, 2}));
  auto Text = std::string();
  llvm::raw_string_ostream Stream(Text);
  llvm::json::OStream JSON(Stream);
  writeDataFlowGraph(JSON, A);
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Parsed));
  EXPECT_EQ(readDataFlowGraph(*Parsed), A);
  EXPECT_EQ(Parsed->getAsObject()->getInteger("schema_version"), 3);
}

TEST(BinaryDiffLLVMDataFlow, ExactWidthsConstantsFlagsAndPredicatesDiffer) {
  for (const auto &[A, B] : std::vector<std::pair<std::string, std::string>>{
           {"add i8 %x, 1", "add nsw i8 %x, 1"},
           {"add i8 %x, 1", "add nuw i8 %x, 1"},
           {"lshr i8 %x, 1", "lshr exact i8 %x, 1"},
           {"or i8 %x, 1", "or disjoint i8 %x, 1"},
           {"add i8 %x, 1", "add i8 %x, 2"}}) {
    const auto Wrap = [](const std::string &Op) {
      return "define i8 @f(i8 %x) { %a = " + Op + "\n ret i8 %a }";
    };
    EXPECT_EQ(match(graph(Wrap(A)), 0, graph(Wrap(B)), 0).Outcome,
              FlowMatchOutcome::Different);
  }
  auto Bit = graph("define i1 @f(i1 %x) { %a = xor i1 %x, true ret i1 %a }");
  auto Byte = graph("define i8 @f(i8 %x) { %a = xor i8 %x, 1 ret i8 %a }");
  EXPECT_EQ(match(Bit, 0, Byte, 0).Outcome, FlowMatchOutcome::Different);
  auto Signed =
      graph("define i1 @f(i8 %x) { %a = icmp slt i8 %x, 1 ret i1 %a }");
  auto Unsigned =
      graph("define i1 @f(i8 %x) { %a = icmp ult i8 %x, 1 ret i1 %a }");
  EXPECT_EQ(match(Signed, 0, Unsigned, 0).Outcome, FlowMatchOutcome::Different);
  auto Undef = graph("define i8 @f(i8 %x) { %a = xor i8 %x, undef ret i8 %a }");
  const auto Open = match(Undef, 0, Undef, 0);
  EXPECT_EQ(Open.Outcome, FlowMatchOutcome::Matched);
  EXPECT_TRUE(Open.OpenDataFrontier);
}

TEST(BinaryDiffLLVMDataFlow, IntrinsicImmediatesAndArgumentContractsDiffer) {
  const auto Funnel = [](int Count) {
    return "declare i32 @llvm.fshl.i32(i32, i32, i32)\n"
           "define i32 @f(i32 %x) { %a = call i32 @llvm.fshl.i32(i32 %x, i32 "
           "%x, i32 " +
           std::to_string(Count) + ") ret i32 %a }";
  };
  EXPECT_EQ(match(graph(Funnel(5)), 0, graph(Funnel(7)), 0).Outcome,
            FlowMatchOutcome::Different);
  auto A = graph("define i8 @f(i8 %x) { %a = add i8 %x, 1 ret i8 %a }");
  auto B = graph("define i8 @f(i8 noundef %x) { %a = add i8 %x, 1 ret i8 %a }");
  EXPECT_EQ(match(A, 0, B, 0).Outcome, FlowMatchOutcome::Different);
}

TEST(BinaryDiffLLVMDataFlow, UnmodelledTypesEffectsAndMetadataRefuse) {
  for (const auto *Text :
       {"define i128 @f(i128 %x) { ret i128 %x }",
        "define <2 x i32> @f(<2 x i32> %x) { ret <2 x i32> %x }",
        "define i32 @f(ptr %p) { %v = load i32, ptr %p ret i32 %v }",
        "declare i32 @callee(i32)\ndefine i32 @f(i32 %x) { %v = call i32 "
        "@callee(i32 %x) ret i32 %v }",
        "define i32 @f(i32 %x) { %v = add i32 %x, 1, !unknown !0 ret i32 %v "
        "}\n!0 = !{i32 1}",
        "define i32 @f(i32 %x) !unknown !0 { ret i32 %x }\n!0 = !{i32 1}",
        "declare i32 @llvm.fshl.i32(i32, i32, i32)\ndefine i32 @f(i32 %x) { %v "
        "= call i32 @llvm.fshl.i32(i32 %x, i32 %x, i32 2) [ \"deopt\"(i32 1) ] "
        "ret i32 %v }"})
    EXPECT_THROW(graph(Text), LLVMDataFlowUnsupported) << Text;
}

TEST(BinaryDiffLLVMDataFlow, WorkAndNativeObservationLimitsAreTransactional) {
  DataFlowOptions Small;
  Small.MaxNodes = 2;
  EXPECT_THROW(graph(Arithmetic, Small), DataFlowInterrupted);
  Small = {};
  Small.MaxWork = 10;
  EXPECT_THROW(graph(Arithmetic, Small), DataFlowInterrupted);
  ComparisonControl Stop;
  std::atomic<bool> Cancel{true};
  Stop.Cancel = &Cancel;
  EXPECT_THROW(graph(Arithmetic, {}, Stop), DataFlowInterrupted);
  auto G = graph(Arithmetic);
  G.Nodes[0].Origins.push_back(G.Nodes[0].Origins.front());
  EXPECT_TRUE(G.validate({}).has_value());
}

TEST(BinaryDiffLLVMDataFlow,
     PhiSwitchArmsAndRepeatedDestinationsRemainExplicit) {
  const std::string Body = R"(
define i32 @f(i32 %x) {
entry: switch i32 %x, label %other [i32 1, label %one i32 2, label %two]
one: %a = add i32 %x, 7
 br label %exit
two: %b = sub i32 %x, 9
 br label %exit
other: %c = xor i32 %x, 23
 br label %exit
exit: %v = phi i32 [%a, %one], [%b, %two], [%c, %other]
 ret i32 %v
})";
  const auto A = graph(Body);
  ASSERT_TRUE(A.Nodes[7].Phi);
  EXPECT_EQ(A.Blocks[0].Successors, (std::vector<uint32_t>{3, 1, 2}));
  ASSERT_EQ(A.Nodes[0].Inputs.size(), 3u);
  EXPECT_EQ(A.Nodes[0].Inputs[1].Value, 1u);
  auto Changed = Body;
  Changed[Changed.find("i32 1, label %one") + 4] = '4';
  EXPECT_NE(match(A, 7, graph(Changed), 7).Outcome, FlowMatchOutcome::Matched);
  Changed = Body;
  const auto Default = Changed.find("label %other");
  Changed.replace(Default, std::string("label %other").size(), "label %one");
  const auto Case = Changed.find("i32 1, label %one");
  Changed.replace(Case, std::string("i32 1, label %one").size(),
                  "i32 1, label %other");
  EXPECT_NE(match(A, 7, graph(Changed), 7).Outcome, FlowMatchOutcome::Matched);
  const auto Repeated = graph(R"(
define i32 @f(i32 %x) {
entry: switch i32 %x, label %same [i32 1, label %same i32 2, label %different]
same: ret i32 3
different: ret i32 5
})");
  EXPECT_EQ(Repeated.Blocks[0].Successors, (std::vector<uint32_t>{1, 1, 2}));
}

TEST(BinaryDiffLLVMDataFlow,
     ScalarExtensionComparisonAndReturnAttributesDiffer) {
  const auto Pair = [](llvm::StringRef A, llvm::StringRef B) {
    EXPECT_EQ(match(graph(A), 0, graph(B), 0).Outcome,
              FlowMatchOutcome::Different);
  };
  Pair("define i32 @f(i8 %x) { %v = zext i8 %x to i32 ret i32 %v }",
       "define i32 @f(i8 %x) { %v = zext nneg i8 %x to i32 ret i32 %v }");
  Pair("define i1 @f(i8 %x) { %v = icmp ult i8 %x, 1 ret i1 %v }",
       "define i1 @f(i8 %x) { %v = icmp samesign ult i8 %x, 1 ret i1 %v }");
  Pair("define i8 @f(i8 %x) { ret i8 %x }",
       "define range(i8 0, 10) i8 @f(i8 %x) { ret i8 %x }");
  Pair("define i8 @f(i8 %x) { %v = add i8 %x, 1 ret i8 %v }",
       "define i8 @f(i8 %x) mustprogress { %v = add i8 %x, 1 ret i8 %v }");
  EXPECT_THROW(graph("define i32 @f(i32 %x) !neverd.rewrite.source-identity !0 "
                     "{ ret i32 %x }\n!0 = !{i32 1, i64 8192}"),
               LLVMDataFlowUnsupported);
}

TEST(BinaryDiffLLVMDataFlow, DetachedAndForeignLiveObservationsRefuseSafely) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module =
      llvm::parseAssemblyString("define i32 @f(i32 %x) { ret i32 %x }\n"
                                "define i32 @other(i32 %x) { ret i32 %x }",
                                Error, Context);
  ASSERT_TRUE(Module);
  auto *F = Module->getFunction("f");
  LLVMSourceMap Sources;
  Sources.Functions[0x1000] = F;
  auto *Detached = llvm::BinaryOperator::CreateAdd(
      F->getArg(0), llvm::ConstantInt::get(F->getArg(0)->getType(), 1));
  Sources.Observations.push_back({0x1000, {0x1000, 0}, Detached});
  EXPECT_THROW(buildLLVMDataFlow(*F, Sources, 0x1000, "owned", "abi", true),
               LLVMDataFlowUnsupported);
  Detached->deleteValue();
  Sources.Observations[0].Value =
      Module->getFunction("other")->getEntryBlock().getTerminator();
  EXPECT_THROW(buildLLVMDataFlow(*F, Sources, 0x1000, "owned", "abi", true),
               LLVMDataFlowUnsupported);
}

TEST(BinaryDiffLLVMDataFlow, ContextMetadataInventoryConsumesProjectionWork) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(
      "define i32 @f(i32 %x) { ret i32 %x }", Error, Context);
  ASSERT_TRUE(Module);
  auto *F = Module->getFunction("f");
  rewrite_source::setOriginalVA(*F, 0x1000);
  for (unsigned I = 0; I < 1024; ++I)
    Context.getMDKindID("unrelated-kind-" + std::to_string(I));
  LLVMSourceMap Sources;
  Sources.Functions[0x1000] = F;
  DataFlowOptions Options;
  Options.MaxWork = 512;
  EXPECT_THROW(
      buildLLVMDataFlow(*F, Sources, 0x1000, "owned", "abi", true, Options),
      DataFlowInterrupted);
}
