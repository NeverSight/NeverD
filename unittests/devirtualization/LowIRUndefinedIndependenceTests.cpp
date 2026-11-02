//===- LowIRUndefinedIndependenceTests.cpp - Relational certificates
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/LowIRUndefinedIndependence.h"

#include <iterator>

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRIndependenceStatus;

NdVar reg(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar number(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp op(NdOp Code, NdVar Output = {},
         std::initializer_list<NdVar> Inputs = {}) {
  LowOp Value;
  Value.Opcode = Code;
  Value.Output = Output;
  for (const auto &Input : Inputs)
    Value.addInput(Input);
  return Value;
}

struct Program {
  LowFunc Function;
  std::vector<LowIRUndefinedInstruction> Records;
  LowIRIndependenceContract Contract;

  Program() {
    Function.Entry = 0x100;
    Contract.ReturnRegisters = {{0, 8}};
    block(0, 0x100);
  }

  void block(int Id, va_t Address, std::vector<int> Successors = {}) {
    LowBlock B;
    B.Id = Id;
    B.StartAddr = B.EndAddr = Address;
    B.Succs = std::move(Successors);
    Function.Blocks.push_back(std::move(B));
  }

  LowIRUndefinedInstruction &instruction(std::initializer_list<LowOp> Ops) {
    auto &B = Function.Blocks.back();
    LowInstructionBoundary Boundary;
    Boundary.Address = B.EndAddr++;
    Boundary.Size = 1;
    Boundary.FirstOp = B.Ops.size();
    Boundary.OpCount = Ops.size();
    for (auto Value : Ops) {
      Value.Addr = Boundary.Address;
      Value.Seq = static_cast<int>(B.Ops.size() - Boundary.FirstOp);
      if (Value.Opcode == NdOp::BRANCH || Value.Opcode == NdOp::COND_BR ||
          Value.Opcode == NdOp::INDIR_BR) {
        Boundary.Control = LowInstructionControl::Branch;
        Boundary.ControlFlags = LowInstructionControlFlag::Branch;
        if (Value.Opcode == NdOp::COND_BR)
          Boundary.ControlFlags |= LowInstructionControlFlag::Conditional;
        if (Value.Opcode == NdOp::INDIR_BR)
          Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
        else
          Boundary.Immediate = Value.Inputs[0].Offset;
      } else if (Value.Opcode == NdOp::RETURN) {
        Boundary.Control = LowInstructionControl::Return;
        Boundary.ControlFlags = LowInstructionControlFlag::Return;
      }
      B.Ops.push_back(Value);
    }
    B.InstructionBoundaries.push_back(Boundary);
    LowIRUndefinedInstruction Record;
    Record.BlockId = B.Id;
    Record.Boundary = Boundary;
    Record.Effects.Coverage = LowUndefinedCoverage::Complete;
    Record.Effects.OpCount = Ops.size();
    Record.Effects.OperationDigest = lowUndefinedOperationDigest(
        llvm::ArrayRef<LowOp>(B.Ops).slice(Boundary.FirstOp, Boundary.OpCount));
    Records.push_back(std::move(Record));
    return Records.back();
  }

  void arbitrary(NdVar Output, uint16_t BitOffset = 0, uint16_t BitCount = 1) {
    auto &Record = instruction({op(NdOp::NOP)});
    Record.Effects.Effects.push_back({1, Output, BitOffset, BitCount, {}});
  }

  void finish() { instruction({op(NdOp::RETURN)}); }

  void rebind() {
    for (auto &R : Records)
      for (const auto &B : Function.Blocks)
        if (B.Id == R.BlockId)
          R.Effects.OperationDigest =
              lowUndefinedOperationDigest(llvm::ArrayRef<LowOp>(B.Ops).slice(
                  R.Boundary.FirstOp, R.Boundary.OpCount));
  }

  void frame() { Contract.Frame = LowIRIndependenceFrame{{32, 8}, -16, 16}; }

  LowIRIndependenceResult check(const LowIRIndependenceLimits &Limits = {}) {
    return checkLowIRUndefinedIndependence(Function, Records, Contract, Limits);
  }
};

void expectStatus(Program &P, Status Expected,
                  const LowIRIndependenceLimits &Limits = {}) {
  const auto Result = P.check(Limits);
  EXPECT_EQ(Result.Status, Expected) << Result.Diagnostic;
  EXPECT_EQ(Result.Certificate.has_value(), Expected == Status::Proved);
}

TEST(LowIRUndefinedIndependence,
     CopiesShareOneProducerButSeparateEffectsDoNot) {
  Program Shared;
  Shared.arbitrary(reg(8), 0, 64);
  Shared.instruction({op(NdOp::COPY, reg(16), {reg(8)}),
                      op(NdOp::INT_XOR, reg(0), {reg(8), reg(16)})});
  Shared.finish();
  expectStatus(Shared, Status::Proved);

  Program Fresh;
  Fresh.arbitrary(reg(8), 0, 64);
  Fresh.instruction({op(NdOp::COPY, reg(16), {reg(8)})});
  // Reusing the destination must create a new producer, not reuse its name.
  Fresh.arbitrary(reg(8), 0, 64);
  Fresh.instruction({op(NdOp::INT_XOR, reg(0), {reg(8), reg(16)})});
  Fresh.finish();
  expectStatus(Fresh, Status::Dependent);
}

TEST(LowIRUndefinedIndependence,
     UnauditedNativeBoundaryOptionRejectsStaticAPI) {
  Program P;
  P.instruction({op(NdOp::COPY, reg(0), {number(7)})});
  P.finish();
  expectStatus(P, Status::Proved);
  P.Contract.RetainUnauditedNativeBoundaries = true;
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence, NativeOverlapOptionRejectsStaticAPI) {
  Program P;
  P.instruction({op(NdOp::COPY, reg(0), {number(7)})});
  P.finish();
  expectStatus(P, Status::Proved);
  P.Contract.AllowOverlappingNativeInstructions = true;
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence, DeferredNativeEdgesRejectStaticAPI) {
  Program P;
  P.instruction({op(NdOp::COPY, reg(0), {number(7)})});
  P.finish();
  expectStatus(P, Status::Proved);
  P.Contract.DeferNativeConditionalEdges = true;
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence, PartialRegisterOverwritePreservesOtherBits) {
  Program P;
  P.arbitrary(reg(0), 32, 1);
  P.instruction({op(NdOp::COPY, reg(0, 4), {number(0, 4)})});
  P.finish();
  expectStatus(P, Status::Dependent);
  P.Contract.ReturnRegisters = {{0, 4}};
  expectStatus(P, Status::Proved);
}

TEST(LowIRUndefinedIndependence,
     SpillReloadKeepsCorrelationAndWrittenBytesCount) {
  Program P;
  P.frame();
  P.arbitrary(reg(8), 0, 64);
  const auto Address = NdVar::tmp(0, 8);
  P.instruction({op(NdOp::INT_SUB, Address, {reg(32), number(4)}),
                 op(NdOp::STORE, {}, {Address, reg(8)}),
                 op(NdOp::LOAD, reg(16), {Address}),
                 op(NdOp::INT_XOR, reg(0), {reg(16), reg(8)})});
  P.finish();
  expectStatus(P,
               Status::Dependent); // The final spill is observable by default.
  P.Contract.ObserveWrittenFrameBytes = false;
  expectStatus(P, Status::Proved);

  Program Killed;
  Killed.frame();
  Killed.arbitrary(reg(8), 0, 64);
  Killed.instruction({op(NdOp::INT_SUB, Address, {reg(32), number(4)}),
                      op(NdOp::STORE, {}, {Address, reg(8)}),
                      op(NdOp::LOAD, reg(16), {Address}),
                      op(NdOp::INT_XOR, reg(0), {reg(16), reg(8)}),
                      op(NdOp::STORE, {}, {Address, number(0)})});
  Killed.finish();
  expectStatus(Killed, Status::Proved);
}

TEST(LowIRUndefinedIndependence, PartialFrameOverwriteDoesNotEraseHighBytes) {
  Program P;
  P.frame();
  P.arbitrary(reg(8), 0, 64);
  P.instruction({op(NdOp::STORE, {}, {reg(32), reg(8)}),
                 op(NdOp::STORE, {}, {reg(32), number(0, 4)}),
                 op(NdOp::LOAD, reg(0, 4), {reg(32)})});
  P.finish();
  P.Contract.ReturnRegisters = {{0, 4}};
  expectStatus(P, Status::Dependent);
  P.Contract.ObserveWrittenFrameBytes = false;
  expectStatus(P, Status::Proved);
  P.Contract.ReturnRegisters = {{0, 8}};
  // Changing the LOAD, not only the observation, exposes the surviving bytes.
  P.Function.Blocks[0].Ops[3].Output = reg(0);
  P.rebind();
  expectStatus(P, Status::Dependent);
}

Program diamond(NdVar Condition) {
  Program P;
  P.Function.Blocks[0].Succs = {1, 2};
  P.instruction({op(NdOp::COND_BR, {}, {number(0x200), Condition})});
  P.block(1, 0x200);
  P.instruction({op(NdOp::COPY, reg(0), {number(0)})});
  P.finish();
  P.block(2, 0x300);
  P.instruction({op(NdOp::COPY, reg(0), {number(0)})});
  P.finish();
  return P;
}

TEST(LowIRUndefinedIndependence, BranchIsCheckedBeforeItsOwnPathAssumption) {
  auto P = diamond(reg(8, 1));
  P.Records[0].Effects.Effects.push_back({0, reg(8, 1), 0, 8, {}});
  // Both arms return zero; the strict certificate still observes control.
  expectStatus(P, Status::Dependent);
  auto Shared = diamond(reg(8, 1));
  const auto Result = Shared.check();
  EXPECT_EQ(Result.Status, Status::Proved) << Result.Diagnostic;
  EXPECT_EQ(Result.Paths, 2u);
}

TEST(LowIRUndefinedIndependence, UnreachableUnsupportedArmDoesNotPoisonProof) {
  auto P = diamond(number(0, 1));
  P.Records[1].Effects.Coverage = LowUndefinedCoverage::Unsupported;
  P.Records[1].Effects.Diagnostic = "synthetic unsupported instruction";
  expectStatus(P, Status::Proved);
  P.Function.Blocks[0].Ops[0].Inputs[1] = number(1, 1);
  P.rebind();
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence, FingerprintBindsEveryExceptionalContextPair) {
  auto P = diamond(number(0, 1));
  auto &Edges = P.Function.Blocks[1].ExceptionalSuccs;
  Edges.emplace_back(-1, 0x8000, ExceptionalEdgeKind::CxxCleanup);
  Edges.front().SharedContexts =
      std::make_shared<const ExceptionalEdgeContextDomain>(
          std::vector<ExceptionalEdgeContext>{{0, 3}, {1, 4}});
  // The exceptional block is infeasible. Its full metadata still belongs to
  // this input fingerprint, without granting support to a reachable edge.
  auto A = P.check();
  ASSERT_TRUE(A.proved()) << A.Diagnostic;
  ASSERT_TRUE(A.Certificate);
  Edges.front().SharedContexts =
      std::make_shared<const ExceptionalEdgeContextDomain>(
          std::vector<ExceptionalEdgeContext>{{1, 4}, {0, 3}, {1, 4}});
  auto B = P.check();
  ASSERT_TRUE(B.proved()) << B.Diagnostic;
  EXPECT_EQ(A.Certificate->InputDigest, B.Certificate->InputDigest);
  Edges.front().SharedContexts =
      std::make_shared<const ExceptionalEdgeContextDomain>(
          std::vector<ExceptionalEdgeContext>{{0, 4}, {1, 3}});
  auto C = P.check();
  ASSERT_TRUE(C.proved()) << C.Diagnostic;
  EXPECT_NE(A.Certificate->InputDigest, C.Certificate->InputDigest);
  Edges.front().SharedContexts =
      std::make_shared<const ExceptionalEdgeContextDomain>(
          std::vector<ExceptionalEdgeContext>{{0, 3}});
  auto D = P.check();
  ASSERT_TRUE(D.proved()) << D.Diagnostic;
  Edges.front().SharedContexts.reset();
  Edges.front().ExplicitContext = {0, 3};
  auto E = P.check();
  ASSERT_TRUE(E.proved()) << E.Diagnostic;
  EXPECT_EQ(D.Certificate->InputDigest, E.Certificate->InputDigest);
  P.Function.Blocks[0].Ops[0].Inputs[1] = number(1, 1);
  P.rebind();
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence, ReachableCyclesRefuseWithoutUnrolling) {
  Program P;
  P.Function.Blocks[0].Succs = {0, 1};
  P.instruction({op(NdOp::COND_BR, {}, {number(0x100), number(0, 1)})});
  P.block(1, 0x200);
  P.finish();
  expectStatus(P, Status::Proved);
  P.Function.Blocks[0].Ops[0].Inputs[1] = reg(8, 1);
  P.rebind();
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence, SidecarMustBindCompleteBoundaryAndCoverage) {
  Program P;
  P.finish();
  P.Records[0].Effects.Coverage = LowUndefinedCoverage::Missing;
  expectStatus(P, Status::Unsupported);
  P.Records[0].Effects.Coverage = LowUndefinedCoverage::Complete;
  ++P.Records[0].Effects.OpCount;
  expectStatus(P, Status::Invalid);
  --P.Records[0].Effects.OpCount;
  ++P.Records[0].Boundary.Size;
  expectStatus(P, Status::Invalid);
  --P.Records[0].Boundary.Size;
  P.Records.clear();
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence,
     ConditionalProducerUsesDefinedBooleanSnapshot) {
  Program P;
  auto &R =
      P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {reg(8), number(1)}),
                     op(NdOp::COPY, reg(0), {number(0)})});
  R.Effects.Effects.push_back({1, reg(0), 0, 1, NdVar::tmp(0, 1)});
  P.finish();
  expectStatus(P, Status::Proved); // The later ordinary write kills the choice.
  P.Records[0].Effects.Effects[0].When = reg(8, 1);
  expectStatus(P, Status::Invalid);
  P.Records[0].Effects.Effects[0].When = NdVar::tmp(2, 1);
  expectStatus(P, Status::Invalid);
  P.Records[0].Effects.Effects[0].When = number(2, 1);
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence, FalseConditionalProducerPreservesOldValue) {
  Program P;
  auto &R = P.instruction({op(NdOp::COPY, reg(0), {number(42)})});
  R.Effects.Effects.push_back({1, reg(0), 0, 64, number(0, 1)});
  P.finish();
  expectStatus(P, Status::Proved);
  P.Records[0].Effects.Effects[0].When = number(1, 1);
  expectStatus(P, Status::Dependent);
}

