//===- KernelCancellationTests.cpp - WDM/framework cancellation bridge ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise cancellation through the public KMDF 1.33 binding ABI and the
/// production KernelModel, scheduler and Unicorn guest memory. No WDK binary
/// or private framework access is required.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelExportRegistry.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <algorithm>
#include <map>

namespace neverd::emulation {
namespace {

class KernelCancellation : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  static constexpr uint64_t CancelPC = Entry + 0x100;
  static constexpr uint64_t DestroyPC = Entry + 0x200;
  static constexpr uint64_t WorkerPC = Entry + 0x300;
  static constexpr uint64_t Info = Scratch;
  static constexpr uint64_t Component = Scratch + 0x100;
  static constexpr uint64_t TableSlot = Scratch + 0x200;
  static constexpr uint64_t GlobalsSlot = Scratch + 0x208;
  static constexpr uint64_t DriverSlot = Scratch + 0x210;
  static constexpr uint64_t InitSlot = Scratch + 0x218;
  static constexpr uint64_t DeviceSlot = Scratch + 0x220;
  static constexpr uint64_t QueueSlot = Scratch + 0x228;
  static constexpr uint64_t ContextSlot = Scratch + 0x230;
  static constexpr uint64_t BufferSlot = Scratch + 0x238;
  static constexpr uint64_t LengthSlot = Scratch + 0x240;
  static constexpr uint64_t Config = Scratch + 0x300;
  static constexpr uint64_t Attributes = Scratch + 0x400;
  static constexpr uint64_t QueueConfig = Scratch + 0x500;
  static constexpr uint64_t Security = Scratch + 0x600;
  static constexpr uint64_t DeviceName = Scratch + 0x700;
  static constexpr uint64_t Type = Scratch + 0x900;
  static constexpr uint32_t Pending = 0x103;
  static constexpr uint32_t Cancelled = 0xc0000120;
  static constexpr const char *Name = "\\Device\\KernelCancellation";

  std::unique_ptr<UnicornBackend> Memory;
  KernelExportRegistry Exports;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  std::map<std::string, KernelExportRegistry::Export> Functions;
  uint64_t Globals = 0, Device = 0, WdmDevice = 0, Queue = 0;

  static void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }

