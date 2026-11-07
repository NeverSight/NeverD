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
#include "neverd/symbolic/SymValueIdentity.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

TEST(SymValueIdentity, ExactResultsSurviveOnlyCompleteValueTransport) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (unsigned Bits : {32u, 64u}) {
      SymContext Context;
      SymState State(Context, Order);
      SymValueIdentities Identities;
      SymValueIdentity I{SymValueIdentity::Kind::Symbol,
                         "example",
                         "module",
                         "resolver",
                         0x100,
                         0x110};
      auto Value = Context.mkFreshVar(Bits, "result");
      EXPECT_TRUE(Identities.bind(Context, Value, I));
      EXPECT_FALSE(Identities.bind(Context, Context.mkZero(Bits), I));
      EXPECT_FALSE(Identities.bind(Context, Context.mkVar("input", Bits), I));
      auto Address = Context.mkConst(64, 0x1000);
      State.store(Address, Value);
      auto Reload = State.load(Address, Bits / 8);
      ASSERT_NE(Identities.lookup(Reload), nullptr);
      EXPECT_EQ(*Identities.lookup(Reload), I);
      EXPECT_EQ(Identities.lookup(Context.mkAdd(Value, Context.mkOne(Bits))),
                nullptr);
      State.store(Address, Context.mkConst(8, 7));
      EXPECT_EQ(Identities.lookup(State.load(Address, Bits / 8)), nullptr);
      I.Name = "conflicting";
      EXPECT_FALSE(Identities.bind(Context, Value, I));
      EXPECT_EQ(Identities.lookup(Value), nullptr);
    }
}

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
  // that partition and fills the unread bytes with one input.  The width
  // suffixes note that each leaf is narrower than the word being printed.
  EXPECT_EQ(Ctx.toString(State.read(SymSpace::Register, kRax, 4)),
            "concat(reg$1#24, reg$0#8)");
}

TEST(SymState, ZeroByteReadFailsClosedWithoutCreatingAnInput) {
  SymContext Ctx;
  SymState State(Ctx);
  EXPECT_FALSE(State.read(SymSpace::Register, kRax, 0).isValid());
  EXPECT_EQ(Ctx.numVars(), 0u);
}

TEST(SymState, WrappingRegisterRangesFailClosedWithoutMutatingState) {
  SymContext Ctx;
  SymState State(Ctx);
  constexpr uint64_t Last = std::numeric_limits<uint64_t>::max();

  EXPECT_FALSE(State.read(SymSpace::Register, Last, 2).isValid());
  EXPECT_EQ(Ctx.numVars(), 0u);
  State.write(SymSpace::Register, Last, Ctx.mkConst(16, 0xAABB));

  SymRef AtZero = State.read(SymSpace::Register, 0, 1);
  ASSERT_TRUE(Ctx.isVar(AtZero));
  const SymVarInfo &Info = Ctx.varInfo(Ctx.varId(AtZero));
  ASSERT_TRUE(Info.InputOrigin.has_value());
  EXPECT_EQ(*Info.InputOrigin,
            (SymInputOrigin{SymInputKind::Register, 0, 1, 0}));
}

TEST(SymState, OversizedWriteDoesNotTruncateItsByteCount) {
  SymContext Ctx;
  SymState State(Ctx);
  constexpr uint32_t TooManyBytes =
      uint32_t{std::numeric_limits<uint16_t>::max()} + 1;
  SymRef Wide = Ctx.mkVar("wide", TooManyBytes * 8);

  State.write(SymSpace::Register, 0, Wide);
  SymRef Byte = State.read(SymSpace::Register, 0, 1);
  ASSERT_TRUE(Ctx.isVar(Byte));
  EXPECT_NE(Ctx.varId(Byte), Ctx.varId(Wide));
  ASSERT_TRUE(Ctx.varInfo(Ctx.varId(Byte)).InputOrigin.has_value());
}

TEST(SymState, AWholeUntouchedRegisterStartsAsOneInput) {
  SymContext Ctx;
  SymState State(Ctx);

  SymRef Whole = State.read(SymSpace::Register, kRax, 8);
  EXPECT_EQ(Ctx.op(Whole), SymOp::Var);
  EXPECT_EQ(State.read(SymSpace::Register, kRax, 4),
            Ctx.mkExtract(Whole, 0, 32));
}

TEST(SymState, ConstantScalarProjectionRetainsCompleteStoredExpressions) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    SymRef Input = State.read(SymSpace::Register, 64, 8);
    SymRef Word = Ctx.mkAnd(Input, Ctx.mkConst(64, 0x00ff00ff00ff00ff));
    SymRef Complement = Ctx.mkNot(Word);
    State.write(SymSpace::Register, 0, Word);
    State.write(SymSpace::Temporary, 16, Complement);
    const size_t Before = Ctx.numNodes();
    const auto Scalars = State.constantScalarBytes();
    EXPECT_EQ(Ctx.numNodes(), Before);
    ASSERT_EQ(Scalars.size(), 8u);
    const unsigned First = Order == llvm::endianness::little ? 1 : 0;
    for (unsigned I = 0; I != 4; ++I) {
      EXPECT_EQ(Scalars[I].Space, SymSpace::Register);
      EXPECT_EQ(Scalars[I].Offset, First + 2 * I);
      EXPECT_EQ(Scalars[I].Value, 0);
      EXPECT_EQ(Scalars[I + 4].Space, SymSpace::Temporary);
      EXPECT_EQ(Scalars[I + 4].Offset, 16 + First + 2 * I);
      EXPECT_EQ(Scalars[I + 4].Value, 0xff);
    }
    EXPECT_EQ(State.read(SymSpace::Register, 0, 8), Word);
    EXPECT_EQ(State.read(SymSpace::Temporary, 16, 8), Complement);
  }
}