TEST(LowIRUndefinedIndependence,
     TemporaryIdentityCannotLeakAcrossInstructions) {
  Program P;
  P.instruction({op(NdOp::COPY, NdVar::tmp(0, 8), {number(0)})});
  P.instruction({op(NdOp::COPY, reg(0), {NdVar::tmp(0, 8)})});
  P.finish();
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence, UnknownAliasAndDependentAddressRefuse) {
  Program P;
  P.frame();
  P.instruction({op(NdOp::STORE, {}, {reg(48), number(0)})});
  P.finish();
  expectStatus(P, Status::Unsupported);

  Program Choice;
  Choice.frame();
  Choice.arbitrary(reg(8), 0, 64);
  Choice.instruction({op(NdOp::LOAD, reg(0), {reg(8)})});
  Choice.finish();
  expectStatus(Choice, Status::Dependent);
}

TEST(LowIRUndefinedIndependence, EntryNonwrappingContractIsNotVacuousSuccess) {
  Program P;
  P.frame();
  P.Contract.EntryConstants.push_back({reg(32), 0});
  P.finish();
  expectStatus(P, Status::InfeasibleEntry);
  P.Contract.EntryConstants[0].Value = 0x1000;
  expectStatus(P, Status::Proved);
}

TEST(LowIRUndefinedIndependence, AllBudgetsFailWithoutPublishingCertificate) {
  Program P;
  P.frame();
  P.arbitrary(reg(8));
  P.finish();
  LowIRIndependenceLimits L;
  L.MaxOperations = 1;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxProducers = 0;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxFrameBytes = 1;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxSolverQueries = 0;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxPaths = 0;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxBlockVisits = 0;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxSymbolicNodes = 0;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxInstructions = 1;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxObservations = 0;
  expectStatus(P, Status::BudgetExceeded, L);
}

