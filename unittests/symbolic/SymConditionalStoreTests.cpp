//===- SymConditionalStoreTests.cpp - Conditional writes tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Byte-oracle and execution tests for bounded conditional memory writes.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExec.h"

#include <array>
#include <map>
#include <vector>

using namespace neverd;
using namespace neverd::solver;
using namespace neverd::symbolic;

namespace {

uint8_t initialByte(uint64_t Address) {
  return static_cast<uint8_t>(
      (Address ^ (Address >> 8) ^ (Address >> 40)) * 37 + 11);
}

llvm::APInt byteWord(llvm::ArrayRef<uint8_t> Bytes, llvm::endianness Order) {
  llvm::APInt Result(Bytes.size() * 8, 0);
  for (unsigned I = 0; I != Bytes.size(); ++I) {
    const unsigned Lane =
        Order == llvm::endianness::little ? I : unsigned(Bytes.size()) - I - 1;
    Result |= llvm::APInt(Result.getBitWidth(), Bytes[I]).shl(8 * Lane);
  }
  return Result;
}

// Model the shared concrete memory by address, then apply one finite write.
// Initial values of aliased symbolic banks come from this same byte function.
llvm::APInt memoryWord(uint64_t Address, unsigned Bytes, llvm::endianness Order,
                       uint64_t WriteAddress = 0,
                       llvm::ArrayRef<uint8_t> Written = {}) {
  std::vector<uint8_t> Result;
  for (unsigned I = 0; I != Bytes; ++I) {
    const uint64_t At = Address + I;
    const uint64_t Distance = At - WriteAddress;
    Result.push_back(Distance < Written.size()
                         ? Written[static_cast<size_t>(Distance)]
                         : initialByte(At));
  }
  return byteWord(Result, Order);
}

void expectUnsat(SymContext &C, SymRef Predicate) {
  SolverOptions Options;
  Options.Sat.MaxConflicts = 1000;
  Options.Sat.MaxPropagations = 100000;
  Options.Sat.MaxWatchVisits = 1000000;
  BitVectorSolver Solver(C, Options);
  ASSERT_TRUE(Solver.assertTrue(Predicate));
  EXPECT_EQ(Solver.check(), SatResult::Unsat);
}

void checkByteOracle(llvm::endianness Order, unsigned WriteBytes, bool Absolute,
                     bool RelativeWrap) {
  SCOPED_TRACE(static_cast<unsigned>(Order));
  SCOPED_TRACE(WriteBytes);
  SCOPED_TRACE(Absolute);
  SCOPED_TRACE(RelativeWrap);
  SymContext C;
  SymState S(C, Order);
  const auto A = C.mkVar("first", 64);
  const auto B = C.mkVar("second", 64);
  const auto D = C.mkVar("destination", 64);
  const auto Value = C.mkVar("value", WriteBytes * 8);
  const uint64_t ReadOffset = RelativeWrap ? UINT64_MAX - 7 : 0;
  const uint64_t WriteOffset = RelativeWrap ? UINT64_MAX - 3 : 0;
  const auto At = [&](SymRef Root, uint64_t Offset) {
    return C.mkAdd(Root, C.mkConst(64, Offset));
  };
  const auto ReadA = At(A, ReadOffset);
  const auto Write = Absolute ? C.mkConst(64, 0x3001) : At(D, WriteOffset);
  std::vector<SymRef> OldABytes;
  if (RelativeWrap)
    for (unsigned I = 0; I != 16; ++I)
      OldABytes.push_back(S.load(At(A, ReadOffset + I), 1));
  const auto OldA = S.load(ReadA, 16);
  const auto OldB = S.load(B, 16);
  const auto OldAbsolute = S.load(C.mkConst(64, 0x3000), 16);
  const auto OldOwn = S.load(At(D, 40), 2);
  if (!RelativeWrap)
    ASSERT_TRUE(C.isVar(OldA));
  for (const auto Input : {OldB, OldAbsolute, OldOwn})
    ASSERT_TRUE(C.isVar(Input));
  for (const auto Input : OldABytes)
    ASSERT_TRUE(C.isVar(Input));
  SymConditionalStore Control{100000};
  EXPECT_EQ(S.storeConditional(Write, Value, Control), Absolute);
  ASSERT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
  const std::array<SymRef, 5> Results{
      S.load(ReadA, 16), S.load(B, 16), S.load(C.mkConst(64, 0x3000), 16),
      S.load(At(D, 40), 2), S.load(Write, WriteBytes)};
  std::vector<SymEvalPlan> Plans;
  for (const auto Result : Results)
    Plans.emplace_back(C, Result);

  for (const uint64_t AV : {uint64_t(0x1000), UINT64_MAX - 5}) {
    const uint64_t ReadAV = AV + ReadOffset;
    const std::array<uint64_t, 9> Destinations{
        ReadAV - 1,  ReadAV,      ReadAV + 1,
        ReadAV + 15, ReadAV + 16, 0x2000,
        0x3003,      0x8000,      UINT64_MAX - WriteBytes / 2};
    for (const uint64_t Destination : Destinations) {
      const uint64_t DV = Destination - WriteOffset;
      const uint64_t ActualWrite = Absolute ? 0x3001 : Destination;
      for (unsigned Pattern = 0; Pattern != 2; ++Pattern) {
        std::vector<uint8_t> Written(WriteBytes);
        for (unsigned I = 0; I != WriteBytes; ++I)
          Written[I] = Pattern ? uint8_t(29 * I + 73) : uint8_t(255);
        std::map<uint32_t, llvm::APInt> Inputs;
        const auto Bind = [&](SymRef Input, llvm::APInt Bits) {
          Inputs.emplace(C.varId(Input), std::move(Bits));
        };
        Bind(A, llvm::APInt(64, AV));
        Bind(B, llvm::APInt(64, 0x2000));
        Bind(D, llvm::APInt(64, DV));
        Bind(Value, byteWord(Written, Order));
        if (RelativeWrap) {
          for (unsigned I = 0; I != OldABytes.size(); ++I)
            Bind(OldABytes[I], memoryWord(ReadAV + I, 1, Order));
        } else {
          Bind(OldA, memoryWord(ReadAV, 16, Order));
        }
        Bind(OldB, memoryWord(0x2000, 16, Order));
        Bind(OldAbsolute, memoryWord(0x3000, 16, Order));
        Bind(OldOwn, memoryWord(DV + 40, 2, Order));
        const std::array<uint64_t, 5> Reads{ReadAV, 0x2000, 0x3000, DV + 40,
                                            ActualWrite};
        const std::array<unsigned, 5> Sizes{16, 16, 16, 2, WriteBytes};
        for (unsigned I = 0; I != Plans.size(); ++I) {
          SCOPED_TRACE(I);
          const auto Actual = Plans[I].evalWith([&](uint32_t Id) {
            const auto Found = Inputs.find(Id);
            EXPECT_NE(Found, Inputs.end());
            return Found == Inputs.end() ? nullptr : &Found->second;
          });
          EXPECT_EQ(Actual, memoryWord(Reads[I], Sizes[I], Order, ActualWrite,
                                       Written));
        }
      }
    }
  }
}

TEST(SymConditionalStore, ByteOracleCoversWidePartialAndAbsoluteAliases) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (const unsigned Bytes : {1u, 8u, 16u})
      for (const bool Absolute : {false, true})
        checkByteOracle(Order, Bytes, Absolute, false);
}