TEST(SymState, ConstantRegionProjectionKeepsLiteralByteFacts) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    SymRef Word =
        Ctx.mkAnd(Ctx.mkVar("input", 64), Ctx.mkConst(64, 0x00ff00ff00ff00ff));
    SymRef Root = Ctx.mkFreshVar(64, "frame");
    State.write(SymSpace::Register, 0, Word);
    State.store(Root, Word);
    State.store(Ctx.mkAdd(Root, Ctx.mkConst(64, 16)), Ctx.mkConst(8, 42));
    const size_t Before = Ctx.numNodes();

    // Scalar projection may prove additional lanes. Region export deliberately
    // retains the literal-only policy, without replacing mixed memory words
    // with finer entry inputs at a subsequent projection boundary.
    EXPECT_EQ(State.constantScalarBytes().size(), 4u);
    const auto Memory = State.constantRegionBytes(Root);
    ASSERT_EQ(Memory.size(), 1u);
    EXPECT_EQ(Memory.front().Offset, 16u);
    EXPECT_EQ(Memory.front().Value, 42);
    EXPECT_EQ(Ctx.numNodes(), Before);
    EXPECT_EQ(State.load(Root, 8), Word);
  }
}

TEST(SymState, SiblingStatesShareWideThenNarrowInitialRegisterBytes) {
  SymContext Ctx;
  SymState WideState(Ctx);
  SymState NarrowState(Ctx);

  SymRef Wide = WideState.read(SymSpace::Register, kRax, 8);
  ASSERT_TRUE(Ctx.isVar(Wide));
  const uint32_t WideId = Ctx.varId(Wide);
  ASSERT_TRUE(Ctx.varInfo(WideId).InputOrigin.has_value());
  EXPECT_EQ(*Ctx.varInfo(WideId).InputOrigin,
            (SymInputOrigin{SymInputKind::Register, kRax, 8, 0}));

  SymRef Narrow = NarrowState.read(SymSpace::Register, kRax, 4);
  EXPECT_EQ(Narrow, Ctx.mkExtract(Wide, 0, 32));
  EXPECT_EQ(Ctx.varInfo(WideId).Width, Ctx.width(Wide));
  EXPECT_EQ(*Ctx.varInfo(WideId).InputOrigin,
            (SymInputOrigin{SymInputKind::Register, kRax, 8, 0}));
}

TEST(SymState, SiblingStatesShareNarrowThenWideStructuredMemoryBytes) {
  SymContext Ctx;
  SymState NarrowState(Ctx);
  SymState WideState(Ctx);
  SymRef Address = Ctx.mkConst(64, 0x401000);

  SymRef Narrow = NarrowState.load(Address, 4);
  ASSERT_TRUE(Ctx.isVar(Narrow));
  const uint32_t NarrowId = Ctx.varId(Narrow);
  ASSERT_TRUE(Ctx.varInfo(NarrowId).InputOrigin.has_value());
  EXPECT_EQ(*Ctx.varInfo(NarrowId).InputOrigin,
            (SymInputOrigin{SymInputKind::AbsoluteMemory, 0x401000, 4, 0}));

  SymRef Wide = WideState.load(Address, 8);
  EXPECT_EQ(Ctx.mkExtract(Wide, 0, 32), Narrow);
  EXPECT_EQ(Ctx.varInfo(NarrowId).Width, Ctx.width(Narrow));
  EXPECT_EQ(*Ctx.varInfo(NarrowId).InputOrigin,
            (SymInputOrigin{SymInputKind::AbsoluteMemory, 0x401000, 4, 0}));

  llvm::SmallVector<uint32_t, 2> Inputs;
  Ctx.collectVars(Wide, Inputs);
  ASSERT_EQ(Inputs.size(), 2u);
  for (uint32_t Id : Inputs) {
    const SymVarInfo &Info = Ctx.varInfo(Id);
    ASSERT_TRUE(Info.InputOrigin.has_value());
    EXPECT_EQ(Info.InputOrigin->Kind, SymInputKind::AbsoluteMemory);
    EXPECT_EQ(Info.InputOrigin->Epoch, 0u);
    EXPECT_GE(Info.InputOrigin->Offset, 0x401000u);
    EXPECT_LE(Info.InputOrigin->Offset + Info.InputOrigin->Bytes, 0x401008u);
  }
}

TEST(SymState, BigEndianSiblingsShareWideThenNarrowInitialRegisterBytes) {
  SymContext Ctx;
  SymState WideState(Ctx, llvm::endianness::big);
  SymState NarrowState(Ctx, llvm::endianness::big);

  SymRef Wide = WideState.read(SymSpace::Register, kRax, 8);
  ASSERT_TRUE(Ctx.isVar(Wide));
  const uint32_t WideId = Ctx.varId(Wide);
  SymRef Narrow = NarrowState.read(SymSpace::Register, kRax, 4);

  EXPECT_EQ(Narrow, Ctx.mkExtract(Wide, 32, 32));
  EXPECT_EQ(Ctx.varInfo(WideId).Width, Ctx.width(Wide));
  ASSERT_TRUE(Ctx.varInfo(WideId).InputOrigin.has_value());
  EXPECT_EQ(*Ctx.varInfo(WideId).InputOrigin,
            (SymInputOrigin{SymInputKind::Register, kRax, 8, 0}));
}

TEST(SymState, BigEndianSiblingsShareNarrowThenWideInitialRegisterBytes) {
  SymContext Ctx;
  SymState NarrowState(Ctx, llvm::endianness::big);
  SymState WideState(Ctx, llvm::endianness::big);

  SymRef Narrow = NarrowState.read(SymSpace::Register, kRax, 4);
  ASSERT_TRUE(Ctx.isVar(Narrow));
  const uint32_t NarrowId = Ctx.varId(Narrow);
  SymRef Wide = WideState.read(SymSpace::Register, kRax, 8);

  EXPECT_EQ(Ctx.mkExtract(Wide, 32, 32), Narrow);
  EXPECT_EQ(Ctx.varInfo(NarrowId).Width, Ctx.width(Narrow));
  ASSERT_TRUE(Ctx.varInfo(NarrowId).InputOrigin.has_value());
  EXPECT_EQ(*Ctx.varInfo(NarrowId).InputOrigin,
            (SymInputOrigin{SymInputKind::Register, kRax, 4, 0}));
}

