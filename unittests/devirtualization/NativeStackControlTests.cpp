//===- NativeStackControlTests.cpp - Shared physical control contract -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/arch/x86_64/NativeStackControl.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/symbolic/SymExec.h"

#include <limits>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

namespace {

const NdVar Stack = NdVar::reg(32, 8);
const NdVar Scratch = NdVar::tmp(128, 8);

LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (const auto &Input : Inputs)
    Result.addInput(Input);
  return Result;
}

SpecializationInstruction instruction(std::vector<LowOp> Ops,
                                      SpecializationNativeStackControl Kind) {
  SpecializationInstruction Result;
  Result.Ops = std::move(Ops);
  Result.Origin.Address = 0x100;
  Result.Origin.Size = 5;
  Result.Origin.OpCount = Result.Ops.size();
  Result.Fallthrough.Address = 0x105;
  Result.NativeStackControl = Kind;
  for (size_t I = 0; I < Result.Ops.size(); ++I) {
    auto &Op = Result.Ops[I];
    Op.Addr = Result.Origin.Address;
    Op.Seq = static_cast<int>(I);
  }
  if (Kind == SpecializationNativeStackControl::Call) {
    Result.IsNativeCall = true;
    Result.Origin.Control = LowInstructionControl::Call;
    Result.Origin.ControlFlags = LowInstructionControlFlag::Call;
    if (Result.Ops.back().Opcode == NdOp::INDIR_CALL)
      Result.Origin.ControlFlags |= LowInstructionControlFlag::Indirect;
    else
      Result.Origin.Immediate = Result.Ops.back().Inputs[0].Offset;
  } else if (Kind == SpecializationNativeStackControl::Return) {
    Result.Origin.Control = LowInstructionControl::Return;
    Result.Origin.ControlFlags = LowInstructionControlFlag::Return;
  }
  Result.UndefinedEffects.Coverage = LowUndefinedCoverage::Complete;
  Result.UndefinedEffects.OpCount = Result.Ops.size();
  Result.UndefinedEffects.OperationDigest =
      lowUndefinedOperationDigest(Result.Ops);
  return Result;
}

SpecializationInstruction directCall() {
  return instruction({op(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x200, 8)})},
                     SpecializationNativeStackControl::Call);
}

SpecializationInstruction memoryCall() {
  return instruction(
      {op(NdOp::INT_SUB, NdVar::tmp(0, 8), {Stack, NdVar::scalar(8, 8)}),
       op(NdOp::LOAD, NdVar::tmp(8, 8), {NdVar::tmp(0, 8)}),
       op(NdOp::INDIR_CALL, NdVar::reg(0, 8), {NdVar::tmp(8, 8)})},
      SpecializationNativeStackControl::Call);
}

SpecializationInstruction returnInstruction() {
  return instruction({op(NdOp::RETURN, {}, {NdVar::reg(0, 8)})},
                     SpecializationNativeStackControl::Return);
}

auto expand(const SpecializationInstruction &Insn) {
  return expandNativeStackControl(Insn, Stack, Scratch,
                                  NativeReturnExpansion::InternalTransfer);
}

void reject(const SpecializationInstruction &Insn) {
  auto Result = expand(Insn);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_FALSE(llvm::toString(Result.takeError()).empty());
}

