//===- NdOpEmulatorTests.cpp - arithmetic and comparison tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NdOpEmulatorTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/symbolic/SymExec.h"
#include "neverd/symbolic/SymExplore.h"

#include <optional>
#include <vector>

namespace {

using namespace neverd;
using namespace neverd::ndop_emulator_test;

TEST_F(NdOpEmulatorTest, BasicArithmetic) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 10);

  LowOp Add = makeArith(NdOp::INT_ADD, 8, 0, 5);
  ASSERT_TRUE(Emu.step(Add));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 15u);
}

TEST_F(NdOpEmulatorTest, SubAndShift) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 20);

  LowOp Sub = makeArith(NdOp::INT_SUB, 8, 0, 5);
  ASSERT_TRUE(Emu.step(Sub));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 15u);

  LowOp Shift = makeArith(NdOp::INT_LEFT, 16, 8, 2);
  ASSERT_TRUE(Emu.step(Shift));
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 60u);
}

TEST_F(NdOpEmulatorTest, CopyChain) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 42);

  LowOp C1 = makeCopy(8, 0);
  LowOp C2 = makeCopy(16, 8);
  ASSERT_TRUE(Emu.step(C1));
  ASSERT_TRUE(Emu.step(C2));
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 42u);
}

TEST_F(NdOpEmulatorTest, RunStopsAtTerminator) {
  std::vector<LowOp> Ops;
  Ops.push_back(makeArith(NdOp::INT_ADD, 8, 0, 1));
  Ops.push_back(makeArith(NdOp::INT_ADD, 16, 8, 2));
  Ops.push_back(makeBranchInd(16));
  Ops.push_back(makeArith(NdOp::INT_ADD, 24, 16, 3));

  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 10);
  size_t Ran = Emu.run(Ops);
  EXPECT_EQ(Ran, 2u);
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 13u);
  EXPECT_FALSE(Emu.getRegister(24).has_value());
}

TEST_F(NdOpEmulatorTest, RunTreatsPredicatedReturnAsOneGuestInstruction) {
  constexpr va_t PredicatedAddress = 0x1100;
  constexpr va_t NextAddress = PredicatedAddress + 4;
  constexpr uint64_t FlagReg = 64;

  auto operations = [&]() {
    LowOp Guard;
    Guard.Opcode = NdOp::COND_BR;
    Guard.Addr = PredicatedAddress;
    Guard.addInput(NdVar::cst(NextAddress, 8));
    Guard.addInput(NdVar::reg(FlagReg, 1));

    LowOp Effect = makeCopy(8, 0);
    Effect.Addr = PredicatedAddress;
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = PredicatedAddress;
    LowOp Continued = makeCopy(16, 0);
    Continued.Addr = NextAddress;
    return std::vector<LowOp>{Guard, Effect, Return, Continued};
  };

  NdOpEmulator Skipped(Img);
  Skipped.setRegister(0, 42);
  Skipped.setRegister(FlagReg, 1);
  Skipped.run(operations());
  EXPECT_FALSE(Skipped.getRegister(8).has_value());
  EXPECT_EQ(Skipped.getRegister(16).value_or(0), 42u)
      << "a true skip guard continues at the next instruction";

  NdOpEmulator Returned(Img);
  Returned.setRegister(0, 42);
  Returned.setRegister(FlagReg, 0);
  Returned.run(operations());
  EXPECT_EQ(Returned.getRegister(8).value_or(0), 42u)
      << "a false guard executes the predicated instruction effects";
  EXPECT_FALSE(Returned.getRegister(16).has_value())
      << "the conditional return ends only its executing truth path";

  NdOpEmulator UnknownPredicate(Img);
  UnknownPredicate.setRegister(0, 42);
  UnknownPredicate.run(operations());
  EXPECT_FALSE(UnknownPredicate.getRegister(8).has_value());
  EXPECT_FALSE(UnknownPredicate.getRegister(16).has_value())
      << "an unknown predicate must not be guessed in either direction";
}

