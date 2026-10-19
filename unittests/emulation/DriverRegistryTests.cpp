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

#include "HvfTestPolicy.h"
#include "RegistryEnumerationTestFixture.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <filesystem>
#include <tuple>

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

TEST(DriverRegistry, ExplicitClockAndHistoricalTimesRoundTripWithoutDefaults) {
  auto Options = driverOptionsFromScenarioJSON(R"({
    "registry_system_time_100ns":1000,
    "registry":[{"path":"\\Registry\\Machine","last_write_time_100ns":0}]
  })");
  ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
  EXPECT_EQ(Options->RegistrySystemTime100ns, 1000u);
  EXPECT_EQ(Options->Registry->front().LastWriteTime100ns, 0u);
  auto Inherited = driverOptionsFromScenarioJSON("{}", *Options);
  ASSERT_TRUE(bool(Inherited)) << llvm::toString(Inherited.takeError());
  EXPECT_EQ(Inherited->RegistrySystemTime100ns, 1000u);
  DriverResult Report;
  Report.Configuration = *Options;
  Report.Registry = Options->Registry;
  const auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Report)));
  const auto *Configuration = JSON.getAsObject()->getObject("configuration");
  ASSERT_NE(Configuration, nullptr);
  EXPECT_EQ(Configuration->getInteger("registry_system_time_100ns"), 1000);
  const auto *Keys = JSON.getAsObject()->getArray("registry");
  ASSERT_NE(Keys, nullptr);
  EXPECT_EQ(Keys->front().getAsObject()->getInteger("last_write_time_100ns"),
            0);
  for (
      const char *Bad :
      {R"({"registry_system_time_100ns":1})",
       R"({"registry":[],"registry_system_time_100ns":null})",
       R"({"registry":[],"registry_system_time_100ns":true})",
       R"({"registry":[],"registry_system_time_100ns":1.0})",
       R"({"registry":[],"registry_system_time_100ns":-1})",
       R"({"registry":[],"registry_system_time_100ns":9223372036854775808})",
       R"({"registry":[{"path":"\\Registry\\Machine","last_write_time_100ns":null}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","last_write_time_100ns":true}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","last_write_time_100ns":-1}]})",
       R"({"registry":[{"path":"\\Registry\\Machine","last_write_time_100ns":9223372036854775808}]})"}) {
    SCOPED_TRACE(Bad);
    auto Result = driverOptionsFromScenarioJSON(Bad);
    ASSERT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
  }
  DriverOptions Bad;
  Bad.RegistrySystemTime100ns = 1;
  auto Result = emulateDriver("missing-registry-clock.sys", Bad);
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("explicit registry"),
            std::string::npos);
}

TEST(DriverRegistry, ExecutesQueriesMutationsAndHandleCleanup) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    SCOPED_TRACE(executionContractName(Contract));
    auto Options = driverOptionsFromScenarioJSON(Scenario);
    ASSERT_TRUE(static_cast<bool>(Options))
        << llvm::toString(Options.takeError());
    Options->Contract = Contract;
    auto Run = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_registry.sys",
                             *Options);
    ASSERT_TRUE(static_cast<bool>(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->NTStatus, 0u);
    EXPECT_TRUE(Run->UnloadCompleted);
    ASSERT_TRUE(Run->Registry);
    const auto Key = std::find_if(
        Run->Registry->begin(), Run->Registry->end(), [&](const auto &K) {
          return K.Path == Options->Registry->front().Path;
        });
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
    const auto Open =
        std::find_if(Run->Calls.begin(), Run->Calls.end(),
                     [](const auto &C) { return C.Name == "ZwOpenKey"; });
    ASSERT_NE(Open, Run->Calls.end());
    EXPECT_EQ(Open->Arguments.at(1), 0xc0000000u);
    const auto Create =
        std::find_if(Run->Calls.begin(), Run->Calls.end(),
                     [](const auto &C) { return C.Name == "ZwCreateKey"; });
    ASSERT_NE(Create, Run->Calls.end());
    EXPECT_EQ(Create->Arguments.at(1), 0x10000000u);
  }
}

class DriverRegistryEnumeration
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {};

