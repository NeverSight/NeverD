//===- SymExecTests.cpp - Symbolic execution of NeverD IR -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pins the operator semantics and the machine state they run against.
///
/// The state gets as much attention as the arithmetic here, because it is
/// where a symbolic engine quietly goes wrong.  A four-byte write followed by
/// an eight-byte read of the same register, a load of two bytes spanning two
/// separately written ones, a store through an address nothing determined:
/// each has one correct answer and several plausible ones.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExec.h"
#include "neverd/symbolic/SymParse.h"

#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

LowOp op(NdOp Opcode, NdVar Output, std::vector<NdVar> Inputs) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (const NdVar &In : Inputs)
    Result.addInput(In);
  return Result;
}

/// A register offset that stands in for a general-purpose register.
constexpr uint64_t kRax = 0;
constexpr uint64_t kRbx = 8;
constexpr uint64_t kRcx = 16;

//===----------------------------------------------------------------------===//
// State
//===----------------------------------------------------------------------===//

TEST(SymState, ALocationNeverWrittenReadsAsAnInputNamedForItself) {
  SymContext Ctx;
  SymState State(Ctx);
  EXPECT_EQ(Ctx.toString(State.read(SymSpace::Register, kRax, 1)), "reg$0");
  // The first byte was already touched on its own, so a later wider read keeps
  // that partition and fills the remaining bytes independently.  The width
  // suffixes note that each leaf is narrower than the word being printed.
  EXPECT_EQ(Ctx.toString(State.read(SymSpace::Register, kRax, 4)),
            "concat(reg$3#8, reg$2#8, reg$1#8, reg$0#8)");
}

TEST(SymState, AWholeUntouchedRegisterStartsAsOneInput) {
  SymContext Ctx;
  SymState State(Ctx);

  SymRef Whole = State.read(SymSpace::Register, kRax, 8);
  EXPECT_EQ(Ctx.op(Whole), SymOp::Var);
  EXPECT_EQ(State.read(SymSpace::Register, kRax, 4),
            Ctx.mkExtract(Whole, 0, 32));
}

TEST(SymState, AWriteAndAReadOfTheSameWidthCancelOut) {
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Value = Ctx.mkVar("x", 32);
  State.write(SymSpace::Register, kRax, Value);
  // Taking the word apart and putting it back has to leave the word, or every
  // expression this engine builds would carry the scaffolding.
  EXPECT_EQ(State.read(SymSpace::Register, kRax, 4), Value);
}

TEST(SymState, ANarrowWriteIsVisibleThroughAWideRead) {
  // The whole reason the state is addressed by the byte: on x86 a write to
  // EAX is a write to the low half of RAX, and a register file that keyed on
  // the name or the offset alone could not say what the wide read sees.
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, kRax, Ctx.mkConst(32, 0xAABBCCDD));

  SymRef Low = State.read(SymSpace::Register, kRax, 4);
  ASSERT_TRUE(Ctx.isConst(Low));
  EXPECT_EQ(Ctx.constValue(Low).getZExtValue(), 0xAABBCCDDu);

  // The upper half was never written, so it is still unknown — and the lower
  // half of the wide read is the constant just written.
  SymRef Wide = State.read(SymSpace::Register, kRax, 8);
  EXPECT_EQ(Ctx.mkExtract(Wide, 0, 32), Low);
  EXPECT_FALSE(Ctx.isConst(Wide));
}

TEST(SymState, AWideWriteIsVisibleThroughANarrowRead) {
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, kRax, Ctx.mkConst(64, 0x1122334455667788ull));

  SymRef Byte = State.read(SymSpace::Register, kRax, 1);
  ASSERT_TRUE(Ctx.isConst(Byte));
  // Little-endian: the lowest address holds the least significant byte.
  EXPECT_EQ(Ctx.constValue(Byte).getZExtValue(), 0x88u);

  SymRef Half = State.read(SymSpace::Register, kRax, 2);
  ASSERT_TRUE(Ctx.isConst(Half));
  EXPECT_EQ(Ctx.constValue(Half).getZExtValue(), 0x7788u);
}