TEST_F(NdOpEmulatorTest, RunPreservesPredicatedDirectAndIndirectCalls) {
  constexpr va_t PredicatedAddress = 0x1200;
  constexpr va_t NextAddress = PredicatedAddress + 4;
  constexpr uint64_t FlagReg = 64;

  for (NdOp CallOpcode : {NdOp::CALL, NdOp::INDIR_CALL}) {
    SCOPED_TRACE(ndOpName(CallOpcode));
    auto operations = [&]() {
      LowOp Guard;
      Guard.Opcode = NdOp::COND_BR;
      Guard.Addr = PredicatedAddress;
      Guard.addInput(NdVar::cst(NextAddress, 8));
      Guard.addInput(NdVar::reg(FlagReg, 1));

      LowOp Effect = makeCopy(8, 0);
      Effect.Addr = PredicatedAddress;
      LowOp Call;
      Call.Opcode = CallOpcode;
      Call.Addr = PredicatedAddress;
      Call.addInput(CallOpcode == NdOp::CALL ? NdVar::cst(0x1800, 8)
                                             : NdVar::reg(24, 8));
      LowOp Continued = makeCopy(16, 0);
      Continued.Addr = NextAddress;
      return std::vector<LowOp>{Guard, Effect, Call, Continued};
    };

    NdOpEmulator Skipped(Img);
    Skipped.setCallPreservedRegisters({0, 8});
    Skipped.setRegister(0, 13);
    Skipped.setRegister(24, 0x1800);
    Skipped.setRegister(FlagReg, 1);
    Skipped.run(operations());
    EXPECT_FALSE(Skipped.getRegister(8).has_value());
    EXPECT_EQ(Skipped.getRegister(16).value_or(0), 13u);

    NdOpEmulator Called(Img);
    Called.setCallPreservedRegisters({0, 8});
    Called.setRegister(0, 13);
    Called.setRegister(24, 0x1800);
    Called.setRegister(FlagReg, 0);
    Called.run(operations());
    EXPECT_EQ(Called.getRegister(8).value_or(0), 13u);
    EXPECT_EQ(Called.getRegister(16).value_or(0), 13u)
        << "a returning predicated call rejoins its skip continuation";
  }
}

TEST_F(NdOpEmulatorTest, RunExecutesPredicatedIndirectBranchOnlyWhenSelected) {
  constexpr va_t PredicatedAddress = 0x1300;
  constexpr va_t NextAddress = PredicatedAddress + 4;
  constexpr uint64_t FlagReg = 64;

  LowOp Guard;
  Guard.Opcode = NdOp::COND_BR;
  Guard.Addr = PredicatedAddress;
  Guard.addInput(NdVar::cst(NextAddress, 8));
  Guard.addInput(NdVar::reg(FlagReg, 1));
  LowOp Target = makeCopy(8, 0);
  Target.Addr = PredicatedAddress;
  LowOp Branch = makeBranchInd(8);
  Branch.Addr = PredicatedAddress;
  LowOp Continued = makeCopy(16, 0);
  Continued.Addr = NextAddress;
  const std::vector<LowOp> Ops{Guard, Target, Branch, Continued};

  NdOpEmulator Skipped(Img);
  Skipped.setRegister(0, 0x1400);
  Skipped.setRegister(FlagReg, 1);
  Skipped.run(Ops);
  EXPECT_FALSE(Skipped.getRegister(8).has_value());
  EXPECT_EQ(Skipped.getRegister(16).value_or(0), 0x1400u);

  NdOpEmulator Branched(Img);
  Branched.setRegister(0, 0x1400);
  Branched.setRegister(FlagReg, 0);
  Branched.run(Ops);
  EXPECT_EQ(Branched.getRegister(8).value_or(0), 0x1400u);
  EXPECT_FALSE(Branched.getRegister(16).has_value());
}

