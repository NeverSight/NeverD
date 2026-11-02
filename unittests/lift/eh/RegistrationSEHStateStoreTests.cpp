//===- RegistrationSEHStateStoreTests.cpp - State binding tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks callback source-store binding and retained scope closure.

#include "gtest/gtest.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationSEHStateStore.h"

using namespace neverd;

namespace {

MedFunc sourceStateStore() {
  MedFunc Function;
  auto &EH = Function.ExceptionMetadata.emplace();
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
  auto &Chain = EH.Registration.emplace();
  Chain.SeededTryLevel = -1;
  Chain.TryLevelOffset = -4;
  Chain.Scopes = {
      {-1, 0, 0x2020, true}, {0, 0, 0x2020, true}, {-1, 0, 0x2020, true}};
  Chain.TryLevelStores = {{0x2020, 0x2025, 1, 4}};
  auto &Analysis = Function.RegistrationStates.emplace();
  Analysis.Complete = true;
  Analysis.CallbackStatesComplete = true;
  Analysis.RegistrationLifetimeComplete = true;
  Analysis.Blocks = {{10, {0x2000, 0x2010}, {2}},
                     {20, {0x2020, 0x2030}, {-1}, false, true}};
  Analysis.SEHStateStores = {{20, 0x2020, 0x2025, 4, 4, 1, {{-1, 1}}}};
  MedBlock Parent;
  Parent.Id = 10;
  Parent.StartAddr = 0x2000;
  Parent.EndAddr = 0x2010;
  Function.Blocks.push_back(Parent);
  MedBlock Callback;
  Callback.Id = 20;
  Callback.StartAddr = 0x2020;
  Callback.EndAddr = 0x2030;
  MedOp Store;
  Store.Opcode = NdOp::STORE;
  Store.Addr = 0x2020;
  Store.OriginSeq = 4;
  Store.NumInputs = 2;
  Store.Inputs[0].Kind = MedVar::Temp;
  Store.Inputs[0].Id = 5;
  Store.Inputs[0].SSAVer = 1;
  Store.Inputs[0].Size = 4;
  Store.Inputs[1] = MedVar::makeConst(1, 4);
  Callback.Ops.push_back(Store);
  Function.Blocks.push_back(Callback);
  return Function;
}

TEST(RegistrationSEHStateStore, BindsTheExactFullWidthCallbackOccurrence) {
  auto Function = sourceStateStore();
  const auto Stores = getX86RegistrationSEHStateStores(Function);
  ASSERT_TRUE(Stores);
  ASSERT_EQ(Stores->size(), 1u);
  EXPECT_EQ(Stores->front().Operation, &Function.Blocks[1].Ops[0]);
  EXPECT_EQ(Stores->front().Source,
            &Function.RegistrationStates->SEHStateStores[0]);
  EXPECT_EQ(getX86RegistrationSEHRetainedScopes(Function),
            (std::optional<std::vector<uint32_t>>{{0, 1}}));
  std::reverse(Function.Blocks.begin(), Function.Blocks.end());
  EXPECT_TRUE(getX86RegistrationSEHStateStores(Function));
}

TEST(RegistrationSEHStateStore, RejectsChangedOrMissingSourceEffects) {
  auto Reject = [](auto Mutate) {
    auto Function = sourceStateStore();
    Mutate(Function);
    EXPECT_FALSE(getX86RegistrationSEHStateStores(Function));
    EXPECT_FALSE(getX86RegistrationSEHRetainedScopes(Function));
  };
  Reject([](auto &F) { F.RegistrationStates->Complete = false; });
  Reject([](auto &F) { F.RegistrationStates->CallbackStatesComplete = false; });
  Reject([](auto &F) {
    F.RegistrationStates->RegistrationLifetimeComplete = false;
  });
  Reject([](auto &F) { F.RegistrationStates->SEHStateStoresComplete = false; });
  Reject([](auto &F) { F.RegistrationStates->SEHStateStores.clear(); });
  Reject(
      [](auto &F) { F.RegistrationStates->SEHStateStores[0].EndAddress += 1; });
  Reject([](auto &F) {
    F.RegistrationStates->SEHStateStores[0].Transitions[0].second = 2;
  });
  Reject([](auto &F) {
    F.RegistrationStates->SEHStateStores[0].Transitions[0].first = 3;
  });
  Reject([](auto &F) {
    F.RegistrationStates->SEHStateStores[0].Transitions.clear();
  });
  Reject([](auto &F) {
    F.RegistrationStates->SEHStateStores.push_back(
        F.RegistrationStates->SEHStateStores[0]);
  });
  Reject([](auto &F) { F.RegistrationStates->Blocks[1].Unknown = true; });
  Reject([](auto &F) { F.RegistrationStates->Blocks[1].Range.End += 1; });
  Reject([](auto &F) { F.Blocks[1].Ops[0].Inputs[1].ConstVal = 2; });
  Reject([](auto &F) { F.Blocks[1].Ops[0].Inputs[1].Kind = MedVar::Temp; });
  Reject([](auto &F) { F.Blocks[1].Ops[0].Dead = true; });
  Reject([](auto &F) { F.Blocks[1].Ops[0].OriginSeq = -1; });
  Reject([](auto &F) { F.Blocks[1].Ops[0].Opcode = NdOp::LOAD; });
  Reject([](auto &F) {
    F.Blocks[1].Ops[0].MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  });
  Reject([](auto &F) { F.Blocks[1].Ops.push_back(F.Blocks[1].Ops[0]); });
  Reject([](auto &F) {
    F.ExceptionMetadata->Registration->TryLevelStores[0].Width = 1;
    F.RegistrationStates->SEHStateStores[0].Width = 1;
    F.Blocks[1].Ops[0].Inputs[1].Size = 1;
  });
}

TEST(RegistrationSEHStateStore, UsesAllDynamicParentStatesAndOuterScopes) {
  auto Function = sourceStateStore();
  Function.RegistrationStates->Blocks[0].ExplicitLevels = {1, 2};
  EXPECT_EQ(getX86RegistrationSEHRetainedScopes(Function),
            (std::optional<std::vector<uint32_t>>{{}}));
  Function.RegistrationStates->Blocks[0].ExplicitLevels = {-1, 2};
  EXPECT_EQ(getX86RegistrationSEHRetainedScopes(Function),
            (std::optional<std::vector<uint32_t>>{{0, 1}}));
  Function.ExceptionMetadata->Registration->Scopes[1].EnclosingLevel = 1;
  EXPECT_FALSE(getX86RegistrationSEHRetainedScopes(Function));
}

TEST(RegistrationSEHStateStore, ComputedParentsKeepTheirExactSourceOccurrence) {
  auto Make = [] {
    auto Function = sourceStateStore();
    Function.ExceptionMetadata->Registration->TryLevelStores.clear();
    Function.RegistrationStates->Blocks[1].CallbackOnly = false;
    Function.RegistrationStates->SEHStateStores[0].EndAddress = 0x2030;
    Function.Blocks[1].Ops[0].Inputs[1] = Function.Blocks[1].Ops[0].Inputs[0];
    return Function;
  };
  auto Function = Make();
  const auto Callbacks = getX86RegistrationSEHStateStores(Function);
  ASSERT_TRUE(Callbacks);
  EXPECT_TRUE(Callbacks->empty());
  const auto All = getX86RegistrationSEHStateStores(Function, true);
  ASSERT_TRUE(All);
  ASSERT_EQ(All->size(), 1u);
  EXPECT_EQ(All->front().Operation, &Function.Blocks[1].Ops[0]);
  auto Reject = [&](auto Mutate) {
    auto Changed = Make();
    Mutate(Changed);
    EXPECT_FALSE(getX86RegistrationSEHStateStores(Changed));
    EXPECT_FALSE(getX86RegistrationSEHStateStores(Changed, true));
  };
  Reject([](auto &F) { --F.RegistrationStates->SEHStateStores[0].EndAddress; });
  Reject([](auto &F) { F.RegistrationStates->SEHStateStores[0].Width = 2; });
  Reject([](auto &F) {
    F.RegistrationStates->SEHStateStores[0].Transitions.clear();
  });
  Reject([](auto &F) { F.RegistrationStates->Blocks[1].CallbackOnly = true; });
  Reject([](auto &F) { F.Blocks[1].Ops.clear(); });
  Reject([](auto &F) { F.Blocks[1].Ops[0].Dead = true; });
  Reject(
      [](auto &F) { F.Blocks[1].Ops[0].Inputs[1] = MedVar::makeConst(2, 4); });
  Reject([](auto &F) {
    F.Blocks[1].Ops[0].MemoryOrdering = NdMemoryOrdering::Relaxed;
  });
  Reject([](auto &F) { F.Blocks[1].Ops.push_back(F.Blocks[1].Ops[0]); });
}

TEST(RegistrationSEHStateStore, ComparesTheDeclaredFullWordBits) {
  auto Function = sourceStateStore();
  Function.ExceptionMetadata->Registration->TryLevelStores[0].Level = -1;
  auto &Receipt = Function.RegistrationStates->SEHStateStores[0];
  Receipt.Value = UINT32_MAX;
  Receipt.Transitions = {{1, -1}};
  Function.Blocks[1].Ops[0].Inputs[1] = MedVar::makeConst(UINT64_MAX, 4);
  EXPECT_TRUE(getX86RegistrationSEHStateStores(Function));
  Function.Blocks[1].Ops[0].Inputs[1].Size = 8;
  EXPECT_FALSE(getX86RegistrationSEHStateStores(Function));
}

TEST(RegistrationSEHStateStore, AcceptsWidenedEffectiveAddresses) {
  auto Function = sourceStateStore();
  // The x86 lifter carries effective addresses in its common 64-bit domain.
  // The source receipt and installer frame proof own the pointed-to slot.
  for (uint16_t Width : {4, 8}) {
    Function.Blocks[1].Ops[0].Inputs[0].Size = Width;
    EXPECT_TRUE(getX86RegistrationSEHStateStores(Function));
  }
  for (uint16_t Width : {0, 1, 2, 16}) {
    Function.Blocks[1].Ops[0].Inputs[0].Size = Width;
    EXPECT_FALSE(getX86RegistrationSEHStateStores(Function));
  }
}

TEST(RegistrationSEHStateStore, NarrowStoresPreserveTheCompleteSourceLevel) {
  for (uint8_t Width : {1, 2}) {
    SCOPED_TRACE(Width);
    auto Function = sourceStateStore();
    auto &Native = Function.ExceptionMetadata->Registration->TryLevelStores[0];
    auto &Receipt = Function.RegistrationStates->SEHStateStores[0];
    auto &Value = Function.Blocks[1].Ops[0].Inputs[1];
    Native.Width = Receipt.Width = Value.Size = Width;
    Receipt.Transitions = {{2, 1}};
    auto Bound = getX86RegistrationSEHStateStores(Function);
    ASSERT_TRUE(Bound);
    EXPECT_EQ(Bound->front().TargetLevels, (std::vector<int32_t>{1}));
    // The same low bytes cannot clear the untouched high bytes of -1.
    Receipt.Transitions = {{-1, 1}};
    EXPECT_FALSE(getX86RegistrationSEHStateStores(Function));
    const uint32_t Mask = Width == 1 ? UINT8_MAX : UINT16_MAX;
    Native.Level = Receipt.Value = Mask;
    Value.ConstVal = UINT64_MAX;
    Receipt.Transitions = {{-1, -1}};
    Bound = getX86RegistrationSEHStateStores(Function);
    ASSERT_TRUE(Bound);
    EXPECT_EQ(Bound->front().TargetLevels, (std::vector<int32_t>{-1}));
    Receipt.Value ^= uint32_t(1) << (Width * 8);
    Native.Level = Receipt.Value;
    EXPECT_FALSE(getX86RegistrationSEHStateStores(Function));
  }
}

TEST(RegistrationSEHStateStore, ByteLiteralDoesNotNameTheCompleteScope) {
  auto Function = sourceStateStore();
  auto &Chain = *Function.ExceptionMetadata->Registration;
  Chain.Scopes.clear();
  for (int32_t Level = 0; Level != 259; ++Level)
    Chain.Scopes.push_back({Level - 1, 0, 0x2020, true});
  Function.RegistrationStates->Blocks[0].ExplicitLevels = {257};
  auto &Native = Chain.TryLevelStores[0];
  auto &Receipt = Function.RegistrationStates->SEHStateStores[0];
  auto &Value = Function.Blocks[1].Ops[0].Inputs[1];
  Native.Width = Receipt.Width = Value.Size = 1;
  Native.Level = Receipt.Value = Value.ConstVal = 2;
  Receipt.Transitions = {{257, 258}};
  auto Bound = getX86RegistrationSEHStateStores(Function);
  ASSERT_TRUE(Bound);
  EXPECT_EQ(Bound->front().TargetLevels, (std::vector<int32_t>{258}));
  EXPECT_EQ(getX86RegistrationSEHRetainedScopes(Function),
            (std::optional<std::vector<uint32_t>>{{258}}));
  Receipt.Transitions.push_back({1, 2});
  Bound = getX86RegistrationSEHStateStores(Function);
  ASSERT_TRUE(Bound);
  EXPECT_EQ(Bound->front().TargetLevels, (std::vector<int32_t>{2, 258}));
  EXPECT_EQ(Bound->front().FirstWriteID, 1u);
  std::reverse(Receipt.Transitions.begin(), Receipt.Transitions.end());
  Bound = getX86RegistrationSEHStateStores(Function);
  ASSERT_TRUE(Bound);
  EXPECT_EQ(Bound->front().TargetLevels, (std::vector<int32_t>{2, 258}));
  EXPECT_EQ(getX86RegistrationSEHRetainedScopes(Function),
            (std::optional<std::vector<uint32_t>>{{258}}));
  // Replacing just the low byte cannot clear the other complete result's high
  // bytes, even when both proposed states name valid scopes.
  Receipt.Transitions.back().second = 2;
  EXPECT_FALSE(getX86RegistrationSEHStateStores(Function));
}

TEST(RegistrationSEHStateStore,
     NarrowSentinelAndPositiveOutcomesKeepTheirHighBytes) {
  for (bool EH4 : {false, true}) {
    SCOPED_TRACE(EH4);
    auto Function = sourceStateStore();
    auto &EH = *Function.ExceptionMetadata;
    EH.Encoding = EH4 ? ExceptionEncoding::X86ScopeTableEH4
                      : ExceptionEncoding::X86ScopeTableEH3;
    auto &Chain = *EH.Registration;
    const int32_t Sentinel = EH4 ? -2 : -1;
    Chain.SeededTryLevel = Sentinel;
    Chain.Scopes.clear();
    for (int32_t Level = 0; Level != 256; ++Level)
      Chain.Scopes.push_back({Level ? Level - 1 : Sentinel, 0, 0x2020, true});
    Function.RegistrationStates->Blocks[0].ExplicitLevels = {255};
    auto &Native = Chain.TryLevelStores[0];
    auto &Receipt = Function.RegistrationStates->SEHStateStores[0];
    auto &Value = Function.Blocks[1].Ops[0].Inputs[1];
    Native.Width = Receipt.Width = Value.Size = 1;
    const int32_t Positive = uint8_t(Sentinel);
    Native.Level = Receipt.Value = Value.ConstVal = Positive;
    Receipt.Transitions = {{1, Positive}, {Sentinel, Sentinel}};
    auto Bound = getX86RegistrationSEHStateStores(Function);
    ASSERT_TRUE(Bound);
    EXPECT_EQ(Bound->front().TargetLevels,
              (std::vector<int32_t>{Sentinel, Positive}));
    Receipt.Transitions.back().second = Positive;
    EXPECT_FALSE(getX86RegistrationSEHStateStores(Function));
  }
}

TEST(RegistrationSEHStateStore,
     DoesNotInventReachabilityFromCallbackEntryState) {
  auto Function = sourceStateStore();
  Function.ExceptionMetadata->Registration->TryLevelStores.clear();
  Function.RegistrationStates->SEHStateStores.clear();
  Function.RegistrationStates->Blocks[1].ExplicitLevels = {1};
  ASSERT_TRUE(getX86RegistrationSEHStateStores(Function));
  EXPECT_FALSE(getX86RegistrationSEHRetainedScopes(Function));
}

} // namespace