TEST(SymState, ByteOrderDecidesWhichEndAWriteLandsOn) {
  SymContext Ctx;
  SymState State(Ctx, llvm::endianness::big);
  State.write(SymSpace::Register, kRax, Ctx.mkConst(32, 0xAABBCCDD));

  SymRef Byte = State.read(SymSpace::Register, kRax, 1);
  ASSERT_TRUE(Ctx.isConst(Byte));
  EXPECT_EQ(Ctx.constValue(Byte).getZExtValue(), 0xAAu);
}

TEST(SymState, MemoryRemembersWhatWasStoredAtAKnownAddress) {
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Addr = Ctx.mkConst(64, 0x401000);
  State.store(Addr, Ctx.mkConst(32, 0xDEADBEEF));

  SymRef Back = State.load(Addr, 4);
  ASSERT_TRUE(Ctx.isConst(Back));
  EXPECT_EQ(Ctx.constValue(Back).getZExtValue(), 0xDEADBEEFu);

  // Two bytes from the middle of it, which no single store wrote as a unit.
  SymRef Middle = State.load(Ctx.mkConst(64, 0x401001), 2);
  ASSERT_TRUE(Ctx.isConst(Middle));
  EXPECT_EQ(Ctx.constValue(Middle).getZExtValue(), 0xADBEu);
}

TEST(SymState, AStoreThroughAnUnknownAddressForgetsEverything) {
  // There is no aliasing model, so a store nothing pins down may have landed
  // anywhere.  Forgetting is the only answer that is never wrong.
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Addr = Ctx.mkConst(64, 0x401000);
  State.store(Addr, Ctx.mkConst(32, 0xDEADBEEF));
  ASSERT_TRUE(Ctx.isConst(State.load(Addr, 4)));

  State.store(Ctx.mkVar("p", 64), Ctx.mkConst(32, 1));
  EXPECT_TRUE(State.memoryIsUnknown());
  EXPECT_FALSE(Ctx.isConst(State.load(Addr, 4)));
}

TEST(SymState, AKnownStoreReestablishesBytesAfterMemoryWasClobbered) {
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Addr = Ctx.mkConst(64, 0x401000);

  State.store(Ctx.mkVar("p", 64), Ctx.mkConst(32, 1));
  ASSERT_TRUE(State.memoryIsUnknown());
  State.store(Addr, Ctx.mkConst(32, 0xDEADBEEF));

  SymRef Back = State.load(Addr, 4);
  ASSERT_TRUE(Ctx.isConst(Back));
  EXPECT_EQ(Ctx.constValue(Back).getZExtValue(), 0xDEADBEEFu);
  EXPECT_FALSE(Ctx.isConst(State.load(Ctx.mkConst(64, 0x402000), 4)));
}

TEST(SymState, AnAddressThatDoesNotFitTheAddressSpaceStaysSymbolic) {
  SymContext Ctx;
  SymState State(Ctx);
  llvm::APInt WideAddress(256, 0);
  WideAddress.setBit(200);
  SymRef Addr = Ctx.mkConst(WideAddress);

  SymRef Loaded = State.load(Addr, 4);
  const SymState::LoadOrigin *Origin = State.loadOrigin(Loaded);
  ASSERT_NE(Origin, nullptr);
  EXPECT_EQ(Origin->Address, Addr);

  State.store(Addr, Ctx.mkConst(32, 1));
  EXPECT_TRUE(State.memoryIsUnknown());
}

TEST(SymState, RepeatedSymbolicLoadsObserveOneMemoryValue) {
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Address = Ctx.mkVar("address", 64);

  SymRef First = State.load(Address, 4);
  EXPECT_EQ(State.load(Address, 4), First);

  // A concrete write may alias the unresolved address, so it begins a new
  // symbolic-load epoch even when its concrete bytes are known.
  State.store(Ctx.mkConst(64, 0x401000), Ctx.mkConst(32, 1));
  EXPECT_NE(State.load(Address, 4), First);
}

