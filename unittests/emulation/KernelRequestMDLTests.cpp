//===- KernelRequestMDLTests.cpp - WDM/framework request storage ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise request access through the public KMDF 1.33 binding ABI and the
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

class KernelRequestMDL : public ::testing::Test {
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
  static constexpr const char *Name = "\\Device\\KernelRequestMDL";

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

  virtual uint32_t ioType() const { return framework::ControlIoBuffered; }
  virtual uint64_t callerContextPC() const { return 0; }

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
    invoke("WdfDeviceInitSetIoType", {Globals, Init, ioType()});
    if (callerContextPC())
      invoke("WdfDeviceInitSetIoInCallerContextCallback",
             {Globals, Init, callerContextPC()});
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
    put(QueueConfig + 12, 1, 1);  // Allow zero-length callbacks.
    put(QueueConfig + 24, Entry); // EvtIoRead.
    put(QueueConfig + 32, Entry); // EvtIoWrite.
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

  struct Transfer {
    uint64_t IRP = 0, Request = 0;
  };

  void open(bool Asynchronous = false) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Create;
    Request.Device = Name;
    Request.File = 1;
    if (Asynchronous)
      Request.AsynchronousFile = true;
    const auto Invocation = take(Model->beginRequest(Request));
    EXPECT_EQ(Invocation.PC, 0u);
    ASSERT_FALSE(Result.Requests.empty());
    EXPECT_TRUE(Result.Requests.back().Completed);
  }

  Transfer begin(DriverRequestKind Kind = DriverRequestKind::DeviceControl,
                 uint32_t ControlCode = 0x222000, uint32_t Input = 3,
                 uint32_t Output = 7,
                 std::optional<uint64_t> Delay = std::nullopt) {
    DriverRequest Request;
    Request.Kind = Kind;
    Request.Device = Name;
    Request.File = 1;
    Request.ControlCode =
        Kind == DriverRequestKind::DeviceControl ? ControlCode : 0;
    if (Kind != DriverRequestKind::Read)
      Request.Input.assign(Input, 0x5a);
    if (Kind != DriverRequestKind::Write)
      Request.OutputSize = Output;
    Request.CancelAfter100ns = Delay;
    const auto Invocation = take(Model->beginRequest(Request));
    EXPECT_EQ(Invocation.PC, Entry);
    EXPECT_EQ(Invocation.Argument0, Queue);
    EXPECT_EQ(Invocation.FrameworkDispatchStatus, Pending);
    EXPECT_NE(Invocation.IRP, 0u);
    success(Model->recordDispatchReturn(Invocation.IRP, Pending));
    return {Invocation.IRP, Invocation.Argument1};
  }

  uint64_t mdl(const Transfer &Request, bool Output) {
    put(BufferSlot, UINT64_MAX);
    EXPECT_EQ(invoke(Output ? "WdfRequestRetrieveOutputWdmMdl"
                            : "WdfRequestRetrieveInputWdmMdl",
                     {Globals, Request.Request, BufferSlot}),
              0u);
    return get(BufferSlot);
  }

  uint64_t mapping(uint64_t MDL, uint32_t Flags = 0x10) {
    return kernel("MmGetSystemAddressForMdlSafe", {MDL, Flags});
  }

  void complete(const Transfer &Request, uint64_t Information = 0,
                uint32_t Status = 0) {
    invoke("WdfRequestCompleteWithInformation",
           {Globals, Request.Request, Status, Information});
  }

  void finalize(const Transfer &Request) {
    success(Model->finalizeRequest(Request.IRP));
  }

  void context(const Transfer &Request, uint64_t Cleanup = 0,
               uint64_t Destroy = 0) {
    put(Type, 40, 4);
    put(Type + 8, Type + 48);
    put(Type + 16, 24);
    success(Memory->write(Type + 48, {'C', 't', 'x', 0}));
    put(Attributes + 8, Cleanup);
    put(Attributes + 16, Destroy);
    put(Attributes + 48, Type);
    EXPECT_EQ(invoke("WdfObjectAllocateContext",
                     {Globals, Request.Request, Attributes, ContextSlot}),
              0u);
  }
};

class KernelDirectRequestMDL : public KernelRequestMDL {
protected:
  uint32_t ioType() const override { return framework::ControlIoDirect; }
};

