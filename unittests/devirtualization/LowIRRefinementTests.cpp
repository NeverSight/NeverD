//===- LowIRRefinementTests.cpp - Constructive program relations
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/LowIRRefinement.h"

#include <iterator>
#include <ostream>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRRefinementStatus;
using Witness = LowIRRefinementWitness;

NdVar r(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar n(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp op(NdOp Code, NdVar Output = {},
         std::initializer_list<NdVar> Inputs = {}) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (const auto &Input : Inputs)
    O.addInput(Input);
  return O;
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

  LowIRUndefinedInstruction &instruction(llvm::ArrayRef<LowOp> Ops) {
    auto &B = Function.Blocks.back();
    LowInstructionBoundary IB;
    IB.Address = B.EndAddr++;
    IB.Size = 1;
    IB.FirstOp = B.Ops.size();
    IB.OpCount = Ops.size();
    for (auto O : Ops) {
      O.Addr = IB.Address;
      O.Seq = B.Ops.size() - IB.FirstOp;
      if (O.Opcode == NdOp::BRANCH || O.Opcode == NdOp::COND_BR) {
        IB.Control = LowInstructionControl::Branch;
        IB.ControlFlags = LowInstructionControlFlag::Branch;
        IB.Immediate = O.Inputs[0].Offset;
        if (O.Opcode == NdOp::COND_BR)
          IB.ControlFlags |= LowInstructionControlFlag::Conditional;
      } else if (O.Opcode == NdOp::RETURN) {
        IB.Control = LowInstructionControl::Return;
        IB.ControlFlags = LowInstructionControlFlag::Return;
      }
      B.Ops.push_back(O);
    }
    B.InstructionBoundaries.push_back(IB);
    LowInstructionUndefinedEffects Effects;
    Effects.Coverage = LowUndefinedCoverage::Complete;
    Effects.OpCount = Ops.size();
    Effects.OperationDigest = lowUndefinedOperationDigest(
        llvm::ArrayRef<LowOp>(B.Ops).slice(IB.FirstOp, IB.OpCount));
    Records.push_back({B.Id, IB, std::move(Effects)});
    return Records.back();
  }

  void finish() { instruction({op(NdOp::RETURN, {}, {r(0)})}); }
  void frame() { Contract.Frame = LowIRIndependenceFrame{{32, 8}, -16, 8}; }
  LowIRRefinementResult check(const Program &Candidate,
                              Witness Choice = Witness::LiftedBits,
                              const LowIRRefinementLimits &Limits = {}) const {
    return checkLowIRRefinement(Function, Records, Candidate.Function, Contract,
                                Choice, Limits);
  }
};

void expect(const Program &Original, const Program &Candidate, Status S,
            Witness W = Witness::LiftedBits,
            const LowIRRefinementLimits &Limits = {}) {
  const auto Result = Original.check(Candidate, W, Limits);
  EXPECT_EQ(Result.Status, S) << Result.Diagnostic;
  EXPECT_EQ(Result.proved(), S == Status::Proved);
  EXPECT_EQ(Result.Certificate.has_value(), S == Status::Proved);
}

TEST(LowIRRefinement, EntryAlignmentConstrainsOnlyTheDeclaredRootBits) {
  for (uint64_t Root : {32, 80})
    for (uint32_t Alignment : {1, 2, 4, 16})
      for (uint32_t Residue = 0; Residue != Alignment; ++Residue) {
        SCOPED_TRACE(Root);
        SCOPED_TRACE(Alignment);
        SCOPED_TRACE(Residue);
        Program A, B;
        A.Contract.Frame = LowIRIndependenceFrame{{Root, 8}, -16, 8};
        A.Contract.Frame->EntryAlignment = {Alignment, Residue};
        A.instruction({op(NdOp::INT_AND, r(0), {r(Root), n(Alignment - 1)})});
        A.finish();
        B.instruction({op(NdOp::COPY, r(0), {n(Residue)})});
        B.finish();
        expect(A, B, Status::Proved);
        // No bit above the declared mask becomes a constant.
        B.Function.Blocks[0].Ops[0].Inputs[0] = r(Root);
        expect(A, B, Status::Different);
        B.Function.Blocks[0].Ops[0].Inputs[0] = n(Residue);
        A.Contract.Frame->EntryAlignment.reset();
        expect(A, B, Alignment == 1 ? Status::Proved : Status::Different);
      }
}

TEST(LowIRRefinement, EntryAlignmentIntersectsConstantsBoundsAndExclusions) {
  Program A, B;
  A.frame();
  A.finish();
  B.finish();
  for (auto Invalid : {InterpreterEntryAlignment{0, 0}, {3, 1}, {4, 4}}) {
    A.Contract.Frame->EntryAlignment = Invalid;
    expect(A, B, Status::Invalid);
  }
  A.Contract.Frame->EntryAlignment = {16, 3};
  A.Contract.EntryConstants = {{r(32), 0x2003}};
  expect(A, B, Status::Proved);
  A.Contract.EntryConstants[0].Value = 0x2004;
  expect(A, B, Status::InfeasibleEntry);
  A.Contract.EntryConstants[0].Value = 3;
  expect(A, B, Status::InfeasibleEntry); // Negative frame bytes wrap.
  A.Contract.EntryConstants.clear();
  // Only root 0x2010 fits the exact 24-byte gap, and it is not residue 3.
  A.Contract.Frame->ExcludedAddressRanges = {{0, 0x2000}, {0x2018, UINT64_MAX}};
  expect(A, B, Status::InfeasibleEntry);
  A.Contract.Frame->EntryAlignment->Residue = 0;
  expect(A, B, Status::Proved);
}

TEST(LowIRRefinement, EntryAlignmentBindsReceiptsBudgetsAndPreservation) {
  Program A, B;
  A.frame();
  A.finish();
  B.finish();
  auto Previous = A.check(B);
  ASSERT_TRUE(Previous.proved());
  for (auto Domain :
       {InterpreterEntryAlignment{1, 0}, {16, 3}, {16, 4}, {32, 4}}) {
    A.Contract.Frame->EntryAlignment = Domain;
    auto Good = A.check(B);
    ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
    EXPECT_NE(Previous.Certificate->InputDigest, Good.Certificate->InputDigest);
    EXPECT_EQ(Good.Certificate->Contract.Frame->EntryAlignment, Domain);
    Previous = std::move(Good);
  }
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxSolverQueries = Previous.SolverQueries;
  expect(A, B, Status::Proved, Witness::LiftedBits, Limits);
  ASSERT_GT(Limits.Execution.MaxSolverQueries, 0U);
  --Limits.Execution.MaxSolverQueries;
  expect(A, B, Status::BudgetExceeded, Witness::LiftedBits, Limits);
  Limits = {};
  Limits.Execution.MaxSymbolicNodes = 0;
  expect(A, B, Status::BudgetExceeded, Witness::LiftedBits, Limits);
  Program Changed;
  Changed.frame();
  Changed.Contract.Frame->EntryAlignment = {16, 3};
  Changed.Contract.PreservedRegisters = {{32, 8}};
  Changed.instruction({op(NdOp::INT_ADD, r(32), {r(32), n(16)})});
  Changed.finish();
  expect(Changed, Changed, Status::ContractViolation);
}

TEST(LowIRRefinement, DefinedRewriteAndWrongCandidate) {
  Program A, B;
  A.instruction({op(NdOp::INT_ADD, r(0), {r(8), r(16)})});
  A.finish();
  B.instruction({op(NdOp::INT_ADD, r(0), {r(16), r(8)})});
  B.finish();
  const auto Good = A.check(B);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.Certificate->Scope,
            LowIRRefinementScope::CompleteFiniteLowIRPaths);
  EXPECT_EQ(Good.OriginalPaths, 1U);
  EXPECT_EQ(Good.CandidatePaths, 1U);
  B.Function.Blocks[0].Ops[0].Opcode = NdOp::INT_SUB;
  expect(A, B, Status::Different);
}

TEST(LowIRRefinement, UnauditedNativeBoundaryOptionRejectsStaticAPIs) {
  Program A, B;
  A.instruction({op(NdOp::COPY, r(0), {n(7)})});
  A.finish();
  B.instruction({op(NdOp::COPY, r(0), {n(7)})});
  B.finish();
  expect(A, B, Status::Proved);
  A.Contract.RetainUnauditedNativeBoundaries = true;
  expect(A, B, Status::Unsupported);
  const auto Loop = checkLowIRLoopRefinement(A.Function, A.Records, B.Function,
                                             A.Contract, {});
  EXPECT_EQ(Loop.Status, Status::Unsupported) << Loop.Diagnostic;
  EXPECT_FALSE(Loop.Certificate);
  const auto Inference = inferLowIRLoopRefinementPlan(B.Function, A.Contract);
  EXPECT_EQ(Inference.Status, LowIRLoopInferenceStatus::Unsupported)
      << Inference.Diagnostic;
  EXPECT_FALSE(Inference.Plan);
}

TEST(LowIRRefinement, NativeOverlapOptionRejectsStaticAndLoopAPIs) {
  Program A, B;
  A.instruction({op(NdOp::COPY, r(0), {n(7)})});
  A.finish();
  B.instruction({op(NdOp::COPY, r(0), {n(7)})});
  B.finish();
  expect(A, B, Status::Proved);
  A.Contract.AllowOverlappingNativeInstructions = true;
  expect(A, B, Status::Unsupported);
  const auto Loop = checkLowIRLoopRefinement(A.Function, A.Records, B.Function,
                                             A.Contract, {});
  EXPECT_EQ(Loop.Status, Status::Unsupported) << Loop.Diagnostic;
  EXPECT_FALSE(Loop.Certificate);
  const auto Inference = inferLowIRLoopRefinementPlan(B.Function, A.Contract);
  EXPECT_EQ(Inference.Status, LowIRLoopInferenceStatus::Unsupported)
      << Inference.Diagnostic;
  EXPECT_FALSE(Inference.Plan);
}

TEST(LowIRRefinement, DeferredNativeEdgesRejectStaticAndLoopAPIs) {
  Program A, B;
  A.instruction({op(NdOp::COPY, r(0), {n(7)})});
  A.finish();
  B.instruction({op(NdOp::COPY, r(0), {n(7)})});
  B.finish();
  expect(A, B, Status::Proved);
  A.Contract.DeferNativeConditionalEdges = true;
  expect(A, B, Status::Unsupported);
  const auto Loop = checkLowIRLoopRefinement(A.Function, A.Records, B.Function,
                                             A.Contract, {});
  EXPECT_EQ(Loop.Status, Status::Unsupported) << Loop.Diagnostic;
  EXPECT_FALSE(Loop.Certificate);
  const auto Inference = inferLowIRLoopRefinementPlan(B.Function, A.Contract);
  EXPECT_EQ(Inference.Status, LowIRLoopInferenceStatus::Unsupported)
      << Inference.Diagnostic;
  EXPECT_FALSE(Inference.Plan);
}

TEST(LowIRRefinement, ExplicitWitnessDoesNotBecomeIndependence) {
  Program A, B, Zero;
  auto &Effect = A.instruction({op(NdOp::COPY, r(0), {r(8)})});
  Effect.Effects.Effects.push_back({1, r(0), 0, 1, {}});
  A.finish();
  B.instruction({op(NdOp::COPY, r(0), {r(8)})});
  B.finish();
  Zero.instruction({op(NdOp::INT_AND, r(0), {r(8), n(~uint64_t{1})})});
  Zero.finish();
  expect(A, B, Status::Proved);
  expect(A, B, Status::Different, Witness::ZeroBits);
  expect(A, Zero, Status::Proved, Witness::ZeroBits);
  expect(A, Zero, Status::Different);
  const auto Independent =
      checkLowIRUndefinedIndependence(A.Function, A.Records, A.Contract);
  EXPECT_EQ(Independent.Status, LowIRIndependenceStatus::Dependent);
  EXPECT_FALSE(Independent.Certificate);
}

TEST(LowIRRefinement, GuardActivationAndMalformedBoolean) {
  Program A, B;
  auto &Effect =
      A.instruction({op(NdOp::INT_AND, NdVar::tmp(0, 1), {r(16, 1), n(1, 1)}),
                     op(NdOp::COPY, r(0), {r(8)})});
  Effect.Effects.Effects.push_back({2, r(0), 0, 1, NdVar::tmp(0, 1)});
  A.finish();
  B.instruction(
      {op(NdOp::INT_AND, NdVar::tmp(0, 8), {r(16), n(1)}),
       op(NdOp::INT_XOR, NdVar::tmp(8, 8), {NdVar::tmp(0, 8), n(UINT64_MAX)}),
       op(NdOp::INT_AND, r(0), {r(8), NdVar::tmp(8, 8)})});
  B.finish();
  expect(A, B, Status::Proved, Witness::ZeroBits);
  A.Function.Blocks[0].Ops[0] = op(NdOp::COPY, NdVar::tmp(0, 1), {n(2, 1)});
  A.Function.Blocks[0].Ops[0].Addr = A.Records[0].Boundary.Address;
  A.Function.Blocks[0].Ops[0].Seq = 0;
  A.Records[0].Effects.OperationDigest = lowUndefinedOperationDigest(
      llvm::ArrayRef<LowOp>(A.Function.Blocks[0].Ops).take_front(2));
  expect(A, B, Status::Invalid);
}

TEST(LowIRRefinement, CopiesAndSpillsCannotChooseDifferentValues) {
  Program A, B;
  A.frame();
  A.Contract.ReturnRegisters.push_back({16, 8});
  auto &E = A.instruction({op(NdOp::COPY, r(0), {r(8)})});
  E.Effects.Effects.push_back({1, r(0), 0, 64, {}});
  A.instruction(
      {op(NdOp::COPY, r(16), {r(0)}), op(NdOp::STORE, {}, {r(32), r(0)})});
  A.finish();
  B.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(16), {r(0)}),
                 op(NdOp::STORE, {}, {r(32), r(0)})});
  B.finish();
  expect(A, B, Status::Proved, Witness::ZeroBits);
  B.Function.Blocks[0].Ops[1].Inputs[0] = n(1);
  expect(A, B, Status::Different, Witness::ZeroBits);
  B.Function.Blocks[0].Ops[1].Inputs[0] = n(0);
  B.Function.Blocks[0].Ops[2].Inputs[1] = n(1);
  expect(A, B, Status::Different, Witness::ZeroBits);
}

TEST(LowIRRefinement, ObservesUnionOfWritesAndReturnOperand) {
  Program A, B;
  A.frame();
  A.Contract.ReturnRegisters.clear();
  A.instruction({op(NdOp::COPY, r(0), {n(7)})});
  A.finish();
  B.instruction(
      {op(NdOp::COPY, r(0), {n(7)}), op(NdOp::STORE, {}, {r(32), n(9, 1)})});
  B.finish();
  expect(A, B, Status::Different);
  A.Contract.ObserveWrittenFrameBytes = false;
  expect(A, B, Status::Proved);
  B.Function.Blocks[0].Ops[0].Inputs[0] = n(8);
  expect(A, B, Status::Different);
}

TEST(LowIRRefinement, OrdinaryInputsShareOverlappingByteViews) {
  Program A, B;
  A.instruction({op(NdOp::INT_AND, r(0), {r(8), n(255)})});
  A.finish();
  B.instruction({op(NdOp::INT_ZEXT, r(0), {r(8, 1)})});
  B.finish();
  expect(A, B, Status::Proved);
  B.Function.Blocks[0].Ops[0].Inputs[0] = r(9, 1);
  expect(A, B, Status::Different);
}