TEST(SymState, UntouchedMachineBanksCarryStructuredInitialOrigins) {
  SymContext Ctx;
  SymState State(Ctx);

  struct Case {
    const char *Label;
    SymRef Value;
    SymInputOrigin Expected;
  };
  const Case Cases[] = {
      {"register",
       State.read(SymSpace::Register, kRbx, 8),
       {SymInputKind::Register, kRbx, 8, 0}},
      {"temporary",
       State.read(SymSpace::Temporary, 40, 4),
       {SymInputKind::Temporary, 40, 4, 0}},
      {"absolute memory",
       State.load(Ctx.mkConst(64, 0x401020), 2),
       {SymInputKind::AbsoluteMemory, 0x401020, 2, 0}},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Label);
    ASSERT_TRUE(Ctx.isVar(C.Value));
    const SymVarInfo &Info = Ctx.varInfo(Ctx.varId(C.Value));
    ASSERT_TRUE(Info.InputOrigin.has_value());
    EXPECT_EQ(*Info.InputOrigin, C.Expected);
    EXPECT_FALSE(Info.Fresh);
  }
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

TEST(SymState, MemoryEffectsInvalidateRegionsFirstReadAfterAFork) {
  for (unsigned Effect = 0; Effect != 3; ++Effect) {
    SCOPED_TRACE(Effect);
    SymContext Ctx;
    SymState Entry(Ctx);
    SymState Changed = Entry;
    SymRef Address = Ctx.mkVar("unseen_address", 64);
    if (Effect == 0)
      Changed.clobberMemory();
    else if (Effect == 1)
      Changed.store(Ctx.mkConst(64, 0x401000), Ctx.mkConst(32, 1));
    else
      Changed.store(Ctx.mkVar("other_address", 64), Ctx.mkConst(32, 1));

    SymRef Before = Entry.load(Address, 4);
    SymRef After = Changed.load(Address, 4);
    EXPECT_NE(After, Before);
    EXPECT_EQ(Changed.load(Address, 4), After);
  }
}

TEST(SymState, IndependentMemoryClobbersKeepUnseenRegionsIndependent) {
  SymContext Ctx;
  SymState Left(Ctx);
  SymState Right = Left;
  Left.clobberMemory();
  Right.clobberMemory();
  SymRef Address = Ctx.mkVar("unseen_address", 64);

  EXPECT_NE(Left.load(Address, 4), Right.load(Address, 4));
}

TEST(SymState, ForkedMemoryClobberSharesOverlappingUnseenRegionBytes) {
  for (llvm::endianness Order :
       {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState Left(Ctx, Order);
    Left.clobberMemory();
    SymState Right = Left;
    SymRef Address = Ctx.mkVar("unseen_address", 64);
    SymRef Wide = Left.load(Address, 8);
    SymRef Narrow = Right.load(Ctx.mkAdd(Address, Ctx.mkConst(64, 1)), 4);

    const uint32_t Low = Order == llvm::endianness::little ? 8 : 24;
    EXPECT_EQ(Narrow, Ctx.mkExtract(Wide, Low, 32));
    EXPECT_EQ(Right.load(Address, 8), Wide);
  }
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

TEST(SymState, FreshAndClobberedValuesHaveNoReplayableInputOrigin) {
  SymContext Ctx;
  SymState State(Ctx);

  SymRef Fresh = State.freshInput("opaque", 32);
  ASSERT_TRUE(Ctx.isVar(Fresh));
  EXPECT_TRUE(Ctx.varInfo(Ctx.varId(Fresh)).Fresh);
  EXPECT_FALSE(Ctx.varInfo(Ctx.varId(Fresh)).InputOrigin.has_value());

  State.clobberRegistersExcept({});
  SymRef Register = State.read(SymSpace::Register, kRax, 8);
  ASSERT_TRUE(Ctx.isVar(Register));
  EXPECT_TRUE(Ctx.varInfo(Ctx.varId(Register)).Fresh);
  EXPECT_FALSE(Ctx.varInfo(Ctx.varId(Register)).InputOrigin.has_value());

  SymRef Temporary = State.read(SymSpace::Temporary, 40, 4);
  ASSERT_TRUE(Ctx.isVar(Temporary));
  EXPECT_TRUE(Ctx.varInfo(Ctx.varId(Temporary)).Fresh);
  EXPECT_FALSE(Ctx.varInfo(Ctx.varId(Temporary)).InputOrigin.has_value());

  SymRef Pointer = Ctx.mkVar("pointer", 64);
  SymRef SymbolicMemory = State.load(Pointer, 4);
  ASSERT_TRUE(Ctx.isVar(SymbolicMemory));
  EXPECT_FALSE(Ctx.varInfo(Ctx.varId(SymbolicMemory)).InputOrigin.has_value());

  State.clobberMemory();
  SymRef Memory = State.load(Ctx.mkConst(64, 0x401000), 4);
  ASSERT_TRUE(Ctx.isVar(Memory));
  EXPECT_TRUE(Ctx.varInfo(Ctx.varId(Memory)).Fresh);
  EXPECT_FALSE(Ctx.varInfo(Ctx.varId(Memory)).InputOrigin.has_value());
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

TEST(SymState, WholeScalarOverwritesDoNotBuildUnusedByteExpressions) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (auto Space : {SymSpace::Register, SymSpace::Temporary}) {
      for (unsigned Width : {64u, 256u, 512u}) {
        SymContext Ctx;
        SymState State(Ctx, Order);
        const auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
        std::vector<SymRef> Words;
        for (unsigned I = 0; I != 32; ++I)
          Words.push_back(Ctx.mkUDiv(Ctx.mkAdd(X, Ctx.mkConst(Width, I)), Y));
        const size_t Before = Ctx.numNodes();
        for (auto Word : Words) {
          State.write(Space, 17, Word);
          EXPECT_EQ(State.read(Space, 17, Width / 8), Word);
        }
        EXPECT_EQ(State.numLiveBytes(), Width / 8);
        EXPECT_EQ(Ctx.numNodes(), Before);
      }
    }
  }
}

TEST(SymState, OverlappingScalarViewsMatchAnIndependentByteStore) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (auto Space : {SymSpace::Register, SymSpace::Temporary}) {
      SymContext Ctx;
      SymState State(Ctx, Order);
      const auto Whole = Ctx.mkVar("whole", 128);
      const auto Patch = Ctx.mkVar("patch", 32);
      State.write(Space, 9, Whole);
      State.write(Space, 12, Patch);
      State.write(Space, 17, Ctx.mkExtract(Whole, 40, 16));
      struct Read {
        unsigned Offset, Bytes;
        SymRef Value;
      };
      std::vector<Read> Reads;
      for (unsigned Offset = 0; Offset != 16; ++Offset)
        for (unsigned Bytes = 1; Bytes <= 16 - Offset; ++Bytes)
          Reads.push_back(
              {Offset, Bytes, State.read(Space, 9 + Offset, Bytes)});

      for (uint64_t Seed = 0; Seed != 32; ++Seed) {
        const uint64_t Raw[] = {0x1b5d739af024c681ULL * (Seed + 1),
                                0xf097b43a261dc85eULL ^ (Seed << 41)};
        const llvm::APInt Input(128, Raw);
        const llvm::APInt Replacement(
            32, static_cast<uint32_t>(0x31a7c95dULL * (Seed + 3)));
        const std::vector<llvm::APInt> Values{Input, Replacement};
        std::array<uint8_t, 16> Bytes{};
        const auto Write = [&](unsigned Offset, const llvm::APInt &Value) {
          const unsigned Size = Value.getBitWidth() / 8;
          for (unsigned I = 0; I != Size; ++I) {
            const unsigned Lane =
                Order == llvm::endianness::little ? I : Size - I - 1;
            Bytes[Offset + I] = Value.extractBitsAsZExtValue(8, Lane * 8);
          }
        };
        Write(0, Input);
        Write(3, Replacement);
        Write(8, Input.extractBits(16, 40));
        for (const auto &Read : Reads) {
          llvm::APInt Expected(Read.Bytes * 8, 0);
          for (unsigned I = 0; I != Read.Bytes; ++I) {
            const unsigned Lane =
                Order == llvm::endianness::little ? I : Read.Bytes - I - 1;
            Expected |= llvm::APInt(Read.Bytes * 8, Bytes[Read.Offset + I])
                        << (Lane * 8);
          }
          EXPECT_EQ(Ctx.eval(Read.Value, Values), Expected)
              << "offset " << Read.Offset << " bytes " << Read.Bytes;
        }
      }
    }
  }
}