class KernelNeitherRequestMDL : public KernelRequestMDL {
protected:
  static constexpr uint64_t CallerContextPC = Entry + 0x400;
  uint32_t ioType() const override { return framework::ControlIoNeither; }
  uint64_t callerContextPC() const override { return CallerContextPC; }

  DriverRequest nestedRequest() {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::DeviceControl;
    Request.Device = Name;
    Request.File = 1;
    Request.ControlCode = 0x222000 | windows::MethodNeither;
    Request.Input.resize(profile::PointerSize);
    Request.UserBuffers = {
        {"descriptor", 16, {}, DriverUserPageAccess::ReadOnly},
        {"payload", 12, {0x31}},
        {"sealed", 8, {}, DriverUserPageAccess::NoAccess}};
    using Kind = DriverUserBufferKind;
    Request.UserPointers = {
        {{Kind::Input, {}, 0}, {Kind::Memory, "descriptor", 0}},
        {{Kind::Memory, "descriptor", 0}, {Kind::Memory, "payload", 4}}};
    return Request;
  }

  Transfer beginCaller(const DriverRequest &Input) {
    auto Invocation = Model->beginRequest(Input);
    if (!Invocation) {
      ADD_FAILURE() << llvm::toString(Invocation.takeError());
      return {};
    }
    EXPECT_TRUE(Invocation->FrameworkCallerContext);
    EXPECT_EQ(Invocation->PC, CallerContextPC);
    EXPECT_EQ(Invocation->Argument0, Device);
    Model->enterExecution(profile::StackBase);
    success(Model->setUserRequestContext(true, Input.RequestorProcessID));
    return {Invocation->IRP, Invocation->Argument1};
  }

  void enqueue(const Transfer &Request) {
    EXPECT_EQ(
        invoke("WdfDeviceEnqueueRequest", {Globals, Device, Request.Request}),
        windows::StatusSuccess);
    const auto Dispatch =
        take(Model->continueFrameworkCallerContext(Request.IRP));
    EXPECT_EQ(Dispatch.PC, Entry);
    EXPECT_EQ(Dispatch.Argument0, Queue);
    EXPECT_EQ(Dispatch.Argument1, Request.Request);
    success(Model->recordDispatchReturn(Request.IRP, Pending));
  }

  uint64_t lock(const Transfer &Request, uint64_t Buffer, uint64_t Size,
                bool ForWrite = false) {
    EXPECT_EQ(invoke(ForWrite ? "WdfRequestProbeAndLockUserBufferForWrite"
                              : "WdfRequestProbeAndLockUserBufferForRead",
                     {Globals, Request.Request, Buffer, Size, BufferSlot}),
              windows::StatusSuccess);
    return get(BufferSlot);
  }

  uint64_t memoryBuffer(uint64_t Handle) {
    return invoke("WdfMemoryGetBuffer", {Globals, Handle, LengthSlot});
  }
};

TEST_F(KernelNeitherRequestMDL,
       CallerContextLocksNestedPointersAndRetainsAliasesAfterUnmap) {
  open();
  const auto Request = beginCaller(nestedRequest());
  ASSERT_NE(Request.IRP, 0u);
  EXPECT_EQ(invoke("WdfRequestRetrieveUnsafeUserInputBuffer",
                   {Globals, Request.Request, profile::PointerSize, BufferSlot,
                    LengthSlot}),
            windows::StatusSuccess);
  const auto Root = get(BufferSlot);
  const auto RootMemory = lock(Request, Root, profile::PointerSize);
  const auto Descriptor = get(memoryBuffer(RootMemory));
  const auto DescriptorMemory = lock(Request, Descriptor, profile::PointerSize);
  const auto Payload = get(memoryBuffer(DescriptorMemory));
  const auto PayloadMemory = lock(Request, Payload, sizeof(uint32_t), true);
  const auto Alias = memoryBuffer(PayloadMemory);
  const auto PeerMemory = lock(Request, Payload, sizeof(uint32_t), true);
  const auto PeerAlias = memoryBuffer(PeerMemory);
  enqueue(Request);
  // Returning from caller context closes probe-and-lock admission.
  EXPECT_EQ(
      invoke("WdfRequestProbeAndLockUserBufferForRead",
             {Globals, Request.Request, Payload, sizeof(uint32_t), BufferSlot}),
      framework::RequestAccessViolation);
  success(Model->revokeRequestUserBuffers(Request.IRP));
  EXPECT_FALSE(take(Memory->canAccess(Payload, 1, Read)));
  put(Alias, 0x77665544, sizeof(uint32_t));
  EXPECT_EQ(get(PeerAlias, sizeof(uint32_t)), 0x77665544u);
  success(Model->snapshot());
  EXPECT_TRUE(Result.Requests.back().UserBuffers[1].Revoked);
  EXPECT_EQ(Result.Requests.back().UserBuffers[1].Backing[4], 0x44u);
  complete(Request);
  finalize(Request);
  success(Model->snapshot());
  EXPECT_TRUE(Result.Requests.back().Output.empty());
  EXPECT_EQ(Result.Requests.back().UserBuffers[1].Backing[7], 0x77u);
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
  EXPECT_FALSE(take(Memory->canAccess(PeerAlias, 1, Read)));
}