TEST_F(NdOpEmulatorTest,
       LowBlockPredicatedMemoryEffectsSkipExecuteAndRejectUnknownGuard) {
  constexpr uint64_t FlagReg = 64;

  LowOp Guard;
  Guard.Opcode = NdOp::COND_BR;
  Guard.Addr = 0;
  Guard.addInput(NdVar::cst(4, 8));
  Guard.addInput(NdVar::reg(FlagReg, 1));

  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Addr = 0;
  Load.Output = NdVar::reg(8, 4);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::cst(0x1010, 8));

  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.Addr = 0;
  Store.addInput(NdVar::cst(0, 8));
  Store.addInput(NdVar::cst(0x1020, 8));
  Store.addInput(NdVar::reg(0, 4));

  LowOp LoadBack;
  LoadBack.Opcode = NdOp::LOAD;
  LoadBack.Addr = 4;
  LoadBack.Output = NdVar::reg(16, 4);
  LoadBack.addInput(NdVar::cst(0, 8));
  LoadBack.addInput(NdVar::cst(0x1020, 8));

  LowBlock Block;
  Block.StartAddr = 0;
  Block.EndAddr = 8;
  Block.Ops = {Guard, Load, Store, LoadBack};
  Block.InstructionBoundaries.push_back(
      {0, 4, 0, 3, InstructionMode::ARM, LowInstructionControl::Branch,
       LowInstructionControlFlag::Branch |
           LowInstructionControlFlag::Conditional |
           LowInstructionControlFlag::InstructionGuard,
       LowInstructionTargetMode::Preserve, 4});
  Block.InstructionBoundaries.push_back(
      {4, 4, 3, 1, InstructionMode::ARM, LowInstructionControl::None,
       LowInstructionControlFlag::None, LowInstructionTargetMode::Preserve,
       std::nullopt});

  NdOpEmulator Skipped(Img);
  Skipped.setRegister(0, 42);
  Skipped.setRegister(8, 7);
  Skipped.setRegister(FlagReg, 1);
  Skipped.run(Block);
  EXPECT_EQ(Skipped.getRegister(8).value_or(0), 7u);
  EXPECT_EQ(Skipped.getRegister(16).value_or(~0ULL), 0u);

  NdOpEmulator Executed(Img);
  Executed.setRegister(0, 42);
  Executed.setRegister(8, 7);
  Executed.setRegister(FlagReg, 0);
  Executed.run(Block);
  EXPECT_EQ(Executed.getRegister(8).value_or(~0ULL), 0u);
  EXPECT_EQ(Executed.getRegister(16).value_or(0), 42u);

  NdOpEmulator Unknown(Img);
  Unknown.setRegister(0, 42);
  Unknown.setRegister(8, 7);
  Unknown.run(Block);
  EXPECT_EQ(Unknown.getRegister(8).value_or(0), 7u);
  EXPECT_FALSE(Unknown.getRegister(16).has_value());

  LowBlock Malformed = Block;
  const uint16_t WithoutGuard =
      static_cast<uint16_t>(
          Malformed.InstructionBoundaries.front().ControlFlags) &
      ~static_cast<uint16_t>(LowInstructionControlFlag::InstructionGuard);
  Malformed.InstructionBoundaries.front().ControlFlags =
      static_cast<LowInstructionControlFlag>(WithoutGuard);
  NdOpEmulator Rejected(Img);
  Rejected.setRegister(FlagReg, 0);
  EXPECT_EQ(Rejected.run(Malformed), 0u);
  EXPECT_FALSE(Rejected.getRegister(8).has_value());
}

TEST_F(NdOpEmulatorTest,
       ComputeTargetDoesNotReportAPredicatedBranchThatWasSkipped) {
  constexpr va_t PredicatedAddress = 0x1500;
  constexpr va_t NextAddress = PredicatedAddress + 4;

  LowOp Guard;
  Guard.Opcode = NdOp::COND_BR;
  Guard.Addr = PredicatedAddress;
  Guard.addInput(NdVar::cst(NextAddress, 8));
  Guard.addInput(NdVar::reg(0, 1));
  LowOp Target = makeCopy(8, 0);
  Target.Addr = PredicatedAddress;
  LowOp Branch = makeBranchInd(8);
  Branch.Addr = PredicatedAddress;
  const std::vector<LowOp> Ops{Guard, Target, Branch};

  NdOpEmulator Emu(Img);
  EXPECT_FALSE(Emu.computeTarget(Ops, 0, 1).has_value());
  EXPECT_EQ(Emu.computeTarget(Ops, 0, 0).value_or(~0ULL), 0u);
}

