//===- KernelFrameworkFileTests.cpp - KMDF file-object lifecycle ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise guest file callbacks against one WDM file identity, including
/// creation failure and object-context destruction after CLOSE.
///
//===----------------------------------------------------------------------===//

#include "KernelFrameworkQueueTestSupport.h"

#include <optional>

namespace neverd::emulation {
namespace {
using namespace framework_test;
namespace api = framework::api;

class DriverKernelFrameworkFile : public DriverKernelFrameworkQueue {
protected:
  static constexpr uint64_t FileConfig = Driver + 0x2400;
  static constexpr uint64_t WdmFile = Driver + 0x5000;
  static constexpr uint64_t CreateIRP = Driver + 0x5100;
  static constexpr uint64_t CleanupIRP = Driver + 0x5200;
  static constexpr uint64_t CloseIRP = Driver + 0x5300;
  static constexpr uint64_t SecondWdmFile = Driver + 0x5400;
  static constexpr uint64_t SecondCreateIRP = Driver + 0x5500;
  static constexpr uint64_t FirstControlIRP = Driver + 0x5600;
  static constexpr uint64_t SecondControlIRP = Driver + 0x5700;
  static constexpr uint64_t FileNameBuffer = Driver + 0x5800;
  static constexpr uint64_t CreatePC = 0x180003000;
  static constexpr uint64_t CleanupPC = 0x180003100;
  static constexpr uint64_t ClosePC = 0x180003200;
  uint64_t FileDevice = 0, WdmDevice = 0, FileQueue = 0;
  std::map<uint64_t, KernelFramework::RequestView> Views;
  std::map<uint64_t, uint32_t> Completions;
  unsigned Pending = 0;

  void configure(uint32_t Class = framework::FileObjectCannotUseFsContexts,
                 bool Context = false,
                 std::optional<uint32_t> Dispatch = std::nullopt) {
    string(Security, "D:P(A;;GA;;;WD)");
    const auto Init = take(invoke(api::WdfControlDeviceInitAllocate,
                                  {Globals, DriverHandle, Security}));
    put(InitSlot, Init);
    string(DeviceName, "\\Device\\QueueTestFiles");
    EXPECT_EQ(
        take(invoke(api::WdfDeviceInitAssignName, {Globals, Init, DeviceName})),
        0u);
    success(Memory.write(FileConfig,
                         std::vector<uint8_t>(framework::FileConfigSize)));
    put(FileConfig, framework::FileConfigSize, sizeof(uint32_t));
    put(FileConfig + framework::FileCreateCallbackOffset, CreatePC);
    put(FileConfig + framework::FileCleanupCallbackOffset, CleanupPC);
    put(FileConfig + framework::FileCloseCallbackOffset, ClosePC);
    put(FileConfig + framework::FileAutoForwardOffset,
        framework::FileAutoForwardDefault, sizeof(uint32_t));
    put(FileConfig + framework::FileClassOffset, Class, sizeof(uint32_t));
    if (Context) {
      type();
      queueAttributes(0, Type, ChildCleanup, ChildDestroy);
    }
    EXPECT_EQ(take(invoke(api::WdfDeviceInitSetFileObjectConfig,
                          {Globals, Init, FileConfig, Context ? Attrs : 0})),
              0u);
    queueAttributes();
    EXPECT_EQ(take(invoke(api::WdfDeviceCreate,
                          {Globals, InitSlot, Attrs, DeviceSlot})),
              0u);
    FileDevice = get(DeviceSlot);
    WdmDevice =
        take(invoke(api::WdfDeviceWdmGetDeviceObject, {Globals, FileDevice}));
    if (Dispatch) {
      Device = FileDevice;
      queueConfiguration();
      put(QueueConfig + framework::QueueConfigDispatch, *Dispatch,
          sizeof(uint32_t));
      if (*Dispatch == framework::QueueDispatchManual)
        put(QueueConfig + framework::QueueConfigDeviceControl, 0);
      EXPECT_EQ(take(createQueue()), 0u);
      FileQueue = get(QueueSlot);
    }
    take(invoke(api::WdfControlFinishInitializing, {Globals, FileDevice}));
    KernelFramework::RequestHost Host;
    Host.View =
        [this](uint64_t IRP) -> llvm::Expected<KernelFramework::RequestView> {
      auto View = Views.find(IRP);
      if (View == Views.end())
        return failure("unknown file request");
      return View->second;
    };
    Host.MarkPending = [this](uint64_t) -> llvm::Error {
      ++Pending;
      return llvm::Error::success();
    };
    Host.Information = [](uint64_t) -> llvm::Expected<uint64_t> { return 0; };
    Host.SetInformation = [](uint64_t, uint64_t) {
      return llvm::Error::success();
    };
    Host.ValidateCompletion = [](uint64_t, uint32_t, uint64_t) {
      return llvm::Error::success();
    };
    Host.Complete = [this](uint64_t IRP, uint32_t Status,
                           uint64_t Information) -> llvm::Error {
      if (Information || !Completions.emplace(IRP, Status).second)
        return failure("duplicate or nonempty file completion");
      return llvm::Error::success();
    };
    Model.setRequestHost(std::move(Host));
  }

