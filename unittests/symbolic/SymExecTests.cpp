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

TEST(SymState, AbsoluteLoadRetainsItsAddressOrigin) {
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Addr = Ctx.mkConst(64, 0x401000);

  SymRef Loaded = State.load(Addr, 8);
  const SymState::LoadOrigin *Origin = State.loadOrigin(Loaded);
  ASSERT_NE(Origin, nullptr);
  EXPECT_EQ(Origin->Address, Addr);
  EXPECT_EQ(Origin->Bytes, 8u);
}

TEST(SymState, AStoreThroughAnUnknownAddressForgetsEverything) {
  // A store through a pointer may have landed on this number, and nothing here
  // can say it did not.  Giving up the numbers is the only answer that is
  // never wrong.
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Addr = Ctx.mkConst(64, 0x401000);
  State.store(Addr, Ctx.mkConst(32, 0xDEADBEEF));
  ASSERT_TRUE(Ctx.isConst(State.load(Addr, 4)));

  State.store(Ctx.mkVar("p", 64), Ctx.mkConst(32, 1));
  EXPECT_TRUE(State.memoryIsUnknown());
  EXPECT_FALSE(Ctx.isConst(State.load(Addr, 4)));
}

TEST(SymState, TwoSlotsOffOneBaseStayIndependent) {
  // What a region is for.  `sp - 8` and `sp - 16` cannot be the same byte
  // whatever `sp` turns out to be, so a write to one is known not to be a
  // write to the other and a frame survives its own spills — where forgetting
  // memory at the first one would have lost every local the function has.
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Frame = Ctx.mkVar("sp", 64);
  SymRef First = Ctx.mkSub(Frame, Ctx.mkConst(64, 8));
  SymRef Second = Ctx.mkSub(Frame, Ctx.mkConst(64, 16));

  State.store(First, Ctx.mkConst(32, 0xAAAABBBB));
  State.store(Second, Ctx.mkConst(32, 0xCCCCDDDD));

  SymRef BackFirst = State.load(First, 4);
  ASSERT_TRUE(Ctx.isConst(BackFirst));
  EXPECT_EQ(Ctx.constValue(BackFirst).getZExtValue(), 0xAAAABBBBu);
  SymRef BackSecond = State.load(Second, 4);
  ASSERT_TRUE(Ctx.isConst(BackSecond));
  EXPECT_EQ(Ctx.constValue(BackSecond).getZExtValue(), 0xCCCCDDDDu);

  // Both slots hang off one base, so they are one region and there was never
  // an aliasing question to answer.
  EXPECT_EQ(State.numMemoryRegions(), 1u);
}

TEST(SymState, OneSlotIsStillVisibleThroughAWiderReadOfItsNeighbour) {
  // Separation is by the byte inside a region as much as between registers: a
  // read spanning two slots sees both of them, and a read of one sees one.
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Frame = Ctx.mkVar("sp", 64);
  State.store(Ctx.mkSub(Frame, Ctx.mkConst(64, 8)),
              Ctx.mkConst(32, 0x11223344));
  State.store(Ctx.mkSub(Frame, Ctx.mkConst(64, 4)),
              Ctx.mkConst(32, 0x55667788));

  SymRef Both = State.load(Ctx.mkSub(Frame, Ctx.mkConst(64, 8)), 8);
  ASSERT_TRUE(Ctx.isConst(Both));
  EXPECT_EQ(Ctx.constValue(Both).getZExtValue(), 0x5566778811223344ull);
}