TEST(LowIRUndefinedIndependence, GraphMetadataIsBoundedAndWellFormed) {
  Program P;
  P.finish();
  LowIRIndependenceLimits Limits;
  Limits.MaxBlockVisits = 1;
  P.Function.ModuleAnalysisRoots = {P.Function.Entry, P.Function.Entry + 1};
  expectStatus(P, Status::BudgetExceeded, Limits);
  P.Function.ModuleAnalysisRoots.clear();
  P.Function.Blocks.front().Succs = {1, 2, 3};
  expectStatus(P, Status::BudgetExceeded, Limits);
  P.Function.Blocks.front().Succs = {1};
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence, PartialOverwriteUsesDeclaredByteOrder) {
  Program P;
  P.arbitrary(reg(0), 0, 1);
  P.instruction({op(NdOp::COPY, reg(0, 1), {number(0, 1)})});
  P.finish();
  expectStatus(P, Status::Proved);
  P.Contract.ByteOrder = llvm::endianness::big;
  expectStatus(P, Status::Dependent);
  P.Function.Blocks[0].Ops[1].Output = reg(7, 1);
  P.rebind();
  expectStatus(P, Status::Proved);
}

TEST(LowIRUndefinedIndependence, CertificateBindsOperationsEffectsAndContract) {
  Program P;
  P.instruction({op(NdOp::COPY, reg(0), {number(0)})});
  P.finish();
  auto A = P.check();
  ASSERT_TRUE(A.proved()) << A.Diagnostic;
  ASSERT_TRUE(A.Certificate);
  EXPECT_EQ(A.Certificate->Scope, LowIRIndependenceScope::CompleteAcyclicLowIR);
  EXPECT_EQ(A.Certificate->Instructions.size(), P.Records.size());
  EXPECT_EQ(A.Certificate->InputDigest.size(), 64u);
  P.Function.Blocks[0].Ops[0].Inputs[0] = number(1);
  expectStatus(P, Status::Invalid); // Same-length code cannot reuse a sidecar.
  P.rebind();
  auto B = P.check();
  ASSERT_TRUE(B.proved()) << B.Diagnostic;
  EXPECT_NE(A.Certificate->InputDigest, B.Certificate->InputDigest);
  P.Records[0].Effects.Effects.push_back({0, reg(8), 0, 1, {}});
  auto C = P.check();
  ASSERT_TRUE(C.proved()) << C.Diagnostic;
  EXPECT_NE(B.Certificate->InputDigest, C.Certificate->InputDigest);
  P.Contract.ObserveWrittenFrameBytes = false;
  auto D = P.check();
  ASSERT_TRUE(D.proved()) << D.Diagnostic;
  EXPECT_NE(C.Certificate->InputDigest, D.Certificate->InputDigest);
  P.Function.ModuleAnalysisRoots.insert(P.Function.Entry);
  auto E = P.check();
  ASSERT_TRUE(E.proved()) << E.Diagnostic;
  EXPECT_NE(D.Certificate->InputDigest, E.Certificate->InputDigest);
  P.Function.OrdinaryModuleAnalysisRoots.insert(P.Function.Entry);
  auto F = P.check();
  ASSERT_TRUE(F.proved()) << F.Diagnostic;
  EXPECT_NE(E.Certificate->InputDigest, F.Certificate->InputDigest);
  LowIRIndependenceLimits Limits;
  Limits.MaxOperations += 1;
  auto G = P.check(Limits);
  ASSERT_TRUE(G.proved()) << G.Diagnostic;
  EXPECT_EQ(G.Certificate->Limits.MaxOperations, Limits.MaxOperations);
  EXPECT_NE(F.Certificate->InputDigest, G.Certificate->InputDigest);
}