TEST_F(NdOpEmulatorTest, ComputeTarget) {
  std::vector<LowOp> Ops;
  Ops.push_back(makeArith(NdOp::INT_MULT, 8, 0, 4));
  Ops.push_back(makeArith(NdOp::INT_ADD, 16, 8, 0x1000));
  Ops.push_back(makeBranchInd(16));

  NdOpEmulator Emu(Img);
  auto Tgt = Emu.computeTarget(Ops, 0, 3);
  ASSERT_TRUE(Tgt.has_value());
  EXPECT_EQ(*Tgt, 0x100Cu);
}

TEST_F(NdOpEmulatorTest,
       LowBlockTargetUsesBoundaryMetadataAtZeroVirtualAddress) {
  constexpr uint64_t FlagReg = 64;
  auto MakeBlock = [=](uint64_t RawTarget) {
    LowOp Guard;
    Guard.Opcode = NdOp::COND_BR;
    Guard.Addr = 0;
    Guard.addInput(NdVar::cst(4, 8));
    Guard.addInput(NdVar::reg(FlagReg, 1));

    LowOp Branch;
    Branch.Opcode = NdOp::INDIR_BR;
    Branch.Addr = 0;
    Branch.addInput(NdVar::cst(RawTarget, 8));

    LowBlock Block;
    Block.StartAddr = 0;
    Block.EndAddr = 4;
    Block.Ops = {Guard, Branch};
    Block.InstructionBoundaries.push_back(
        {0, 4, 0, 2, InstructionMode::ARM, LowInstructionControl::Branch,
         LowInstructionControlFlag::Branch |
             LowInstructionControlFlag::Conditional |
             LowInstructionControlFlag::Indirect |
             LowInstructionControlFlag::InstructionGuard,
         LowInstructionTargetMode::FromTargetBit0, std::nullopt});
    return Block;
  };

  const LowBlock ThumbBlock = MakeBlock(0x1001);
  NdOpEmulator Emu(Img);
  auto Target = Emu.computeTarget(ThumbBlock, FlagReg, 0);
  ASSERT_TRUE(Target.has_value());
  EXPECT_EQ(Target->Address, 0x1000u);
  EXPECT_EQ(Target->Mode, InstructionMode::Thumb);

  EXPECT_FALSE(Emu.computeTarget(ThumbBlock, FlagReg, 1).has_value())
      << "the guard-taken path skips the indirect transfer";
  const std::vector<LowOp> LegacyOps{ThumbBlock.Ops.back()};
  EXPECT_EQ(Emu.computeTarget(LegacyOps, FlagReg, 0).value_or(0), 0x1001u)
      << "the vector API keeps its raw-target compatibility contract";

  const LowBlock MisalignedARM = MakeBlock(0x1002);
  EXPECT_FALSE(Emu.computeTarget(MisalignedARM, FlagReg, 0).has_value());
}

TEST_F(NdOpEmulatorTest, BitwiseOps) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0xFF00);

  LowOp And = makeArith(NdOp::INT_AND, 8, 0, 0xFF);
  ASSERT_TRUE(Emu.step(And));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 0u);

  Emu.setRegister(0, 0xA5);
  LowOp Xor = makeArith(NdOp::INT_XOR, 8, 0, 0xFF);
  ASSERT_TRUE(Emu.step(Xor));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 0x5Au);
}