TEST(SymConditionalStore, RelativeCoordinatesAndPhysicalAddressesCanWrap) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big})
    checkByteOracle(Order, 16, false, true);
}

TEST(SymConditionalStore, OwnBankUsesOrdinaryByteOverlap) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto Root = C.mkVar("root", 64);
    std::vector<uint8_t> Bytes(16);
    for (unsigned I = 0; I != Bytes.size(); ++I)
      Bytes[I] = uint8_t(I * 13 + 7);
    S.store(Root, C.mkConst(byteWord(Bytes, Order)));
    const std::array<uint8_t, 8> Written{1, 3, 5, 7, 9, 11, 13, 15};
    SymConditionalStore Control{100};
    EXPECT_FALSE(S.storeConditional(C.mkAdd(Root, C.mkConst(64, 4)),
                                    C.mkConst(byteWord(Written, Order)),
                                    Control));
    ASSERT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
    EXPECT_EQ(Control.WorkUsed, 10u); // Eight written bytes and two banks.
    for (unsigned I = 0; I != Written.size(); ++I)
      Bytes[I + 4] = Written[I];
    EXPECT_EQ(S.load(Root, 16), C.mkConst(byteWord(Bytes, Order)));
  }
}

TEST(SymConditionalStore, SmallSolverProvesBothAliasAndNonaliasEffects) {
  SymContext C;
  SymState S(C);
  const auto A = C.mkVar("observer", 64);
  const auto D = C.mkVar("destination", 64);
  const auto Old = S.load(A, 1);
  const auto Value = C.mkVar("byte", 8);
  SymConditionalStore Control{100};
  S.storeConditional(D, Value, Control);
  ASSERT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
  const auto After = S.load(A, 1);
  expectUnsat(C, C.mkAnd(C.mkEq(A, D), C.mkNe(After, Value)));
  expectUnsat(C, C.mkAnd(C.mkNe(A, D), C.mkNe(After, Old)));
}