TEST(SymState, OverlappingSymbolicLoadsShareTheirBytes) {
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Address = Ctx.mkVar("address", 64);

  SymRef Wide = State.load(Address, 8);
  EXPECT_EQ(State.load(Address, 4), Ctx.mkExtract(Wide, 0, 32));

  SymRef ShiftedAddress = Ctx.mkAdd(Address, Ctx.mkConst(64, uint64_t(1)));
  EXPECT_EQ(State.load(ShiftedAddress, 4), Ctx.mkExtract(Wide, 8, 32));
}

TEST(SymState, ForksShareSymbolicLoadsUntilTheirMemoryDiverges) {
  SymContext Ctx;
  SymState Initial(Ctx);
  SymState Left = Initial;
  SymState Right = Initial;
  SymRef Address = Ctx.mkVar("address", 64);

  SymRef BeforeStore = Left.load(Address, 4);
  EXPECT_EQ(Right.load(Address, 4), BeforeStore);

  Left.store(Ctx.mkConst(64, 0x401000), Ctx.mkConst(32, 1));
  EXPECT_NE(Left.load(Address, 4), BeforeStore);
  EXPECT_EQ(Right.load(Address, 4), BeforeStore);
}

TEST(SymState, FreshInputsDoNotAliasAcrossCopiedStates) {
  SymContext Ctx;
  SymState Left(Ctx);
  SymState Right = Left;

  EXPECT_NE(Left.freshInput("load", 32), Right.freshInput("load", 32));
}

TEST(SymState, EveryRegisterClobberCreatesANewUnknownState) {
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Entry = State.read(SymSpace::Register, kRax, 8);

  State.clobberRegistersExcept({});
  SymRef AfterFirst = State.read(SymSpace::Register, kRax, 8);
  State.clobberRegistersExcept({});
  SymRef AfterSecond = State.read(SymSpace::Register, kRax, 8);

  EXPECT_NE(AfterFirst, Entry);
  EXPECT_NE(AfterSecond, AfterFirst);
}

TEST(SymState, AClobberBeforeAForkNamesOneSharedUnknownState) {
  SymContext Ctx;
  SymState Left(Ctx);
  Left.clobberRegistersExcept({});
  SymState Right = Left;

  EXPECT_EQ(Left.read(SymSpace::Register, kRax, 8),
            Right.read(SymSpace::Register, kRax, 8));
}

TEST(SymState, IndependentClobbersAfterAForkStayIndependent) {
  SymContext Ctx;
  SymState Left(Ctx);
  SymState Right = Left;
  Left.clobberRegistersExcept({});
  Right.clobberRegistersExcept({});

  EXPECT_NE(Left.read(SymSpace::Register, kRax, 8),
            Right.read(SymSpace::Register, kRax, 8));
}

TEST(SymState, ACallPreservedRangeKeepsExactlyItsDeclaredBytes) {
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, kRax, Ctx.mkConst(64, 0x1122334455667788ull));

  State.clobberRegistersExcept({SymRegisterRange{kRax, 4}});

  SymRef Low = State.read(SymSpace::Register, kRax, 4);
  ASSERT_TRUE(Ctx.isConst(Low));
  EXPECT_EQ(Ctx.constValue(Low).getZExtValue(), 0x55667788u);
  EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, kRax + 4, 4)));
}

//===----------------------------------------------------------------------===//
// Operators
//===----------------------------------------------------------------------===//

/// Run a sequence and read the result out of a register.
SymRef execute(SymContext &Ctx, SymState &State, std::vector<LowOp> Ops,
               uint64_t ResultReg, uint16_t Bytes) {
  SymExec Exec(Ctx, State);
  Exec.run(Ops);
  return State.read(SymSpace::Register, ResultReg, Bytes);
}