Program countdown(bool Decrement = true) {
  Program A;
  A.Function.Blocks[0].Succs = {1};
  A.instruction({op(NdOp::INT_AND, r(8), {r(16), n(3)}),
                 op(NdOp::COPY, r(0), {n(0)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  A.block(1, 0x200, {2, 3});
  A.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  A.block(2, 0x300, {1});
  auto &E = A.instruction({op(NdOp::INT_ADD, r(0), {r(0), n(1)}),
                           op(NdOp::COPY, r(24, 1), {n(1, 1)})});
  E.Effects.Effects.push_back({2, r(24, 1), 0, 1, {}});
  A.instruction({op(NdOp::INT_SUB, r(8), {r(8), n(Decrement ? 1 : 0)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  A.block(3, 0x400);
  A.finish();
  return A;
}

TEST(LowIRRefinement, DifferentLoopStructureAndDistinctProducerVisits) {
  auto A = countdown();
  Program B;
  B.instruction({op(NdOp::INT_AND, r(0), {r(16), n(3)})});
  B.finish();
  const auto Good = A.check(B);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.OriginalPaths, 4U);
  EXPECT_EQ(Good.CandidatePaths, 1U);
  ASSERT_GT(Good.Certificate->Producers.size(), 1U);
  const auto &P = Good.Certificate->Producers;
  for (size_t I = 1; I != P.size(); ++I) {
    EXPECT_NE(P[I - 1].InstructionVisit, P[I].InstructionVisit);
    EXPECT_EQ(P[I - 1].InstructionAddress, P[I].InstructionAddress);
  }
  expect(B, A, Status::Proved);
  B.Function.Blocks[0].Ops[0].Inputs[1] = n(2);
  expect(A, B, Status::Different);
}

TEST(LowIRRefinement, FinishingSiblingCannotHideAnInfiniteCandidatePath) {
  auto A = countdown();
  auto B = countdown(false);
  // Schedule the zero-iteration return before exploring the infinite sibling.
  B.Function.Blocks[1].Ops[0].Opcode = NdOp::INT_NOTEQUAL;
  B.Function.Blocks[1].Ops[1].Inputs[0] = n(0x300);
  B.Function.Blocks[1].InstructionBoundaries[0].Immediate = 0x300;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxPaths = 80;
  expect(A, B, Status::BudgetExceeded, Witness::LiftedBits, Limits);
  const auto Partial = A.check(B, Witness::LiftedBits, Limits);
  EXPECT_EQ(Partial.OriginalPaths, 4U);
  EXPECT_EQ(Partial.CandidatePaths, 1U);
  const auto InfiniteOriginal = countdown(false);
  expect(InfiniteOriginal, A, Status::BudgetExceeded, Witness::LiftedBits,
         Limits);
}

TEST(LowIRRefinement, PreservationStillAppliesToBothPrograms) {
  Program A, B;
  A.Contract.PreservedRegisters = {{8, 8}};
  A.instruction({op(NdOp::COPY, r(0), {n(1)})});
  A.finish();
  B.instruction({op(NdOp::COPY, r(8), {n(0)}), op(NdOp::COPY, r(0), {n(1)})});
  B.finish();
  expect(A, B, Status::ContractViolation);
  expect(B, A, Status::Proved);
}

TEST(LowIRRefinement, MissingStaleAndMalformedEvidenceRefuses) {
  Program A, B;
  A.finish();
  B.finish();
  A.Records[0].Effects.OperationDigest = "stale";
  expect(A, B, Status::Invalid);
  A.Records.clear();
  expect(A, B, Status::Unsupported);
  auto C = B;
  C.Function.Blocks[0].InstructionBoundaries[0].OpCount = UINT64_MAX;
  expect(B, C, Status::Invalid);
  C = B;
  C.Function.Blocks[0].Ops[0].NumInputs = 255;
  expect(B, C, Status::Invalid);
  expect(B, B, Status::Invalid, static_cast<Witness>(255));
}

TEST(LowIRRefinement, StaleUnusedInputRefusesAcrossDigestStorageBoundaries) {
  for (unsigned Count : {3U, 4U, 199U, 200U, 201U}) {
    SCOPED_TRACE(Count);
    Program Original, Candidate;
    std::vector<LowOp> Ops(Count, op(NdOp::NOP));
    Original.instruction(Ops);
    Original.finish();
    Candidate.finish();
    expect(Original, Candidate, Status::Proved);

    // Unused input storage remains part of the instruction evidence, even
    // though this change cannot affect the execution of these NOPs.
    auto &Stored = Original.Function.Blocks[0].Ops;
    Stored[Count - 1].Inputs[5].AddressOwnerVA = UINT64_C(0x8123456789abcdef);
    const auto Stale = Original.check(Candidate);
    EXPECT_EQ(Stale.Status, Status::Invalid) << Stale.Diagnostic;
    EXPECT_FALSE(Stale.Certificate.has_value());
    EXPECT_EQ(Stale.Diagnostic,
              "undefined-effect operation digest is stale or missing");

    Original.Records[0].Effects.OperationDigest = lowUndefinedOperationDigest(
        llvm::ArrayRef<LowOp>(Stored).take_front(Count));
    expect(Original, Candidate, Status::Proved);
  }
}

TEST(LowIRRefinement, LiftedWitnessCannotReadAnUnboundTemporary) {
  Program A, B;
  auto &E =
      A.instruction({op(NdOp::NOP), op(NdOp::COPY, r(0), {NdVar::tmp(0, 8)})});
  E.Effects.Effects.push_back({1, NdVar::tmp(0, 8), 0, 64, {}});
  A.finish();
  B.instruction({op(NdOp::COPY, r(0), {n(0)})});
  B.finish();
  expect(A, B, Status::Invalid);
  expect(A, B, Status::Proved, Witness::ZeroBits);
  expect(B, A, Status::Invalid);
}

TEST(LowIRRefinement, FunctionTemporaryNeedsEveryByteAndBindsItsLifetime) {
  Program A, B;
  A.instruction({op(NdOp::COPY, r(0), {n(0x2211)})});
  A.finish();
  B.instruction({op(NdOp::COPY, NdVar::tmp(64, 2), {n(0x2211, 2)}),
                 op(NdOp::COPY, NdVar::tmp(66, 6), {n(0, 6)})});
  B.instruction({op(NdOp::COPY, r(0), {NdVar::tmp(64, 8)})});
  B.finish();
  expect(A, B,
         Status::Invalid); // Ordinary temporaries do not cross instructions.
  B.Function.FunctionTemporaries = {{64, 8}};
  const auto Good = A.check(B);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  expect(B, A, Status::Proved);
  B.Function.FunctionTemporaries.push_back({80, 8});
  const auto OtherScope = A.check(B);
  ASSERT_TRUE(OtherScope.proved()) << OtherScope.Diagnostic;
  EXPECT_NE(Good.Certificate->CandidateDigest,
            OtherScope.Certificate->CandidateDigest);
  B.Function.FunctionTemporaries = {{64, 2}};
  expect(A, B, Status::Invalid); // The other six bytes expire at the boundary.
  B.Function.FunctionTemporaries = {{64, 8}};
  auto &High = B.Function.Blocks.front().Ops[1];
  High.Inputs[0] = n(1, 6);
  expect(A, B, Status::Different);
  High = op(NdOp::NOP);
  High.Addr = B.Function.Blocks.front().StartAddr;
  High.Seq = 1;
  expect(A, B,
         Status::Invalid); // Declaration does not initialize the high bytes.
}

TEST(LowIRRefinement, FunctionTemporaryDefinitionsArePathLocal) {
  Program A, B;
  A.instruction({op(NdOp::COPY, r(0), {n(7)})});
  A.finish();
  B.Function.FunctionTemporaries = {{64, 8}};
  B.Function.Blocks.front().Succs = {1, 2};
  B.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x200), NdVar::tmp(0, 1)})});
  B.block(1, 0x200, {3});
  B.instruction({op(NdOp::COPY, NdVar::tmp(64, 8), {n(7)}),
                 op(NdOp::BRANCH, {}, {n(0x400)})});
  B.block(2, 0x300, {3});
  B.instruction({op(NdOp::NOP), op(NdOp::BRANCH, {}, {n(0x400)})});
  B.block(3, 0x400);
  B.instruction({op(NdOp::COPY, r(0), {NdVar::tmp(64, 8)})});
  B.finish();
  expect(A, B, Status::Invalid);
  auto &Missing = B.Function.Blocks[2].Ops.front();
  Missing.Opcode = NdOp::COPY;
  Missing.Output = NdVar::tmp(64, 8);
  Missing.addInput(n(7));
  expect(A, B, Status::Proved);
  Missing.Inputs[0] = n(9);
  expect(A, B, Status::Different);
  Missing.Inputs[0] = n(7);
  expect(A, B,
         Status::Proved); // A new invocation must not reuse a prior result.
}

TEST(LowIRRefinement, FunctionTemporaryRangesAreBoundedAndValidated) {
  Program P;
  P.finish();
  for (const auto &Ranges :
       std::vector<std::vector<LowFunctionTemporary>>{{{0, 0}},
                                                      {{UINT64_MAX, 2}},
                                                      {{8, 8}, {0, 8}},
                                                      {{0, 8}, {7, 8}},
                                                      {{0, 8}, {0, 8}},
                                                      {{UINT64_MAX - 7, 8}}}) {
    auto Bad = P;
    Bad.Function.FunctionTemporaries = Ranges;
    expect(P, Bad, Status::Invalid);
  }
  auto Large = P;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxInstructions = 2;
  Large.Function.FunctionTemporaries = {{0, 8}, {8, 8}, {16, 8}};
  expect(P, Large, Status::BudgetExceeded, Witness::LiftedBits, Limits);
  Large.Function.FunctionTemporaries.pop_back();
  expect(P, Large, Status::Proved, Witness::LiftedBits, Limits);
}

TEST(LowIRRefinement, FunctionTemporaryFiniteLoopUsesLatestDefinition) {
  const auto Loop = [](bool Temporary) {
    Program P;
    const auto Value = Temporary ? NdVar::tmp(64, 8) : r(0);
    if (Temporary)
      P.Function.FunctionTemporaries = {{64, 8}};
    P.Contract.ReturnRegisters.push_back({8, 8});
    P.Function.Blocks.front().Succs = {1};
    P.instruction({op(NdOp::INT_AND, r(8), {r(8), n(1)}),
                   op(NdOp::INT_ADD, r(8), {r(8), n(1)}),
                   op(NdOp::COPY, Value, {n(0)}),
                   op(NdOp::BRANCH, {}, {n(0x200)})});
    P.block(1, 0x200, {1, 2});
    P.instruction({op(NdOp::INT_ADD, Value, {Value, n(3)}),
                   op(NdOp::INT_SUB, r(8), {r(8), n(1)}),
                   op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                   op(NdOp::COND_BR, {}, {n(0x300), NdVar::tmp(0, 1)})});
    P.block(2, 0x300);
    if (Temporary)
      P.instruction({op(NdOp::COPY, r(0), {Value})});
    P.finish();
    return P;
  };
  const auto A = Loop(false);
  auto B = Loop(true);
  expect(A, B, Status::Proved);
  B.Function.Blocks[1].Ops.front().Inputs[1] = n(4);
  expect(A, B, Status::Different);
  B.Function.Blocks[1].Ops.front().Inputs[1] = n(3);
  auto &Seed = B.Function.Blocks.front().Ops[2];
  Seed.Opcode = NdOp::NOP;
  Seed.Output = {};
  Seed.NumInputs = 0;
  expect(A, B,
         Status::Invalid); // A backedge write cannot define the first use.
}

TEST(LowIRRefinement, BudgetsCoverBothExecutionsAndFinalRelation) {
  Program A;
  A.instruction({op(NdOp::COPY, r(0), {n(2)})});
  A.finish();
  const auto Good = A.check(A);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 5; ++Kind) {
    LowIRRefinementLimits L;
    if (Kind == 0)
      L.Execution.MaxOperations = Good.Operations - 1;
    if (Kind == 1)
      L.Execution.MaxInstructions = Good.Instructions - 1;
    if (Kind == 2)
      L.Execution.MaxObservations = Good.Observations - 1;
    if (Kind == 3)
      L.Execution.MaxSolverQueries = Good.SolverQueries - 1;
    if (Kind == 4)
      L.MaxTerminalPairs = 0;
    expect(A, A, Status::BudgetExceeded, Witness::LiftedBits, L);
  }
  LowIRRefinementLimits L;
  ++L.MaxTerminalPairs;
  const auto Changed = A.check(A, Witness::LiftedBits, L);
  ASSERT_TRUE(Changed.proved());
  EXPECT_NE(Good.Certificate->InputDigest, Changed.Certificate->InputDigest);
  const auto Zero = A.check(A, Witness::ZeroBits);
  ASSERT_TRUE(Zero.proved());
  EXPECT_NE(Good.Certificate->InputDigest, Zero.Certificate->InputDigest);
}

TEST(LowIRRefinement, MemoryScratchCannotOverwriteInputValues) {
  for (uint64_t Offset : {UINT64_MAX - 7, UINT64_MAX - 10}) {
    Program A, B;
    A.frame();
    const auto Scratch = NdVar::tmp(Offset, 8);
    auto &E = A.instruction({op(NdOp::COPY, Scratch, {n(1)}),
                             op(NdOp::STORE, {}, {r(32), Scratch}),
                             op(NdOp::COPY, r(0), {n(0)})});
    E.Effects.Effects.push_back({1, Scratch, 0, 64, {}});
    A.finish();
    B = A;
    B.Function.Blocks[0].Ops[0].Inputs[0] = n(2);
    expect(A, B, Status::Invalid);
    const auto Strict =
        checkLowIRUndefinedIndependence(A.Function, A.Records, A.Contract);
    EXPECT_EQ(Strict.Status, LowIRIndependenceStatus::Invalid);
    EXPECT_FALSE(Strict.Certificate);
  }
}

TEST(LowIRRefinement, CandidateReturnArityAndEntryDomainAreChecked) {
  Program A, B;
  A.frame();
  A.finish();
  B.instruction({op(NdOp::RETURN)});
  expect(A, B, Status::Different);
  A.Contract.EntryConstants.push_back({r(32), 0});
  expect(A, B, Status::InfeasibleEntry);
}

Program wordLoop(unsigned Step = 1, bool Alternate = false) {
  Program P;
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {r(16)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 3});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {1});
  if (Alternate)
    P.instruction({op(NdOp::INT_NEG2, NdVar::tmp(0, 8), {r(8)}),
                   op(NdOp::INT_SUB, r(0), {r(0), NdVar::tmp(0, 8)})});
  else
    P.instruction({op(NdOp::INT_ADD, r(0), {r(0), r(8)})});
  P.instruction({op(NdOp::INT_SUB, r(8), {r(8), n(Step)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(3, 0x400);
  P.finish();
  return P;
}

LowIRLoopLocation regLocation(uint64_t Offset, uint16_t Bytes = 8) {
  return {LowIRLoopSpace::Register, Offset, Bytes};
}

LowIRLoopRefinementPlan wordLoopPlan() {
  LowIRLoopCutpoint C;
  C.OriginalAddress = C.CandidateAddress = 0x200;
  C.Inputs = {{LowIRLoopSide::Original, regLocation(0), NdVar::tmp(0, 8)},
              {LowIRLoopSide::Original, regLocation(8), NdVar::tmp(8, 8)}};
  C.OriginalState = C.CandidateState = {{regLocation(0), NdVar::tmp(0, 8)},
                                        {regLocation(8), NdVar::tmp(8, 8)}};
  C.Rank = {NdVar::tmp(8, 8)};
  return {{std::move(C)}};
}

LowIRRefinementResult loopCheck(const Program &A, const Program &B,
                                const LowIRLoopRefinementPlan &Plan,
                                const LowIRRefinementLimits &Limits = {}) {
  return checkLowIRLoopRefinement(A.Function, A.Records, B.Function, A.Contract,
                                  Plan, Witness::LiftedBits, Limits);
}

void loopRefused(const LowIRRefinementResult &R, Status S) {
  EXPECT_EQ(R.Status, S) << R.Diagnostic;
  EXPECT_FALSE(R.proved());
  EXPECT_FALSE(R.Certificate);
}

LowIRLoopRefinementPlan guardedWordLoopPlan(bool Frame = false) {
  const LowIRLoopLocation Location =
      Frame ? LowIRLoopLocation{LowIRLoopSpace::Frame, uint64_t(-8), 8}
            : regLocation(24);
  LowIRLoopRefinementPlan Plan;
  for (uint64_t Value : {0, 1}) {
    auto Cut = wordLoopPlan().Cutpoints.front();
    Cut.Inputs.push_back({LowIRLoopSide::Entry, Location, NdVar::tmp(16, 8)});
    Cut.Expressions = {
        op(NdOp::INT_AND, NdVar::tmp(24, 8), {NdVar::tmp(16, 8), n(1)}),
        op(NdOp::INT_EQUAL, NdVar::tmp(32, 1), {NdVar::tmp(24, 8), n(Value)})};
    Cut.Predicate = NdVar::tmp(32, 1);
    Cut.OriginalGuards = Cut.CandidateGuards = {{Location, 1, Value}};
    Plan.Cutpoints.push_back(std::move(Cut));
  }
  return Plan;
}

TEST(LowIRLoopRefinement, GuardedCutsSplitRepeatedAddresses) {
  for (bool Frame : {false, true})
    for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      auto A = wordLoop(), B = wordLoop(1, true);
      if (Frame)
        A.frame();
      A.Contract.ByteOrder = Order;
      const auto Plan = guardedWordLoopPlan(Frame);
      const auto R = loopCheck(A, B, Plan);
      ASSERT_TRUE(R.proved()) << R.Diagnostic;
      EXPECT_EQ(R.LoopInitiations, 2U);
      EXPECT_EQ(R.LoopTransitions, 2U);
      EXPECT_EQ(R.RankingChecks, 2U);
      loopRefused(loopCheck(A, wordLoop(2), Plan), Status::Different);
    }
}

Program guardedBypassLoop() {
  Program P;
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {r(16)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 5});
  P.instruction(
      {op(NdOp::INT_AND, NdVar::tmp(0, 8), {r(24), n(1)}),
       op(NdOp::INT_NOTEQUAL, NdVar::tmp(8, 1), {NdVar::tmp(0, 8), n(0)}),
       op(NdOp::COND_BR, {}, {n(0x500), NdVar::tmp(8, 1)})});
  P.block(2, 0x250, {3, 4});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(3, 0x300, {1});
  P.instruction({op(NdOp::INT_ADD, r(0), {r(0), r(8)}),
                 op(NdOp::INT_SUB, r(8), {r(8), n(1)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(4, 0x400);
  P.finish();
  P.block(5, 0x500);
  P.instruction({op(NdOp::COPY, r(0), {n(7)})});
  P.finish();
  return P;
}

TEST(LowIRLoopRefinement, GuardedCutsExecuteAndCheckUnmatchedDomains) {
  const auto P = guardedBypassLoop();
  auto Plan = guardedWordLoopPlan();
  Plan.Cutpoints.resize(1);
  const auto Good = loopCheck(P, P, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.LoopInitiations, 1U);
  EXPECT_EQ(Good.RankingChecks, 1U);
  EXPECT_EQ(Good.OriginalPaths, 2U);
  auto Wrong = P;
  Wrong.Function.Blocks.back().Ops.front().Inputs[0] = n(8);
  loopRefused(loopCheck(P, Wrong, Plan), Status::Different);
  // The missing selector is not a domain restriction. A loop there still
  // requires coverage, even though the selected domain has a valid rank.
  const auto Cyclic = wordLoop();
  LowIRRefinementLimits Short;
  Short.Execution.MaxBlockVisits = 64;
  loopRefused(loopCheck(Cyclic, Cyclic, Plan, Short), Status::BudgetExceeded);
}

TEST(LowIRLoopRefinement, GuardedCutsPreserveSelectorsInUniquePairings) {
  const auto P = guardedBypassLoop();
  auto Self = guardedWordLoopPlan();
  Self.Cutpoints.resize(1);
  const std::vector<LowIRLoopCutpointPair> Pairs{
      {0x200,
       0x200,
       {{regLocation(0), regLocation(0)}, {regLocation(8), regLocation(8)}}}};
  // Each input plan has 13 metadata items; the pairing has one cut and two
  // shared inputs. The selectors remain separate on the resulting sides.
  auto Paired = pairLowIRLoopRefinementPlans(Self, Self, Pairs, 29);
  ASSERT_TRUE(static_cast<bool>(Paired)) << llvm::toString(Paired.takeError());
  EXPECT_EQ(Paired->Cutpoints[0].OriginalGuards.size(), 1U);
  EXPECT_EQ(Paired->Cutpoints[0].CandidateGuards.size(), 1U);
  const auto Good = loopCheck(P, P, *Paired);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  auto Short = pairLowIRLoopRefinementPlans(Self, Self, Pairs, 28);
  EXPECT_FALSE(static_cast<bool>(Short));
  if (!Short)
    llvm::consumeError(Short.takeError());
  Self.Cutpoints[0].CandidateGuards[0].Value = 1;
  auto Wrong = pairLowIRLoopRefinementPlans(Self, Self, Pairs);
  EXPECT_FALSE(static_cast<bool>(Wrong));
  if (!Wrong)
    llvm::consumeError(Wrong.takeError());
  Self = guardedWordLoopPlan();
  auto Ambiguous =
      pairLowIRLoopRefinementPlans(Self, Self, {Pairs.front(), Pairs.front()});
  EXPECT_FALSE(static_cast<bool>(Ambiguous));
  if (!Ambiguous)
    llvm::consumeError(Ambiguous.takeError());
}

TEST(LowIRLoopRefinement, GuardedCutsRejectOverlapAndWrongSide) {
  const auto P = wordLoop();
  auto Plan = guardedWordLoopPlan();
  Plan.Cutpoints[1].OriginalGuards = Plan.Cutpoints[0].OriginalGuards;
  loopRefused(loopCheck(P, P, Plan), Status::Invalid);
  Plan = guardedWordLoopPlan();
  std::swap(Plan.Cutpoints[0].CandidateGuards,
            Plan.Cutpoints[1].CandidateGuards);
  loopRefused(loopCheck(P, P, Plan), Status::Different);
  Plan = guardedWordLoopPlan();
  Plan.Cutpoints[1].OriginalGuards.clear();
  loopRefused(loopCheck(P, P, Plan), Status::Invalid);
}

TEST(LowIRLoopRefinement, GuardedCutsRequireTheirStartingSelector) {
  const auto P = wordLoop();
  for (bool Prefix : {false, true}) {
    auto Plan = guardedWordLoopPlan();
    for (auto &C : Plan.Cutpoints) {
      C.UseEntryPrefix = C.GeneralizeEntryPrefix = Prefix;
      C.Predicate = n(1, 1);
    }
    const auto R = loopCheck(P, P, Plan);
    loopRefused(R, Status::Different);
    EXPECT_NE(R.Diagnostic.find("starting selector"), std::string::npos);
  }
}

TEST(LowIRLoopRefinement, GuardedCutsValidateMasksAndLocations) {
  const auto P = wordLoop();
  for (const auto &Guard : std::vector<LowIRLoopGuard>{
           {regLocation(24, 1), 0, 0},
           {regLocation(24, 1), 0x100, 0},
           {regLocation(24, 1), 1, 2},
           {regLocation(24, 0), 1, 0},
           {regLocation(24, 9), 1, 0},
           {{LowIRLoopSpace::FunctionTemporary, 24, 8}, 1, 0},
           {{LowIRLoopSpace::Frame, uint64_t(-8), 8}, 1, 0},
           {{LowIRLoopSpace::SystemFlags, 0, 8}, 1, 0},
           {{static_cast<LowIRLoopSpace>(99), 0, 8}, 1, 0}}) {
    auto Plan = guardedWordLoopPlan();
    Plan.Cutpoints[0].OriginalGuards = {Guard};
    loopRefused(loopCheck(P, P, Plan), Status::Invalid);
  }
}

TEST(LowIRLoopRefinement, GuardedCutsValidateFrameAndRegisterRanges) {
  auto P = wordLoop();
  P.frame();
  const auto Plan = guardedWordLoopPlan(true);
  const auto Good = loopCheck(P, P, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  for (auto Bad : {LowIRLoopLocation{LowIRLoopSpace::Frame, uint64_t(-24), 8},
                   LowIRLoopLocation{LowIRLoopSpace::Frame, 4, 8},
                   regLocation(UINT64_MAX, 8)}) {
    auto Wrong = Plan;
    Wrong.Cutpoints[0].OriginalGuards[0].Location = Bad;
    loopRefused(loopCheck(P, P, Wrong), Status::Invalid);
  }
}

TEST(LowIRLoopRefinement, GuardedCutsRemainRelativeToTheSelectedWitness) {
  auto P = wordLoop();
  auto &Entry = P.Function.Blocks.front();
  Entry.Ops.front().Inputs[0] = n(1);
  auto &Record = P.Records.front();
  Record.Effects.OperationDigest =
      lowUndefinedOperationDigest(llvm::ArrayRef<LowOp>(Entry.Ops).slice(
          Record.Boundary.FirstOp, Record.Boundary.OpCount));
  Record.Effects.Effects.push_back({1, r(0), 0, 1, {}});
  auto Plan = guardedWordLoopPlan();
  for (unsigned I = 0; I != 2; ++I) {
    auto &C = Plan.Cutpoints[I];
    C.Inputs.pop_back();
    C.Expressions[0].Inputs[0] = NdVar::tmp(0, 8);
    C.OriginalGuards = C.CandidateGuards = {{regLocation(0), 1, I}};
  }
  const auto Good = loopCheck(P, P, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  ASSERT_FALSE(Good.Certificate->Producers.empty());
  const auto Other = checkLowIRLoopRefinement(
      P.Function, P.Records, P.Function, P.Contract, Plan, Witness::ZeroBits);
  loopRefused(Other, Status::Different);
}

TEST(LowIRLoopRefinement, GuardedCutsCannotDropUnknownDomains) {
  const auto P = wordLoop();
  const auto Plan = guardedWordLoopPlan();
  LowIRRefinementLimits Tiny;
  Tiny.Execution.Solver.Blast.MaxGates = 1;
  const auto R = loopCheck(P, P, Plan, Tiny);
  loopRefused(R, Status::BudgetExceeded);
  EXPECT_NE(R.Diagnostic.find("solver budget"), std::string::npos);
  Tiny = {};
  Tiny.Execution.MaxInstructions = 1;
  loopRefused(loopCheck(P, P, Plan, Tiny), Status::BudgetExceeded);
}

TEST(LowIRLoopRefinement, GuardedCutsBindDigestsAndSharedBudgets) {
  const auto P = wordLoop();
  auto Plan = guardedWordLoopPlan();
  const auto Good = loopCheck(P, P, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  for (auto &C : Plan.Cutpoints)
    C.OriginalGuards.push_back(C.OriginalGuards.front());
  const auto Duplicate = loopCheck(P, P, Plan);
  ASSERT_TRUE(Duplicate.proved()) << Duplicate.Diagnostic;
  EXPECT_NE(Good.Certificate->InputDigest, Duplicate.Certificate->InputDigest);
  Plan = guardedWordLoopPlan();
  LowIRRefinementLimits Exact;
  Exact.Execution.MaxOperations = Good.Operations;
  Exact.Execution.MaxSolverQueries = Good.SolverQueries;
  Exact.Execution.MaxObservations = Good.Observations;
  Exact.MaxTerminalPairs = Good.TerminalPairs;
  ASSERT_TRUE(loopCheck(P, P, Plan, Exact).proved());
  for (unsigned Case = 0; Case != 5; ++Case) {
    auto Short = Exact;
    if (Case == 0)
      --Short.Execution.MaxOperations;
    if (Case == 1)
      --Short.Execution.MaxSolverQueries;
    if (Case == 2)
      --Short.Execution.MaxObservations;
    if (Case == 3)
      --Short.MaxTerminalPairs;
    if (Case == 4)
      Short.Execution.MaxPaths = 1;
    loopRefused(loopCheck(P, P, Plan, Short), Status::BudgetExceeded);
  }
}

TEST(LowIRLoopRefinement, ArbitraryWordCountWithDifferentBody) {
  const auto A = wordLoop(), B = wordLoop(1, true);
  const auto Plan = wordLoopPlan();
  const auto R = loopCheck(A, B, Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  EXPECT_EQ(R.Certificate->Scope, LowIRRefinementScope::InductiveLowIRLoops);
  ASSERT_TRUE(R.Certificate->LoopPlan);
  EXPECT_EQ(R.LoopInitiations, 1U);
  EXPECT_EQ(R.LoopTransitions, 1U);
  EXPECT_EQ(R.RankingChecks, 1U);
  EXPECT_EQ(R.OriginalPaths, 1U);
  EXPECT_EQ(R.CandidatePaths, 1U);
  EXPECT_EQ(R.OriginalCutpoints, 2U);
  EXPECT_LT(R.Instructions, 20U);
  const auto Finite = A.check(B);
  EXPECT_FALSE(Finite.proved());
  EXPECT_EQ(Finite.Status, Status::BudgetExceeded);
}

Program savedInputLoop(std::optional<uint64_t> Temporary,
                       uint16_t ChangedBytes = 0, uint16_t InitialBytes = 8) {
  Program P;
  if (Temporary)
    P.Function.FunctionTemporaries = {{*Temporary, 8}};
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {r(16)})});
  if (Temporary)
    P.instruction({op(NdOp::COPY, NdVar::tmp(*Temporary, InitialBytes),
                      {r(16, InitialBytes)})});
  P.instruction({op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 3});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {1});
  P.instruction({op(NdOp::INT_ADD, r(0),
                    {r(0), Temporary ? NdVar::tmp(*Temporary, 8) : r(16)})});
  if (ChangedBytes)
    P.instruction(
        {op(NdOp::INT_ADD, NdVar::tmp(*Temporary, ChangedBytes),
            {NdVar::tmp(*Temporary, ChangedBytes), n(1, ChangedBytes)})});
  P.instruction({op(NdOp::INT_SUB, r(8), {r(8), n(1)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(3, 0x400);
  P.finish();
  return P;
}

TEST(LowIRLoopRefinement, CheckedPrefixRetainsFunctionTemporaryBytes) {
  const auto A = savedInputLoop({});
  auto B = savedInputLoop(64);
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].UseEntryPrefix = true;
  const auto R = loopCheck(A, B, Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  EXPECT_EQ(R.Certificate->Scope, LowIRRefinementScope::InductiveLowIRLoops);
  EXPECT_EQ(R.LoopTransitions, 1U);
  EXPECT_EQ(R.RankingChecks, 1U);
  EXPECT_LT(R.Instructions, 32U);
  EXPECT_TRUE(loopCheck(savedInputLoop(128), B, Plan).proved());
  B.Function.FunctionTemporaries.push_back({80, 8});
  const auto OtherLifetime = loopCheck(A, B, Plan);
  ASSERT_TRUE(OtherLifetime.proved()) << OtherLifetime.Diagnostic;
  EXPECT_NE(R.Certificate->InputDigest, OtherLifetime.Certificate->InputDigest);

  Plan.Cutpoints[0].UseEntryPrefix = false;
  const auto Missing = loopCheck(A, B, Plan);
  loopRefused(Missing, Status::Different);
  EXPECT_NE(Missing.Diagnostic.find("function-temporary definedness"),
            std::string::npos);
}

TEST(LowIRLoopRefinement,
     FunctionTemporaryCutRejectsDriftAndPartialDefinitions) {
  const auto A = savedInputLoop({});
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].UseEntryPrefix = true;
  for (const uint16_t Bytes : {uint16_t{1}, uint16_t{8}}) {
    const auto R = loopCheck(A, savedInputLoop(64, Bytes), Plan);
    loopRefused(R, Status::Different);
    EXPECT_NE(R.Diagnostic.find("loop function-temporary byte"),
              std::string::npos);
  }
  const auto Partial = loopCheck(A, savedInputLoop(64, 0, 4), Plan);
  loopRefused(Partial, Status::Invalid);
  EXPECT_NE(Partial.Diagnostic.find("unbound temporary"), std::string::npos);
}

TEST(LowIRLoopRefinement, FunctionTemporaryPrefixKeepsProofBudgets) {
  const auto A = savedInputLoop({}), B = savedInputLoop(64);
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].UseEntryPrefix = true;
  const auto Good = loopCheck(A, B, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  ASSERT_GT(Good.Observations, 0U);
  ASSERT_GT(Good.SolverQueries, 0U);
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxObservations = Good.Observations;
  Limits.Execution.MaxSolverQueries = Good.SolverQueries;
  ASSERT_TRUE(loopCheck(A, B, Plan, Limits).proved());
  --Limits.Execution.MaxObservations;
  loopRefused(loopCheck(A, B, Plan, Limits), Status::BudgetExceeded);
  Limits.Execution.MaxObservations = Good.Observations;
  --Limits.Execution.MaxSolverQueries;
  loopRefused(loopCheck(A, B, Plan, Limits), Status::BudgetExceeded);
}

TEST(LowIRLoopInference, FixedFunctionTemporaryPrefixIsChecked) {
  const auto A = savedInputLoop({}), B = savedInputLoop(64);
  const auto Inferred = inferLowIRLoopRefinementPlan(B.Function, B.Contract);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  ASSERT_TRUE(loopCheck(A, B, *Inferred.Plan).proved());
  loopRefused(loopCheck(A, savedInputLoop(64, 1), *Inferred.Plan),
              Status::Different);
}

TEST(LowIRLoopInference, ChangingFunctionTemporaryStateIsInferred) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint16_t Bytes : {uint16_t{1}, uint16_t{3}, uint16_t{8}}) {
      auto P = savedInputLoop(67, Bytes);
      P.Contract.ByteOrder = Order;
      const auto Inferred =
          inferLowIRLoopRefinementPlan(P.Function, P.Contract);
      ASSERT_TRUE(Inferred.inferred()) << Bytes << ": " << Inferred.Diagnostic;
      const auto Proof = loopCheck(P, P, *Inferred.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
      EXPECT_GT(Proof.RankingChecks, 0U);
      EXPECT_LT(Proof.Instructions, 32U);
    }
}

LowIRLoopRefinementPlan changingTemporaryPlan(uint64_t Original,
                                              uint64_t Candidate) {
  auto Plan = wordLoopPlan();
  auto &Cut = Plan.Cutpoints[0];
  Cut.UseEntryPrefix = true;
  const LowIRLoopLocation Left{LowIRLoopSpace::FunctionTemporary, Original, 8};
  const LowIRLoopLocation Right{LowIRLoopSpace::FunctionTemporary, Candidate,
                                8};
  Cut.Inputs.push_back({LowIRLoopSide::Original, Left, NdVar::tmp(16, 8)});
  Cut.OriginalState.push_back({Left, NdVar::tmp(16, 8)});
  Cut.CandidateState.push_back({Right, NdVar::tmp(16, 8)});
  return Plan;
}

TEST(LowIRLoopRefinement, FunctionTemporaryParametersKeepSeparateSideStorage) {
  const auto A = savedInputLoop(67, 3), B = savedInputLoop(131, 3);
  const auto Plan = changingTemporaryPlan(67, 131);
  const auto Good = loopCheck(A, B, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_GT(Good.RankingChecks, 0U);
  loopRefused(loopCheck(A, savedInputLoop(131, 8), Plan), Status::Different);

  auto Missing = Plan;
  Missing.Cutpoints[0].CandidateState.pop_back();
  loopRefused(loopCheck(A, B, Missing), Status::Different);
  auto WrongSide = Plan;
  WrongSide.Cutpoints[0].Inputs.back().Side = LowIRLoopSide::Candidate;
  loopRefused(loopCheck(A, B, WrongSide), Status::Invalid);

  const auto Left = changingTemporaryPlan(67, 67);
  const auto Right = changingTemporaryPlan(131, 131);
  LowIRLoopCutpointPair Pair;
  Pair.OriginalAddress = Pair.CandidateAddress = 0x200;
  Pair.SharedInputs = {{regLocation(0), regLocation(0)},
                       {regLocation(8), regLocation(8)},
                       {{LowIRLoopSpace::FunctionTemporary, 67, 8},
                        {LowIRLoopSpace::FunctionTemporary, 131, 8}}};
  auto Paired = pairLowIRLoopRefinementPlans(Left, Right, {Pair});
  ASSERT_TRUE(bool(Paired)) << llvm::toString(Paired.takeError());
  ASSERT_TRUE(loopCheck(A, B, *Paired).proved());
  loopRefused(loopCheck(A, savedInputLoop(131, 1), *Paired), Status::Different);
}

TEST(LowIRLoopRefinement, FunctionTemporaryTemplateCannotInventDefinedBytes) {
  auto P = savedInputLoop(67, 3);
  const auto Plan = changingTemporaryPlan(67, 67);
  for (unsigned Case = 0; Case != 7; ++Case) {
    SCOPED_TRACE(Case);
    auto Bad = Plan;
    auto &Cut = Bad.Cutpoints[0];
    switch (Case) {
    case 0:
      Cut.UseEntryPrefix = false;
      break;
    case 1:
      Cut.Inputs.back().Side = LowIRLoopSide::Entry;
      break;
    case 2:
      Cut.Inputs.back().Location.Offset = 68; // crosses the declared range
      break;
    case 3:
      Cut.Inputs.back().Location.Offset = UINT64_MAX - 3;
      break;
    case 4:
      Cut.OriginalState.back().Location.Offset = 66;
      break;
    case 5:
      Cut.CandidateState.back().Location.Offset = 75;
      break;
    case 6:
      Cut.Inputs.back().Location.Bytes = 0;
      break;
    }
    loopRefused(loopCheck(P, P, Bad), Status::Invalid);
  }
  // Declared but uninitialized bytes cannot be created by an assignment or
  // supplied as a prefix input, even when the body never reads them.
  P.Function.FunctionTemporaries.push_back({83, 8});
  const LowIRLoopLocation Unbound{LowIRLoopSpace::FunctionTemporary, 83, 8};
  auto Invented = Plan;
  Invented.Cutpoints[0].CandidateState.push_back({Unbound, n(0)});
  loopRefused(loopCheck(P, P, Invented), Status::Invalid);
  for (auto Side :
       {LowIRLoopSide::OriginalPrefix, LowIRLoopSide::CandidatePrefix}) {
    auto Uninitialized = Plan;
    Uninitialized.Cutpoints[0].Inputs.push_back(
        {Side, Unbound, NdVar::tmp(24, 8)});
    loopRefused(loopCheck(P, P, Uninitialized), Status::Invalid);
  }
}

TEST(LowIRLoopRefinement, FunctionTemporaryInputsMustProjectFromAssignedState) {
  auto P = savedInputLoop(67, 3);
  P.Contract.EntryConstants = {{r(16), 0}};
  auto Plan = changingTemporaryPlan(67, 67);
  ASSERT_TRUE(loopCheck(P, P, Plan).proved());
  for (auto Side : {LowIRLoopSide::Original, LowIRLoopSide::Candidate}) {
    auto Bad = Plan;
    auto &Cut = Bad.Cutpoints[0];
    Cut.Inputs.back().Side = Side;
    auto &Assignments = Side == LowIRLoopSide::Original ? Cut.OriginalState
                                                        : Cut.CandidateState;
    Assignments.back().Value = n(0);
    const auto Proof = loopCheck(P, P, Bad);
    loopRefused(Proof, Status::Different);
    EXPECT_NE(Proof.Diagnostic.find("parameter projection"), std::string::npos);
  }
}

Program temporaryCounterLoop(uint16_t Bytes) {
  Program P;
  constexpr uint64_t Offset = 67;
  const auto Count = NdVar::tmp(Offset, Bytes);
  P.Function.FunctionTemporaries = {{Offset, 12}};
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}),
                 op(NdOp::COPY, Count, {r(16, Bytes)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 3});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {Count, n(0, Bytes)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {1});
  P.instruction({op(NdOp::INT_ADD, r(0), {r(0), n(1)}),
                 op(NdOp::INT_SUB, Count, {Count, n(1, Bytes)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(3, 0x400);
  P.finish();
  return P;
}

TEST(LowIRLoopInference, PartiallyDefinedFunctionTemporarySuppliesTheRank) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint16_t Bytes : {uint16_t{1}, uint16_t{3}, uint16_t{8}}) {
      auto P = temporaryCounterLoop(Bytes);
      P.Contract.ByteOrder = Order;
      const auto Inferred =
          inferLowIRLoopRefinementPlan(P.Function, P.Contract);
      ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
      bool Bound = false;
      for (const auto &Cut : Inferred.Plan->Cutpoints)
        for (const auto &Input : Cut.Inputs)
          if (Input.Location.Space == LowIRLoopSpace::FunctionTemporary) {
            EXPECT_EQ(Input.Location.Offset, 67U);
            EXPECT_EQ(Input.Location.Bytes, Bytes);
            Bound = true;
          }
      EXPECT_TRUE(Bound);
      const auto Proof = loopCheck(P, P, *Inferred.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
      EXPECT_GT(Proof.RankingChecks, 0U);
      auto OtherLifetime = P;
      OtherLifetime.Function.FunctionTemporaries[0].Bytes = Bytes;
      const auto Other =
          loopCheck(OtherLifetime, OtherLifetime, *Inferred.Plan);
      ASSERT_TRUE(Other.proved()) << Other.Diagnostic;
      EXPECT_NE(Other.Certificate->InputDigest, Proof.Certificate->InputDigest);
    }
}

TEST(LowIRLoopInference, FunctionTemporaryParametersKeepIndependentBudgets) {
  const auto P = temporaryCounterLoop(3);
  const auto Good = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  LowIRLoopInferenceLimits Inference;
  Inference.Execution.MaxOperations = Good.Operations;
  Inference.Execution.MaxSolverQueries = Good.SolverQueries;
  Inference.MaxWideningRounds = Good.WideningRounds;
  ASSERT_TRUE(inferLowIRLoopRefinementPlan(P.Function, P.Contract, Inference)
                  .inferred());
  for (bool Queries : {false, true}) {
    auto Short = Inference;
    if (Queries)
      --Short.Execution.MaxSolverQueries;
    else
      --Short.Execution.MaxOperations;
    const auto Rejected =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, Short);
    EXPECT_EQ(Rejected.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_FALSE(Rejected.Plan);
  }
  const auto Proof = loopCheck(P, P, *Good.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxOperations = Proof.Operations;
  Limits.Execution.MaxSolverQueries = Proof.SolverQueries;
  Limits.Execution.MaxObservations = Proof.Observations;
  ASSERT_TRUE(loopCheck(P, P, *Good.Plan, Limits).proved());
  for (unsigned Case = 0; Case != 3; ++Case) {
    auto Short = Limits;
    if (Case == 0)
      --Short.Execution.MaxOperations;
    else if (Case == 1)
      --Short.Execution.MaxSolverQueries;
    else
      --Short.Execution.MaxObservations;
    loopRefused(loopCheck(P, P, *Good.Plan, Short), Status::BudgetExceeded);
  }
}

TEST(LowIRLoopRefinement, UnusedFunctionTemporariesDoNotInventCutpointState) {
  auto A = wordLoop(), B = wordLoop();
  B.Function.FunctionTemporaries = {{64, 8}};
  EXPECT_TRUE(loopCheck(A, B, wordLoopPlan()).proved());
  const auto Inferred = inferLowIRLoopRefinementPlan(B.Function, B.Contract);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  EXPECT_TRUE(loopCheck(A, B, *Inferred.Plan).proved());
  // The finite checker can still enumerate complete bounded executions.
  A.Contract.EntryConstants = {{r(16), 2}};
  expect(A, B, Status::Proved);
}

TEST(LowIRLoopRefinement, EntryAlignmentSurvivesEntryAndInductionTemplates) {
  auto A = wordLoop(), B = wordLoop(1, true);
  A.frame();
  A.Contract.Frame->EntryAlignment = {16, 3};
  // Replace the final instruction, keeping complete instruction sidecars.
  for (auto *P : {&A, &B}) {
    auto &Exit = P->Function.Blocks.back();
    Exit.Ops.clear();
    Exit.InstructionBoundaries.clear();
    Exit.EndAddr = Exit.StartAddr;
    P->Records.pop_back();
    P->instruction(
        {op(NdOp::INT_AND, NdVar::tmp(0, 8), {P == &A ? r(32) : n(3), n(15)}),
         op(NdOp::INT_ADD, r(0), {r(0), NdVar::tmp(0, 8)}),
         op(NdOp::RETURN, {}, {r(0)})});
  }
  const auto Plan = wordLoopPlan();
  const auto Good = loopCheck(A, B, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.Certificate->Contract.Frame->EntryAlignment,
            A.Contract.Frame->EntryAlignment);
  A.Contract.Frame->EntryAlignment->Residue = 4;
  loopRefused(loopCheck(A, B, Plan), Status::Different);
  A.Contract.Frame->EntryAlignment.reset();
  loopRefused(loopCheck(A, B, Plan), Status::Different);
}

TEST(LowIRLoopInference, CountAndAccumulatorNeedNoHandwrittenTemplate) {
  const auto A = wordLoop(), B = wordLoop(1, true);
  const auto Inferred = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  EXPECT_GT(Inferred.Operations, 0U);
  EXPECT_GT(Inferred.SolverQueries, 0U);
  EXPECT_GT(Inferred.RankCandidates, 0U);
  const auto R = loopCheck(A, B, *Inferred.Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  EXPECT_EQ(R.Certificate->Scope, LowIRRefinementScope::InductiveLowIRLoops);
  loopRefused(loopCheck(A, wordLoop(2), *Inferred.Plan), Status::Different);
}

Program sharedHeaderCounters(uint16_t Bytes, bool SharedLatch,
                             unsigned FirstStep = 1) {
  Program P;
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}),
                 op(NdOp::INT_ZEXT, r(8), {r(8, Bytes)}),
                 op(NdOp::INT_ZEXT, r(16), {r(16, Bytes)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 3});
  P.instruction(
      {op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8, Bytes), n(0, Bytes)}),
       op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {SharedLatch ? 5 : 1});
  P.instruction(
      {op(NdOp::INT_SUB, r(8, Bytes), {r(8, Bytes), n(FirstStep, Bytes)}),
       op(NdOp::INT_ZEXT, r(8), {r(8, Bytes)}),
       op(NdOp::INT_ADD, r(0), {r(0), n(3)}),
       op(NdOp::BRANCH, {}, {n(SharedLatch ? 0x600 : 0x200)})});
  P.block(3, 0x400, {4, 6});
  P.instruction(
      {op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(16, Bytes), n(0, Bytes)}),
       op(NdOp::COND_BR, {}, {n(0x700), NdVar::tmp(0, 1)})});
  P.block(4, 0x500, {SharedLatch ? 5 : 1});
  P.instruction({op(NdOp::INT_SUB, r(16, Bytes), {r(16, Bytes), n(1, Bytes)}),
                 op(NdOp::INT_ZEXT, r(16), {r(16, Bytes)}),
                 op(NdOp::INT_ADD, r(0), {r(0), n(5)}),
                 op(NdOp::BRANCH, {}, {n(SharedLatch ? 0x600 : 0x200)})});
  if (SharedLatch) {
    P.block(5, 0x600, {1});
    P.instruction({op(NdOp::BRANCH, {}, {n(0x200)})});
  }
  P.block(6, 0x700);
  P.finish();
  return P;
}

TEST(LowIRLoopInference, SharedHeaderAndLatchNeedLexicographicRanks) {
  for (uint16_t Bytes : {4, 8})
    for (bool SharedLatch : {false, true}) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(SharedLatch);
      const auto P = sharedHeaderCounters(Bytes, SharedLatch);
      LowIRLoopInferenceLimits Limits;
      Limits.Execution.MaxOperations = 4096;
      const auto R =
          inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits);
      ASSERT_TRUE(R.inferred())
          << R.Diagnostic << " cuts=" << R.CutpointAttempts
          << " ranks=" << R.RankCandidates;
      ASSERT_EQ(R.Plan->Cutpoints.size(), 1U);
      EXPECT_GT(R.Plan->Cutpoints[0].Rank.size(), 1U);
      const auto Proof = loopCheck(P, P, *R.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    }
}

TEST(LowIRLoopInference, SharedCyclesRejectWrongBodiesAndNonprogress) {
  for (bool SharedLatch : {false, true}) {
    SCOPED_TRACE(SharedLatch);
    const auto P = sharedHeaderCounters(8, SharedLatch);
    const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
    ASSERT_TRUE(R.inferred()) << R.Diagnostic;
    auto Wrong = P;
    Wrong.Function.Blocks[2].Ops[2].Inputs[1] = n(4);
    loopRefused(loopCheck(P, Wrong, *R.Plan), Status::Different);
    for (unsigned Step : {0, 2}) {
      SCOPED_TRACE(Step);
      const auto Infinite = sharedHeaderCounters(8, SharedLatch, Step);
      const auto Refused =
          inferLowIRLoopRefinementPlan(Infinite.Function, Infinite.Contract);
      EXPECT_FALSE(Refused.inferred());
      EXPECT_FALSE(Refused.Plan);
      loopRefused(loopCheck(Infinite, Infinite, *R.Plan), Status::Different);
    }
  }
}

TEST(LowIRLoopInference, SharedCycleScalarAndTupleSearchUseOneBudget) {
  const auto P = sharedHeaderCounters(8, true);
  const auto Good = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  ASSERT_GT(Good.CutpointAttempts, 1U);
  ASSERT_GT(Good.RankCandidates, 2U);
  LowIRLoopInferenceLimits Exact;
  Exact.MaxCutpointAttempts = Good.CutpointAttempts;
  Exact.MaxRankCandidates = Good.RankCandidates;
  Exact.Execution.MaxOperations = Good.Operations;
  Exact.Execution.MaxSolverQueries = Good.SolverQueries;
  Exact.Execution.MaxPaths = Good.ScheduledPaths;
  const auto Enough =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Exact);
  ASSERT_TRUE(Enough.inferred()) << Enough.Diagnostic;
  const auto Proof = loopCheck(P, P, *Enough.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;

  for (unsigned Kind = 0; Kind != 5; ++Kind) {
    SCOPED_TRACE(Kind);
    auto Limits = Exact;
    if (Kind == 0)
      --Limits.MaxCutpointAttempts;
    if (Kind == 1)
      --Limits.MaxRankCandidates;
    if (Kind == 2)
      --Limits.Execution.MaxOperations;
    if (Kind == 3)
      --Limits.Execution.MaxSolverQueries;
    if (Kind == 4)
      --Limits.Execution.MaxPaths;
    const auto Refused =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits);
    EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded)
        << Refused.Diagnostic;
    EXPECT_FALSE(Refused.Plan);
    if (Kind == 0) {
      // Reject the incomplete body cut structurally, before unrolling it.
      EXPECT_EQ(Refused.Operations, 0U);
      EXPECT_EQ(Refused.RankCandidates, 0U);
    }
  }
  LowIRRefinementLimits ProofLimits;
  ProofLimits.Execution.MaxSolverQueries = Proof.SolverQueries - 1;
  loopRefused(loopCheck(P, P, *Enough.Plan, ProofLimits),
              Status::BudgetExceeded);
}

Program resettingSharedCounter(unsigned InnerStep = 1, unsigned OuterStep = 1,
                               bool Inclusive = false) {
  Program P;
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {n(0)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 5});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(16), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x600), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {3, 4});
  P.instruction({op(Inclusive ? NdOp::INT_LESSEQUAL : NdOp::INT_LESS,
                    NdVar::tmp(0, 1), {r(8), r(24)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(3, 0x400, {1});
  P.instruction({op(NdOp::INT_ADD, r(8), {r(8), n(InnerStep)}),
                 op(NdOp::INT_ADD, r(0), {r(0), n(3)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(4, 0x500, {1});
  P.instruction({op(NdOp::INT_SUB, r(16), {r(16), n(OuterStep)}),
                 op(NdOp::COPY, r(8), {n(0)}),
                 op(NdOp::INT_ADD, r(0), {r(0), n(5)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(5, 0x600);
  P.finish();
  return P;
}

TEST(LowIRLoopInference, AdditiveAndResetArmsWidenWithoutUnrollingCounterBits) {
  const auto P = resettingSharedCounter();
  LowIRLoopInferenceLimits Limits;
  Limits.MaxWideningRounds = 4;
  const auto R =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, {0x200});
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  EXPECT_GT(R.Plan->Cutpoints[0].Rank.size(), 1U);
  const auto Proof = loopCheck(P, P, *R.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  ASSERT_GT(R.WideningRounds, 0U);
  Limits.MaxWideningRounds = R.WideningRounds - 1;
  const auto Refused =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, {0x200});
  EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded);
  EXPECT_FALSE(Refused.Plan);
  Limits.MaxWideningRounds = R.WideningRounds;
  Limits.Execution.MaxSolverQueries = R.SolverQueries - 1;
  const auto QueryRefused =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, {0x200});
  EXPECT_EQ(QueryRefused.Status, LowIRLoopInferenceStatus::BudgetExceeded);
  EXPECT_FALSE(QueryRefused.Plan);
}

TEST(LowIRLoopInference, ResettingCounterStillRejectsStutterAndWrap) {
  const auto P = resettingSharedCounter();
  const auto R =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, {0x200});
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  for (const auto &Infinite :
       {resettingSharedCounter(0), resettingSharedCounter(2),
        resettingSharedCounter(1, 0), resettingSharedCounter(1, 1, true)}) {
    const auto Refused = inferLowIRLoopRefinementPlan(
        Infinite.Function, Infinite.Contract, {}, {0x200});
    EXPECT_FALSE(Refused.inferred());
    EXPECT_FALSE(Refused.Plan);
    loopRefused(loopCheck(Infinite, Infinite, *R.Plan), Status::Different);
  }
}

Program nonUnitDiamondLoop(bool UnitNoise = false) {
  Program P;
  if (UnitNoise)
    for (uint64_t Offset : {24, 32, 40})
      P.Contract.ReturnRegisters.push_back({Offset, 8});
  P.Function.Blocks[0].Succs = {1};
  P.instruction(
      {op(NdOp::COPY, r(0), {n(0)}), op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 5});
  P.instruction({op(NdOp::INT_LESSEQUAL, NdVar::tmp(0, 1), {r(8), n(1)}),
                 op(NdOp::COND_BR, {}, {n(0x600), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {3, 4});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(16), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x500), NdVar::tmp(0, 1)})});
  for (int Id : {3, 4}) {
    P.block(Id, (Id + 1) * 0x100, {1});
    if (UnitNoise)
      P.instruction(
          {op(NdOp::INT_ADD, r(Id == 3 ? 24 : 32),
              {r(Id == 3 ? 24 : 32), n(1)}),
           op(Id == 3 ? NdOp::INT_ADD : NdOp::INT_SUB, r(40), {r(40), n(1)})});
    P.instruction({op(NdOp::INT_SUB, r(8), {r(8), n(2)}),
                   op(NdOp::INT_ADD, r(0), {r(0), n(Id == 3 ? 3 : 5)}),
                   op(NdOp::BRANCH, {}, {n(0x200)})});
  }
  P.block(5, 0x600);
  P.finish();
  return P;
}

TEST(LowIRLoopInference, DiamondRetainsNonUnitScalarRank) {
  const auto P = nonUnitDiamondLoop();
  const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  const auto Proof = loopCheck(P, P, *R.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
}

Program counterChain(unsigned Counters, unsigned Accumulators = 0) {
  Program P;
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(0), {n(0)})});
  for (unsigned I = 0; I != Accumulators; ++I) {
    const auto Offset = 64 + I * 8;
    P.Contract.ReturnRegisters.push_back({Offset, 8});
    P.instruction({op(NdOp::COPY, r(Offset), {n(0)})});
  }
  P.instruction({op(NdOp::BRANCH, {}, {n(0x200)})});
  for (unsigned I = 0; I != Counters; ++I) {
    const int Header = 1 + I * 2;
    const auto Address = 0x200 + I * 0x200;
    const auto Counter = r(8 + I * 8);
    P.block(Header, Address, {Header + 1, Header + 2});
    P.instruction(
        {op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {Counter, n(0)}),
         op(NdOp::COND_BR, {}, {n(Address + 0x200), NdVar::tmp(0, 1)})});
    P.block(Header + 1, Address + 0x100, {1});
    P.instruction({op(NdOp::INT_SUB, Counter, {Counter, n(1)}),
                   op(NdOp::INT_ADD, r(0), {r(0), n(3 + I * 2)})});
    for (unsigned J = 0; J != Accumulators; ++J) {
      const auto Value = r(64 + J * 8);
      P.instruction({op(NdOp::INT_ADD, Value, {Value, n(7 + J * 2)})});
    }
    P.instruction({op(NdOp::BRANCH, {}, {n(0x200)})});
  }
  P.block(1 + Counters * 2, 0x200 + Counters * 0x200);
  P.finish();
  return P;
}

TEST(LowIRLoopInference, ObservedTuplesPrecedeUnrelatedScalarGuesses) {
  const auto P = counterChain(2, 10);
  LowIRLoopInferenceLimits Limits;
  Limits.MaxRankCandidates = 24;
  const auto R =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, {0x200});
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  const auto Proof = loopCheck(P, P, *R.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  auto Wrong = P;
  Wrong.Function.Blocks[2].Ops[1].Inputs[1] = n(4);
  loopRefused(loopCheck(P, Wrong, *R.Plan), Status::Different);
}

TEST(LowIRLoopInference, WrappingUnitNoiseDoesNotStarveNonUnitScalarRank) {
  const auto P = nonUnitDiamondLoop(true);
  LowIRLoopInferenceLimits Limits;
  Limits.MaxRankCandidates = 16;
  const auto R =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, {0x200});
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  const auto Proof = loopCheck(P, P, *R.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
}

TEST(LowIRLoopInference, TupleSearchResumesPastTheEarlyWindowWithoutReplay) {
  const auto P = counterChain(3);
  LowIRLoopInferenceLimits Limits;
  // The first valid tuple uses all three counters, after three singleton
  // and six pair proposals. Replaying the early window would exceed this.
  Limits.MaxRankCandidates = 28;
  const auto R =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, {0x200});
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  EXPECT_EQ(R.Plan->Cutpoints[0].Rank.size(), 5U);
  const auto Proof = loopCheck(P, P, *R.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  Limits.MaxRankCandidates = R.RankCandidates;
  const auto Exact =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, {0x200});
  ASSERT_TRUE(Exact.inferred()) << Exact.Diagnostic;
  --Limits.MaxRankCandidates;
  const auto Short =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, {0x200});
  EXPECT_EQ(Short.Status, LowIRLoopInferenceStatus::BudgetExceeded);
  EXPECT_FALSE(Short.Plan);
}

