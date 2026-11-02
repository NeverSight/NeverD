//===- RegistrationCxxUnwindTests.cpp - PE32 nested search projection ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86/X86RegistrationCxxUnwind.h"
#include "neverd/loader/ExceptionInfo.h"

using namespace neverd;

namespace {
using Kind = X86RegistrationCxxUnwindTarget::Kind;

CxxExceptionInfo nestedTries() {
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 4;
  Cxx.IsSynchronous = true;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}, {0, 0}, {-1, 0}};
  Cxx.TryBlocks = {{1, 1, 2, {{}}}, {0, 2, 3, {{}}}};
  return Cxx;
}

CxxExceptionInfo tryInsideCatch() {
  auto Cxx = nestedTries();
  Cxx.MaxState = 6;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}, {0, 0}, {2, 0}, {2, 0}, {-1, 0}};
  Cxx.TryBlocks = {{3, 3, 4, {{}}}, {1, 1, 4, {{}}}, {0, 4, 5, {{}, {}}}};
  return Cxx;
}

TEST(RegistrationCxxUnwind, CatchIntervalsKeepDistinctEntriesAndSharedEnds) {
  const auto Result = projectX86RegistrationCxxUnwind(tryInsideCatch());
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 6u);
  const unsigned Expected[] = {2, 1, 2, 0, 2};
  for (unsigned State = 0; State != 5; ++State) {
    EXPECT_EQ((*Result)[State].TargetKind, Kind::Try);
    EXPECT_EQ((*Result)[State].Index, Expected[State]);
  }
  EXPECT_EQ((*Result)[5].TargetKind, Kind::Caller);
}

TEST(RegistrationCxxUnwind, RejectsCrossingOrReenteredCatchIntervals) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Cxx = tryInsideCatch();
    switch (Mutation) {
    case 0:
      std::swap(Cxx.TryBlocks[0], Cxx.TryBlocks[1]);
      break;
    case 1:
      Cxx.TryBlocks[1].CatchHigh = 3;
      break;
    case 2:
      Cxx.TryBlocks[0].TryLow = 2;
      break;
    case 3:
      Cxx.UnwindMap[3].ToState = 1;
      break;
    case 4:
      Cxx.UnwindMap[4].ToState = 3;
      break;
    case 5:
      Cxx.UnwindMap[3].ToState = -1;
      break;
    }
    EXPECT_FALSE(projectX86RegistrationCxxUnwind(Cxx));
  }
}

TEST(RegistrationCxxUnwind, NestedCatchResumesOuterSearch) {
  const auto Result = projectX86RegistrationCxxUnwind(nestedTries());
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 4u);
  for (unsigned State : {0u, 2u}) {
    EXPECT_EQ((*Result)[State].TargetKind, Kind::Try);
    EXPECT_EQ((*Result)[State].Index, 1u);
  }
  EXPECT_EQ((*Result)[1].TargetKind, Kind::Try);
  EXPECT_EQ((*Result)[1].Index, 0u);
  EXPECT_EQ((*Result)[3].TargetKind, Kind::Caller);
}

TEST(RegistrationCxxUnwind, DisjointTriesKeepIndependentDispatch) {
  auto Cxx = nestedTries();
  Cxx.UnwindMap = {{-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}};
  Cxx.TryBlocks = {{0, 0, 1, {{}}}, {2, 2, 3, {{}}}};
  for (bool Reverse : {false, true}) {
    if (Reverse)
      std::swap(Cxx.TryBlocks[0], Cxx.TryBlocks[1]);
    const auto Result = projectX86RegistrationCxxUnwind(Cxx);
    ASSERT_TRUE(Result);
    for (unsigned State : {0u, 2u}) {
      EXPECT_EQ((*Result)[State].TargetKind, Kind::Try);
      EXPECT_EQ((*Result)[State].Index, Reverse ? 1 - State / 2 : State / 2);
      EXPECT_EQ((*Result)[State + 1].TargetKind, Kind::Caller);
    }
  }
}

TEST(RegistrationCxxUnwind, NestedCleanupRetainsItsImmediateSearch) {
  auto Cxx = nestedTries();
  Cxx.MaxState = 5;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}, {1, 0x401000}, {0, 0}, {-1, 0}};
  Cxx.TryBlocks = {{1, 2, 3, {{}}}, {0, 3, 4, {{}}}};
  const auto Result = projectX86RegistrationCxxUnwind(Cxx);
  ASSERT_TRUE(Result);
  EXPECT_EQ((*Result)[2].TargetKind, Kind::Cleanup);
  EXPECT_EQ((*Result)[2].Index, 2u);
  EXPECT_EQ((*Result)[3].TargetKind, Kind::Try);
  EXPECT_EQ((*Result)[3].Index, 1u);
}

TEST(RegistrationCxxUnwind, CleanupOnlyStatesPreserveTheirOrderedLifetime) {
  CxxExceptionInfo Cxx;
  Cxx.MaxState = 6;
  Cxx.UnwindMap = {{-1, 0x401000}, {0, 0},  {1, 0x401010},
                   {2, 0},         {-1, 0}, {4, 0x401020}};
  const auto Result = projectX86RegistrationCxxUnwind(Cxx);
  ASSERT_TRUE(Result);
  ASSERT_EQ(Result->size(), 6u);
  for (unsigned State : {0u, 1u, 2u, 3u, 5u}) {
    EXPECT_EQ((*Result)[State].TargetKind, Kind::Cleanup);
    EXPECT_EQ((*Result)[State].Index, State < 2 ? 0u : State < 4 ? 2u : 5u);
  }
  EXPECT_EQ((*Result)[4].TargetKind, Kind::Caller);
  // State 2 cleans its object, follows the empty state 1, then cleans state 0.
  EXPECT_EQ((*Result)[Cxx.UnwindMap[2].ToState].Index, 0u);
  EXPECT_EQ(Cxx.UnwindMap[0].ToState, -1);
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Cxx;
    switch (Mutation) {
    case 0:
      Changed.UnwindMap[0].ToState = 0;
      break;
    case 1:
      Changed.UnwindMap[2].ToState = 3;
      break;
    case 2:
      Changed.UnwindMap[2].ToState = -2;
      break;
    case 3:
      Changed.MaxState = 5;
      break;
    case 4:
      for (auto &Action : Changed.UnwindMap)
        Action.ActionVA = 0;
      break;
    case 5:
      Changed.UnwindMap.clear();
      Changed.MaxState = 0;
      break;
    }
    EXPECT_FALSE(projectX86RegistrationCxxUnwind(Changed));
  }
}