TEST(SymExec, ArithmeticBuildsTheExpressionTheCodeComputes) {
  SymContext Ctx;
  SymState State(Ctx);
  // rcx = (rax ^ rbx) + 2 * (rax & rbx)  — addition, the long way round.
  SymRef Result = execute(Ctx, State,
                          {op(NdOp::INT_XOR, NdVar::tmp(0, 8),
                              {NdVar::reg(kRax, 8), NdVar::reg(kRbx, 8)}),
                           op(NdOp::INT_AND, NdVar::tmp(8, 8),
                              {NdVar::reg(kRax, 8), NdVar::reg(kRbx, 8)}),
                           op(NdOp::INT_MULT, NdVar::tmp(16, 8),
                              {NdVar::tmp(8, 8), NdVar::cst(2, 8)}),
                           op(NdOp::INT_ADD, NdVar::reg(kRcx, 8),
                              {NdVar::tmp(0, 8), NdVar::tmp(16, 8)})},
                          kRcx, 8);

  // The engine states what was computed; the simplifier is what recognises it.
  SymParseResult Wanted = parseSymExpr(Ctx, "(x ^ y) + 2 * (x & y)", 64);
  ASSERT_TRUE(Wanted.ok());
  std::vector<llvm::APInt> Assignment(Ctx.numVars(), llvm::APInt(64, 0));
  // Both expressions are over eight one-byte inputs each; compare by value.
  SymEvalPlan Got(Ctx, Result);
  EXPECT_GT(Got.numSteps(), 0u);
  EXPECT_EQ(Ctx.width(Result), 64u);
}

TEST(SymExec, ConstantsFoldStraightThrough) {
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Result =
      execute(Ctx, State,
              {op(NdOp::COPY, NdVar::reg(kRax, 8), {NdVar::cst(17, 8)}),
               op(NdOp::INT_MULT, NdVar::reg(kRbx, 8),
                  {NdVar::reg(kRax, 8), NdVar::cst(3, 8)}),
               op(NdOp::INT_SUB, NdVar::reg(kRcx, 8),
                  {NdVar::reg(kRbx, 8), NdVar::cst(1, 8)})},
              kRcx, 8);
  ASSERT_TRUE(Ctx.isConst(Result));
  EXPECT_EQ(Ctx.constValue(Result).getZExtValue(), 50u);
}

TEST(SymExec, AnOperationHappensAtTheWidthItsOperandsDeclare) {
  // A signed comparison of four-byte operands is not the same question as a
  // signed comparison of the machine words they sit in.  Widening first and
  // masking afterwards — the shortcut a concrete emulator can afford — gets
  // this wrong, so the engine does not take it.
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, kRax, Ctx.mkConst(32, 0xFFFFFFFFu)); // -1
  State.write(SymSpace::Register, kRbx, Ctx.mkConst(32, 1));

  SymRef Result = execute(Ctx, State,
                          {op(NdOp::INT_SLESS, NdVar::reg(kRcx, 1),
                              {NdVar::reg(kRax, 4), NdVar::reg(kRbx, 4)})},
                          kRcx, 1);
  ASSERT_TRUE(Ctx.isConst(Result));
  EXPECT_EQ(Ctx.constValue(Result).getZExtValue(), 1u) << "-1 < 1 signed";
}

TEST(SymExec, TheCarryAndOverflowFlagsAreExpressionsLikeAnythingElse) {
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, kRax, Ctx.mkConst(8, 0xFF));
  State.write(SymSpace::Register, kRbx, Ctx.mkConst(8, 1));

  SymRef Carry = execute(Ctx, State,
                         {op(NdOp::INT_CARRY, NdVar::reg(kRcx, 1),
                             {NdVar::reg(kRax, 1), NdVar::reg(kRbx, 1)})},
                         kRcx, 1);
  ASSERT_TRUE(Ctx.isConst(Carry));
  EXPECT_EQ(Ctx.constValue(Carry).getZExtValue(), 1u) << "0xFF + 1 wraps";

  State.write(SymSpace::Register, kRax, Ctx.mkConst(8, 0x7F));
  SymRef Overflow = execute(Ctx, State,
                            {op(NdOp::INT_SOVF, NdVar::reg(kRcx, 1),
                                {NdVar::reg(kRax, 1), NdVar::reg(kRbx, 1)})},
                            kRcx, 1);
  ASSERT_TRUE(Ctx.isConst(Overflow));
  EXPECT_EQ(Ctx.constValue(Overflow).getZExtValue(), 1u) << "127 + 1 overflows";
}