TEST(LowIRUndefinedIndependence, OpaqueTerminatorMetadataCannotBecomeNop) {
  Program P;
  P.instruction({op(NdOp::NOP)});
  auto &Boundary = P.Function.Blocks[0].InstructionBoundaries[0];
  Boundary.Control = LowInstructionControl::Terminator;
  Boundary.ControlFlags = LowInstructionControlFlag::Terminator;
  P.Records[0].Boundary = Boundary;
  P.finish();
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence,
     MalformedOperandCapacityAndWrappingTemporaryRefuse) {
  Program P;
  P.instruction({op(NdOp::COPY, reg(0), {number(0)})});
  P.finish();
  auto &Malformed = P.Function.Blocks[0].Ops[0];
  Malformed.NumInputs = std::size(Malformed.Inputs) + 1;
  expectStatus(P, Status::Invalid);
  P.Function.Blocks[0].Ops[0].NumInputs = 1;
  P.Function.Blocks[0].Ops[0].Inputs[0] = NdVar::tmp(UINT64_MAX, 1);
  P.rebind();
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence,
     ExtendedOperandStorageRetainsUnsupportedEffectRefusal) {
  LowOp Opaque = op(NdOp::INTRINSIC, reg(0));
  for (size_t I = 0; I != std::size(Opaque.Inputs); ++I)
    Opaque.addInput(number(0));
  Program P;
  P.instruction({Opaque});
  P.finish();
  expectStatus(P, Status::Unsupported);
  auto &Malformed = P.Function.Blocks[0].Ops[0];
  Malformed.NumInputs = std::size(Malformed.Inputs) + 1;
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence,
     EmptyOperationSpansStillConsumeInstructionBudget) {
  Program P;
  P.instruction({});
  P.instruction({});
  P.finish();
  expectStatus(P, Status::Proved);
  LowIRIndependenceLimits L;
  L.MaxInstructions = 2;
  expectStatus(P, Status::BudgetExceeded, L);
}

