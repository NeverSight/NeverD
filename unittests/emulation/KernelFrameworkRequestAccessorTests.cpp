//===- KernelFrameworkRequestAccessorTests.cpp - WDF request access -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify accessor status, output validation and retained-handle behavior
/// independently from WDM storage, using a counted host that rejects dead IRPs.
///
//===----------------------------------------------------------------------===//

#include "KernelFrameworkQueueTestSupport.h"

namespace neverd::emulation {
namespace {
using namespace framework_test;

class DriverKernelFrameworkRequestAccessor : public DriverKernelFrameworkQueue {
protected:
  static constexpr uint64_t IRP = Driver + 0x3000;
  static constexpr uint64_t MDL = Driver + 0x4000;
  static constexpr uint64_t MdlSlot = Driver + 0x2100;
  KernelFramework::RequestView View{IRP, 0, 14, 0x222000, 3, 7};
  uint64_t Information = 0, MdlResult = MDL, Queue = 0, Request = 0;
  unsigned Reads = 0, Writes = 0, Views = 0, Completions = 0;
  bool Completed = false;
  std::vector<bool> MdlDirections;

  llvm::Error live() {
    return Completed ? failure("host touched a retired IRP")
                     : llvm::Error::success();
  }

  void SetUp() override {
    DriverKernelFrameworkQueue::SetUp();
    put(QueueConfig + 24, IoControlPC);
    put(QueueConfig + 32, IoControlPC);
    EXPECT_EQ(take(createQueue()), 0u);
    Queue = get(QueueSlot);
    take(invoke("WdfControlFinishInitializing", {Globals, Device}));
    KernelFramework::RequestHost Host;
    Host.View =
        [this](
            uint64_t Packet) -> llvm::Expected<KernelFramework::RequestView> {
      EXPECT_EQ(Packet, IRP);
      if (auto E = live())
        return std::move(E);
      ++Views;
      return View;
    };
    Host.MarkPending = [this](uint64_t Packet) {
      EXPECT_EQ(Packet, IRP);
      return live();
    };
    Host.Information = [this](uint64_t Packet) -> llvm::Expected<uint64_t> {
      EXPECT_EQ(Packet, IRP);
      if (auto E = live())
        return std::move(E);
      ++Reads;
      return Information;
    };
    Host.SetInformation = [this](uint64_t Packet,
                                 uint64_t Value) -> llvm::Error {
      EXPECT_EQ(Packet, IRP);
      if (auto E = live())
        return E;
      ++Writes;
      Information = Value;
      return llvm::Error::success();
    };
    Host.Mdl = [this](uint64_t Packet,
                      bool Output) -> llvm::Expected<uint64_t> {
      EXPECT_EQ(Packet, IRP);
      if (auto E = live())
        return std::move(E);
      MdlDirections.push_back(Output);
      return MdlResult;
    };
    Host.ValidateCompletion = [this](uint64_t Packet, uint32_t, uint64_t) {
      EXPECT_EQ(Packet, IRP);
      return live();
    };
    Host.Complete = [this](uint64_t Packet, uint32_t,
                           uint64_t Value) -> llvm::Error {
      EXPECT_EQ(Packet, IRP);
      if (auto E = live())
        return E;
      Completed = true;
      Information = Value;
      ++Completions;
      return llvm::Error::success();
    };
    Model.setRequestHost(std::move(Host));
    const auto Wdm =
        take(invoke("WdfDeviceWdmGetDeviceObject", {Globals, Device}));
    auto Dispatch = take(Model.routeRequest(Wdm, IRP));
    ASSERT_TRUE(Dispatch);
    ASSERT_EQ(Dispatch->Arguments.size(), 5u);
    Request = Dispatch->Arguments[1];
    ASSERT_NE(Request, 0u);
  }

  uint64_t mdl(bool Output) {
    put(MdlSlot, Sentinel);
    return take(invoke(Output ? "WdfRequestRetrieveOutputWdmMdl"
                              : "WdfRequestRetrieveInputWdmMdl",
                       {Globals, Request, MdlSlot}));
  }