TEST_F(KernelNeitherRequestMDL,
       ProbeAdmissionChecksPageRightsAndCurrentRequestDeclarations) {
  open(true);
  const auto First = beginCaller(nestedRequest());
  ASSERT_NE(First.IRP, 0u);
  const auto FirstPayload = Result.Requests.back().UserBuffers[1].Address;
  enqueue(First);
  const auto Second = beginCaller(nestedRequest());
  ASSERT_NE(Second.IRP, 0u);
  const auto Descriptor = Result.Requests.back().UserBuffers[0].Address;
  const auto Payload = Result.Requests.back().UserBuffers[1].Address;
  const auto Sealed = Result.Requests.back().UserBuffers[2].Address;
  EXPECT_TRUE(take(Memory->canAccess(FirstPayload, 1, Read)));
  for (const auto &Attempt : {std::pair<uint64_t, uint64_t>{FirstPayload, 4},
                              {Payload + 10, 4},
                              {Payload + 12, 1}}) {
    EXPECT_EQ(invoke("WdfRequestProbeAndLockUserBufferForRead",
                     {Globals, Second.Request, Attempt.first, Attempt.second,
                      BufferSlot}),
              framework::RequestAccessViolation);
    EXPECT_EQ(get(BufferSlot), 0u);
  }
  EXPECT_EQ(invoke("WdfRequestProbeAndLockUserBufferForWrite",
                   {Globals, Second.Request, Descriptor, profile::PointerSize,
                    BufferSlot}),
            framework::RequestAccessViolation);
  EXPECT_EQ(get(BufferSlot), 0u);
  success(Model->setUserRequestContext(
      true, DriverRequest::DefaultRequestorProcessID + 1));
  EXPECT_EQ(invoke("WdfRequestProbeAndLockUserBufferForRead",
                   {Globals, Second.Request, Payload, 4, BufferSlot}),
            framework::RequestAccessViolation);
  EXPECT_EQ(get(BufferSlot), 0u);
  success(Model->setUserRequestContext(true));
  const auto DescriptorMemory = lock(Second, Descriptor, profile::PointerSize);
  EXPECT_EQ(get(memoryBuffer(DescriptorMemory)), Payload + 4);
  const auto PayloadMemory = lock(Second, Payload, 4, true);
  put(memoryBuffer(PayloadMemory), 0x99887766, sizeof(uint32_t));
  complete(Second);
  const auto Return = take(Model->continueFrameworkCallerContext(Second.IRP));
  EXPECT_EQ(Return.PC, 0u);
  success(Model->recordDispatchReturn(Second.IRP, Pending));
  finalize(Second);
  complete(First);
  finalize(First);
}