Program renamedCounterLoop() {
  auto P = wordLoop(1, true);
  for (auto &B : P.Function.Blocks)
    for (auto &O : B.Ops) {
      if (O.Output == r(8))
        O.Output = r(24);
      for (unsigned I = 0; I != O.NumInputs; ++I)
        if (O.Inputs[I] == r(8))
          O.Inputs[I] = r(24);
    }
  for (auto &Record : P.Records) {
    const auto &B = P.Function.Blocks[Record.BlockId];
    Record.Effects.OperationDigest =
        lowUndefinedOperationDigest(llvm::ArrayRef<LowOp>(B.Ops).slice(
            Record.Boundary.FirstOp, Record.Boundary.OpCount));
  }
  return P;
}

LowIRLoopCutpointPair counterPair(const LowIRLoopRefinementPlan &A,
                                  const LowIRLoopRefinementPlan &B) {
  return {
      A.Cutpoints.front().OriginalAddress,
      B.Cutpoints.front().OriginalAddress,
      {{regLocation(0), regLocation(0)}, {regLocation(8), regLocation(24)}}};
}

TEST(LowIRLoopPlanPairing, RenamedCounterAndDifferentArithmeticBody) {
  const auto A = wordLoop(), B = renamedCounterLoop();
  const auto IA = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  const auto IB = inferLowIRLoopRefinementPlan(B.Function, B.Contract);
  ASSERT_TRUE(IA.inferred()) << IA.Diagnostic;
  ASSERT_TRUE(IB.inferred()) << IB.Diagnostic;
  ASSERT_EQ(IA.Plan->Cutpoints.size(), 1U);
  ASSERT_EQ(IB.Plan->Cutpoints.size(), 1U);
  const auto Pair = counterPair(*IA.Plan, *IB.Plan);
  auto Plan = pairLowIRLoopRefinementPlans(*IA.Plan, *IB.Plan, {Pair});
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  const auto R = loopCheck(A, B, *Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  EXPECT_GT(R.RankingChecks, 0U);
  auto Wrong = B;
  for (auto &Block : Wrong.Function.Blocks)
    for (auto &O : Block.Ops)
      if (O.Opcode == NdOp::INT_SUB && O.Output == r(24))
        O.Inputs[1] = n(2);
  loopRefused(loopCheck(A, Wrong, *Plan), Status::Different);

  auto Uncoupled = Pair;
  Uncoupled.SharedInputs.erase(Uncoupled.SharedInputs.begin());
  auto Weak = pairLowIRLoopRefinementPlans(*IA.Plan, *IB.Plan, {Uncoupled});
  ASSERT_TRUE(bool(Weak)) << llvm::toString(Weak.takeError());
  loopRefused(loopCheck(A, B, *Weak), Status::Different);
}

TEST(LowIRLoopPlanPairing, CandidatePredicateAndOwnPrefixAreRetained) {
  const auto A = wordLoop(), B = renamedCounterLoop();
  const auto IA = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  const auto IB = inferLowIRLoopRefinementPlan(B.Function, B.Contract);
  ASSERT_TRUE(IA.inferred()) << IA.Diagnostic;
  ASSERT_TRUE(IB.inferred()) << IB.Diagnostic;
  auto Right = *IB.Plan;
  auto &Cut = Right.Cutpoints.front();
  ASSERT_TRUE(Cut.UseEntryPrefix);
  NdVar Counter;
  for (const auto &I : Cut.Inputs)
    if (I.Side == LowIRLoopSide::Original && I.Location.Offset == 24)
      Counter = I.Temporary;
  ASSERT_EQ(Counter.Size, 8U);
  const auto Prefix = NdVar::tmp(100000, 8);
  const auto Bound = NdVar::tmp(100008, 1);
  const auto Predicate = NdVar::tmp(100016, 1);
  Cut.Inputs.push_back(
      {LowIRLoopSide::OriginalPrefix, regLocation(24), Prefix});
  Cut.Expressions.push_back(op(NdOp::INT_LESSEQUAL, Bound, {Counter, Prefix}));
  Cut.Expressions.push_back(
      op(NdOp::BOOL_AND, Predicate, {Cut.Predicate, Bound}));
  Cut.Predicate = Predicate;
  const auto Pair = counterPair(*IA.Plan, Right);
  auto Plan = pairLowIRLoopRefinementPlans(*IA.Plan, Right, {Pair});
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  const auto R = loopCheck(A, B, *Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  Cut.Predicate = n(0, 1);
  auto False = pairLowIRLoopRefinementPlans(*IA.Plan, Right, {Pair});
  ASSERT_TRUE(bool(False)) << llvm::toString(False.takeError());
  loopRefused(loopCheck(A, B, *False), Status::Different);
}

TEST(LowIRLoopPlanPairing, MalformedBindingsAndExhaustedBudgetsRefuse) {
  const auto Left = wordLoopPlan();
  auto Right = Left;
  auto Pair = LowIRLoopCutpointPair{
      0x200,
      0x200,
      {{regLocation(0), regLocation(0)}, {regLocation(8), regLocation(8)}}};
  const auto Reject = [&](const LowIRLoopRefinementPlan &B,
                          llvm::ArrayRef<LowIRLoopCutpointPair> Pairs,
                          uint64_t Budget = 65536) {
    auto R = pairLowIRLoopRefinementPlans(Left, B, Pairs, Budget);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
  };
  Reject(Right, {Pair}, 0);
  Reject(Right, {});
  Reject(Right, {Pair, Pair});
  auto Missing = Pair;
  Missing.SharedInputs[0].Candidate = regLocation(64);
  Reject(Right, {Missing});
  auto Narrow = Pair;
  Narrow.SharedInputs[0].Candidate.Bytes = 4;
  Reject(Right, {Narrow});
  Right.Cutpoints[0].Inputs[1].Temporary = NdVar::tmp(4, 8);
  Reject(Right, {Pair});
  Right = Left;
  Right.Cutpoints[0].Predicate = NdVar::tmp(0, 1);
  Reject(Right, {Pair});
  Right = Left;
  Right.Cutpoints[0].Inputs[0].Side = LowIRLoopSide::Candidate;
  Reject(Right, {Pair});
  Right = Left;
  Right.Cutpoints[0].UseEntryPrefix = true;
  Reject(Right, {Pair});
  Right = Left;
  Right.Cutpoints[0].CandidateState[0].Value = n(7);
  Reject(Right, {Pair});
}

TEST(LowIRLoopPlanPairing, SideEffectsRemainVisibleToTheChecker) {
  const auto A = wordLoop();
  auto Left = wordLoopPlan(), Right = Left;
  Right.Cutpoints[0].Expressions.push_back(
      op(NdOp::LOAD, NdVar::tmp(32, 8), {NdVar::tmp(0, 8)}));
  const LowIRLoopCutpointPair Pair{
      0x200,
      0x200,
      {{regLocation(0), regLocation(0)}, {regLocation(8), regLocation(8)}}};
  auto Plan = pairLowIRLoopRefinementPlans(Left, Right, {Pair});
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  loopRefused(loopCheck(A, A, *Plan), Status::Invalid);
}

TEST(LowIRLoopPlanPairing, CandidateInputProjectionCannotDisappear) {
  const auto A = wordLoop();
  const auto Left = wordLoopPlan();
  auto Right = Left;
  // The candidate claims its accumulator parameter comes from an unrelated
  // register, while its assignments still write the actual accumulator.
  Right.Cutpoints[0].Inputs[0].Location = regLocation(64);
  const LowIRLoopCutpointPair Pair{
      0x200,
      0x200,
      {{regLocation(0), regLocation(64)}, {regLocation(8), regLocation(8)}}};
  auto Plan = pairLowIRLoopRefinementPlans(Left, Right, {Pair});
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  loopRefused(loopCheck(A, A, *Plan), Status::Different);
}

Program spilledWordLoop() {
  Program P;
  P.frame();
  P.Contract.PreservedFrameRanges = {{0, 8}};
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(8), {r(16)}), op(NdOp::COPY, r(0), {n(0)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 3});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {1});
  P.instruction({op(NdOp::INT_ADD, NdVar::tmp(0, 8), {r(32), n(uint64_t(-8))}),
                 op(NdOp::LOAD, r(0), {NdVar::tmp(0, 8)}),
                 op(NdOp::INT_ADD, r(0), {r(0), r(8)}),
                 op(NdOp::STORE, {}, {NdVar::tmp(0, 8), r(0)}),
                 op(NdOp::INT_SUB, r(8), {r(8), n(1)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(3, 0x400);
  P.finish();
  return P;
}

std::vector<LowIRLoopCutpointPair>
selfPairs(const LowIRLoopRefinementPlan &Plan) {
  std::vector<LowIRLoopCutpointPair> Pairs;
  for (const auto &Cut : Plan.Cutpoints) {
    LowIRLoopCutpointPair Pair;
    Pair.OriginalAddress = Pair.CandidateAddress = Cut.OriginalAddress;
    for (const auto &I : Cut.Inputs)
      if (I.Side == LowIRLoopSide::Original)
        Pair.SharedInputs.push_back({I.Location, I.Location});
    Pairs.push_back(std::move(Pair));
  }
  return Pairs;
}

TEST(LowIRLoopPlanPairing, FrameInputsAndWritesRemainObservable) {
  const auto A = spilledWordLoop();
  const auto Inferred = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  auto Pairs = selfPairs(*Inferred.Plan);
  bool SharedFrame = false;
  for (const auto &P : Pairs)
    for (const auto &I : P.SharedInputs)
      SharedFrame |= I.Original.Space == LowIRLoopSpace::Frame;
  ASSERT_TRUE(SharedFrame);
  auto Plan =
      pairLowIRLoopRefinementPlans(*Inferred.Plan, *Inferred.Plan, Pairs);
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  const auto Good = loopCheck(A, A, *Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  auto Wrong = A;
  Wrong.Function.Blocks[2].Ops[3].Inputs[1] = n(0);
  loopRefused(loopCheck(A, Wrong, *Plan), Status::Different);
}

Program zeroPaddedFrameLoop(uint16_t Bytes, llvm::endianness Order,
                            uint64_t Step = 3) {
  Program P;
  P.frame();
  P.Contract.ByteOrder = Order;
  P.Function.Blocks[0].Succs = {1};
  P.instruction(
      {op(NdOp::INT_ADD, r(40), {r(32), n(-8)}),
       op(NdOp::INT_ADD, r(48),
          {r(40), n(Order == llvm::endianness::little ? 0 : 8 - Bytes)}),
       op(NdOp::INT_ADD, r(56),
          {r(40), n(Order == llvm::endianness::little ? Bytes : 0)}),
       op(NdOp::STORE, {}, {r(40), n(0)}), op(NdOp::COPY, r(8), {r(16)}),
       op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 3});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {1});
  P.instruction({op(NdOp::LOAD, NdVar::tmp(0, Bytes), {r(48)}),
                 op(NdOp::INT_ADD, NdVar::tmp(8, Bytes),
                    {NdVar::tmp(0, Bytes), n(Step, Bytes)}),
                 op(NdOp::STORE, {}, {r(48), NdVar::tmp(8, Bytes)}),
                 op(NdOp::STORE, {}, {r(56), n(0, 8 - Bytes)}),
                 op(NdOp::INT_SUB, r(8), {r(8), n(1)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(3, 0x400);
  P.instruction({op(NdOp::LOAD, r(0), {r(40)})});
  P.finish();
  return P;
}

TEST(LowIRLoopInference, NarrowFrameUpdatesKeepTheirExplicitZeroPadding) {
  for (uint16_t Bytes : {1, 2, 3, 4, 7})
    for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(Order == llvm::endianness::little);
      const auto P = zeroPaddedFrameLoop(Bytes, Order);
      const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
      ASSERT_TRUE(R.inferred()) << R.Diagnostic;
      const auto Proof = loopCheck(P, P, *R.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
      loopRefused(loopCheck(P, zeroPaddedFrameLoop(Bytes, Order, 5), *R.Plan),
                  Status::Different);
      auto WrongPadding = P;
      WrongPadding.Function.Blocks[2].Ops[3].Inputs[1] = n(1, 8 - Bytes);
      loopRefused(loopCheck(P, WrongPadding, *R.Plan), Status::Different);
    }
}

TEST(LowIRLoopInference, ZeroPaddedFrameInferenceAndProofKeepSeparateBudgets) {
  const auto P = zeroPaddedFrameLoop(3, llvm::endianness::big);
  const auto Good = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  LowIRLoopInferenceLimits Exact;
  Exact.Execution.MaxOperations = Good.Operations;
  Exact.Execution.MaxSolverQueries = Good.SolverQueries;
  Exact.MaxWideningRounds = Good.WideningRounds;
  const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract, Exact);
  ASSERT_TRUE(R.inferred()) << R.Diagnostic;
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    auto Short = Exact;
    if (Kind == 0)
      --Short.Execution.MaxOperations;
    if (Kind == 1)
      --Short.Execution.MaxSolverQueries;
    if (Kind == 2)
      --Short.MaxWideningRounds;
    const auto Refused =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, Short);
    EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded)
        << Refused.Diagnostic;
    EXPECT_FALSE(Refused.Plan);
  }
  const auto Proof = loopCheck(P, P, *R.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  LowIRRefinementLimits Limit;
  Limit.Execution.MaxSolverQueries = Proof.SolverQueries;
  ASSERT_TRUE(loopCheck(P, P, *R.Plan, Limit).proved());
  --Limit.Execution.MaxSolverQueries;
  loopRefused(loopCheck(P, P, *R.Plan, Limit), Status::BudgetExceeded);
}

TEST(LowIRLoopInference, AscendingCountersSpillsAndEarlyReturns) {
  for (uint16_t Bytes : {4, 8}) {
    for (bool EarlyReturn : {false, true}) {
      Program P;
      P.frame();
      P.Function.Blocks[0].Succs = {1};
      P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {n(0)}),
                     op(NdOp::INT_ZEXT, r(24), {r(16, Bytes)}),
                     op(NdOp::INT_ADD, r(40), {r(32), n(-8)}),
                     op(NdOp::STORE, {}, {r(40), r(8)}),
                     op(NdOp::BRANCH, {}, {n(0x200)})});
      P.block(1, 0x200, {2, 4});
      const auto Condition = NdVar::tmp(0, 1);
      P.instruction({op(NdOp::INT_LESS, Condition, {r(8), r(24)}),
                     op(NdOp::COND_BR, {}, {n(0x300), Condition})});
      P.block(2, 0x300, {3});
      P.instruction(
          {op(NdOp::INT_ADD, r(0), {r(0), r(8)}),
           op(NdOp::INT_ADD, NdVar::tmp(8, Bytes), {r(8, Bytes), n(1, Bytes)}),
           op(NdOp::INT_ZEXT, r(8), {NdVar::tmp(8, Bytes)}),
           op(NdOp::STORE, {}, {r(40), r(8)}),
           op(NdOp::BRANCH, {}, {n(0x400)})});
      P.block(3, 0x400,
              EarlyReturn ? std::vector<int>{4, 1} : std::vector<int>{1});
      if (EarlyReturn)
        P.instruction({op(NdOp::INT_EQUAL, Condition, {r(0), r(48)}),
                       op(NdOp::COND_BR, {}, {n(0x500), Condition})});
      else
        P.instruction({op(NdOp::BRANCH, {}, {n(0x200)})});
      P.block(4, 0x500);
      P.finish();
      const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
      ASSERT_TRUE(R.inferred()) << "bytes=" << Bytes << " early=" << EarlyReturn
                                << ": " << R.Diagnostic;
      const auto Proof = loopCheck(P, P, *R.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    }
  }
}

TEST(LowIRLoopInference, InfiniteAndWrappingTransitionsHaveNoPlan) {
  for (unsigned Step : {0, 2}) {
    const auto A = wordLoop(Step);
    const auto R = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
    EXPECT_FALSE(R.inferred()) << R.Diagnostic;
    EXPECT_FALSE(R.Plan);
  }
}

TEST(LowIRLoopInference, SearchBudgetDoesNotExpandTheProofBudget) {
  const auto A = wordLoop();
  const auto Good = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 8; ++Kind) {
    LowIRLoopInferenceLimits Limits;
    if (Kind == 0)
      Limits.MaxCutpointAttempts = 0;
    if (Kind == 1)
      Limits.MaxWideningRounds = 0;
    if (Kind == 2)
      Limits.Execution.MaxOperations = Good.Operations - 1;
    if (Kind == 3)
      Limits.Execution.MaxSolverQueries = Good.SolverQueries - 1;
    if (Kind == 4)
      Limits.MaxRankCandidates = 0;
    if (Kind == 5)
      Limits.Execution.MaxPaths = 0;
    if (Kind == 6)
      Limits.Execution.MaxSymbolicNodes = 0;
    if (Kind == 7)
      Limits.Execution.MaxObservations = 0;
    const auto R = inferLowIRLoopRefinementPlan(A.Function, A.Contract, Limits);
    EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::BudgetExceeded)
        << R.Diagnostic;
    EXPECT_FALSE(R.Plan);
  }
  LowIRRefinementLimits ProofLimits;
  ProofLimits.Execution.MaxSolverQueries = 0;
  loopRefused(loopCheck(A, A, *Good.Plan, ProofLimits), Status::BudgetExceeded);
}

TEST(LowIRLoopInference, MalformedGraphsAndAbsentCutsHaveNoPlan) {
  const auto Good = wordLoop();
  for (unsigned Kind = 0; Kind != 6; ++Kind) {
    auto P = Good;
    if (Kind == 0)
      P.Function.Entry = 0x999;
    if (Kind == 1)
      P.Function.Blocks[0].Succs.push_back(99);
    if (Kind == 2)
      P.Function.Blocks[1].Id = P.Function.Blocks[0].Id;
    if (Kind == 3)
      P.Function.Blocks[0].InstructionBoundaries[0].FirstOp = SIZE_MAX;
    if (Kind == 4) {
      auto &Malformed = P.Function.Blocks[0].Ops[0];
      Malformed.NumInputs = std::size(Malformed.Inputs) + 1;
    }
    if (Kind == 5)
      P.Function.Blocks.clear();
    const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
    EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::Invalid)
        << "kind=" << Kind << ": " << R.Diagnostic;
    EXPECT_FALSE(R.Plan);
  }
  const va_t Absent = 0x999;
  const auto R =
      inferLowIRLoopRefinementPlan(Good.Function, Good.Contract, {}, {Absent});
  EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(R.Plan);
  EXPECT_EQ(R.CutpointAttempts, 0U);
}

