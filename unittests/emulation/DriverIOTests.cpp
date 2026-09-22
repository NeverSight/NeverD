//===- DriverIOTests.cpp - Software driver lifecycle semantics ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest execution tests for buffered requests and orderly driver unload.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

namespace neverd::emulation {
namespace {

DriverRequest request(DriverRequestKind Kind) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.Device = "\\Device\\NeverDIO";
  return Request;
}

DriverOptions lifecycle(uint32_t Code = 0x222000) {
  DriverOptions Options;
  Options.Requests.push_back(request(DriverRequestKind::Create));
  auto IO = request(DriverRequestKind::DeviceControl);
  IO.ControlCode = Code;
  IO.Input = {0, 1, 0x5a, 0xff};
  IO.OutputSize = 8;
  Options.Requests.push_back(std::move(IO));
  Options.Requests.push_back(request(DriverRequestKind::Cleanup));
  Options.Requests.push_back(request(DriverRequestKind::Close));
  Options.Unload = true;
  return Options;
}

TEST(DriverIO, BufferedRequestKeepsFileContextAndUnloadsWithoutResources) {
  auto Result = emulateDriver(NEVERD_DRIVER_IO_FIXTURE, lifecycle());
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 4u);
  for (const auto &Request : Result->Requests) {
    EXPECT_TRUE(Request.Completed);
    EXPECT_EQ(Request.DispatchStatus, 0u);
    EXPECT_EQ(Request.IOStatus, 0u);
    EXPECT_NE(Request.IRP, 0u);
  }
  EXPECT_EQ(Result->Requests[0].Information, 1u);
  EXPECT_EQ(Result->Requests[1].Information, 4u);
  EXPECT_EQ(Result->Requests[1].Output,
            (std::vector<uint8_t>{0x5a, 0x5b, 0, 0xa5}));
  EXPECT_TRUE(Result->UnloadCompleted);
  EXPECT_TRUE(Result->Devices.empty());
}

TEST(DriverIO, FailedIOCTLStillCompletesAndAllowsCleanupCloseUnload) {
  auto Options = lifecycle();
  Options.Requests[1].OutputSize = 2;
  auto Result = emulateDriver(NEVERD_DRIVER_IO_FIXTURE, Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 4u);
  EXPECT_TRUE(Result->Requests[1].Completed);
  EXPECT_EQ(Result->Requests[1].IOStatus, 0xc0000023u);
  EXPECT_TRUE(Result->Requests[1].Output.empty());
  EXPECT_TRUE(Result->UnloadCompleted);
}

TEST(DriverIO, ReturningWithoutCompletionRetainsTheIncompleteRequest) {
  auto Result = emulateDriver(NEVERD_DRIVER_IO_FIXTURE, lifecycle(0x222004));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_EQ(Result->Requests[1].DispatchStatus, 0u);
  EXPECT_NE(Result->Diagnostic.find("without completing"), std::string::npos);
  EXPECT_FALSE(Result->UnloadCompleted);
}

TEST(DriverIO, PendingDispatchIsNotReportedAsCompleted) {
  auto Result = emulateDriver(NEVERD_DRIVER_IO_FIXTURE, lifecycle(0x222008));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
  EXPECT_NE(Result->Diagnostic.find("PENDING"), std::string::npos);
}

TEST(DriverIO, CompletionRejectsInformationBeyondTheOutputBuffer) {
  auto Result = emulateDriver(NEVERD_DRIVER_IO_FIXTURE, lifecycle(0x22200c));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_TRUE(Result->Requests[1].Output.empty());
  EXPECT_NE(Result->Diagnostic.find("Information"), std::string::npos);
}

TEST(DriverIO, RegistryPathStorageExpiresWhenDriverEntryReturns) {
  auto Result = emulateDriver(NEVERD_DRIVER_IO_FIXTURE, lifecycle(0x222010));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_TRUE(Result->Requests[0].Completed);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_NE(Result->Diagnostic.find("freed"), std::string::npos);
  EXPECT_FALSE(Result->UnloadCompleted);
}

TEST(DriverIO, RequestsCannotSkipCreateOrCleanup) {
  auto Options = lifecycle();
  Options.Requests.erase(Options.Requests.begin());
  auto Result = emulateDriver(NEVERD_DRIVER_IO_FIXTURE, Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  ASSERT_EQ(Result->Requests.size(), 1u);
  EXPECT_FALSE(Result->Requests[0].Completed);
  EXPECT_NE(Result->Diagnostic.find("CREATE"), std::string::npos);

  Options = lifecycle();
  Options.Requests.erase(Options.Requests.begin() + 2);
  Result = emulateDriver(NEVERD_DRIVER_IO_FIXTURE, Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  ASSERT_EQ(Result->Requests.size(), 3u);
  EXPECT_FALSE(Result->Requests[2].Completed);
  EXPECT_NE(Result->Diagnostic.find("order"), std::string::npos);
}

TEST(DriverIO, UnloadRequiresClosedFilesAndExactDeviceSelection) {
  auto Options = lifecycle();
  Options.Requests.resize(1);
  auto Result = emulateDriver(NEVERD_DRIVER_IO_FIXTURE, Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("file closed"), std::string::npos);
  EXPECT_FALSE(Result->UnloadCompleted);

  Options = lifecycle();
  Options.Requests[0].Device = "\\Device\\Other";
  Result = emulateDriver(NEVERD_DRIVER_IO_FIXTURE, Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("device name"), std::string::npos);
}

TEST(DriverIO, DirectAndNeitherTransfersStopBeforeCallingDispatch) {
  for (uint32_t Method = 1; Method != 4; ++Method) {
    auto Result =
        emulateDriver(NEVERD_DRIVER_IO_FIXTURE, lifecycle(0x222000 | Method));
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    ASSERT_EQ(Result->Requests.size(), 2u);
    EXPECT_FALSE(Result->Requests[1].Completed);
    EXPECT_EQ(Result->Requests[1].IRP, 0u);
    EXPECT_NE(Result->Diagnostic.find("METHOD_BUFFERED"), std::string::npos);
  }
}

} // namespace
} // namespace neverd::emulation
