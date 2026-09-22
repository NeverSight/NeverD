//===- DriverRegistryTests.cpp - Registry scenario execution --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks registry scenario parsing, preflight, execution and observations.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

#include <algorithm>

namespace neverd::emulation {
namespace {

constexpr char Scenario[] = R"({"registry":[{
  "path":"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\NeverDDriver",
  "values":[{"name":"Mode","type":4,"data":"78563412"}]
}],"unload":true})";

TEST(DriverRegistry, PreservesExplicitEmptyAndUnspecifiedInventories) {
  auto Default = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(static_cast<bool>(Default))
      << llvm::toString(Default.takeError());
  EXPECT_FALSE(Default->Registry);
  auto Empty = driverOptionsFromScenarioJSON(R"({"registry":[]})");
  ASSERT_TRUE(static_cast<bool>(Empty)) << llvm::toString(Empty.takeError());
  ASSERT_TRUE(Empty->Registry);
  EXPECT_TRUE(Empty->Registry->empty());
  auto Inherited = driverOptionsFromScenarioJSON("{}", *Empty);
  ASSERT_TRUE(static_cast<bool>(Inherited))
      << llvm::toString(Inherited.takeError());
  EXPECT_TRUE(Inherited->Registry);
}

TEST(DriverRegistry, RejectsAmbiguousMalformedAndUnboundedInventories) {
  for (
      const char *JSON :
      {R"({"registry":null})", R"({"registry":{}})", R"({"registry":[{}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","typo":1}]})",
       R"({"registry":[{"path":"relative"}]})",
       R"({"registry":[{"path":"\\Registry\\Machine\\"}]})",
       R"({"registry":[{"path":"\\Registry\\Machine"},{"path":"\\registry\\MACHINE"}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","values":{}}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","values":[{"name":"A","type":4,"data":"00","data":"11"}]}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","values":[{"name":"A","type":4,"data":"00"},{"name":"a","type":4,"data":"00"}]}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","values":[{"name":"A","type":-1,"data":"00"}]}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","values":[{"name":"A","type":4294967296,"data":"00"}]}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","values":[{"name":"A","type":4.0,"data":"00"}]}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","values":[{"name":"A","type":4,"data":"0x00"}]}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","values":[{"name":"A","type":4,"data":"0"}]}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","values":[{"name":"A","type":4,"data":"gg"}]}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","values":[{"name":"\u00e9","type":4,"data":"00"}]}]})"}) {
    SCOPED_TRACE(JSON);
    auto Options = driverOptionsFromScenarioJSON(JSON);
    ASSERT_FALSE(static_cast<bool>(Options));
    EXPECT_NE(llvm::toString(Options.takeError()).find("driver scenario:"),
              std::string::npos);
  }
  DriverOptions Options;
  Options.Registry = std::vector<DriverRegistryKey>{{"relative", {}}};
  auto Run = emulateDriver("missing-registry-preflight.sys", Options);
  ASSERT_FALSE(static_cast<bool>(Run));
  EXPECT_EQ(llvm::toString(Run.takeError()).find("driver scenario:"), 0u);

  Options.Registry = std::vector<DriverRegistryKey>{
      {"\\Registry\\Machine",
       {{"Data", 3, std::vector<uint8_t>(MaxRegistryValueBytes + 1)}}}};
  Run = emulateDriver("missing-registry-preflight.sys", Options);
  ASSERT_FALSE(static_cast<bool>(Run));
  EXPECT_EQ(llvm::toString(Run.takeError()).find("driver scenario:"), 0u);
}