  void checkCompletedAccessors() {
    const auto OldReads = Reads;
    const auto OldViews = Views;
    const auto OldMdls = MdlDirections.size();
    EXPECT_EQ(take(invoke("WdfRequestGetInformation", {Globals, Request})), 0u);
    EXPECT_EQ(take(invoke("WdfRequestGetIoQueue", {Globals, Request})), 0u);
    for (bool Output : {false, true}) {
      EXPECT_EQ(mdl(Output), 0xc00000e5u);
      EXPECT_EQ(get(MdlSlot), 0u);
    }
    expectError(invoke("WdfRequestSetInformation", {Globals, Request, 1}),
                "complet");
    expectError(invoke("WdfRequestGetFileObject", {Globals, Request}),
                "complet");
    expectError(invoke("WdfRequestWdmGetIrp", {Globals, Request}), "complet");
    EXPECT_EQ(Reads, OldReads);
    EXPECT_EQ(Views, OldViews);
    EXPECT_EQ(MdlDirections.size(), OldMdls);
  }
};

TEST_F(DriverKernelFrameworkRequestAccessor,
       LiveIdentityAndInformationUseTheirAuthoritativeHost) {
  EXPECT_TRUE(Model.ownsRequestIRP(IRP));
  EXPECT_FALSE(Model.ownsRequestIRP(IRP + 0x100));
  EXPECT_EQ(take(invoke("WdfRequestGetIoQueue", {Globals, Request})), Queue);
  EXPECT_EQ(take(invoke("WdfRequestGetFileObject", {Globals, Request})), 0u);
  EXPECT_EQ(take(invoke("WdfRequestWdmGetIrp", {Globals, Request})), IRP);
  take(invoke("WdfRequestSetInformation", {Globals, Request, Sentinel}));
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(Information, Sentinel);
  EXPECT_EQ(take(invoke("WdfRequestGetInformation", {Globals, Request})),
            Sentinel);
  Information = Sentinel - 1;
  EXPECT_EQ(take(invoke("WdfRequestGetInformation", {Globals, Request})),
            Sentinel - 1);
  take(invoke("WdfRequestComplete", {Globals, Request, 0}));
  EXPECT_EQ(Information, Sentinel - 1);
  EXPECT_EQ(Completions, 1u);
  EXPECT_FALSE(Model.ownsRequestIRP(IRP));
  expectError(invoke("WdfRequestGetInformation", {Globals, Request}),
              "invalid");
}

TEST_F(DriverKernelFrameworkRequestAccessor,
       CleanupAndReferencedCompletionDoNotTouchUnderlyingPacket) {
  type();
  attributes(0, Type, ChildCleanup, ChildDestroy);
  take(invoke("WdfObjectAllocateContext",
              {Globals, Request, Attrs, ContextOutput}));
  take(invoke("WdfObjectReferenceActual", {Globals, Request, 0, 0, 0}));
  take(invoke("WdfRequestSetInformation", {Globals, Request, 4}));
  take(invoke("WdfRequestComplete", {Globals, Request, 0}));
  const auto Cleanup = callback();
  EXPECT_EQ(Cleanup.PC, ChildCleanup);
  EXPECT_FALSE(Completed);
  checkCompletedAccessors();
  finish(Cleanup);
  EXPECT_TRUE(Completed);
  EXPECT_EQ(Completions, 1u);
  EXPECT_EQ(Information, 4u);
  EXPECT_TRUE(Model.ownsRequestIRP(IRP));
  checkCompletedAccessors();
  take(invoke("WdfObjectDereferenceActual", {Globals, Request, 0, 0, 0}));
  const auto Destroy = callback();
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  checkCompletedAccessors();
  finish(Destroy);
  EXPECT_FALSE(Model.ownsRequestIRP(IRP));
  expectError(invoke("WdfRequestGetIoQueue", {Globals, Request}), "invalid");
}

TEST_F(DriverKernelFrameworkRequestAccessor,
       DirectionAndEmptyBufferFailuresClearOutputWithoutAskingHostForMdl) {
  View.Major = 3;
  EXPECT_EQ(mdl(false), 0xc0000010u);
  EXPECT_EQ(get(MdlSlot), 0u);
  View.Major = 4;
  EXPECT_EQ(mdl(true), 0xc0000010u);
  EXPECT_EQ(get(MdlSlot), 0u);
  View.Major = 14;
  View.ControlCode |= 3;
  EXPECT_EQ(mdl(false), 0xc0000010u);
  EXPECT_EQ(get(MdlSlot), 0u);
  View.ControlCode &= ~uint32_t(3);
  View.InputLength = View.OutputLength = 0;
  for (bool Output : {false, true}) {
    EXPECT_EQ(mdl(Output), 0xc0000023u);
    EXPECT_EQ(get(MdlSlot), 0u);
  }
  EXPECT_TRUE(MdlDirections.empty());
}

TEST_F(DriverKernelFrameworkRequestAccessor,
       ValidatedOutputPrecedesAllocationAndResourceFailureIsStatus) {
  put(MdlSlot, Sentinel);
  DenyWriteAt = MdlSlot;
  expectError(
      invoke("WdfRequestRetrieveInputWdmMdl", {Globals, Request, MdlSlot}),
      "write validation");
  EXPECT_EQ(get(MdlSlot), Sentinel);
  EXPECT_TRUE(MdlDirections.empty());
  DenyWriteAt = 0;
  expectError(invoke("WdfRequestRetrieveInputWdmMdl", {Globals, Request, 0}),
              "null output");
  MdlResult = 0;
  EXPECT_EQ(mdl(false), 0xc000009au);
  EXPECT_EQ(get(MdlSlot), 0u);
  MdlResult = MDL;
  EXPECT_EQ(mdl(true), 0u);
  EXPECT_EQ(get(MdlSlot), MDL);
  EXPECT_EQ(MdlDirections, (std::vector<bool>{false, true}));
  const auto Other = object();
  put(MdlSlot, Sentinel);
  expectError(
      invoke("WdfRequestRetrieveInputWdmMdl", {Globals, Other, MdlSlot}),
      "invalid");
  EXPECT_EQ(get(MdlSlot), Sentinel);
}

} // namespace
} // namespace neverd::emulation