  template <class T> static T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }

  static void rejected(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    const auto Message = llvm::toString(std::move(E));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }

  template <class T>
  static void rejected(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    const auto Message = llvm::toString(Value.takeError());
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }

  void put(uint64_t Address, uint64_t Value, unsigned Size = 8) {
    success(Memory->writeInteger(Address, Value, Size));
  }

  uint64_t get(uint64_t Address, unsigned Size = 8) {
    return take(Memory->readInteger(Address, Size));
  }

  void unicode(uint64_t Descriptor, llvm::StringRef Text) {
    for (size_t I = 0; I < Text.size(); ++I)
      put(Descriptor + 32 + I * 2, uint8_t(Text[I]), 2);
    put(Descriptor + 32 + Text.size() * 2, 0, 2);
    put(Descriptor, Text.size() * 2, 2);
    put(Descriptor + 2, Text.size() * 2 + 2, 2);
    put(Descriptor + 8, Descriptor + 32);
  }

  llvm::Expected<uint64_t> call(const char *Name,
                                std::initializer_list<uint64_t> Arguments) {
    return Model->call(Functions.at(Name), Arguments, nullptr);
  }

  uint64_t invoke(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(call(Name, Arguments));
  }

  uint64_t kernel(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name, Arguments));
  }

  void SetUp() override {
    auto Backend = UnicornBackend::create(8 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    success(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    success(Model->initialize(Image, Options));

    // Independent public x64 WDF_BIND_INFO v1 and exact 1.33 function table.
    unicode(Component, "KmdfLibrary");
    put(Info, 48, 4);
    put(Info + 8, Component + 32);
    put(Info + 16, 1, 4);
    put(Info + 20, 33, 4);
    put(Info + 28, 458, 4);
    put(Info + 32, TableSlot);
    const auto BindAddress =
        take(Exports.bindImport({0, "WDFLDR.SYS", "WdfVersionBind"}));
    const auto *Bind = Exports.lookup(BindAddress);
    ASSERT_NE(Bind, nullptr);
    EXPECT_EQ(take(Model->call(*Bind,
                               {Model->driverObject(), Model->registryPath(),
                                Info, GlobalsSlot},
                               nullptr)),
              0u);
    Globals = get(GlobalsSlot);
    ASSERT_NE(Globals, 0u);
    const auto Table = get(TableSlot);
    ASSERT_NE(Table, 0u);
    for (unsigned I = 0; I < 458; ++I) {
      const auto *Export = Exports.lookup(get(Table + I * 8));
      ASSERT_NE(Export, nullptr);
      ASSERT_EQ(Export->Binding, Globals);
      Functions.emplace(Export->Name, *Export);
    }
    put(Config, 32, 4);
    put(Config + 16, Entry);
    put(Config + 24, 1, 4); // WdfDriverInitNonPnpDriver.
    EXPECT_EQ(invoke("WdfDriverCreate",
                     {Globals, Model->driverObject(), Model->registryPath(), 0,
                      Config, DriverSlot}),
              0u);
    unicode(Security, "D:P(A;;GA;;;WD)");
    const auto Init = invoke("WdfControlDeviceInitAllocate",
                             {Globals, get(DriverSlot), Security});
    ASSERT_NE(Init, 0u);
    put(InitSlot, Init);
    unicode(DeviceName, Name);
    EXPECT_EQ(invoke("WdfDeviceInitAssignName", {Globals, Init, DeviceName}),
              0u);
    put(Attributes, 56, 4);
    put(Attributes + 24, 2, 4); // WdfExecutionLevelPassive.
    put(Attributes + 28, 4, 4); // WdfSynchronizationScopeNone.
    EXPECT_EQ(
        invoke("WdfDeviceCreate", {Globals, InitSlot, Attributes, DeviceSlot}),
        0u);
    Device = get(DeviceSlot);
    ASSERT_NE(Device, 0u);
    WdmDevice = invoke("WdfDeviceWdmGetDeviceObject", {Globals, Device});
    ASSERT_NE(WdmDevice, 0u);
    put(QueueConfig, 96, 4);
    put(QueueConfig + 4, 1, 4);   // Sequential dispatch.
    put(QueueConfig + 8, 2, 4);   // WdfUseDefault power policy.
    put(QueueConfig + 13, 1, 1);  // DefaultQueue.
    put(QueueConfig + 40, Entry); // EvtIoDeviceControl.
    EXPECT_EQ(invoke("WdfIoQueueCreate",
                     {Globals, Device, QueueConfig, Attributes, QueueSlot}),
              0u);
    Queue = get(QueueSlot);
    ASSERT_NE(Queue, 0u);
    invoke("WdfControlFinishInitializing", {Globals, Device});
    success(Model->finishEntry());
  }

  void file(DriverRequestKind Kind, uint32_t Identity = 1) {
    DriverRequest Request;
    Request.Kind = Kind;
    Request.Device = Name;
    Request.File = Identity;
    const auto Invocation = take(Model->beginRequest(Request));
    EXPECT_EQ(Invocation.PC, 0u); // The default WDF file package completes it.
    ASSERT_FALSE(Result.Requests.empty());
    EXPECT_TRUE(Result.Requests.back().Completed);
    EXPECT_EQ(Result.Requests.back().IOStatus, 0u);
  }

  struct Transfer {
    uint64_t IRP = 0, Request = 0;
  };

  Transfer begin(std::optional<uint64_t> Delay, uint32_t Identity = 1,
                 uint32_t ControlCode = 0x222000) {
    DriverRequest Request;
    Request.Device = Name;
    Request.File = Identity;
    Request.ControlCode = ControlCode;
    Request.Input = {0x12, 0x34};
    Request.OutputSize = 4;
    Request.CancelAfter100ns = Delay;
    const auto Invocation = take(Model->beginRequest(Request));
    EXPECT_EQ(Invocation.PC, Entry);
    EXPECT_EQ(Invocation.Argument0, Queue);
    EXPECT_EQ(Invocation.FrameworkDispatchStatus, Pending);
    EXPECT_NE(Invocation.IRP, 0u);
    return {Invocation.IRP, Invocation.Argument1};
  }

  void pending(const Transfer &Request) {
    success(Model->recordDispatchReturn(Request.IRP, Pending));
    EXPECT_TRUE(Model->requestPending(Request.IRP));
  }

  void mark(const Transfer &Request) {
    EXPECT_EQ(invoke("WdfRequestMarkCancelableEx",
                     {Globals, Request.Request, CancelPC}),
              0u);
  }

  void complete(const Transfer &Request, uint32_t Status = Cancelled) {
    invoke("WdfRequestCompleteWithInformation",
           {Globals, Request.Request, Status, 0});
  }

  DriverRequestResult observation(uint64_t IRP) {
    const auto I = std::find_if(Result.Requests.begin(), Result.Requests.end(),
                                [IRP](const auto &R) { return R.IRP == IRP; });
    if (I == Result.Requests.end()) {
      ADD_FAILURE() << "missing IRP observation";
      return {};
    }
    return *I;
  }

  KernelScheduler::Invocation scheduledCancel(uint64_t Deadline) {
    EXPECT_EQ(Model->nextEventTime(), Deadline);
    // Reach the deadline without a guest timer. A boundary-only advance may
    // require one subsequent selection of the newly queued cancellation.
    auto Call = take(Model->nextScheduled(true));
    if (!Call)
      Call = take(Model->nextScheduled(false));
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->Kind, KernelScheduler::CallbackKind::FrameworkCancel);
    EXPECT_EQ(Call->DueTime100ns, Deadline);
    EXPECT_EQ(Call->Owner, WdmDevice);
    EXPECT_EQ(Call->PC, CancelPC);
    EXPECT_EQ(Call->IRQL, 0u);
    return *Call;
  }

  void finishCancellation(uint64_t ID) {
    EXPECT_FALSE(take(Model->continueScheduled(ID, 0)));
    success(Model->finishScheduled(ID));
  }
};