TEST(SymConditionalStore, KeepsCurrentBytesWithoutRevivingHistoryOrEpochs) {
  SymContext C;
  SymState S(C);
  const auto A = C.mkVar("current", 64);
  const auto D = C.mkVar("destination", 64);
  const auto H = C.mkVar("historical", 64);
  const auto Historical = S.load(H, 8);
  S.clobberMemory();
  const auto Current = S.load(A, 1);
  const auto NeighborAddress = C.mkAdd(D, C.mkConst(64, 8));
  const auto Neighbor = S.load(NeighborAddress, 1);
  auto Before = S;
  SymConditionalStore Control{9}; // One write, four banks, one byte candidate.
  S.storeConditional(D, C.mkConst(8, 19), Control);
  ASSERT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
  EXPECT_EQ(Control.WorkUsed, 9u);
  EXPECT_EQ(S.numLiveBytes(), 3u);
  EXPECT_FALSE(S.loadOrigins(Historical).empty());
  EXPECT_EQ(S.load(NeighborAddress, 1), Neighbor);
  auto Fork = S;
  const auto NewHistorical = S.load(H, 8);
  EXPECT_NE(NewHistorical, Historical);
  EXPECT_EQ(NewHistorical, Fork.load(H, 8));
  EXPECT_TRUE(S.memoryInputOrigins(NewHistorical).empty());
  const auto Missing = C.mkAdd(A, C.mkConst(64, 16));
  const auto NewMissing = S.load(Missing, 1);
  EXPECT_NE(NewMissing, Before.load(Missing, 1));
  EXPECT_EQ(NewMissing, Fork.load(Missing, 1));
  const auto Unseen = C.mkVar("unseen", 64);
  EXPECT_EQ(S.load(Unseen, 1), Fork.load(Unseen, 1));
  EXPECT_TRUE(S.memoryInputOrigins(S.load(Unseen, 1)).empty());
  expectUnsat(C, C.mkAnd(C.mkNe(A, D), C.mkNe(S.load(A, 1), Current)));
}