TEST_F(KernelRequestMDL,
       InformationAndRawIrpShareStorageAndFinalCompletionValidation) {
  open();
  const auto Request = begin();
  EXPECT_EQ(invoke("WdfRequestWdmGetIrp", {Globals, Request.Request}),
            Request.IRP);
  EXPECT_EQ(invoke("WdfRequestGetFileObject", {Globals, Request.Request}), 0u);
  constexpr uint64_t Large = 0x123456789abcdef0;
  invoke("WdfRequestSetInformation", {Globals, Request.Request, Large});
  EXPECT_EQ(invoke("WdfRequestGetInformation", {Globals, Request.Request}),
            Large);
  const uint64_t Information = Request.IRP + windows::IRPInformationOffset;
  success(Model->validateGuestAccess(Information, 8, false));
  EXPECT_EQ(get(Information), Large);
  // Setting Information must not initialize the independent Status bytes.
  rejected(Model->validateGuestAccess(Request.IRP + windows::IRPStatusOffset, 4,
                                      false),
           "opaque");
  rejected(call("WdfRequestComplete", {Globals, Request.Request, 0}),
           "exceeds");
  EXPECT_TRUE(Model->requestPending(Request.IRP));
  EXPECT_EQ(invoke("WdfRequestGetIoQueue", {Globals, Request.Request}), Queue);
  success(Model->validateGuestAccess(Information, 8, true));
  put(Information, 3);
  EXPECT_EQ(invoke("WdfRequestGetInformation", {Globals, Request.Request}), 3u);
  invoke("WdfRequestComplete", {Globals, Request.Request, 0});
  EXPECT_EQ(Result.Requests.back().Information, 3u);
  EXPECT_EQ(Result.Requests.back().Output.size(), 3u);
  rejected(Model->validateGuestAccess(Information, 8, false), "freed");
  finalize(Request);

  const auto NoOutput = begin(DriverRequestKind::DeviceControl, 0x222000, 3, 0);
  invoke("WdfRequestSetInformation", {Globals, NoOutput.Request, Large});
  invoke("WdfRequestComplete", {Globals, NoOutput.Request, 0});
  EXPECT_EQ(Result.Requests.back().Information, Large);
  EXPECT_TRUE(Result.Requests.back().Output.empty());
  finalize(NoOutput);
}

TEST_F(KernelRequestMDL, RawWdmCompletionCannotBypassFrameworkOwnership) {
  open();
  const auto Request = begin();
  const auto IRP = invoke("WdfRequestWdmGetIrp", {Globals, Request.Request});
  for (const char *Name : {"IofCompleteRequest", "IoCompleteRequest"}) {
    rejected(Model->call(Name, {IRP, 0}), "framework-owned requests");
    EXPECT_FALSE(Result.Requests.back().Completed);
    EXPECT_TRUE(Model->requestPending(IRP));
    EXPECT_EQ(invoke("WdfRequestGetIoQueue", {Globals, Request.Request}),
              Queue);
  }
  complete(Request);
  EXPECT_TRUE(Result.Requests.back().Completed);
  finalize(Request);
}

TEST_F(KernelRequestMDL,
       BufferedDirectionsShareFirstDescriptorAndOriginalSystemBuffer) {
  open();
  for (bool OutputFirst : {false, true}) {
    SCOPED_TRACE(OutputFirst);
    const auto Request = begin();
    const auto First = mdl(Request, OutputFirst);
    const auto Second = mdl(Request, !OutputFirst);
    ASSERT_NE(First, 0u);
    EXPECT_EQ(First, Second);
    EXPECT_EQ(get(First + windows::MDLByteCountOffset, 4),
              OutputFirst ? 7u : 3u);
    EXPECT_EQ(get(First + windows::MDLFlagsOffset, 2),
              windows::MDLSourceIsNonPagedPool);
    EXPECT_EQ(get(Request.IRP + windows::IRPMdlOffset), 0u);
    const auto System = get(Request.IRP + windows::IRPSystemBufferOffset);
    EXPECT_EQ(mapping(First), System);
    EXPECT_EQ(mapping(First, 0xc0000010), System);
    success(Model->validateGuestAccess(System, 7, true));
    rejected(Model->validateGuestAccess(First + windows::MDLByteCountOffset, 4,
                                        true),
             "read-only");
    success(Model->validateGuestAccess(First + windows::MDLSize, 8, false));
    const auto PFN = get(First + windows::MDLSize);
    EXPECT_GE(PFN, DriverDmaPhysicalBase / DriverDmaPageSize);
    EXPECT_LT(PFN, (DriverDmaPhysicalBase + DriverDmaPhysicalSize) /
                       DriverDmaPageSize);
    EXPECT_NE(PFN, System / DriverDmaPageSize);
    rejected(Model->validateGuestAccess(First + windows::MDLSize, 8, true),
             "read-only");
    rejected(
        Model->call("MmMapLockedPagesSpecifyCache", {First, 0, 1, 0, 0, 0x10}),
        "additional");
    rejected(Model->call("MmUnmapLockedPages", {System, First}), "existing");
    rejected(Model->call("IoFreeMdl", {First}), "request-owned");
    EXPECT_EQ(mapping(First), System);
    success(Memory->write(System, {1, 2, 3, 4, 5, 6, 7}));
    complete(Request, 7);
    EXPECT_EQ(Result.Requests.back().Output,
              (std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7}));
    rejected(Model->validateGuestAccess(First, 1, false), "freed");
    rejected(Model->validateGuestAccess(System, 1, false), "freed");
    // Retiring a descriptor cannot revoke an arena page shared by the device.
    success(Model->validateGuestAccess(WdmDevice + windows::DeviceFlagsOffset,
                                       4, false));
    finalize(Request);
  }
}

