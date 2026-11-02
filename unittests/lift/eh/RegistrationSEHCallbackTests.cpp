//===- RegistrationSEHCallbackTests.cpp - Callback tail ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/med/X86/X86RegistrationCallback.h"

#include <algorithm>

using namespace neverd;

TEST(RegistrationSEHCallback, UnreachedTailsRetainTheirOwningInvocation) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    MedFunc Function;
    auto &EH = Function.ExceptionMetadata.emplace();
    EH.Registration.emplace();
    EH.Registration->Scopes = {{-1, 0, 0x1800, true},
                               {-1, 0x1900, 0x1000, false}};
    auto &States = Function.RegistrationStates.emplace();
    States.Complete = States.CallbackStatesComplete =
        States.RegistrationLifetimeComplete = true;
    constexpr va_t Addresses[] = {0x1000, 0x1800, 0x1805, 0x1900};
    for (unsigned I = 0; I != 4; ++I) {
      MedBlock Block;
      Block.Id = I;
      Block.StartAddr = Addresses[I];
      Block.EndAddr = Addresses[I] + 5;
      if (I == 1)
        Block.Succs = {2};
      Function.Blocks.push_back(Block);
      RegistrationBlockState State;
      State.BlockId = I;
      State.Range = {Block.StartAddr, Block.EndAddr};
      State.CallbackOnly = I == 1 || I == 3;
      State.Reached = I != 2;
      States.Blocks.push_back(State);
    }
    if (Mutation == 1)
      States.Blocks[2].Reached = true;
    if (Mutation == 2)
      States.Blocks[1].CallbackOnly = false;
    if (Mutation == 3)
      Function.Blocks[3].Succs = {2};
    if (Mutation == 4)
      Function.Blocks[2].Id = 1;
    if (Mutation == 5)
      States.Blocks[0].CallbackOnly = true;
    if (Mutation == 6)
      States.Blocks.pop_back();
    if (Mutation == 7)
      ++States.Blocks[2].Range.End;
    if (Mutation == 8)
      Function.Blocks[1].Succs = {4};
    if (Mutation == 9)
      States.Blocks[2].Unknown = true;
    const auto Owners = registrationSEHCallbackOwners(Function);
    if (Mutation) {
      EXPECT_FALSE(Owners);
      continue;
    }
    ASSERT_TRUE(Owners);
    EXPECT_EQ(*Owners,
              (std::map<int, std::pair<va_t, bool>>{{1, {0x1800, true}},
                                                    {2, {0x1800, true}},
                                                    {3, {0x1900, false}}}));
    std::reverse(Function.Blocks.begin(), Function.Blocks.end());
    EXPECT_EQ(registrationSEHCallbackOwners(Function), Owners);
  }
}
