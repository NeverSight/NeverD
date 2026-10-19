///
/// \file
/// KernelServiceTests.
///

#include "ServiceImageTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/driver/DriverServicesJSON.h"
#include "os/windows/kernel/KernelModel.h"

#include <cstdlib>
namespace neverd::emulation {
namespace {
class KernelServiceBoundary : public testing::Test {
protected:
  static constexpr uint64_t S = 0x22000000, PC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  KernelExportRegistry Exports;
  DriverResult Result;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T value(llvm::Expected<T> V) {
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return {};
    }
    return std::move(*V);
  }
  void initialize(DriverOptions O) {
    Model.reset();
    Result = {};
    Exports = KernelExportRegistry();
    Memory = value(UnicornBackend::create(32 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(S, 65536, Read | Write));
    ok(Memory->map(PC, 4096, Read | Execute));
    ok(Exports.initialize(O));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage I;
    I.Base = PC - 4096;
    I.Entry = PC;
    I.Size = 0x3000;
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    string(S, "\\Registry\\Machine\\System\\CurrentControlSet\\Services\\Servic"
              "eChild");
  }
  void string(uint64_t A, const std::string &Text) {
    ok(Memory->writeInteger(A, Text.size() * 2, 2));
    ok(Memory->writeInteger(A + 2, (Text.size() + 1) * 2, 2));
    ok(Memory->writeInteger(A + 8, A + 32, 8));
    for (size_t I = 0; I < Text.size(); ++I)
      ok(Memory->writeInteger(A + 32 + I * 2, uint8_t(Text[I]), 2));
  }
  void bad(llvm::Expected<uint64_t> V, llvm::StringRef Text) {
    ASSERT_FALSE(bool(V));
    const auto E = llvm::toString(V.takeError());
    EXPECT_NE(E.find(Text.str()), std::string::npos) << E;
  }
};
TEST_F(KernelServiceBoundary,
       MissingRegistryAndResourcesRemainUnknownAndKnownAbsenceIsANativeStatus) {
  auto O = serviceOptions();
  O.Registry.reset();
  O.ServiceImages.reset();
  initialize(O);
  bad(Model->call("ZwLoadDriver", {S}), "registry provider");
  O = serviceOptions();
  O.Registry = std::vector<DriverRegistryKey>{};
  O.ServiceImages = std::vector<DriverServiceImage>{};
  initialize(O);
  EXPECT_EQ(value(Model->call("ZwLoadDriver", {S})), 0xc0000034u);
  EXPECT_EQ(value(Model->call("ZwUnloadDriver", {S})), 0xc0000034u);
  EXPECT_EQ(value(Model->call("ZwLoadDriver", {0})), 0xc000000du);
  EXPECT_FALSE(Model->takeGuestCall());
  initialize(serviceOptions());
  bad(Model->call("ZwLoadDriver", {}), "argument count");
  value(Model->call("KfRaiseIrql", {2}));
  bad(Model->call("ZwLoadDriver", {S}), "PASSIVE_LEVEL");
}
TEST_F(KernelServiceBoundary, RegistryTypeAndImagePathAreCheckedBeforeMapping) {
  auto O = serviceOptions();
  (*O.Registry)[0].Values[0].Data[0] = 2;
  initialize(O);
  bad(Model->call("ZwLoadDriver", {S}), "SERVICE_KERNEL_DRIVER");
  EXPECT_FALSE(value(Memory->canAccess(0x1a0001000, 1, Execute)));
  O = serviceOptions();
  (*O.Registry)[0].Values[1].Data[2] = 'X';
  initialize(O);
  bad(Model->call("ZwLoadDriver", {S}), "matching explicit immutable");
  EXPECT_FALSE(value(Memory->canAccess(0x1a0001000, 1, Execute)));
  O = serviceOptions();
  O.ServiceImages.reset();
  initialize(O);
  bad(Model->call("ZwLoadDriver", {S}), "explicit immutable");
  O = serviceOptions();
  (*O.Registry)[0].Values.push_back({"ObjectName", 1, {0, 0}});
  initialize(O);
  bad(Model->call("ZwLoadDriver", {S}), "ObjectName overrides");
  bad(Model->call("ZwUnloadDriver", {S}), "ObjectName overrides");
  EXPECT_FALSE(value(Memory->canAccess(0x1a0001000, 1, Execute)));
}
TEST_F(KernelServiceBoundary,
       LazyMappingQueuesAChildObjectAndPreservesItsOwnedMetadata) {
  initialize(serviceOptions());
  EXPECT_FALSE(value(Memory->canAccess(0x1a0001000, 1, Execute)));
  EXPECT_EQ(value(Model->call("ZwLoadDriver", {S})), 0u);
  auto C = Model->takeGuestCall();
  ASSERT_TRUE(C);
  ASSERT_EQ(C->Arguments.size(), 2u);
  EXPECT_EQ(C->Token.Owner, GuestCallOwner::DriverService);
  EXPECT_NE(C->Arguments[0], Model->driverObject());
  EXPECT_EQ(value(Memory->readInteger(C->Arguments[0] + 0x18, 8)),
            0x1a0000000u);
  auto E = Model->validateGuestAccess(C->Arguments[0] + 0x18, 8, true);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  ok(Model->beginGuestCall(C->Token));
  bad(Model->call("PsCreateSystemThread", {S + 0x1000, 0, 0, 0, 0, PC, 0}),
      "child legacy callback");
  auto Finished = Model->finishGuestCall(C->Token, 0);
  ASSERT_TRUE(bool(Finished)) << llvm::toString(Finished.takeError());
  ASSERT_TRUE(*Finished);
  EXPECT_EQ(**Finished, 0u);
  auto Borrowed = Model->validateGuestAccess(C->Arguments[1], 2, false);
  ASSERT_TRUE(bool(Borrowed));
  llvm::consumeError(std::move(Borrowed));
  EXPECT_EQ(value(Model->call("ZwLoadDriver", {S})), 0xc000010eu);
  EXPECT_EQ(value(Model->call("ZwUnloadDriver", {S})), 0xc0000010u);
}
void runService(DriverOptions O, llvm::StringRef Fixture, bool Thread) {
  auto R = emulateDriver(serviceFixture(Fixture), O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
  EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
  EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
  EXPECT_TRUE(R->Devices.empty());
  unsigned Entries = 0, Unloads = 0, Failures = 0, Callbacks = 0,
           ThreadUnloads = 0;
  for (const auto &M : R->Messages) {
    Entries += M == "child service entry executed";
    Unloads += M == "child service unload executed";
    Failures += M == "child failure entry executed";
  }
  EXPECT_EQ(Entries, Thread ? 1u : 2u);
  EXPECT_EQ(Unloads, Entries);
  EXPECT_EQ(Failures, Thread ? 0u : 1u);
  if (Thread) {
    EXPECT_EQ(std::count(R->Messages.begin(), R->Messages.end(),
                         "service thread create notification"),
              1);
    EXPECT_EQ(std::count(R->Messages.begin(), R->Messages.end(),
                         "service thread exit notification"),
              1);
  }
  for (const auto &C : R->Calls) {
    Callbacks +=
        llvm::StringRef(C.Phase).starts_with("callback:driver-service:");
    ThreadUnloads += Thread && C.Name == "ZwUnloadDriver" &&
                     llvm::StringRef(C.Phase).starts_with("callback:");
  }
  EXPECT_GT(Callbacks, 0u);
  if (Thread)
    EXPECT_EQ(ThreadUnloads, 1u);
}
TEST(DriverService,
     ActualChildEntryUnloadFailedEntryAndReloadShareKernelState) {
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preempt : {false, true})
      for (bool Paging : {false, true}) {
        auto O = serviceOptions();
        O.Contract = C;
        O.PageTables = Paging;
        if (Preempt)
          O.Scheduling = DriverScheduling{23, 1};
        runService(O, "driver_service_lifecycle", false);
      }
}
TEST(DriverService, NativeServiceExportCanBeARealSystemThreadEntry) {
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preempt : {false, true}) {
      auto O = serviceOptions();
      O.Contract = C;
      if (Preempt)
        O.Scheduling = DriverScheduling{23, 1};
      runService(O, "driver_service_thread", true);
    }
}
TEST(DriverService, PinnedKernelAddressesRetainTheSameServiceAndThreadOwner) {
  auto *I = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
       *P = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!I || !P)
    GTEST_SKIP() << "requires owned pinned PE/PDB";
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Thread : {false, true}) {
      auto O = serviceOptions();
      O.Contract = C;
      O.KernelImage = I;
      O.KernelPDB = P;
      O.Scheduling = DriverScheduling{23, 1};
      runService(O,
                 Thread ? "driver_service_thread" : "driver_service_lifecycle",
                 Thread);
    }
}
TEST(DriverService,
     MissingUnloadRoutineKeepsTheChildLoadedAndIsNeverReportedSuccessful) {
  auto O = serviceOptions();
  O.ServiceImages->front().Source = serviceFixture("driver_service_nounload");
  O.Unload = false;
  auto R = emulateDriver(serviceFixture("driver_service_unload_denied"), O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
  EXPECT_EQ(R->NTStatus, 0u);
  EXPECT_EQ(R->Devices.size(), 1u);
  unsigned Denied = 0;
  for (const auto &C : R->Calls)
    Denied += C.Name == "ZwUnloadDriver" && C.Result == 0xc0000010;
  EXPECT_EQ(Denied, 1u);
  O.Unload = true;
  auto U = emulateDriver(serviceFixture("driver_service_unload_denied"), O);
  ASSERT_TRUE(bool(U)) << llvm::toString(U.takeError());
  EXPECT_EQ(U->Stop, DriverStopReason::ModelError);
  EXPECT_NE(U->Diagnostic.find("loaded child service"), std::string::npos);
  EXPECT_FALSE(U->UnloadCompleted);
}
TEST(DriverService,
     ChildDeviceRequestsUseTheirOwnDispatchTableAndUnloadFromTheParent) {
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Paging : {false, true}) {
      auto O = serviceOptions();
      O.Contract = C;
      O.PageTables = Paging;
      O.Scheduling = DriverScheduling{23, 1};
      O.Requests.push_back({DriverRequestKind::Create});
      DriverRequest Q;
      Q.Kind = DriverRequestKind::DeviceControl;
      Q.ControlCode = 0x222000;
      Q.OutputSize = 4;
      O.Requests.push_back(Q);
      O.Requests.push_back({DriverRequestKind::Cleanup});
      O.Requests.push_back({DriverRequestKind::Close});
      auto R = emulateDriver(serviceFixture("driver_service_requests"), O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      ASSERT_EQ(R->Requests.size(), 4u);
      for (const auto &Req : R->Requests) {
        EXPECT_TRUE(Req.Completed);
        EXPECT_EQ(Req.IOStatus, 0u);
      }
      EXPECT_EQ(R->Requests[1].Output,
                (std::vector<uint8_t>{0x41, 0x90, 1, 0}));
      EXPECT_EQ(std::count(R->Messages.begin(), R->Messages.end(),
                           "child service dispatch executed"),
                1);
      EXPECT_TRUE(R->Devices.empty());
    }
}
TEST(DriverService, ExplicitSourcesAndGuestPathsRoundTripAndRejectAmbiguity) {
  auto O = serviceOptions();
  EXPECT_FALSE(bool(validateDriverServices(O)));
  auto V = driverServiceImagesJSON(*O.ServiceImages);
  auto P = parseDriverServiceImages(V);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  ASSERT_EQ(P->size(), 2u);
  EXPECT_EQ((*P)[0].Source, (*O.ServiceImages)[0].Source);
  EXPECT_EQ((*P)[0].LoadAddress, 0x1a0000000ULL);
  O.ServiceImages->push_back(O.ServiceImages->front());
  auto E = validateDriverServices(O);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  auto Bad = parseDriverServiceImages(llvm::json::Object{{"host", "unknown"}});
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
}
} // namespace
} // namespace neverd::emulation