TEST_F(KernelRequestMDL, PrivateFrameworkDescriptorCannotJoinWdmChain) {
  open();
  const auto Request = begin();
  const auto Private = mdl(Request, true);
  const auto Link = Request.IRP + windows::IRPMdlOffset;
  success(Model->validateGuestAccess(Link, sizeof(uint64_t), true));
  put(Link, Private);
  rejected(call("WdfRequestComplete", {Globals, Request.Request, 0}),
           "framework buffer");
  EXPECT_TRUE(Model->requestPending(Request.IRP));
  EXPECT_EQ(mdl(Request, true), Private);
  success(Model->validateGuestAccess(Private, sizeof(uint64_t), false));
  put(Link, 0);
  complete(Request);
  rejected(Model->validateGuestAccess(Private, 1, false), "freed");
  finalize(Request);
}

TEST_F(KernelRequestMDL,
       BothDirectIoctlMethodsKeepInputDescriptorSeparateAndMappingRestricted) {
  open();
  for (uint32_t Method : {1u, 2u}) {
    SCOPED_TRACE(Method);
    const auto Request =
        begin(DriverRequestKind::DeviceControl, 0x222000 | Method);
    const auto Original = get(Request.IRP + windows::IRPMdlOffset);
    ASSERT_NE(Original, 0u);
    const auto Input = mdl(Request, false);
    const auto Output = mdl(Request, true);
    EXPECT_NE(Input, Output);
    EXPECT_EQ(Output, Original);
    EXPECT_EQ(get(Output + windows::MDLMappedSystemVAOffset), 0u);
    EXPECT_EQ(get(Output + windows::MDLFlagsOffset, 2),
              windows::MDLPagesLocked);
    success(Model->validateGuestAccess(Output + windows::MDLSize, 8, false));
    const auto OutputPFN = get(Output + windows::MDLSize);
    EXPECT_GE(OutputPFN, DriverDmaPhysicalBase / DriverDmaPageSize);
    EXPECT_NE(OutputPFN,
              get(Output + windows::MDLStartVAOffset) / DriverDmaPageSize);
    EXPECT_EQ(get(Input + windows::MDLByteCountOffset, 4), 3u);
    EXPECT_EQ(get(Output + windows::MDLByteCountOffset, 4), 7u);
    const auto System = get(Request.IRP + windows::IRPSystemBufferOffset);
    EXPECT_EQ(mapping(Input), System);
    const auto OutputVA = kernel("MmMapLockedPagesSpecifyCache",
                                 {Output, 0, 1, 0, 0, 0x80000010});
    EXPECT_NE(OutputVA, System);
    EXPECT_EQ(mdl(Request, true), Output);
    EXPECT_EQ(invoke("WdfRequestRetrieveOutputBuffer",
                     {Globals, Request.Request, 7, BufferSlot, LengthSlot}),
              0u);
    EXPECT_EQ(get(BufferSlot), OutputVA);
    rejected(Model->validateGuestAccess(OutputVA, 1, true),
             "MdlMappingNoWrite");
    success(Model->validateGuestAccess(System, 3, true));
    EXPECT_EQ(get(System, 1), 0x5au);
    complete(Request);
    for (uint64_t Address : {Input, Output, System, OutputVA, Request.IRP})
      rejected(Model->validateGuestAccess(Address, 1, false), "freed");
    finalize(Request);
  }
}