TEST(RegistrationCxxUnwind, WideCleanupGraphsKeepEachIndependentLifetime) {
  for (uint32_t Count :
       {129u, 601u, 2073u, 3252u, limits::kMaxRegistrationEHRecords}) {
    SCOPED_TRACE(Count);
    CxxExceptionInfo Cxx;
    Cxx.MaxState = Count;
    for (uint32_t State = 0; State != Count; ++State)
      Cxx.UnwindMap.push_back(
          {State % 128 ? int32_t(State - 1) : -1, 0x401000 + State * 16});
    const auto Result = projectX86RegistrationCxxUnwind(Cxx);
    ASSERT_TRUE(Result);
    ASSERT_EQ(Result->size(), Count);
    for (uint32_t State = 0; State != Count; ++State) {
      EXPECT_EQ((*Result)[State].TargetKind, Kind::Cleanup);
      EXPECT_EQ((*Result)[State].Index, State);
    }
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Changed = Cxx;
      if (Mutation == 0)
        Changed.UnwindMap.back().ToState = Count - 1;
      if (Mutation == 1)
        Changed.UnwindMap.back().ToState = -2;
      if (Mutation == 2)
        --Changed.MaxState;
      if (Mutation == 3)
        Changed.UnwindMap[128].ToState = 127; // 129 nested actions
      EXPECT_FALSE(projectX86RegistrationCxxUnwind(Changed));
    }
  }
}

TEST(RegistrationCxxUnwind, EmptyTransitionsDoNotInventNativeCleanupDepth) {
  CxxExceptionInfo Cxx;
  Cxx.MaxState = limits::kMaxRegistrationEHRecords;
  Cxx.UnwindMap.push_back({-1, 0x401000});
  for (uint32_t State = 1; State != Cxx.MaxState; ++State)
    Cxx.UnwindMap.push_back({int32_t(State - 1), 0});
  Cxx.UnwindMap.back().ActionVA = 0x401010;
  const auto Result = projectX86RegistrationCxxUnwind(Cxx);
  ASSERT_TRUE(Result);
  for (uint32_t State = 0; State + 1 < Cxx.MaxState; ++State) {
    EXPECT_EQ((*Result)[State].TargetKind, Kind::Cleanup);
    EXPECT_EQ((*Result)[State].Index, 0u);
  }
  EXPECT_EQ(Result->back().TargetKind, Kind::Cleanup);
  EXPECT_EQ(Result->back().Index, Cxx.MaxState - 1);
  ++Cxx.MaxState;
  Cxx.UnwindMap.push_back({-1, 0x401020});
  EXPECT_FALSE(projectX86RegistrationCxxUnwind(Cxx));
}

TEST(RegistrationCxxUnwind, ThreeLevelsFollowInnerToOuterOrder) {
  auto Cxx = nestedTries();
  Cxx.MaxState = 6;
  Cxx.UnwindMap = {{-1, 0}, {0, 0}, {1, 0}, {1, 0}, {0, 0}, {-1, 0}};
  Cxx.TryBlocks = {{2, 2, 3, {{}}}, {1, 3, 4, {{}}}, {0, 4, 5, {{}}}};
  const auto Result = projectX86RegistrationCxxUnwind(Cxx);
  ASSERT_TRUE(Result);
  const unsigned Expected[] = {2, 1, 0, 1, 2};
  for (unsigned State = 0; State != 5; ++State) {
    EXPECT_EQ((*Result)[State].TargetKind, Kind::Try);
    EXPECT_EQ((*Result)[State].Index, Expected[State]);
  }
  EXPECT_EQ((*Result)[5].TargetKind, Kind::Caller);
}

TEST(RegistrationCxxUnwind, RejectsUnprovedSearchAndLifetimeGraphs) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Cxx = nestedTries();
    switch (Mutation) {
    case 0:
      std::swap(Cxx.TryBlocks[0], Cxx.TryBlocks[1]);
      break;
    case 1:
      Cxx.UnwindMap[1].ToState = -1;
      break;
    case 2:
      Cxx.UnwindMap[2].ToState = 1;
      break;
    case 3:
      Cxx.TryBlocks[1].TryHigh = 1;
      break;
    case 4:
      Cxx.TryBlocks[0].CatchHigh = 3;
      break;
    case 5:
      Cxx.UnwindMap[1].ActionVA = 0x401000;
      break;
    case 6:
      Cxx.UnwindMap[3].ActionVA = 0x401000;
      break;
    case 7:
      Cxx.TryBlocks[0].Handlers.clear();
      break;
    case 8:
      Cxx.TryBlocks.push_back(Cxx.TryBlocks[0]);
      break;
    case 9:
      Cxx.UnwindMap.resize(129);
      Cxx.MaxState = 129;
      break;
    }
    EXPECT_FALSE(projectX86RegistrationCxxUnwind(Cxx));
  }
}
} // namespace