TEST_F(KernelCancellation, ZeroDelaySetsCancelBeforeMarkWithoutCallback) {
  file(DriverRequestKind::Create);
  const auto Request = begin(0);
  EXPECT_EQ(get(Request.IRP + windows::IRPCancelOffset, 1), 1u);
  EXPECT_EQ(observation(Request.IRP).CancelRequestedAt100ns, 0u);
  EXPECT_EQ(invoke("WdfRequestMarkCancelableEx",
                   {Globals, Request.Request, CancelPC}),
            Cancelled);
  EXPECT_EQ(invoke("WdfRequestIsCanceled", {Globals, Request.Request}), 1u);
  EXPECT_FALSE(Model->takeGuestCall());
  pending(Request);
  EXPECT_FALSE(Model->nextEventTime());
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  complete(Request);
  success(Model->finalizeRequest(Request.IRP));
  EXPECT_EQ(observation(Request.IRP).IOStatus, Cancelled);
}

TEST_F(KernelCancellation, DeadlineWithoutTimerSetsTheAuthoritativeIRPFlag) {
  file(DriverRequestKind::Create);
  const auto Request = begin(37);
  EXPECT_EQ(get(Request.IRP + windows::IRPCancelOffset, 1), 0u);
  EXPECT_FALSE(observation(Request.IRP).CancelRequestedAt100ns);
  mark(Request);
  pending(Request);
  const auto Cancel = scheduledCancel(37);
  EXPECT_EQ(Cancel.Object, Request.IRP);
  EXPECT_EQ(Cancel.Arguments, (std::vector<uint64_t>{Request.Request}));
  success(Model->validateGuestAccess(Request.IRP + windows::IRPCancelOffset, 1,
                                     false));
  EXPECT_EQ(get(Request.IRP + windows::IRPCancelOffset, 1), 1u);
  EXPECT_EQ(observation(Request.IRP).CancelRequestedAt100ns, 37u);
  EXPECT_FALSE(Model->nextEventTime());
  EXPECT_EQ(invoke("WdfRequestUnmarkCancelable", {Globals, Request.Request}),
            Cancelled);
  // nextScheduled performed the real callback-entry notification itself.
  complete(Request);
  EXPECT_TRUE(observation(Request.IRP).Completed);
  rejected(Model->validateGuestAccess(Request.IRP, 1, false), "freed");
  finishCancellation(Cancel.ID);
  success(Model->finalizeRequest(Request.IRP));
  EXPECT_EQ(observation(Request.IRP).DispatchStatus, Pending);
  EXPECT_EQ(observation(Request.IRP).IOStatus, Cancelled);
}

TEST_F(KernelCancellation, SuccessfulCompletionRemovesFutureCancellation) {
  file(DriverRequestKind::Create);
  const auto Request = begin(90);
  mark(Request);
  EXPECT_EQ(invoke("WdfRequestUnmarkCancelable", {Globals, Request.Request}),
            0u);
  pending(Request);
  complete(Request, 0);
  EXPECT_FALSE(Model->nextEventTime());
  EXPECT_FALSE(take(Model->nextScheduled(true, 100)));
  EXPECT_FALSE(observation(Request.IRP).CancelRequestedAt100ns);
  EXPECT_EQ(observation(Request.IRP).IOStatus, 0u);
  success(Model->finalizeRequest(Request.IRP));
}