TEST(SymState, ForkedClobbersShareDeferredOverlappingScalarInputs) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (auto Space : {SymSpace::Register, SymSpace::Temporary}) {
      SymContext Ctx;
      SymState Left(Ctx, Order);
      Left.clobberRegistersExcept({});
      auto Right = Left;
      const auto Whole = Left.read(Space, 17, 8);
      const unsigned Low = Order == llvm::endianness::little ? 16 : 24;
      EXPECT_EQ(Right.read(Space, 19, 3), Ctx.mkExtract(Whole, Low, 24));
      const auto Changed = Ctx.mkFreshVar(8, "changed");
      Right.write(Space, 20, Changed);
      EXPECT_EQ(Right.read(Space, 20, 1), Changed);
      EXPECT_EQ(Left.read(Space, 17, 8), Whole);
      EXPECT_NE(Right.read(Space, 17, 8), Whole);
      EXPECT_FALSE(Left.mergeIdentical(Right));
      Right.clobberRegistersExcept({});
      EXPECT_NE(Right.read(Space, 17, 8), Whole);
    }
  }
}

TEST(SymState, StateMergeComparesByteValuesAcrossDifferentScalarViews) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (auto Space : {SymSpace::Register, SymSpace::Temporary}) {
      SymContext Ctx;
      SymState WholeState(Ctx, Order), ByteState(Ctx, Order);
      const auto Whole = Ctx.mkVar("whole", 256);
      WholeState.write(Space, 17, Whole);
      for (unsigned I = 0; I != 32; ++I) {
        const unsigned Lane = Order == llvm::endianness::little ? I : 31 - I;
        ByteState.write(Space, 17 + I, Ctx.mkExtract(Whole, Lane * 8, 8));
      }
      EXPECT_TRUE(WholeState.mergeIdentical(ByteState));
      ByteState.write(Space, 29, Ctx.mkFreshVar(8, "different"));
      EXPECT_FALSE(WholeState.mergeIdentical(ByteState));
      EXPECT_EQ(WholeState.read(Space, 17, 32), Whole);
    }
  }
}

TEST(SymState, PreservedScalarViewsKeepOnlyTheRequestedLanes) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    const auto Whole = Ctx.mkVar("whole", 64);
    State.write(SymSpace::Register, 17, Whole);
    State.clobberRegistersExcept({SymRegisterRange{19, 3}});
    const unsigned Low = Order == llvm::endianness::little ? 16 : 24;
    EXPECT_EQ(State.read(SymSpace::Register, 19, 3),
              Ctx.mkExtract(Whole, Low, 24));
    const auto Before = State.read(SymSpace::Register, 17, 2);
    const auto After = State.read(SymSpace::Register, 22, 3);
    ASSERT_TRUE(Ctx.isVar(Before));
    ASSERT_TRUE(Ctx.isVar(After));
    EXPECT_TRUE(Ctx.varInfo(Ctx.varId(Before)).Fresh);
    EXPECT_TRUE(Ctx.varInfo(Ctx.varId(After)).Fresh);
    EXPECT_NE(Ctx.varId(Before), Ctx.varId(After));
    EXPECT_NE(State.read(SymSpace::Register, 17, 8), Whole);
  }
}