TEST_F(NdOpEmulatorTest, ResetClearsState) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 42);
  EXPECT_TRUE(Emu.getRegister(0).has_value());
  Emu.reset();
  EXPECT_FALSE(Emu.getRegister(0).has_value());
}
TEST_F(NdOpEmulatorTest, DivAndRem) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 100);

  LowOp Div = makeArith(NdOp::INT_DIV, 8, 0, 7);
  ASSERT_TRUE(Emu.step(Div));
  EXPECT_EQ(Emu.getRegister(8).value_or(0), 14u);

  LowOp Rem = makeArith(NdOp::INT_REM, 16, 0, 7);
  ASSERT_TRUE(Emu.step(Rem));
  EXPECT_EQ(Emu.getRegister(16).value_or(0), 2u);
}

TEST_F(NdOpEmulatorTest, DivByZeroNoOutput) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 42);

  LowOp Div = makeArith(NdOp::INT_DIV, 8, 0, 0);
  Emu.step(Div);
  EXPECT_FALSE(Emu.getRegister(8).has_value());
}

TEST_F(NdOpEmulatorTest, SignedDivAndRem) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, static_cast<uint64_t>(-15));

  LowOp Div = makeArith(NdOp::INT_SDIV, 8, 0, 4);
  ASSERT_TRUE(Emu.step(Div));
  EXPECT_EQ(static_cast<int64_t>(Emu.getRegister(8).value_or(0)), -3);

  LowOp Rem = makeArith(NdOp::INT_SREM, 16, 0, 4);
  ASSERT_TRUE(Emu.step(Rem));
  EXPECT_EQ(static_cast<int64_t>(Emu.getRegister(16).value_or(0)), -3);
}

TEST_F(NdOpEmulatorTest, CompareOps) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 10);

  LowOp Eq = makeArith(NdOp::INT_EQUAL, 8, 0, 10);
  ASSERT_TRUE(Emu.step(Eq));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 1u);

  LowOp Neq = makeArith(NdOp::INT_NOTEQUAL, 16, 0, 10);
  ASSERT_TRUE(Emu.step(Neq));
  EXPECT_EQ(Emu.getRegister(16).value_or(~0ULL), 0u);

  LowOp Lt = makeArith(NdOp::INT_LESS, 24, 0, 20);
  ASSERT_TRUE(Emu.step(Lt));
  EXPECT_EQ(Emu.getRegister(24).value_or(~0ULL), 1u);

  LowOp Le = makeArith(NdOp::INT_LESSEQUAL, 32, 0, 10);
  ASSERT_TRUE(Emu.step(Le));
  EXPECT_EQ(Emu.getRegister(32).value_or(~0ULL), 1u);
}

TEST_F(NdOpEmulatorTest, SignedCompareOps) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, static_cast<uint64_t>(-5));

  LowOp SLess = makeArith(NdOp::INT_SLESS, 8, 0, 0);
  ASSERT_TRUE(Emu.step(SLess));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 1u);

  LowOp SLeq =
      makeArith(NdOp::INT_SLESSEQUAL, 16, 0, static_cast<uint64_t>(-5));
  ASSERT_TRUE(Emu.step(SLeq));
  EXPECT_EQ(Emu.getRegister(16).value_or(~0ULL), 1u);
}