TEST(LowIRLoopRefinement, PrefixInputRequiresCapturedReachablePrefix) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].Inputs.push_back(
      {LowIRLoopSide::OriginalPrefix, regLocation(0), NdVar::tmp(16, 8)});
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan.Cutpoints[0].UseEntryPrefix = true;
  ASSERT_TRUE(loopCheck(A, A, Plan).proved());
  Plan.Cutpoints[0].Inputs.back().Side = LowIRLoopSide::CandidatePrefix;
  ASSERT_TRUE(loopCheck(A, A, Plan).proved());
}

TEST(LowIRLoopRefinement, WrongBodyAndInitialStateRefuse) {
  const auto A = wordLoop();
  loopRefused(loopCheck(A, wordLoop(2), wordLoopPlan()), Status::Different);
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].CandidateState[0].Value = n(7);
  loopRefused(loopCheck(A, A, Plan), Status::Different);
}

TEST(LowIRLoopRefinement, InfiniteLoopAndUnsignedWraparoundRefuse) {
  for (unsigned Step : {0U, 2U}) {
    const auto A = wordLoop(Step);
    const auto R = loopCheck(A, A, wordLoopPlan());
    loopRefused(R, Status::Different);
    EXPECT_NE(R.Diagnostic.find("rank decrease"), std::string::npos);
  }
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].Rank = {n(0)};
  const auto A = wordLoop();
  loopRefused(loopCheck(A, A, Plan), Status::Different);
}

TEST(LowIRLoopRefinement, PredicateCannotNarrowEntryDomain) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  auto &C = Plan.Cutpoints[0];
  C.Expressions.push_back(
      op(NdOp::INT_EQUAL, NdVar::tmp(16, 1), {NdVar::tmp(8, 8), n(0)}));
  C.Predicate = NdVar::tmp(16, 1);
  const auto R = loopCheck(A, A, Plan);
  loopRefused(R, Status::Different);
  EXPECT_NE(R.Diagnostic.find("invariant predicate"), std::string::npos);
  C.Predicate = n(0, 1);
  loopRefused(loopCheck(A, A, Plan), Status::Different);
  C.Predicate = n(2, 1);
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
}

TEST(LowIRLoopRefinement, MissingCutDoesNotHideCompletedSibling) {
  auto A = wordLoop();
  // Visit the zero-count exit before exploring the unbounded sibling.
  auto &Header = A.Function.Blocks[1];
  Header.Ops[0].Opcode = NdOp::INT_NOTEQUAL;
  Header.Ops[1].Inputs[0] = n(0x300);
  Header.InstructionBoundaries[0].Immediate = 0x300;
  A.Records[1].Boundary = Header.InstructionBoundaries[0];
  A.Records[1].Effects.OperationDigest =
      lowUndefinedOperationDigest(Header.Ops);
  auto Plan = wordLoopPlan();
  // A real block, but no cycle passes through this proposed cutpoint.
  Plan.Cutpoints[0].OriginalAddress = Plan.Cutpoints[0].CandidateAddress =
      0x400;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxPaths = 40;
  const auto R = loopCheck(A, A, Plan, Limits);
  loopRefused(R, Status::BudgetExceeded);
  EXPECT_GT(R.OriginalCutpoints, 0U);
  Plan.Cutpoints[0].CandidateAddress = 0x300;
  Plan.Cutpoints[0].OriginalAddress = 0x200;
  loopRefused(loopCheck(A, A, Plan), Status::Different);
}

TEST(LowIRLoopRefinement, PrefixWritesStayObservableAfterCut) {
  auto A = wordLoop(), B = wordLoop();
  A.frame();
  auto AddStore = [](Program &P, uint64_t Value) {
    auto &Block = P.Function.Blocks[0];
    auto Store = op(NdOp::STORE, {}, {r(32), n(Value)});
    Store.Addr = Block.Ops[0].Addr;
    Block.Ops.insert(Block.Ops.begin(), Store);
    for (size_t I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
    ++Block.InstructionBoundaries[0].OpCount;
    P.Records[0].Boundary = Block.InstructionBoundaries[0];
    P.Records[0].Effects.OpCount = Block.Ops.size();
    P.Records[0].Effects.OperationDigest =
        lowUndefinedOperationDigest(Block.Ops);
  };
  AddStore(A, 7);
  AddStore(B, 8);
  auto Plan = wordLoopPlan();
  const LowIRLoopLocation Slot{LowIRLoopSpace::Frame, 0, 8};
  Plan.Cutpoints[0].OriginalState.push_back({Slot, n(7)});
  Plan.Cutpoints[0].CandidateState.push_back({Slot, n(8)});
  const auto R = loopCheck(A, B, Plan);
  loopRefused(R, Status::Different);
  EXPECT_NE(R.Diagnostic.find("written frame byte"), std::string::npos);
  A.Contract.ObserveWrittenFrameBytes = false;
  const auto Unobserved = loopCheck(A, B, Plan);
  EXPECT_TRUE(Unobserved.proved()) << Unobserved.Diagnostic;
  A.Contract.PreservedFrameRanges = {{0, 8}};
  loopRefused(loopCheck(A, B, Plan), Status::ContractViolation);
}

TEST(LowIRLoopRefinement, MalformedBindingsAndPlansRefuse) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  Plan.Cutpoints.push_back(Plan.Cutpoints.front());
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan = wordLoopPlan();
  Plan.Cutpoints[0].OriginalAddress++;
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan = wordLoopPlan();
  Plan.Cutpoints[0].Inputs[1].Temporary = NdVar::tmp(4, 8);
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan = wordLoopPlan();
  Plan.Cutpoints[0].Rank = {NdVar::tmp(31, 8)};
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan = wordLoopPlan();
  Plan.Cutpoints[0].Expressions.push_back(
      op(NdOp::LOAD, NdVar::tmp(16, 8), {n(0)}));
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan.Cutpoints[0].Expressions[0].NumInputs = 255;
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan = wordLoopPlan();
  Plan.Cutpoints[0].OriginalState.push_back(Plan.Cutpoints[0].OriginalState[0]);
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  loopRefused(loopCheck(A, A, {}), Status::Invalid);
}

TEST(LowIRLoopRefinement, RankCannotResetThroughUnrepresentedParameter) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  // Restricting the seed state while retaining an unrelated parameter would
  // let an unsound representation pretend that the ranking value decreased.
  Plan.Cutpoints[0].OriginalState[1].Value = n(0);
  auto Zero = A;
  Zero.Contract.EntryConstants.push_back({r(16), 0});
  const auto R = loopCheck(Zero, Zero, Plan);
  loopRefused(R, Status::Different);
  EXPECT_NE(R.Diagnostic.find("parameter projection"), std::string::npos);
}

TEST(LowIRLoopRefinement, AllStagesShareBudgetsAndDigestBindsPlan) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  const auto R = loopCheck(A, A, Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxInstructions = R.Instructions - 1;
  loopRefused(loopCheck(A, A, Plan, Limits), Status::BudgetExceeded);
  Limits = {};
  Limits.MaxTerminalPairs = R.TerminalPairs - 1;
  loopRefused(loopCheck(A, A, Plan, Limits), Status::BudgetExceeded);
  Limits = {};
  Limits.Execution.MaxSolverQueries = R.SolverQueries - 1;
  loopRefused(loopCheck(A, A, Plan, Limits), Status::BudgetExceeded);
  Plan.Cutpoints[0].Rank.insert(Plan.Cutpoints[0].Rank.begin(), n(42));
  const auto Other = loopCheck(A, A, Plan);
  ASSERT_TRUE(Other.proved()) << Other.Diagnostic;
  EXPECT_NE(R.Certificate->InputDigest, Other.Certificate->InputDigest);
}

TEST(LowIRLoopRefinement, EntryPrefixIsCheckedRatherThanAssumed) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].UseEntryPrefix = true;
  const auto R = loopCheck(A, wordLoop(1, true), Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  const auto Explicit = loopCheck(A, A, wordLoopPlan());
  ASSERT_TRUE(Explicit.proved());
  EXPECT_NE(R.Certificate->InputDigest, Explicit.Certificate->InputDigest);
  // Keeping the entry accumulator fixed at its first arrival cannot prove
  // later iterations. A prefix snapshot is not itself an invariant.
  Plan.Cutpoints[0].OriginalState.erase(
      Plan.Cutpoints[0].OriginalState.begin());
  Plan.Cutpoints[0].CandidateState.erase(
      Plan.Cutpoints[0].CandidateState.begin());
  loopRefused(loopCheck(A, A, Plan), Status::Different);
}

Program joinedEntryLoop(bool DifferentRegister = false,
                        bool DifferentFrame = false, unsigned Step = 1,
                        bool SplitCount = false, bool ZeroSecondArm = false) {
  Program P;
  P.frame();
  P.Function.Blocks[0].Succs = {1, 2};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {r(16)}),
                 op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(24), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x300), NdVar::tmp(0, 1)})});
  for (unsigned Arm = 0; Arm != 2; ++Arm) {
    P.block(Arm + 1, 0x200 + 0x100 * Arm, {3});
    if (SplitCount && Arm == unsigned(ZeroSecondArm))
      P.instruction({op(NdOp::COPY, r(8), {n(0)})});
    P.instruction({op(NdOp::COPY, r(40), {n(5 + (DifferentRegister && Arm))}),
                   op(NdOp::STORE, {}, {r(32), n(9 + (DifferentFrame && Arm))}),
                   op(NdOp::BRANCH, {}, {n(0x400)})});
  }
  P.block(3, 0x400, {4, 5});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x600), NdVar::tmp(0, 1)})});
  P.block(4, 0x500, {3});
  P.instruction({op(NdOp::INT_ADD, r(0), {r(0), r(8)}),
                 op(NdOp::INT_SUB, r(8), {r(8), n(Step)}),
                 op(NdOp::BRANCH, {}, {n(0x400)})});
  P.block(5, 0x600);
  P.finish();
  return P;
}

LowIRLoopRefinementPlan joinedEntryPlan(bool Generalize) {
  auto Plan = wordLoopPlan();
  auto &Cut = Plan.Cutpoints.front();
  Cut.OriginalAddress = Cut.CandidateAddress = 0x400;
  Cut.UseEntryPrefix = true;
  Cut.GeneralizeEntryPrefix = Generalize;
  return Plan;
}

