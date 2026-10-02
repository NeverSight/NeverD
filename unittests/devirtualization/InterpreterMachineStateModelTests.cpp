//===- InterpreterMachineStateModelTests.cpp - State model relations
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterMachineState.h"
#include "neverd/analysis/LowIRRefinement.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRRefinementStatus;
NdVar r(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar t(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::tmp(Offset, Bytes);
}
NdVar n(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp op(NdOp Code, NdVar Output = {},
         std::initializer_list<NdVar> Inputs = {}) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (NdVar V : Inputs)
    O.addInput(V);
  return O;
}

struct Program {
  LowFunc Function;
  Program() {
    Function.Entry = 0x100;
    block(0, 0x100);
  }
  void block(int Id, va_t Address, std::vector<int> Successors = {}) {
    LowBlock B;
    B.Id = Id;
    B.StartAddr = B.EndAddr = Address;
    B.Succs = std::move(Successors);
    Function.Blocks.push_back(std::move(B));
  }
  void instruction(std::initializer_list<LowOp> Ops) {
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
  }
  void finish(NdVar StatusValue = n(0)) {
    instruction({op(NdOp::RETURN, {}, {StatusValue})});
  }
  void edges() {
    for (auto &B : Function.Blocks)
      B.Preds.clear();
    for (const auto &B : Function.Blocks)
      for (int Id : B.Succs)
        for (auto &Dest : Function.Blocks)
          if (Dest.Id == Id)
            Dest.Preds.push_back(B.Id);
  }
};

LowIRIndependenceContract contract(bool CanonicalEntry = true) {
  LowIRIndependenceContract C;
  for (unsigned I = 0; I != 17; ++I)
    C.ReturnRegisters.push_back({I * 8, 8});
  if (CanonicalEntry)
    C.EntryConstants.push_back({r(128), 2});
  return C;
}

void expect(const InterpreterMachineStateModel &Model, const LowFunc &Candidate,
            const LowIRIndependenceContract &C,
            Status Expected = Status::Proved) {
  const auto Result =
      checkLowIRRefinement(Model.Function, Model.Instructions, Candidate, C,
                           LowIRRefinementWitness::LiftedBits);
  EXPECT_EQ(Result.Status, Expected) << Result.Diagnostic;
  EXPECT_EQ(Result.Certificate.has_value(), Expected == Status::Proved);
}

TEST(InterpreterMachineStateModel, RawEntryAndStatusAreDistinctFromGuestRAX) {
  Program Residual, Reference;
  Residual.finish(r(0));
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  // Independently specify the whole raw flag domain, including invalid entry.
  Reference.instruction(
      {op(NdOp::INT_AND, t(0), {r(128), n(~uint64_t{0x204ed7})}),
       op(NdOp::INT_NOTEQUAL, t(8, 1), {t(0), n(0)}),
       op(NdOp::INT_AND, t(16), {r(128), n(2)}),
       op(NdOp::INT_EQUAL, t(24, 1), {t(16), n(0)}),
       op(NdOp::BOOL_OR, t(32, 1), {t(8, 1), t(24, 1)}),
       op(NdOp::INT_ZEXT, r(256), {t(32, 1)})});
  Reference.finish(r(256));
  expect(*Model, Reference.Function, contract(false));
  Reference.Function.Blocks.back().Ops.back().Inputs[0] = r(0);
  expect(*Model, Reference.Function, contract(false), Status::Different);
}

TEST(InterpreterMachineStateModel, AlignmentGuardPreservesRejectedState) {
  Program Residual, Reference;
  Residual.instruction({op(NdOp::COPY, r(0), {n(37)})});
  Residual.finish();
  auto Model = modelInterpreterMachineStateX64(
      Residual.Function, InterpreterMachineStateProfile::UserX64NoFaultV1,
      65536, InterpreterEntryAlignment{16, 3});
  ASSERT_TRUE(bool(Model)) << llvm::toString(Model.takeError());
  uint64_t Operations = 0;
  for (const auto &B : Model->Function.Blocks)
    Operations += B.Ops.size();
  auto Exact = modelInterpreterMachineStateX64(
      Residual.Function, InterpreterMachineStateProfile::UserX64NoFaultV1,
      Operations, InterpreterEntryAlignment{16, 3});
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  auto Short = modelInterpreterMachineStateX64(
      Residual.Function, InterpreterMachineStateProfile::UserX64NoFaultV1,
      Operations - 1, InterpreterEntryAlignment{16, 3});
  ASSERT_FALSE(bool(Short));
  llvm::consumeError(Short.takeError());
  Reference.Function.Blocks.front().Succs = {2, 1};
  Reference.instruction({op(NdOp::INT_AND, t(0), {r(32), n(15)}),
                         op(NdOp::INT_NOTEQUAL, t(8, 1), {t(0), n(3)}),
                         op(NdOp::COND_BR, {}, {n(0x300), t(8, 1)})});
  Reference.block(1, 0x200);
  Reference.instruction({op(NdOp::COPY, r(0), {n(37)})});
  Reference.finish(n(0));
  Reference.block(2, 0x300);
  Reference.finish(n(2));
  Reference.edges();
  expect(*Model, Reference.Function, contract());
  Reference.Function.Blocks.back().Ops.back().Inputs[0] = n(0);
  expect(*Model, Reference.Function, contract(), Status::Different);
}

TEST(InterpreterMachineStateModel, EveryStateWordIsAnObservableOutput) {
  Program Residual;
  Residual.finish();
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  for (unsigned I = 0; I != 17; ++I) {
    SCOPED_TRACE(I);
    Program Wrong;
    Wrong.instruction({op(NdOp::INT_XOR, r(I * 8), {r(I * 8), n(1)})});
    Wrong.finish();
    expect(*Model, Wrong.Function, contract(), Status::Different);
  }
}

TEST(InterpreterMachineStateModel, PartialWritesAndReadsKeepNativeLaneRules) {
  Program Residual, Reference;
  Residual.instruction({op(NdOp::INT_XOR, r(0, 4), {r(8, 4), r(16, 4)})});
  Residual.instruction({op(NdOp::COPY, r(1, 1), {r(25, 1)})});
  Residual.instruction({op(NdOp::COPY, r(48, 2), {r(0, 2)})});
  Residual.finish();
  Reference.instruction({op(NdOp::INT_XOR, t(0, 4), {r(8, 4), r(16, 4)}),
                         op(NdOp::INT_ZEXT, r(0), {t(0, 4)})});
  Reference.instruction({op(NdOp::COPY, r(1, 1), {r(25, 1)})});
  Reference.instruction({op(NdOp::COPY, r(48, 2), {r(0, 2)})});
  Reference.finish();
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  expect(*Model, Reference.Function, contract());
  Reference.Function.Blocks[0].Ops[1] = op(NdOp::COPY, r(0, 4), {t(0, 4)});
  Reference.Function.Blocks[0].Ops[1].Addr = 0x100;
  Reference.Function.Blocks[0].Ops[1].Seq = 1;
  expect(*Model, Reference.Function, contract(), Status::Different);
}

TEST(InterpreterMachineStateModel, SameRegisterSliceDoesNotZeroUpperWord) {
  Program Residual, Reference;
  Residual.instruction({op(NdOp::SUBBYTES, r(0, 4), {r(0), n(0, 4)})});
  Residual.finish();
  Reference.finish();
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  expect(*Model, Reference.Function, contract());
}

TEST(InterpreterMachineStateModel, ScalarFlagsCommitToThePackedStateWord) {
  const uint64_t Registers[] = {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                                x86reg::SF, x86reg::DF, x86reg::OF};
  const unsigned Bits[] = {0, 2, 4, 6, 7, 10, 11};
  for (unsigned I = 0; I != 7; ++I) {
    SCOPED_TRACE(I);
    Program Residual, Reference;
    Residual.instruction(
        {op(NdOp::INT_NOTEQUAL, r(Registers[I], 1), {r(8), n(0)})});
    Residual.finish();
    Reference.instruction({op(NdOp::INT_NOTEQUAL, t(0, 1), {r(8), n(0)}),
                           op(NdOp::INT_ZEXT, t(8), {t(0, 1)}),
                           op(NdOp::INT_LEFT, t(16), {t(8), n(Bits[I])}),
                           op(NdOp::INT_OR, r(128), {n(2), t(16)})});
    Reference.finish();
    auto Model = modelInterpreterMachineStateX64(Residual.Function);
    ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
    expect(*Model, Reference.Function, contract());
  }
}

TEST(InterpreterMachineStateModel, RejectedDynamicFlagWriteStaysSticky) {
  Program Residual, Reference;
  Residual.instruction(
      {op(NdOp::INTRINSIC, {},
          {n(static_cast<uint64_t>(Intrinsic::Popf), 2), r(8)})});
  Residual.instruction(
      {op(NdOp::INTRINSIC, {},
          {n(static_cast<uint64_t>(Intrinsic::Popf), 2), n(2)})});
  Residual.finish();
  Reference.instruction({op(NdOp::INT_AND, t(0), {r(8), n(0x40100)}),
                         op(NdOp::INT_NOTEQUAL, t(8, 1), {t(0), n(0)}),
                         op(NdOp::INT_ZEXT, r(256), {t(8, 1)})});
  Reference.finish(r(256));
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  expect(*Model, Reference.Function, contract());
  Reference.Function.Blocks.back().Ops.back().Inputs[0] = n(0);
  expect(*Model, Reference.Function, contract(), Status::Different);
}

TEST(InterpreterMachineStateModel,
     GuestFrameRemainsMemoryAndReturnSlotIsPreserved) {
  Program Residual, Reference;
  for (Program *P : {&Residual, &Reference}) {
    P->instruction({op(NdOp::INT_SUB, t(0), {r(32), n(8)}),
                    op(NdOp::STORE, {}, {t(0), r(8)}),
                    op(NdOp::LOAD, r(0), {t(0)})});
    P->finish();
  }
  auto C = contract();
  C.Frame = LowIRIndependenceFrame{{32, 8}, -16, 8};
  C.PreservedRegisters = {{32, 8}};
  C.PreservedFrameRanges = {{0, 8}};
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  expect(*Model, Reference.Function, C);
  Reference.Function.Blocks[0].Ops[1].Inputs[1] = n(0);
  Reference.Function.Blocks[0].Ops[2] = op(NdOp::COPY, r(0), {r(8)});
  Reference.Function.Blocks[0].Ops[2].Addr = 0x100;
  Reference.Function.Blocks[0].Ops[2].Seq = 2;
  expect(*Model, Reference.Function, C, Status::Different);
}

TEST(InterpreterMachineStateModel, BothBranchArmsCommitTheirOwnState) {
  Program Residual, Reference;
  for (Program *P : {&Residual, &Reference}) {
    P->Function.Blocks[0].Succs = {1, 2};
    P->instruction({op(NdOp::INT_EQUAL, t(0, 1), {r(8), n(0)}),
                    op(NdOp::COND_BR, {}, {n(0x300), t(0, 1)})});
    P->block(1, 0x200);
    P->instruction({op(NdOp::INT_ADD, r(0), {r(0), r(16)})});
    P->finish();
    P->block(2, 0x300);
    P->instruction({op(NdOp::INT_XOR, r(0), {r(0), r(24)})});
    P->finish();
    P->edges();
  }
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  expect(*Model, Reference.Function, contract());
  Reference.Function.Blocks[2].Ops[0].Opcode = NdOp::INT_OR;
  expect(*Model, Reference.Function, contract(), Status::Different);
}

TEST(InterpreterMachineStateModel, CyclicModelRequiresAFreshInductiveProof) {
  Program Residual;
  Residual.Function.Blocks[0].Succs = {1};
  Residual.instruction({op(NdOp::BRANCH, {}, {n(0x200)})});
  Residual.block(1, 0x200, {2, 3});
  Residual.instruction({op(NdOp::INT_EQUAL, t(0, 1), {r(8), n(0)}),
                        op(NdOp::COND_BR, {}, {n(0x400), t(0, 1)})});
  Residual.block(2, 0x300, {1});
  Residual.instruction({op(NdOp::INT_SUB, r(8), {r(8), n(1)}),
                        op(NdOp::BRANCH, {}, {n(0x200)})});
  Residual.block(3, 0x400);
  Residual.finish();
  Residual.edges();
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  const auto C = contract();
  const auto Inferred = inferLowIRLoopRefinementPlan(Model->Function, C);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  const auto Checked = checkLowIRLoopRefinement(
      Model->Function, Model->Instructions, Model->Function, C, *Inferred.Plan,
      LowIRRefinementWitness::LiftedBits);
  EXPECT_TRUE(Checked.proved()) << Checked.Diagnostic;
}

TEST(InterpreterMachineStateModel,
     GenerationRefusesUnsupportedOrMalformedInput) {
  Program Good;
  Good.finish();
  auto BadProfile = modelInterpreterMachineStateX64(
      Good.Function, static_cast<InterpreterMachineStateProfile>(255));
  EXPECT_FALSE(static_cast<bool>(BadProfile));
  llvm::consumeError(BadProfile.takeError());
  auto Bad = Good.Function;
  Bad.Blocks[0].InstructionBoundaries.clear();
  auto Missing = modelInterpreterMachineStateX64(Bad);
  EXPECT_FALSE(static_cast<bool>(Missing));
  llvm::consumeError(Missing.takeError());
  Program Unsupported;
  Unsupported.instruction({op(NdOp::COPY, r(x86reg::XMM0), {n(0)})});
  Unsupported.finish();
  auto SIMD = modelInterpreterMachineStateX64(Unsupported.Function);
  EXPECT_FALSE(static_cast<bool>(SIMD));
  llvm::consumeError(SIMD.takeError());
}

TEST(InterpreterMachineStateModel, InputAndGeneratedOperationBudgetsBothApply) {
  Program Residual;
  Residual.finish();
  for (uint64_t Limit : {0U, 3U}) {
    auto Limited = modelInterpreterMachineStateX64(
        Residual.Function, InterpreterMachineStateProfile::UserX64NoFaultV1,
        Limit);
    EXPECT_FALSE(static_cast<bool>(Limited));
    llvm::consumeError(Limited.takeError());
  }
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  uint64_t Count = Model->Function.Blocks[0].Ops.size();
  auto Exact = modelInterpreterMachineStateX64(
      Residual.Function, InterpreterMachineStateProfile::UserX64NoFaultV1,
      Count);
  ASSERT_TRUE(static_cast<bool>(Exact)) << llvm::toString(Exact.takeError());
}

TEST(InterpreterMachineStateModel,
     InstructionRecordsCannotBeReusedAfterMutation) {
  Program Residual;
  Residual.instruction({op(NdOp::INT_ADD, r(0), {r(8), n(1)})});
  Residual.finish();
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(static_cast<bool>(Model)) << llvm::toString(Model.takeError());
  const auto Original = Model->Function;
  Model->Function.Blocks.back().Ops.back().Inputs[0] = n(0);
  expect(*Model, Original, contract(), Status::Invalid);
}

TEST(InterpreterMachineStateModel, UnboundedAncillaryMetadataIsNotCopied) {
  Program Residual;
  Residual.finish();
  Residual.Function.Name = std::string(100000, 'n');
  Residual.Function.DebugName = std::string(100000, 'd');
  Residual.Function.SourceFile = std::string(100000, 's');
  Residual.Function.UnsupportedInstructionAddresses.resize(100000);
  Residual.Function.RelocatedInstructionAddressOccurrences.resize(100000);
  Residual.Function.ModuleAnalysisRoots = {Residual.Function.Entry};
  auto Model = modelInterpreterMachineStateX64(Residual.Function);
  ASSERT_TRUE(bool(Model)) << llvm::toString(Model.takeError());
  EXPECT_TRUE(Model->Function.Name.empty());
  EXPECT_TRUE(Model->Function.DebugName.empty());
  EXPECT_TRUE(Model->Function.SourceFile.empty());
  EXPECT_TRUE(Model->Function.UnsupportedInstructionAddresses.empty());
  EXPECT_TRUE(Model->Function.RelocatedInstructionAddressOccurrences.empty());
  EXPECT_EQ(Model->Function.ModuleAnalysisRoots,
            Residual.Function.ModuleAnalysisRoots);
  for (va_t I = 0; I != 1024; ++I)
    Residual.Function.OrdinaryModuleAnalysisRoots.insert(I);
  auto Limited = modelInterpreterMachineStateX64(
      Residual.Function, InterpreterMachineStateProfile::UserX64NoFaultV1, 512);
  ASSERT_FALSE(bool(Limited));
  EXPECT_NE(llvm::toString(Limited.takeError()).find("budget"),
            std::string::npos);
}

TEST(InterpreterMachineStateModel, MissingPredecessorsCannotHideEntryBackedge) {
  for (bool DirectBranch : {false, true}) {
    Program Residual;
    Residual.instruction({op(NdOp::BRANCH, {}, {n(0x200)})});
    Residual.block(1, 0x200,
                   DirectBranch ? std::vector<int>{} : std::vector<int>{0});
    if (DirectBranch)
      Residual.instruction({op(NdOp::BRANCH, {}, {n(0x100)})});
    Residual.finish();
    auto Model = modelInterpreterMachineStateX64(Residual.Function);
    ASSERT_FALSE(bool(Model));
    EXPECT_NE(llvm::toString(Model.takeError()).find("backedge"),
              std::string::npos);
    auto Source = wrapInterpreterMachineStateX64(Residual.Function);
    ASSERT_FALSE(bool(Source));
    EXPECT_NE(llvm::toString(Source.takeError()).find("backedge"),
              std::string::npos);
  }
}
} // namespace
