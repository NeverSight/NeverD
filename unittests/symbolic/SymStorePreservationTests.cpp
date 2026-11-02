//===- SymStorePreservationTests.cpp - Explicit store contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <limits>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

TEST(SymStorePreservation, AbsoluteCellsNeedAnExplicitPerWriteContract) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto Address = C.mkConst(64, 0x1000);
    const auto Pointer = C.mkVar("pointer", 64);
    ASSERT_TRUE(S.store(Address, C.mkConst(64, 0x123456789abcdef0)));
    const auto Before = S;
    SymStorePreservation P{{}, 0x1000, 0x1008, 23};
    EXPECT_FALSE(S.store(Pointer, C.mkConst(8, 7), P));
    EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
    EXPECT_TRUE(S.mergeIdentical(Before));
    P.MaxWork = 24;
    EXPECT_FALSE(S.store(Pointer, C.mkConst(8, 7), P));
    ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
    EXPECT_EQ(P.WorkUsed, 24u);
    EXPECT_EQ(S.load(Address, 8), C.mkConst(64, 0x123456789abcdef0));
    EXPECT_EQ(S.load(Pointer, 1), C.mkConst(8, 7));
    EXPECT_TRUE(S.memoryIsUnknown());
    EXPECT_TRUE(S.memoryInputOrigins(S.load(C.mkConst(64, 0x1008), 1)).empty());
    S.store(C.mkVar("next_pointer", 64), C.mkConst(8, 9));
    EXPECT_FALSE(C.isConst(S.load(Address, 8)));
  }
}

TEST(SymStorePreservation, AbsoluteRangesCannotProtectTheirOwnWriteBank) {
  SymContext C;
  SymState S(C);
  const auto Address = C.mkConst(64, 0x1000);
  S.store(Address, C.mkConst(64, 11));
  const auto Before = S;
  SymStorePreservation P{{}, 0x1000, 0x1008, 100};
  EXPECT_FALSE(S.store(C.mkConst(64, 0x2000), C.mkConst(64, 17), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
  EXPECT_TRUE(S.mergeIdentical(Before));
  const auto Root = C.mkVar("root", 64);
  const SymStorePreservationRegion More[] = {{{}, 0x1000, 0x1008}};
  P = {Root, -8, 8, 100};
  P.AdditionalRegions = More;
  EXPECT_FALSE(S.store(Address, C.mkConst(64, 17), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
  EXPECT_TRUE(S.mergeIdentical(Before));
}

TEST(SymStorePreservation, AbsoluteAndSymbolicCellsShareOnePreservation) {
  SymContext C;
  SymState S(C);
  const auto Root = C.mkVar("root", 64);
  const auto Other = C.mkVar("other", 64);
  ASSERT_TRUE(
      S.restoreMemoryKnowledge({&C,
                                llvm::endianness::little,
                                {{{}, UINT64_MAX - 7, C.mkConst(64, 19)},
                                 {Root, 0, C.mkConst(64, 23)}}}));
  const SymStorePreservationRegion More[] = {{{}, -8, 0}};
  SymStorePreservation P{Root, 0, 8, 49};
  P.AdditionalRegions = More;
  EXPECT_FALSE(S.store(Other, C.mkConst(8, 31), P));
  ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
  EXPECT_EQ(P.WorkUsed, 49u);
  EXPECT_EQ(S.load(C.mkConst(64, UINT64_MAX - 7), 8), C.mkConst(64, 19));
  EXPECT_EQ(S.load(Root, 8), C.mkConst(64, 23));
}

TEST(SymState, AbsoluteProjectionsArePureBoundedAndKeepByteOrder) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    S.store(C.mkConst(64, 0x1000), C.mkConst(32, 0x12345678));
    S.load(C.mkConst(64, 0x1008), 2);
    const auto Nodes = C.numNodes();
    const auto Live = S.numLiveBytes();
    const auto Constants = S.constantAbsoluteBytes();
    ASSERT_EQ(Constants.size(), 4u);
    for (unsigned I = 0; I != 4; ++I) {
      EXPECT_EQ(Constants[I].Offset, 0x1000u + I);
      const unsigned Shift =
          (Order == llvm::endianness::little ? I : 3 - I) * 8;
      EXPECT_EQ(Constants[I].Value, uint8_t(0x12345678u >> Shift));
    }
    uint64_t Work = 6;
    EXPECT_FALSE(S.materializedAbsoluteRanges(Work));
    Work = 7;
    const auto Ranges = S.materializedAbsoluteRanges(Work);
    ASSERT_TRUE(Ranges);
    EXPECT_EQ(Work, 0u);
    ASSERT_EQ(Ranges->size(), 2u);
    EXPECT_EQ((*Ranges)[0].Offset, 0x1000u);
    EXPECT_EQ((*Ranges)[0].Bytes, 4u);
    EXPECT_EQ((*Ranges)[1].Offset, 0x1008u);
    EXPECT_EQ((*Ranges)[1].Bytes, 2u);
    EXPECT_EQ(C.numNodes(), Nodes);
    EXPECT_EQ(S.numLiveBytes(), Live);
    S.clobberMemory();
    EXPECT_TRUE(S.constantAbsoluteBytes().empty());
  }
}

TEST(SymStorePreservation, RetainsOnlyKnownBytesAndKeepsPostStoreEpochs) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto Root = C.mkVar("root", 64);
    const auto External = C.mkVar("external", 64);
    const auto At = [&](int64_t Offset) {
      return C.mkAdd(Root, C.mkConst(64, static_cast<uint64_t>(Offset)));
    };
    S.store(At(-8), C.mkConst(64, 0x123456789abcdef0));
    const auto EntryInput = S.load(Root, 4);
    S.store(At(8), C.mkConst(32, 99));
    SymState Before = S;
    const auto Live = S.numLiveBytes();
    SymStorePreservation P{Root, -8, 8, 40};
    EXPECT_FALSE(S.store(External, C.mkConst(32, 7), P));
    ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
    EXPECT_EQ(P.WorkUsed, 40u); // 16 inspected, 12 saved and restored.
    EXPECT_EQ(S.numLiveBytes(), Live);
    EXPECT_EQ(S.load(At(-8), 8), C.mkConst(64, 0x123456789abcdef0));
    EXPECT_EQ(S.load(Root, 4), EntryInput);
    EXPECT_EQ(S.load(External, 4), C.mkConst(32, 7));
    const auto Missing = S.load(At(4), 4);
    EXPECT_NE(Missing, Before.load(At(4), 4));
    EXPECT_TRUE(S.memoryInputOrigins(Missing).empty());
    EXPECT_FALSE(C.isConst(S.load(At(8), 4)));
    const auto Other = C.mkVar("unseen", 64);
    EXPECT_NE(S.load(Other, 1), Before.load(Other, 1));
    EXPECT_TRUE(S.memoryIsUnknown());
  }
}