  KernelFramework::RequestDispatch route(uint64_t IRP, uint32_t Major,
                                         uint64_t File = WdmFile) {
    Views.emplace(IRP, KernelFramework::RequestView{IRP, 0, Major, 0, 0, 0,
                                                    false, 0, 0, File});
    auto Dispatch = take(Model.routeRequest(WdmDevice, IRP));
    EXPECT_TRUE(Dispatch);
    return Dispatch.value_or(KernelFramework::RequestDispatch{});
  }
};

TEST_F(DriverKernelFrameworkFile,
       CreateRequestAndFileAccessorsKeepDistinctWdfAndWdmIdentity) {
  configure();
  const auto Create = route(CreateIRP, framework::RequestMajorCreate);
  ASSERT_EQ(Create.PC, CreatePC);
  ASSERT_EQ(Create.Arguments.size(), 3u);
  const uint64_t Request = Create.Arguments[1];
  const uint64_t File = Create.Arguments[2];
  EXPECT_EQ(Create.Arguments[0], FileDevice);
  EXPECT_NE(Request, File);
  EXPECT_NE(File, WdmFile);
  EXPECT_EQ(take(invoke(api::WdfRequestGetFileObject, {Globals, Request})),
            File);
  EXPECT_EQ(take(invoke(api::WdfFileObjectGetDevice, {Globals, File})),
            FileDevice);
  EXPECT_EQ(take(invoke(api::WdfFileObjectWdmGetFileObject, {Globals, File})),
            WdmFile);
  EXPECT_EQ(take(invoke(api::WdfRequestComplete, {Globals, Request, 0})), 0u);
  EXPECT_EQ(Completions.at(CreateIRP), 0u);
  EXPECT_EQ(Pending, 1u);

  const auto Cleanup = route(CleanupIRP, framework::RequestMajorCleanup);
  EXPECT_EQ(Cleanup.Status, windows::StatusPending);
  EXPECT_FALSE(Completions.contains(CleanupIRP));
  auto Call = callback();
  EXPECT_EQ(Call.PC, CleanupPC);
  ASSERT_EQ(Call.Arguments.size(), 1u);
  EXPECT_EQ(Call.Arguments[0], File);
  finish(Call);
  EXPECT_EQ(Completions.at(CleanupIRP), 0u);

  const auto Close = route(CloseIRP, framework::RequestMajorClose);
  EXPECT_EQ(Close.Status, windows::StatusPending);
  Call = callback();
  EXPECT_EQ(Call.PC, ClosePC);
  EXPECT_EQ(Call.Arguments[0], File);
  EXPECT_FALSE(Completions.contains(CloseIRP));
  finish(Call);
  EXPECT_EQ(Completions.at(CloseIRP), 0u);
  expectError(invoke(api::WdfFileObjectGetDevice, {Globals, File}),
              "live framework file object");
  expectError(invoke(api::WdfFileObjectGetFileName, {Globals, File}),
              "live framework file object");
  expectError(invoke(api::WdfFileObjectGetFlags, {Globals, File}),
              "live framework file object");
}

TEST_F(DriverKernelFrameworkFile,
       FileNameAndFlagsReadTheAuthoritativeWdmFileObject) {
  configure();
  constexpr llvm::StringLiteral Name = "\\sample";
  utf16(FileNameBuffer, Name);
  put(WdmFile + windows::FileNameOffset, Name.size() * sizeof(uint16_t),
      sizeof(uint16_t));
  put(WdmFile + windows::FileNameOffset + windows::UnicodeMaximumOffset,
      (Name.size() + 1) * sizeof(uint16_t), sizeof(uint16_t));
  put(WdmFile + windows::FileNameOffset + windows::UnicodeBufferOffset,
      FileNameBuffer);
  constexpr uint32_t Flags = windows::FileSynchronousIO;
  put(WdmFile + windows::FileFlagsOffset, Flags, sizeof(uint32_t));

  const auto Create = route(CreateIRP, framework::RequestMajorCreate);
  const uint64_t File = Create.Arguments[2];
  const uint64_t FileName =
      take(invoke(api::WdfFileObjectGetFileName, {Globals, File}));
  EXPECT_EQ(FileName, WdmFile + windows::FileNameOffset);
  EXPECT_EQ(get(FileName, sizeof(uint16_t)), Name.size() * sizeof(uint16_t));
  EXPECT_EQ(get(FileName + windows::UnicodeBufferOffset), FileNameBuffer);
  EXPECT_EQ(take(invoke(api::WdfFileObjectGetFlags, {Globals, File})), Flags);

  constexpr uint32_t UpdatedFlags = 0;
  put(WdmFile + windows::FileFlagsOffset, UpdatedFlags, sizeof(uint32_t));
  EXPECT_EQ(take(invoke(api::WdfFileObjectGetFlags, {Globals, File})),
            UpdatedFlags);
  take(invoke(api::WdfRequestComplete, {Globals, Create.Arguments[1], 0}));
}

TEST_F(DriverKernelFrameworkFile,
       ManualQueueFindAndRetrieveRequestsByFileObject) {
  configure(framework::FileObjectCannotUseFsContexts, false,
            framework::QueueDispatchManual);
  const auto FirstCreate = route(CreateIRP, framework::RequestMajorCreate);
  const uint64_t FirstFile = FirstCreate.Arguments[2];
  take(invoke(api::WdfRequestComplete, {Globals, FirstCreate.Arguments[1], 0}));
  const auto SecondCreate =
      route(SecondCreateIRP, framework::RequestMajorCreate, SecondWdmFile);
  const uint64_t SecondFile = SecondCreate.Arguments[2];
  take(
      invoke(api::WdfRequestComplete, {Globals, SecondCreate.Arguments[1], 0}));

  EXPECT_EQ(route(FirstControlIRP, framework::RequestMajorDeviceControl).Status,
            windows::StatusPending);
  EXPECT_EQ(route(SecondControlIRP, framework::RequestMajorDeviceControl,
                  SecondWdmFile)
                .Status,
            windows::StatusPending);

  put(Output, Sentinel);
  EXPECT_EQ(take(invoke(api::WdfIoQueueFindRequest,
                        {Globals, FileQueue, 0, FirstFile, 0, Output})),
            0u);
  const uint64_t FirstRequest = get(Output);

  put(Output, Sentinel);
  EXPECT_EQ(take(invoke(api::WdfIoQueueRetrieveRequestByFileObject,
                        {Globals, FileQueue, SecondFile, Output})),
            0u);
  const uint64_t SecondRequest = get(Output);
  EXPECT_NE(FirstRequest, SecondRequest);
  EXPECT_EQ(
      take(invoke(api::WdfRequestGetFileObject, {Globals, SecondRequest})),
      SecondFile);
  put(Output, Sentinel);
  EXPECT_EQ(take(invoke(api::WdfIoQueueRetrieveRequestByFileObject,
                        {Globals, FileQueue, SecondFile, Output})),
            framework::QueueNoMoreEntries);
  EXPECT_EQ(get(Output), Sentinel);
  expectError(invoke(api::WdfIoQueueRetrieveRequestByFileObject,
                     {Globals, FileQueue, 0, Output}),
              "live file object");
  EXPECT_EQ(get(Output), Sentinel);

  EXPECT_EQ(take(invoke(api::WdfIoQueueRetrieveFoundRequest,
                        {Globals, FileQueue, FirstRequest, Output})),
            0u);
  EXPECT_EQ(get(Output), FirstRequest);
  EXPECT_EQ(take(invoke(api::WdfRequestGetFileObject, {Globals, FirstRequest})),
            FirstFile);
  take(invoke(api::WdfObjectDereferenceActual,
              {Globals, FirstRequest, 0, 0, 0}));
  take(invoke(api::WdfRequestComplete, {Globals, FirstRequest, 0}));
  take(invoke(api::WdfRequestComplete, {Globals, SecondRequest, 0}));
  EXPECT_EQ(Completions.at(FirstControlIRP), 0u);
  EXPECT_EQ(Completions.at(SecondControlIRP), 0u);
}

TEST_F(DriverKernelFrameworkFile,
       SequentialQueueRetrievesQueuedRequestByFileObject) {
  configure(framework::FileObjectCannotUseFsContexts, false,
            framework::QueueDispatchSequential);
  const auto FirstCreate = route(CreateIRP, framework::RequestMajorCreate);
  take(invoke(api::WdfRequestComplete, {Globals, FirstCreate.Arguments[1], 0}));
  const auto SecondCreate =
      route(SecondCreateIRP, framework::RequestMajorCreate, SecondWdmFile);
  const uint64_t SecondFile = SecondCreate.Arguments[2];
  take(
      invoke(api::WdfRequestComplete, {Globals, SecondCreate.Arguments[1], 0}));

  const auto First =
      route(FirstControlIRP, framework::RequestMajorDeviceControl);
  EXPECT_EQ(First.PC, IoControlPC);
  ASSERT_GE(First.Arguments.size(), 2u);
  const uint64_t FirstRequest = First.Arguments[1];
  const auto Second = route(
      SecondControlIRP, framework::RequestMajorDeviceControl, SecondWdmFile);
  EXPECT_EQ(Second.Status, windows::StatusPending);
  EXPECT_EQ(Second.PC, 0u);

  put(Output, Sentinel);
  EXPECT_EQ(take(invoke(api::WdfIoQueueRetrieveRequestByFileObject,
                        {Globals, FileQueue, SecondFile, Output})),
            0u);
  const uint64_t SecondRequest = get(Output);
  EXPECT_NE(SecondRequest, FirstRequest);
  EXPECT_EQ(
      take(invoke(api::WdfRequestGetFileObject, {Globals, SecondRequest})),
      SecondFile);
  take(invoke(api::WdfRequestComplete, {Globals, SecondRequest, 0}));
  take(invoke(api::WdfRequestComplete, {Globals, FirstRequest, 0}));
  EXPECT_EQ(Completions.at(FirstControlIRP), 0u);
  EXPECT_EQ(Completions.at(SecondControlIRP), 0u);
}

TEST_F(DriverKernelFrameworkFile,
       FailedCreateDeletesFileWithoutCleanupOrCloseCallbacks) {
  configure(framework::FileObjectCannotUseFsContexts, true);
  const auto Create = route(CreateIRP, framework::RequestMajorCreate);
  const uint64_t File = Create.Arguments[2];
  EXPECT_EQ(
      take(invoke(api::WdfRequestComplete,
                  {Globals, Create.Arguments[1], framework::InvalidParameter})),
      0u);
  const auto Cleanup = callback();
  EXPECT_EQ(Cleanup.PC, ChildCleanup);
  EXPECT_EQ(Cleanup.Arguments[0], File);
  finish(Cleanup);
  const auto Destroy = callback();
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  finish(Destroy);
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_EQ(Completions.at(CreateIRP), framework::InvalidParameter);
  expectError(invoke(api::WdfFileObjectWdmGetFileObject, {Globals, File}),
              "live framework file object");
}

TEST_F(DriverKernelFrameworkFile,
       CloseRunsFileCallbackThenContextCleanupAndDestroyBeforeCompleting) {
  configure(framework::FileObjectCannotUseFsContexts, true);
  const auto Create = route(CreateIRP, framework::RequestMajorCreate);
  const uint64_t File = Create.Arguments[2];
  const uint64_t Context =
      take(invoke(api::WdfObjectGetTypedContextWorker, {Globals, File, Type}));
  ASSERT_NE(Context, 0u);
  EXPECT_EQ(get(Context), 0u);
  take(invoke(api::WdfRequestComplete, {Globals, Create.Arguments[1], 0}));
  route(CleanupIRP, framework::RequestMajorCleanup);
  finish(callback());
  route(CloseIRP, framework::RequestMajorClose);
  auto Call = callback();
  EXPECT_EQ(Call.PC, ClosePC);
  finish(Call);
  Call = callback();
  EXPECT_EQ(Call.PC, ChildCleanup);
  EXPECT_FALSE(Completions.contains(CloseIRP));
  finish(Call);
  Call = callback();
  EXPECT_EQ(Call.PC, ChildDestroy);
  EXPECT_FALSE(Completions.contains(CloseIRP));
  finish(Call);
  EXPECT_EQ(Completions.at(CloseIRP), 0u);
}

TEST_F(DriverKernelFrameworkFile,
       FileObjectNotRequiredPassesNullToCreateAndRequestAccessors) {
  configure(framework::FileObjectNotRequired);
  const auto Create = route(CreateIRP, framework::RequestMajorCreate);
  ASSERT_EQ(Create.Arguments.size(), 3u);
  EXPECT_EQ(Create.Arguments[2], 0u);
  EXPECT_EQ(take(invoke(api::WdfRequestGetFileObject,
                        {Globals, Create.Arguments[1]})),
            0u);
  take(invoke(api::WdfRequestComplete, {Globals, Create.Arguments[1], 0}));
  EXPECT_EQ(Completions.at(CreateIRP), 0u);
  route(CleanupIRP, framework::RequestMajorCleanup);
  auto Call = callback();
  EXPECT_EQ(Call.PC, CleanupPC);
  EXPECT_EQ(Call.Arguments[0], 0u);
  finish(Call);
  route(CloseIRP, framework::RequestMajorClose);
  Call = callback();
  EXPECT_EQ(Call.PC, ClosePC);
  EXPECT_EQ(Call.Arguments[0], 0u);
  finish(Call);
  EXPECT_EQ(Completions.at(CloseIRP), 0u);
}

TEST_F(DriverKernelFrameworkFile,
       UnsupportedFileClassAndForwardingPreserveInitializer) {
  string(Security, "D:P(A;;GA;;;WD)");
  const auto Init = take(invoke(api::WdfControlDeviceInitAllocate,
                                {Globals, DriverHandle, Security}));
  success(Memory.write(FileConfig,
                       std::vector<uint8_t>(framework::FileConfigSize)));
  put(FileConfig, framework::FileConfigSize, sizeof(uint32_t));
  put(FileConfig + framework::FileAutoForwardOffset,
      framework::FileAutoForwardDefault, sizeof(uint32_t));
  put(FileConfig + framework::FileClassOffset,
      framework::FileObjectCanUseFsContext, sizeof(uint32_t));
  expectError(invoke(api::WdfDeviceInitSetFileObjectConfig,
                     {Globals, Init, FileConfig, 0}),
              "unsupported framework file-object class");
  put(FileConfig + framework::FileClassOffset,
      framework::FileObjectCannotUseFsContexts, sizeof(uint32_t));
  put(FileConfig + framework::FileAutoForwardOffset,
      framework::FileAutoForwardTrue, sizeof(uint32_t));
  expectError(invoke(api::WdfDeviceInitSetFileObjectConfig,
                     {Globals, Init, FileConfig, 0}),
              "lower target");
  EXPECT_EQ(take(invoke(api::WdfDeviceInitFree, {Globals, Init})), 0u);
}
} // namespace
} // namespace neverd::emulation
