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

namespace neverd::emulation {
namespace {
using namespace framework_test;

class DriverKernelFrameworkFile : public DriverKernelFrameworkQueue {
protected:
  static constexpr uint64_t FileConfig = Driver + 0x2400;
  static constexpr uint64_t WdmFile = Driver + 0x5000;
  static constexpr uint64_t CreateIRP = Driver + 0x5100;
  static constexpr uint64_t CleanupIRP = Driver + 0x5200;
  static constexpr uint64_t CloseIRP = Driver + 0x5300;
  static constexpr uint64_t CreatePC = 0x180003000;
  static constexpr uint64_t CleanupPC = 0x180003100;
  static constexpr uint64_t ClosePC = 0x180003200;
  uint64_t FileDevice = 0, WdmDevice = 0;
  std::map<uint64_t, KernelFramework::RequestView> Views;
  std::map<uint64_t, uint32_t> Completions;
  unsigned Pending = 0;

  void configure(uint32_t Class = framework::FileObjectCannotUseFsContexts,
                 bool Context = false) {
    string(Security, "D:P(A;;GA;;;WD)");
    const auto Init = take(invoke("WdfControlDeviceInitAllocate",
                                  {Globals, DriverHandle, Security}));
    put(InitSlot, Init);
    string(DeviceName, "\\Device\\QueueTestFiles");
    EXPECT_EQ(
        take(invoke("WdfDeviceInitAssignName", {Globals, Init, DeviceName})),
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
    EXPECT_EQ(take(invoke("WdfDeviceInitSetFileObjectConfig",
                          {Globals, Init, FileConfig, Context ? Attrs : 0})),
              0u);
    queueAttributes();
    EXPECT_EQ(
        take(invoke("WdfDeviceCreate", {Globals, InitSlot, Attrs, DeviceSlot})),
        0u);
    FileDevice = get(DeviceSlot);
    WdmDevice =
        take(invoke("WdfDeviceWdmGetDeviceObject", {Globals, FileDevice}));
    take(invoke("WdfControlFinishInitializing", {Globals, FileDevice}));
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

  KernelFramework::RequestDispatch route(uint64_t IRP, uint32_t Major) {
    Views.emplace(IRP, KernelFramework::RequestView{IRP, 0, Major, 0, 0, 0,
                                                    false, 0, 0, WdmFile});
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
  EXPECT_EQ(take(invoke("WdfRequestGetFileObject", {Globals, Request})), File);
  EXPECT_EQ(take(invoke("WdfFileObjectGetDevice", {Globals, File})),
            FileDevice);
  EXPECT_EQ(take(invoke("WdfFileObjectWdmGetFileObject", {Globals, File})),
            WdmFile);
  EXPECT_EQ(take(invoke("WdfRequestComplete", {Globals, Request, 0})), 0u);
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
  expectError(invoke("WdfFileObjectGetDevice", {Globals, File}),
              "live framework file object");
}

TEST_F(DriverKernelFrameworkFile,
       FailedCreateDeletesFileWithoutCleanupOrCloseCallbacks) {
  configure(framework::FileObjectCannotUseFsContexts, true);
  const auto Create = route(CreateIRP, framework::RequestMajorCreate);
  const uint64_t File = Create.Arguments[2];
  EXPECT_EQ(take(invoke("WdfRequestComplete", {Globals, Create.Arguments[1],
                                               framework::InvalidParameter})),
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
  expectError(invoke("WdfFileObjectWdmGetFileObject", {Globals, File}),
              "live framework file object");
}

TEST_F(DriverKernelFrameworkFile,
       CloseRunsFileCallbackThenContextCleanupAndDestroyBeforeCompleting) {
  configure(framework::FileObjectCannotUseFsContexts, true);
  const auto Create = route(CreateIRP, framework::RequestMajorCreate);
  const uint64_t File = Create.Arguments[2];
  const uint64_t Context =
      take(invoke("WdfObjectGetTypedContextWorker", {Globals, File, Type}));
  ASSERT_NE(Context, 0u);
  EXPECT_EQ(get(Context), 0u);
  take(invoke("WdfRequestComplete", {Globals, Create.Arguments[1], 0}));
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
  EXPECT_EQ(
      take(invoke("WdfRequestGetFileObject", {Globals, Create.Arguments[1]})),
      0u);
  take(invoke("WdfRequestComplete", {Globals, Create.Arguments[1], 0}));
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
  const auto Init = take(invoke("WdfControlDeviceInitAllocate",
                                {Globals, DriverHandle, Security}));
  success(Memory.write(FileConfig,
                       std::vector<uint8_t>(framework::FileConfigSize)));
  put(FileConfig, framework::FileConfigSize, sizeof(uint32_t));
  put(FileConfig + framework::FileAutoForwardOffset,
      framework::FileAutoForwardDefault, sizeof(uint32_t));
  put(FileConfig + framework::FileClassOffset,
      framework::FileObjectCanUseFsContext, sizeof(uint32_t));
  expectError(invoke("WdfDeviceInitSetFileObjectConfig",
                     {Globals, Init, FileConfig, 0}),
              "unsupported framework file-object class");
  put(FileConfig + framework::FileClassOffset,
      framework::FileObjectCannotUseFsContexts, sizeof(uint32_t));
  put(FileConfig + framework::FileAutoForwardOffset,
      framework::FileAutoForwardTrue, sizeof(uint32_t));
  expectError(invoke("WdfDeviceInitSetFileObjectConfig",
                     {Globals, Init, FileConfig, 0}),
              "lower target");
  EXPECT_EQ(take(invoke("WdfDeviceInitFree", {Globals, Init})), 0u);
}
} // namespace
} // namespace neverd::emulation
