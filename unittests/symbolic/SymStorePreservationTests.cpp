//===- SymStorePreservationTests.cpp - Explicit store contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExec.h"

#include <limits>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

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

TEST(SymStorePreservation, ExecutorAppliesContractToOneOrdinaryStoreOnly) {
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
  EXPECT_EQ(E.step(Store, nullptr, &P), StepResult::Continue);
  EXPECT_EQ(P.Status, SymStorePreservationStatus::Invalid);
  EXPECT_EQ(S.read(SymSpace::Register, 16, 8), C.mkConst(64, 17));
  EXPECT_EQ(S.load(S.read(SymSpace::Register, 8, 8), 8), C.mkConst(64, 34));
}

} // namespace
