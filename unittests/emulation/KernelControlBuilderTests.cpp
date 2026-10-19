// Synchronous I/O-manager packets retain caller buffers through completion.
///
/// \file
/// KernelControlBuilderTests.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/JSON.h"

#include <cstdlib>

namespace neverd::emulation {
namespace {
using namespace windows;
class KernelControlBuilder : public testing::Test {
protected:
  static constexpr uint64_t S = 0x60000000, PC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t Device = 0, Input = 0, Output = 0, IOSB = 0, Event = 0;
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
  void bad(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    const auto Message = llvm::toString(std::move(E));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  template <class T> void bad(llvm::Expected<T> V, llvm::StringRef Text) {
    ASSERT_FALSE(bool(V));
    bad(V.takeError(), Text);
  }
  uint64_t call(const char *API, std::initializer_list<uint64_t> A) {
    return value(Model->call(API, A));
  }
  uint64_t get(uint64_t A, unsigned W = 8) {
    ok(Model->validateGuestAccess(A, W, false));
    return value(Memory->readInteger(A, W));
  }
  void put(uint64_t A, uint64_t V, unsigned W = 8) {
    ok(Model->validateGuestAccess(A, W, true));
    ok(Memory->writeInteger(A, V, W));
  }
  void initialize(std::optional<uint8_t> Hint = 2, bool Version = true) {
    Model.reset();
    Result = {};
    Memory = value(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(S, 4096, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = PC - 4096;
    I.Entry = PC;
    I.Size = 0x3000;
    DriverOptions O;
    if (Version)
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.IRPIOPriorityHint = Hint;
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    call("IoCreateDevice",
         {Model->driverObject(), 0, 0, UnknownDeviceType, 0, 0, S});
    Device = get(S);
    put(Model->driverObject() + DriverDispatchOffset + 14 * 8, PC);
    put(Model->driverObject() + DriverDispatchOffset + 15 * 8, PC);
    ok(Model->finishEntry());
    Input = call("ExAllocatePoolWithTag", {0, 64, 0x636f6e74});
    Output = call("ExAllocatePoolWithTag", {0, 64, 0x636f6e74});
    IOSB = call("ExAllocatePoolWithTag", {0, 16, 0x636f6e74});
    Event = call("ExAllocatePoolWithTag", {0, 24, 0x636f6e74});
    call("KeInitializeEvent", {Event, dispatcher::NotificationObject, 0});
    put(Input, 0x0807060504030201);
    put(Output, 0xaaaaaaaaaaaaaaaa);
    put(IOSB, 0x7777777777777777);
    put(IOSB + 8, 0x7777777777777777);
  }
  void SetUp() override { initialize(); }
  llvm::Expected<uint64_t> build(unsigned Method = 0, bool Internal = false,
                                 unsigned N = 4, unsigned M = 8,
                                 bool WithEvent = true) {
    return Model->call("IoBuildDeviceIoControlRequest",
                       {0x222000u | Method, Device, N ? Input : 0, N,
                        M ? Output : 0, M, uint64_t(Internal),
                        WithEvent ? Event : 0, IOSB});
  }
  uint64_t stack(uint64_t IRP) {
    return get(IRP + IRPStackPointerOffset) - StackSize;
  }
  KernelGuestCall callback(uint64_t Expected = PC) {
    auto C = Model->takeGuestCall();
    EXPECT_TRUE(C);
    if (!C)
      return {};
    EXPECT_EQ(C->PC, Expected);
    Model->enterExecution(profile::CallbackStackBase, profile::StackBase,
                          C->Token);
    return *C;
  }
  KernelGuestCall submit(uint64_t IRP) {
    EXPECT_EQ(call("IoCallDriver", {Device, IRP}), 0u);
    return callback();
  }
  void finish(KernelGuestCall C, uint32_t Status = StatusSuccess,
              std::optional<uint64_t> Expected = std::nullopt) {
    Model->enterExecution(profile::CallbackStackBase, profile::StackBase,
                          C.Token);
    auto R = value(Model->finishGuestCall(C.Token, Status));
    ASSERT_TRUE(R);
    if (Expected)
      EXPECT_EQ(*R, *Expected);
  }
  void complete(uint64_t IRP, uint64_t N = 0, uint32_t Status = 0) {
    put(IRP + IRPStatusOffset, Status, 4);
    put(IRP + IRPInformationOffset, N);
    call("IofCompleteRequest", {IRP, 0});
  }
};

TEST_F(KernelControlBuilder,
       OriginalConstructionMatrixMatchesAllMethodsAndMajors) {
  struct Case {
    unsigned Method, Internal, Hint, N, M, Flags, BufferSize;
  };
  const Case Cases[] = {
#define NEVERD_CONTROL_NATIVE_CASE(M, I, H, N, O, F, B) {M, I, H, N, O, F, B},
#include "ControlBuilderNativeCases.def"
#undef NEVERD_CONTROL_NATIVE_CASE
  };
  ASSERT_EQ(std::size(Cases), 120u);
  for (const auto &C : Cases) {
    SCOPED_TRACE(std::to_string(C.Method) + ":" + std::to_string(C.Internal) +
                 ":" + std::to_string(C.Hint) + ":" + std::to_string(C.N) +
                 ":" + std::to_string(C.M));
    initialize(uint8_t(C.Hint));
    put(Device + DeviceStackCountOffset, 2, 1);
    auto IRP = value(build(C.Method, C.Internal, C.N, C.M));
    ASSERT_NE(IRP, 0u);
    EXPECT_EQ(get(IRP + IRPFlagsOffset, 4), C.Flags);
    EXPECT_EQ(get(IRP + IRPRequestorModeOffset, 1), KernelMode);
    EXPECT_EQ(get(IRP + IRPStackCountOffset, 1), 2u);
    EXPECT_EQ(get(IRP + IRPLocationOffset, 1), 3u);
    EXPECT_EQ(get(IRP + IRPUserIOSBOffset), IOSB);
    EXPECT_EQ(get(IRP + IRPUserEventOffset), Event);
    EXPECT_NE(get(IRP + IRPThreadOffset), 0u);
    const auto Stack = stack(IRP), System = get(IRP + IRPSystemBufferOffset);
    EXPECT_EQ(get(Stack, 1), C.Internal ? 15u : 14u);
    EXPECT_EQ(get(Stack + StackInputLengthOffset, 4), C.N);
    EXPECT_EQ(get(Stack + StackParametersOffset, 4), C.M);
    EXPECT_EQ(get(Stack + StackIOControlOffset, 4), 0x222000u | C.Method);
    EXPECT_EQ(get(Stack + StackType3InputOffset),
              C.Method == 3 && C.N ? Input : 0u);
    EXPECT_EQ(get(IRP + IRPUserBufferOffset),
              C.M && (C.Method == 0 || C.Method == 3) ? Output : 0u);
    EXPECT_EQ(bool(System), bool(C.BufferSize));
    if (System && C.N)
      EXPECT_EQ(get(System, C.N), get(Input, C.N));
    if (System && C.BufferSize > C.N)
      bad(Model->validateGuestAccess(System + C.N, C.BufferSize - C.N, false),
          "uninitialized system buffer");
    EXPECT_EQ(bool(get(IRP + IRPMdlOffset)),
              C.M && (C.Method == 1 || C.Method == 2));
    EXPECT_EQ(get(IOSB), 0x7777777777777777u);
    EXPECT_EQ(call("KeReadStateEvent", {Event}), 0u);
    bad(Model->call("IoFreeIrp", {IRP}), "I/O-manager-owned");
  }
}
TEST_F(KernelControlBuilder,
       CompletionCopiesOutputSignalsEventAndPreservesCallerStorage) {
  for (unsigned Method : {0u, 1u, 2u, 3u})
    for (bool Internal : {false, true}) {
      initialize();
      auto IRP = value(build(Method, Internal));
      ASSERT_NE(IRP, 0u);
      auto Dispatch = submit(IRP);
      uint64_t Destination =
          Method == 0 ? get(IRP + IRPSystemBufferOffset) : Output;
      const uint64_t MDL = get(IRP + IRPMdlOffset);
      if (MDL) {
        auto Alias = call("MmMapLockedPagesSpecifyCache",
                          {MDL, KernelMode, 1, 0, 0, 16});
        ASSERT_NE(Alias, 0u);
        Destination = Alias;
      }
      const uint64_t Payload =
          Method == 1 ? 0xaaaaaaaaaaaaaaaa : 0x8877665544332211;
      if (Method == 1)
        bad(Model->validateGuestAccess(Destination, 8, true), "read-only");
      else
        put(Destination, Payload);
      complete(IRP, 8);
      EXPECT_EQ(get(IOSB, 4), 0u);
      EXPECT_EQ(get(IOSB + 8), 8u);
      EXPECT_EQ(call("KeReadStateEvent", {Event}), 1u);
      EXPECT_EQ(get(Output), Payload);
      bad(Model->validateGuestAccess(IRP, 1, false), "freed");
      if (MDL)
        bad(Model->validateGuestAccess(MDL, 1, false), "freed");
      finish(Dispatch);
      ASSERT_EQ(Result.Requests.size(), 1u);
      EXPECT_TRUE(Result.Requests[0].Completed);
      EXPECT_EQ(Result.Requests[0].Origin,
                DriverRequestOrigin::IoControlBuilder);
      ASSERT_EQ(Result.Requests[0].Output.size(), 8u);
      put(Output, 0x1122);
      for (auto P : {Input, Output, IOSB, Event})
        call("ExFreePoolWithTag", {P, 0x636f6e74});
      call("IoDeleteDevice", {Device});
      ok(Model->snapshot());
      EXPECT_TRUE(Result.Devices.empty());
    }
}
TEST_F(KernelControlBuilder,
       PendingDispatchCompletesLaterAndDisposesWithoutAnActiveDispatch) {
  auto IRP = value(build());
  auto D = submit(IRP);
  call("IoMarkIrpPending", {IRP});
  finish(D, StatusPending);
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 0u);
  Model->enterExecution(profile::StackBase);
  put(get(IRP + IRPSystemBufferOffset), 0x44332211, 4);
  complete(IRP, 4);
  EXPECT_EQ(get(Output, 4), 0x44332211u);
  EXPECT_EQ(get(IOSB + 8), 4u);
  bad(Model->validateGuestAccess(IRP, 1, false), "freed");
  call("IoDeleteDevice", {Device});
  ok(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
}
TEST_F(KernelControlBuilder, ExplicitCancellationRetainsManagerDisposal) {
  const auto IRP = value(build(2));
  const auto MDL = get(IRP + IRPMdlOffset);
  const auto D = submit(IRP);
  call("IoAcquireCancelSpinLock", {S});
  put(IRP + IRPCancelRoutineOffset, PC + 0x200);
  call("IoMarkIrpPending", {IRP});
  call("IoReleaseCancelSpinLock", {get(S, 1)});
  finish(D, StatusPending);
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(call("IoCancelIrp", {IRP}), 0u);
  const auto C = callback(PC + 0x200);
  EXPECT_EQ(C.Arguments, (std::vector<uint64_t>{Device, IRP}));
  call("IoReleaseCancelSpinLock", {get(IRP + IRPCancelIROffset, 1)});
  complete(IRP, 0, StatusCancelled);
  finish(C, StatusSuccess, 1);
  EXPECT_EQ(get(IOSB, 4), StatusCancelled);
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 1u);
  bad(Model->validateGuestAccess(MDL, 1, false), "freed");
  put(Output, 0x1122);
  call("IoDeleteDevice", {Device});
  ok(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
}
TEST_F(KernelControlBuilder,
       MoreProcessingRequiredHoldsNotificationUntilResume) {
  auto IRP = value(build(0, false, 4, 8, false));
  const auto Stack = stack(IRP);
  put(Stack + StackCompletionOffset, PC + 0x100);
  put(Stack + StackControlOffset,
      StackInvokeOnSuccess | StackInvokeOnError | StackInvokeOnCancel, 1);
  auto D = submit(IRP);
  complete(IRP, 4);
  auto C = callback(PC + 0x100);
  EXPECT_EQ(get(IOSB), 0x7777777777777777u);
  finish(C, StatusMoreProcessingRequired);
  finish(D);
  EXPECT_FALSE(Result.Requests[0].Completed);
  Model->enterExecution(profile::StackBase);
  complete(IRP, 4);
  EXPECT_EQ(get(IOSB + 8), 4u);
  EXPECT_TRUE(Result.Requests[0].Completed);
  bad(Model->validateGuestAccess(IRP, 1, false), "freed");
  call("IoDeleteDevice", {Device});
  ok(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
}
TEST_F(KernelControlBuilder,
       InvalidCompletionPreservesCursorAndDoesNotPublishUndefinedBytes) {
  auto IRP = value(build());
  auto D = submit(IRP);
  put(IRP + IRPInformationOffset, 8);
  const auto Before = get(IRP + IRPLocationOffset, 1);
  bad(Model->call("IofCompleteRequest", {IRP, 0}),
      "uninitialized system buffer");
  EXPECT_EQ(get(IRP + IRPLocationOffset, 1), Before);
  EXPECT_EQ(get(IOSB), 0x7777777777777777u);
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 0u);
  put(IRP + IRPInformationOffset, 9);
  bad(Model->call("IofCompleteRequest", {IRP, 0}), "output capacity");
  complete(IRP, 0, StatusNotSupported);
  finish(D, StatusNotSupported);
  EXPECT_EQ(get(IOSB, 4), StatusNotSupported);
}
TEST_F(KernelControlBuilder,
       RedirectedHeadersAndMissingCompletionRejectBeforeAdoption) {
  auto IRP = value(build(0, false, 4, 8, false));
  const auto Stack = stack(IRP);
  bad(Model->call("IoCallDriver", {Device, IRP}), "completion routine");
  EXPECT_TRUE(Result.Requests.empty());
  put(Stack + StackCompletionOffset, PC + 0x100);
  put(Stack + StackControlOffset,
      StackInvokeOnSuccess | StackInvokeOnError | StackInvokeOnCancel, 1);
  for (auto Field : {IRP + IRPStackPointerOffset, IRP + IRPUserIOSBOffset,
                     Stack + StackIOControlOffset}) {
    const auto Old = get(Field);
    put(Field, Old + 1);
    bad(Model->call("IoCallDriver", {Device, IRP}), "constructed packet");
    EXPECT_TRUE(Result.Requests.empty());
    put(Field, Old);
  }
  auto D = submit(IRP);
  complete(IRP, 4);
  auto C = callback(PC + 0x100);
  finish(C);
  finish(D);
  EXPECT_TRUE(Result.Requests[0].Completed);
}
TEST_F(KernelControlBuilder,
       CallerStorageCannotBeFreedUntilTerminalCompletion) {
  auto IRP = value(build(3));
  auto D = submit(IRP);
  for (auto P : {Input, Output, IOSB, Event})
    bad(Model->call("ExFreePoolWithTag", {P, 0x636f6e74}),
        "live request caller storage");
  complete(IRP);
  finish(D);
  for (auto P : {Input, Output, IOSB, Event})
    call("ExFreePoolWithTag", {P, 0x636f6e74});
}
TEST_F(KernelControlBuilder,
       MissingFactsAndBadBuffersFailBeforePacketCreation) {
  initialize(std::nullopt);
  bad(build(), "irp_io_priority_hint");
  initialize(2, false);
  bad(build(), "windows_version");
  initialize();
  bad(Model->call("IoBuildDeviceIoControlRequest",
                  {0x222000, Device, Input, 4, Output, 8, 0, Event, 0}),
      "kernel storage");
  bad(Model->call("IoBuildDeviceIoControlRequest",
                  {0x222000, Device, Input, 4, Output, 8, 0, Event, Output}),
      "overlapping");
  call("KfRaiseIrql", {2});
  bad(build(), "IRQL");
  call("KeLowerIrql", {0});
  EXPECT_TRUE(Result.Requests.empty());
}
TEST(ControlBuilderScenario, ValidatesAndReportsEffectivePriorityObservation) {
  for (unsigned Hint : {0u, 2u, 4u}) {
    auto O = driverOptionsFromScenarioJSON(
        "{\"irp_io_priority_hint\":" + std::to_string(Hint) + "}");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    EXPECT_EQ(O->IRPIOPriorityHint, Hint);
    DriverResult R;
    R.Configuration = *O;
    auto J = llvm::json::parse(driverResultJSON(R));
    ASSERT_TRUE(bool(J));
    EXPECT_EQ(J->getAsObject()
                  ->getObject("configuration")
                  ->getInteger("irp_io_priority_hint"),
              Hint);
  }
  for (const char *Bad : {"-1", "5", "true", "null", "1.5", "\"2\""}) {
    auto O = driverOptionsFromScenarioJSON(
        std::string("{\"irp_io_priority_hint\":") + Bad + "}");
    ASSERT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
}
TEST(ControlBuilderDriver, CompiledPacketLifecycleWorksAcrossExecutionModes) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Paging : {false, true}) {
        SCOPED_TRACE(int(Contract));
        SCOPED_TRACE(Preemptive);
        SCOPED_TRACE(Paging);
        DriverOptions O;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.IRPIOPriorityHint = 2;
        O.Contract = Contract;
        O.PageTables = Paging;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_control_builder.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        EXPECT_TRUE(R->Devices.empty());
        EXPECT_EQ(R->Messages,
                  (std::vector<std::string>{"IOCTL builder 24 requests passed",
                                            "IOCTL builder unload passed"}));
        ASSERT_EQ(R->Requests.size(), 24u);
        for (const auto &Request : R->Requests) {
          EXPECT_EQ(Request.Origin, DriverRequestOrigin::IoControlBuilder);
          EXPECT_TRUE(Request.Completed);
          EXPECT_EQ(Request.IOStatus, 0u);
          EXPECT_EQ(Request.Information, 8u);
          EXPECT_EQ(Request.Output.size(), 8u);
        }
      }
}
TEST(ControlBuilderDriver, MatchedReferenceCallsTheRealNineArgumentExport) {
  const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
             *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.IRPIOPriorityHint = 2;
      O.Contract = Contract;
      O.Unload = true;
      O.KernelImage = Image;
      O.KernelPDB = PDB;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_control_builder.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted);
      ASSERT_EQ(R->Requests.size(), 24u);
      unsigned Found = 0;
      for (const auto &Call : R->Calls)
        if (Call.Name == "IoBuildDeviceIoControlRequest") {
          EXPECT_GE(Call.PC, 0xfffff80000000000ULL);
          ++Found;
        }
      EXPECT_EQ(Found, 24u);
    }
}
} // namespace
} // namespace neverd::emulation