TEST(LowIRLoopRefinement, GeneralizedPrefixChecksAllEntryArmsAndFullState) {
  const auto P = joinedEntryLoop();
  loopRefused(loopCheck(P, P, joinedEntryPlan(false)), Status::Different);
  const auto Plan = joinedEntryPlan(true);
  const auto Good = loopCheck(P, P, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_TRUE(Good.Certificate->LoopPlan->Cutpoints[0].GeneralizeEntryPrefix);
  EXPECT_GT(Good.LoopInitiations, 1U);
  EXPECT_EQ(Good.RankingChecks, 1U);
  // Registers not returned to the caller and every frame byte are still
  // checked at arrivals. A single captured arm cannot invent their values.
  for (bool Frame : {false, true}) {
    const auto Different = joinedEntryLoop(!Frame, Frame);
    loopRefused(loopCheck(Different, Different, Plan), Status::Different);
    loopRefused(loopCheck(P, Different, Plan), Status::Different);
  }
  for (unsigned Step : {0, 2}) {
    const auto Infinite = joinedEntryLoop(false, false, Step);
    loopRefused(loopCheck(Infinite, Infinite, Plan), Status::Different);
  }
}

TEST(LowIRLoopRefinement, GeneralizedPrefixChecksFunctionTemporaryDefinedness) {
  const auto A = joinedEntryLoop();
  auto B = A;
  B.Function.FunctionTemporaries = {{64, 8}};
  // Only one real entry arm defines the byte range. Even when the temporary
  // is unused, a generalized prefix cannot invent the other arm's definition.
  auto &Arm = B.Function.Blocks[1];
  auto Save = op(NdOp::COPY, NdVar::tmp(64, 8), {r(16)});
  Save.Addr = Arm.StartAddr;
  Arm.Ops.insert(Arm.Ops.begin(), Save);
  for (unsigned I = 0; I != Arm.Ops.size(); ++I)
    Arm.Ops[I].Seq = I;
  ++Arm.InstructionBoundaries[0].OpCount;
  const auto Bad = loopCheck(A, B, joinedEntryPlan(true));
  loopRefused(Bad, Status::Different);
  EXPECT_NE(Bad.Diagnostic.find("function-temporary definedness"),
            std::string::npos);
}

TEST(LowIRLoopRefinement, GeneralizationPolicyIsExplicitAndDigestBound) {
  auto P = joinedEntryLoop();
  P.Contract.EntryConstants.push_back({r(24), 0});
  const auto Guarded = loopCheck(P, P, joinedEntryPlan(false));
  const auto General = loopCheck(P, P, joinedEntryPlan(true));
  ASSERT_TRUE(Guarded.proved()) << Guarded.Diagnostic;
  ASSERT_TRUE(General.proved()) << General.Diagnostic;
  EXPECT_NE(Guarded.Certificate->InputDigest, General.Certificate->InputDigest);
  auto Invalid = joinedEntryPlan(true);
  Invalid.Cutpoints[0].UseEntryPrefix = false;
  loopRefused(loopCheck(P, P, Invalid), Status::Invalid);
  // An unrelated induction parameter must not reset the rank.
  Invalid = joinedEntryPlan(true);
  Invalid.Cutpoints[0].OriginalState[1].Value = n(0);
  Invalid.Cutpoints[0].CandidateState[1].Value = n(0);
  P.Contract.EntryConstants.push_back({r(16), 0});
  loopRefused(loopCheck(P, P, Invalid), Status::Different);
}

TEST(LowIRLoopRefinement, GeneralizedPrefixRetainsSharedProofBudgets) {
  const auto P = joinedEntryLoop();
  const auto Plan = joinedEntryPlan(true);
  const auto Good = loopCheck(P, P, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  for (bool Zero : {false, true})
    for (unsigned Kind = 0; Kind != 4; ++Kind) {
      LowIRRefinementLimits Limits;
      if (Kind == 0)
        Limits.Execution.MaxOperations = Zero ? 0 : Good.Operations - 1;
      if (Kind == 1)
        Limits.Execution.MaxSolverQueries = Zero ? 0 : Good.SolverQueries - 1;
      if (Kind == 2)
        Limits.Execution.MaxPaths =
            Zero ? 0 : Good.OriginalPaths + Good.CandidatePaths - 1;
      if (Kind == 3)
        Limits.MaxTerminalPairs = Zero ? 0 : Good.TerminalPairs - 1;
      loopRefused(loopCheck(P, P, Plan, Limits), Status::BudgetExceeded);
    }
}

TEST(LowIRLoopRefinement, PrefixFeasibilityDoesNotCanonicalizePredicate) {
  Program P;
  P.Function.Blocks[0].Succs = {1, 3};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {r(16)}),
                 op(NdOp::INT_NOTEQUAL, NdVar::tmp(0, 1), {r(24), n(1)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(1, 0x200, {2, 3});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {1});
  P.instruction({op(NdOp::INT_ADD, r(0), {r(0), r(8)}),
                 op(NdOp::INT_SUB, r(8), {r(8), n(1)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(3, 0x400);
  P.finish();
  auto Plan = wordLoopPlan();
  auto &Cut = Plan.Cutpoints.front();
  Cut.UseEntryPrefix = Cut.GeneralizeEntryPrefix = true;
  Cut.Inputs.push_back(
      {LowIRLoopSide::Entry, regLocation(24, 1), NdVar::tmp(16, 1)});
  // Every real arrival has entry r24 == 1, but the untrusted template must
  // validate its Boolean on the entire entry domain, including r24 == 2.
  Cut.Predicate = NdVar::tmp(16, 1);
  const auto Bad = loopCheck(P, P, Plan);
  loopRefused(Bad, Status::Invalid);
  EXPECT_NE(Bad.Diagnostic.find("canonical Boolean"), std::string::npos);
  Cut.Predicate = n(1, 1);
  const auto Good = loopCheck(P, P, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
}

TEST(LowIRLoopInference, JoinedEntryArmsUseRecheckedGeneralizedTemplate) {
  const auto P = joinedEntryLoop();
  const auto Inferred = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  ASSERT_EQ(Inferred.Plan->Cutpoints.size(), 1U);
  EXPECT_TRUE(Inferred.Plan->Cutpoints[0].GeneralizeEntryPrefix);
  const auto Proof = loopCheck(P, P, *Inferred.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  auto Wrong = P;
  Wrong.Function.Blocks[4].Ops[0].Opcode = NdOp::INT_SUB;
  loopRefused(loopCheck(P, Wrong, *Inferred.Plan), Status::Different);
}

TEST(LowIRLoopInference, ZeroIterationWitnessDoesNotHideAnotherEntryLoop) {
  for (bool ZeroSecondArm : {false, true}) {
    const auto P = joinedEntryLoop(false, false, 1, true, ZeroSecondArm);
    const auto Inferred =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, {0x400});
    ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
    ASSERT_EQ(Inferred.Plan->Cutpoints.size(), 1U);
    EXPECT_EQ(Inferred.Plan->Cutpoints[0].CandidateAddress, 0x400U);
    EXPECT_TRUE(Inferred.Plan->Cutpoints[0].GeneralizeEntryPrefix);
    const auto Good = loopCheck(P, P, *Inferred.Plan);
    ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
    auto Wrong = P;
    Wrong.Function.Blocks[4].Ops[0].Opcode = NdOp::INT_SUB;
    loopRefused(loopCheck(P, Wrong, *Inferred.Plan), Status::Different);
  }
}

// Two sequential countdown loops have entry arms that agree on their
// respective domains but can produce different total prefix expressions.
// Padding changes which arm a breadth-first witness search reaches first.
Program joinedPrefixLoops(bool SameExpression, bool ReverseCondition,
                          bool ToggleScratch, unsigned ShortPadding) {
  Program P;
  P.Function.Blocks[0].Succs = {1, 2};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {r(16)}),
                 op(NdOp::COPY, r(24), {r(32)}),
                 op(NdOp::INT_AND, r(56), {r(48), n(~uint64_t{1})}),
                 op(ReverseCondition ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL,
                    NdVar::tmp(0, 1), {r(56), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x200), NdVar::tmp(0, 1)})});
  for (unsigned Arm = 0; Arm != 2; ++Arm) {
    P.block(Arm + 1, 0x200 + Arm * 0x100, {Arm ? 3 : 5});
    const bool ZeroArm = ReverseCondition ? Arm == 0 : Arm == 1;
    P.instruction(
        {op(NdOp::COPY, r(40), {(!SameExpression && ZeroArm) ? n(0) : r(56)}),
         op(NdOp::BRANCH, {},
            {n(Arm ? 0x400 : (ShortPadding ? 0xc00 : 0x600))})});
    if (!Arm && ShortPadding)
      P.Function.Blocks.back().Succs = {11};
  }
  P.block(3, 0x400, {4});
  P.instruction({op(NdOp::BRANCH, {}, {n(0x500)})});
  P.block(4, 0x500, {5});
  P.instruction({op(NdOp::BRANCH, {}, {n(0x600)})});
  P.block(5, 0x600, {6, 8});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x900), NdVar::tmp(0, 1)})});
  P.block(6, 0x700, {5});
  P.instruction({op(NdOp::INT_ADD, r(0), {r(0), r(8)}),
                 op(NdOp::INT_SUB, r(8), {r(8), n(1)})});
  if (ToggleScratch)
    P.instruction({op(NdOp::INT_XOR, r(40), {r(40), n(1)})});
  P.instruction({op(NdOp::BRANCH, {}, {n(0x600)})});
  P.block(8, 0x900, {9, 10});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(24), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0xb00), NdVar::tmp(0, 1)})});
  P.block(9, 0xa00, {8});
  P.instruction({op(NdOp::INT_ADD, r(0), {r(0), r(24)}),
                 op(NdOp::INT_SUB, r(24), {r(24), n(1)})});
  if (ToggleScratch)
    P.instruction({op(NdOp::INT_XOR, r(40), {r(40), n(1)})});
  P.instruction({op(NdOp::BRANCH, {}, {n(0x900)})});
  P.block(10, 0xb00);
  P.finish();
  for (unsigned I = 0; I != ShortPadding; ++I) {
    P.block(11 + I, 0xc00 + 0x100 * I,
            {I + 1 == ShortPadding ? 5 : int(12 + I)});
    P.instruction({op(NdOp::BRANCH, {},
                      {n(I + 1 == ShortPadding ? 0x600 : 0xd00 + 0x100 * I)})});
  }
  return P;
}

TEST(LowIRLoopInference, JoinedPrefixUsesCompleteEntryTraversal) {
  for (bool Same : {false, true})
    for (bool Reverse : {false, true})
      for (bool Toggle : {false, true})
        for (unsigned Padding : {0U, 3U}) {
          SCOPED_TRACE(::testing::Message()
                       << Same << Reverse << Toggle << Padding);
          const auto P = joinedPrefixLoops(Same, Reverse, Toggle, Padding);
          const auto I = inferLowIRLoopRefinementPlan(P.Function, P.Contract,
                                                      {}, {0x600, 0x900});
          ASSERT_TRUE(I.inferred()) << I.Diagnostic;
          ASSERT_EQ(I.Plan->Cutpoints.size(), 2U);
          const auto Check = checkLowIRLoopRefinement(
              P.Function, P.Records, P.Function, P.Contract, *I.Plan,
              Witness::LiftedBits, {});
          ASSERT_TRUE(Check.proved()) << Check.Diagnostic;
          ASSERT_TRUE(Check.Certificate);
          EXPECT_EQ(Check.LoopInitiations, 3U);
          EXPECT_EQ(Check.RankingChecks, 3U);
        }
}

TEST(LowIRLoopInference, JoinedPrefixStillChecksEveryArrivalAndProgress) {
  for (bool Toggle : {false, true}) {
    const auto P = joinedPrefixLoops(false, false, Toggle, 0);
    const auto I = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {},
                                                {0x600, 0x900});
    ASSERT_TRUE(I.inferred()) << I.Diagnostic;
    for (unsigned Fault = 0; Fault != 3; ++Fault) {
      SCOPED_TRACE(Fault);
      auto Wrong = P;
      auto &Body = Wrong.Function.Blocks[6];
      ASSERT_EQ(Body.StartAddr, 0x700U);
      if (Fault == 0)
        Body.Ops[0].Opcode = NdOp::INT_SUB;
      else if (Fault == 1)
        Body.Ops[1].Inputs[1] = n(0);
      else {
        auto &Arm = Wrong.Function.Blocks[1];
        ASSERT_EQ(Arm.StartAddr, 0x200U);
        Arm.Ops[0].Inputs[0] = n(2);
      }
      const auto Check = checkLowIRLoopRefinement(
          P.Function, P.Records, Wrong.Function, P.Contract, *I.Plan,
          Witness::LiftedBits, {});
      EXPECT_FALSE(Check.proved());
      EXPECT_FALSE(Check.Certificate);
      EXPECT_EQ(Check.Status, Status::Different) << Check.Diagnostic;
    }
    auto WrongRank = *I.Plan;
    for (auto &Cut : WrongRank.Cutpoints)
      Cut.Rank.assign(Cut.Rank.size(), n(0));
    const auto Check =
        checkLowIRLoopRefinement(P.Function, P.Records, P.Function, P.Contract,
                                 WrongRank, Witness::LiftedBits, {});
    EXPECT_FALSE(Check.proved());
    EXPECT_FALSE(Check.Certificate);
    EXPECT_EQ(Check.Status, Status::Different) << Check.Diagnostic;
  }
}

TEST(LowIRLoopInference,
     JoinedPrefixPreservesIndependentInferenceAndProofBudgets) {
  for (bool Toggle : {false, true}) {
    const auto P = joinedPrefixLoops(false, false, Toggle, 0);
    const auto I = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {},
                                                {0x600, 0x900});
    ASSERT_TRUE(I.inferred()) << I.Diagnostic;
    LowIRLoopInferenceLimits Exact;
    Exact.Execution.MaxOperations = I.Operations;
    Exact.Execution.MaxSolverQueries = I.SolverQueries;
    Exact.MaxWideningRounds = I.WideningRounds;
    ASSERT_TRUE(inferLowIRLoopRefinementPlan(P.Function, P.Contract, Exact,
                                             {0x600, 0x900})
                    .inferred());
    for (unsigned Kind = 0; Kind != 3; ++Kind) {
      auto Short = Exact;
      if (!Kind)
        --Short.Execution.MaxOperations;
      else if (Kind == 1)
        --Short.Execution.MaxSolverQueries;
      else
        --Short.MaxWideningRounds;
      const auto Refused = inferLowIRLoopRefinementPlan(P.Function, P.Contract,
                                                        Short, {0x600, 0x900});
      EXPECT_FALSE(Refused.inferred());
      EXPECT_FALSE(Refused.Plan);
      EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded)
          << Refused.Diagnostic;
    }
    const auto Good =
        checkLowIRLoopRefinement(P.Function, P.Records, P.Function, P.Contract,
                                 *I.Plan, Witness::LiftedBits, {});
    ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
    LowIRRefinementLimits Proof;
    Proof.Execution.MaxOperations = Good.Operations;
    Proof.Execution.MaxSolverQueries = Good.SolverQueries;
    ASSERT_TRUE(checkLowIRLoopRefinement(P.Function, P.Records, P.Function,
                                         P.Contract, *I.Plan,
                                         Witness::LiftedBits, Proof)
                    .proved());
    for (unsigned Kind = 0; Kind != 2; ++Kind) {
      auto Short = Proof;
      if (!Kind)
        --Short.Execution.MaxOperations;
      else
        --Short.Execution.MaxSolverQueries;
      const auto Refused = checkLowIRLoopRefinement(
          P.Function, P.Records, P.Function, P.Contract, *I.Plan,
          Witness::LiftedBits, Short);
      EXPECT_FALSE(Refused.proved());
      EXPECT_FALSE(Refused.Certificate);
      EXPECT_EQ(Refused.Status, Status::BudgetExceeded) << Refused.Diagnostic;
    }
  }
}

Program nestedWordLoops() {
  Program A;
  A.Function.Blocks[0].Succs = {1};
  A.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {r(16)}),
                 op(NdOp::COPY, r(24), {n(0)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  A.block(1, 0x200, {2, 6});
  A.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x700), NdVar::tmp(0, 1)})});
  A.block(2, 0x300, {3});
  A.instruction(
      {op(NdOp::COPY, r(24), {r(32)}), op(NdOp::BRANCH, {}, {n(0x400)})});
  A.block(3, 0x400, {4, 5});
  A.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(24), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x600), NdVar::tmp(0, 1)})});
  A.block(4, 0x500, {3});
  A.instruction({op(NdOp::INT_ADD, r(0), {r(0), r(24)}),
                 op(NdOp::INT_SUB, r(24), {r(24), n(1)}),
                 op(NdOp::BRANCH, {}, {n(0x400)})});
  A.block(5, 0x600, {1});
  A.instruction({op(NdOp::INT_SUB, r(8), {r(8), n(1)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  A.block(6, 0x700);
  A.finish();
  return A;
}

TEST(LowIRLoopRefinement, NestedLoopsRequireLexicographicPhaseAndCount) {
  const auto A = nestedWordLoops();
  auto Plan = wordLoopPlan();
  auto &Outer = Plan.Cutpoints[0];
  Outer.Rank = {NdVar::tmp(8, 8), n(1, 1), n(0)};
  Outer.OriginalState.push_back({regLocation(24), n(0)});
  Outer.CandidateState = Outer.OriginalState;
  auto Inner = Outer;
  Inner.OriginalAddress = Inner.CandidateAddress = 0x400;
  Inner.Inputs.push_back(
      {LowIRLoopSide::Original, regLocation(24), NdVar::tmp(16, 8)});
  Inner.OriginalState.back().Value = NdVar::tmp(16, 8);
  Inner.CandidateState = Inner.OriginalState;
  Inner.Expressions = {
      op(NdOp::INT_NOTEQUAL, NdVar::tmp(24, 1), {NdVar::tmp(8, 8), n(0)})};
  Inner.Predicate = NdVar::tmp(24, 1);
  Inner.Rank = {NdVar::tmp(8, 8), n(0, 1), NdVar::tmp(16, 8)};
  Plan.Cutpoints.push_back(Inner);
  const auto Good = loopCheck(A, A, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.RankingChecks, 3U);
  EXPECT_EQ(Good.LoopTransitions, 3U);
  // Dropping the nonzero outer-domain obligation admits underflow on the
  // inner exit, even though finite runs of small positive counts look fine.
  Plan.Cutpoints[1].Predicate = n(1, 1);
  loopRefused(loopCheck(A, A, Plan), Status::Different);
  Plan.Cutpoints[1].Predicate = Inner.Predicate;
  Plan.Cutpoints[1].Rank[1] = n(0, 8);
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan.Cutpoints[1].Rank = Inner.Rank;
  Plan.Cutpoints[1].UseEntryPrefix = true;
  // An independently reached inner prefix requires a positive entry count.
  // Unconstrained outer parameters must not invent reachability when the
  // original entry count is zero.
  loopRefused(loopCheck(A, A, Plan), Status::Different);
  for (auto &Cut : Plan.Cutpoints) {
    Cut.Inputs.push_back(
        {LowIRLoopSide::Entry, regLocation(16), NdVar::tmp(32, 8)});
    Cut.Expressions.push_back(op(NdOp::INT_LESSEQUAL, NdVar::tmp(40, 1),
                                 {Cut.Rank[0], NdVar::tmp(32, 8)}));
    Cut.Expressions.push_back(op(NdOp::BOOL_AND, NdVar::tmp(48, 1),
                                 {Cut.Predicate, NdVar::tmp(40, 1)}));
    Cut.Predicate = NdVar::tmp(48, 1);
  }
  const auto Prefix = loopCheck(A, A, Plan);
  ASSERT_TRUE(Prefix.proved()) << Prefix.Diagnostic;
  EXPECT_GT(Prefix.LoopInitiations, Good.LoopInitiations);
  EXPECT_EQ(Prefix.RankingChecks, Good.RankingChecks);
  EXPECT_NE(Prefix.Certificate->InputDigest, Good.Certificate->InputDigest);
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxOperations = Prefix.Operations - 1;
  loopRefused(loopCheck(A, A, Plan, Limits), Status::BudgetExceeded);
}

TEST(LowIRLoopInference, NestedCountersNeedNoHandwrittenPhases) {
  const auto A = nestedWordLoops();
  for (const auto &Eligible :
       {std::vector<va_t>{}, std::vector<va_t>{0x300, 0x500}}) {
    const auto Inferred =
        inferLowIRLoopRefinementPlan(A.Function, A.Contract, {}, Eligible);
    ASSERT_TRUE(Inferred.inferred())
        << Inferred.Diagnostic << " cuts=" << Inferred.CutpointAttempts
        << " rounds=" << Inferred.WideningRounds;
    if (!Eligible.empty())
      ASSERT_GT(Inferred.Plan->Cutpoints.size(), 1U);
    EXPECT_GT(Inferred.Plan->Cutpoints[0].Rank.size(), 1U);
    const auto Proof = loopCheck(A, A, *Inferred.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    auto Wrong = A;
    Wrong.Function.Blocks[4].Ops[0].Opcode = NdOp::INT_SUB;
    loopRefused(loopCheck(A, Wrong, *Inferred.Plan), Status::Different);
    LowIRLoopInferenceLimits Limits;
    Limits.MaxCutpointAttempts = Inferred.Plan->Cutpoints.size() - 1;
    const auto TooFew =
        inferLowIRLoopRefinementPlan(A.Function, A.Contract, Limits, Eligible);
    EXPECT_EQ(TooFew.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_FALSE(TooFew.Plan);
  }
}

TEST(LowIRLoopPlanPairing, AllNestedCutsAndFreshProofBudgetsAreRequired) {
  const auto A = nestedWordLoops();
  const auto Inferred =
      inferLowIRLoopRefinementPlan(A.Function, A.Contract, {}, {0x300, 0x500});
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  ASSERT_GT(Inferred.Plan->Cutpoints.size(), 1U);
  auto Pairs = selfPairs(*Inferred.Plan);
  auto Plan =
      pairLowIRLoopRefinementPlans(*Inferred.Plan, *Inferred.Plan, Pairs);
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  const auto Good = loopCheck(A, A, *Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_GT(Good.RankingChecks, 1U);
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxSolverQueries = 0;
  loopRefused(loopCheck(A, A, *Plan, Limits), Status::BudgetExceeded);
  Pairs.pop_back();
  auto Incomplete =
      pairLowIRLoopRefinementPlans(*Inferred.Plan, *Inferred.Plan, Pairs);
  EXPECT_FALSE(bool(Incomplete));
  if (!Incomplete)
    llvm::consumeError(Incomplete.takeError());
}

Program alternativeWordLoops(bool ReverseChoice, unsigned FirstStep = 1) {
  Program P;
  P.Function.Blocks[0].Succs = {1, 3};
  P.instruction({op(NdOp::COPY, r(8), {r(16)}),
                 op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(24), n(0)}),
                 op(NdOp::COND_BR, {},
                    {n(ReverseChoice ? 0x400 : 0x200), NdVar::tmp(0, 1)})});
  for (unsigned Side = 0; Side != 2; ++Side) {
    const auto Header = 0x200 + 0x200 * Side;
    const int HeaderId = 1 + 2 * Side;
    P.block(HeaderId, Header, {HeaderId + 1, 5});
    P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                   op(NdOp::COND_BR, {}, {n(0x600), NdVar::tmp(0, 1)})});
    P.block(HeaderId + 1, Header + 0x100, {HeaderId});
    P.instruction({op(NdOp::INT_ADD, r(0), {r(0), n(Side + 1)}),
                   op(NdOp::INT_SUB, r(8), {r(8), n(Side ? 1 : FirstStep)}),
                   op(NdOp::BRANCH, {}, {n(Header)})});
  }
  P.block(5, 0x600);
  P.finish();
  return P;
}

TEST(LowIRLoopInference, AlternativeLoopsReachBothPrefixesWithinSharedBudgets) {
  for (bool ReverseChoice : {false, true}) {
    SCOPED_TRACE(ReverseChoice);
    const auto P = alternativeWordLoops(ReverseChoice);
    const auto Inferred = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
    ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
    ASSERT_EQ(Inferred.Plan->Cutpoints.size(), 2U);
    const auto Proof = loopCheck(P, P, *Inferred.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    EXPECT_EQ(Proof.RankingChecks, 2U);

    auto Wrong = P;
    Wrong.Function.Blocks[4].Ops[0].Opcode = NdOp::INT_SUB;
    loopRefused(loopCheck(P, Wrong, *Inferred.Plan), Status::Different);
    const auto Infinite = alternativeWordLoops(ReverseChoice, 0);
    loopRefused(loopCheck(Infinite, Infinite, *Inferred.Plan),
                Status::Different);

    LowIRLoopInferenceLimits Search;
    Search.Execution.MaxPaths = Inferred.ScheduledPaths - 1;
    const auto Refused =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, Search);
    EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_FALSE(Refused.Plan);
    LowIRRefinementLimits Limits;
    Limits.Execution.MaxPaths = Proof.OriginalPaths + Proof.CandidatePaths - 1;
    loopRefused(loopCheck(P, P, *Inferred.Plan, Limits),
                Status::BudgetExceeded);
  }
}

enum class CounterTag { Zero, Prefix, Changing };
enum class CounterCopy { None, Word, Lane };

Program projectedCounterLoops(
    uint16_t Bytes, bool Ascending, unsigned Step = 1, bool Guarded = true,
    LowIRLoopSpace Space = LowIRLoopSpace::Register, bool EqualityExit = false,
    unsigned BoundStep = 0, bool PadBounds = false, bool ZeroInitialize = false,
    bool CachedExit = false, bool OrderedExit = false,
    bool MutableBounds = false, CounterTag Tag = CounterTag::Zero,
    CounterCopy Copy = CounterCopy::None) {
  Program P;
  const uint64_t Mask = (uint64_t{1} << (Bytes * 8)) - 1;
  if (Space == LowIRLoopSpace::Frame)
    P.Contract.Frame = LowIRIndependenceFrame{{32, 8}, -32, 0};
  if (Space == LowIRLoopSpace::FunctionTemporary)
    P.Function.FunctionTemporaries = {{67, 8}, {83, 8}};
  const auto Counter = [&](unsigned Level) {
    return Space == LowIRLoopSpace::FunctionTemporary
               ? NdVar::tmp(67 + 16 * Level, 8)
               : r(8 + 8 * Level);
  };
  const auto Address = [&](unsigned Level) {
    return Space == LowIRLoopSpace::Frame
               ? op(NdOp::INT_ADD, NdVar::tmp(0, 8),
                    {r(32), n(static_cast<uint64_t>(-32 + 8 * int(Level)))})
               : op(NdOp::COPY, NdVar::tmp(0, 8), {n(0)});
  };
  const auto Read = [&](unsigned Level) {
    return Space == LowIRLoopSpace::Frame
               ? op(NdOp::LOAD, NdVar::tmp(8, 8), {NdVar::tmp(0, 8)})
               : op(NdOp::COPY, NdVar::tmp(8, 8), {Counter(Level)});
  };
  const auto Write = [&](unsigned Level, NdVar Value) {
    return Space == LowIRLoopSpace::Frame
               ? op(NdOp::STORE, {}, {NdVar::tmp(0, 8), Value})
               : op(NdOp::COPY, Counter(Level), {Value});
  };
  const auto CacheExit = [&](unsigned Level) {
    if (CachedExit)
      P.instruction(
          {Address(Level), Read(Level),
           op(NdOp::SUBBYTES, NdVar::tmp(16, Bytes), {NdVar::tmp(8, 8), n(0)}),
           op(NdOp::SUBBYTES, NdVar::tmp(24, Bytes), {r(64 + 8 * Level), n(0)}),
           op(OrderedExit ? NdOp::INT_LESSEQUAL : NdOp::INT_EQUAL,
              r(176 + 8 * Level, 1),
              {NdVar::tmp(OrderedExit ? 24 : 16, Bytes),
               NdVar::tmp(OrderedExit ? 16 : 24, Bytes)})});
  };
  const auto Initialize = [&](unsigned Level) {
    P.instruction({Address(Level),
                   op(NdOp::INT_AND, NdVar::tmp(8, 8),
                      {r(96 + 8 * Level), n(ZeroInitialize ? 0 : ~Mask)}),
                   op(NdOp::INT_OR, NdVar::tmp(16, 8),
                      {NdVar::tmp(8, 8), Ascending ? n(0) : r(64 + 8 * Level)}),
                   Write(Level, NdVar::tmp(16, 8))});
    CacheExit(Level);
  };
  if (Tag != CounterTag::Zero)
    P.instruction({op(NdOp::INT_LEFT, r(96), {r(96), n(Bytes * 8)}),
                   op(NdOp::INT_LEFT, r(104), {r(104), n(Bytes * 8)})});
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::INT_ZEXT, r(64), {r(112, Bytes)}),
                 op(NdOp::INT_ZEXT, r(72), {r(120, Bytes)}),
                 op(NdOp::COPY, r(0), {n(0)})});
  // The raw input must not stand in for the derived prefix bound.
  if (MutableBounds)
    P.instruction({op(NdOp::INT_XOR, r(64), {r(64), n((Mask + 1) / 2)}),
                   op(NdOp::INT_XOR, r(72), {r(72), n((Mask + 1) / 2)})});
  if (PadBounds)
    P.instruction({op(NdOp::INT_AND, NdVar::tmp(0, 8), {r(160), n(~Mask)}),
                   op(NdOp::INT_OR, r(64), {r(64), NdVar::tmp(0, 8)}),
                   op(NdOp::INT_AND, NdVar::tmp(8, 8), {r(168), n(~Mask)}),
                   op(NdOp::INT_OR, r(72), {r(72), NdVar::tmp(8, 8)})});
  Initialize(0);
  Initialize(1);
  P.instruction({op(NdOp::BRANCH, {}, {n(0x200)})});
  const auto Header = [&](int Id, va_t PC, va_t Done, int DoneId,
                          unsigned Level) {
    P.block(Id, PC, {Id + 1, DoneId});
    P.instruction(
        {Address(Level), Read(Level),
         op(NdOp::SUBBYTES, NdVar::tmp(16, Bytes), {NdVar::tmp(8, 8), n(0)}),
         op(NdOp::SUBBYTES, NdVar::tmp(24, Bytes), {r(64 + 8 * Level), n(0)}),
         CachedExit && Guarded
             ? op(NdOp::COPY, NdVar::tmp(32, 1), {r(176 + 8 * Level, 1)})
         : Guarded
             ? op(Ascending && !EqualityExit ? NdOp::INT_LESSEQUAL
                                             : NdOp::INT_EQUAL,
                  NdVar::tmp(32, 1),
                  {Ascending ? NdVar::tmp(24, Bytes) : NdVar::tmp(16, Bytes),
                   Ascending ? NdVar::tmp(16, Bytes) : n(0, Bytes)})
             : op(NdOp::COPY, NdVar::tmp(32, 1), {n(0, 1)}),
         op(NdOp::COND_BR, {}, {n(Done), NdVar::tmp(32, 1)})});
  };
  const auto Update = [&](unsigned Level) {
    auto ReadUpdate = Read(Level);
    if (Copy != CounterCopy::None && Level == 0)
      ReadUpdate = op(NdOp::COPY, NdVar::tmp(8, 8), {r(200)});
    if (Tag == CounterTag::Zero) {
      P.instruction(
          {Address(Level), ReadUpdate,
           op(NdOp::SUBBYTES, NdVar::tmp(16, Bytes), {NdVar::tmp(8, 8), n(0)}),
           op(Ascending ? NdOp::INT_ADD : NdOp::INT_SUB, NdVar::tmp(24, Bytes),
              {NdVar::tmp(16, Bytes), n(Step, Bytes)}),
           op(NdOp::INT_ZEXT, NdVar::tmp(32, 8), {NdVar::tmp(24, Bytes)}),
           Write(Level, NdVar::tmp(32, 8))});
    } else {
      P.instruction(
          {Address(Level), ReadUpdate,
           op(NdOp::SUBBYTES, NdVar::tmp(16, Bytes), {NdVar::tmp(8, 8), n(0)}),
           op(Ascending ? NdOp::INT_ADD : NdOp::INT_SUB, NdVar::tmp(24, Bytes),
              {NdVar::tmp(16, Bytes), n(Step, Bytes)}),
           op(NdOp::INT_ZEXT, NdVar::tmp(32, 8), {NdVar::tmp(24, Bytes)}),
           Tag == CounterTag::Changing
               ? op(NdOp::INT_ADD, r(96 + 8 * Level),
                    {r(96 + 8 * Level), n(Mask + 1)})
               : op(NdOp::COPY, r(96 + 8 * Level), {r(96 + 8 * Level)}),
           op(NdOp::INT_AND, NdVar::tmp(40, 8), {r(96 + 8 * Level), n(~Mask)}),
           op(NdOp::INT_OR, NdVar::tmp(32, 8),
              {NdVar::tmp(32, 8), NdVar::tmp(40, 8)}),
           Write(Level, NdVar::tmp(32, 8))});
    }
    if (BoundStep)
      P.instruction({op(NdOp::INT_ADD, r(64 + 8 * Level),
                        {r(64 + 8 * Level), n(BoundStep)})});
    // Preserve the comparison lane while changing the full observed word.
    if (MutableBounds)
      P.instruction({op(NdOp::INT_ADD, r(64 + 8 * Level),
                        {r(64 + 8 * Level), n(Mask + 1)})});
    CacheExit(Level);
  };
  Header(1, 0x200, 0x700, 6, 0);
  P.block(2, 0x300, {3});
  if (Copy != CounterCopy::None)
    P.instruction(
        {Address(0), Read(0),
         op(NdOp::INT_AND, NdVar::tmp(16, 8), {NdVar::tmp(8, 8), n(Mask)}),
         op(NdOp::INT_AND, NdVar::tmp(24, 8), {r(104), n(~Mask)}),
         Copy == CounterCopy::Word
             ? op(NdOp::COPY, r(200), {NdVar::tmp(8, 8)})
             : op(NdOp::INT_OR, r(200),
                  {NdVar::tmp(16, 8), NdVar::tmp(24, 8)})});
  Initialize(1);
  P.instruction({op(NdOp::BRANCH, {}, {n(0x400)})});
  Header(3, 0x400, 0x600, 5, 1);
  P.block(4, 0x500, {3});
  P.instruction(
      {Address(1), Read(1), op(NdOp::INT_ADD, r(0), {r(0), NdVar::tmp(8, 8)})});
  Update(1);
  P.instruction({op(NdOp::BRANCH, {}, {n(0x400)})});
  P.block(5, 0x600, {1});
  Update(0);
  P.instruction({op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(6, 0x700);
  P.finish();
  return P;
}

TEST(LowIRLoopInference, RebuiltCounterLanesPreserveCompleteState) {
  for (uint16_t Bytes : {1, 3, 4})
    for (auto Space : {LowIRLoopSpace::Register, LowIRLoopSpace::Frame,
                       LowIRLoopSpace::FunctionTemporary})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big})
        for (bool Cached : {false, true})
          for (auto Tag : {CounterTag::Prefix, CounterTag::Changing}) {
            SCOPED_TRACE(Bytes);
            SCOPED_TRACE(static_cast<unsigned>(Space));
            SCOPED_TRACE(static_cast<unsigned>(Order));
            SCOPED_TRACE(Cached);
            SCOPED_TRACE(static_cast<unsigned>(Tag));
            auto P =
                projectedCounterLoops(Bytes, true, 1, true, Space, true, 0,
                                      true, false, Cached, false, false, Tag);
            P.Contract.ByteOrder = Order;
            for (uint64_t Offset : {8, 16, 64, 72, 96, 104})
              P.Contract.ReturnRegisters.push_back({Offset, 8});
            const std::vector<va_t> Cuts =
                Cached ? std::vector<va_t>{0x200, 0x400}
                       : std::vector<va_t>{0x300, 0x500};
            const auto R =
                inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
            ASSERT_TRUE(R.inferred()) << R.Diagnostic;
            const auto Proof = loopCheck(P, P, *R.Plan);
            ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
            // Rank projection never hides the remainder of an observed word.
            // Keep the counter lane and tag registers unchanged, but omit the
            // high tag when writing each updated counter.
            auto Wrong = P;
            for (auto &B : Wrong.Function.Blocks)
              for (auto &O : B.Ops)
                if (O.Opcode == NdOp::INT_OR && O.Output == NdVar::tmp(32, 8) &&
                    O.Inputs[1] == NdVar::tmp(40, 8))
                  O.Inputs[1] = n(0);
            loopRefused(loopCheck(P, Wrong, *R.Plan), Status::Different);
          }
}