TEST(LowIRUndefinedIndependence, CorrelationsSurviveControlEdges) {
  Program P;
  P.Function.Blocks[0].Succs = {1};
  P.arbitrary(reg(8), 0, 64);
  P.instruction({op(NdOp::COPY, reg(16), {reg(8)}),
                 op(NdOp::BRANCH, {}, {number(0x200)})});
  P.block(1, 0x200);
  P.instruction({op(NdOp::INT_XOR, reg(0), {reg(8), reg(16)})});
  P.finish();
  expectStatus(P, Status::Proved);
  P.Function.Blocks[1].Ops[0].Inputs[1] = number(0);
  P.rebind();
  expectStatus(P, Status::Dependent);
}

TEST(LowIRUndefinedIndependence,
     IndependentDynamicTargetNeedsItsOwnResolutionProof) {
  Program P;
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::INDIR_BR, {}, {reg(8)})});
  P.block(1, 0x200);
  P.finish();
  // A finite CFG successor list must not justify an unproved target value.
  expectStatus(P, Status::Unsupported);
  P.Contract.EntryConstants.push_back({reg(8), 0x200});
  expectStatus(P, Status::Proved);
  P.Contract.EntryConstants[0].Value = 0x201;
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence, AdditionalEntriesAreNotSilentlyOmitted) {
  auto P = diamond(number(0, 1));
  P.Function.ModuleAnalysisRoots = {0x100};
  P.Function.OrdinaryModuleAnalysisRoots = {0x100};
  expectStatus(P, Status::Proved);
  P.Function.OrdinaryModuleAnalysisRoots.insert(0x200);
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence, PairwiseEqualStackChangesViolatePreservation) {
  Program P;
  P.frame();
  P.instruction({op(NdOp::INT_ADD, reg(32), {reg(32), number(8)})});
  P.finish();
  expectStatus(P, Status::Proved); // Both executions make the same change.
  P.Contract.PreservedRegisters = {{32, 8}};
  expectStatus(P, Status::ContractViolation);

  Program Restored;
  Restored.frame();
  Restored.Contract.PreservedRegisters = {{32, 8}};
  Restored.instruction({op(NdOp::COPY, reg(40), {reg(32)}),
                        op(NdOp::INT_ADD, reg(32), {reg(32), number(8)}),
                        op(NdOp::COPY, reg(32), {reg(40)})});
  Restored.finish();
  expectStatus(Restored, Status::Proved);
}

TEST(LowIRUndefinedIndependence, PreservedSnapshotsIncludeEntryConstants) {
  Program P;
  P.Contract.PreservedRegisters = {{0, 8}};
  P.instruction({op(NdOp::COPY, reg(0), {number(42)})});
  P.finish();
  expectStatus(P, Status::ContractViolation);
  P.Contract.EntryConstants = {{reg(0), 42}};
  expectStatus(P, Status::Proved);
  P.Contract.EntryConstants[0].Value = 43;
  expectStatus(P, Status::ContractViolation);

  // Only a low lane is initially bound; snapshotting must keep that binding
  // together with the shared, unbound high bytes of the same register.
  Program Partial;
  Partial.Contract.EntryConstants = {{reg(0, 4), 42}};
  Partial.Contract.PreservedRegisters = {{0, 8}};
  Partial.instruction({op(NdOp::COPY, reg(0, 4), {number(42, 4)})});
  Partial.finish();
  expectStatus(Partial, Status::Proved);
}

TEST(LowIRUndefinedIndependence,
     PreservedUndefinedOutputsAreContractViolations) {
  Program P;
  P.Contract.PreservedRegisters = {{0, 8}};
  P.arbitrary(reg(0), 0, 64);
  P.finish();
  // Preservation is checked before the ordinary return-register observation.
  expectStatus(P, Status::ContractViolation);

  Program Restored;
  Restored.Contract.PreservedRegisters = {{0, 8}};
  Restored.instruction({op(NdOp::COPY, reg(8), {reg(0)})});
  Restored.arbitrary(reg(0), 0, 64);
  Restored.instruction({op(NdOp::COPY, reg(0), {reg(8)})});
  Restored.finish();
  expectStatus(Restored, Status::Proved);
}

TEST(LowIRUndefinedIndependence, EntryReturnSlotMustRestoreItsOriginalBytes) {
  Program P;
  P.frame();
  P.Contract.ObserveWrittenFrameBytes = false;
  P.instruction({op(NdOp::STORE, {}, {reg(32), number(0x1234)})});
  P.finish();
  expectStatus(P, Status::Proved);
  P.Contract.PreservedFrameRanges = {{0, 8}};
  expectStatus(P, Status::ContractViolation);

  Program Restored;
  Restored.frame();
  Restored.Contract.PreservedFrameRanges = {{0, 8}};
  Restored.instruction({op(NdOp::LOAD, reg(8), {reg(32)}),
                        op(NdOp::STORE, {}, {reg(32), number(0x1234)}),
                        op(NdOp::STORE, {}, {reg(32), reg(8)})});
  Restored.finish();
  expectStatus(Restored, Status::Proved);
  Restored.Function.Blocks[0].Ops[2].Inputs[1] = number(0);
  expectStatus(Restored,
               Status::Invalid); // The former effect binding is stale.
  Restored.rebind();
  expectStatus(Restored, Status::ContractViolation);
}

TEST(LowIRUndefinedIndependence, PreservationCoversEveryFeasibleReturnPath) {
  auto P = diamond(reg(8, 1));
  P.Contract.PreservedRegisters = {{0, 8}};
  P.Contract.EntryConstants = {{reg(0), 0}};
  // Only one return path breaks preservation; completing the other path
  // cannot authorize a partial certificate.
  P.Function.Blocks[1].Ops[0].Inputs[0] = number(1);
  P.rebind();
  expectStatus(P, Status::ContractViolation);
  P.Function.Blocks[0].Ops[0].Inputs[1] = number(0, 1);
  P.rebind();
  expectStatus(P, Status::Proved);
  P.Function.Blocks[0].Ops[0].Inputs[1] = number(1, 1);
  P.rebind();
  expectStatus(P, Status::ContractViolation);
}