TEST(SymConditionalStore, ExactBudgetAndRefusalAreTransactionalAtOwner) {
  SymContext C;
  SymState Initial(C);
  const auto A = C.mkVar("observer", 64);
  const auto D = C.mkVar("destination", 64);
  const auto Value = C.mkVar("word", 16);
  Initial.load(A, 4);
  Initial.load(C.mkConst(64, 0x4000), 2);
  Initial.load(C.mkAdd(D, C.mkConst(64, 8)), 1);
  // Two output bytes, three banks, six cross-bank bytes at five units each.
  for (const uint64_t Budget : {uint64_t(0), uint64_t(34), uint64_t(35)}) {
    SCOPED_TRACE(Budget);
    auto S = Initial;
    const auto Nodes = C.numNodes();
    SymConditionalStore Control{Budget};
    S.storeConditional(D, Value, Control);
    EXPECT_LE(Control.WorkUsed, Budget);
    if (Budget == 35) {
      EXPECT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
      EXPECT_EQ(Control.WorkUsed, Budget);
      EXPECT_EQ(S.load(D, 2), Value);
    } else {
      EXPECT_EQ(Control.Status, SymConditionalStoreStatus::BudgetExceeded);
      EXPECT_EQ(C.numNodes(), Nodes);
      EXPECT_TRUE(S.mergeIdentical(Initial));
    }
  }
}

TEST(SymConditionalStore, InvalidReferencesWidthsAndAbsoluteWrapDoNotMutate) {
  SymContext C;
  SymState Initial(C);
  const auto A = C.mkVar("observer", 64);
  const auto D = C.mkVar("destination", 64);
  const auto Narrow = C.mkVar("narrow", 32);
  const auto Value = C.mkVar("word", 16);
  const auto TooWide = C.mkVar("too_wide", 8u * 65536u);
  const auto OddWidth = C.mkConst(3, 5);
  Initial.load(A, 4);
  const auto Invalid = SymRef(uint32_t(C.numNodes() + 100));
  const std::array<std::pair<SymRef, SymRef>, 8> Cases{
      std::pair{SymRef(), Value},
      {Invalid, Value},
      {D, SymRef()},
      {D, Invalid},
      {Narrow, Value},
      {D, OddWidth},
      {D, TooWide},
      {C.mkConst(64, UINT64_MAX), Value}};
  for (const auto &[Address, Bits] : Cases) {
    auto S = Initial;
    SymConditionalStore Control{10000};
    EXPECT_FALSE(S.storeConditional(Address, Bits, Control));
    EXPECT_EQ(Control.Status, SymConditionalStoreStatus::Invalid);
    EXPECT_TRUE(S.mergeIdentical(Initial));
  }
  Initial.load(Narrow, 1);
  auto S = Initial;
  SymConditionalStore Control{10000};
  S.storeConditional(D, Value, Control);
  EXPECT_EQ(Control.Status, SymConditionalStoreStatus::Invalid);
  EXPECT_TRUE(S.mergeIdentical(Initial));
}

LowOp memoryOp(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const auto Input : Inputs)
    Op.addInput(Input);
  return Op;
}

LowOp cmpxchg(NdVar Output, NdVar Address, NdVar Expected, NdVar Desired) {
  auto Op =
      memoryOp(NdOp::ATOMIC_CMPXCHG, Output, {Address, Expected, Desired});
  Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  return Op;
}

TEST(SymConditionalStore, ExecutorUsesEffectiveGSAddressAndOrdinaryMemory) {
  for (const bool SymbolicBase : {false, true}) {
    SymContext C;
    SymState S(C);
    SymExec E(C, S);
    const auto A = C.mkVar("observer", 64);
    const auto Base =
        SymbolicBase ? C.mkVar("segment_base", 64) : C.mkConst(64, 0x1000);
    S.store(C.mkConst(64, 0x40), C.mkConst(8, 7));
    S.load(A, 1);
    ASSERT_TRUE(
        S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, Base));
    auto Op = memoryOp(NdOp::STORE, {},
                       {NdVar::scalar(0x40, 4), NdVar::scalar(19, 1)});
    Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    const auto Address = C.mkAdd(Base, C.mkConst(64, 0x40));
    EXPECT_EQ(E.memoryAddressValue(Op), Address);
    SymConditionalStore Control{100};
    EXPECT_EQ(E.stepWithConditionalStore(Op, Control), StepResult::Continue);
    ASSERT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
    EXPECT_EQ(E.opaqueOperationCount(), 0u);
    EXPECT_EQ(E.memoryHavocCount(), SymbolicBase ? 1u : 0u);
    EXPECT_EQ(S.load(Address, 1), C.mkConst(8, 19));
    expectUnsat(
        C, C.mkAnd(C.mkEq(A, Address), C.mkNe(S.load(A, 1), C.mkConst(8, 19))));
    if (!SymbolicBase)
      EXPECT_EQ(S.load(C.mkConst(64, 0x40), 1), C.mkConst(8, 7));
  }
}