TEST(LowIRLoopInference, RebuiltCounterLanesKeepProofBoundaries) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (auto Tag : {CounterTag::Prefix, CounterTag::Changing}) {
      const auto Make = [&](unsigned Step, bool Guarded) {
        auto P = projectedCounterLoops(1, true, Step, Guarded,
                                       LowIRLoopSpace::Register, true, 0, true,
                                       false, false, false, false, Tag);
        P.Contract.ByteOrder = Order;
        for (uint64_t Offset : {8, 16, 64, 72, 96, 104})
          P.Contract.ReturnRegisters.push_back({Offset, 8});
        return P;
      };
      const auto P = Make(1, true);
      const std::vector<va_t> Cuts{0x300, 0x500};
      const auto Good =
          inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
      ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
      for (const auto &Bad : {Make(0, true), Make(2, true), Make(1, false)}) {
        const auto R =
            inferLowIRLoopRefinementPlan(Bad.Function, Bad.Contract, {}, Cuts);
        EXPECT_FALSE(R.inferred());
        EXPECT_FALSE(R.Plan);
        loopRefused(loopCheck(Bad, Bad, *Good.Plan), Status::Different);
      }
      LowIRLoopInferenceLimits Exact;
      Exact.Execution.MaxOperations = Good.Operations;
      Exact.Execution.MaxSolverQueries = Good.SolverQueries;
      Exact.MaxRankCandidates = Good.RankCandidates;
      Exact.MaxWideningRounds = Good.WideningRounds;
      const auto AtLimit =
          inferLowIRLoopRefinementPlan(P.Function, P.Contract, Exact, Cuts);
      ASSERT_TRUE(AtLimit.inferred()) << AtLimit.Diagnostic;
      for (unsigned Case = 0; Case != 4; ++Case) {
        auto Short = Exact;
        if (Case == 0)
          --Short.Execution.MaxOperations;
        else if (Case == 1)
          --Short.Execution.MaxSolverQueries;
        else if (Case == 2)
          --Short.MaxRankCandidates;
        else
          --Short.MaxWideningRounds;
        const auto R =
            inferLowIRLoopRefinementPlan(P.Function, P.Contract, Short, Cuts);
        EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::BudgetExceeded);
        EXPECT_FALSE(R.Plan);
      }
      const auto Proof = loopCheck(P, P, *Good.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
      LowIRRefinementLimits ShortProof;
      ShortProof.Execution.MaxSolverQueries = Proof.SolverQueries - 1;
      loopRefused(loopCheck(P, P, *Good.Plan, ShortProof),
                  Status::BudgetExceeded);
    }
}

TEST(LowIRLoopInference, RebuiltCounterLanesFitBoundedSearch) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (bool Cached : {false, true})
      for (auto Tag : {CounterTag::Prefix, CounterTag::Changing}) {
        SCOPED_TRACE(static_cast<unsigned>(Order));
        SCOPED_TRACE(Cached);
        SCOPED_TRACE(static_cast<unsigned>(Tag));
        auto P = projectedCounterLoops(4, true, 1, true,
                                       LowIRLoopSpace::Register, true, 0, true,
                                       false, Cached, false, false, Tag);
        P.Contract.ByteOrder = Order;
        for (uint64_t Offset : {8, 16, 64, 72, 96, 104})
          P.Contract.ReturnRegisters.push_back({Offset, 8});
        const std::vector<va_t> Cuts = Cached ? std::vector<va_t>{0x200, 0x400}
                                              : std::vector<va_t>{0x300, 0x500};
        // Concrete prefix lanes can coincidentally differ by one while other
        // bits change. Keep enough budget for the symbolic counter without
        // expanding those incidental matches into additional relations.
        LowIRLoopInferenceLimits Limits;
        const bool Changing = Tag == CounterTag::Changing;
        Limits.Execution.MaxSolverQueries =
            Cached ? (Changing ? 928 : 640) : (Changing ? 1856 : 1248);
        Limits.Execution.MaxOperations =
            Cached ? (Changing ? 672 : 624) : (Changing ? 848 : 768);
        const auto R =
            inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, Cuts);
        ASSERT_TRUE(R.inferred()) << R.Diagnostic;
        const auto Proof = loopCheck(P, P, *R.Plan);
        ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
      }
}

TEST(LowIRLoopInference, ZeroExtendedUpdatesUseTheirObservedCounterLanes) {
  for (uint16_t Bytes : {1, 3, 4})
    for (bool Ascending : {false, true}) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(Ascending);
      const auto P = projectedCounterLoops(Bytes, Ascending);
      const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {},
                                                  {0x300, 0x500});
      ASSERT_TRUE(R.inferred()) << R.Diagnostic;
      ASSERT_EQ(R.Plan->Cutpoints.size(), 2U);
      const auto Proof = loopCheck(P, P, *R.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    }
}

TEST(LowIRLoopInference, CounterProjectionsKeepStorageAndByteOrder) {
  for (auto Space : {LowIRLoopSpace::Register, LowIRLoopSpace::Frame,
                     LowIRLoopSpace::FunctionTemporary})
    for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      SCOPED_TRACE(static_cast<unsigned>(Space));
      SCOPED_TRACE(static_cast<unsigned>(Order));
      auto P = projectedCounterLoops(3, true, 1, true, Space);
      P.Contract.ByteOrder = Order;
      const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {},
                                                  {0x300, 0x500});
      ASSERT_TRUE(R.inferred()) << R.Diagnostic;
      const auto Proof = loopCheck(P, P, *R.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
      auto Wrong = P;
      for (auto &O : Wrong.Function.Blocks[4].Ops)
        if (O.Opcode == NdOp::INT_ADD && O.Output == r(0))
          O.Opcode = NdOp::INT_SUB;
      loopRefused(loopCheck(P, Wrong, *R.Plan), Status::Different);
      auto UpperBytes = P;
      for (auto &B : UpperBytes.Function.Blocks)
        for (auto &O : B.Ops)
          if (O.Opcode == NdOp::INT_ZEXT && O.Output == NdVar::tmp(32, 8))
            O.Opcode = NdOp::INT_SEXT;
      // A projected rank does not make the rest of the counter unobservable.
      loopRefused(loopCheck(P, UpperBytes, *R.Plan), Status::Different);
    }
}

TEST(LowIRLoopInference, ProjectedRanksRejectStutterWrapAndSkippedExits) {
  for (bool Ascending : {false, true}) {
    const auto P = projectedCounterLoops(1, Ascending);
    const auto Good = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {},
                                                   {0x300, 0x500});
    ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
    for (const auto &Bad : {projectedCounterLoops(1, Ascending, 0),
                            projectedCounterLoops(1, Ascending, 2),
                            projectedCounterLoops(1, Ascending, 1, false)}) {
      const auto Refused = inferLowIRLoopRefinementPlan(
          Bad.Function, Bad.Contract, {}, {0x300, 0x500});
      EXPECT_FALSE(Refused.inferred());
      EXPECT_FALSE(Refused.Plan);
      loopRefused(loopCheck(Bad, Bad, *Good.Plan), Status::Different);
    }
  }
}

TEST(LowIRLoopInference, ProjectedBoundsProveNarrowEqualityExits) {
  for (uint16_t Bytes : {1, 3, 4})
    for (auto Space : {LowIRLoopSpace::Register, LowIRLoopSpace::Frame,
                       LowIRLoopSpace::FunctionTemporary})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(static_cast<unsigned>(Space));
        SCOPED_TRACE(static_cast<unsigned>(Order));
        auto P = projectedCounterLoops(Bytes, true, 1, true, Space, true);
        P.Contract.ByteOrder = Order;
        const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {},
                                                    {0x300, 0x500});
        ASSERT_TRUE(R.inferred()) << R.Diagnostic;
        const auto Proof = loopCheck(P, P, *R.Plan);
        ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
      }
}

TEST(LowIRLoopInference, LateCounterLanesRefreshEqualityBounds) {
  for (uint16_t Bytes : {1, 3, 4})
    for (auto Space : {LowIRLoopSpace::Register, LowIRLoopSpace::Frame,
                       LowIRLoopSpace::FunctionTemporary})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big})
        for (bool PadBounds : {false, true}) {
          SCOPED_TRACE(Bytes);
          SCOPED_TRACE(static_cast<unsigned>(Space));
          SCOPED_TRACE(static_cast<unsigned>(Order));
          SCOPED_TRACE(PadBounds);
          auto P = projectedCounterLoops(Bytes, true, 1, true, Space, true, 0,
                                         PadBounds, true);
          P.Contract.ByteOrder = Order;
          const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract,
                                                      {}, {0x300, 0x500});
          ASSERT_TRUE(R.inferred()) << R.Diagnostic;
          const auto Proof = loopCheck(P, P, *R.Plan);
          ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
        }
}

TEST(LowIRLoopInference, ProjectedBoundsCannotHideChangedBoundsOrWraparound) {
  const auto Space = LowIRLoopSpace::Frame;
  const auto P = projectedCounterLoops(1, true, 1, true, Space, true);
  const std::vector<va_t> Cuts{0x300, 0x500};
  const auto Good =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  for (const auto &Bad :
       {projectedCounterLoops(1, true, 0, true, Space, true),
        projectedCounterLoops(1, true, 2, true, Space, true),
        projectedCounterLoops(1, true, 1, true, Space, true, 1),
        projectedCounterLoops(1, true, 1, false, Space, true)}) {
    const auto Refused =
        inferLowIRLoopRefinementPlan(Bad.Function, Bad.Contract, {}, Cuts);
    EXPECT_FALSE(Refused.inferred());
    EXPECT_FALSE(Refused.Plan);
    loopRefused(loopCheck(Bad, Bad, *Good.Plan), Status::Different);
  }
}

TEST(LowIRLoopInference, ProjectedBoundsKeepUnselectedInputObservations) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    auto P = projectedCounterLoops(3, true, 1, true, LowIRLoopSpace::Frame,
                                   true, 0, true);
    P.Contract.ByteOrder = Order;
    P.Contract.ReturnRegisters.push_back({64, 8});
    P.Contract.ReturnRegisters.push_back({72, 8});
    const auto Good = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {},
                                                   {0x300, 0x500});
    ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
    const auto Proof = loopCheck(P, P, *Good.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    auto Wrong = P;
    for (auto &O : Wrong.Function.Blocks.front().Ops)
      if (O.Opcode == NdOp::INT_AND && O.Inputs[0] == r(160))
        O.Inputs[1] = n(0);
    loopRefused(loopCheck(P, Wrong, *Good.Plan), Status::Different);
  }
}

TEST(LowIRLoopInference, ProjectedComparisonBitsProveNarrowEqualityHeaders) {
  for (uint16_t Bytes : {1, 3, 4})
    for (auto Space : {LowIRLoopSpace::Register, LowIRLoopSpace::Frame,
                       LowIRLoopSpace::FunctionTemporary})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big})
        for (bool ZeroInitialize : {false, true}) {
          SCOPED_TRACE(Bytes);
          SCOPED_TRACE(static_cast<unsigned>(Space));
          SCOPED_TRACE(static_cast<unsigned>(Order));
          SCOPED_TRACE(ZeroInitialize);
          auto P = projectedCounterLoops(Bytes, true, 1, true, Space, true, 0,
                                         true, ZeroInitialize, true);
          P.Contract.ByteOrder = Order;
          const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract,
                                                      {}, {0x200, 0x400});
          ASSERT_TRUE(R.inferred()) << R.Diagnostic;
          const auto Proof = loopCheck(P, P, *R.Plan);
          ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
        }
}

void complementCachedExit(Program &P) {
  for (auto &B : P.Function.Blocks)
    for (auto &O : B.Ops) {
      if (O.Opcode == NdOp::INT_LESSEQUAL &&
          (O.Output == r(176, 1) || O.Output == r(184, 1))) {
        O.Opcode = NdOp::INT_LESS;
        std::swap(O.Inputs[0], O.Inputs[1]);
      } else if (O.Opcode == NdOp::COPY && O.Output == NdVar::tmp(32, 1) &&
                 (O.Inputs[0] == r(176, 1) || O.Inputs[0] == r(184, 1))) {
        O.Opcode = NdOp::INT_EQUAL;
        O.NumInputs = 2;
        O.Inputs[1] = n(0, 1);
      }
    }
  // Rebind the newly constructed fixture after changing its Boolean encoding.
  for (auto &Record : P.Records) {
    const auto &B = *std::find_if(
        P.Function.Blocks.begin(), P.Function.Blocks.end(),
        [&](const auto &Block) { return Block.Id == Record.BlockId; });
    Record.Effects.OperationDigest =
        lowUndefinedOperationDigest(llvm::ArrayRef<LowOp>(B.Ops).slice(
            Record.Boundary.FirstOp, Record.Boundary.OpCount));
  }
}

TEST(LowIRLoopInference, OrderedComparisonBitsProveBothExitEncodings) {
  for (uint16_t Bytes : {1, 3, 4})
    for (auto Space : {LowIRLoopSpace::Register, LowIRLoopSpace::Frame,
                       LowIRLoopSpace::FunctionTemporary})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big})
        for (bool Complement : {false, true}) {
          SCOPED_TRACE(Bytes);
          SCOPED_TRACE(static_cast<unsigned>(Space));
          SCOPED_TRACE(static_cast<unsigned>(Order));
          SCOPED_TRACE(Complement);
          auto P = projectedCounterLoops(Bytes, true, 1, true, Space, true, 0,
                                         true, true, true, true);
          P.Contract.ByteOrder = Order;
          if (Complement)
            complementCachedExit(P);
          const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract,
                                                      {}, {0x200, 0x400});
          ASSERT_TRUE(R.inferred()) << R.Diagnostic;
          const auto Proof = loopCheck(P, P, *R.Plan);
          ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
        }
}

void checkComparisonNontermination(bool OrderedExit,
                                   bool MutableBounds = false) {
  const auto Space = LowIRLoopSpace::FunctionTemporary;
  const auto P = projectedCounterLoops(1, true, 1, true, Space, true, 0, true,
                                       true, true, OrderedExit, MutableBounds);
  const std::vector<va_t> Cuts{0x200, 0x400};
  const auto Good =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  unsigned Case = 0;
  for (const auto &Bad :
       {projectedCounterLoops(1, true, 0, true, Space, true, 0, true, true,
                              true, OrderedExit, MutableBounds),
        projectedCounterLoops(1, true, 2, true, Space, true, 0, true, true,
                              true, OrderedExit, MutableBounds),
        projectedCounterLoops(1, true, 1, false, Space, true, 0, true, true,
                              true, OrderedExit, MutableBounds)}) {
    SCOPED_TRACE(OrderedExit);
    SCOPED_TRACE(MutableBounds);
    SCOPED_TRACE(Case++);
    const auto Refused =
        inferLowIRLoopRefinementPlan(Bad.Function, Bad.Contract, {}, Cuts);
    EXPECT_FALSE(Refused.inferred());
    EXPECT_FALSE(Refused.Plan);
    loopRefused(loopCheck(Bad, Bad, *Good.Plan), Status::Different);
  }
  const auto Moving =
      projectedCounterLoops(1, true, 1, true, Space, true, 1, true, true, true,
                            OrderedExit, MutableBounds);
  if (!OrderedExit) {
    // Equal increments keep a nonzero difference and can skip equality forever.
    const auto Refused = inferLowIRLoopRefinementPlan(
        Moving.Function, Moving.Contract, {}, Cuts);
    EXPECT_FALSE(Refused.inferred());
    EXPECT_FALSE(Refused.Plan);
  }
  // An unsigned-order exit can terminate when the moving bound wraps. Either
  // way it cannot reuse a template proved for the unchanged lower bound.
  loopRefused(loopCheck(Moving, Moving, *Good.Plan), Status::Different);
}

TEST(LowIRLoopInference, ProjectedComparisonBitsRejectNonterminatingUpdates) {
  checkComparisonNontermination(false);
}

TEST(LowIRLoopInference, OrderedComparisonBitsRejectNonterminatingUpdates) {
  checkComparisonNontermination(true);
}

void checkComparisonObservations(bool OrderedExit, bool MutableBounds = false) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    auto P =
        projectedCounterLoops(3, true, 1, true, LowIRLoopSpace::Frame, true, 0,
                              true, true, true, OrderedExit, MutableBounds);
    P.Contract.ByteOrder = Order;
    P.Contract.ReturnRegisters.push_back({64, 8});
    P.Contract.ReturnRegisters.push_back({72, 8});
    const auto Good = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {},
                                                   {0x200, 0x400});
    ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
    const auto Proof = loopCheck(P, P, *Good.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    if (MutableBounds) {
      auto WrongUpperUpdate = P;
      for (auto &B : WrongUpperUpdate.Function.Blocks)
        for (auto &O : B.Ops)
          if (O.Opcode == NdOp::INT_ADD && O.Output == r(72))
            O.Inputs[1] = n(uint64_t{1} << 25);
      loopRefused(loopCheck(P, WrongUpperUpdate, *Good.Plan),
                  Status::Different);
    }
    auto UpperBoundBytes = P;
    for (auto &O : UpperBoundBytes.Function.Blocks.front().Ops)
      if (O.Opcode == NdOp::INT_AND && O.Inputs[0] == r(160))
        O.Inputs[1] = n(0);
    loopRefused(loopCheck(P, UpperBoundBytes, *Good.Plan), Status::Different);
    auto UpperCounterBytes = P;
    for (auto &B : UpperCounterBytes.Function.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::INT_ZEXT && O.Output == NdVar::tmp(32, 8))
          O.Opcode = NdOp::INT_SEXT;
    loopRefused(loopCheck(P, UpperCounterBytes, *Good.Plan), Status::Different);
    auto WrongCache = P;
    for (auto &B : WrongCache.Function.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == (OrderedExit ? NdOp::INT_LESSEQUAL : NdOp::INT_EQUAL) &&
            O.Output == r(184, 1))
          O.Opcode = OrderedExit ? NdOp::INT_LESS : NdOp::INT_NOTEQUAL;
    loopRefused(loopCheck(P, WrongCache, *Good.Plan), Status::Different);
  }
}

TEST(LowIRLoopInference, ProjectedComparisonBitsPreserveFullStateAndCache) {
  checkComparisonObservations(false);
}

TEST(LowIRLoopInference, OrderedComparisonBitsPreserveFullStateAndCache) {
  checkComparisonObservations(true);
}

TEST(LowIRLoopInference, LateCounterBoundsCannotRestoreRejectedRelations) {
  const auto Space = LowIRLoopSpace::FunctionTemporary;
  const auto P =
      projectedCounterLoops(1, true, 1, true, Space, true, 0, true, true);
  const std::vector<va_t> Cuts{0x300, 0x500};
  const auto Good =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  for (const auto &Bad :
       {projectedCounterLoops(1, true, 0, true, Space, true, 0, true, true),
        projectedCounterLoops(1, true, 2, true, Space, true, 0, true, true),
        projectedCounterLoops(1, true, 1, true, Space, true, 1, true, true),
        projectedCounterLoops(1, true, 1, false, Space, true, 0, true, true)}) {
    const auto Refused =
        inferLowIRLoopRefinementPlan(Bad.Function, Bad.Contract, {}, Cuts);
    EXPECT_FALSE(Refused.inferred());
    EXPECT_FALSE(Refused.Plan);
    loopRefused(loopCheck(Bad, Bad, *Good.Plan), Status::Different);
  }
}

TEST(LowIRLoopInference, LateCounterBoundsKeepFullStateObservations) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    auto P = projectedCounterLoops(3, true, 1, true, LowIRLoopSpace::Frame,
                                   true, 0, true, true);
    P.Contract.ByteOrder = Order;
    P.Contract.ReturnRegisters.push_back({64, 8});
    P.Contract.ReturnRegisters.push_back({72, 8});
    const auto Good = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {},
                                                   {0x300, 0x500});
    ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
    const auto Proof = loopCheck(P, P, *Good.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    auto Wrong = P;
    for (auto &O : Wrong.Function.Blocks.front().Ops)
      if (O.Opcode == NdOp::INT_AND && O.Inputs[0] == r(160))
        O.Inputs[1] = n(0);
    loopRefused(loopCheck(P, Wrong, *Good.Plan), Status::Different);
    auto UpperCounterBytes = P;
    for (auto &B : UpperCounterBytes.Function.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::INT_ZEXT && O.Output == NdVar::tmp(32, 8))
          O.Opcode = NdOp::INT_SEXT;
    loopRefused(loopCheck(P, UpperCounterBytes, *Good.Plan), Status::Different);
  }
}

