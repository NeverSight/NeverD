///
/// \file
/// DriverCPUIDPolicyTests.
///

#include "CPUIDPolicyTestFixture.h"
#include "gtest/gtest.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
TEST(DriverCPUIDPolicy, DefaultRemainsExactAndDoesNotForwardHostQueries) {
  auto O = cpuidpolicyfixture::options();
  O.CPUIDPolicy.reset();
  auto M = DriverCPUIDModel::fromDriverOptions(O);
  ASSERT_TRUE(bool(M));
  EXPECT_EQ(M->query(7, 2), cpuidpolicyfixture::entry(O, 7, 2));
  EXPECT_FALSE(M->query(0xaabbccdd, 2));
  EXPECT_FALSE(M->query(0x40000001, 0));
  EXPECT_FALSE(M->query(6, 0));
}
TEST(DriverCPUIDPolicy, IntelFallbackUsesGuestMaximumAndExactECX) {
  auto O = cpuidpolicyfixture::options();
  auto M = DriverCPUIDModel::fromDriverOptions(O);
  ASSERT_TRUE(bool(M)) << llvm::toString(M.takeError());
  const auto Expected0 = cpuidpolicyfixture::entry(O, 7, 0),
             Expected2 = cpuidpolicyfixture::entry(O, 7, 2);
  cpuidpolicyfixture::entry(O, 7, 2) = {};
  for (uint32_t Leaf : {8u, 0x3fffffffu, 0x50000000u, 0x7fffffffu, 0x80000009u,
                        0xaabbccddu, 0xffffffffu}) {
    EXPECT_EQ(M->query(Leaf, 0), Expected0);
    EXPECT_EQ(M->query(Leaf, 2), Expected2);
    EXPECT_FALSE(M->query(Leaf, 3));
  }
  for (uint32_t Leaf : {2u, 6u, 0x80000001u, 0x80000008u})
    EXPECT_FALSE(
        M->query(Leaf, 0)); // Missing in-range observations stay unknown.
  EXPECT_FALSE(M->query(0x40000100, 0));
  EXPECT_FALSE(M->query(0x4fffffff, 0));
}
TEST(DriverCPUIDPolicy, NeverDDiscoveryIsCoherentAndIndependentOfSubleaf) {
  auto O = cpuidpolicyfixture::options();
  auto M = DriverCPUIDModel::fromDriverOptions(O);
  ASSERT_TRUE(bool(M));
  for (uint32_t S : {0u, 1u, 11u, UINT32_MAX}) {
    EXPECT_EQ(
        M->query(0x40000000, S),
        (DriverCPUIDRegisters{0x40000001, 0x6576654e, 0x45204472, 0x2020756d}));
    EXPECT_EQ(M->query(0x40000001, S),
              (DriverCPUIDRegisters{0x3123444e, 0, 0, 0}));
    for (uint32_t L : {0x40000002u, 0x400000ffu})
      EXPECT_EQ(M->query(L, S), DriverCPUIDRegisters{});
  }
}
TEST(DriverCPUIDPolicy, InvalidOrContradictoryPoliciesFailBeforeExecution) {
  std::vector<DriverOptions> Bad;
  auto Add = [&](auto Mutate) {
    auto O = cpuidpolicyfixture::options();
    Mutate(O);
    Bad.push_back(std::move(O));
  };
  Add([](auto &O) { cpuidpolicyfixture::erase(O, 0, 0); });
  Add([](auto &O) { cpuidpolicyfixture::erase(O, 0x80000000, 0); });
  Add([](auto &O) { cpuidpolicyfixture::erase(O, 7, 0); });
  Add([](auto &O) { cpuidpolicyfixture::entry(O, 0, 0)[1] ^= 1; });
  Add([](auto &O) { cpuidpolicyfixture::entry(O, 0, 0)[0] = 0x21; });
  Add([](auto &O) { cpuidpolicyfixture::entry(O, 0x80000000, 0)[0] = 1; });
  Add([](auto &O) { cpuidpolicyfixture::entry(O, 1, 0)[2] = 0; });
  Add([](auto &O) {
    cpuidpolicyfixture::entry(O, 0x40000000, 0)[0] = 0x40000000;
  });
  Add([](auto &O) {
    cpuidpolicyfixture::entry(O, 0x40000001, 0) = {0x31237648, 0, 0, 0};
  });
  Add([](auto &O) { cpuidpolicyfixture::entry(O, 0xaabbccdd, 2) = {}; });
  Add([](auto &O) {
    O.CPUIDPolicy->InvalidLeaf = DriverCPUIDInvalidLeafPolicy(255);
  });
  Add([](auto &O) {
    O.CPUIDPolicy->Hypervisor = DriverCPUIDHypervisorPolicy(255);
  });
  for (auto &O : Bad) {
    auto M = DriverCPUIDModel::fromDriverOptions(O);
    ASSERT_FALSE(bool(M));
    llvm::consumeError(M.takeError());
    auto R = driverOptionsFromScenarioJSON("{}", O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("cpuid_policy"),
              std::string::npos);
  }
}
TEST(DriverCPUIDPolicy, ScenarioRoundTripPreservesOptInAndStrictDefaults) {
  auto O = cpuidpolicyfixture::options();
  O.CPUIDPolicy.reset();
  auto R = driverOptionsFromScenarioJSON(
      R"({"cpuid_policy":{"invalid_leaf":"intel-max-basic-v1","hypervisor":"neverd-v1"}})",
      O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->CPUIDPolicy->InvalidLeaf,
            DriverCPUIDInvalidLeafPolicy::IntelMaxBasicV1);
  EXPECT_EQ(R->CPUIDPolicy->Hypervisor, DriverCPUIDHypervisorPolicy::NeverDV1);
  auto P = driverOptionsFromScenarioJSON("{}", *R);
  ASSERT_TRUE(bool(P));
  DriverResult Result;
  Result.Configuration = *P;
  auto J = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(J));
  const auto *Policy =
      J->getAsObject()->getObject("configuration")->getObject("cpuid_policy");
  ASSERT_NE(Policy, nullptr);
  EXPECT_EQ(Policy->getString("invalid_leaf"), "intel-max-basic-v1");
  EXPECT_EQ(Policy->getString("hypervisor"), "neverd-v1");
  for (const char *Bad :
       {"null", "0", "{\"invalid_leaf\":0}", "{\"invalid_leaf\":\"guess\"}",
        "{\"hypervisor\":\"hyperv\"}", "{\"unknown\":0}"}) {
    auto X = driverOptionsFromScenarioJSON(
        std::string("{\"cpuid_policy\":") + Bad + "}", O);
    ASSERT_FALSE(bool(X));
    llvm::consumeError(X.takeError());
  }
  auto Cleared = driverOptionsFromScenarioJSON(R"({"cpuid":[]})", *R);
  ASSERT_FALSE(bool(Cleared));
  llvm::consumeError(Cleared.takeError());
  auto Strict =
      driverOptionsFromScenarioJSON(R"({"cpuid":[],"cpuid_policy":{}})", *R);
  ASSERT_TRUE(bool(Strict));
  EXPECT_TRUE(Strict->CPUID.empty());
  auto M = DriverCPUIDModel::fromDriverOptions(*Strict);
  ASSERT_TRUE(bool(M));
  EXPECT_FALSE(M->enabled());
}
TEST(DriverCPUIDPolicyCaller,
     DerivedAndGeneratedResultsPreserveFullCpuContract) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = cpuidpolicyfixture::options();
      O.Contract = Contract;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_cpuid_policy.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u);
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    }
}
TEST(DriverCPUIDPolicyCaller,
     MissingValidLeavesAndMissingMaximumSubleavesStillStop) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    auto O = cpuidpolicyfixture::options();
    if (Mode == 0)
      O.CPUIDPolicy.reset();
    if (Mode == 1)
      cpuidpolicyfixture::erase(O, 7, 2);
    const char *Fixture = Mode == 2 ? "driver_cpuid_policy_unknown.sys"
                                    : "driver_cpuid_policy.sys";
    auto R = emulateDriver(
        std::filesystem::path(NEVERD_DRIVER_FIXTURES) / Fixture, O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, DriverStopReason::UnsupportedInstruction)
        << R->Diagnostic;
    EXPECT_FALSE(R->NTStatus.has_value());
    EXPECT_NE(R->Diagnostic.find("driver cpuid:"), std::string::npos);
  }
}
} // namespace
} // namespace neverd::emulation