TEST(SymConditionalStore, ExecutorCmpxchgSelectsOneWriteBeforePublishingOld) {
  for (const auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (const bool Match : {false, true}) {
      SymContext C;
      SymState S(C, Order);
      SymExec E(C, S);
      const auto Root = C.mkVar("observer", 64);
      const auto Old = C.mkConst(32, 0x12345678);
      const auto Desired = C.mkConst(32, 0xabcdef01);
      S.store(C.mkConst(64, 0x4000), Old);
      S.load(Root, 4);
      // Desired and Old output deliberately alias the same register.
      S.write(SymSpace::Register, 0, Desired);
      const auto Op =
          cmpxchg(NdVar::reg(0, 4), NdVar::scalar(0x4000, 8),
                  NdVar::scalar(Match ? 0x12345678 : 0, 4), NdVar::reg(0, 4));
      SymConditionalStore Control{100};
      EXPECT_EQ(E.stepWithConditionalStore(Op, Control), StepResult::Continue);
      ASSERT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
      EXPECT_EQ(S.read(SymSpace::Register, 0, 4), Old);
      EXPECT_EQ(S.load(C.mkConst(64, 0x4000), 4), Match ? Desired : Old);
      EXPECT_EQ(E.opaqueOperationCount(), 0u);
      for (unsigned I = 0; I != 4; ++I) {
        const unsigned Lane = Order == llvm::endianness::little ? I : 3 - I;
        const auto Byte = C.mkExtract(Match ? Desired : Old, Lane * 8, 8);
        expectUnsat(
            C,
            C.mkAnd(C.mkEq(Root, C.mkConst(64, 0x4000)),
                    C.mkNe(S.load(C.mkAdd(Root, C.mkConst(64, I)), 1), Byte)));
      }
    }
  }
}

TEST(SymConditionalStore, ExecutorCmpxchgBudgetRefusalDoesNotPublishOld) {
  SymContext C;
  SymState Initial(C);
  Initial.store(C.mkConst(64, 0x4000), C.mkConst(32, 7));
  Initial.load(C.mkVar("observer", 64), 4);
  const auto Op = cmpxchg(NdVar::reg(0, 4), NdVar::scalar(0x4000, 8),
                          NdVar::scalar(7, 4), NdVar::scalar(11, 4));
  for (const uint64_t Budget : {uint64_t(0), uint64_t(33), uint64_t(34)}) {
    auto S = Initial;
    SymExec E(C, S);
    SymConditionalStore Control{Budget};
    const auto Flow = E.stepWithConditionalStore(Op, Control);
    EXPECT_LE(Control.WorkUsed, Budget);
    if (Budget == 34) { // Four write bytes, two banks, four seven-unit bytes.
      EXPECT_EQ(Flow, StepResult::Continue);
      EXPECT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
      EXPECT_EQ(Control.WorkUsed, 34u);
      EXPECT_EQ(S.read(SymSpace::Register, 0, 4), C.mkConst(32, 7));
    } else {
      EXPECT_EQ(Flow, StepResult::Unmodelled);
      EXPECT_EQ(Control.Status, SymConditionalStoreStatus::BudgetExceeded);
      EXPECT_EQ(E.opaqueOperationCount(), 1u);
      EXPECT_EQ(E.memoryHavocCount(), 1u);
      EXPECT_FALSE(C.isConst(S.read(SymSpace::Register, 0, 4)));
      EXPECT_FALSE(C.isConst(S.load(C.mkConst(64, 0x4000), 4)));
      // SymExec refuses conservatively; owner rollback does not imply that
      // the executor's memory and output are unchanged.
    }
  }
}

