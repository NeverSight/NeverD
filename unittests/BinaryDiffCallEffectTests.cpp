//===- BinaryDiffCallEffectTests.cpp - Qualified call-site evidence -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owned native/SSA snapshots exercise import, arity, origin and ABI admission.
/// These are contract tests, not binary-lifter or vulnerability qualification.
//===----------------------------------------------------------------------===//

#include "neverd/diff/CallEffectReview.h"
#include "neverd/diff/FlowEvidence.h"

#include <atomic>
#include <gtest/gtest.h>
#include <stdexcept>

using namespace neverd;
using namespace neverd::diff;
namespace {
struct Fixture {
  Program P;
  std::shared_ptr<DataFlowEvidence> SSA;
  Fixture() {
    ProgramIdentity I;
    I.Revision = "effects-fixture";
    I.Format = "elf";
    I.Architecture = "x86-64";
    I.Bits = 64;
    I.Endian = "little";
    I.ABI = std::to_string(unsigned(BinaryFormat::ELF)) + ":x86-64";
    I.Normalization = "native-x86-operands-v1";
    I.DiscoveryComplete = true;
    ProgramBuilder B(I);
    B.beginFunction(0x1000, 1, "entry", diff::NameSource::Symbol,
                    TraitEntryPoint);
    B.addBlock(0x1000);
    B.addInstruction(0x1000, {0xe8}, "call", 1, 1, InstructionFlow::Call,
                     0x2000);
    B.beginFunction(0x2000, 3, "helper", diff::NameSource::Symbol, 0);
    B.addBlock(0x2000);
    B.addInstruction(0x2000, {0xe8}, "call", 1, 1, InstructionFlow::Call,
                     0x3000);
    B.addInstruction(0x2001, {0xff}, "call", 2, 2,
                     InstructionFlow::IndirectCall, 0x4000);
    B.addInstruction(0x2002, {0xff}, "call", 2, 2,
                     InstructionFlow::IndirectCall);
    B.beginFunction(0x3000, 0, "memcpy", diff::NameSource::Import, TraitImport);
    B.beginFunction(0x4000, 0, "free", diff::NameSource::Import, TraitImport);
    B.beginFunction(0x5000, 1, "unreached", diff::NameSource::Symbol, 0);
    B.addBlock(0x5000);
    B.addInstruction(0x5000, {0xe8}, "call", 1, 1, InstructionFlow::Call,
                     0x3000);
    P = B.finish();
    DataFlowGraph G;
    G.Revision = I.Revision;
    G.ABI = I.ABI;
    G.Function = 0x2000;
    G.EntryBlock = 0;
    G.Blocks.push_back({0x2000, 0x2003});
    for (unsigned N = 0; N < 2; ++N) {
      FlowNode Call;
      Call.Opcode = N ? "INDIR_CALL" : "CALL";
      Call.Block = 0;
      Call.Address = 0x2000 + N;
      Call.OriginSequence = N;
      Call.Gaps = FlowCallEffects | FlowMemoryAlias;
      Call.CallArgumentBegin = 0;
      Call.CallArgumentCount = N ? 1 : 3;
      Call.Inputs.resize(Call.CallArgumentCount);
      for (auto &A : Call.Inputs) {
        A.Type = FlowOperand::Kind::Parameter;
        A.Bytes = 8;
      }
      G.Nodes.push_back(std::move(Call));
    }
    SSA = std::make_shared<DataFlowEvidence>();
    SSA->Entries.push_back({0x2000, FlowAvailability::Available, {}, G});
    P.DataFlow = SSA;
  }
  DataFlowGraph &graph() { return *SSA->Entries[0].Graph; }
  CallEffectReview review(const CallEffectReviewOptions &O = {}) {
    return reviewCallEffects(
        P, analyzeCallImpact(P, {0}, ImpactDirection::Callees), O);
  }
};
} // namespace

TEST(BinaryDiffCallEffects,
     ReachableImportsKeepRolesAndUnknownEffectBoundaries) {
  Fixture F;
  const auto R = F.review();
  ASSERT_TRUE(R.Complete);
  ASSERT_EQ(R.State, ComparisonState::Complete);
  ASSERT_EQ(R.Records.size(), 4u);
  EXPECT_EQ(R.Qualified, 2u);
  EXPECT_EQ(R.Records[0].Status, CallEffectStatus::InternalTarget);
  const auto &Copy = R.Records[1];
  EXPECT_EQ(Copy.Status, CallEffectStatus::Qualified);
  EXPECT_EQ(Copy.Family, safety::CallEffectFamily::Copy);
  EXPECT_EQ(Copy.Node, 0u);
  EXPECT_EQ(Copy.ArgumentCount, 3u);
  EXPECT_EQ(Copy.Destination, 0);
  EXPECT_EQ(Copy.Source, 1);
  EXPECT_EQ(Copy.Length, 2);
  EXPECT_EQ(Copy.Capacity, -1);
  EXPECT_TRUE(Copy.SSAGaps & FlowMemoryAlias);
  EXPECT_TRUE(Copy.SSAGaps & FlowCallEffects);
  EXPECT_EQ(R.Records[2].Family, safety::CallEffectFamily::Release);
  EXPECT_EQ(R.Records[2].Handle, 0);
  EXPECT_FALSE(R.Records[2].ReleaseMayFail);
  EXPECT_EQ(R.Records[3].Status, CallEffectStatus::UnresolvedTarget);
  EXPECT_EQ(R.Records[3].Family, safety::CallEffectFamily::None);
  EXPECT_EQ(F.graph().Nodes[0].Gaps, FlowMemoryAlias | FlowCallEffects);
  EXPECT_EQ(F.review().Records, R.Records);
}

