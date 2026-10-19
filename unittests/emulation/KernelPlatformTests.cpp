//===- KernelPlatformTests.cpp - Explicit guest platform facts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit guest platform facts.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
TEST(DriverPlatformScenario, BooleanFactsPreserveFalseAndRoundTrip) {
  for (bool Enabled : {false, true}) {
    auto O = driverOptionsFromScenarioJSON(
        Enabled ? R"({"secure_pci_enabled":true})"
                : R"({"secure_pci_enabled":false})");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    EXPECT_EQ(O->SecurePciEnabled, Enabled);
    auto Preserved = driverOptionsFromScenarioJSON("{}", *O);
    ASSERT_TRUE(bool(Preserved));
    EXPECT_EQ(Preserved->SecurePciEnabled, Enabled);
    DriverResult R;
    R.Configuration = *Preserved;
    auto Report = llvm::json::parse(driverResultJSON(R));
    ASSERT_TRUE(bool(Report));
    EXPECT_EQ(Report->getAsObject()
                  ->getObject("configuration")
                  ->getBoolean("secure_pci_enabled"),
              Enabled);
  }
  auto O = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(bool(O));
  EXPECT_FALSE(O->SecurePciEnabled.has_value());
  DriverResult R;
  R.Configuration = *O;
  auto Report = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(Report));
  EXPECT_EQ(Report->getAsObject()
                ->getObject("configuration")
                ->get("secure_pci_enabled"),
            nullptr);
}

TEST(DriverPlatformScenario, WrongTypesAreRejectedBeforeExecution) {
  for (const char *Value : {"null", "0", "1", "1.5", "\"false\"", "{}", "[]"}) {
    auto O = driverOptionsFromScenarioJSON(
        std::string("{\"secure_pci_enabled\":") + Value + "}");
    ASSERT_FALSE(bool(O)) << Value;
    EXPECT_NE(llvm::toString(O.takeError()).find("secure_pci_enabled"),
              std::string::npos);
  }
}

class KernelPlatform : public testing::Test {
protected:
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  llvm::Error initialize(const DriverOptions &O) {
    auto B = UnicornBackend::create(4 * 1024 * 1024);
    if (!B)
      return B.takeError();
    Memory = std::move(*B);
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    if (auto E = Model->initialize(I, O))
      return E;
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    return llvm::Error::success();
  }
};

TEST_F(KernelPlatform, MissingFactIsNotInferredFromCPUIDOrStaticReference) {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.CPUID.push_back({1, 0, {0, 0, uint32_t(1) << 31, 0}});
  auto E = initialize(O);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  auto R = Model->call("VslGetSecurePciEnabled", {});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("requires secure_pci_enabled"),
            std::string::npos);
}

TEST_F(KernelPlatform, AbsentVersionDoesNotInventAnUndocumentedContract) {
  DriverOptions O;
  O.SecurePciEnabled = false;
  auto E = initialize(O);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  auto R = Model->call("VslGetSecurePciEnabled", {});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("10.0.19041"),
            std::string::npos);
}

TEST_F(KernelPlatform, OtherVersionIsNotTreatedAs19041) {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 22621};
  O.SecurePciEnabled = true;
  auto E = initialize(O);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  auto R = Model->call("VslGetSecurePciEnabled", {});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("10.0.19041"),
            std::string::npos);
}

TEST_F(KernelPlatform, ExplicitFactIsCopiedAndTheQueryConsumesNoArguments) {
  for (bool Enabled : {false, true}) {
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.SecurePciEnabled = Enabled;
    auto E = initialize(O);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    O.SecurePciEnabled = !Enabled;
    EXPECT_EQ(Model->argumentCount("VslGetSecurePciEnabled"), 0u);
    for (unsigned I = 0; I != 3; ++I) {
      auto R = Model->call("VslGetSecurePciEnabled", {});
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(*R, uint64_t(Enabled));
    }
    auto Wrong = Model->call("VslGetSecurePciEnabled", {0});
    ASSERT_FALSE(bool(Wrong));
    EXPECT_NE(llvm::toString(Wrong.takeError()).find("argument count"),
              std::string::npos);
  }
}

TEST_F(KernelPlatform, HigherIRQLRemainsOutsideTheObservedProfile) {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.SecurePciEnabled = true;
  auto E = initialize(O);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  for (uint8_t Level : {1, 2, 15}) {
    auto Raised = Model->call("KfRaiseIrql", {Level});
    ASSERT_TRUE(bool(Raised));
    auto R = Model->call("VslGetSecurePciEnabled", {});
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("PASSIVE_LEVEL"),
              std::string::npos);
    EXPECT_EQ(Model->currentIRQL(), Level);
    auto Lowered = Model->call("KeLowerIrql", {0});
    ASSERT_TRUE(bool(Lowered));
    R = Model->call("VslGetSecurePciEnabled", {});
    ASSERT_TRUE(bool(R));
    EXPECT_EQ(*R, 1u);
  }
}

TEST(KernelPlatformDriver, ImportsAndDynamicLookupObserveBothBooleanFacts) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Enabled : {false, true}) {
        DriverOptions O;
        O.Contract = Contract;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.SecurePciEnabled = Enabled;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_secure_pci.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u);
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        EXPECT_EQ(R->Messages,
                  (std::vector<std::string>{Enabled ? "secure-pci:enabled"
                                                    : "secure-pci:disabled"}));
        unsigned Queries = 0;
        for (const auto &C : R->Calls)
          if (C.Name == "VslGetSecurePciEnabled") {
            EXPECT_TRUE(C.Arguments.empty());
            EXPECT_EQ(C.Result, uint64_t(Enabled));
            ++Queries;
          }
        EXPECT_EQ(Queries, 3u);
      }
}
} // namespace
} // namespace neverd::emulation