TEST(SymConditionalStore, AtomicAddAndExchangeCommitAliasedInputsInBothOrders) {
  for (const auto Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG}) {
    for (const auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      for (const uint16_t Bytes : {1, 2, 4, 8}) {
        for (const uint64_t Output : {uint64_t(0), uint64_t(8)}) {
          SymContext C;
          SymState S(C, Order);
          SymExec E(C, S);
          const auto Root = C.mkVar("observer", 64);
          const llvm::APInt Old(Bytes * 8, UINT64_MAX - 2);
          const llvm::APInt Value(Bytes * 8, 7);
          const auto Result =
              C.mkConst(Opcode == NdOp::ATOMIC_ADD ? Old + Value : Value);
          S.store(C.mkConst(64, 0x4000), C.mkConst(Old));
          S.load(Root, Bytes);
          S.write(SymSpace::Register, 0, C.mkConst(Value));
          S.write(SymSpace::Register, 8, C.mkConst(64, 0x4000));
          auto Op = memoryOp(Opcode, NdVar::reg(Output, Bytes),
                             {NdVar::reg(8, 8), NdVar::reg(0, Bytes)});
          Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
          SymConditionalStore Control{1000};
          EXPECT_TRUE(supportsAtomicWritePreservation(Op));
          EXPECT_FALSE(supportsCmpxchgPreservation(Op));
          EXPECT_EQ(E.stepWithConditionalStore(Op, Control),
                    StepResult::Continue);
          ASSERT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
          EXPECT_EQ(S.read(SymSpace::Register, Output, Bytes), C.mkConst(Old));
          EXPECT_EQ(S.load(C.mkConst(64, 0x4000), Bytes), Result);
          EXPECT_EQ(E.opaqueOperationCount(), 0u);
          for (unsigned I = 0; I != Bytes; ++I) {
            const unsigned Lane =
                Order == llvm::endianness::little ? I : unsigned(Bytes) - I - 1;
            expectUnsat(
                C, C.mkAnd(C.mkEq(Root, C.mkConst(64, 0x4000)),
                           C.mkNe(S.load(C.mkAdd(Root, C.mkConst(64, I)), 1),
                                  C.mkExtract(Result, Lane * 8, 8))));
          }
        }
      }
    }
  }
}

TEST(SymConditionalStore, AtomicWriteBudgetRefusalDoesNotPublishOld) {
  for (const auto Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG}) {
    SymContext C;
    SymState Initial(C);
    Initial.store(C.mkConst(64, 0x4000), C.mkConst(32, 7));
    Initial.load(C.mkVar("observer", 64), 4);
    auto Op = memoryOp(Opcode, NdVar::reg(0, 4),
                       {NdVar::scalar(0x4000, 8), NdVar::scalar(11, 4)});
    Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    for (const uint64_t Budget : {uint64_t(0), uint64_t(33), uint64_t(34)}) {
      auto S = Initial;
      SymExec E(C, S);
      SymConditionalStore Control{Budget};
      const auto Flow = E.stepWithConditionalStore(Op, Control);
      EXPECT_LE(Control.WorkUsed, Budget);
      if (Budget == 34) {
        EXPECT_EQ(Flow, StepResult::Continue);
        EXPECT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
        EXPECT_EQ(Control.WorkUsed, 34u);
        EXPECT_EQ(S.read(SymSpace::Register, 0, 4), C.mkConst(32, 7));
        EXPECT_EQ(S.load(C.mkConst(64, 0x4000), 4),
                  C.mkConst(32, Opcode == NdOp::ATOMIC_ADD ? 18 : 11));
      } else {
        EXPECT_EQ(Flow, StepResult::Unmodelled);
        EXPECT_EQ(Control.Status, SymConditionalStoreStatus::BudgetExceeded);
        EXPECT_EQ(E.opaqueOperationCount(), 1u);
        EXPECT_EQ(E.memoryHavocCount(), 1u);
        EXPECT_FALSE(C.isConst(S.read(SymSpace::Register, 0, 4)));
        EXPECT_FALSE(C.isConst(S.load(C.mkConst(64, 0x4000), 4)));
      }
    }
  }
}