TEST_F(NdOpEmulatorTest, BoolOps) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 1);

  LowOp BoolNot;
  BoolNot.Opcode = NdOp::BOOL_NOT;
  BoolNot.Output = NdVar::reg(8, 1);
  BoolNot.addInput(NdVar::reg(0, 1));
  ASSERT_TRUE(Emu.step(BoolNot));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 0u);

  LowOp BoolAnd = makeArith(NdOp::BOOL_AND, 16, 0, 1);
  ASSERT_TRUE(Emu.step(BoolAnd));
  EXPECT_EQ(Emu.getRegister(16).value_or(~0ULL), 1u);

  LowOp BoolOr = makeArith(NdOp::BOOL_OR, 24, 0, 0);
  ASSERT_TRUE(Emu.step(BoolOr));
  EXPECT_EQ(Emu.getRegister(24).value_or(~0ULL), 1u);
}
TEST_F(NdOpEmulatorTest, CarryAndBorrowFlags) {
  NdOpEmulator Emu(Img);

  // INT_CARRY: unsigned overflow detection
  Emu.setRegister(0, ~0ULL);
  LowOp Carry = makeArith(NdOp::INT_CARRY, 8, 0, 1);
  ASSERT_TRUE(Emu.step(Carry));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 1u);

  Emu.setRegister(0, 0);
  ASSERT_TRUE(Emu.step(Carry));
  EXPECT_EQ(Emu.getRegister(8).value_or(~0ULL), 0u);

  // INT_SOVF: signed overflow detection
  Emu.setRegister(0, INT64_MAX);
  LowOp SCarry = makeArith(NdOp::INT_SOVF, 16, 0, 1);
  ASSERT_TRUE(Emu.step(SCarry));
  EXPECT_EQ(Emu.getRegister(16).value_or(~0ULL), 1u);

  // INT_SBOR: signed borrow detection
  Emu.setRegister(0, INT64_MIN);
  LowOp SBorrow = makeArith(NdOp::INT_SBOR, 24, 0, 1);
  ASSERT_TRUE(Emu.step(SBorrow));
  EXPECT_EQ(Emu.getRegister(24).value_or(~0ULL), 1u);
}

//===----------------------------------------------------------------------===//
// Reporting what was stepped over
//===----------------------------------------------------------------------===//

TEST_F(NdOpEmulatorTest, AnOpcodeWithNoModelIsCountedAndSteppedOverByDefault) {
  NdOpEmulator Emu(Img);

  LowOp NoModel;
  NoModel.Opcode = NdOp::FLOAT_ADD;
  NoModel.Output = NdVar::reg(8, 8);
  NoModel.addInput(NdVar::reg(0, 8));
  NoModel.addInput(NdVar::reg(0, 8));

  EXPECT_TRUE(Emu.step(NoModel))
      << "switch recovery needs the path to carry on past it";
  EXPECT_EQ(Emu.skips().UnsupportedOps, 1u);
  EXPECT_TRUE(Emu.skips().any());

  // A caller reading values back out rather than validating them wants the
  // opposite bargain, and can have it without changing anyone else's.
  Emu.setStrictMode(true);
  EXPECT_FALSE(Emu.step(NoModel));
  EXPECT_EQ(Emu.skips().UnsupportedOps, 2u);

  Emu.clearSkips();
  EXPECT_FALSE(Emu.skips().any());
}

TEST_F(NdOpEmulatorTest, AFullWriteBackStoreIsReportedRatherThanSilent) {
  NdOpEmulator Emu(Img);
  Emu.setRegister(0, 0x42);

  auto storeTo = [](uint64_t Addr) {
    LowOp Store;
    Store.Opcode = NdOp::STORE;
    Store.Output = {};
    Store.addInput(NdVar::cst(0, 8));
    Store.addInput(NdVar::cst(Addr, 8));
    Store.addInput(NdVar::reg(0, 4));
    return Store;
  };

  for (int I = 0; I < limits::kMaxEmulatorStoreEntries; ++I)
    ASSERT_TRUE(Emu.step(storeTo(0x2000 + uint64_t(I) * 8)));
  EXPECT_EQ(Emu.skips().DroppedStores, 0u);

  // One past what the store holds.  The write is still dropped — the bound is
  // there to stop a long path eating memory — but a later load of that address
  // now reads what was underneath it, and that is worth being able to find
  // out about.
  LowOp TooMany = storeTo(0x9000);
  EXPECT_TRUE(Emu.step(TooMany));
  EXPECT_EQ(Emu.skips().DroppedStores, 1u);

  Emu.setStrictMode(true);
  EXPECT_FALSE(Emu.step(TooMany));
}

//===----------------------------------------------------------------------===//
// The concrete half of a concolic walk
//===----------------------------------------------------------------------===//