TEST_F(KernelRequestMDL, PoolMDLViewsSharePhysicalPagesAndRemainReadOnly) {
  const auto Pool =
      kernel("ExAllocatePoolWithTag",
             {windows::PoolNX, 2 * DriverDmaPageSize, 0x70666e31});
  ASSERT_NE(Pool, 0u);
  const auto First =
      kernel("IoAllocateMdl", {Pool + 16, DriverDmaPageSize, 0, 0, 0});
  const auto Second =
      kernel("IoAllocateMdl", {Pool + DriverDmaPageSize, 16, 0, 0, 0});
  ASSERT_NE(First, 0u);
  ASSERT_NE(Second, 0u);
  rejected(Model->validateGuestAccess(First + windows::MDLSize, 8, false),
           "unbuilt");
  kernel("MmBuildMdlForNonPagedPool", {First});
  kernel("MmBuildMdlForNonPagedPool", {Second});
  success(Model->validateGuestAccess(First + windows::MDLSize, 16, false));
  success(Model->validateGuestAccess(Second + windows::MDLSize, 8, false));
  EXPECT_EQ(get(First + windows::MDLSize + 8), get(Second + windows::MDLSize));
  EXPECT_NE(get(First + windows::MDLSize), get(First + windows::MDLSize + 8));
  EXPECT_EQ(mapping(First), Pool + 16);
  EXPECT_EQ(mapping(Second), Pool + DriverDmaPageSize);
  rejected(Model->validateGuestAccess(Second + windows::MDLSize, 8, true),
           "read-only");
  kernel("IoFreeMdl", {First});
  success(Model->validateGuestAccess(Second + windows::MDLSize, 8, false));
  kernel("IoFreeMdl", {Second});
  kernel("ExFreePoolWithTag", {Pool, 0x70666e31});
}

TEST_F(KernelRequestMDL, BufferedReadWriteDirectionsAndZeroLengthStatus) {
  open();
  for (auto Kind : {DriverRequestKind::Read, DriverRequestKind::Write}) {
    const bool Output = Kind == DriverRequestKind::Read;
    const auto Request = begin(Kind);
    const auto MDL = mdl(Request, Output);
    EXPECT_EQ(get(MDL + windows::MDLByteCountOffset, 4), Output ? 7u : 3u);
    EXPECT_EQ(mapping(MDL), get(Request.IRP + windows::IRPSystemBufferOffset));
    EXPECT_EQ(invoke(Output ? "WdfRequestRetrieveInputWdmMdl"
                            : "WdfRequestRetrieveOutputWdmMdl",
                     {Globals, Request.Request, BufferSlot}),
              0xc0000010u);
    EXPECT_EQ(get(BufferSlot), 0u);
    complete(Request);
    finalize(Request);
    const auto Empty = begin(Kind, 0, 0, 0);
    put(BufferSlot, UINT64_MAX);
    EXPECT_EQ(invoke(Output ? "WdfRequestRetrieveOutputWdmMdl"
                            : "WdfRequestRetrieveInputWdmMdl",
                     {Globals, Empty.Request, BufferSlot}),
              0xc0000023u);
    EXPECT_EQ(get(BufferSlot), 0u);
    complete(Empty);
    finalize(Empty);
  }
}

TEST_F(KernelDirectRequestMDL, DirectReadWriteReuseThePacketMdlWithoutMapping) {
  open();
  for (auto Kind : {DriverRequestKind::Read, DriverRequestKind::Write}) {
    const auto Request = begin(Kind);
    const auto MDL = mdl(Request, Kind == DriverRequestKind::Read);
    EXPECT_EQ(MDL, get(Request.IRP + windows::IRPMdlOffset));
    EXPECT_EQ(get(MDL + windows::MDLMappedSystemVAOffset), 0u);
    const auto Buffer = mapping(MDL);
    success(Model->validateGuestAccess(Buffer, 1, false));
    if (Kind == DriverRequestKind::Write)
      EXPECT_EQ(get(Buffer, 1), 0x5au);
    complete(Request);
    rejected(Model->validateGuestAccess(MDL, 1, false), "freed");
    rejected(Model->validateGuestAccess(Buffer, 1, false), "freed");
    finalize(Request);
  }
}