TEST(BinaryDiffCallEffects,
     NamesOrIncompleteSSAAssociationsCannotGrantContracts) {
  Fixture F;
  F.P.Functions[2].Source = diff::NameSource::Symbol;
  EXPECT_EQ(F.review().Records[1].Status,
            CallEffectStatus::UnqualifiedIdentity);
  F.P.Functions[2].Source = diff::NameSource::Import;
  F.P.Functions[2].Name = "project_memcpy";
  EXPECT_EQ(F.review().Records[1].Status,
            CallEffectStatus::UnsupportedContract);
  F.P.Functions[2].Name = "memcpy";
  F.P.DataFlow.reset();
  EXPECT_EQ(F.review().Records[1].Status, CallEffectStatus::SSAUnavailable);
  F.P.DataFlow = F.SSA;
  auto &Call = F.graph().Nodes[0];
  Call.OriginSequence = -1;
  EXPECT_EQ(F.review().Records[1].Status, CallEffectStatus::MissingCall);
  Call.OriginSequence = 0;
  Call.CallArgumentBegin = NoIndex;
  Call.CallArgumentCount = 0;
  EXPECT_EQ(F.review().Records[1].Status, CallEffectStatus::MissingArguments);
  Call.CallArgumentBegin = 0;
  Call.CallArgumentCount = 2;
  EXPECT_EQ(F.review().Records[1].Status,
            CallEffectStatus::UnsupportedContract);
  Call.CallArgumentCount = 3;
  F.graph().Nodes.push_back(Call);
  EXPECT_EQ(F.review().Records[1].Status, CallEffectStatus::AmbiguousCall);
}

TEST(BinaryDiffCallEffects, ExactEnvironmentAndFallibleReleaseStayDistinct) {
  Fixture F;
  F.P.Functions[2].Name = "HeapFree";
  EXPECT_EQ(F.review().Records[1].Status,
            CallEffectStatus::UnsupportedContract);
  F.P.Identity.Format = "coff";
  F.P.Identity.ABI = std::to_string(unsigned(BinaryFormat::COFF)) + ":x86-64";
  F.graph().ABI = F.P.Identity.ABI;
  const auto Row = F.review().Records[1];
  EXPECT_EQ(Row.Status, CallEffectStatus::Qualified);
  EXPECT_EQ(Row.Family, safety::CallEffectFamily::Release);
  EXPECT_EQ(Row.Handle, 2);
  EXPECT_TRUE(Row.ReleaseMayFail);
  EXPECT_EQ(Row.Capacity, -1);
  F.P.Identity.Format = "raw";
  EXPECT_EQ(F.review().Records[1].Status, CallEffectStatus::Qualified);
  F.P.Identity.ABI = "unspecified";
  EXPECT_EQ(F.review().Records[1].Status,
            CallEffectStatus::UnsupportedEnvironment);
  F.P.Identity.Bits = 32;
  EXPECT_EQ(F.review().Records[1].Status,
            CallEffectStatus::UnsupportedEnvironment);
  F.P.Identity.Bits = 64;
  F.P.Identity.Architecture = "aarch64";
  F.P.Identity.ABI = std::to_string(unsigned(BinaryFormat::COFF)) + ":aarch64";
  F.P.Identity.Normalization = "native-aarch64-operands-v1";
  F.graph().ABI = F.P.Identity.ABI;
  EXPECT_EQ(F.review().Records[1].Status,
            CallEffectStatus::UnsupportedEnvironment);
  EXPECT_EQ(F.review().Qualified, 0u);
}

TEST(BinaryDiffCallEffects,
     FiniteScopesKeepFrontierContractsAndStoppedPrefixes) {
  Fixture F;
  ImpactOptions Scope;
  Scope.MaxDepth = 1;
  const auto Native =
      analyzeCallImpact(F.P, {0}, ImpactDirection::Callees, Scope);
  ASSERT_FALSE(Native.TraversalComplete);
  ASSERT_FALSE(Native.FrontierSites.empty());
  const auto Review = reviewCallEffects(F.P, Native);
  EXPECT_TRUE(Review.Complete); // Inspection only; native scope stays partial.
  EXPECT_EQ(Review.Qualified, 2u);
  CallEffectReviewOptions O;
  O.MaxRecords = 2;
  const auto Limited = F.review(O);
  EXPECT_FALSE(Limited.Complete);
  EXPECT_EQ(Limited.State, ComparisonState::ResourceLimited);
  EXPECT_EQ(Limited.Records.size(), 2u);
  O.MaxWork = 1;
  EXPECT_EQ(F.review(O).State, ComparisonState::ResourceLimited);
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  EXPECT_EQ(reviewCallEffects(F.P, Native, {}, Control).State,
            ComparisonState::Cancelled);
  Cancel = false;
  Control.Deadline = std::chrono::steady_clock::now();
  EXPECT_EQ(reviewCallEffects(F.P, Native, {}, Control).State,
            ComparisonState::TimedOut);
  O.MaxRecords = 0;
  EXPECT_THROW(F.review(O), std::invalid_argument);
}

TEST(BinaryDiffCallEffects, ForeignGraphsAndSiteOwnersFailClearly) {
  Fixture F;
  F.graph().Revision = "foreign";
  EXPECT_THROW(F.review(), std::invalid_argument);
  F.graph().Revision = F.P.Identity.Revision;
  F.graph().Nodes[0].Address = 0x5000;
  EXPECT_THROW(F.review(), std::invalid_argument);
  F.graph().Nodes[0].Address = 0x2000;
  auto Native = analyzeCallImpact(F.P, {0}, ImpactDirection::Callees);
  Native.Sites[1].Caller = 0;
  EXPECT_THROW(reviewCallEffects(F.P, Native), std::invalid_argument);
}