TEST_F(KernelCancellation, UnmarkWinsButLaterCancellationFactIsStillVisible) {
  file(DriverRequestKind::Create);
  const auto Request = begin(73);
  mark(Request);
  EXPECT_EQ(invoke("WdfRequestUnmarkCancelable", {Globals, Request.Request}),
            0u);
  pending(Request);
  EXPECT_EQ(Model->nextEventTime(), 73u);
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_EQ(get(Request.IRP + windows::IRPCancelOffset, 1), 1u);
  EXPECT_EQ(invoke("WdfRequestIsCanceled", {Globals, Request.Request}), 1u);
  EXPECT_EQ(observation(Request.IRP).CancelRequestedAt100ns, 73u);
  EXPECT_FALSE(Model->nextEventTime());
  complete(Request);
  success(Model->finalizeRequest(Request.IRP));
}

TEST_F(KernelCancellation,
       CancelDestructionContinuationDoesNotConsumeWorkItemReferences) {
  file(DriverRequestKind::Create);
  const auto Request = begin(19);
  put(Type, 40, 4);
  put(Type + 8, Type + 48);
  put(Type + 16, 24);
  success(Memory->write(Type + 48, {'C', 't', 'x', 0}));
  put(Attributes + 16, DestroyPC);
  put(Attributes + 48, Type);
  EXPECT_EQ(invoke("WdfObjectAllocateContext",
                   {Globals, Request.Request, Attributes, ContextSlot}),
            0u);
  const auto Context = get(ContextSlot);
  ASSERT_NE(Context, 0u);
  mark(Request);
  pending(Request);
  const auto Cancel = scheduledCancel(19);
  const auto Item = kernel("IoAllocateWorkItem", {WdmDevice});
  ASSERT_NE(Item, 0u);
  kernel("IoQueueWorkItem",
         {Item, WorkerPC, profile::DelayedWorkQueue, Scratch});
  EXPECT_EQ(get(WdmDevice + windows::DeviceReferenceCount, 4), 1u);
  complete(Request);
  EXPECT_TRUE(observation(Request.IRP).Completed);
  success(Model->validateGuestAccess(Context, 24, false));
  rejected(Model->finishScheduled(Cancel.ID), "continuation");
  auto Destroy = take(Model->continueScheduled(Cancel.ID, 0));
  ASSERT_TRUE(Destroy);
  EXPECT_EQ(Destroy->PC, DestroyPC);
  EXPECT_EQ(Destroy->Arguments, (std::vector<uint64_t>{Request.Request}));
  EXPECT_EQ(get(WdmDevice + windows::DeviceReferenceCount, 4), 1u);
  // A suspended destroy continuation retains the same cancellation task. The
  // independently scheduled work item releases only its own device reference.
  success(Model->suspendScheduled(Cancel.ID));
  auto Worker = take(Model->nextScheduled(false));
  ASSERT_TRUE(Worker);
  EXPECT_EQ(Worker->Kind, KernelScheduler::CallbackKind::WorkItem);
  EXPECT_EQ(Worker->Arguments, (std::vector<uint64_t>{WdmDevice, Scratch}));
  kernel("IoFreeWorkItem", {Item});
  EXPECT_FALSE(take(Model->continueScheduled(Worker->ID, 0)));
  success(Model->finishScheduled(Worker->ID));
  EXPECT_EQ(get(WdmDevice + windows::DeviceReferenceCount, 4), 1u);
  success(Model->resumeScheduled(Cancel.ID));
  success(Model->validateGuestAccess(Context, 24, false));
  finishCancellation(Cancel.ID);
  rejected(Model->validateGuestAccess(Context, 1, false), "freed");
  EXPECT_EQ(get(WdmDevice + windows::DeviceReferenceCount, 4), 1u);
  success(Model->finalizeRequest(Request.IRP));
  file(DriverRequestKind::Cleanup);
  file(DriverRequestKind::Close);
  invoke("WdfObjectDelete", {Globals, Device});
  success(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
}

TEST_F(KernelCancellation, CancellationDoesNotLeakToTheNextFileOrIRP) {
  file(DriverRequestKind::Create, 1);
  file(DriverRequestKind::Create, 2);
  const auto First = begin(11, 1);
  mark(First);
  pending(First);
  const auto Cancel = scheduledCancel(11);
  complete(First);
  finishCancellation(Cancel.ID);
  success(Model->finalizeRequest(First.IRP));

  const auto Second = begin(std::nullopt, 2);
  EXPECT_NE(Second.IRP, First.IRP);
  EXPECT_NE(Second.Request, First.Request);
  EXPECT_EQ(get(Second.IRP + windows::IRPCancelOffset, 1), 0u);
  EXPECT_EQ(invoke("WdfRequestIsCanceled", {Globals, Second.Request}), 0u);
  pending(Second);
  EXPECT_FALSE(take(Model->nextScheduled(true, 100)));
  EXPECT_FALSE(observation(Second.IRP).CancelRequestedAt100ns);
  EXPECT_EQ(observation(First.IRP).CancelRequestedAt100ns, 11u);
  EXPECT_EQ(observation(First.IRP).File, 1u);
  EXPECT_EQ(observation(Second.IRP).File, 2u);
  complete(Second, 0);
  success(Model->finalizeRequest(Second.IRP));
  EXPECT_EQ(observation(First.IRP).IOStatus, Cancelled);
  EXPECT_EQ(observation(Second.IRP).IOStatus, 0u);
}

TEST_F(KernelCancellation,
       CancelHoldKeepsContextButRetiresBothDirectIOCTLMethodsStorage) {
  file(DriverRequestKind::Create);
  for (uint32_t Method : {1u, 2u}) {
    SCOPED_TRACE(Method);
    const auto Request = begin(23, 1, 0x222000 | Method);
    put(Type, 40, 4);
    put(Type + 8, Type + 48);
    put(Type + 16, 24);
    success(Memory->write(Type + 48, {'C', 't', 'x', 0}));
    put(Attributes + 48, Type);
    EXPECT_EQ(invoke("WdfObjectAllocateContext",
                     {Globals, Request.Request, Attributes, ContextSlot}),
              0u);
    const auto Context = get(ContextSlot);
    ASSERT_NE(Context, 0u);
    put(Context, 0x123456789abcdef0);
    EXPECT_EQ(invoke("WdfRequestRetrieveOutputBuffer",
                     {Globals, Request.Request, 4, BufferSlot, LengthSlot}),
              0u);
    const auto Mapping = get(BufferSlot);
    const auto MDL = get(Request.IRP + windows::IRPMdlOffset);
    ASSERT_NE(Mapping, 0u);
    ASSERT_NE(MDL, 0u);
    EXPECT_EQ(get(LengthSlot), 4u);
    success(Model->validateGuestAccess(Mapping, 4, false));
    success(Model->validateGuestAccess(MDL, 1, false));
    mark(Request);
    pending(Request);
    const auto Cancel = scheduledCancel(23 * Method);
    complete(Request);

    rejected(Model->validateGuestAccess(Mapping, 1, false), "freed");
    rejected(Model->validateGuestAccess(MDL, 1, false), "freed");
    rejected(Model->validateGuestAccess(Request.IRP, 1, false), "freed");
    success(Model->validateGuestAccess(Context, 24, false));
    EXPECT_EQ(get(Context), 0x123456789abcdef0u);
    EXPECT_TRUE(observation(Request.IRP).Completed);
    EXPECT_EQ(observation(Request.IRP).IOStatus, Cancelled);
    EXPECT_EQ(observation(Request.IRP).Information, 0u);
    EXPECT_TRUE(observation(Request.IRP).Output.empty());

    finishCancellation(Cancel.ID);
    rejected(Model->validateGuestAccess(Context, 1, false), "freed");
    success(Model->finalizeRequest(Request.IRP));
  }
}

TEST_F(KernelCancellation, WDMRouteChecksFileBeforeAllocatingAnIRP) {
  unicode(DeviceName, "\\Device\\PlainWdmCancellation");
  EXPECT_EQ(kernel("IoCreateDevice", {Model->driverObject(), 0, DeviceName,
                                      0x22, 0, 0, DeviceSlot}),
            0u);
  DriverRequest Request;
  Request.Device = "\\Device\\PlainWdmCancellation";
  Request.ControlCode = 0x222000;
  Request.CancelAfter100ns = 0;
  rejected(Model->beginRequest(Request),
           "request requires a successful CREATE on the same device and file");
  ASSERT_FALSE(Result.Requests.empty());
  EXPECT_EQ(Result.Requests.back().IRP, 0u);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_FALSE(Result.Requests.back().CancelRequestedAt100ns);
  EXPECT_FALSE(Model->requestPending());
  EXPECT_FALSE(Model->nextEventTime());
}

} // namespace
} // namespace neverd::emulation
