//===- FeatureTests.cpp - Digest CPU feature admission -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent CPUID fixtures check required bits, unavailable leaves and
/// query reuse without changing the host's capabilities.
///
//===----------------------------------------------------------------------===//

#include "SHA256Internal.h"
#include "gtest/gtest.h"

#include <cstdint>
#include <vector>

namespace neverd::detail {
namespace {

TEST(DigestFeatures, RequiresEveryInstructionFeature) {
  // CPUID.01H:ECX[9,19] and CPUID.07H.00H:EBX[29], independently of the
  // production inventory. All unrelated feature bits are deliberately set.
  for (unsigned Present = 0; Present < 8; ++Present) {
    SCOPED_TRACE(Present);
    std::vector<uint32_t> Leaves;
    const bool Supported = x86HasSHA256Features(
        [&](uint32_t Leaf, uint32_t Subleaf) -> X86CPUIDResult {
          Leaves.push_back(Leaf);
          EXPECT_EQ(Subleaf, 0U);
          if (Leaf == 0)
            return {7, 0, 0, 0};
          if (Leaf == 1) {
            uint32_t ECX = ~((uint32_t{1} << 9) | (uint32_t{1} << 19));
            if (Present & 1)
              ECX |= uint32_t{1} << 9;
            if (Present & 2)
              ECX |= uint32_t{1} << 19;
            return {0, 0, ECX, 0};
          }
          EXPECT_EQ(Leaf, 7U);
          uint32_t EBX = ~(uint32_t{1} << 29);
          if (Present & 4)
            EBX |= uint32_t{1} << 29;
          return {0, EBX, 0, 0};
        });
    EXPECT_EQ(Supported, Present == 7);
    // Missing SHA stops before the older vector query. Its two requirements
    // otherwise share one CPUID invocation.
    EXPECT_EQ(Leaves, (Present & 4 ? std::vector<uint32_t>{0, 7, 1}
                                   : std::vector<uint32_t>{0, 7}));
  }
}

TEST(DigestFeatures, NeverQueriesAnUnavailableLeaf) {
  for (uint32_t Maximum : {0, 1, 6}) {
    SCOPED_TRACE(Maximum);
    std::vector<uint32_t> Leaves;
    EXPECT_FALSE(x86HasSHA256Features(
        [&](uint32_t Leaf, uint32_t Subleaf) -> X86CPUIDResult {
          Leaves.push_back(Leaf);
          EXPECT_EQ(Leaf, 0U);
          EXPECT_EQ(Subleaf, 0U);
          // Non-EAX bits from leaf zero must not be mistaken for features.
          return {Maximum, UINT32_MAX, UINT32_MAX, UINT32_MAX};
        }));
    EXPECT_EQ(Leaves, (std::vector<uint32_t>{0}));
  }
}

} // namespace
} // namespace neverd::detail
