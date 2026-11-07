//===- HighVariableIdentityTests.cpp - HighIR variable keys ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks SSA key identity and bucket distribution for repeated carriers.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/high/HighIR.h"

#include <algorithm>
#include <array>

using namespace neverd;

TEST(HighVariableIdentity, HashDistributesRepeatedSSAVersions) {
  // MSVC uses power-of-two bucket counts. Both parts must influence the low
  // hash bits: successive definitions of one register are a common SSA shape.
  constexpr size_t Buckets = 4096;
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    SCOPED_TRACE(Mode);
    std::array<unsigned, Buckets> Counts{};
    for (int I = 0; I != int(Buckets); ++I) {
      const VarKey Key = Mode == 0   ? VarKey{17, I}
                         : Mode == 1 ? VarKey{17, -I - 1}
                                     : VarKey{I << 16, 0};
      ++Counts[VarKeyHash{}(Key) & (Buckets - 1)];
    }
    // A generous distribution bound that does not require fixed hash values
    // or the standard library's choice of prime or power-of-two buckets.
    EXPECT_LE(*std::max_element(Counts.begin(), Counts.end()), 32u);
  }
}

TEST(HighVariableIdentity, SyntheticParametersKeepTheirOwnNamespace) {
  MedVar V;
  V.Id = 17;
  V.Kind = MedVar::Reg;
  VarKeyMap<int> Values;
  for (int Version = 0; Version != 64; ++Version) {
    V.SSAVer = Version;
    V.Kind = MedVar::Reg;
    EXPECT_TRUE(Values.emplace(varKey(V), Version).second);
    V.Kind = MedVar::Param;
    EXPECT_TRUE(Values.emplace(varKey(V), -Version - 1).second);
  }
  for (int Version = 0; Version != 64; ++Version) {
    V.SSAVer = Version;
    V.Kind = MedVar::Reg;
    EXPECT_EQ(Values.at(varKey(V)), Version);
    V.Kind = MedVar::Param;
    EXPECT_EQ(Values.at(varKey(V)), -Version - 1);
  }
}