/// A two-way function: `rbx = rax < 10 ? 111 : 222`, laid out as a block per
/// arm so that a walk of it has a genuine choice to make.
LowFunc buildComparisonFunction(uint64_t IndexReg, uint64_t FlagReg) {
  constexpr va_t kBase = 0x2000;
  constexpr uint64_t kBlockSize = 0x10;
  constexpr uint64_t kResultReg = 16;

  LowFunc Func;
  auto addressOf = [](int Block) {
    return NdVar::cst(kBase + uint64_t(Block) * kBlockSize, 8);
  };
  auto addBlock = [&](std::vector<LowOp> Ops, std::vector<int> Succs) {
    LowBlock B;
    B.Id = static_cast<int>(Func.Blocks.size());
    B.StartAddr = kBase + uint64_t(B.Id) * kBlockSize;
    B.EndAddr = B.StartAddr + kBlockSize;
    B.Ops = std::move(Ops);
    B.Succs = std::move(Succs);
    Func.Blocks.push_back(std::move(B));
  };
  auto makeOp = [](NdOp Opcode, NdVar Output, std::vector<NdVar> Inputs) {
    LowOp Result;
    Result.Opcode = Opcode;
    Result.Output = Output;
    for (const NdVar &In : Inputs)
      Result.addInput(In);
    return Result;
  };

  addBlock(
      {makeOp(NdOp::INT_LESS, NdVar::reg(FlagReg, 1),
              {NdVar::reg(IndexReg, 8), NdVar::cst(10, 8)}),
       makeOp(NdOp::COND_BR, NdVar{}, {addressOf(1), NdVar::reg(FlagReg, 1)})},
      {1, 2});
  addBlock({makeOp(NdOp::COPY, NdVar::reg(kResultReg, 8), {NdVar::cst(111, 8)}),
            makeOp(NdOp::RETURN, NdVar{}, {})},
           {});
  addBlock({makeOp(NdOp::COPY, NdVar::reg(kResultReg, 8), {NdVar::cst(222, 8)}),
            makeOp(NdOp::RETURN, NdVar{}, {})},
           {});
  Func.Entry = kBase;
  return Func;
}

TEST_F(NdOpEmulatorTest, AConcolicWalkFollowsTheSeededRunAndKeepsItsCondition) {
  using namespace neverd::symbolic;

  constexpr uint64_t kIndexReg = 0;
  constexpr uint64_t kFlagReg = 64;
  const LowFunc Func = buildComparisonFunction(kIndexReg, kFlagReg);

  SymContext Ctx;
  NdOpEmulator Emu(Img);
  // A concolic walk reads values back out of the concrete state to decide
  // branches, so it wants to be told when the emulator stepped over something
  // rather than to read a register that operation left stale.
  Emu.setStrictMode(true);
  NdOpEmulatorShadow Shadow(Emu);

  auto walk = [&](uint64_t Index) {
    ExploreOptions Opts;
    Opts.Concolic = &Shadow;
    Opts.ConcolicSeed.push_back({kIndexReg, Index});
    return explorePathsDetailed(Ctx, Func, Opts);
  };

  SymExploration Below = walk(3);
  ASSERT_EQ(Below.Paths.size(), 1u) << "a concolic walk follows one run";
  EXPECT_EQ(Below.Paths[0].Outcome, PathOutcome::Returned);
  EXPECT_EQ(Below.Paths[0].Blocks, (std::vector<int>{0, 1}));
  EXPECT_EQ(Below.Paths[0].ConcreteBranches, 1u);
  ASSERT_EQ(Below.Paths[0].Constraints.size(), 1u);
  EXPECT_EQ(Below.Paths[0].UnmodelledOps, 0u);

  SymExploration Above = walk(20);
  ASSERT_EQ(Above.Paths.size(), 1u);
  EXPECT_EQ(Above.Paths[0].Blocks, (std::vector<int>{0, 2}));
  ASSERT_EQ(Above.Paths[0].Constraints.size(), 1u);

  // The two runs went opposite ways and the conditions they recorded say so,
  // which is the whole product of a concolic walk: not where one input went,
  // but what an input has to satisfy to go there.
  EXPECT_FALSE(Ctx.isConst(Below.Paths[0].Constraints[0]));
  EXPECT_EQ(Ctx.mkNot(Below.Paths[0].Constraints[0]),
            Above.Paths[0].Constraints[0]);
}