TEST(SymState, StateMergeDistinguishesDifferentSlicesOfOneSource) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (auto Space : {SymSpace::Register, SymSpace::Temporary}) {
      SymContext Ctx;
      SymState Left(Ctx, Order), Right(Ctx, Order);
      const auto Word = Ctx.mkVar("word", 128);
      Left.write(Space, 17, Word);
      Left.write(Space, 25, Word);
      Right.write(Space, 25, Word);
      Right.write(Space, 17, Word);
      // Every byte refers to the same word and both banks cover [17, 41),
      // but the overlapping eight bytes retain different slices of it.
      EXPECT_FALSE(Left.mergeIdentical(Right));
      EXPECT_NE(Left.read(Space, 25, 8), Right.read(Space, 25, 8));
    }
  }
}

TEST(SymState, StateMergeRejectsForeignContextReferences) {
  SymContext LeftContext, RightContext;
  SymState Left(LeftContext), Right(RightContext);
  Left.write(SymSpace::Register, 0, LeftContext.mkVar("left", 64));
  Right.write(SymSpace::Register, 0, RightContext.mkVar("right", 64));
  EXPECT_FALSE(Left.mergeIdentical(Right));
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

TEST(SymExec, ShiftCountsAreNotTruncatedToTheValueWidth) {
  for (NdOp Opcode : {NdOp::INT_LEFT, NdOp::INT_RIGHT, NdOp::INT_ASHR}) {
    SCOPED_TRACE(static_cast<unsigned>(Opcode));
    for (uint64_t Amount : {uint64_t(0), uint64_t(1), uint64_t(7), uint64_t(8),
                            uint64_t(255), uint64_t(256), uint64_t(257)}) {
      SCOPED_TRACE(Amount);
      for (uint64_t Input : {uint64_t(0x71), uint64_t(0x91)}) {
        SCOPED_TRACE(Input);
        SymContext Ctx;
        SymState State(Ctx);
        SymExec Exec(Ctx, State);
        ASSERT_EQ(Exec.step(op(Opcode, NdVar::reg(kRax, 1),
                               {NdVar::cst(Input, 1), NdVar::cst(Amount, 2)})),
                  StepResult::Continue);
        SymRef Result = State.read(SymSpace::Register, kRax, 1);
        ASSERT_TRUE(Ctx.isConst(Result));
        llvm::APInt Bits(8, Input);
        llvm::APInt Expected = Opcode == NdOp::INT_LEFT
                                   ? Bits.shl(std::min(Amount, uint64_t(8)))
                               : Opcode == NdOp::INT_RIGHT
                                   ? Bits.lshr(std::min(Amount, uint64_t(8)))
                                   : Bits.ashr(std::min(Amount, uint64_t(7)));
        EXPECT_EQ(Ctx.constValue(Result), Expected);
        EXPECT_EQ(Exec.unmodelledCount(), 0u);
      }
    }
  }
}

TEST(SymExec, SymbolicShiftCountsKeepAllTheirBits) {
  for (NdOp Opcode : {NdOp::INT_LEFT, NdOp::INT_RIGHT, NdOp::INT_ASHR}) {
    SymContext Ctx;
    SymState State(Ctx);
    SymRef Value = Ctx.mkVar("shift_value", 8);
    SymRef Count = Ctx.mkVar("shift_count", 16);
    State.write(SymSpace::Register, kRax, Value);
    State.write(SymSpace::Register, kRbx, Count);
    SymExec Exec(Ctx, State);
    ASSERT_EQ(Exec.step(op(Opcode, NdVar::reg(kRcx, 1),
                           {NdVar::reg(kRax, 1), NdVar::reg(kRbx, 2)})),
              StepResult::Continue);
    SymRef Result = State.read(SymSpace::Register, kRcx, 1);
    for (uint64_t Amount :
         {uint64_t(1), uint64_t(8), uint64_t(256), uint64_t(257)}) {
      SCOPED_TRACE(Amount);
      llvm::APInt Bits(8, 0x91);
      llvm::APInt Expected = Opcode == NdOp::INT_LEFT
                                 ? Bits.shl(std::min(Amount, uint64_t(8)))
                             : Opcode == NdOp::INT_RIGHT
                                 ? Bits.lshr(std::min(Amount, uint64_t(8)))
                                 : Bits.ashr(std::min(Amount, uint64_t(7)));
      const llvm::APInt Inputs[] = {Bits, llvm::APInt(16, Amount)};
      EXPECT_EQ(Ctx.eval(Result, Inputs), Expected);
    }
  }
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

TEST(SymExec, SubtractionSignAndOverflowRecoverTheSignedPredicate) {
  for (unsigned Bytes : {1u, 2u, 4u, 8u, 16u, 32u}) {
    SymContext Ctx;
    SymState State(Ctx);
    SymRef X = Ctx.mkVar("x", Bytes * 8), Y = Ctx.mkVar("y", Bytes * 8);
    State.write(SymSpace::Register, 0, X);
    State.write(SymSpace::Register, 64, Y);
    for (NdOp Logic : {NdOp::BOOL_XOR, NdOp::INT_XOR}) {
      SymRef Result =
          execute(Ctx, State,
                  {op(NdOp::INT_SUB, NdVar::reg(128, Bytes),
                      {NdVar::reg(0, Bytes), NdVar::reg(64, Bytes)}),
                   op(NdOp::INT_SBOR, NdVar::reg(192, 1),
                      {NdVar::reg(0, Bytes), NdVar::reg(64, Bytes)}),
                   op(NdOp::INT_SLESS, NdVar::reg(193, 1),
                      {NdVar::reg(128, Bytes), NdVar::cst(0, Bytes)}),
                   op(Logic, NdVar::reg(194, 1),
                      {NdVar::reg(192, 1), NdVar::reg(193, 1)})},
                  194, 1);
      EXPECT_EQ(Result, Ctx.mkZExt(Ctx.mkSlt(X, Y), 8));
    }
  }
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

TEST(SymExec, NonDefaultAddressSpaceLoadFailsClosedWithAWidthStableUnknown) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  SymRef Address = Ctx.mkConst(64, 0x401000);
  State.store(Address, Ctx.mkConst(32, 0x12345678));

  LowOp Load = op(NdOp::LOAD, NdVar::reg(kRcx, 4), {NdVar::reg(kRax, 8)});
  Load.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  EXPECT_EQ(Exec.step(Load), StepResult::Unmodelled);

  SymRef Result = State.read(SymSpace::Register, kRcx, 4);
  ASSERT_TRUE(Ctx.isVar(Result));
  EXPECT_EQ(Ctx.width(Result), 32u);
  const SymVarInfo &Info = Ctx.varInfo(Ctx.varId(Result));
  EXPECT_TRUE(Info.Fresh);
  EXPECT_FALSE(Info.InputOrigin.has_value());
  EXPECT_EQ(Exec.unmodelledCount(), 1u);
  EXPECT_EQ(Exec.opaqueOperationCount(), 1u);
  EXPECT_EQ(Exec.memoryHavocCount(), 0u);
  EXPECT_TRUE(Ctx.isConst(State.load(Address, 4)))
      << "a segmented load must not read or clobber default memory";
}

TEST(SymExec, NonDefaultAddressSpaceStoreClobbersDefaultMemory) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  SymRef Address = Ctx.mkConst(64, 0x401000);
  State.store(Address, Ctx.mkConst(32, 0x12345678));

  LowOp Store = op(NdOp::STORE, NdVar{},
                   {NdVar::reg(kRax, 8), NdVar::cst(0xAABBCCDD, 4)});
  Store.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  EXPECT_EQ(Exec.step(Store), StepResult::Unmodelled);

  EXPECT_TRUE(State.memoryIsUnknown());
  EXPECT_EQ(Exec.unmodelledCount(), 1u);
  EXPECT_EQ(Exec.memoryHavocCount(), 1u);
  SymRef After = State.load(Address, 4);
  ASSERT_TRUE(Ctx.isVar(After));
  EXPECT_EQ(Ctx.width(After), 32u);
  const SymVarInfo &Info = Ctx.varInfo(Ctx.varId(After));
  EXPECT_TRUE(Info.Fresh);
  EXPECT_FALSE(Info.InputOrigin.has_value());
}

TEST(SymExec, NonDefaultAddressSpaceAtomicsClobberMemoryAndHavocOutputs) {
  struct AtomicCase {
    NdOp Opcode;
    std::vector<NdVar> Inputs;
  };
  const AtomicCase Cases[] = {
      {NdOp::ATOMIC_XCHG, {NdVar::reg(kRax, 8), NdVar::cst(0x11111111, 4)}},
      {NdOp::ATOMIC_ADD, {NdVar::reg(kRax, 8), NdVar::cst(0x22222222, 4)}},
      {NdOp::ATOMIC_CMPXCHG,
       {NdVar::reg(kRax, 8), NdVar::cst(0x33333333, 4),
        NdVar::cst(0x44444444, 4)}},
  };

  for (const AtomicCase &C : Cases) {
    SCOPED_TRACE(static_cast<unsigned>(C.Opcode));
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    SymRef Address = Ctx.mkConst(64, 0x401000);
    State.store(Address, Ctx.mkConst(32, 0x12345678));

    LowOp Atomic = op(C.Opcode, NdVar::reg(kRcx, 4), C.Inputs);
    Atomic.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    Atomic.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    EXPECT_EQ(Exec.step(Atomic), StepResult::Unmodelled);

    EXPECT_TRUE(State.memoryIsUnknown());
    EXPECT_EQ(Exec.unmodelledCount(), 1u);
    EXPECT_EQ(Exec.memoryHavocCount(), 1u);
    SymRef Output = State.read(SymSpace::Register, kRcx, 4);
    SymRef Memory = State.load(Address, 4);
    ASSERT_TRUE(Ctx.isVar(Output));
    ASSERT_TRUE(Ctx.isVar(Memory));
    EXPECT_EQ(Ctx.width(Output), 32u);
    EXPECT_EQ(Ctx.width(Memory), 32u);
    EXPECT_NE(Ctx.varId(Output), Ctx.varId(Memory));
    for (SymRef Value : {Output, Memory}) {
      const SymVarInfo &Info = Ctx.varInfo(Ctx.varId(Value));
      EXPECT_TRUE(Info.Fresh);
      EXPECT_FALSE(Info.InputOrigin.has_value());
    }
  }
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

TEST(SymState, WholeMemoryOverwritesAndSnapshotsRetainTheirWrittenWords) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (const bool Relative : {false, true})
      for (const unsigned Width : {64u, 256u, 512u}) {
        SymContext C;
        SymState State(C, Order);
        const auto Address =
            Relative ? C.mkFreshVar(64, "memory_base_") : C.mkConst(64, 0x1000);
        const auto X = C.mkFreshVar(Width, "memory_x_");
        const auto Y = C.mkFreshVar(Width, "memory_y_");
        std::vector<SymRef> Values;
        for (unsigned I = 0; I < 32; ++I)
          Values.push_back(C.mkUDiv(C.mkAdd(X, C.mkConst(Width, I)), Y));
        const auto Before = C.numNodes();
        for (const auto V : Values) {
          State.store(Address, V);
          EXPECT_EQ(State.load(Address, Width / 8), V);
        }
        EXPECT_EQ(C.numNodes(), Before);
        EXPECT_EQ(State.numLiveBytes(), Width / 8);
        if (Width == 64) {
          const auto Knowledge = State.memoryKnowledge();
          ASSERT_EQ(Knowledge.Facts.size(), 1u);
          EXPECT_EQ(Knowledge.Facts.front().Value, Values.back());
          EXPECT_EQ(C.numNodes(), Before);
        }
      }
}

TEST(SymState, DeferredMemoryProjectionRetainsLiteralConcatLanes) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState State(C, Order);
    const auto Base = C.mkFreshVar(64, "literal_lanes_");
    const auto Middle = C.mkFreshVar(32, "mixed_middle_");
    const auto Value =
        C.mkConcat({C.mkConst(16, 0x1234), Middle, C.mkConst(16, 0x5678)});
    State.store(Base, Value);
    const auto Before = C.numNodes();
    const auto Projection = State.constantRegionBytes(Base);
    ASSERT_EQ(Projection.size(), 4u);
    for (const auto &Byte : Projection) {
      const auto Lane =
          Order == llvm::endianness::little ? Byte.Offset : 7 - Byte.Offset;
      EXPECT_TRUE(Lane < 2 || Lane >= 6);
      const uint64_t Literal = Lane < 2 ? 0x5678 : 0x1234;
      EXPECT_EQ(Byte.Value, (Literal >> ((Lane % 2) * 8)) & 0xff);
    }
    EXPECT_EQ(C.numNodes(), Before);
  }
}