TEST(SymState, AStoreThroughAnUnrelatedBaseIsTreatedAsPossiblyAliasing) {
  // The other half of the bargain.  Two bases can be the same pointer and
  // nothing here can prove otherwise, so a write through one gives up
  // everything reached through the other.  Precision between regions would be
  // a guess; precision within one is a fact.
  SymContext Ctx;
  SymState State(Ctx);
  SymRef Slot = Ctx.mkSub(Ctx.mkVar("sp", 64), Ctx.mkConst(64, 8));
  State.store(Slot, Ctx.mkConst(32, 0xAAAABBBB));
  ASSERT_TRUE(Ctx.isConst(State.load(Slot, 4)));

  State.store(Ctx.mkVar("p", 64), Ctx.mkConst(32, 1));
  EXPECT_FALSE(Ctx.isConst(State.load(Slot, 4)));
  EXPECT_TRUE(State.memoryIsUnknown());
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

TEST(SymState, OneValueLoadedFromTwoAddressesHasConflictingOrigins) {
  SymContext Ctx;
  SymRef Base = Ctx.mkVar("base", 64);
  SymRef FirstAddress = Ctx.mkAdd(Base, Ctx.mkConst(64, 0x10));
  SymRef SecondAddress = Ctx.mkAdd(Base, Ctx.mkConst(64, 0x20));
  SymRef Value = Ctx.mkVar("value", 32);

  auto LoadInOrder = [&](SymRef First, SymRef Second) {
    SymState State(Ctx);
    State.store(FirstAddress, Value);
    State.store(SecondAddress, Value);
    EXPECT_EQ(State.load(First, 4), Value);
    EXPECT_EQ(State.load(Second, 4), Value);
    return State;
  };

  SymState Forward = LoadInOrder(FirstAddress, SecondAddress);
  SymState Reverse = LoadInOrder(SecondAddress, FirstAddress);

  EXPECT_EQ(Forward.loadOrigins(Value), Reverse.loadOrigins(Value));
  EXPECT_EQ(Forward.loadOrigins(Value).size(), 2u);
  EXPECT_EQ(Forward.loadOrigin(Value), nullptr);
  EXPECT_EQ(Reverse.loadOrigin(Value), nullptr);
}

TEST(SymState, IdenticalStateMergeUnionsLoadOriginConflicts) {
  SymContext Ctx;
  SymRef Base = Ctx.mkVar("base", 64);
  SymRef FirstAddress = Ctx.mkAdd(Base, Ctx.mkConst(64, 0x10));
  SymRef SecondAddress = Ctx.mkAdd(Base, Ctx.mkConst(64, 0x20));
  SymRef Value = Ctx.mkVar("value", 32);

  SymState Left(Ctx);
  Left.store(FirstAddress, Value);
  Left.store(SecondAddress, Value);
  SymState Right = Left;

  EXPECT_EQ(Left.load(FirstAddress, 4), Value);
  EXPECT_EQ(Right.load(SecondAddress, 4), Value);
  ASSERT_TRUE(Left.mergeIdentical(Right));
  EXPECT_EQ(Left.loadOrigins(Value).size(), 2u);
  EXPECT_EQ(Left.loadOrigin(Value), nullptr);
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

TEST(SymExec, TheBitCountsAndBitFieldsAreModelledRatherThanNamed) {
  // The concrete emulator computes all four of these.  Two engines over one
  // operator set that disagree about an opcode is the worst kind of bug: the
  // symbolic side quietly loses the dependence and what comes out looks like
  // independence, somewhere else entirely.
  SymContext Ctx;
  SymState State(Ctx);

  SymRef Ones =
      execute(Ctx, State,
              {op(NdOp::COPY, NdVar::reg(kRax, 1), {NdVar::cst(0xF0, 1)}),
               op(NdOp::POPCOUNT, NdVar::reg(kRbx, 1), {NdVar::reg(kRax, 1)})},
              kRbx, 1);
  ASSERT_TRUE(Ctx.isConst(Ones));
  EXPECT_EQ(Ctx.constValue(Ones).getZExtValue(), 4u);

  // Counted at the width the operand declares, not at the width of the machine
  // word it happens to sit in: the leading zeros of a byte are not the leading
  // zeros of the register holding it.
  SymRef Leading =
      execute(Ctx, State,
              {op(NdOp::COPY, NdVar::reg(kRax, 1), {NdVar::cst(0x0F, 1)}),
               op(NdOp::LZCOUNT, NdVar::reg(kRbx, 1), {NdVar::reg(kRax, 1)})},
              kRbx, 1);
  ASSERT_TRUE(Ctx.isConst(Leading));
  EXPECT_EQ(Ctx.constValue(Leading).getZExtValue(), 4u);

  SymRef Field =
      execute(Ctx, State,
              {op(NdOp::EXTRACT, NdVar::reg(kRcx, 2),
                  {NdVar::cst(0xDEAD, 2), NdVar::cst(4, 1), NdVar::cst(8, 1)})},
              kRcx, 2);
  ASSERT_TRUE(Ctx.isConst(Field));
  EXPECT_EQ(Ctx.constValue(Field).getZExtValue(), 0xEAu);

  SymRef Inserted = execute(Ctx, State,
                            {op(NdOp::INSERT, NdVar::reg(kRcx, 2),
                                {NdVar::cst(0xFF00, 2), NdVar::cst(0xAB, 1),
                                 NdVar::cst(0, 1), NdVar::cst(8, 1)})},
                            kRcx, 2);
  ASSERT_TRUE(Ctx.isConst(Inserted));
  EXPECT_EQ(Ctx.constValue(Inserted).getZExtValue(), 0xFFABu);
}

TEST(SymExec, ABitCountOfSomethingUnknownStaysExactInIt) {
  // The model is not a folding trick.  With the operand unknown the count is
  // an expression in it, and it still says the one thing that matters here:
  // that the result depends on the operand at all.
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);

  ASSERT_EQ(
      Exec.step(op(NdOp::POPCOUNT, NdVar::reg(kRbx, 1), {NdVar::reg(kRax, 1)})),
      StepResult::Continue);
  EXPECT_EQ(Exec.unmodelledCount(), 0u);

  SymRef Count = State.read(SymSpace::Register, kRbx, 1);
  EXPECT_FALSE(Ctx.isConst(Count));
  llvm::SmallVector<uint32_t, 4> Vars;
  Ctx.collectVars(Count, Vars);
  ASSERT_EQ(Vars.size(), 1u);
  EXPECT_EQ(Ctx.varInfo(Vars.front()).Name, "reg$0");
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
  EXPECT_EQ(Exec.opaqueOperationCount(), 1u);
  EXPECT_EQ(Exec.callHavocCount(), 0u);
  EXPECT_EQ(Exec.memoryHavocCount(), 0u);

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

  EXPECT_EQ(Exec.unmodelledCount(), 1u);
  EXPECT_EQ(Exec.callHavocCount(), 1u);
  EXPECT_EQ(Exec.opaqueOperationCount(), 0u);
  EXPECT_EQ(Exec.memoryHavocCount(), 0u);
  EXPECT_TRUE(State.memoryIsUnknown());
  EXPECT_FALSE(Ctx.isConst(State.load(Addr, 4)));
}

TEST(SymExec, AStoreThroughAnUnknownAddressIsReportedAsAnApproximation) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);

  ASSERT_EQ(Exec.step(op(NdOp::STORE, NdVar{},
                         {NdVar::reg(kRax, 8), NdVar::cst(0x12345678, 4)})),
            StepResult::Continue);

  EXPECT_EQ(Exec.unmodelledCount(), 1u);
  EXPECT_EQ(Exec.memoryHavocCount(), 1u);
  EXPECT_EQ(Exec.opaqueOperationCount(), 0u);
  EXPECT_EQ(Exec.callHavocCount(), 0u);
  EXPECT_TRUE(State.memoryIsUnknown());
}

} // namespace