void validBoundary(const NativeStackExpansion &Expansion) {
  LowBlock Block;
  Block.StartAddr = Expansion.Boundary.Address;
  Block.EndAddr = Block.StartAddr + Expansion.Boundary.Size;
  Block.Ops = Expansion.Ops;
  Block.InstructionBoundaries.push_back(Expansion.Boundary);
  auto Error = validateLowInstructionBoundaries(
      Block, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  EXPECT_EQ(Expansion.UndefinedEffects.OpCount, Expansion.Ops.size());
  EXPECT_EQ(Expansion.UndefinedEffects.OperationDigest,
            lowUndefinedOperationDigest(Expansion.Ops));
}

uint64_t word(SymContext &Ctx, SymRef Value) {
  const auto Concrete = Ctx.asConst(Value);
  EXPECT_TRUE(Concrete.has_value());
  return Concrete ? Concrete->getZExtValue() : 0;
}

TEST(NativeStackControl, DirectCallRebindsOnlyTheTransformedEvidence) {
  const auto Insn = directCall();
  const auto OriginalDigest = Insn.UndefinedEffects.OperationDigest;
  auto Result = expand(Insn);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  validBoundary(*Result);
  EXPECT_EQ(Result->Boundary.Control, LowInstructionControl::Branch);
  EXPECT_EQ(Result->Boundary.ControlFlags, LowInstructionControlFlag::Branch);
  EXPECT_EQ(Result->Boundary.Immediate, 0x200U);
  EXPECT_EQ(Result->UndefinedEffects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_EQ(Result->Receipt.Version, 1U);
  EXPECT_EQ(Result->Receipt.OriginalOperationDigest, OriginalDigest);
  EXPECT_EQ(Result->Receipt.OriginalBoundary.Control,
            LowInstructionControl::Call);
  EXPECT_EQ(Insn.UndefinedEffects.OperationDigest, OriginalDigest);
  EXPECT_EQ(Insn.Ops.size(), 1U);
  EXPECT_NE(Result->UndefinedEffects.OperationDigest, OriginalDigest);

  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  State.write(SymSpace::Register, Stack.Offset, Ctx.mkConst(64, 0x1000));
  for (const auto &Op : Result->Ops)
    Exec.step(Op);
  EXPECT_EQ(word(Ctx, State.read(SymSpace::Register, Stack.Offset, 8)), 0xff8U);
  EXPECT_EQ(word(Ctx, State.load(Ctx.mkConst(64, 0xff8), 8)), 0x105U);
  EXPECT_EQ(word(Ctx, Exec.branchTarget()), 0x200U);
}

TEST(NativeStackControl, RegisterStackTargetIsCapturedBeforeThePush) {
  const auto Insn =
      instruction({op(NdOp::INDIR_CALL, NdVar::reg(0, 8), {Stack})},
                  SpecializationNativeStackControl::Call);
  auto Result = expand(Insn);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  validBoundary(*Result);
  EXPECT_TRUE(Result->ExpandedIndirectCall);
  EXPECT_EQ(Result->OriginalPrefixSize, 0U);
  EXPECT_FALSE(Result->Boundary.Immediate);
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  State.write(SymSpace::Register, Stack.Offset, Ctx.mkConst(64, 0x300));
  for (const auto &Op : Result->Ops)
    Exec.step(Op);
  EXPECT_EQ(word(Ctx, Exec.branchTarget()), 0x300U);
  EXPECT_EQ(word(Ctx, State.read(SymSpace::Register, Stack.Offset, 8)), 0x2f8U);
}

TEST(NativeStackControl, MemoryTargetIsLoadedBeforeItsSlotIsOverwritten) {
  auto Result = expand(memoryCall());
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  validBoundary(*Result);
  EXPECT_EQ(Result->OriginalPrefixSize, 2U);
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  State.write(SymSpace::Register, Stack.Offset, Ctx.mkConst(64, 0x1000));
  State.store(Ctx.mkConst(64, 0xff8), Ctx.mkConst(64, 0x400));
  for (const auto &Op : Result->Ops)
    Exec.step(Op);
  EXPECT_EQ(word(Ctx, Exec.branchTarget()), 0x400U);
  EXPECT_EQ(word(Ctx, State.load(Ctx.mkConst(64, 0xff8), 8)), 0x105U);
}

TEST(NativeStackControl, InternalReturnReadsTheActualOldStackThenPops) {
  auto Insn = returnInstruction();
  Insn.Origin.Immediate = 0;
  auto Result = expand(Insn);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  validBoundary(*Result);
  EXPECT_TRUE(Result->ExpandedReturn);
  EXPECT_FALSE(Result->Boundary.Immediate);
  EXPECT_EQ(Result->Boundary.ControlFlags,
            LowInstructionControlFlag::Branch |
                LowInstructionControlFlag::Indirect);
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  State.write(SymSpace::Register, Stack.Offset, Ctx.mkConst(64, 0xff8));
  State.store(Ctx.mkConst(64, 0xff8), Ctx.mkConst(64, 0x900));
  for (const auto &Op : Result->Ops)
    Exec.step(Op);
  EXPECT_EQ(word(Ctx, Exec.branchTarget()), 0x900U);
  EXPECT_EQ(word(Ctx, State.read(SymSpace::Register, Stack.Offset, 8)),
            0x1000U);
}

TEST(NativeStackControl, InternalCleanupIsUnsignedAndFollowsTheTargetRead) {
  for (uint64_t Cleanup : {uint64_t{1}, uint64_t{24}, uint64_t{65535}}) {
    auto Insn = returnInstruction();
    Insn.Origin.Immediate = Cleanup;
    auto Result = expand(Insn);
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    validBoundary(*Result);
    EXPECT_EQ(Result->Receipt.OriginalBoundary.Immediate, Cleanup);
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    State.write(SymSpace::Register, Stack.Offset, Ctx.mkConst(64, 0x1000));
    State.store(Ctx.mkConst(64, 0x1000), Ctx.mkConst(64, 0x900));
    for (const auto &Op : Result->Ops)
      Exec.step(Op);
    EXPECT_EQ(word(Ctx, Exec.branchTarget()), 0x900u);
    EXPECT_EQ(word(Ctx, State.read(SymSpace::Register, Stack.Offset, 8)),
              0x1008u + Cleanup);
  }
}

TEST(NativeStackControl, OuterReturnKeepsThePrepopObservation) {
  const auto Insn = returnInstruction();
  auto Result = expandNativeStackControl(
      Insn, Stack, Scratch, NativeReturnExpansion::OuterFunctionBoundary);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  validBoundary(*Result);
  EXPECT_FALSE(Result->ExpandedReturn);
  EXPECT_EQ(Result->Boundary.Control, LowInstructionControl::Return);
  EXPECT_EQ(Result->Ops.size(), 1U);
  EXPECT_EQ(Result->UndefinedEffects.OperationDigest,
            Insn.UndefinedEffects.OperationDigest);
}

TEST(NativeStackControl, CallNextRetainsItsSingleExplicitPush) {
  auto Insn =
      instruction({op(NdOp::INT_SUB, Stack, {Stack, NdVar::scalar(8, 8)}),
                   op(NdOp::STORE, {}, {Stack, NdVar::cst(0x105, 8)})},
                  SpecializationNativeStackControl::None);
  Insn.IsNativeCall = true;
  auto Result = expand(Insn);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  validBoundary(*Result);
  EXPECT_EQ(Result->Ops.size(), 2U);
  EXPECT_EQ(Result->Boundary.Control, LowInstructionControl::None);
  EXPECT_EQ(Result->UndefinedEffects.OperationDigest,
            Insn.UndefinedEffects.OperationDigest);
  Insn.IsNativeCall = false;
  reject(Insn);
  Insn.IsNativeCall = true;
  Insn.Ops[1].Inputs[1].Offset++;
  Insn.UndefinedEffects = {};
  reject(Insn);
}

TEST(NativeStackControl, MissingAndUnsupportedCoverageAreNeverUpgraded) {
  for (const auto Coverage :
       {LowUndefinedCoverage::Missing, LowUndefinedCoverage::Unsupported}) {
    auto Insn = directCall();
    Insn.IsNativeCall =
        false; // Legacy providers certify physical effects separately.
    Insn.UndefinedEffects = {};
    Insn.UndefinedEffects.Coverage = Coverage;
    Insn.UndefinedEffects.Diagnostic = "unavailable architecture evidence";
    auto Result = expand(Insn);
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->UndefinedEffects.Coverage, Coverage);
    EXPECT_EQ(Result->UndefinedEffects.Diagnostic,
              Insn.UndefinedEffects.Diagnostic);
    validBoundary(*Result);
  }
}

TEST(NativeStackControl, StaleOrNonemptyCompleteEvidenceIsRejected) {
  auto Insn = directCall();
  Insn.UndefinedEffects.OpCount++;
  reject(Insn);
  Insn = directCall();
  Insn.UndefinedEffects.OperationDigest = "stale";
  reject(Insn);
  Insn = directCall();
  Insn.UndefinedEffects.Effects.push_back({1, NdVar::reg(8, 1), 0, 1, {}});
  reject(Insn);
  Insn.UndefinedEffects.Coverage = LowUndefinedCoverage::Missing;
  reject(Insn);
}

TEST(NativeStackControl, ScratchRejectsPartialOperandAndReservedOverlap) {
  for (uint64_t Offset : {121U, 128U, 135U}) {
    auto Insn = memoryCall();
    Insn.Ops[1].Output = Insn.Ops[2].Inputs[0] = NdVar::tmp(Offset, 8);
    Insn.UndefinedEffects = {};
    reject(Insn);
  }
  for (uint64_t Offset : {121U, 128U, 135U}) {
    const NdVar Reserved = NdVar::tmp(Offset, 8);
    auto Result = expandNativeStackControl(
        directCall(), Stack, Scratch, NativeReturnExpansion::InternalTransfer,
        {Reserved});
    ASSERT_FALSE(static_cast<bool>(Result));
    llvm::consumeError(Result.takeError());
  }
  const NdVar Reserved = NdVar::tmp(136, 8);
  auto Adjacent = expandNativeStackControl(
      directCall(), Stack, Scratch, NativeReturnExpansion::InternalTransfer,
      {Reserved});
  ASSERT_TRUE(static_cast<bool>(Adjacent))
      << llvm::toString(Adjacent.takeError());
}

TEST(NativeStackControl, ScratchMayEndAtTheLastAddressWithoutWrapping) {
  const auto Last = std::numeric_limits<uint64_t>::max();
  auto Result =
      expandNativeStackControl(directCall(), Stack, NdVar::tmp(Last - 7, 8),
                               NativeReturnExpansion::InternalTransfer);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  auto Wrapped =
      expandNativeStackControl(directCall(), Stack, NdVar::tmp(Last - 6, 8),
                               NativeReturnExpansion::InternalTransfer);
  ASSERT_FALSE(static_cast<bool>(Wrapped));
  llvm::consumeError(Wrapped.takeError());
}

TEST(NativeStackControl, RejectsStaleBoundaryAndOperationCoordinates) {
  for (unsigned Variant = 0; Variant < 7; ++Variant) {
    SCOPED_TRACE(Variant);
    auto Insn = directCall();
    if (Variant == 0)
      Insn.Origin.OpCount++;
    else if (Variant == 1)
      Insn.Origin.Immediate = 0x201;
    else if (Variant == 2)
      Insn.Ops[0].Seq++;
    else if (Variant == 3)
      Insn.Ops[0].Addr++;
    else if (Variant == 4)
      Insn.Fallthrough.Address++;
    else if (Variant == 5)
      Insn.NativeBytes = {0xe8};
    else
      Insn.Origin.ControlFlags |= LowInstructionControlFlag::NoReturn;
    reject(Insn);
  }
}

TEST(NativeStackControl, RejectsNoncanonicalMemoryPrefixes) {
  for (unsigned Variant = 0; Variant < 7; ++Variant) {
    SCOPED_TRACE(Variant);
    auto Insn = memoryCall();
    if (Variant == 0)
      Insn.Ops[1].Inputs[0].Size = 4;
    else if (Variant == 1)
      Insn.Ops[1].Output.Size = Insn.Ops[2].Inputs[0].Size = 4;
    else if (Variant == 2)
      Insn.Ops[0].Output = NdVar::reg(0, 8);
    else if (Variant == 3)
      Insn.Ops[0].Opcode = NdOp::LOAD;
    else if (Variant == 4)
      Insn.Ops[0].Inputs[1].Size = 4;
    else if (Variant == 5)
      Insn.Ops[1].Opcode = NdOp::COPY;
    else
      Insn.Origin.Immediate = 0x200;
    Insn.UndefinedEffects = {};
    reject(Insn);
  }
}

TEST(NativeStackControl, RejectsDirectPrefixAndInvalidCleanupBoundaries) {
  auto Insn = directCall();
  Insn.Ops.insert(Insn.Ops.begin(), op(NdOp::COPY, NdVar::tmp(0, 8), {Stack}));
  Insn.Ops[0].Addr = Insn.Origin.Address;
  Insn.Ops[1].Seq = 1;
  Insn.Origin.OpCount = 2;
  Insn.UndefinedEffects = {};
  reject(Insn);
  Insn = returnInstruction();
  Insn.Origin.Immediate = 65536;
  reject(Insn);
  Insn.Origin.Immediate = 8;
  auto Outer = expandNativeStackControl(
      Insn, Stack, Scratch, NativeReturnExpansion::OuterFunctionBoundary);
  ASSERT_FALSE(static_cast<bool>(Outer));
  EXPECT_FALSE(llvm::toString(Outer.takeError()).empty());
}

TEST(NativeStackControl, FreshMemoryCallBindsPreservationAndTargetBeforePush) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  SpecializationInstruction I;
  I.NativeBytes = {0xff, 0x54, 0x24, 0xf8}; // call qword [rsp-8]
  DecodedInsn Decoded{};
  ASSERT_EQ(D.decodeOneForLift(I.NativeBytes.data(), I.NativeBytes.size(),
                               0x100, Decoded),
            4);
  ASSERT_TRUE(D.liftX64MemoryCallToLow(Decoded, I.Ops, &I.UndefinedEffects,
                                       &I.PreservedState));
  I.Origin.Address = 0x100;
  I.Origin.Size = 4;
  I.Origin.OpCount = I.Ops.size();
  I.Origin.Control = LowInstructionControl::Call;
  I.Origin.ControlFlags =
      LowInstructionControlFlag::Call | LowInstructionControlFlag::Indirect;
  I.IsNativeCall = true;
  I.NativeStackControl = SpecializationNativeStackControl::Call;
  I.Fallthrough.Address = 0x104;
  const auto Original = I.PreservedState;
  auto R = expand(I);
  ASSERT_TRUE(static_cast<bool>(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Receipt.Version, 2U);
  EXPECT_EQ(R->Receipt.PreservedStateDigest, lowPreservedStateDigest(Original));
  EXPECT_EQ(R->Receipt.OriginalOperationDigest, Original.OperationDigest);
  EXPECT_EQ(R->Receipt.ExpandedOperationDigest,
            lowUndefinedOperationDigest(R->Ops));
  EXPECT_EQ(I.PreservedState, Original);
  EXPECT_FALSE(
      matchesLowPreservedState(Original, R->Boundary, I.NativeBytes, R->Ops));
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  State.write(SymSpace::Register, Stack.Offset, Ctx.mkConst(64, 0x1000));
  State.store(Ctx.mkConst(64, 0xff8), Ctx.mkConst(64, 0x400));
  for (const auto &Op : R->Ops)
    Exec.step(Op);
  EXPECT_EQ(word(Ctx, Exec.branchTarget()), 0x400U);
  EXPECT_EQ(word(Ctx, State.load(Ctx.mkConst(64, 0xff8), 8)), 0x104U);
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Bad = I;
    switch (Mutation) {
    case 0:
      ++Bad.PreservedState.SemanticsVersion;
      break;
    case 1:
      Bad.NativeBytes.back() ^= 1;
      break;
    case 2:
      ++Bad.Ops.front().Seq;
      Bad.UndefinedEffects.OperationDigest =
          lowUndefinedOperationDigest(Bad.Ops);
      break;
    case 3:
      Bad.PreservedState.Audit = LowPreservedStateAudit::Missing;
      break;
    case 4:
      Bad.ProfileProjection =
          InterpreterProfileProjection::CetDisabledReadShadowStackV1;
      break;
    }
    reject(Bad);
  }
}

} // namespace