TEST_P(DriverRegistryEnumeration,
       CompiledValuesAndDeletionPreserveLiveIndices) {
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(Backend, GuestArchitecture::X64))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  DriverOptions Options;
  Options.Backend = Backend;
  Options.Contract = Contract;
  if (Rebase)
    Options.LoadAddress = 0x1c0000000;
  Options.Registry = std::vector<DriverRegistryKey>{
      {"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\NeverDDriver",
       {{"Zulu", 4, {3, 0, 0, 0}}, {"Alpha", 4, {7, 0, 0, 0}}}}};
  auto Run = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_registry_values.sys",
                           Options);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
  EXPECT_EQ(Run->NTStatus, 0u);
  ASSERT_TRUE(Run->Registry);
  const auto &Values = Run->Registry->back().Values;
  ASSERT_EQ(Values.size(), 2u);
  EXPECT_EQ(Values[0].Name, "Alpha");
  EXPECT_EQ(Values[1].Name, "Zulu");
  EXPECT_EQ(Values[1].Data, (std::vector<uint8_t>{10, 0, 0, 0}));
}

INSTANTIATE_TEST_SUITE_P(
    Backends, DriverRegistryEnumeration,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()));

TEST_P(DriverRegistryEnumeration, CompiledKeyQueriesObserveChildDeletion) {
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(Backend, GuestArchitecture::X64))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  DriverOptions Options;
  Options.Backend = Backend;
  Options.Contract = Contract;
  if (Rebase)
    Options.LoadAddress = 0x1c0000000;
  Options.RegistrySystemTime100ns = 1000;
  const std::string Path =
      "\\Registry\\Machine\\System\\CurrentControlSet\\Services\\NeverDDriver";
  Options.Registry =
      std::vector<DriverRegistryKey>{{Path, {{"Mode", 4, {1, 0, 0, 0}}}, 123},
                                     {Path + "\\Longest", {}},
                                     {Path + "\\A", {}}};
  auto Run = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_registry_keys.sys",
                           Options);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
  EXPECT_EQ(Run->NTStatus, 0u);
  ASSERT_TRUE(Run->Registry);
  bool Found = false;
  for (const auto &Key : *Run->Registry) {
    EXPECT_NE(Key.Path, Path + "\\Longest");
    if (Key.Path == Path) {
      Found = true;
      EXPECT_EQ(Key.LastWriteTime100ns, 1000u);
    }
  }
  EXPECT_TRUE(Found);
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

TEST(DriverRegistryEnumeration,
     ExplicitMetadataAndOrderRoundTripWithoutFabricatedDefaults) {
  auto O = test::registryEnumerationOptions();
  DriverResult R;
  R.Configuration = O;
  auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(R)));
  auto Copy = driverOptionsFromScenarioJSON(
      "{\"registry\":" +
      llvm::formatv(
          "{0}",
          *JSON.getAsObject()->getObject("configuration")->get("registry"))
          .str() +
      "}");
  ASSERT_TRUE(bool(Copy)) << llvm::toString(Copy.takeError());
  EXPECT_EQ(Copy->Registry->at(0).SubkeyOrder, O.Registry->at(0).SubkeyOrder);
  EXPECT_FALSE(Copy->Registry->at(0).Information);
  EXPECT_EQ(Copy->Registry->at(1).Information->LastWriteTime,
            UINT64_C(0x1122334455667788));
  for (const char *Bad :
       {R"("information":null)", R"("information":{})",
        R"("information":{"last_write_time":0})",
        R"("information":{"last_write_time":-1,"class_name":""})",
        R"("information":{"last_write_time":0,"class_name":"Class"})",
        R"("subkey_order":null)", R"("subkey_order":["missing"])",
        R"("subkey_order":["A","a"])", R"("subkey_order":[1])"}) {
    auto Parsed = driverOptionsFromScenarioJSON(
        std::string(R"({"registry":[{"path":"\\Registry\\Machine\\NeverD",)") +
        Bad + "}]}");
    ASSERT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
  auto Missing = driverOptionsFromScenarioJSON(
      R"({"registry":[{"path":"\\Registry\\Machine\\NeverD","subkey_order":[]},{"path":"\\Registry\\Machine\\NeverD\\A"}]})");
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
}
TEST(DriverRegistryEnumeration,
     CompiledDriverEnumeratesAllClassesWithRealFiltersAndUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Paging : {false, true}) {
        auto O = test::registryEnumerationOptions();
        O.Contract = Contract;
        O.Unload = true;
        O.PageTables = Paging;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_registry_enumeration.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        ASSERT_EQ(R->Messages.size(), 1u);
        EXPECT_EQ(R->Messages[0], "registry enumeration and filters passed");
      }
}
} // namespace
} // namespace neverd::emulation