TEST_F(NdOpEmulatorTest, WithoutASeedTheSymbolicWalkStillForksBothWays) {
  // The concolic option is additive: the same function walked without a
  // shadow is walked exactly as it was before there was one.
  using namespace neverd::symbolic;

  const LowFunc Func = buildComparisonFunction(0, 64);
  SymContext Ctx;
  std::vector<SymPath> Paths = explorePaths(Ctx, Func);
  ASSERT_EQ(Paths.size(), 2u);
  EXPECT_EQ(Paths[0].Blocks, (std::vector<int>{0, 1}));
  EXPECT_EQ(Paths[1].Blocks, (std::vector<int>{0, 2}));
  EXPECT_EQ(Paths[0].ConcreteBranches, 0u);
}

//===----------------------------------------------------------------------===//
// The two engines against each other
//===----------------------------------------------------------------------===//

TEST_F(NdOpEmulatorTest, TheTwoEnginesAgreeOnTheBitOpcodes) {
  // Neither engine can notice from its own side that the other computes an
  // opcode differently, so nothing but running both of them says they agree.
  // Every operand here is narrower than a machine word, which is where the two
  // have room to disagree: the concrete side keeps its values in 64-bit slots
  // and the symbolic side works at the width each operand declares.
  using namespace neverd::symbolic;

  constexpr uint64_t kIn = 0;
  constexpr uint64_t kOut = 8;

  auto agree = [&](const LowOp &Op, uint64_t Input) {
    SCOPED_TRACE(ndOpName(Op.Opcode));

    NdOpEmulator Emu(Img);
    Emu.setStrictMode(true);
    Emu.setRegister(kIn, Input);
    EXPECT_TRUE(Emu.step(Op));
    const std::optional<uint64_t> Concrete = Emu.getRegister(kOut);
    ASSERT_TRUE(Concrete.has_value());

    SymContext Ctx;
    SymState State(Ctx);
    State.write(SymSpace::Register, kIn,
                Ctx.mkConst(uint32_t(Op.Inputs[0].Size) * 8, Input));
    SymExec Exec(Ctx, State);
    EXPECT_EQ(Exec.step(Op), StepResult::Continue);
    EXPECT_EQ(Exec.unmodelledCount(), 0u);

    SymRef Symbolic = State.read(SymSpace::Register, kOut, Op.Output.Size);
    ASSERT_TRUE(Ctx.isConst(Symbolic));
    EXPECT_EQ(Ctx.constValue(Symbolic).getZExtValue(), *Concrete);
  };

  auto bitOp = [kIn, kOut](NdOp Opcode, uint16_t InSize,
                           std::vector<NdVar> Rest) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = NdVar::reg(kOut, InSize);
    Op.addInput(NdVar::reg(kIn, InSize));
    for (const NdVar &In : Rest)
      Op.addInput(In);
    return Op;
  };

  // A byte holding 0xF0 has four bits set and four leading zeros.  Read as the
  // whole slot it sits in it has the same four bits set and fifty-six leading
  // zeros, so an engine that forgets the declared width is wrong about one of
  // these and right about the other.
  agree(bitOp(NdOp::POPCOUNT, 1, {}), 0xF0);
  agree(bitOp(NdOp::LZCOUNT, 1, {}), 0xF0);
  agree(bitOp(NdOp::EXTRACT, 2, {NdVar::cst(4, 1), NdVar::cst(8, 1)}), 0xDEAD);
  agree(bitOp(NdOp::INSERT, 2,
              {NdVar::cst(0xAB, 1), NdVar::cst(0, 1), NdVar::cst(8, 1)}),
        0xFF00);
}

} // namespace
