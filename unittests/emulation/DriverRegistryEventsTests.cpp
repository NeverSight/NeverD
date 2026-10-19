//===- DriverRegistryEventsTests.cpp - External registry filter stimuli ---===//
// NeverD Decompiler
///
/// \file
/// External registry filter stimuli.
///

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
using Kind = DriverRegistryEvent::Kind;
DriverOptions options() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.Registry = std::vector<DriverRegistryKey>{
      {"\\Registry\\Machine\\Software\\NeverD", {{"Mode", 4, {1, 0, 0, 0}}}}};
  O.Unload = true;
  return O;
}
DriverRegistryEvent open(uint32_t ID = 1, uint32_t Access = 3) {
  DriverRegistryEvent E;
  E.Handle = ID;
  E.DesiredAccess = Access;
  E.Path = "\\Registry\\Machine\\Software\\NeverD";
  return E;
}
DriverRegistryEvent event(Kind K, uint32_t Value = 0, uint32_t ID = 1) {
  DriverRegistryEvent E;
  E.Operation = K;
  E.Handle = ID;
  if (K == Kind::SetValue || K == Kind::QueryValue)
    E.ValueName = "Mode";
  if (K == Kind::SetValue) {
    E.ValueType = 4;
    E.Data = {uint8_t(Value), uint8_t(Value >> 8), 0, 0};
  }
  if (K == Kind::QueryValue)
    E.OutputSize = 16;
  return E;
}
std::filesystem::path fixture(bool Filter) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (Filter ? "driver_registry_events.sys"
                 : "driver_registry_events_plain.sys");
}
uint32_t data(const DriverRegistryEventResult &R) {
  EXPECT_EQ(R.Output.size(), 16u);
  return R.Output.size() == 16
             ? llvm::support::endian::read32le(R.Output.data() + 12)
             : 0;
}
TEST(DriverRegistryEvents,
     ActualGuestFiltersBlockBypassAndOverrideCommittedWrites) {
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    for (bool Preemptive : {false, true}) {
      auto O = options();
      O.Contract = C;
      if (Preemptive)
        O.Scheduling = DriverScheduling{17, 1};
      O.RegistryEvents = {open(),
                          event(Kind::SetValue, 0x1234),
                          event(Kind::QueryValue),
                          event(Kind::SetValue, 0xbaad),
                          event(Kind::QueryValue),
                          event(Kind::SetValue, 0xbeef),
                          event(Kind::QueryValue),
                          event(Kind::SetValue, 0xcafe),
                          event(Kind::QueryValue),
                          event(Kind::Close)};
      auto R = emulateDriver(fixture(true), O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      ASSERT_EQ(R->RegistryEvents.size(), 10u);
      for (const auto &E : R->RegistryEvents)
        EXPECT_TRUE(E.Completed);
      EXPECT_EQ(R->RegistryEvents[3].Status, 0xc0000022u);
      EXPECT_EQ(R->RegistryEvents[5].Status, 0u);
      EXPECT_EQ(R->RegistryEvents[7].Status, 0xc0000022u);
      for (size_t I : {2, 4, 6})
        EXPECT_EQ(data(R->RegistryEvents[I]), 0x1234u);
      EXPECT_EQ(data(R->RegistryEvents[8]), 0xcafeu);
      auto J = llvm::cantFail(llvm::json::parse(driverResultJSON(*R)));
      EXPECT_EQ(J.getAsObject()->getBoolean("scenario_success"), false);
      EXPECT_EQ(J.getAsObject()->getArray("registry_events")->size(), 10u);
    }
  }
}
TEST(DriverRegistryEvents,
     PlainProviderPreservesAccessFailureAndQueryBufferStatus) {
  for (bool Filter : {false, true}) {
    auto O = options();
    auto Small = event(Kind::QueryValue);
    Small.OutputSize = 0;
    auto Partial = Small;
    Partial.OutputSize = 12;
    O.RegistryEvents = {open(1, 1),
                        Small,
                        Partial,
                        event(Kind::SetValue, 2),
                        event(Kind::QueryValue),
                        event(Kind::Close)};
    auto R = emulateDriver(fixture(Filter), O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    ASSERT_EQ(R->RegistryEvents.size(), 6u);
    EXPECT_EQ(R->RegistryEvents[1].Status, 0xc0000023u);
    EXPECT_EQ(R->RegistryEvents[1].RequiredSize, 16u);
    EXPECT_EQ(R->RegistryEvents[2].Status, 0x80000005u);
    EXPECT_EQ(R->RegistryEvents[2].RequiredSize, 16u);
    EXPECT_EQ(R->RegistryEvents[3].Status, 0xc0000022u);
    EXPECT_EQ(data(R->RegistryEvents[4]), 1u);
  }
}
TEST(DriverRegistryEvents, FailedOpenRetainsFailureAndDoesNotInventAKeyHandle) {
  auto O = options();
  auto Missing = open();
  Missing.Path += "\\Missing";
  O.RegistryEvents = {Missing, event(Kind::QueryValue), event(Kind::Close)};
  auto R = emulateDriver(fixture(true), O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
  ASSERT_EQ(R->RegistryEvents.size(), 3u);
  EXPECT_EQ(R->RegistryEvents[0].Status, 0xc0000034u);
  EXPECT_EQ(R->RegistryEvents[1].Status, 0xc0000008u);
  EXPECT_EQ(R->RegistryEvents[2].Status, 0xc0000008u);
  EXPECT_TRUE(R->UnloadCompleted);
}
TEST(DriverRegistryEvents,
     StrictScenarioValidationRejectsUnknownOrInapplicableInputs) {
  auto O = options();
  const std::vector<std::string> Bad = {
      "null", "[{}]", R"([{"kind":"close","handle":1,"before_request":0}])",
      R"([{"kind":"open","handle":1,"before_request":0,"path":"\\Registry\\Machine","desired_access":3,"data":"00"}])",
      R"([{"kind":"open","handle":1,"before_request":0,"path":"\\Registry\\Machine","desired_access":3}])"};
  for (const auto &A : Bad) {
    auto P =
        driverOptionsFromScenarioJSON("{\"registry_events\":" + A + "}", O);
    ASSERT_FALSE(bool(P)) << A;
    llvm::consumeError(P.takeError());
  }
  O.RegistryEvents = {open(), event(Kind::Close)};
  O.Registry.reset();
  auto E = validateDriverRegistryEvents(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST(DriverRegistryEvents, CompletedEventInputCannotBeRetainedUntilUnload) {
  auto O = options();
  O.RegistryEvents = {open(), event(Kind::SetValue, 7), event(Kind::Close)};
  auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                             "driver_registry_events_borrow.sys",
                         O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::ModelError) << R->Diagnostic;
  EXPECT_FALSE(R->UnloadCompleted);
  ASSERT_EQ(R->RegistryEvents.size(), 3u);
  EXPECT_TRUE(R->RegistryEvents.back().Completed);
  EXPECT_NE(R->Diagnostic.find("freed"), std::string::npos) << R->Diagnostic;
}
TEST(DriverRegistryEvents, RelativeOpensPreserveRootIdentityAndCreatedActor) {
  for (bool Preemptive : {false, true}) {
    auto O = options();
    if (Preemptive)
      O.Scheduling = DriverScheduling{17, 1};
    O.ProcessEvents = {{DriverProcessEvent::Kind::Create, 100, 4, 4, 0}};
    auto Root = open();
    Root.Path = "\\Registry\\Machine";
    auto Child = open(2);
    Child.Path = "Software\\NeverD";
    Child.RootHandle = 1;
    O.RegistryEvents = {Root,
                        Child,
                        event(Kind::SetValue, 7, 2),
                        event(Kind::QueryValue, 0, 2),
                        event(Kind::Close, 0, 2),
                        event(Kind::Close)};
    for (auto &E : O.RegistryEvents)
      E.ActorProcessID = 100;
    auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_registry_events_actor.sys",
                           O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    ASSERT_EQ(R->RegistryEvents.size(), 6u);
    EXPECT_EQ(data(R->RegistryEvents[3]), 7u);
    for (const auto &E : R->RegistryEvents) {
      EXPECT_TRUE(E.Completed);
      EXPECT_EQ(E.Status, 0u);
    }
  }
}
TEST(DriverRegistryEvents, InvalidActorsAndClosedRootsFailBeforeExecution) {
  auto O = options();
  O.RegistryEvents = {open(), event(Kind::Close)};
  O.RegistryEvents[0].ActorProcessID = 100;
  auto E = validateDriverRegistryEvents(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.ProcessEvents = {{DriverProcessEvent::Kind::Create, 100, 4, 4, 0},
                     {DriverProcessEvent::Kind::Exit, 100, 4, 4, 0}};
  E = validateDriverRegistryEvents(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O = options();
  auto Child = open(2);
  Child.RootHandle = 1;
  Child.Path = "Software\\NeverD";
  O.RegistryEvents = {open(), event(Kind::Close), Child,
                      event(Kind::Close, 0, 2)};
  E = validateDriverRegistryEvents(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
} // namespace
} // namespace neverd::emulation
