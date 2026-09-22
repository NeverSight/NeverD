//===- DriverKMDFLifecycleTests.cpp - Genuine KMDF fixture execution ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute an optional original fixture linked through the genuine WDK KMDF
/// entry library. No replacement framework loader stub supplies test success.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {

#ifdef NEVERD_KMDF_FIXTURE
DriverOptions lifecycleOptions(llvm::StringRef Service = "NeverDKmdf0") {
  DriverOptions Options;
  Options.ServiceName = Service.str();
  Options.Unload = true;
  return Options;
}

std::vector<std::string> lifecycleMessages(const DriverResult &Result) {
  std::vector<std::string> Messages;
  for (const auto &Message : Result.Messages)
    if (llvm::StringRef(Message).starts_with("KMDF lifecycle:"))
      Messages.push_back(Message);
  return Messages;
}

void checkLifecycle(llvm::StringRef Path, const DriverOptions &Options,
                    llvm::StringRef ExtraMessage = {}) {
  auto Result = emulateDriver(Path.str(), Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  EXPECT_TRUE(Result->UnloadCompleted);
  EXPECT_TRUE(Result->Devices.empty());
  std::vector<std::string> Expected{
      "KMDF lifecycle: driver created\n", "KMDF lifecycle: child cleanup\n",
      "KMDF lifecycle: child destroy\n",  "KMDF lifecycle: child released\n",
      "KMDF lifecycle: driver unload\n",  "KMDF lifecycle: driver cleanup\n",
      "KMDF lifecycle: driver destroy\n"};
  if (ExtraMessage == "duplicate driver checked")
    Expected.insert(Expected.begin() + 1,
                    "KMDF lifecycle: duplicate driver checked\n");
  else if (ExtraMessage == "delete pending checked")
    Expected.insert(Expected.begin() + 2,
                    "KMDF lifecycle: delete pending checked\n");
  EXPECT_EQ(lifecycleMessages(*Result), Expected);
  auto Create = std::find_if(
      Result->Calls.begin(), Result->Calls.end(),
      [](const auto &Call) { return Call.Name == "WdfDriverCreate"; });
  ASSERT_NE(Create, Result->Calls.end());
  EXPECT_EQ(Create->Arguments.size(), 6u);
  EXPECT_EQ(Create->Result, 0u);
  EXPECT_TRUE(std::any_of(Result->Calls.begin(), Result->Calls.end(),
                          [](const auto &Call) {
                            return Call.Name == "WdfObjectAllocateContext" &&
                                   Call.Result == 0x40000000u;
                          }));
}

void checkCreateFailure(llvm::StringRef Service, uint32_t Status) {
  auto Result = emulateDriver(NEVERD_KMDF_FIXTURE, lifecycleOptions(Service));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, Status);
  EXPECT_FALSE(Result->UnloadCompleted);
  EXPECT_TRUE(lifecycleMessages(*Result).empty());
}

void checkHandleFailure(llvm::StringRef Service, llvm::StringRef API) {
  auto Result = emulateDriver(NEVERD_KMDF_FIXTURE, lifecycleOptions(Service));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_FALSE(Result->UnloadCompleted);
  ASSERT_FALSE(Result->Calls.empty());
  EXPECT_EQ(Result->Calls.back().Name, API);
  EXPECT_FALSE(Result->Calls.back().Result);
  EXPECT_FALSE(Result->Diagnostic.empty());
  for (const auto &Message : Result->Messages)
    EXPECT_EQ(Message.find("forbidden"), std::string::npos) << Message;
}

TEST(DriverKMDFLifecycle, RealEntryBindingContextsAndUnloadCallbacks) {
  checkLifecycle(NEVERD_KMDF_FIXTURE, lifecycleOptions());
}

TEST(DriverKMDFLifecycle, RelocatedEntryRetainsFunctionAndContextIdentity) {
  auto Options = lifecycleOptions();
  Options.LoadAddress = 0x190000000;
  checkLifecycle(NEVERD_KMDF_FIXTURE, Options);
}

TEST(DriverKMDFLifecycle, DuplicateDriverCreatePreservesLiveDriver) {
  checkLifecycle(NEVERD_KMDF_FIXTURE, lifecycleOptions("NeverDKmdfD"),
                 "duplicate driver checked");
}

