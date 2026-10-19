//===- DriverProcessCreateTests.cpp - Actual extended create callbacks ----===//
///
/// \file
/// Actual extended create callbacks.
///

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
namespace {
DriverOptions createOptions(bool Veto = false) {
  DriverOptions O;
  O.Unload = true;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  DriverProcessEvent E{DriverProcessEvent::Kind::Create, 100, 4, 4, 0};
  E.CreateInfo = DriverProcessCreateInfo{true, "\\??\\C:\\NeverDCreate.exe",
                                         "NeverDCreate.exe --test"};
  O.ProcessEvents.push_back(E);
  if (!Veto)
    O.ProcessEvents.push_back({DriverProcessEvent::Kind::Exit, 100, 4, 4, 0});
  return O;
}
TEST(DriverProcessCreate,
     ActualCreateAndExitPreserveOrderIdentityAndBorrowedInfo) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = createOptions();
      O.Contract = Contract;
      if (Preemptive)
        O.Scheduling = DriverScheduling{17, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_process_create.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      ASSERT_EQ(R->ProcessEvents.size(), 2u);
      EXPECT_EQ(R->ProcessEvents[0].CreationStatus, 0u);
      EXPECT_EQ(R->ProcessEvents[0].DeliveredRoutines.size(), 3u);
      EXPECT_EQ(R->ProcessEvents[1].DeliveredRoutines.size(), 3u);
      EXPECT_FALSE(R->ProcessEvents[1].CreationStatus);
      auto J = llvm::cantFail(llvm::json::parse(driverResultJSON(*R)));
      EXPECT_EQ(J.getAsObject()->getBoolean("scenario_success"), true);
    }
}
TEST(DriverProcessCreate, VetoStopsRemainingCallbacksAndRecordsFailedCreation) {
  auto O = createOptions(true);
  auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                             "driver_process_create_veto.sys",
                         O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
  EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
  ASSERT_EQ(R->ProcessEvents.size(), 1u);
  EXPECT_TRUE(R->ProcessEvents[0].Completed);
  EXPECT_EQ(R->ProcessEvents[0].CreationStatus, 0xc0000022u);
  EXPECT_EQ(R->ProcessEvents[0].DeliveredRoutines.size(), 2u);
  auto J = llvm::cantFail(llvm::json::parse(driverResultJSON(*R)));
  EXPECT_EQ(J.getAsObject()->getBoolean("scenario_success"), false);
}
TEST(DriverProcessCreate, CreateInfoStorageExpiresBeforeUnload) {
  auto O = createOptions();
  auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                             "driver_process_create_borrow.sys",
                         O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::ModelError) << R->Diagnostic;
  EXPECT_NE(R->Diagnostic.find("freed"), std::string::npos) << R->Diagnostic;
  EXPECT_FALSE(R->UnloadCompleted);
}
TEST(DriverProcessCreate,
     CreateInfoSchemaRejectsExitUseMalformedNamesAndUnknownFields) {
  for (
      const auto *Info :
      {R"({"image_file_name":"x"})",
       R"({"file_open_name_available":true,"image_file_name":"x","unknown":1})",
       R"({"file_open_name_available":true,"image_file_name":"x","command_line":3})"}) {
    std::string S =
        std::string("{\"process_events\":[{\"kind\":\"create\",\"process_id\":"
                    "100,\"parent_id\":4,\"create_info\":") +
        Info + "}]}";
    auto P = driverOptionsFromScenarioJSON(S, createOptions());
    ASSERT_FALSE(bool(P));
    llvm::consumeError(P.takeError());
  }
}
} // namespace
} // namespace neverd::emulation