TEST(SymConditionalStore, AtomicWriteRejectsMalformedPolicyShapes) {
  for (const auto Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG}) {
    for (unsigned Case = 0; Case != 8; ++Case) {
      SymContext C;
      SymState S(C);
      SymExec E(C, S);
      S.store(C.mkConst(64, 0x4000), C.mkConst(64, 7));
      auto Op = memoryOp(Opcode, NdVar::reg(0, 4),
                         {NdVar::scalar(0x4000, 8), NdVar::scalar(11, 4)});
      Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      switch (Case) {
      case 0:
        Op.MemoryOrdering = NdMemoryOrdering::None;
        break;
      case 1:
        Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
        break;
      case 2:
        Op.Inputs[0].Size = 4;
        break;
      case 3:
        Op.Output.Size = Op.Inputs[1].Size = 16;
        break;
      case 4:
        Op.NumInputs = 1;
        break;
      case 5:
        Op.Inputs[1] = NdVar::ram(0x5000, 4);
        break;
      case 6:
        Op.Output = NdVar::ram(0x5000, 4);
        break;
      case 7:
        Op.Inputs[1].Size = 2;
        break;
      }
      EXPECT_FALSE(supportsAtomicWritePreservation(Op));
      SymConditionalStore Control{1000};
      EXPECT_EQ(E.stepWithConditionalStore(Op, Control),
                StepResult::Unmodelled);
      EXPECT_EQ(Control.Status, SymConditionalStoreStatus::Invalid);
      EXPECT_EQ(Control.WorkUsed, 0u);
      EXPECT_EQ(E.memoryHavocCount(), 1u);
      EXPECT_EQ(E.opaqueOperationCount(), 1u);
      EXPECT_FALSE(C.isConst(S.load(C.mkConst(64, 0x4000), 4)));
    }
  }
}

TEST(SymConditionalStore, ExecutorStoreDistinguishesAppliedFromRefusedEffects) {
  SymContext C;
  SymState Initial(C);
  const auto A = C.mkVar("observer", 64);
  const auto D = C.mkVar("destination", 64);
  Initial.load(A, 4);
  Initial.write(SymSpace::Register, 8, D);
  const auto Op =
      memoryOp(NdOp::STORE, {}, {NdVar::reg(8, 8), NdVar::scalar(19, 2)});
  for (const uint64_t Budget : {uint64_t(0), uint64_t(23), uint64_t(24)}) {
    auto S = Initial;
    SymExec E(C, S);
    SymConditionalStore Control{Budget};
    const auto Flow = E.stepWithConditionalStore(Op, Control);
    EXPECT_LE(Control.WorkUsed, Budget);
    // The legacy symbolic-write counter alone cannot distinguish a committed
    // conditional effect from conservative refusal.
    EXPECT_EQ(E.memoryHavocCount(), 1u);
    if (Budget == 24) {
      EXPECT_EQ(Flow, StepResult::Continue);
      EXPECT_EQ(Control.Status, SymConditionalStoreStatus::Applied);
      EXPECT_EQ(Control.WorkUsed, 24u);
      EXPECT_EQ(E.opaqueOperationCount(), 0u);
      EXPECT_EQ(S.load(D, 2), C.mkConst(16, 19));
      expectUnsat(
          C, C.mkAnd(C.mkEq(A, D), C.mkNe(S.load(A, 2), C.mkConst(16, 19))));
    } else {
      EXPECT_EQ(Flow, StepResult::Unmodelled);
      EXPECT_EQ(Control.Status, SymConditionalStoreStatus::BudgetExceeded);
      EXPECT_EQ(E.opaqueOperationCount(), 1u);
      EXPECT_FALSE(C.isConst(S.load(D, 2)));
    }
  }
}