TEST(LowIRUndefinedIndependence,
     PreservationUsesOnlyIndependentlyProvedGuards) {
  Program P;
  P.Contract.PreservedRegisters = {{0, 8}};
  P.Function.Blocks[0].Succs = {1, 2};
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {reg(0), number(0)}),
                 op(NdOp::COND_BR, {}, {number(0x200), NdVar::tmp(0, 1)})});
  P.block(1, 0x200);
  P.instruction({op(NdOp::COPY, reg(0), {number(0)})});
  P.finish();
  P.block(2, 0x300);
  P.finish();
  expectStatus(P, Status::Proved); // The written arm already had entry RAX=0.
}

TEST(LowIRUndefinedIndependence, PreservedFrameSlicesAreByteExactAcrossZero) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    Program P;
    P.frame();
    P.Contract.ByteOrder = Order;
    P.Contract.PreservedFrameRanges = {{-4, 8}};
    const auto Address = NdVar::tmp(0, 8);
    P.instruction({op(NdOp::INT_SUB, Address, {reg(32), number(4)}),
                   op(NdOp::LOAD, reg(8), {Address}),
                   op(NdOp::STORE, {}, {Address, number(0)}),
                   op(NdOp::STORE, {}, {Address, reg(8)})});
    P.finish();
    expectStatus(P, Status::Proved);
    // Restoring just four bytes cannot restore the original eight-byte slice.
    P.Function.Blocks[0].Ops[3].Inputs[1] = reg(8, 4);
    P.rebind();
    expectStatus(P, Status::ContractViolation);
  }
}

TEST(LowIRUndefinedIndependence,
     PreservedRangesRejectMalformedAndOverlappingInput) {
  Program P;
  P.finish();
  P.Contract.PreservedRegisters = {{0, 0}};
  expectStatus(P, Status::Invalid);
  P.Contract.PreservedRegisters = {{0, 9}};
  expectStatus(P, Status::Invalid);
  P.Contract.PreservedRegisters = {{UINT64_MAX, 2}};
  expectStatus(P, Status::Invalid);
  P.Contract.PreservedRegisters = {{0, 8}, {7, 1}};
  expectStatus(P, Status::Invalid);
  P.Contract.PreservedRegisters = {{0, 4}, {4, 4}};
  expectStatus(P, Status::Proved);
  P.Contract.PreservedFrameRanges = {{0, 8}};
  expectStatus(P, Status::Invalid); // There is no frame contract yet.
  P.frame();
  for (auto Range : {LowIRIndependenceFrameRange{0, 0},
                     {-17, 1},
                     {15, 2},
                     {INT64_MAX, 1},
                     {INT64_MIN, UINT16_MAX}}) {
    P.Contract.PreservedFrameRanges = {Range};
    expectStatus(P, Status::Invalid);
  }
  P.Contract.PreservedFrameRanges = {{-1, 2}, {0, 1}};
  expectStatus(P, Status::Invalid);
  P.Contract.PreservedFrameRanges = {{-16, 8}, {-8, 8}, {0, 8}, {8, 8}};
  expectStatus(P, Status::Proved);
}

TEST(LowIRUndefinedIndependence,
     PreservationSnapshotAndBothExecutionChecksAreBounded) {
  Program P;
  P.frame();
  P.Contract.ReturnRegisters.clear();
  P.Contract.PreservedFrameRanges = {{0, 8}};
  P.finish();
  LowIRIndependenceLimits L;
  L.MaxObservations = 7;
  expectStatus(P, Status::BudgetExceeded, L); // Eight entry snapshot bytes.
  L.MaxObservations = 15;
  expectStatus(P, Status::BudgetExceeded, L); // Both executions are checked.
  L.MaxObservations = 16;
  expectStatus(P, Status::Proved, L);
  L = {};
  L.MaxInstructions = 1;
  P.Contract.PreservedFrameRanges = {{0, 1}, {1, 1}};
  expectStatus(P, Status::BudgetExceeded, L);
  P.Contract.PreservedFrameRanges.clear();
  P.Contract.PreservedRegisters = {{0, 1}, {1, 1}};
  expectStatus(P, Status::BudgetExceeded, L);
}

TEST(LowIRUndefinedIndependence, CertificateBindsEveryPreservationRange) {
  Program P;
  P.frame();
  P.finish();
  auto A = P.check();
  ASSERT_TRUE(A.proved()) << A.Diagnostic;
  P.Contract.PreservedRegisters = {{32, 8}};
  auto B = P.check();
  ASSERT_TRUE(B.proved()) << B.Diagnostic;
  EXPECT_NE(A.Certificate->InputDigest, B.Certificate->InputDigest);
  EXPECT_EQ(B.Certificate->Contract.PreservedRegisters.size(), 1u);
  P.Contract.PreservedRegisters[0].Bytes = 4;
  auto C = P.check();
  ASSERT_TRUE(C.proved()) << C.Diagnostic;
  EXPECT_NE(B.Certificate->InputDigest, C.Certificate->InputDigest);
  P.Contract.PreservedFrameRanges = {{0, 8}};
  auto D = P.check();
  ASSERT_TRUE(D.proved()) << D.Diagnostic;
  EXPECT_NE(C.Certificate->InputDigest, D.Certificate->InputDigest);
  EXPECT_EQ(D.Certificate->Contract.PreservedFrameRanges[0].Offset, 0);
  P.Contract.PreservedFrameRanges[0].Offset = -8;
  auto E = P.check();
  ASSERT_TRUE(E.proved()) << E.Diagnostic;
  EXPECT_NE(D.Certificate->InputDigest, E.Certificate->InputDigest);
  P.Contract.PreservedFrameRanges[0].Bytes = 4;
  auto F = P.check();
  ASSERT_TRUE(F.proved()) << F.Diagnostic;
  EXPECT_NE(E.Certificate->InputDigest, F.Certificate->InputDigest);
}