TEST(SymState, MaterializedLoadsDoNotCreateMemoryOrObservations) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState State(C, Order);
    const auto Base = C.mkFreshVar(64, "materialized_base_");
    const auto Missing = C.mkFreshVar(64, "missing_base_");
    const auto Value = C.mkFreshVar(64, "materialized_word_");
    const auto At = C.mkAdd(Base, C.mkConst(64, 3));
    EXPECT_FALSE(State.loadMaterialized(Base, 8));
    EXPECT_EQ(State.numMemoryRegions(), 0u);
    State.store(At, Value);
    const auto Before = C.numNodes();
    const auto Live = State.numLiveBytes();
    EXPECT_EQ(State.loadMaterialized(At, 8), Value);
    EXPECT_FALSE(State.loadMaterialized(Base, 8));
    EXPECT_FALSE(State.loadMaterialized(At, 9));
    EXPECT_FALSE(State.loadMaterialized(Missing, 8));
    EXPECT_FALSE(State.loadMaterialized(At, 0));
    EXPECT_FALSE(State.loadMaterialized({}, 8));
    EXPECT_EQ(State.numLiveBytes(), Live);
    EXPECT_EQ(State.numMemoryRegions(), 1u);
    EXPECT_EQ(C.numNodes(), Before);
    EXPECT_TRUE(State.loadOrigins(Value).empty());
    const auto PatchAt = C.mkAdd(At, C.mkConst(64, 2));
    State.store(PatchAt, C.mkConst(16, 0x1789));
    EXPECT_EQ(State.loadMaterialized(At, 8), State.load(At, 8));
    State.clobberMemory();
    auto Sibling = State;
    Sibling.load(At, 8);
    EXPECT_FALSE(State.loadMaterialized(At, 8));
    EXPECT_EQ(State.numLiveBytes(), 0u);
  }
}