TEST(SymStorePreservation, HalfOpenBoundsPreservePartialWordsInBothByteOrders) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto Root = C.mkVar("root", 64);
    const auto At = [&](int64_t Offset) {
      return C.mkAdd(Root, C.mkConst(64, static_cast<uint64_t>(Offset)));
    };
    S.store(At(-8), C.mkConst(64, 0x1020304050607080));
    SymStorePreservation P{Root, -6, -2, 16};
    EXPECT_TRUE(S.store(C.mkConst(64, 0x30000), C.mkConst(8, 9), P));
    ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
    EXPECT_EQ(P.WorkUsed, 16u);
    EXPECT_EQ(S.load(At(-6), 4), C.mkConst(32, 0x30405060));
    EXPECT_FALSE(C.isConst(S.load(At(-7), 1)));
    EXPECT_FALSE(C.isConst(S.load(At(-2), 1)));
    EXPECT_EQ(S.load(C.mkConst(64, 0x30000), 1), C.mkConst(8, 9));
  }
}

TEST(SymStorePreservation, ExactBudgetAppliesAndOneShortDoesNotMutate) {
  SymContext C;
  SymState S(C);
  const auto Root = C.mkVar("root", 64);
  const auto External = C.mkVar("external", 64);
  S.store(Root, C.mkConst(64, 11));
  SymState Before = S;
  SymStorePreservation P{Root, 0, 8, 23};
  EXPECT_FALSE(S.store(External, C.mkConst(64, 17), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
  EXPECT_LE(P.WorkUsed, 23u);
  EXPECT_TRUE(S.mergeIdentical(Before));
  P.MaxWork = 24;
  EXPECT_FALSE(S.store(External, C.mkConst(64, 17), P));
  ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
  EXPECT_EQ(P.WorkUsed, 24u);
  EXPECT_EQ(S.load(Root, 8), C.mkConst(64, 11));
  EXPECT_EQ(S.load(External, 8), C.mkConst(64, 17));
}

TEST(SymStorePreservation, InvalidContractsAndWritesDoNotMutate) {
  SymContext C;
  SymState S(C);
  const auto Root = C.mkVar("root", 64);
  const auto External = C.mkVar("external", 64);
  S.store(Root, C.mkConst(64, 11));
  SymState Before = S;
  const auto Reject = [&](SymRef Address, SymRef Value,
                          SymStorePreservation P) {
    EXPECT_FALSE(S.store(Address, Value, P));
    EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
    EXPECT_TRUE(S.mergeIdentical(Before));
  };
  SymStorePreservation P{Root, 0, 8, 100};
  Reject(Root, C.mkConst(64, 17), P);
  Reject(C.mkAdd(Root, C.mkConst(64, 32)), C.mkConst(64, 17), P);
  Reject(C.mkConst(64, UINT64_MAX), C.mkConst(16, 17), P);
  Reject(External, C.mkConst(3, 7), P);
  Reject(C.mkVar("narrow", 32), C.mkConst(64, 17), P);
  P.Begin = P.End;
  Reject(External, C.mkConst(64, 17), P);
  P.Begin = 0;
  P.Base = C.mkConst(64, 1);
  Reject(External, C.mkConst(64, 17), P);
}

TEST(SymStorePreservation, HugeAbsentRangeMaterializesNoProtectedMemory) {
  SymContext C;
  SymState S(C);
  const auto Root = C.mkVar("root", 64);
  const auto External = C.mkVar("external", 64);
  SymStorePreservation P{Root, INT64_MIN, INT64_MAX, 0};
  EXPECT_FALSE(S.store(External, C.mkConst(8, 17), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Applied);
  EXPECT_EQ(P.WorkUsed, 0u);
  EXPECT_EQ(S.numMemoryRegions(), 1u);
  EXPECT_EQ(S.numLiveBytes(), 1u);
  EXPECT_TRUE(S.constantRegionBytes(Root).empty());
}

TEST(SymStorePreservation, ExecutorDoesNotPersistContractIntoLaterOperations) {
  SymContext C;
  SymState S(C);
  SymExec E(C, S);
  const auto Root = C.mkVar("root", 64);
  S.store(Root, C.mkConst(64, 11));
  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(NdVar::reg(8, 8));
  Store.addInput(NdVar::scalar(17, 8));
  SymStorePreservation P{Root, 0, 8, 24};
  EXPECT_EQ(E.step(Store, nullptr, &P), StepResult::Continue);
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Applied);
  EXPECT_EQ(S.load(Root, 8), C.mkConst(64, 11));
  EXPECT_EQ(E.step(Store), StepResult::Continue);
  EXPECT_FALSE(C.isConst(S.load(Root, 8)));
  Store.Output = NdVar::reg(16, 8);
  Store.Opcode = NdOp::ATOMIC_ADD;
  EXPECT_EQ(E.step(Store), StepResult::Continue);
  EXPECT_EQ(S.read(SymSpace::Register, 16, 8), C.mkConst(64, 17));
  EXPECT_EQ(S.load(S.read(SymSpace::Register, 8, 8), 8), C.mkConst(64, 34));
}

TEST(SymStorePreservation,
     AtomicAddAndExchangeShareWriteBudgetAndOldPublication) {
  for (const auto Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG}) {
    for (const auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      for (const uint16_t Bytes : {1, 2, 4, 8}) {
        SymContext C;
        SymState Initial(C, Order);
        const auto Root = C.mkVar("frame", 64);
        const auto Address = C.mkConst(64, 0x4000);
        Initial.store(Root, C.mkConst(64, 19));
        SymStorePreservation Seed{Root, 0, 8, 24};
        Initial.store(Address, C.mkConst(Bytes * 8, 7), Seed);
        ASSERT_EQ(Seed.Status, SymStorePreservationStatus::Applied);
        Initial.write(SymSpace::Register, 0, C.mkConst(Bytes * 8, 11));
        LowOp Op;
        Op.Opcode = Opcode;
        Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
        Op.Output = NdVar::reg(0, Bytes);
        Op.addInput(NdVar::scalar(0x4000, 8));
        Op.addInput(NdVar::reg(0, Bytes));
        for (const uint64_t Budget : {uint64_t(23), uint64_t(24)}) {
          auto S = Initial;
          SymExec E(C, S);
          SymStorePreservation P{Root, 0, 8, Budget};
          const auto Flow = E.step(Op, nullptr, &P);
          EXPECT_LE(P.WorkUsed, Budget);
          if (Budget == 24) {
            EXPECT_EQ(Flow, StepResult::Continue);
            EXPECT_EQ(P.Status, SymStorePreservationStatus::Applied);
            EXPECT_EQ(P.WorkUsed, 24u);
            EXPECT_EQ(S.read(SymSpace::Register, 0, Bytes),
                      C.mkConst(Bytes * 8, 7));
            EXPECT_EQ(
                S.load(Address, Bytes),
                C.mkConst(Bytes * 8, Opcode == NdOp::ATOMIC_ADD ? 18 : 11));
            EXPECT_EQ(S.load(Root, 8), C.mkConst(64, 19));
            EXPECT_EQ(E.opaqueOperationCount(), 0u);
          } else {
            EXPECT_EQ(Flow, StepResult::Unmodelled);
            EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
            EXPECT_EQ(E.opaqueOperationCount(), 1u);
            EXPECT_EQ(E.memoryHavocCount(), 1u);
            EXPECT_FALSE(C.isConst(S.read(SymSpace::Register, 0, Bytes)));
            EXPECT_FALSE(C.isConst(S.load(Address, Bytes)));
          }
        }
      }
    }
  }
}

TEST(SymStorePreservation, AdditionalRangesRetainTheUnionAndDiscardItsGaps) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const auto Root = C.mkVar("root", 64);
    const auto At = [&](int64_t Offset) {
      return C.mkAdd(Root, C.mkConst(64, static_cast<uint64_t>(Offset)));
    };
    S.store(At(-24), C.mkConst(64, 0x123456789abcdef0));
    S.store(At(-16), C.mkConst(64, 31));
    const auto OldGap = S.load(At(-8), 8);
    const auto OldProtected = S.load(Root, 8);
    const auto Before = S;
    const SymStorePreservationRange More[] = {{0, 8}, {24, 32}, {-24, -20}};
    SymStorePreservation P{Root, -24, -16, UINT64_MAX};
    P.AdditionalRanges = More;
    EXPECT_TRUE(S.store(C.mkConst(64, 0x30000), C.mkConst(32, 17), P));
    ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
    EXPECT_EQ(S.load(At(-24), 8), C.mkConst(64, 0x123456789abcdef0));
    EXPECT_EQ(S.load(Root, 8), OldProtected);
    EXPECT_FALSE(C.isConst(S.load(At(-16), 8)));
    EXPECT_NE(S.load(At(-8), 8), OldGap);
    EXPECT_EQ(S.load(C.mkConst(64, 0x30000), 4), C.mkConst(32, 17));
    EXPECT_TRUE(llvm::none_of(S.constantRegionBytes(Root), [](const auto &B) {
      return B.Offset >= 24 && B.Offset < 32;
    }));
    auto UnseenBefore = Before;
    EXPECT_NE(S.load(At(24), 8), UnseenBefore.load(At(24), 8));
  }
}