TEST(LowIRUndefinedIndependence,
     ExclusionsCoverTheWholeFrameWithoutMemoryAccesses) {
  Program P;
  P.frame();
  P.Contract.EntryConstants = {{reg(32), 0x1000}};
  P.finish();
  // The accessible frame is [0x0ff0, 0x1010), even though no LOAD or STORE
  // runs.
  P.Contract.Frame->ExcludedAddressRanges = {{0x100f, 0x1010}};
  expectStatus(P, Status::InfeasibleEntry);
  P.Contract.Frame->ExcludedAddressRanges = {{0x0ff0, 0x0ff1}};
  expectStatus(P, Status::InfeasibleEntry);
  P.Contract.Frame->ExcludedAddressRanges = {{0x1000, 0x1001}};
  expectStatus(P, Status::InfeasibleEntry);
  P.Contract.Frame->ExcludedAddressRanges.clear();
  expectStatus(P, Status::Proved);
}

TEST(LowIRUndefinedIndependence, ExclusionAdjacencyIsAllowedOnEitherSide) {
  Program P;
  P.Contract.Frame = LowIRIndependenceFrame{{32, 8}, -8, 8};
  P.Contract.EntryConstants = {{reg(32), 0x1008}};
  P.finish();
  // Accessible bytes [0x1000, 0x1010) fit exactly between these ranges.
  P.Contract.Frame->ExcludedAddressRanges = {{0, 0x1000}, {0x1010, 0x1020}};
  expectStatus(P, Status::Proved);
  P.Contract.Frame->ExcludedAddressRanges[0].End = 0x1001;
  expectStatus(P, Status::InfeasibleEntry);
  P.Contract.Frame->ExcludedAddressRanges[0].End = 0x1000;
  P.Contract.Frame->ExcludedAddressRanges[1].Begin = 0x100f;
  expectStatus(P, Status::InfeasibleEntry);
}

TEST(LowIRUndefinedIndependence,
     ExclusionsConstrainASymbolicRootWithoutAssumingIt) {
  Program P;
  P.Contract.Frame = LowIRIndependenceFrame{{32, 8}, -8, 8};
  P.Contract.PreservedRegisters = {{32, 8}};
  P.instruction({op(NdOp::COPY, reg(32), {number(0x1008)})});
  P.finish();
  expectStatus(P, Status::ContractViolation);
  // The only location for all sixteen accessible bytes is [0x1000, 0x1010).
  // No entry constant pins RSP. A root-only exclusion check would be too weak
  // to prove that writing 0x1008 restores every permitted entry root.
  P.Contract.Frame->ExcludedAddressRanges = {{0, 0x1000}, {0x1010, UINT64_MAX}};
  expectStatus(P, Status::Proved);
  P.Contract.Frame->ExcludedAddressRanges[1].Begin = 0x100f;
  expectStatus(P, Status::InfeasibleEntry); // The remaining gap is too small.
}

TEST(LowIRUndefinedIndependence, ExclusionsAllowTheLastUnsignedAddress) {
  Program P;
  P.Contract.Frame = LowIRIndependenceFrame{{32, 8}, 0, 1};
  P.Contract.EntryConstants = {{reg(32), UINT64_MAX}};
  P.Contract.Frame->ExcludedAddressRanges = {{0, UINT64_MAX}};
  P.finish();
  // The last accessible byte is representable; the mathematical exclusive
  // frame end is 2^64 and must never be formed as a wrapped address.
  expectStatus(P, Status::Proved);
  P.Contract.Frame->Begin = -1;
  expectStatus(P, Status::InfeasibleEntry);
  P.Contract.Frame->ExcludedAddressRanges[0].End = UINT64_MAX - 1;
  expectStatus(P, Status::Proved);

  P.Contract.Frame->End = 0;
  P.Contract.EntryConstants[0].Value = 0;
  P.Contract.Frame->ExcludedAddressRanges = {{1, UINT64_MAX}};
  expectStatus(P, Status::InfeasibleEntry); // Negative offset would wrap.
  P.Contract.EntryConstants[0].Value = 1;
  expectStatus(P, Status::Proved); // Accessible range is exactly [0, 1).
  P.Contract.Frame->ExcludedAddressRanges = {{0, 1}};
  expectStatus(P, Status::InfeasibleEntry);
}

TEST(LowIRUndefinedIndependence,
     ExclusionsUseUnsignedAddressesAndSignedOffsets) {
  Program P;
  P.Contract.Frame = LowIRIndependenceFrame{{32, 8}, INT64_MIN, INT64_MIN + 8};
  P.Contract.EntryConstants = {{reg(32), uint64_t{1} << 63}};
  P.Contract.Frame->ExcludedAddressRanges = {{8, 16}};
  P.finish();
  expectStatus(P, Status::Proved); // Translating INT64_MIN gives [0, 8).
  P.Contract.Frame->ExcludedAddressRanges[0].Begin = 7;
  expectStatus(P, Status::InfeasibleEntry);

  P.Contract.Frame->Begin = 0;
  P.Contract.Frame->End = 8;
  const uint64_t High = uint64_t{1} << 63;
  P.Contract.Frame->ExcludedAddressRanges = {{0, High}, {High + 8, UINT64_MAX}};
  expectStatus(P, Status::Proved);
  P.Contract.Frame->ExcludedAddressRanges[0].End = High + 1;
  expectStatus(P, Status::InfeasibleEntry);
}