TEST(DriverRegistry, ExecutesQueriesMutationsAndHandleCleanup) {
  auto Options = driverOptionsFromScenarioJSON(Scenario);
  ASSERT_TRUE(static_cast<bool>(Options))
      << llvm::toString(Options.takeError());
  auto Run = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_registry.sys",
                           *Options);
  ASSERT_TRUE(static_cast<bool>(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
  EXPECT_EQ(Run->NTStatus, 0u);
  EXPECT_TRUE(Run->UnloadCompleted);
  ASSERT_TRUE(Run->Registry);
  const auto Key = std::find_if(
      Run->Registry->begin(), Run->Registry->end(),
      [&](const auto &K) { return K.Path == Options->Registry->front().Path; });
  ASSERT_NE(Key, Run->Registry->end());
  ASSERT_EQ(Key->Values.size(), 2u);
  const auto Value =
      std::find_if(Key->Values.begin(), Key->Values.end(),
                   [](const auto &V) { return V.Name == "Observed"; });
  ASSERT_NE(Value, Key->Values.end());
  EXPECT_EQ(Value->Type, 4u);
  EXPECT_EQ(Value->Data, (std::vector<uint8_t>{0x78, 0x56, 0x34, 0x12}));
  EXPECT_TRUE(std::none_of(
      Run->Registry->begin(), Run->Registry->end(),
      [](const auto &K) { return K.Path.ends_with("Temporary"); }));
  auto JSON = llvm::json::parse(driverResultJSON(*Run));
  ASSERT_TRUE(static_cast<bool>(JSON)) << llvm::toString(JSON.takeError());
  ASSERT_NE(JSON->getAsObject(), nullptr);
  EXPECT_NE(JSON->getAsObject()->getArray("registry"), nullptr);
  EXPECT_EQ(JSON->getAsObject()->getBoolean("scenario_success"), true);
  const auto *Configuration = JSON->getAsObject()->getObject("configuration");
  ASSERT_NE(Configuration, nullptr);
  const auto *Initial = Configuration->getArray("registry");
  ASSERT_NE(Initial, nullptr);
  ASSERT_EQ(Initial->size(), 1u);
  EXPECT_EQ(Initial->front().getAsObject()->getArray("values")->size(), 1u);
}

TEST(DriverRegistry,
     UnspecifiedAvailabilityStopsAndExplicitAbsenceReturnsStatus) {
  for (bool Configured : {false, true}) {
    DriverOptions Options;
    if (Configured)
      Options.Registry.emplace();
    auto Run = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_registry.sys",
                             Options);
    ASSERT_TRUE(static_cast<bool>(Run)) << llvm::toString(Run.takeError());
    if (Configured) {
      EXPECT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
      EXPECT_TRUE(Run->NTStatus && (*Run->NTStatus & 0x80000000u));
    } else {
      EXPECT_EQ(Run->Stop, DriverStopReason::ModelError);
      EXPECT_NE(Run->Diagnostic.find("registry"), std::string::npos);
    }
  }
}

TEST(DriverRegistry, SnapshotRetainsMutationsBeforeAFaultOrHandleLeakStop) {
  for (uint8_t Mode : {1, 2}) {
    auto Options = driverOptionsFromScenarioJSON(Scenario);
    ASSERT_TRUE(static_cast<bool>(Options))
        << llvm::toString(Options.takeError());
    Options->Registry->front().Values.front().Data = {Mode, 0, 0, 0};
    auto Run = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_registry.sys",
                             *Options);
    ASSERT_TRUE(static_cast<bool>(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, Mode == 1 ? DriverStopReason::MemoryFault
                                   : DriverStopReason::ModelError)
        << Run->Diagnostic;
    EXPECT_FALSE(Run->UnloadCompleted);
    if (Mode == 2)
      EXPECT_NE(Run->Diagnostic.find("live registry handles"),
                std::string::npos);
    ASSERT_TRUE(Run->Registry);
    bool Observed = false;
    for (const auto &Key : *Run->Registry)
      for (const auto &Value : Key.Values)
        if (Value.Name == "Observed") {
          Observed = true;
          EXPECT_EQ(Value.Data, (std::vector<uint8_t>{Mode, 0, 0, 0}));
        }
    EXPECT_TRUE(Observed);
  }
}

} // namespace
} // namespace neverd::emulation