TEST(DriverKMDFLifecycle, InvalidDriverConfigSizeReturnsDocumentedStatus) {
  checkCreateFailure("NeverDKmdfS", 0xc0000004u);
}

TEST(DriverKMDFLifecycle, NonPnpAddDeviceReturnsDocumentedStatus) {
  checkCreateFailure("NeverDKmdfA", 0xc000000du);
}

TEST(DriverKMDFLifecycle, ContextAllocationRejectsDeletePendingObject) {
  checkLifecycle(NEVERD_KMDF_FIXTURE, lifecycleOptions("NeverDKmdfP"),
                 "delete pending checked");
}

TEST(DriverKMDFLifecycle, FinalDereferenceInvalidatesObjectHandle) {
  checkHandleFailure("NeverDKmdfF", "WdfObjectGetTypedContextWorker");
}

TEST(DriverKMDFLifecycle, UnpairedDereferenceStopsBeforeDestroy) {
  checkHandleFailure("NeverDKmdfR", "WdfObjectDereferenceActual");
}

TEST(DriverKMDFLifecycle, NestedWorkerCleanupWaitPreservesBothContinuations) {
  const auto Options = lifecycleOptions("NeverDKmdfW");
  checkLifecycle(NEVERD_KMDF_FIXTURE, Options);
  auto Result = emulateDriver(NEVERD_KMDF_FIXTURE, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  std::vector<std::string> Messages;
  for (const auto &Message : Result->Messages)
    if (llvm::StringRef(Message).starts_with("KMDF nested:"))
      Messages.push_back(Message);
  EXPECT_EQ(Messages, (std::vector<std::string>{
                          "KMDF nested: delayed cleanup resumed\n",
                          "KMDF nested: worker resumed\n",
                          "KMDF nested: parent cleanup resumed\n"}));
  for (const auto &Call : Result->Calls)
    if (Call.Name == "KeWaitForSingleObject" ||
        Call.Name == "KeDelayExecutionThread")
      EXPECT_EQ(Call.Result, 0u);
}

TEST(DriverKMDFLifecycle, UnmodeledTableEntryStopsAtItsExactIdentity) {
  auto Result =
      emulateDriver(NEVERD_KMDF_FIXTURE, lifecycleOptions("NeverDKmdfU"));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedAPI);
  EXPECT_NE(Result->Diagnostic.find("WdfObjectQuery"), std::string::npos);
  EXPECT_FALSE(Result->UnloadCompleted);
  for (const auto &Message : Result->Messages)
    EXPECT_EQ(Message.find("forbidden"), std::string::npos);
}

TEST(DriverKMDFLifecycle, FailedEntryCleansObjectsWithoutDriverUnloadCallback) {
  auto Result =
      emulateDriver(NEVERD_KMDF_FIXTURE, lifecycleOptions("NeverDKmdfE"));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0xc0000001u);
  EXPECT_FALSE(Result->UnloadCompleted);
  EXPECT_EQ(lifecycleMessages(*Result),
            (std::vector<std::string>{"KMDF lifecycle: driver created\n",
                                      "KMDF lifecycle: child cleanup\n",
                                      "KMDF lifecycle: child destroy\n",
                                      "KMDF lifecycle: child released\n",
                                      "KMDF lifecycle: driver cleanup\n",
                                      "KMDF lifecycle: driver destroy\n"}));
  auto Unbind = std::find_if(
      Result->Calls.begin(), Result->Calls.end(),
      [](const auto &Call) { return Call.Name == "WdfVersionUnbind"; });
  ASSERT_NE(Unbind, Result->Calls.end());
  EXPECT_EQ(Unbind->Result, 0u);
}

TEST(DriverKMDFLifecycle, ActiveCFGEntryAndIndirectCallbacks) {
#ifdef NEVERD_KMDF_CFG_FIXTURE
  checkLifecycle(NEVERD_KMDF_CFG_FIXTURE, lifecycleOptions());
#else
  GTEST_SKIP()
      << "A genuine WDK fixture linked with /guard:cf was not supplied";
#endif
}

#else
TEST(DriverKMDFLifecycle, RequiresOptionalGenuineWDKFixture) {
  GTEST_SKIP() << "NEVERD_KMDF_FIXTURE must name the real WDK-linked fixture";
}
#endif

} // namespace
} // namespace neverd::emulation