TEST_F(KernelRequestMDL,
       CleanupCanUseSavedMdlButRetainedRequestCannotReacquireStorage) {
  open();
  const auto Request = begin();
  const auto MDL = mdl(Request, false);
  const auto System = mapping(MDL);
  const auto IRP = invoke("WdfRequestWdmGetIrp", {Globals, Request.Request});
  invoke("WdfRequestSetInformation", {Globals, Request.Request, 1});
  context(Request, WorkerPC, DestroyPC);
  invoke("WdfObjectReferenceActual", {Globals, Request.Request, 0, 0, 0});
  complete(Request, 3);
  auto Cleanup = Model->takeGuestCall();
  ASSERT_TRUE(Cleanup);
  EXPECT_EQ(Cleanup->PC, WorkerPC);
  EXPECT_FALSE(Result.Requests.back().Completed);
  const uint64_t Information = IRP + windows::IRPInformationOffset;
  success(Model->validateGuestAccess(Information, 8, false));
  EXPECT_EQ(get(Information), 3u);
  // A saved IRP remains authoritative during cleanup. Completion must consume
  // its current Information, not overwrite it from a framework-side cache.
  success(Model->validateGuestAccess(Information, 8, true));
  put(Information, 2);
  EXPECT_EQ(mapping(MDL), System);
  success(Model->validateGuestAccess(System, 3, false));
  for (unsigned Phase = 0; Phase < 2; ++Phase) {
    EXPECT_EQ(invoke("WdfRequestGetInformation", {Globals, Request.Request}),
              0u);
    EXPECT_EQ(invoke("WdfRequestGetIoQueue", {Globals, Request.Request}), 0u);
    for (const char *Name :
         {"WdfRequestRetrieveInputWdmMdl", "WdfRequestRetrieveOutputWdmMdl"}) {
      put(BufferSlot, UINT64_MAX);
      EXPECT_EQ(invoke(Name, {Globals, Request.Request, BufferSlot}),
                0xc00000e5u);
      EXPECT_EQ(get(BufferSlot), 0u);
    }
    if (Phase == 0) {
      EXPECT_TRUE(take(Model->finishGuestCall(Cleanup->Token, 0)).has_value());
      EXPECT_TRUE(Result.Requests.back().Completed);
      EXPECT_EQ(Result.Requests.back().Information, 2u);
      EXPECT_EQ(Result.Requests.back().Output.size(), 2u);
      rejected(Model->validateGuestAccess(MDL, 1, false), "freed");
      rejected(Model->validateGuestAccess(System, 1, false), "freed");
    }
  }
  invoke("WdfObjectDereferenceActual", {Globals, Request.Request, 0, 0, 0});
  auto Destroy = Model->takeGuestCall();
  ASSERT_TRUE(Destroy);
  EXPECT_EQ(Destroy->PC, DestroyPC);
  EXPECT_TRUE(take(Model->finishGuestCall(Destroy->Token, 0)).has_value());
  rejected(call("WdfRequestGetInformation", {Globals, Request.Request}),
           "invalid");
  finalize(Request);
}

TEST_F(KernelRequestMDL,
       CancelCallbackHoldDoesNotExtendSystemOrDirectDescriptorLifetime) {
  open();
  const auto Request =
      begin(DriverRequestKind::DeviceControl, 0x222002, 3, 7, 5);
  context(Request);
  const auto Context = get(ContextSlot);
  const auto Input = mdl(Request, false);
  const auto Output = mdl(Request, true);
  const auto InputVA = mapping(Input);
  const auto OutputVA = mapping(Output);
  EXPECT_EQ(invoke("WdfRequestMarkCancelableEx",
                   {Globals, Request.Request, CancelPC}),
            0u);
  auto Cancel = take(Model->nextScheduled(true));
  if (!Cancel)
    Cancel = take(Model->nextScheduled(false));
  ASSERT_TRUE(Cancel);
  EXPECT_EQ(Cancel->PC, CancelPC);
  complete(Request, 0, Cancelled);
  success(Model->validateGuestAccess(Context, 24, false));
  for (uint64_t Address : {Input, Output, InputVA, OutputVA, Request.IRP})
    rejected(Model->validateGuestAccess(Address, 1, false), "freed");
  EXPECT_EQ(invoke("WdfRequestGetInformation", {Globals, Request.Request}), 0u);
  EXPECT_EQ(invoke("WdfRequestGetIoQueue", {Globals, Request.Request}), 0u);
  EXPECT_EQ(Result.Requests.back().Information, 0u);
  EXPECT_TRUE(Result.Requests.back().Output.empty());
  EXPECT_FALSE(take(Model->continueScheduled(Cancel->ID, 0)));
  success(Model->finishScheduled(Cancel->ID));
  rejected(Model->validateGuestAccess(Context, 1, false), "freed");
  finalize(Request);
}

} // namespace
} // namespace neverd::emulation