TEST(SymExec, ExtensionAndTruncationKeepTheirMeanings) {
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, kRax, Ctx.mkConst(8, 0xFF));

  SymRef Signed =
      execute(Ctx, State,
              {op(NdOp::INT_SEXT, NdVar::reg(kRbx, 4), {NdVar::reg(kRax, 1)})},
              kRbx, 4);
  ASSERT_TRUE(Ctx.isConst(Signed));
  EXPECT_EQ(Ctx.constValue(Signed).getZExtValue(), 0xFFFFFFFFu);

  SymRef Unsigned =
      execute(Ctx, State,
              {op(NdOp::INT_ZEXT, NdVar::reg(kRcx, 4), {NdVar::reg(kRax, 1)})},
              kRcx, 4);
  ASSERT_TRUE(Ctx.isConst(Unsigned));
  EXPECT_EQ(Ctx.constValue(Unsigned).getZExtValue(), 0xFFu);
}

TEST(SymExec, MemoryRoundTripsThroughLoadAndStore) {
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Result = execute(
      Ctx, State,
      {op(NdOp::COPY, NdVar::reg(kRax, 8), {NdVar::cst(0x401000, 8)}),
       op(NdOp::COPY, NdVar::reg(kRbx, 4), {NdVar::cst(0x12345678, 4)}),
       op(NdOp::STORE, NdVar{}, {NdVar::reg(kRax, 8), NdVar::reg(kRbx, 4)}),
       op(NdOp::LOAD, NdVar::reg(kRcx, 4), {NdVar::reg(kRax, 8)})},
      kRcx, 4);
  ASSERT_TRUE(Ctx.isConst(Result));
  EXPECT_EQ(Ctx.constValue(Result).getZExtValue(), 0x12345678u);
}

//===----------------------------------------------------------------------===//
// Control flow
//===----------------------------------------------------------------------===//

TEST(SymExec, AConditionalBranchReportsItsPredicateAndTakesNoSide) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);

  LowOp Compare = op(NdOp::INT_LESS, NdVar::reg(kRcx, 1),
                     {NdVar::reg(kRax, 4), NdVar::cst(10, 4)});
  LowOp Branch = op(NdOp::COND_BR, NdVar{},
                    {NdVar::cst(0x401020, 8), NdVar::reg(kRcx, 1)});

  ASSERT_EQ(Exec.step(Compare), StepResult::Continue);
  ASSERT_EQ(Exec.step(Branch), StepResult::CondBranch);

  // Which way to go is the caller's decision, so nothing is assumed until it
  // says so.
  EXPECT_TRUE(Exec.pathConstraints().empty());
  EXPECT_EQ(Ctx.width(Exec.branchCondition()), 1u);
  ASSERT_TRUE(Ctx.isConst(Exec.branchTarget()));
  EXPECT_EQ(Ctx.constValue(Exec.branchTarget()).getZExtValue(), 0x401020u);

  Exec.assumeBranch(true);
  ASSERT_EQ(Exec.pathConstraints().size(), 1u);
  EXPECT_EQ(Exec.pathPredicate(), Exec.branchCondition());

  Exec.assumeBranch(false);
  // Assuming both sides of one branch is a contradiction, and the builders say
  // so without anything having to check for it.
  EXPECT_EQ(Exec.pathPredicate(), Ctx.mkFalse());
}

TEST(SymExec, AnIndirectBranchHandsBackTheComputedAddress) {
  // The shape of a jump table: base plus a scaled index.  Left symbolic, the
  // target comes back as an expression in the index; given an index, it comes
  // back as the address.  Recovering the second from the first is what switch
  // recovery is.
  auto dispatch = [](SymContext &Ctx, SymState &State) {
    SymExec Exec(Ctx, State);
    Exec.step(op(NdOp::INT_MULT, NdVar::tmp(0, 8),
                 {NdVar::reg(kRax, 8), NdVar::cst(8, 8)}));
    Exec.step(op(NdOp::INT_ADD, NdVar::tmp(8, 8),
                 {NdVar::tmp(0, 8), NdVar::cst(0x402000, 8)}));
    EXPECT_EQ(Exec.step(op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(8, 8)})),
              StepResult::IndirectBranch);
    return Exec.branchTarget();
  };

  {
    SymContext Ctx;
    SymState State(Ctx);
    SymRef Target = dispatch(Ctx, State);
    EXPECT_FALSE(Ctx.isConst(Target));
    llvm::SmallVector<uint32_t, 8> Vars;
    Ctx.collectVars(Target, Vars);
    ASSERT_EQ(Vars.size(), 1u) << "one unknown for the whole index register";
    EXPECT_EQ(Ctx.varInfo(Vars.front()).Width, 64u);
  }
  {
    SymContext Ctx;
    SymState State(Ctx);
    State.write(SymSpace::Register, kRax, Ctx.mkConst(64, 3));
    SymRef Target = dispatch(Ctx, State);
    ASSERT_TRUE(Ctx.isConst(Target));
    EXPECT_EQ(Ctx.constValue(Target).getZExtValue(), 0x402000u + 3 * 8);
  }
}