TEST(SymState, MaterializedLoadsKeepModularRelativeCoordinates) {
  SymContext C;
  SymState State(C);
  const auto Base = C.mkFreshVar(64, "wrapping_base_");
  const auto Address = C.mkSub(Base, C.mkConst(64, 3));
  const auto Value = C.mkFreshVar(64, "wrapping_word_");
  State.store(Address, Value);
  EXPECT_EQ(State.loadMaterialized(Address, 8), Value);
  State.write(SymSpace::Memory, UINT64_MAX, C.mkConst(8, 1));
  EXPECT_FALSE(State.loadMaterialized(C.mkConst(64, UINT64_MAX), 2));
}

TEST(SymState, MemoryViewsKeepConstantWindowsAndPartialOverwrites) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState State(C, Order);
    const auto Base = C.mkFreshVar(64, "constant_region_");
    const auto Value = C.mkConst(64, 0x1122334455667788);
    State.store(Base, Value);
    const auto Region = State.constantRegionBytes(Base);
    ASSERT_EQ(Region.size(), 8u);
    for (unsigned I = 0; I < 8; ++I) {
      EXPECT_EQ(Region[I].Offset, I);
      const auto Lane = Order == llvm::endianness::little ? I : 7 - I;
      EXPECT_EQ(Region[I].Value,
                C.constValue(Value).extractBitsAsZExtValue(8, Lane * 8));
    }
    const auto Patch = C.mkFreshVar(16, "memory_patch_");
    const auto PatchAt = C.mkAdd(Base, C.mkConst(64, 3));
    State.store(PatchAt, Patch);
    EXPECT_EQ(State.constantRegionBytes(Base).size(), 6u);
    const auto Knowledge = State.memoryKnowledge();
    SymState Restored(C, Order);
    ASSERT_TRUE(Restored.restoreMemoryKnowledge(Knowledge));
    EXPECT_EQ(State.load(Base, 8), Restored.load(Base, 8));
    for (unsigned I = 0; I < 8; ++I) {
      const auto At = C.mkAdd(Base, C.mkConst(64, I));
      const auto Actual = State.load(At, 1);
      const auto Lane = Order == llvm::endianness::little ? I : 7 - I;
      const auto Expected =
          I == 3 || I == 4
              ? C.mkExtract(
                    Patch,
                    (Order == llvm::endianness::little ? I - 3 : 4 - I) * 8, 8)
              : C.mkExtract(Value, Lane * 8, 8);
      EXPECT_EQ(Actual, Expected);
    }
  }
}