void checkProjectedCounterBudgets(bool EqualityExit,
                                  bool ZeroInitialize = false,
                                  bool CachedExit = false,
                                  bool OrderedExit = false,
                                  bool MutableBounds = false) {
  const auto P = projectedCounterLoops(
      1, true, 1, true, LowIRLoopSpace::Register, EqualityExit, 0,
      CachedExit || MutableBounds, ZeroInitialize, CachedExit, OrderedExit,
      MutableBounds);
  const std::vector<va_t> Cuts = CachedExit ? std::vector<va_t>{0x200, 0x400}
                                            : std::vector<va_t>{0x300, 0x500};
  const auto Good =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  if (MutableBounds) {
    EXPECT_GT(Good.EntailmentCacheHits, 0U);
    EXPECT_LT(Good.EntailmentCacheHits, Good.SolverQueries);
  }
  LowIRLoopInferenceLimits Exact;
  Exact.Execution.MaxOperations = Good.Operations;
  Exact.Execution.MaxSolverQueries = Good.SolverQueries;
  Exact.Execution.MaxPaths = Good.ScheduledPaths;
  Exact.MaxRankCandidates = Good.RankCandidates;
  Exact.MaxWideningRounds = Good.WideningRounds;
  const auto AtLimit =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Exact, Cuts);
  ASSERT_TRUE(AtLimit.inferred()) << AtLimit.Diagnostic;
  EXPECT_EQ(AtLimit.SolverQueries, Good.SolverQueries);
  EXPECT_EQ(AtLimit.EntailmentCacheHits, Good.EntailmentCacheHits);
  for (unsigned Case = 0; Case != 5; ++Case) {
    auto Short = Exact;
    if (Case == 0)
      --Short.Execution.MaxOperations;
    else if (Case == 1)
      --Short.Execution.MaxSolverQueries;
    else if (Case == 2)
      --Short.Execution.MaxPaths;
    else if (Case == 3)
      --Short.MaxRankCandidates;
    else
      --Short.MaxWideningRounds;
    const auto Refused =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, Short, Cuts);
    EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded)
        << Refused.Diagnostic;
    EXPECT_FALSE(Refused.Plan);
    if (Case == 1) {
      EXPECT_EQ(Refused.SolverQueries, Short.Execution.MaxSolverQueries);
      if (MutableBounds)
        EXPECT_GT(Refused.EntailmentCacheHits, 0U);
    }
  }
  const auto Proof = loopCheck(P, P, *Good.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxOperations = Proof.Operations;
  Limits.Execution.MaxSolverQueries = Proof.SolverQueries;
  Limits.Execution.MaxObservations = Proof.Observations;
  ASSERT_TRUE(loopCheck(P, P, *Good.Plan, Limits).proved());
  for (unsigned Case = 0; Case != 3; ++Case) {
    auto Short = Limits;
    if (Case == 0)
      --Short.Execution.MaxOperations;
    else if (Case == 1)
      --Short.Execution.MaxSolverQueries;
    else
      --Short.Execution.MaxObservations;
    loopRefused(loopCheck(P, P, *Good.Plan, Short), Status::BudgetExceeded);
  }
}

TEST(LowIRLoopInference, ProjectedRanksKeepIndependentBudgets) {
  checkProjectedCounterBudgets(false);
}

TEST(LowIRLoopInference, ProjectedBoundsKeepIndependentBudgets) {
  checkProjectedCounterBudgets(true);
}

TEST(LowIRLoopInference, LateCounterBoundsKeepIndependentBudgets) {
  checkProjectedCounterBudgets(true, true);
}

TEST(LowIRLoopInference, ProjectedComparisonBitsKeepIndependentBudgets) {
  checkProjectedCounterBudgets(true, true, true);
}

TEST(LowIRLoopInference, OrderedComparisonBitsKeepIndependentBudgets) {
  checkProjectedCounterBudgets(true, true, true, true);
}

TEST(LowIRLoopInference, MutablePrefixBoundsKeepReferencedLanes) {
  for (uint16_t Bytes : {1, 3, 4})
    for (auto Space : {LowIRLoopSpace::Register, LowIRLoopSpace::Frame,
                       LowIRLoopSpace::FunctionTemporary})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big})
        for (bool Cached : {false, true}) {
          SCOPED_TRACE(Bytes);
          SCOPED_TRACE(static_cast<unsigned>(Space));
          SCOPED_TRACE(static_cast<unsigned>(Order));
          SCOPED_TRACE(Cached);
          auto P = projectedCounterLoops(Bytes, true, 1, true, Space, true, 0,
                                         true, true, Cached, false, true);
          P.Contract.ByteOrder = Order;
          const std::vector<va_t> Cuts = Cached
                                             ? std::vector<va_t>{0x200, 0x400}
                                             : std::vector<va_t>{0x300, 0x500};
          const auto R =
              inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
          ASSERT_TRUE(R.inferred()) << R.Diagnostic;
          const auto Proof = loopCheck(P, P, *R.Plan);
          ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
        }
}

TEST(LowIRLoopInference, MutablePrefixBoundsRejectNonterminatingUpdates) {
  for (bool Ordered : {false, true})
    checkComparisonNontermination(Ordered, true);
}

TEST(LowIRLoopInference, MutablePrefixBoundsProveTerminatingMovingOrder) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    auto P = projectedCounterLoops(1, true, 1, true,
                                   LowIRLoopSpace::FunctionTemporary, true, 1,
                                   true, true, true, true, true);
    P.Contract.ByteOrder = Order;
    const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract, {},
                                                {0x200, 0x400});
    ASSERT_TRUE(R.inferred()) << R.Diagnostic;
    // Counter and bound advance together. The unsigned order exit becomes
    // true at the bound's wrap, within 255 steps for every initial byte.
    const auto Proof = loopCheck(P, P, *R.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  }
}

TEST(LowIRLoopInference, MutablePrefixBoundsPreserveFullStateAndCache) {
  for (bool Ordered : {false, true})
    checkComparisonObservations(Ordered, true);
}

TEST(LowIRLoopInference, MutablePrefixBoundsKeepIndependentBudgets) {
  for (bool Cached : {false, true})
    checkProjectedCounterBudgets(true, true, Cached, false, true);
}

TEST(LowIRLoopInference, SharedTemplatesFitIndependentOperationBudgets) {
  auto P = projectedCounterLoops(3, true, 1, true, LowIRLoopSpace::Register,
                                 true, 0, true, true, false, false, true);
  for (uint64_t Offset : {8, 16, 64, 72})
    P.Contract.ReturnRegisters.push_back({Offset, 8});
  const std::vector<va_t> Cuts{0x300, 0x500};
  // Repeated projections and prefix reads must fit these bounds without
  // dropping predicates or narrowing the observed counters and bound words.
  LowIRLoopInferenceLimits Limits;
  Limits.Execution.MaxOperations = 1024;
  const auto Good =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, Cuts);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  LowIRRefinementLimits ProofLimits;
  ProofLimits.Execution.MaxOperations = 640;
  const auto Proof = loopCheck(P, P, *Good.Plan, ProofLimits);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;

  Limits.Execution.MaxOperations = Good.Operations - 1;
  const auto Short =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, Cuts);
  EXPECT_EQ(Short.Status, LowIRLoopInferenceStatus::BudgetExceeded);
  EXPECT_FALSE(Short.Plan);
  ProofLimits.Execution.MaxOperations = Proof.Operations - 1;
  loopRefused(loopCheck(P, P, *Good.Plan, ProofLimits), Status::BudgetExceeded);

  // Prefix inputs retain the derived value at this cut, including the XOR.
  // Reusing them must not equate that value with the raw entry input.
  auto Wrong = P;
  for (auto &O : Wrong.Function.Blocks.front().Ops)
    if (O.Opcode == NdOp::INT_XOR && O.Output == r(64))
      O.Inputs[1] = n(0);
  loopRefused(loopCheck(P, Wrong, *Good.Plan), Status::Different);
}

TEST(LowIRLoopInference, IncrementalEntailmentsKeepRollingBudgets) {
  bool SawCapacityRetry = false;
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SCOPED_TRACE(static_cast<unsigned>(Order));
    auto P = projectedCounterLoops(4, true, 1, true, LowIRLoopSpace::Register,
                                   true, 0, true, true, false, false, true);
    P.Contract.ByteOrder = Order;
    for (uint64_t Offset : {8, 16, 64, 72})
      P.Contract.ReturnRegisters.push_back({Offset, 8});
    const std::vector<va_t> Cuts{0x300, 0x500};
    const auto Default =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
    ASSERT_TRUE(Default.inferred()) << Default.Diagnostic;
    LowIRLoopInferenceLimits Limits;
    // Each complete query fits, while all encodings retained together do not.
    Limits.Execution.Solver.Blast.MaxGates = 1344;
    const auto Good =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, Cuts);
    ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
    EXPECT_GE(Good.SolverQueries, Default.SolverQueries);
    SawCapacityRetry |= Good.SolverQueries > Default.SolverQueries;
    const auto Proof = loopCheck(P, P, *Good.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    Limits.Execution.MaxSolverQueries = Good.SolverQueries;
    EXPECT_TRUE(
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, Cuts)
            .inferred());
    --Limits.Execution.MaxSolverQueries;
    const auto Short =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, Cuts);
    EXPECT_EQ(Short.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_FALSE(Short.Plan);
    EXPECT_EQ(Short.SolverQueries, Limits.Execution.MaxSolverQueries);
    for (unsigned Case = 0; Case != 3; ++Case) {
      LowIRLoopInferenceLimits Tiny;
      if (Case == 0)
        Tiny.Execution.Solver.Blast.MaxGates = 512;
      else if (Case == 1)
        Tiny.Execution.Solver.Blast.MaxWidth = 4;
      else
        Tiny.Execution.Solver.Sat.MaxPropagations = 1;
      const auto Refused =
          inferLowIRLoopRefinementPlan(P.Function, P.Contract, Tiny, Cuts);
      EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded)
          << Refused.Diagnostic;
      EXPECT_FALSE(Refused.Plan);
    }
    LowIRRefinementLimits TinyProof;
    TinyProof.Execution.MaxSolverQueries = 0;
    loopRefused(loopCheck(P, P, *Good.Plan, TinyProof), Status::BudgetExceeded);
    ASSERT_TRUE(loopCheck(P, P, *Good.Plan).proved());
  }
  EXPECT_TRUE(SawCapacityRetry);
}

TEST(LowIRLoopInference, CompletedEntailmentsStayInInferenceSession) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    const auto Make = [&](unsigned Step) {
      auto P = projectedCounterLoops(1, true, Step, true, LowIRLoopSpace::Frame,
                                     true, 0, true, true, true, false, true);
      P.Contract.ByteOrder = Order;
      return P;
    };
    const std::vector<va_t> Cuts{0x200, 0x400};
    const auto P = Make(1);
    const auto Good =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
    ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
    EXPECT_GT(Good.EntailmentCacheHits, 0U);
    ASSERT_TRUE(loopCheck(P, P, *Good.Plan).proved());
    for (unsigned Step : {0, 2}) {
      const auto Infinite = Make(Step);
      const auto Refused = inferLowIRLoopRefinementPlan(
          Infinite.Function, Infinite.Contract, {}, Cuts);
      EXPECT_FALSE(Refused.inferred());
      EXPECT_FALSE(Refused.Plan);
      loopRefused(loopCheck(Infinite, Infinite, *Good.Plan), Status::Different);
    }
    const auto Again =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
    ASSERT_TRUE(Again.inferred()) << Again.Diagnostic;
    EXPECT_EQ(Again.SolverQueries, Good.SolverQueries);
    EXPECT_EQ(Again.EntailmentCacheHits, Good.EntailmentCacheHits);
    ASSERT_TRUE(loopCheck(P, P, *Again.Plan).proved());
  }
}

TEST(LowIRLoopInference, CompletedEntailmentsKeepUnknownAndNodeLimits) {
  const auto P =
      projectedCounterLoops(1, true, 1, true, LowIRLoopSpace::Register, true, 0,
                            true, true, true, false, true);
  const std::vector<va_t> Cuts{0x200, 0x400};
  const auto Good =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  EXPECT_GT(Good.EntailmentCacheHits, 0U);
  // A previous successful run must not bypass a new session's query, solver
  // or symbolic-node limits. An incomplete answer must never become a fact.
  for (unsigned Case = 0; Case != 3; ++Case) {
    LowIRLoopInferenceLimits Tiny;
    if (Case == 0)
      Tiny.Execution.MaxSolverQueries = 0;
    else if (Case == 1)
      Tiny.Execution.Solver.Blast.MaxGates = 1;
    else
      Tiny.Execution.MaxSymbolicNodes = 1;
    const auto Refused =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, Tiny, Cuts);
    EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded)
        << Refused.Diagnostic;
    EXPECT_FALSE(Refused.Plan);
    EXPECT_EQ(Refused.EntailmentCacheHits, 0U);
  }
  LowIRRefinementLimits TinyProof;
  TinyProof.Execution.MaxSolverQueries = 0;
  loopRefused(loopCheck(P, P, *Good.Plan, TinyProof), Status::BudgetExceeded);
  ASSERT_TRUE(loopCheck(P, P, *Good.Plan).proved());
}

struct CachedComparisonOptions {
  unsigned Bit = 0;
  uint16_t Bytes = 1;
  bool Invert = false;
  bool GuardEntry = false;
  bool FoldInitialization = false;
  bool FrameCache = false;
};

Program deeperWordLoops(unsigned Depth, bool Ascending, unsigned Step = 1,
                        bool EqualityExit = false, bool CachedExit = false,
                        unsigned BoundStep = 0, bool ResetCounter = false,
                        bool CachedComparison = false,
                        CachedComparisonOptions Comparison = {}) {
  Program P;
  const auto Address = [](unsigned Id) { return n(0x100 + 0x100 * Id); };
  const auto Counter = [](unsigned Level) { return r(8 + 8 * Level); };
  const auto Bound = [](unsigned Level) { return r(64 + 8 * Level); };
  const auto Cache = [](unsigned Level) { return r(128 + 8 * Level); };
  const auto ExitFlag = [&](unsigned Level) {
    return r(128 + 8 * Level, Comparison.Bytes);
  };
  if (Comparison.FrameCache) {
    P.Contract.Frame = LowIRIndependenceFrame{{32, 8}, -9, 1};
    P.Contract.EntryConstants.push_back({r(32), 0x10000});
  }
  const auto FrameOffset = [](unsigned Level) {
    return n(static_cast<uint64_t>(Level ? 0 : -9));
  };
  const auto CacheComparison = [&](unsigned Level, bool Initialize = false) {
    const bool Fold = Initialize && Comparison.FoldInitialization;
    P.instruction(
        {Fold ? op(NdOp::COPY, NdVar::tmp(0, 1), {n(Comparison.Invert, 1)})
              : op(Comparison.Invert ? NdOp::INT_NOTEQUAL : NdOp::INT_EQUAL,
                   NdVar::tmp(0, 1), {Counter(Level), Bound(Level)}),
         op(Comparison.Bytes == 1 ? NdOp::COPY : NdOp::INT_ZEXT,
            ExitFlag(Level), {NdVar::tmp(0, 1)}),
         op(NdOp::INT_LEFT, ExitFlag(Level),
            {ExitFlag(Level), n(Comparison.Bit, Comparison.Bytes)})});
    if (Comparison.FrameCache)
      P.instruction(
          {op(NdOp::INT_ADD, NdVar::tmp(0, 8), {r(32), FrameOffset(Level)}),
           op(NdOp::STORE, {}, {NdVar::tmp(0, 8), ExitFlag(Level)})});
  };
  const unsigned Exit = 1 + 3 * Depth;
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}),
                 op(NdOp::COPY, Counter(0), {Ascending ? n(0) : Bound(0)})});
  if (Comparison.GuardEntry) {
    const unsigned Initialize = Exit + 1;
    P.Function.Blocks[0].Succs = {static_cast<int>(Initialize),
                                  static_cast<int>(Exit)};
    P.instruction(
        {op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {Bound(0), n(0)}),
         op(NdOp::INT_EQUAL, NdVar::tmp(8, 1), {Bound(Depth - 1), n(0)}),
         op(NdOp::BOOL_OR, NdVar::tmp(16, 1),
            {NdVar::tmp(0, 1), NdVar::tmp(8, 1)}),
         op(NdOp::COND_BR, {}, {Address(Exit), NdVar::tmp(16, 1)})});
    P.block(Initialize, Address(Initialize).Offset, {1});
  }
  if (CachedComparison)
    CacheComparison(0, true);
  else if (CachedExit)
    P.instruction({op(NdOp::COPY, Cache(0), {Counter(0)})});
  P.instruction({op(NdOp::BRANCH, {}, {Address(1)})});
  for (unsigned Level = 0; Level != Depth; ++Level) {
    const unsigned Header = 1 + 3 * Level, Body = Header + 1, Tail = Header + 2;
    const unsigned Done = Level ? 3 * Level : Exit;
    const auto Compared = CachedExit ? Cache(Level) : Counter(Level);
    P.block(Header, Address(Header).Offset,
            {static_cast<int>(Body), static_cast<int>(Done)});
    if (CachedComparison && Comparison.FrameCache)
      P.instruction(
          {op(NdOp::INT_ADD, NdVar::tmp(0, 8), {r(32), FrameOffset(Level)}),
           op(NdOp::LOAD, ExitFlag(Level), {NdVar::tmp(0, 8)})});
    if (CachedComparison)
      P.instruction(
          {op(NdOp::INT_RIGHT, NdVar::tmp(0, Comparison.Bytes),
              {ExitFlag(Level), n(Comparison.Bit, Comparison.Bytes)}),
           op(NdOp::INT_AND, NdVar::tmp(0, Comparison.Bytes),
              {NdVar::tmp(0, Comparison.Bytes), n(1, Comparison.Bytes)}),
           op(Comparison.Invert ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL,
              NdVar::tmp(8, 1),
              {NdVar::tmp(0, Comparison.Bytes), n(0, Comparison.Bytes)}),
           op(NdOp::COND_BR, {}, {Address(Done), NdVar::tmp(8, 1)})});
    else
      P.instruction({op(Ascending && !EqualityExit ? NdOp::INT_LESSEQUAL
                                                   : NdOp::INT_EQUAL,
                        NdVar::tmp(0, 1),
                        {Ascending ? Bound(Level) : Compared,
                         Ascending ? Compared : n(0)}),
                     op(NdOp::COND_BR, {}, {Address(Done), NdVar::tmp(0, 1)})});
    const unsigned Next = Level + 1 == Depth ? Tail : Header + 3;
    P.block(Body, Address(Body).Offset, {static_cast<int>(Next)});
    if (Level + 1 == Depth)
      P.instruction({op(NdOp::INT_ADD, r(0), {r(0), Counter(Level)}),
                     op(NdOp::BRANCH, {}, {Address(Next)})});
    else {
      P.instruction({op(NdOp::COPY, Counter(Level + 1),
                        {Ascending ? n(0) : Bound(Level + 1)})});
      if (CachedComparison)
        CacheComparison(Level + 1, true);
      else if (CachedExit)
        P.instruction({op(NdOp::COPY, Cache(Level + 1), {Counter(Level + 1)})});
      P.instruction({op(NdOp::BRANCH, {}, {Address(Next)})});
    }
    P.block(Tail, Address(Tail).Offset, {static_cast<int>(Header)});
    if (ResetCounter)
      P.instruction({op(NdOp::COPY, Counter(Level), {n(0)})});
    else
      P.instruction({op(Ascending ? NdOp::INT_ADD : NdOp::INT_SUB,
                        Counter(Level), {Counter(Level), n(Step)})});
    if (BoundStep)
      P.instruction(
          {op(NdOp::INT_ADD, Bound(Level), {Bound(Level), n(BoundStep)})});
    if (CachedComparison)
      CacheComparison(Level);
    else if (CachedExit)
      P.instruction({op(NdOp::COPY, Cache(Level), {Counter(Level)})});
    P.instruction({op(NdOp::BRANCH, {}, {Address(Header)})});
  }
  P.block(Exit, Address(Exit).Offset);
  P.finish();
  return P;
}

TEST(LowIRLoopInference, ThreeLevelsAndOppositeCounterDirections) {
  for (bool Ascending : {false, true}) {
    const auto P = deeperWordLoops(3, Ascending);
    const auto Inferred = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
    ASSERT_TRUE(Inferred.inferred())
        << "ascending=" << Ascending << ": " << Inferred.Diagnostic;
    const auto Proof = loopCheck(P, P, *Inferred.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    EXPECT_GE(Inferred.Plan->Cutpoints.front().Rank.size(), 3U);
  }
}

TEST(LowIRLoopInference, EqualityExitCountersNeedInductiveInputBounds) {
  for (unsigned Depth : {2, 3}) {
    const auto P = deeperWordLoops(Depth, true, 1, true);
    const auto Inferred = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
    ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
    const auto Good = loopCheck(P, P, *Inferred.Plan);
    ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
    EXPECT_GE(Good.RankingChecks, Depth);
    for (unsigned Step : {0, 2}) {
      const auto Bad = deeperWordLoops(Depth, true, Step, true);
      const auto Search =
          inferLowIRLoopRefinementPlan(Bad.Function, Bad.Contract);
      EXPECT_FALSE(Search.inferred());
      EXPECT_FALSE(Search.Plan);
      loopRefused(loopCheck(Bad, Bad, *Inferred.Plan), Status::Different);
    }
  }
}

TEST(LowIRLoopInference, CachedEqualityExitsNeedCorrelatedOperandCopies) {
  for (unsigned Depth : {2, 3}) {
    const auto P = deeperWordLoops(Depth, true, 1, true, true);
    const auto Inferred = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
    ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
    const auto Proof = loopCheck(P, P, *Inferred.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    for (unsigned Kind = 0; Kind != 4; ++Kind) {
      const auto Bad = deeperWordLoops(Depth, true, Kind == 0 ? 0 : 1, true,
                                       true, Kind == 1 ? 1 : 0, Kind == 2);
      if (Kind == 3) {
        auto Wrong = P;
        for (auto &B : Wrong.Function.Blocks)
          for (auto &O : B.Ops)
            if (O.Opcode == NdOp::COPY && O.Output == r(128))
              O.Inputs[0] = n(0);
        loopRefused(loopCheck(P, Wrong, *Inferred.Plan), Status::Different);
        continue;
      }
      const auto Refused =
          inferLowIRLoopRefinementPlan(Bad.Function, Bad.Contract);
      EXPECT_FALSE(Refused.inferred()) << Refused.Diagnostic;
      EXPECT_FALSE(Refused.Plan);
      loopRefused(loopCheck(Bad, Bad, *Inferred.Plan), Status::Different);
    }
  }
}

TEST(LowIRLoopInference, CachedComparisonsNeedCounterBitRelations) {
  const auto P = deeperWordLoops(2, true, 1, true, false, 0, false, true);
  const std::vector<va_t> Headers{0x200, 0x500};
  const auto Inferred =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Headers);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  const auto Proof = loopCheck(P, P, *Inferred.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  auto Wrong = P;
  for (auto &B : Wrong.Function.Blocks)
    for (auto &O : B.Ops)
      if (B.Id == 6 && O.Opcode == NdOp::INT_EQUAL)
        O.Opcode = NdOp::INT_NOTEQUAL;
  loopRefused(loopCheck(P, Wrong, *Inferred.Plan), Status::Different);
  const auto Infinite =
      deeperWordLoops(2, true, 1, true, false, 1, false, true);
  const auto Refused = inferLowIRLoopRefinementPlan(
      Infinite.Function, Infinite.Contract, {}, Headers);
  EXPECT_FALSE(Refused.inferred());
  EXPECT_FALSE(Refused.Plan);
  for (bool Nodes : {false, true}) {
    LowIRLoopInferenceLimits Limits;
    if (Nodes)
      Limits.Execution.MaxSymbolicNodes = Inferred.PredicateNodes - 1;
    else
      Limits.Execution.MaxSolverQueries = Inferred.SolverQueries - 1;
    const auto Exhausted =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits, Headers);
    EXPECT_EQ(Exhausted.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_FALSE(Exhausted.Plan);
  }
}

TEST(LowIRLoopInference, GuardedComparisonCachesAndPackedHighBits) {
  const std::vector<va_t> Headers{0x200, 0x500};
  for (const auto Options :
       {CachedComparisonOptions{7, 1, false, true},
        CachedComparisonOptions{7, 1, false, true, true},
        CachedComparisonOptions{7, 1, false, true, true, true},
        CachedComparisonOptions{31, 4, true, true, true},
        CachedComparisonOptions{31, 4, true, false},
        CachedComparisonOptions{63, 8, false, false}}) {
    const auto P =
        deeperWordLoops(2, true, 1, true, false, 0, false, true, Options);
    const auto Inferred =
        inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Headers);
    ASSERT_TRUE(Inferred.inferred())
        << "bit=" << Options.Bit << ": " << Inferred.Diagnostic;
    ASSERT_TRUE(loopCheck(P, P, *Inferred.Plan).proved());
    auto Wrong = P;
    for (auto &B : Wrong.Function.Blocks)
      for (auto &O : B.Ops)
        if (B.Id == 6 && O.Opcode == NdOp::INT_LEFT)
          O.Inputs[1] = n(Options.Bit - 1, Options.Bytes);
    loopRefused(loopCheck(P, Wrong, *Inferred.Plan), Status::Different);
    auto Neighbor = P;
    for (auto &B : Neighbor.Function.Blocks)
      for (auto &O : B.Ops)
        if (B.Id == 6 && O.Opcode == NdOp::INT_LEFT) {
          O.Opcode = NdOp::INT_MULT;
          O.Inputs[1] = n((uint64_t{1} << Options.Bit) |
                              (uint64_t{1} << (Options.Bit - 1)),
                          Options.Bytes);
        }
    loopRefused(loopCheck(P, Neighbor, *Inferred.Plan), Status::Different);
    for (unsigned Kind = 0; Kind != 3; ++Kind) {
      const auto Bad =
          deeperWordLoops(2, true, Kind == 0 ? 0 : 1, true, false,
                          Kind == 1 ? 1 : 0, Kind == 2, true, Options);
      loopRefused(loopCheck(Bad, Bad, *Inferred.Plan), Status::Different);
    }
  }
}

TEST(LowIRLoopInference, GuardedOperandCopiesFindDelayedOuterCounters) {
  CachedComparisonOptions Options;
  Options.GuardEntry = true;
  const auto P =
      deeperWordLoops(2, true, 1, true, true, 0, false, false, Options);
  const std::vector<va_t> Headers{0x200, 0x500};
  const auto Inferred =
      inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Headers);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  const auto Proof = loopCheck(P, P, *Inferred.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  const auto Infinite =
      deeperWordLoops(2, true, 0, true, true, 0, false, false, Options);
  loopRefused(loopCheck(Infinite, Infinite, *Inferred.Plan), Status::Different);
}

TEST(LowIRLoopRefinement, UnreachableNestedPrefixCannotSeedInduction) {
  auto A = wordLoop();
  A.Contract.EntryConstants.push_back({r(16), 0});
  auto Plan = wordLoopPlan();
  auto Body = Plan.Cutpoints[0];
  Body.OriginalAddress = Body.CandidateAddress = 0x300;
  Body.UseEntryPrefix = true;
  Plan.Cutpoints.push_back(Body);
  for (bool Generalize : {false, true}) {
    Plan.Cutpoints.back().GeneralizeEntryPrefix = Generalize;
    const auto Proof = loopCheck(A, A, Plan);
    loopRefused(Proof, Status::Unsupported);
    EXPECT_NE(Proof.Diagnostic.find("reachable entry prefix"),
              std::string::npos);
  }
}

TEST(LowIRLoopInference, NestedInfinitePathsAndSharedBudgetsRefuse) {
  const auto Good = deeperWordLoops(2, false);
  const auto Inferred =
      inferLowIRLoopRefinementPlan(Good.Function, Good.Contract);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  for (unsigned Step : {0, 2}) {
    const auto Bad = deeperWordLoops(2, false, Step);
    const auto Refused =
        inferLowIRLoopRefinementPlan(Bad.Function, Bad.Contract);
    EXPECT_FALSE(Refused.inferred());
    EXPECT_FALSE(Refused.Plan);
    loopRefused(loopCheck(Bad, Bad, *Inferred.Plan), Status::Different);
  }
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    LowIRLoopInferenceLimits Limits;
    if (Kind == 0)
      Limits.Execution.MaxPaths = Inferred.ScheduledPaths - 1;
    if (Kind == 1)
      Limits.Execution.MaxSolverQueries = Inferred.SolverQueries - 1;
    if (Kind == 2)
      Limits.MaxWideningRounds = Inferred.WideningRounds - 1;
    if (Kind == 3)
      Limits.MaxRankCandidates = Inferred.RankCandidates - 1;
    const auto Refused =
        inferLowIRLoopRefinementPlan(Good.Function, Good.Contract, Limits);
    EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded)
        << Refused.Diagnostic;
    EXPECT_FALSE(Refused.Plan);
  }
  const auto Proof = loopCheck(Good, Good, *Inferred.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxInstructions = Proof.Instructions - 1;
  loopRefused(loopCheck(Good, Good, *Inferred.Plan, Limits),
              Status::BudgetExceeded);
}