TEST(LowIRUndefinedIndependence, ExclusionInputsAndWorkAreBounded) {
  Program P;
  P.frame();
  P.finish();
  P.Contract.Frame->ExcludedAddressRanges = {{0x1000, 0x1000}};
  expectStatus(P, Status::Invalid);
  P.Contract.Frame->ExcludedAddressRanges = {{UINT64_MAX, 0}};
  expectStatus(P, Status::Invalid);
  P.Contract.Frame->ExcludedAddressRanges = {{0x1000, 0x2000},
                                             {0x3000, 0x4000}};
  LowIRIndependenceLimits L;
  L.MaxInstructions = 1;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxSymbolicNodes = 1;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxSolverQueries = 0;
  expectStatus(P, Status::BudgetExceeded, L);
}

TEST(LowIRUndefinedIndependence, ExclusionsPermitRedundancyAndBindExactInput) {
  Program P;
  P.frame();
  P.finish();
  auto A = P.check();
  ASSERT_TRUE(A.proved()) << A.Diagnostic;
  P.Contract.Frame->ExcludedAddressRanges = {{0x1000, 0x2000}};
  auto B = P.check();
  ASSERT_TRUE(B.proved()) << B.Diagnostic;
  EXPECT_NE(A.Certificate->InputDigest, B.Certificate->InputDigest);
  P.Contract.Frame->ExcludedAddressRanges[0].Begin = 0x1001;
  auto C = P.check();
  ASSERT_TRUE(C.proved()) << C.Diagnostic;
  EXPECT_NE(B.Certificate->InputDigest, C.Certificate->InputDigest);
  P.Contract.Frame->ExcludedAddressRanges[0].End = 0x2001;
  auto D = P.check();
  ASSERT_TRUE(D.proved()) << D.Diagnostic;
  EXPECT_NE(C.Certificate->InputDigest, D.Certificate->InputDigest);
  // User and image ranges may repeat or overlap; neither is malformed.
  P.Contract.Frame->ExcludedAddressRanges.push_back({0x1001, 0x2001});
  P.Contract.Frame->ExcludedAddressRanges.push_back({0x1500, 0x2500});
  auto E = P.check();
  ASSERT_TRUE(E.proved()) << E.Diagnostic;
  EXPECT_NE(D.Certificate->InputDigest, E.Certificate->InputDigest);
  EXPECT_EQ(E.Certificate->Contract.Frame->ExcludedAddressRanges.size(), 3u);
  std::swap(P.Contract.Frame->ExcludedAddressRanges[0],
            P.Contract.Frame->ExcludedAddressRanges[2]);
  auto F = P.check();
  ASSERT_TRUE(F.proved()) << F.Diagnostic;
  EXPECT_NE(E.Certificate->InputDigest, F.Certificate->InputDigest);
}
TEST(LowIRUndefinedIndependence, OrderedAtomicsStillRefusesIndependentProof) {
  for (auto Opcode :
       {NdOp::ATOMIC_CMPXCHG, NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG}) {
    Program P;
    P.frame();
    P.Contract.EntryConstants.push_back({reg(32), 0x1000});
    P.instruction({op(NdOp::COPY, reg(8), {number(7)}),
                   op(NdOp::COPY, reg(16), {number(11)}),
                   op(NdOp::STORE, {}, {reg(32), reg(8)})});
    P.instruction({op(NdOp::LOAD, reg(0), {reg(32)})});
    P.finish();
    const auto Control = P.check();
    ASSERT_TRUE(Control.proved()) << Control.Diagnostic;
    ASSERT_TRUE(Control.Certificate);

    auto &Access = P.Function.Blocks.front().Ops[3];
    Access.Opcode = Opcode;
    Access.addInput(reg(8));
    if (Opcode == NdOp::ATOMIC_CMPXCHG)
      Access.addInput(reg(16));
    Access.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    // Keep complete instruction coverage and bind the changed atomic operands.
    // Reusing the LOAD digest would test stale evidence instead of this
    // boundary.
    P.rebind();
    for (const auto &Record : P.Records) {
      ASSERT_EQ(Record.Effects.Coverage, LowUndefinedCoverage::Complete);
      ASSERT_EQ(Record.Effects.OpCount, Record.Boundary.OpCount);
      ASSERT_EQ(
          Record.Effects.OperationDigest,
          lowUndefinedOperationDigest(
              llvm::ArrayRef<LowOp>(P.Function.Blocks.front().Ops)
                  .slice(Record.Boundary.FirstOp, Record.Boundary.OpCount)));
    }
    const auto Result = P.check();
    EXPECT_EQ(Result.Status, Status::Unsupported) << Result.Diagnostic;
    EXPECT_FALSE(Result.Certificate);
    EXPECT_NE(Result.Diagnostic.find("ordered or nondefault memory"),
              std::string::npos);
    EXPECT_EQ(Result.BlockId, 0);
    EXPECT_EQ(Result.InstructionAddress, Access.Addr);
    EXPECT_EQ(Result.OpSeq, Access.Seq);
  }
}

TEST(LowIRUndefinedIndependence, NativeFlagsProfileCannotAuthorizeStaticProof) {
  Program P;
  P.finish();
  ASSERT_TRUE(P.check().proved());
  P.Contract.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
  expectStatus(P, Status::Unsupported);
}

} // namespace