TEST(SymStorePreservation, AdditionalRangeValidationAndBudgetNeverMutateEarly) {
  SymContext C;
  SymState S(C);
  const auto Root = C.mkVar("root", 64);
  const auto External = C.mkVar("external", 64);
  S.store(Root, C.mkConst(64, 11));
  const auto Before = S;
  const SymStorePreservationRange Invalid[] = {{-16, -8}, {7, 7}};
  SymStorePreservation P{Root, 0, 8, 1};
  P.AdditionalRanges = Invalid;
  EXPECT_FALSE(S.store(External, C.mkConst(32, 17), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
  EXPECT_LE(P.WorkUsed, P.MaxWork);
  EXPECT_TRUE(S.mergeIdentical(Before));
  P.MaxWork = 100;
  EXPECT_FALSE(S.store(External, C.mkConst(32, 17), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
  EXPECT_TRUE(S.mergeIdentical(Before));
}

TEST(SymStorePreservation, AdditionalRangesShareExactMembershipAndSaveBudget) {
  SymContext C;
  SymState Original(C);
  const auto Root = C.mkVar("root", 64);
  const auto External = C.mkVar("external", 64);
  const auto At = [&](int64_t Offset) {
    return C.mkAdd(Root, C.mkConst(64, static_cast<uint64_t>(Offset)));
  };
  Original.store(At(-24), C.mkConst(64, 11));
  Original.store(At(-16), C.mkConst(64, 13));
  Original.store(Root, C.mkConst(64, 17));
  const SymStorePreservationRange More[] = {{0, 8}, {16, 24}};
  SymStorePreservation P{Root, -24, -16, UINT64_MAX};
  P.AdditionalRanges = More;
  auto Good = Original;
  EXPECT_FALSE(Good.store(External, C.mkConst(32, 19), P));
  ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
  ASSERT_GT(P.WorkUsed, 0u);
  const auto ExactWork = P.WorkUsed;
  P.MaxWork = ExactWork;
  auto Exact = Original;
  EXPECT_FALSE(Exact.store(External, C.mkConst(32, 19), P));
  ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
  EXPECT_EQ(Exact.load(At(-24), 8), C.mkConst(64, 11));
  EXPECT_EQ(Exact.load(Root, 8), C.mkConst(64, 17));
  --P.MaxWork;
  auto Short = Original;
  EXPECT_FALSE(Short.store(External, C.mkConst(32, 19), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
  EXPECT_LE(P.WorkUsed, P.MaxWork);
  EXPECT_TRUE(Short.mergeIdentical(Original));
}

TEST(SymStorePreservation, AbsentAdditionalRangesStillValidateWithoutReading) {
  SymContext C;
  SymState S(C);
  const auto Root = C.mkVar("root", 64);
  const auto External = C.mkVar("external", 64);
  const SymStorePreservationRange More[] = {{-16, -8}, {8, 16}};
  SymStorePreservation P{Root, -8, 0, 1};
  P.AdditionalRanges = More;
  EXPECT_FALSE(S.store(External, C.mkConst(32, 17), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
  EXPECT_EQ(S.numLiveBytes(), 0u);
  EXPECT_EQ(S.numMemoryRegions(), 0u);
  P.MaxWork = 2;
  EXPECT_FALSE(S.store(External, C.mkConst(32, 17), P));
  ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
  EXPECT_EQ(P.WorkUsed, 2u);
  EXPECT_EQ(S.numMemoryRegions(), 1u); // only the STORE's external bank
  EXPECT_EQ(S.numLiveBytes(), 4u);
  EXPECT_TRUE(S.constantRegionBytes(Root).empty());
}

LowOp preservingCmpxchg(NdVar Output, NdVar Address, NdVar Expected,
                        NdVar Desired) {
  LowOp Op;
  Op.Opcode = NdOp::ATOMIC_CMPXCHG;
  Op.Output = Output;
  Op.addInput(Address);
  Op.addInput(Expected);
  Op.addInput(Desired);
  Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  return Op;
}

// Construct input words from independently specified memory bytes. The oracle
// chooses a whole byte vector by equality, without using symbolic operations.
llvm::APInt atomicByteWord(llvm::ArrayRef<uint8_t> Bytes,
                           llvm::endianness Order) {
  llvm::APInt Word(Bytes.size() * 8, 0);
  for (unsigned I = 0; I != Bytes.size(); ++I) {
    const unsigned Lane =
        Order == llvm::endianness::little ? I : Bytes.size() - I - 1;
    Word |= llvm::APInt(Word.getBitWidth(), Bytes[I]).shl(Lane * 8);
  }
  return Word;
}

TEST(SymAtomicCmpxchg,
     SymbolicComparisonPreservesCompleteBytesAndOnlyKnownCells) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint16_t Bytes : {1, 2, 4, 8}) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
      SymContext C;
      SymState S(C, Order);
      SymExec E(C, S);
      const auto Root = C.mkVar("frame", 64);
      const auto Base = C.mkVar("object", 64);
      const auto At = [&](SymRef R, int64_t Offset) {
        return C.mkAdd(R, C.mkConst(64, static_cast<uint64_t>(Offset)));
      };
      const std::vector<uint8_t> Frame{0x10, 0x32, 0x54, 0x76,
                                       0x98, 0xba, 0xdc, 0xfe};
      S.store(Root, C.mkConst(atomicByteWord(Frame, Order)));
      std::vector<uint8_t> Old, Desired;
      for (unsigned I = 0; I != Bytes; ++I) {
        Old.push_back(0x31 + 7 * I);
        Desired.push_back(0xe2 - 9 * I);
      }
      std::vector<uint8_t> Initial{0xa6};
      Initial.insert(Initial.end(), Old.begin(), Old.end());
      Initial.push_back(0x5d);
      SymStorePreservation Seed{Root, 0, 8, 24};
      S.store(At(Base, -2), C.mkConst(atomicByteWord(Initial, Order)), Seed);
      ASSERT_EQ(Seed.Status, SymStorePreservationStatus::Applied);
      const auto ExpectedInput = C.mkVar("expected", Bytes * 8);
      const auto DesiredInput = C.mkVar("desired", Bytes * 8);
      S.write(SymSpace::Register, 8, At(Base, -1));
      S.write(SymSpace::Register, 16, ExpectedInput);
      S.write(SymSpace::Register, 24, DesiredInput);
      const auto Op =
          preservingCmpxchg(NdVar::reg(0, Bytes), NdVar::reg(8, 8),
                            NdVar::reg(16, Bytes), NdVar::reg(24, Bytes));
      const SymStorePreservationRange More[] = {{6, 10}};
      // One descriptor, eight byte visits, one cursor advance and four
      // saved/restored bytes. Missing protected bytes are not initialized.
      constexpr uint64_t PreservationWork = 1 + 8 + 1 + 2 * 4;
      SymStorePreservation P{Root, 0, 2, PreservationWork};
      P.AdditionalRanges = More;
      const auto BeforeLive = S.numLiveBytes();
      ASSERT_EQ(E.step(Op, nullptr, &P), StepResult::Continue);
      ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
      EXPECT_EQ(P.WorkUsed, PreservationWork);
      EXPECT_EQ(E.opaqueOperationCount(), 0u);
      EXPECT_EQ(S.numLiveBytes(), BeforeLive + Bytes - 4);
      for (unsigned I = 0; I != 8; ++I) {
        const auto Byte = S.load(At(Root, I), 1);
        if (I < 2 || I >= 6)
          EXPECT_EQ(Byte, C.mkConst(8, Frame[I]));
        else
          EXPECT_FALSE(C.isConst(Byte));
      }
      EXPECT_EQ(S.load(At(Base, -2), 1), C.mkConst(8, Initial.front()));
      EXPECT_EQ(S.load(At(Base, Bytes - 1), 1), C.mkConst(8, Initial.back()));
      for (unsigned I = 0; I != Bytes; ++I)
        EXPECT_EQ(S.read(SymSpace::Register, I, 1), C.mkConst(8, Old[I]));
      for (bool Match : {false, true}) {
        auto Expected = Old;
        if (!Match)
          Expected.back() ^= 0x80;
        const auto &Oracle = Expected == Old ? Desired : Old;
        const uint64_t ExpectedWord =
            atomicByteWord(Expected, Order).getZExtValue();
        const uint64_t DesiredWord =
            atomicByteWord(Desired, Order).getZExtValue();
        for (unsigned I = 0; I != Bytes; ++I) {
          const auto Byte = S.load(At(Base, int64_t(I) - 1), 1);
          ASSERT_TRUE(Byte);
          SymEvalPlan Plan(C, Byte);
          const auto Actual = Plan.evalU64With([&](uint32_t Id) {
            if (Id == C.varId(ExpectedInput))
              return ExpectedWord;
            if (Id == C.varId(DesiredInput))
              return DesiredWord;
            ADD_FAILURE() << "unexpected input in the stored byte: " << Id;
            return uint64_t{0};
          });
          EXPECT_EQ(Actual, Oracle[I]);
        }
      }
    }
}

TEST(SymAtomicCmpxchg, SnapshotsAliasedInputsBeforePublishingOld) {
  constexpr uint64_t Old = 0x1111111111111111;
  constexpr uint64_t Desired = 0x2222222222222222;
  struct AliasCase {
    uint64_t Output;
    uint64_t Expected;
  };
  // Address, expected, desired, then a result overlapping both value inputs.
  for (const auto Case :
       {AliasCase{8, Old}, AliasCase{16, Old ^ 1}, AliasCase{24, Old},
        AliasCase{20, Old ^ (uint64_t{1} << 40)}}) {
    SCOPED_TRACE(Case.Output);
    SymContext C;
    SymState S(C);
    SymExec E(C, S);
    const auto Root = C.mkVar("frame", 64);
    const auto Address = C.mkConst(64, 0x4000);
    S.store(Root, C.mkConst(64, 0xa5a5a5a5a5a5a5a5));
    SymStorePreservation Seed{Root, 0, 8, 24};
    S.store(Address, C.mkConst(64, Old), Seed);
    ASSERT_EQ(Seed.Status, SymStorePreservationStatus::Applied);
    S.write(SymSpace::Register, 8, Address);
    S.write(SymSpace::Register, 16, C.mkConst(64, Case.Expected));
    S.write(SymSpace::Register, 24, C.mkConst(64, Desired));
    const auto Op =
        preservingCmpxchg(NdVar::reg(Case.Output, 8), NdVar::reg(8, 8),
                          NdVar::reg(16, 8), NdVar::reg(24, 8));
    SymStorePreservation P{Root, 0, 8, 24};
    ASSERT_EQ(E.step(Op, nullptr, &P), StepResult::Continue);
    ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
    EXPECT_EQ(S.read(SymSpace::Register, Case.Output, 8), C.mkConst(64, Old));
    EXPECT_EQ(S.load(Address, 8),
              C.mkConst(64, Case.Expected == Old ? Desired : Old));
    EXPECT_EQ(S.load(Root, 8), C.mkConst(64, 0xa5a5a5a5a5a5a5a5));
  }
}

TEST(SymAtomicCmpxchg, ExactBudgetCommitsAndShortBudgetPublishesNoOldResult) {
  SymContext C;
  SymState Initial(C);
  const auto Root = C.mkVar("frame", 64);
  const auto Address = C.mkConst(64, 0x4000);
  Initial.store(Root, C.mkConst(64, 19));
  SymStorePreservation Seed{Root, 0, 8, 24};
  Initial.store(Address, C.mkConst(32, 7), Seed);
  ASSERT_EQ(Seed.Status, SymStorePreservationStatus::Applied);
  Initial.write(SymSpace::Register, 0, C.mkConst(32, 0xdeadbeef));
  const auto Op = preservingCmpxchg(NdVar::reg(0, 4), NdVar::scalar(0x4000, 8),
                                    NdVar::scalar(7, 4), NdVar::scalar(11, 4));
  for (uint64_t Budget : {24, 23}) {
    SCOPED_TRACE(Budget);
    auto S = Initial;
    SymExec E(C, S);
    SymStorePreservation P{Root, 0, 8, Budget};
    const auto Flow = E.step(Op, nullptr, &P);
    if (Budget == 24) {
      EXPECT_EQ(Flow, StepResult::Continue);
      EXPECT_EQ(P.Status, SymStorePreservationStatus::Applied);
      EXPECT_EQ(P.WorkUsed, 24u);
      EXPECT_EQ(S.read(SymSpace::Register, 0, 4), C.mkConst(32, 7));
      EXPECT_EQ(S.load(Address, 4), C.mkConst(32, 11));
      EXPECT_EQ(S.load(Root, 8), C.mkConst(64, 19));
      EXPECT_EQ(E.opaqueOperationCount(), 0u);
    } else {
      EXPECT_EQ(Flow, StepResult::Unmodelled);
      EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
      EXPECT_LE(P.WorkUsed, Budget);
      EXPECT_EQ(E.opaqueOperationCount(), 1u);
      const auto Output = S.read(SymSpace::Register, 0, 4);
      EXPECT_FALSE(C.isConst(Output));
      EXPECT_NE(Output, C.mkConst(32, 7));
      EXPECT_FALSE(C.isConst(S.load(Address, 4)));
      EXPECT_TRUE(S.memoryIsUnknown());
      // Refusal may havoc memory and output. It is not a transaction rollback.
    }
  }
}

TEST(SymAtomicCmpxchg, InvalidPreservationShapesAndContractsFailClosed) {
  enum class Invalid {
    NoOrdering,
    Relaxed,
    UnknownOrdering,
    Segment,
    NarrowAddress,
    OddWidth,
    WideValue,
    MissingInput,
    ExtraInput,
    DifferentWidth,
    RamAddress,
    RamExpected,
    RamDesired,
    ConstantOutput,
    InputLocationOverflow,
    OutputLocationOverflow,
    AbsoluteOverflow,
    ProtectedBase,
    EmptyRange,
    ConstantBase,
  };
  for (auto Case : {Invalid::NoOrdering,
                    Invalid::Relaxed,
                    Invalid::UnknownOrdering,
                    Invalid::Segment,
                    Invalid::NarrowAddress,
                    Invalid::OddWidth,
                    Invalid::WideValue,
                    Invalid::MissingInput,
                    Invalid::ExtraInput,
                    Invalid::DifferentWidth,
                    Invalid::RamAddress,
                    Invalid::RamExpected,
                    Invalid::RamDesired,
                    Invalid::ConstantOutput,
                    Invalid::InputLocationOverflow,
                    Invalid::OutputLocationOverflow,
                    Invalid::AbsoluteOverflow,
                    Invalid::ProtectedBase,
                    Invalid::EmptyRange,
                    Invalid::ConstantBase}) {
    SCOPED_TRACE(static_cast<unsigned>(Case));
    SymContext C;
    SymState S(C);
    SymExec E(C, S);
    const auto Root = C.mkVar("frame", 64);
    S.store(Root, C.mkConst(64, 19));
    SymStorePreservation Seed{Root, 0, 8, 24};
    S.store(C.mkConst(64, 0x4000), C.mkConst(64, 7), Seed);
    ASSERT_EQ(Seed.Status, SymStorePreservationStatus::Applied);
    ASSERT_TRUE(S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS,
                                               C.mkConst(64, 0)));
    auto Op = preservingCmpxchg(NdVar::reg(0, 4), NdVar::scalar(0x4000, 8),
                                NdVar::scalar(7, 4), NdVar::scalar(11, 4));
    SymStorePreservation P{Root, 0, 8, 100};
    switch (Case) {
    case Invalid::NoOrdering:
      Op.MemoryOrdering = NdMemoryOrdering::None;
      break;
    case Invalid::Relaxed:
      Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case Invalid::UnknownOrdering:
      Op.MemoryOrdering = static_cast<NdMemoryOrdering>(255);
      break;
    case Invalid::Segment:
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case Invalid::NarrowAddress:
      Op.Inputs[0].Size = 4;
      break;
    case Invalid::OddWidth:
    case Invalid::WideValue:
      Op.Output.Size = Op.Inputs[1].Size = Op.Inputs[2].Size =
          Case == Invalid::OddWidth ? 3 : 16;
      break;
    case Invalid::MissingInput:
      Op.NumInputs = 2;
      break;
    case Invalid::ExtraInput:
      Op.addInput(NdVar::scalar(13, 4));
      break;
    case Invalid::DifferentWidth:
      Op.Inputs[1].Size = 2;
      break;
    case Invalid::RamAddress:
      Op.Inputs[0] = NdVar::ram(0x4000, 8);
      break;
    case Invalid::RamExpected:
      Op.Inputs[1] = NdVar::ram(0x4000, 4);
      break;
    case Invalid::RamDesired:
      Op.Inputs[2] = NdVar::ram(0x4000, 4);
      break;
    case Invalid::ConstantOutput:
      Op.Output = NdVar::scalar(0, 4);
      break;
    case Invalid::InputLocationOverflow:
      Op.Inputs[1] = NdVar::reg(UINT64_MAX - 1, 4);
      break;
    case Invalid::OutputLocationOverflow:
      Op.Output = NdVar::reg(UINT64_MAX - 1, 4);
      break;
    case Invalid::AbsoluteOverflow:
      Op.Inputs[0].Offset = UINT64_MAX - 1;
      break;
    case Invalid::ProtectedBase:
      S.write(SymSpace::Register, 8, Root);
      Op.Inputs[0] = NdVar::reg(8, 8);
      break;
    case Invalid::EmptyRange:
      P.End = P.Begin;
      break;
    case Invalid::ConstantBase:
      P.Base = C.mkConst(64, 0x5000);
      break;
    }
    EXPECT_EQ(E.step(Op, nullptr, &P), StepResult::Unmodelled);
    EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
    EXPECT_EQ(E.opaqueOperationCount(), 1u);
    const auto Output = S.read(SymSpace::Register, 0, Op.Output.Size);
    EXPECT_FALSE(C.isConst(Output));
    EXPECT_TRUE(S.memoryIsUnknown());
  }
}

TEST(SymAtomicCmpxchg, NoDescriptorRetainsExistingAtomicValueBehavior) {
  for (bool Match : {false, true})
    for (bool MemoryOutput : {false, true}) {
      SymContext C;
      SymState S(C);
      SymExec E(C, S);
      const auto Address = C.mkConst(64, 0x4000);
      S.store(Address, C.mkConst(32, 7));
      auto Op = preservingCmpxchg(
          MemoryOutput ? NdVar::ram(0x4000, 4) : NdVar::reg(0, 4),
          NdVar::scalar(0x4000, 8), NdVar::scalar(Match ? 7 : 9, 4),
          NdVar::scalar(11, 4));
      Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
      ASSERT_EQ(E.step(Op), StepResult::Continue);
      if (!MemoryOutput)
        EXPECT_EQ(S.read(SymSpace::Register, 0, 4), C.mkConst(32, 7));
      EXPECT_EQ(S.load(Address, 4), C.mkConst(32, Match ? 11 : 7));
      EXPECT_EQ(E.opaqueOperationCount(), 0u);
    }
}

TEST(SymStorePreservation, IndependentRegionsCommitOnceAtTheExactBudget) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (unsigned Count : {2u, 3u}) {
      SCOPED_TRACE(Count);
      SymContext C;
      SymState Initial(C, Order);
      const SymRef Roots[] = {C.mkVar("first", 64), C.mkVar("second", 64),
                              C.mkVar("third", 64)};
      const auto External = C.mkVar("external", 64);
      std::vector<SymRef> Current;
      std::vector<SymStorePreservationRegion> More;
      for (unsigned I = 0; I != Count; ++I) {
        Current.push_back(Initial.load(Roots[I], 8));
        if (I)
          More.push_back({Roots[I], 0, 8});
      }
      // One validation per additional region; eight inspections and sixteen
      // save/restore units per current word. The bases have no fixed relation.
      const uint64_t Work = Count - 1 + Count * 24;
      SymStorePreservation P{Roots[0], 0, 8, Work - 1};
      P.AdditionalRegions = More;
      auto Short = Initial;
      EXPECT_FALSE(Short.store(External, C.mkConst(64, 17), P));
      EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
      EXPECT_LE(P.WorkUsed, P.MaxWork);
      EXPECT_TRUE(Short.mergeIdentical(Initial));
      P.MaxWork = Work;
      auto Exact = Initial;
      EXPECT_FALSE(Exact.store(External, C.mkConst(64, 17), P));
      ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
      EXPECT_EQ(P.WorkUsed, Work);
      EXPECT_EQ(Exact.numLiveBytes(), 8 * (Count + 1));
      for (unsigned I = 0; I != Count; ++I)
        EXPECT_EQ(Exact.load(Roots[I], 8), Current[I]);
      EXPECT_EQ(Exact.load(External, 8), C.mkConst(64, 17));
    }
}

TEST(SymStorePreservation, IndependentRegionsPreservePartialWrappingBytes) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext C;
    SymState S(C, Order);
    const SymRef Roots[] = {C.mkVar("first", 64), C.mkVar("second", 64)};
    const auto At = [&](SymRef Base, int64_t Offset) {
      return C.mkAdd(Base, C.mkConst(64, static_cast<uint64_t>(Offset)));
    };
    const uint64_t Words[] = {0x1020304050607080, 0x91a2b3c4d5e6f708};
    // Entry facts are specified independently of the preservation operation.
    SymState::MemoryKnowledge Entry{&C, Order, {}};
    for (unsigned I = 0; I != 2; ++I)
      Entry.Facts.push_back({Roots[I], uint64_t(-3), C.mkConst(64, Words[I])});
    ASSERT_TRUE(S.restoreMemoryKnowledge(Entry));
    const SymStorePreservationRegion More[] = {{Roots[1], -1, 3}};
    SymStorePreservation P{Roots[0], -2, 2, 33};
    P.AdditionalRegions = More;
    EXPECT_TRUE(S.store(C.mkConst(64, 0x30000), C.mkConst(8, 7), P));
    ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
    EXPECT_EQ(P.WorkUsed,
              33u); // 1 descriptor + 16 inspected + 8 saved/restored.
    EXPECT_EQ(S.numLiveBytes(), 9u);
    for (unsigned R = 0; R != 2; ++R)
      for (unsigned I = 0; I != 8; ++I) {
        const int64_t Offset = int64_t(I) - 3;
        const auto Byte = S.load(At(Roots[R], Offset), 1);
        const int64_t Begin = R ? -1 : -2;
        if (Offset >= Begin && Offset < Begin + 4) {
          const unsigned Lane = Order == llvm::endianness::little ? I : 7 - I;
          EXPECT_EQ(Byte, C.mkConst(8, (Words[R] >> (8 * Lane)) & 0xff));
        } else {
          EXPECT_FALSE(C.isConst(Byte));
        }
      }
    EXPECT_EQ(S.load(C.mkConst(64, 0x30000), 1), C.mkConst(8, 7));
  }
}

TEST(SymStorePreservation, IndependentRegionsKeepCurrentBytesAndNewEpochs) {
  SymContext C;
  SymState S(C);
  const auto A = C.mkVar("first", 64);
  const auto B = C.mkVar("second", 64);
  const auto Other = C.mkVar("unprotected", 64);
  const auto External = C.mkVar("external", 64);
  const auto At = [&](SymRef Base, uint64_t Offset) {
    return C.mkAdd(Base, C.mkConst(64, Offset));
  };
  const auto History = C.mkVar("history", 64);
  const auto HistoricalValue = S.load(History, 8);
  S.clobberMemory();
  ASSERT_FALSE(S.loadOrigins(HistoricalValue).empty());
  const auto AValue = S.load(A, 4);
  const auto BValue = S.load(B, 4);
  const auto AGap = S.load(At(A, 8), 2);
  const auto BGap = S.load(At(B, 8), 2);
  const auto OtherValue = S.load(Other, 2);
  const auto Absent = C.mkVar("absent", 64);
  auto Before = S;
  const SymStorePreservationRegion More[] = {
      {B, 0, 4}, {Absent, INT64_MIN, INT64_MAX}, {History, 0, 8}};
  SymStorePreservation P{A, 0, 4, 31};
  P.AdditionalRegions = More;
  EXPECT_FALSE(S.store(External, C.mkConst(16, 7), P));
  ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
  EXPECT_EQ(P.WorkUsed, 31u);
  EXPECT_EQ(S.numLiveBytes(), 10u);
  EXPECT_EQ(S.numMemoryRegions(), 5u); // Absent has not been materialized.
  EXPECT_EQ(S.load(A, 4), AValue);
  EXPECT_EQ(S.load(B, 4), BValue);
  for (auto [Base, OldGap] : {std::pair{A, AGap}, std::pair{B, BGap}}) {
    const auto Gap = S.load(At(Base, 8), 2);
    EXPECT_NE(Gap, OldGap);
    EXPECT_TRUE(S.memoryInputOrigins(Gap).empty());
    const auto Missing = S.load(At(Base, 16), 1);
    EXPECT_NE(Missing, Before.load(At(Base, 16), 1));
    EXPECT_TRUE(S.memoryInputOrigins(Missing).empty());
  }
  EXPECT_NE(S.load(Other, 2), OtherValue);
  EXPECT_NE(S.load(History, 8), HistoricalValue);
  EXPECT_NE(S.load(Absent, 1), Before.load(Absent, 1));
  auto Fork = S;
  EXPECT_EQ(S.load(At(Other, 32), 1), Fork.load(At(Other, 32), 1));
}

TEST(SymStorePreservation, RepeatedRegionsFormAUnionAndChargeEveryScan) {
  SymContext C;
  SymState S(C);
  const auto A = C.mkVar("first", 64);
  const auto B = C.mkVar("second", 64);
  const auto AValue = S.load(A, 8);
  const auto BValue = S.load(B, 8);
  const auto Before = S;
  const SymStorePreservationRange Ranges[] = {{6, 8}};
  const SymStorePreservationRegion Regions[] = {
      {B, 0, 4}, {B, 2, 6}, {A, 1, 3}, {B, 0, 4}};
  // Five descriptors, one ordered scan of A, three scans saving four B
  // bytes each, and a final scan saving two A bytes. Repeated saves do not
  // create extra bytes.
  constexpr uint64_t Work = 5 + (8 + 1 + 2 * 4) + 3 * (8 + 2 * 4) + (8 + 2 * 2);
  SymStorePreservation P{A, 0, 2, Work - 1};
  P.AdditionalRanges = Ranges;
  P.AdditionalRegions = Regions;
  auto Short = S;
  EXPECT_FALSE(Short.store(C.mkConst(64, 0x4000), C.mkConst(8, 9), P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.mergeIdentical(Before));
  P.MaxWork = Work;
  EXPECT_TRUE(S.store(C.mkConst(64, 0x4000), C.mkConst(8, 9), P));
  ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
  EXPECT_EQ(P.WorkUsed, Work);
  EXPECT_EQ(S.numLiveBytes(), 12u);
  for (unsigned I = 0; I != 8; ++I) {
    const auto AByte = S.load(C.mkAdd(A, C.mkConst(64, I)), 1);
    const auto BByte = S.load(C.mkAdd(B, C.mkConst(64, I)), 1);
    EXPECT_EQ(AByte == C.mkExtract(AValue, I * 8, 8), I < 3 || I >= 6);
    EXPECT_EQ(BByte == C.mkExtract(BValue, I * 8, 8), I < 6);
  }
}

TEST(SymStorePreservation, IndependentRegionValidationNeverMutatesState) {
  SymContext C;
  SymState S(C);
  const auto A = C.mkVar("first", 64);
  const auto B = C.mkVar("second", 64);
  const auto External = C.mkVar("external", 64);
  const auto Value = C.mkConst(8, 9);
  S.load(A, 8);
  S.load(B, 8);
  const auto Narrow = C.mkVar("narrow", 32);
  const auto Constant = C.mkConst(64, 19);
  const auto Displaced = C.mkAdd(External, C.mkConst(64, 7));
  const auto Bad = SymRef(uint32_t(C.numNodes() + 100));
  const auto Before = S;
  for (const auto Invalid :
       std::vector<SymStorePreservationRegion>{{Bad, 0, 8},
                                               {Narrow, 0, 8},
                                               {Constant, 0, 8},
                                               {B, 8, 8},
                                               {B, 9, 8},
                                               {External, 0, 1}}) {
    const SymStorePreservationRegion More[] = {{B, 0, 8}, Invalid};
    SymStorePreservation P{A, 0, 8, 100};
    P.AdditionalRegions = More;
    EXPECT_FALSE(S.store(Displaced, Value, P));
    EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
    EXPECT_EQ(P.WorkUsed, 2u); // All descriptors precede byte capture.
    EXPECT_TRUE(S.mergeIdentical(Before));
  }
  const SymStorePreservationRegion More[] = {{B, 0, 8}};
  for (unsigned BadField = 0; BadField != 3; ++BadField) {
    SymStorePreservation P{A, 0, 8, 100};
    P.AdditionalRegions = More;
    if (BadField == 0)
      P.Base = Bad;
    EXPECT_FALSE(S.store(BadField == 1 ? Bad : External,
                         BadField == 2 ? Bad : Value, P));
    EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
    EXPECT_EQ(P.WorkUsed, 0u);
    EXPECT_TRUE(S.mergeIdentical(Before));
  }
  SymStorePreservation P{A, 0, 8, 0};
  P.AdditionalRegions = More;
  EXPECT_FALSE(S.store(External, Value, P));
  EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
  EXPECT_EQ(P.WorkUsed, 0u);
  EXPECT_TRUE(S.mergeIdentical(Before));
}

TEST(SymStorePreservation, EmptyIndependentRegionsKeepBaseUnionAccounting) {
  SymContext C;
  const auto A = C.mkVar("first", 64);
  const auto External = C.mkVar("external", 64);
  for (bool ExtraRange : {false, true}) {
    SymState S(C);
    S.load(A, 8);
    const SymStorePreservationRange More[] = {{16, 24}};
    // The ordered union adds one descriptor and one cursor advance;
    // each materialized byte is visited once, saved and restored.
    const uint64_t Work = ExtraRange ? 1 + 16 + 1 + 2 * 16 : 8 + 2 * 8;
    SymStorePreservation P{A, 0, 8, Work};
    if (ExtraRange) {
      S.load(C.mkAdd(A, C.mkConst(64, 16)), 8);
      P.AdditionalRanges = More;
    }
    P.AdditionalRegions = {};
    EXPECT_FALSE(S.store(External, C.mkConst(8, 7), P));
    ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
    EXPECT_EQ(P.WorkUsed, Work);
  }
}

TEST(SymStorePreservation, ExecutorUsesIndependentRegionsForOnlyOneStore) {
  SymContext C;
  SymState S(C);
  SymExec E(C, S);
  const auto A = C.mkVar("first", 64);
  const auto B = C.mkVar("second", 64);
  const auto External = C.mkVar("external", 64);
  const auto AValue = S.load(A, 8);
  const auto BValue = S.load(B, 8);
  const auto NeighborAddress = C.mkAdd(External, C.mkConst(64, 8));
  const auto Neighbor = S.load(NeighborAddress, 8);
  S.write(SymSpace::Register, 8, External);
  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(NdVar::reg(8, 8));
  Store.addInput(NdVar::scalar(17, 8));
  const SymStorePreservationRegion More[] = {{B, 0, 8}};
  SymStorePreservation P{A, 0, 8, 49};
  P.AdditionalRegions = More;
  EXPECT_EQ(E.step(Store, nullptr, &P), StepResult::Continue);
  ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
  EXPECT_EQ(P.WorkUsed, 49u);
  EXPECT_EQ(E.memoryHavocCount(), 1u);
  EXPECT_EQ(E.opaqueOperationCount(), 0u);
  EXPECT_EQ(S.load(A, 8), AValue);
  EXPECT_EQ(S.load(B, 8), BValue);
  EXPECT_EQ(S.load(External, 8), C.mkConst(64, 17));
  EXPECT_EQ(S.load(NeighborAddress, 8), Neighbor);
  EXPECT_EQ(S.read(SymSpace::Register, 8, 8), External);
  EXPECT_EQ(E.step(Store), StepResult::Continue);
  EXPECT_EQ(E.memoryHavocCount(), 2u);
  EXPECT_NE(S.load(A, 8), AValue);
  EXPECT_NE(S.load(B, 8), BValue);
}

TEST(SymAtomicCmpxchg, IndependentRegionsShareTheWritePhaseAndItsBudget) {
  SymContext C;
  const auto A = C.mkVar("first", 64);
  const auto B = C.mkVar("second", 64);
  const auto Address = C.mkConst(64, 0x4000);
  SymState Initial(C);
  SymState::MemoryKnowledge Entry{&C,
                                  llvm::endianness::little,
                                  {{A, 0, C.mkConst(64, 19)},
                                   {B, 0, C.mkConst(64, 23)},
                                   {{}, 0x4000, C.mkConst(32, 7)}}};
  ASSERT_TRUE(Initial.restoreMemoryKnowledge(Entry));
  Initial.write(SymSpace::Register, 0, C.mkConst(32, 0xdeadbeef));
  const SymStorePreservationRegion More[] = {{B, 0, 8}};
  for (bool Match : {false, true})
    for (uint64_t Budget : {49, 48}) {
      SCOPED_TRACE(Match);
      SCOPED_TRACE(Budget);
      auto S = Initial;
      SymExec E(C, S);
      SymStorePreservation P{A, 0, 8, Budget};
      P.AdditionalRegions = More;
      const auto Op = preservingCmpxchg(
          NdVar::reg(0, 4), NdVar::scalar(0x4000, 8),
          NdVar::scalar(Match ? 7 : 9, 4), NdVar::scalar(11, 4));
      const auto Flow = E.step(Op, nullptr, &P);
      if (Budget == 49) {
        EXPECT_EQ(Flow, StepResult::Continue);
        EXPECT_EQ(P.Status, SymStorePreservationStatus::Applied);
        EXPECT_EQ(P.WorkUsed, 49u);
        EXPECT_EQ(S.read(SymSpace::Register, 0, 4), C.mkConst(32, 7));
        EXPECT_EQ(S.load(Address, 4), C.mkConst(32, Match ? 11 : 7));
        EXPECT_EQ(S.load(A, 8), C.mkConst(64, 19));
        EXPECT_EQ(S.load(B, 8), C.mkConst(64, 23));
        EXPECT_EQ(E.opaqueOperationCount(), 0u);
      } else {
        EXPECT_EQ(Flow, StepResult::Unmodelled);
        EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
        EXPECT_LE(P.WorkUsed, Budget);
        EXPECT_EQ(E.opaqueOperationCount(), 1u);
        EXPECT_FALSE(C.isConst(S.read(SymSpace::Register, 0, 4)));
        EXPECT_FALSE(C.isConst(S.load(Address, 4)));
        EXPECT_FALSE(C.isConst(S.load(A, 8)));
        EXPECT_FALSE(C.isConst(S.load(B, 8)));
        // The executor's established refusal path havocs outputs and memory;
        // only SymState::store itself promises failure without mutation.
      }
    }
}

TEST(SymStorePreservation, OrderedUnionsUseBoundedWorkAndPreserveTheirGaps) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (bool Absolute : {false, true}) {
      SymContext Ctx;
      SymState Before(Ctx, Order);
      const auto Root = Ctx.mkFreshVar(64, "ordered_base");
      const auto Other = Ctx.mkFreshVar(64, "writer");
      const uint64_t Start = Absolute ? 0x4000 : 0;
      const auto Address = [&](uint64_t Offset) {
        return Absolute ? Ctx.mkConst(64, Offset)
                        : Ctx.mkAdd(Root, Ctx.mkConst(64, Offset));
      };
      llvm::SmallVector<SymStorePreservationRange, 64> Ranges;
      for (uint64_t I = 0; I != 64; ++I) {
        const uint64_t Offset = Start + I * 16;
        Before.store(Address(Offset), Ctx.mkConst(64, I + 1));
        if (I)
          Ranges.push_back(
              {static_cast<int64_t>(Offset), static_cast<int64_t>(Offset + 8)});
      }
      const auto OldGap = Before.load(Address(Start + 8), 8);
      const auto OldWriter = Before.load(Other, 8);
      SymStorePreservation P{Absolute ? SymRef{} : Root,
                             static_cast<int64_t>(Start),
                             static_cast<int64_t>(Start + 8), 2048};
      P.AdditionalRanges = Ranges;
      auto Ordered = Before;
      Ordered.store(Other, Ctx.mkConst(64, 77), P);
      ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
      const auto Work = P.WorkUsed;
      for (uint64_t I = 0; I != 64; ++I)
        EXPECT_EQ(Ordered.load(Address(Start + I * 16), 8),
                  Ctx.mkConst(64, I + 1));
      EXPECT_EQ(Ordered.load(Other, 8), Ctx.mkConst(64, 77));
      EXPECT_NE(Ordered.load(Address(Start + 8), 8), OldGap);
      EXPECT_FALSE(Ctx.isConst(Ordered.load(Address(Start + 8), 8)));

      auto Short = Before;
      P.MaxWork = Work - 1;
      Short.store(Other, Ctx.mkConst(64, 77), P);
      EXPECT_EQ(P.Status, SymStorePreservationStatus::BudgetExceeded);
      EXPECT_EQ(Short.load(Other, 8), OldWriter);
      EXPECT_EQ(Short.load(Address(Start + 8), 8), OldGap);
      for (uint64_t I = 0; I != 64; ++I)
        EXPECT_EQ(Short.load(Address(Start + I * 16), 8),
                  Ctx.mkConst(64, I + 1));

      std::reverse(Ranges.begin(), Ranges.end());
      auto Unordered = Before;
      P.MaxWork = UINT64_MAX;
      Unordered.store(Other, Ctx.mkConst(64, 77), P);
      ASSERT_EQ(P.Status, SymStorePreservationStatus::Applied);
      EXPECT_GT(P.WorkUsed, Work);
      for (uint64_t I = 0; I != 64; ++I)
        EXPECT_EQ(Unordered.load(Address(Start + I * 16), 8),
                  Ctx.mkConst(64, I + 1));
      EXPECT_FALSE(Ctx.isConst(Unordered.load(Address(Start + 8), 8)));
    }
}

} // namespace