TEST(LowIRLoopRefinement, DisjointPrefixDomainsCannotBePaired) {
  const auto A = wordLoop();
  auto B = A;
  B.Function.Blocks[1].Ops[0].Opcode = NdOp::INT_NOTEQUAL;
  auto Plan = wordLoopPlan();
  auto Body = Plan.Cutpoints[0];
  Body.OriginalAddress = Body.CandidateAddress = 0x300;
  Body.UseEntryPrefix = true;
  Plan.Cutpoints.push_back(Body);
  for (bool Generalize : {false, true}) {
    Plan.Cutpoints.back().GeneralizeEntryPrefix = Generalize;
    const auto Proof = loopCheck(A, B, Plan);
    loopRefused(Proof, Status::Unsupported);
    EXPECT_NE(Proof.Diagnostic.find("reachable entry prefix"),
              std::string::npos);
  }
}

TEST(LowIRLoopRefinement, SelectedUndefinedBitsRemainCorrelatedAcrossSpills) {
  auto A = wordLoop(), B = wordLoop(1, true);
  // Each iteration selects the lifted low accumulator bit. It is copied to a
  // frame slot, whose invariant shares that value with the register carrier.
  A.frame();
  auto Spill = [](Program &P) {
    auto &Body = P.Function.Blocks[2];
    auto &Boundary = Body.InstructionBoundaries[0];
    auto O = op(NdOp::STORE, {}, {r(32), r(0)});
    O.Addr = Boundary.Address;
    O.Seq = Boundary.OpCount;
    Body.Ops.insert(Body.Ops.begin() + Boundary.OpCount, O);
    ++Boundary.OpCount;
    ++Body.InstructionBoundaries[1].FirstOp;
    P.Records[2].Boundary = Boundary;
    P.Records[2].Effects.OpCount = Boundary.OpCount;
    P.Records[2].Effects.OperationDigest = lowUndefinedOperationDigest(
        llvm::ArrayRef<LowOp>(Body.Ops).take_front(Boundary.OpCount));
    P.Records[3].Boundary = Body.InstructionBoundaries[1];
  };
  Spill(A);
  Spill(B);
  A.Records[2].Effects.Effects.push_back({1, r(0), 0, 1, {}});
  auto Plan = wordLoopPlan();
  const LowIRLoopLocation Slot{LowIRLoopSpace::Frame, 0, 8};
  // The first visit predates a spill, so overapproximate the old slot as a
  // shared independent parameter. Every iteration must still compare it.
  Plan.Cutpoints[0].Inputs.push_back(
      {LowIRLoopSide::Original, Slot, NdVar::tmp(16, 8)});
  Plan.Cutpoints[0].OriginalState.push_back({Slot, NdVar::tmp(16, 8)});
  Plan.Cutpoints[0].CandidateState.push_back({Slot, NdVar::tmp(16, 8)});
  for (bool Generalize : {false, true}) {
    Plan.Cutpoints[0].UseEntryPrefix = true;
    Plan.Cutpoints[0].GeneralizeEntryPrefix = Generalize;
    const auto R = loopCheck(A, B, Plan);
    ASSERT_TRUE(R.proved()) << R.Diagnostic;
    EXPECT_FALSE(R.Certificate->Producers.empty());
    const auto WrongWitness = checkLowIRLoopRefinement(
        A.Function, A.Records, B.Function, A.Contract, Plan, Witness::ZeroBits);
    loopRefused(WrongWitness, Status::Different);
  }
  Plan.Cutpoints[0].CandidateState.back().Value = n(0);
  loopRefused(loopCheck(A, B, Plan), Status::Different);
}

TEST(LowIRLoopRefinement, OverlappingEntryViewsRemainSharedAtLaterCuts) {
  auto A = wordLoop(), B = wordLoop();
  // This additional byte is first read by the loop template, after the real
  // entry has been snapshotted. A later full-width observation must alias it.
  A.Contract.ReturnRegisters.push_back({40, 8});
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].Inputs.push_back(
      {LowIRLoopSide::Entry, regLocation(41, 1), NdVar::tmp(16, 1)});
  Plan.Cutpoints[0].OriginalState.push_back(
      {regLocation(41, 1), NdVar::tmp(16, 1)});
  Plan.Cutpoints[0].CandidateState.push_back(
      {regLocation(41, 1), NdVar::tmp(16, 1)});
  const auto Good = loopCheck(A, B, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  Plan.Cutpoints[0].CandidateState.back().Location.Offset = 42;
  loopRefused(loopCheck(A, B, Plan), Status::Different);
}

TEST(LowIRLoopInference, ProjectedCounterCopiesPreserveCompleteState) {
  unsigned Cases = 0;
  for (uint16_t Bytes : {1, 3, 4})
    for (auto Space : {LowIRLoopSpace::Register, LowIRLoopSpace::Frame,
                       LowIRLoopSpace::FunctionTemporary})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big})
        for (bool Cached : {false, true})
          for (auto Tag : {CounterTag::Prefix, CounterTag::Changing})
            for (auto Copy : {CounterCopy::Lane, CounterCopy::Word}) {
              SCOPED_TRACE(Bytes);
              SCOPED_TRACE(static_cast<unsigned>(Space));
              SCOPED_TRACE(static_cast<unsigned>(Order));
              SCOPED_TRACE(Cached);
              SCOPED_TRACE(static_cast<unsigned>(Tag));
              SCOPED_TRACE(static_cast<unsigned>(Copy));
              auto P = projectedCounterLoops(Bytes, true, 1, true, Space, true,
                                             0, true, false, Cached, false,
                                             false, Tag, Copy);
              P.Contract.ByteOrder = Order;
              for (uint64_t Offset : {8, 16, 64, 72, 96, 104, 200})
                P.Contract.ReturnRegisters.push_back({Offset, 8});
              const std::vector<va_t> Cuts =
                  Cached ? std::vector<va_t>{0x200, 0x400}
                         : std::vector<va_t>{0x300, 0x500};
              const auto R = inferLowIRLoopRefinementPlan(P.Function,
                                                          P.Contract, {}, Cuts);
              ASSERT_TRUE(R.inferred()) << R.Diagnostic;
              const auto Proof = loopCheck(P, P, *R.Plan);
              ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
              // The copied lane may agree while its observed high tag differs.
              // A complete-state proof must retain those independent bits.
              if (Copy == CounterCopy::Lane) {
                auto Wrong = P;
                bool Changed = false;
                for (auto &B : Wrong.Function.Blocks)
                  for (auto &O : B.Ops)
                    if (O.Opcode == NdOp::INT_OR && O.Output == r(200)) {
                      O.Inputs[1] = n(0);
                      Changed = true;
                    }
                ASSERT_TRUE(Changed);
                loopRefused(loopCheck(P, Wrong, *R.Plan), Status::Different);
              }
              ++Cases;
            }
  EXPECT_EQ(Cases, 144u);
}

TEST(LowIRLoopInference, ProjectedCounterCopiesKeepProofBoundaries) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (bool Cached : {false, true}) {
      const auto Make = [&](unsigned Step, bool Guarded) {
        auto P = projectedCounterLoops(
            1, true, Step, Guarded, LowIRLoopSpace::Frame, true, 0, true, false,
            Cached, false, false, CounterTag::Changing, CounterCopy::Lane);
        P.Contract.ByteOrder = Order;
        for (uint64_t Offset : {8, 16, 64, 72, 96, 104, 200})
          P.Contract.ReturnRegisters.push_back({Offset, 8});
        return P;
      };
      const auto P = Make(1, true);
      const std::vector<va_t> Cuts = Cached ? std::vector<va_t>{0x200, 0x400}
                                            : std::vector<va_t>{0x300, 0x500};
      const auto Good =
          inferLowIRLoopRefinementPlan(P.Function, P.Contract, {}, Cuts);
      ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
      for (const auto &Bad : {Make(0, true), Make(2, true), Make(1, false)}) {
        const auto R =
            inferLowIRLoopRefinementPlan(Bad.Function, Bad.Contract, {}, Cuts);
        EXPECT_FALSE(R.inferred());
        EXPECT_FALSE(R.Plan);
        loopRefused(loopCheck(Bad, Bad, *Good.Plan), Status::Different);
      }
      LowIRLoopInferenceLimits Exact;
      Exact.Execution.MaxOperations = Good.Operations;
      Exact.Execution.MaxSolverQueries = Good.SolverQueries;
      Exact.MaxRankCandidates = Good.RankCandidates;
      Exact.MaxWideningRounds = Good.WideningRounds;
      ASSERT_TRUE(
          inferLowIRLoopRefinementPlan(P.Function, P.Contract, Exact, Cuts)
              .inferred());
      for (unsigned Case = 0; Case != 4; ++Case) {
        auto Short = Exact;
        if (Case == 0)
          --Short.Execution.MaxOperations;
        else if (Case == 1)
          --Short.Execution.MaxSolverQueries;
        else if (Case == 2)
          --Short.MaxRankCandidates;
        else
          --Short.MaxWideningRounds;
        const auto R =
            inferLowIRLoopRefinementPlan(P.Function, P.Contract, Short, Cuts);
        EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::BudgetExceeded);
        EXPECT_FALSE(R.Plan);
      }
      const auto Proof = loopCheck(P, P, *Good.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
      LowIRRefinementLimits Short;
      Short.Execution.MaxSolverQueries = Proof.SolverQueries - 1;
      loopRefused(loopCheck(P, P, *Good.Plan, Short), Status::BudgetExceeded);
    }
}

Program relocatedCounterLoops(bool Relocate, uint16_t Bytes, bool Tagged,
                              LowIRLoopSpace Space, unsigned Step = 1,
                              bool Guarded = true, bool Ascending = false) {
  Program P;
  const auto Mask = Bytes == 8 ? UINT64_MAX : (uint64_t{1} << (8 * Bytes)) - 1;
  P.Contract.ReturnRegisters.push_back({8, 8});
  P.Contract.ReturnRegisters.push_back({200, 8});
  P.Contract.ReturnRegisters.push_back({24, 8});

  if (Space == LowIRLoopSpace::Frame)
    P.Contract.Frame = LowIRIndependenceFrame{{40, 8}, -16, 0};
  if (Space == LowIRLoopSpace::FunctionTemporary)
    P.Function.FunctionTemporaries = {{256, 8}, {264, 8}};
  const auto Carrier = [&](NdVar V) -> int {
    if (V == r(8))
      return 0;
    if (V == r(200))
      return 1;
    return -1;
  };
  const auto Emit = [&](std::initializer_list<LowOp> Ops) {
    std::vector<LowOp> Built;
    uint64_t Next = 512;
    const auto Temp = [&] {
      const auto V = NdVar::tmp(Next, 8);
      Next += 8;
      return V;
    };
    const auto Address = [&](int Index) {
      const auto V = Temp();
      Built.push_back(
          op(NdOp::INT_ADD, V, {r(40), n(uint64_t(-16 + 8 * Index))}));
      return V;
    };
    for (auto O : Ops) {
      for (unsigned I = 0; I != O.NumInputs; ++I) {
        const int Index = Carrier(O.Inputs[I]);
        if (Index < 0 || Space == LowIRLoopSpace::Register)
          continue;
        if (Space == LowIRLoopSpace::FunctionTemporary) {
          O.Inputs[I] = NdVar::tmp(256 + 8 * Index, 8);
        } else {
          const auto A = Address(Index), V = Temp();
          Built.push_back(op(NdOp::LOAD, V, {A}));
          O.Inputs[I] = V;
        }
      }
      const int Index = Carrier(O.Output);
      if (Index < 0 || Space == LowIRLoopSpace::Register) {
        Built.push_back(O);
      } else if (Space == LowIRLoopSpace::FunctionTemporary) {
        O.Output = NdVar::tmp(256 + 8 * Index, 8);
        Built.push_back(O);
      } else {
        O.Output = Temp();
        Built.push_back(O);
        const auto A = Address(Index);
        Built.push_back(op(NdOp::STORE, {}, {A, O.Output}));
      }
    }
    P.instruction(Built);
  };
  P.Function.Blocks[0].Succs = {1};
  Emit({op(NdOp::COPY, r(0), {n(0)}),
        Ascending ? op(NdOp::COPY, r(8), {n(0)})
                  : op(NdOp::INT_AND, r(8), {r(16), n(Mask)}),
        op(NdOp::INT_AND, NdVar::tmp(80, 8), {Tagged ? r(96) : n(0), n(~Mask)}),
        op(NdOp::INT_OR, r(8), {r(8), NdVar::tmp(80, 8)}),
        op(NdOp::COPY, r(200), {n(0)}), op(NdOp::COPY, r(24), {n(0)}),
        op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 6});
  Emit({op(NdOp::INT_AND, NdVar::tmp(64, 8), {r(8), n(Mask)}),
        op(NdOp::INT_AND, NdVar::tmp(88, 8),
           {Ascending ? r(16) : n(0), n(Mask)}),
        Guarded ? op(NdOp::INT_EQUAL, NdVar::tmp(0, 1),
                     {NdVar::tmp(64, 8), NdVar::tmp(88, 8)})
                : op(NdOp::COPY, NdVar::tmp(0, 1), {n(0, 1)}),
        op(NdOp::COND_BR, {}, {n(0x700), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {3, 5});
  if (Relocate)
    Emit({op(NdOp::INT_AND, NdVar::tmp(72, 8), {r(8), n(Mask)}),
          op(NdOp::INT_AND, NdVar::tmp(80, 8),
             {Tagged ? r(104) : n(0), n(~Mask)}),
          op(NdOp::INT_OR, r(200), {NdVar::tmp(72, 8), NdVar::tmp(80, 8)}),
          op(NdOp::COPY, r(8), {n(0)})});
  Emit({op(NdOp::COPY, r(24), {r(32)}),
        op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(24), n(0)}),
        op(NdOp::COND_BR, {}, {n(0x600), NdVar::tmp(0, 1)})});
  P.block(3, 0x400, {4, 5});
  Emit({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(24), n(0)}),
        op(NdOp::COND_BR, {}, {n(0x600), NdVar::tmp(0, 1)})});
  P.block(4, 0x500, {3});
  Emit({op(NdOp::INT_ADD, r(0), {r(0), r(24)}),
        op(NdOp::INT_SUB, r(24), {r(24), n(1)}),
        op(NdOp::BRANCH, {}, {n(0x400)})});
  P.block(5, 0x600, {1});
  const auto Count = Relocate ? r(200) : r(8);
  Emit({op(Ascending ? NdOp::INT_ADD : NdOp::INT_SUB, NdVar::tmp(64, 8),
           {Count, n(Step)}),
        op(NdOp::INT_AND, NdVar::tmp(72, 8), {NdVar::tmp(64, 8), n(Mask)}),
        op(NdOp::INT_AND, NdVar::tmp(80, 8), {Tagged ? r(96) : n(0), n(~Mask)}),
        op(NdOp::INT_OR, r(8), {NdVar::tmp(72, 8), NdVar::tmp(80, 8)}),
        op(NdOp::COPY, r(200), {n(0)}), op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(6, 0x700);
  P.finish();
  return P;
}

struct TransferredCounterCase {
  LowIRLoopSpace Space;
  bool Ascending;
  bool Relocate;

  std::string name() const {
    const char *Storage = Space == LowIRLoopSpace::Register ? "Register"
                          : Space == LowIRLoopSpace::Frame  ? "Frame"
                                                            : "Temporary";
    return std::string(Storage) + (Ascending ? "Ascending" : "Descending") +
           (Relocate ? "Relocated" : "InPlace");
  }
};

void PrintTo(const TransferredCounterCase &Case, std::ostream *Out) {
  *Out << Case.name();
}

std::vector<TransferredCounterCase> transferredCounterCases() {
  std::vector<TransferredCounterCase> Cases;
  for (auto Space : {LowIRLoopSpace::Register, LowIRLoopSpace::Frame,
                     LowIRLoopSpace::FunctionTemporary})
    for (bool Ascending : {false, true})
      for (bool Relocate : {false, true})
        Cases.push_back({Space, Ascending, Relocate});
  return Cases;
}

class LowIRTransferredCounterState
    : public ::testing::TestWithParam<TransferredCounterCase> {};

TEST_P(LowIRTransferredCounterState, PreservesCompleteState) {
  const auto [Space, Ascending, Relocate] = GetParam();
  unsigned Cases = 0, WrongOperations = 0, WrongTags = 0;
  for (uint16_t Bytes : {1, 3, 4, 8})
    for (bool Tagged : {false, true})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
        SCOPED_TRACE(static_cast<unsigned>(Space));
        SCOPED_TRACE(Ascending);
        SCOPED_TRACE(Relocate);
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Tagged);
        SCOPED_TRACE(static_cast<unsigned>(Order));
        auto P = relocatedCounterLoops(Relocate, Bytes, Tagged, Space, 1, true,
                                       Ascending);
        P.Contract.ByteOrder = Order;
        LowIRLoopInferenceLimits Limits;
        Limits.MaxCutSelectionWork = 0;
        const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract,
                                                    Limits, {0x200, 0x400});
        ASSERT_TRUE(R.inferred()) << R.Diagnostic;
        EXPECT_EQ(R.CutSelectionWork, 0u);
        ASSERT_TRUE(loopCheck(P, P, *R.Plan).proved());
        if (Relocate) {
          auto Wrong = P;
          Wrong.Function.Blocks[4].Ops[0].Opcode = NdOp::INT_SUB;
          ++WrongOperations;
          loopRefused(loopCheck(P, Wrong, *R.Plan), Status::Different);
          if (Tagged && Bytes != 8) {
            Wrong = P;
            bool Changed = false;
            for (auto &B : Wrong.Function.Blocks)
              for (auto &O : B.Ops)
                if (O.Opcode == NdOp::INT_AND && O.Inputs[0] == r(104)) {
                  O.Inputs[0] = n(0);
                  Changed = true;
                }
            ASSERT_TRUE(Changed);
            ++WrongTags;
            loopRefused(loopCheck(P, Wrong, *R.Plan), Status::Different);
          }
        }
        ++Cases;
      }
  EXPECT_EQ(Cases, 16u);
  EXPECT_EQ(WrongOperations, Relocate ? 16u : 0u);
  EXPECT_EQ(WrongTags, Relocate ? 6u : 0u);
}

INSTANTIATE_TEST_SUITE_P(
    AllStorageAndTransferModes, LowIRTransferredCounterState,
    ::testing::ValuesIn(transferredCounterCases()),
    [](const ::testing::TestParamInfo<LowIRTransferredCounterState::ParamType>
           &Info) { return Info.param.name(); });

TEST(LowIRLoopInference, TransferredCountersKeepProofBoundaries) {
  for (auto Space : {LowIRLoopSpace::Register, LowIRLoopSpace::Frame,
                     LowIRLoopSpace::FunctionTemporary})
    for (bool Ascending : {false, true})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
        SCOPED_TRACE(static_cast<unsigned>(Space));
        SCOPED_TRACE(Ascending);
        SCOPED_TRACE(static_cast<unsigned>(Order));
        const auto Make = [&](unsigned Step, bool Guarded) {
          auto P = relocatedCounterLoops(true, 1, true, Space, Step, Guarded,
                                         Ascending);
          P.Contract.ByteOrder = Order;
          return P;
        };
        const auto P = Make(1, true);
        LowIRLoopInferenceLimits Limits;
        Limits.MaxCutSelectionWork = 0;
        const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract,
                                                    Limits, {0x200, 0x400});
        ASSERT_TRUE(R.inferred()) << R.Diagnostic;
        const auto Proof = loopCheck(P, P, *R.Plan);
        ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
        for (const auto &Bad : {Make(0, true), Make(2, true), Make(1, false)}) {
          const auto Refused = inferLowIRLoopRefinementPlan(
              Bad.Function, Bad.Contract, Limits, {0x200, 0x400});
          EXPECT_FALSE(Refused.inferred());
          EXPECT_FALSE(Refused.Plan);
          loopRefused(loopCheck(Bad, Bad, *R.Plan), Status::Different);
        }
        auto WrongRank = *R.Plan;
        for (auto &V : WrongRank.Cutpoints[1].Rank)
          V = n(0, V.Size);
        loopRefused(loopCheck(P, P, WrongRank), Status::Different);
        Limits.Execution.MaxOperations = R.Operations;
        Limits.Execution.MaxSolverQueries = R.SolverQueries;
        Limits.MaxRankCandidates = R.RankCandidates;
        Limits.MaxWideningRounds = R.WideningRounds;
        ASSERT_TRUE(inferLowIRLoopRefinementPlan(P.Function, P.Contract, Limits,
                                                 {0x200, 0x400})
                        .inferred());
        for (unsigned Kind = 0; Kind != 5; ++Kind) {
          auto Short = Limits;
          if (Kind == 0)
            --Short.Execution.MaxOperations;
          if (Kind == 1)
            --Short.Execution.MaxSolverQueries;
          if (Kind == 2)
            --Short.MaxRankCandidates;
          if (Kind == 3)
            --Short.MaxWideningRounds;
          if (Kind == 4)
            Short.Execution.MaxSymbolicNodes = 0;
          const auto Refused = inferLowIRLoopRefinementPlan(
              P.Function, P.Contract, Short, {0x200, 0x400});
          EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded);
          EXPECT_FALSE(Refused.Plan);
        }
        LowIRRefinementLimits Exact;
        Exact.Execution.MaxOperations = Proof.Operations;
        Exact.Execution.MaxSolverQueries = Proof.SolverQueries;
        Exact.Execution.MaxObservations = Proof.Observations;
        ASSERT_TRUE(loopCheck(P, P, *R.Plan, Exact).proved());
        for (unsigned Kind = 0; Kind != 3; ++Kind) {
          auto Short = Exact;
          if (Kind == 0)
            --Short.Execution.MaxOperations;
          if (Kind == 1)
            --Short.Execution.MaxSolverQueries;
          if (Kind == 2)
            --Short.Execution.MaxObservations;
          loopRefused(loopCheck(P, P, *R.Plan, Short), Status::BudgetExceeded);
        }
      }
}
Program conditionalProducts(bool Candidate, unsigned Change = 0) {
  Program P;
  P.Contract.ReturnRegisters = {{0, 8}, {32, 8}};
  P.Function.Blocks[0].Succs = {1, 2};
  P.instruction({op(Change == 2 ? NdOp::INT_NOTEQUAL : NdOp::INT_EQUAL,
                    NdVar::tmp(0, 1), {r(8), r(16)}),
                 op(NdOp::COND_BR, {}, {n(0x300), NdVar::tmp(0, 1)})});
  P.block(1, 0x200);
  P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(32), {n(0)})});
  P.finish();
  P.block(2, 0x300);
  auto Input = r(Candidate ? 16 : 8);
  P.instruction({op(NdOp::INT_MULT, r(0), {Input, Input}),
                 op(NdOp::INT_MULT, r(32), {Input, r(40)})});
  if (Change == 1)
    P.instruction({op(NdOp::INT_XOR, r(32), {r(32), n(uint64_t{1} << 63)})});
  P.finish();
  return P;
}

TEST(LowIRRefinement, ConditionalProductsKeepEveryObservationAndDomain) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (auto Choice : {Witness::LiftedBits, Witness::ZeroBits}) {
      auto A = conditionalProducts(false), B = conditionalProducts(true);
      A.Contract.ByteOrder = Order;
      LowIRRefinementLimits Limits;
      Limits.Execution.Solver.Blast.MaxGates = 128;
      const auto Good = A.check(B, Choice, Limits);
      ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
      EXPECT_EQ(Good.OriginalPaths, 2U);
      EXPECT_EQ(Good.CandidatePaths, 2U);
      EXPECT_EQ(Good.TerminalPairs, 4U);
      EXPECT_EQ(Good.SolverQueries, 14U);
      Limits.Execution.MaxSolverQueries = Good.SolverQueries;
      ASSERT_TRUE(A.check(B, Choice, Limits).proved());
      --Limits.Execution.MaxSolverQueries;
      expect(A, B, Status::BudgetExceeded, Choice, Limits);
      Limits.Execution.MaxSolverQueries = Good.SolverQueries;
      Limits.MaxTerminalPairs = Good.TerminalPairs - 1;
      expect(A, B, Status::BudgetExceeded, Choice, Limits);
      Limits = {};
      Limits.Execution.Solver.Blast.MaxGates = 128;
      for (unsigned Change : {1U, 2U}) {
        // Keep an earlier true observation, then change a high output bit or
        // give the candidate an unrelated branch domain.
        const auto Wrong =
            A.check(conditionalProducts(true, Change), Choice, Limits);
        EXPECT_FALSE(Wrong.proved());
        EXPECT_FALSE(Wrong.Certificate);
        EXPECT_EQ(Wrong.Status, Status::BudgetExceeded);
      }
      Limits.Execution.Solver.Blast.MaxWidth = 32;
      expect(A, B, Status::BudgetExceeded, Choice, Limits);
    }
}
} // namespace