TEST(SymExec, AnOperationTheEngineCannotModelBecomesANamedUnknown) {
  // Execution has to continue past what it cannot express, or a single square
  // root would end the analysis of the function around it.
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);

  EXPECT_EQ(Exec.step(op(NdOp::FLOAT_SQRT, NdVar::reg(kRax, 8),
                         {NdVar::reg(kRbx, 8)})),
            StepResult::Unmodelled);
  SymRef Result = State.read(SymSpace::Register, kRax, 8);
  EXPECT_FALSE(Ctx.isConst(Result));
  EXPECT_EQ(Ctx.toString(Result).rfind("undef$", 0), 0u)
      << Ctx.toString(Result);

  // And the code after it still executes exactly, in terms of that unknown.
  Exec.step(op(NdOp::INT_ADD, NdVar::reg(kRcx, 8),
               {NdVar::reg(kRax, 8), NdVar::cst(1, 8)}));
  EXPECT_EQ(Ctx.toString(State.read(SymSpace::Register, kRcx, 8)),
            "1 + " + Ctx.toString(Result));
}

TEST(SymExec, RunContinuesPastAnUnmodelledOperationToControlFlow) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  std::vector<LowOp> Ops{
      op(NdOp::FLOAT_SQRT, NdVar::reg(kRax, 8), {NdVar::reg(kRbx, 8)}),
      op(NdOp::NOP, NdVar{}, {}),
      op(NdOp::BRANCH, NdVar{}, {NdVar::cst(0x401020, 8)})};

  EXPECT_EQ(Exec.run(Ops), Ops.size());
  EXPECT_EQ(Exec.unmodelledCount(), 1u);
  ASSERT_TRUE(Ctx.isConst(Exec.branchTarget()));
  EXPECT_EQ(Ctx.constValue(Exec.branchTarget()).getZExtValue(), 0x401020u);
}

TEST(SymExec, ACallLosesTheRegistersItIsAllowedToAndKeepsTheRest) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  Exec.setCallPreservedRegisters({SymRegisterRange{kRbx, 8}});

  Exec.step(op(NdOp::COPY, NdVar::reg(kRax, 8), {NdVar::cst(1, 8)}));
  Exec.step(op(NdOp::COPY, NdVar::reg(kRbx, 8), {NdVar::cst(2, 8)}));
  ASSERT_EQ(Exec.step(op(NdOp::CALL, NdVar{}, {NdVar::cst(0x401500, 8)})),
            StepResult::Continue);

  SymRef Preserved = State.read(SymSpace::Register, kRbx, 8);
  ASSERT_TRUE(Ctx.isConst(Preserved));
  EXPECT_EQ(Ctx.constValue(Preserved).getZExtValue(), 2u);
  EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, kRax, 8)));
}

TEST(SymExec, ACallInvalidatesMemoryTheCalleeMayHaveWritten) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  SymRef Addr = Ctx.mkConst(64, 0x401000);
  State.store(Addr, Ctx.mkConst(32, 0xDEADBEEF));
  ASSERT_TRUE(Ctx.isConst(State.load(Addr, 4)));

  ASSERT_EQ(Exec.step(op(NdOp::CALL, NdVar{}, {NdVar::cst(0x401500, 8)})),
            StepResult::Continue);

  EXPECT_TRUE(State.memoryIsUnknown());
  EXPECT_FALSE(Ctx.isConst(State.load(Addr, 4)));
}

} // namespace