TEST(SymConditionalStore, ExecutorRejectsNonStrictCmpxchgShapes) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    SymContext C;
    SymState S(C);
    SymExec E(C, S);
    S.store(C.mkConst(64, 0x4000), C.mkConst(64, 7));
    auto Op = cmpxchg(NdVar::reg(0, 4), NdVar::scalar(0x4000, 8),
                      NdVar::scalar(7, 4), NdVar::scalar(11, 4));
    switch (Case) {
    case 0:
      Op.MemoryOrdering = NdMemoryOrdering::None;
      break;
    case 1:
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 2:
      Op.Inputs[0].Size = 4;
      break;
    case 3:
      Op.Output.Size = Op.Inputs[1].Size = Op.Inputs[2].Size = 16;
      break;
    case 4:
      Op.NumInputs = 2;
      break;
    case 5:
      Op.Inputs[1] = NdVar::ram(0x5000, 4);
      break;
    }
    SymConditionalStore Control{1000};
    EXPECT_EQ(E.stepWithConditionalStore(Op, Control), StepResult::Unmodelled);
    EXPECT_EQ(Control.Status, SymConditionalStoreStatus::Invalid);
    EXPECT_EQ(Control.WorkUsed, 0u);
    EXPECT_EQ(E.opaqueOperationCount(), 1u);
    EXPECT_FALSE(C.isConst(S.read(SymSpace::Register, 0, Op.Output.Size)));
  }
}

TEST(SymConditionalStore, OtherOpcodesIgnorePolicyAndOldDefaultsKeepCosts) {
  SymContext C;
  SymState Initial(C);
  const auto A = C.mkVar("observer", 64);
  const auto D = C.mkVar("destination", 64);
  const auto Old = Initial.load(A, 4);
  auto Ordinary = Initial;
  Ordinary.store(D, C.mkConst(16, 19));
  EXPECT_EQ(Ordinary.numLiveBytes(), 2u);
  EXPECT_NE(Ordinary.load(A, 4), Old);
  auto Preserved = Initial;
  SymStorePreservation P;
  P.Base = A;
  P.End = 4;
  P.MaxWork = 12;
  Preserved.store(D, C.mkConst(16, 19), P);
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Applied);
  EXPECT_EQ(P.WorkUsed, 12u);
  EXPECT_EQ(Preserved.load(A, 4), Old);

  for (const auto Opcode : {NdOp::COPY, NdOp::INT_ADD}) {
    SymState S(C);
    SymExec E(C, S);
    S.store(C.mkConst(64, 0x4000), C.mkConst(32, 7));
    auto Op = memoryOp(Opcode, NdVar::reg(0, 4),
                       {NdVar::scalar(7, 4), NdVar::scalar(11, 4)});
    if (Opcode == NdOp::COPY)
      Op = memoryOp(Opcode, NdVar::reg(0, 4), {NdVar::scalar(11, 4)});
    SymConditionalStore Control{0, 99, SymConditionalStoreStatus::Applied};
    EXPECT_EQ(E.stepWithConditionalStore(Op, Control), StepResult::Continue);
    EXPECT_EQ(Control.Status, SymConditionalStoreStatus::Invalid);
    EXPECT_EQ(Control.WorkUsed, 0u);
    EXPECT_EQ(S.read(SymSpace::Register, 0, 4),
              C.mkConst(32, Opcode == NdOp::COPY ? 11 : 18));
    EXPECT_EQ(S.load(C.mkConst(64, 0x4000), 4), C.mkConst(32, 7));
  }
}

} // namespace