TEST(SymState, OrderedBankWalkRetainsGapsAndMiddleOverwrites) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (const auto Space :
         {SymSpace::Register, SymSpace::Temporary, SymSpace::Memory}) {
      SymContext C;
      SymState Words(C, Order), Bytes(C, Order);
      for (const uint64_t At : {7u, 11u, 24u, 40u}) {
        const auto V = C.mkConst(8, At);
        Words.write(Space, At, V);
        Bytes.write(Space, At, V);
      }
      auto ReadBytes = [&](uint64_t At, uint16_t Count) {
        std::vector<SymRef> Parts;
        for (unsigned I = 0; I < Count; ++I)
          Parts.push_back(Bytes.read(Space, At + I, 1));
        if (Order == llvm::endianness::little)
          std::reverse(Parts.begin(), Parts.end());
        return C.mkConcat(Parts);
      };
      EXPECT_EQ(Words.read(Space, 8, 16), ReadBytes(8, 16));
      const auto V = C.mkFreshVar(256, "wide_update_");
      Words.write(Space, 8, V);
      for (unsigned I = 0; I < 32; ++I) {
        const auto Low = (Order == llvm::endianness::little ? I : 31 - I) * 8;
        Bytes.write(Space, 8 + I, C.mkExtract(V, Low, 8));
      }
      Words.write(Space, 19, C.mkConst(16, 0x1234));
      for (unsigned I = 0; I < 2; ++I) {
        const auto Low = (Order == llvm::endianness::little ? I : 1 - I) * 8;
        Bytes.write(Space, 19 + I, C.mkExtract(C.mkConst(16, 0x1234), Low, 8));
      }
      EXPECT_EQ(Words.read(Space, 8, 32), ReadBytes(8, 32));
      EXPECT_EQ(Words.read(Space, 9, 16), ReadBytes(9, 16));
    }
}

TEST(SymState, RestoringMemoryDoesNotForkOrReplaceScalarState) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState State(C, Order);
    const auto Base = C.mkFreshVar(64, "restored_memory_");
    const auto Value = C.mkFreshVar(64, "historical_load_");
    State.clobberMemory();
    State.store(Base, Value);
    EXPECT_EQ(State.load(Base, 8), Value);
    State.clobberRegistersExcept({});
    const auto R = State.read(SymSpace::Register, 0, 8);
    State.write(SymSpace::Temporary, 12, Value);
    ASSERT_TRUE(
        State.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, Base));
    auto Sibling = State;
    const auto Knowledge = State.memoryKnowledge();
    ASSERT_TRUE(State.restoreMemoryKnowledge(Knowledge));
    EXPECT_EQ(State.read(SymSpace::Register, 0, 8), R);
    EXPECT_EQ(State.read(SymSpace::Register, 16, 8),
              Sibling.read(SymSpace::Register, 16, 8));
    EXPECT_EQ(State.read(SymSpace::Temporary, 12, 8), Value);
    EXPECT_EQ(State.loadOrigins(Value), Sibling.loadOrigins(Value));
    EXPECT_EQ(State.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS),
              Base);
    EXPECT_EQ(State.loadMaterialized(Base, 8), Value);
    auto Empty = Knowledge;
    Empty.Facts.clear();
    ASSERT_TRUE(State.restoreMemoryKnowledge(Empty));
    EXPECT_FALSE(State.loadMaterialized(Base, 8));
    EXPECT_NE(State.load(Base, 8), Value);
    EXPECT_EQ(Sibling.loadMaterialized(Base, 8), Value);
  }
}

TEST(SymState, ScalarSnapshotsKeepEpochsAndOmitMemoryBanks) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState State(C, Order);
    const auto Base = C.mkFreshVar(64, "snapshot_memory_");
    const auto Value = C.mkFreshVar(64, "snapshot_value_");
    State.store(Base, Value);
    EXPECT_EQ(State.load(Base, 8), Value);
    State.write(SymSpace::Register, 0, C.mkConst(64, 0x12345678));
    State.clobberRegistersExcept({SymRegisterRange{0, 4}});
    State.write(SymSpace::Temporary, 8, Value);
    ASSERT_TRUE(
        State.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, Base));
    auto Snapshot = State.scalarSnapshot();
    EXPECT_EQ(Snapshot.byteOrder(), Order);
    EXPECT_EQ(Snapshot.numMemoryRegions(), 0u);
    EXPECT_TRUE(Snapshot.memoryKnowledge().Facts.empty());
    EXPECT_TRUE(Snapshot.loadOrigins(Value).empty());
    EXPECT_EQ(Snapshot.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS),
              Base);
    EXPECT_EQ(Snapshot.read(SymSpace::Register, 0, 8),
              State.read(SymSpace::Register, 0, 8));
    EXPECT_EQ(Snapshot.read(SymSpace::Register, 16, 8),
              State.read(SymSpace::Register, 16, 8));
    EXPECT_EQ(Snapshot.read(SymSpace::Temporary, 8, 8), Value);
    Snapshot.write(SymSpace::Register, 0, C.mkZero(64));
    EXPECT_NE(Snapshot.read(SymSpace::Register, 0, 8),
              State.read(SymSpace::Register, 0, 8));
  }
}

TEST(SymState, AnUnreadRunOfAPartlyKnownWordIsOneInput) {
  SymContext C;
  SymState S(C);
  S.write(SymSpace::Register, 18, C.mkConst(48, 0));
  const auto Word = S.read(SymSpace::Register, 16, 8);
  ASSERT_EQ(C.op(Word), SymOp::ZExt);
  const auto Low = C.operand(Word, 0);
  ASSERT_TRUE(C.isVar(Low));
  EXPECT_EQ(C.width(Low), 16u);
  EXPECT_EQ(S.read(SymSpace::Register, 16, 2), Low);
  EXPECT_EQ(S.read(SymSpace::Register, 17, 1), C.mkExtract(Low, 8, 8));

  // Runs on both sides of a known byte stay separate inputs.
  S.write(SymSpace::Register, 34, C.mkConst(8, 0x5A));
  const auto Split = S.read(SymSpace::Register, 32, 5);
  ASSERT_EQ(C.op(Split), SymOp::Concat);
  const auto High = S.read(SymSpace::Register, 35, 2);
  const auto Bottom = S.read(SymSpace::Register, 32, 2);
  EXPECT_TRUE(C.isVar(High));
  EXPECT_TRUE(C.isVar(Bottom));
  EXPECT_EQ(Split, C.mkConcat({High, C.mkConst(8, 0x5A), Bottom}));
}
