//===- KernelDriverIRPOwnershipTests.cpp - Submitted caller packets -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise first-send admission and completion disposal through real model
/// APIs. Rejected ownership changes must preserve packets and their callers.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;
enum class RequestMajor : uint8_t {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Value) Name = Value,
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
};

class KernelDriverIRPOwnership : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t DispatchPC = 0x180001000;
  static constexpr uint64_t CompletionPC = DispatchPC + 0x100;
  static constexpr uint64_t Context = Scratch + 0x100;
  static constexpr uint32_t InternalCode = 0x222003;
  static constexpr uint8_t CompletionFlags =
      StackInvokeOnSuccess | StackInvokeOnError | StackInvokeOnCancel;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t Lower = 0, Upper = 0, Other = 0;

  void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void rejected(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    const auto Message = llvm::toString(std::move(E));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  template <class T>
  void rejected(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    rejected(Value.takeError(), Text);
  }
  uint64_t call(llvm::StringRef API,
                std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(API.str(), Arguments));
  }
  uint64_t get(uint64_t Address, unsigned Width = sizeof(uint64_t)) {
    success(Model->validateGuestAccess(Address, Width, false));
    return take(Memory->readInteger(Address, Width));
  }
  void put(uint64_t Address, uint64_t Value,
           unsigned Width = sizeof(uint64_t)) {
    success(Model->validateGuestAccess(Address, Width, true));
    success(Memory->writeInteger(Address, Value, Width));
  }
  void SetUp() override { initialize(); }
  void initialize(DriverOptions Options = {}) {
    Model.reset();
    Result = DriverResult{};
    Memory = take(UnicornBackend::create(4 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, profile::PageSize, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = DispatchPC - profile::PageSize;
    Image.Entry = DispatchPC;
    Image.Size = 3 * profile::PageSize;
    success(Model->initialize(Image, Options));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    const auto Create = [&] {
      EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 0, 0,
                                        UnknownDeviceType, 0, 0, Scratch}),
                StatusSuccess);
      return get(Scratch);
    };
    Lower = Create();
    Upper = Create();
    Other = Create();
    ASSERT_NE(Lower, 0u);
    ASSERT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Lower}), Lower);
    put(Model->driverObject() + DriverDispatchOffset +
            uint8_t(RequestMajor::InternalDeviceControl) * sizeof(uint64_t),
        DispatchPC);
    success(Model->finishEntry());
  }
  uint64_t nextStack(uint64_t IRP) {
    return get(IRP + IRPStackPointerOffset) - StackSize;
  }
  uint64_t packet(uint8_t Count = 1) {
    const auto IRP = call("IoAllocateIrp", {Count, false});
    EXPECT_NE(IRP, 0u);
    const auto Stack = nextStack(IRP);
    put(Stack, uint8_t(RequestMajor::InternalDeviceControl), 1);
    put(Stack + StackIOControlOffset, InternalCode, 4);
    put(Stack + StackCompletionOffset, CompletionPC);
    put(Stack + StackCompletionContextOffset, Context);
    put(Stack + StackControlOffset, CompletionFlags, 1);
    return IRP;
  }
  KernelGuestCall callback(uint64_t PC) {
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->PC, PC);
    EXPECT_EQ(Call->Token.Owner, GuestCallOwner::WDM);
    Model->enterExecution(profile::CallbackStackBase, profile::StackBase,
                          Call->Token);
    return *Call;
  }
  KernelGuestCall submit(uint64_t IRP, uint64_t Device = 0) {
    EXPECT_EQ(call("IoCallDriver", {Device ? Device : Lower, IRP}), 0u);
    return callback(DispatchPC);
  }
  void status(uint64_t IRP, uint32_t Status = StatusSuccess) {
    put(IRP + IRPStatusOffset, Status, 4);
    put(IRP + IRPInformationOffset, 0);
  }
  KernelGuestCall complete(uint64_t IRP) {
    call("IofCompleteRequest", {IRP, 0});
    return callback(CompletionPC);
  }
  void finish(const KernelGuestCall &Call, uint64_t Value,
              uint64_t Expected = 0) {
    Model->enterExecution(profile::CallbackStackBase, profile::StackBase,
                          Call.Token);
    const auto Result = take(Model->finishGuestCall(Call.Token, Value));
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, Expected);
  }
  const DriverRequestResult &observation(uint64_t IRP) {
    auto Found = std::find_if(Result.Requests.begin(), Result.Requests.end(),
                              [&](const auto &R) { return R.IRP == IRP; });
    EXPECT_NE(Found, Result.Requests.end());
    return *Found;
  }
  void finishInline(uint64_t IRP, const KernelGuestCall &Dispatch) {
    status(IRP);
    const auto Complete = complete(IRP);
    call("IoFreeIrp", {IRP});
    finish(Complete, StatusMoreProcessingRequired);
    finish(Dispatch, StatusSuccess);
    EXPECT_TRUE(observation(IRP).Completed);
    EXPECT_EQ(observation(IRP).DispatchStatus, StatusSuccess);
  }
  void expectUnsubmitted(uint64_t IRP, uint8_t Count = 1) {
    EXPECT_EQ(get(IRP + IRPLocationOffset, 1), uint32_t(Count) + 1);
    EXPECT_EQ(get(IRP + IRPStackPointerOffset),
              IRP + IRPSize + uint64_t(Count) * StackSize);
    EXPECT_TRUE(Result.Requests.empty());
    EXPECT_FALSE(Model->takeGuestCall());
  }
};

TEST_F(KernelDriverIRPOwnership, MissingCompletionRejectsBeforeAdoption) {
  const auto IRP = packet();
  const auto Stack = nextStack(IRP);
  put(Stack + StackCompletionOffset, 0);
  rejected(Model->call("IoCallDriver", {Lower, IRP}), "completion routine");
  expectUnsubmitted(IRP);
  put(Stack + StackCompletionOffset, CompletionPC);
  put(Stack + StackControlOffset, StackInvokeOnSuccess, 1);
  rejected(Model->call("IoCallDriver", {Lower, IRP}), "cancellation");
  expectUnsubmitted(IRP);
  put(Stack + StackControlOffset, CompletionFlags, 1);
  const auto Dispatch = submit(IRP);
  finishInline(IRP, Dispatch);
}

TEST_F(KernelDriverIRPOwnership, InvalidHeaderAndModePreserveAllocation) {
  const auto IRP = packet();
  put(IRP + IRPRequestorModeOffset, UserMode, 1);
  rejected(Model->call("IoCallDriver", {Lower, IRP}), "kernel mode");
  expectUnsubmitted(IRP);
  put(IRP + IRPRequestorModeOffset, KernelMode, 1);
  put(IRP + IRPStackCountOffset, 2, 1);
  rejected(Model->call("IoCallDriver", {Lower, IRP}), "allocated header");
  expectUnsubmitted(IRP);
  put(IRP + IRPStackCountOffset, 1, 1);
  const auto Dispatch = submit(IRP);
  finishInline(IRP, Dispatch);
}

TEST_F(KernelDriverIRPOwnership,
       WrongCursorAndInsufficientStackDoNotConsumePacket) {
  const auto IRP = packet();
  const auto Original = get(IRP + IRPStackPointerOffset);
  put(IRP + IRPStackPointerOffset, Original + StackSize);
  rejected(Model->call("IoCallDriver", {Lower, IRP}), "invalid cursor");
  EXPECT_TRUE(Result.Requests.empty());
  put(IRP + IRPStackPointerOffset, Original);
  rejected(Model->call("IoCallDriver", {Upper, IRP}), "stack capacity");
  expectUnsubmitted(IRP);
  const auto Dispatch = submit(IRP);
  finishInline(IRP, Dispatch);
}

TEST_F(KernelDriverIRPOwnership,
       UserBufferAndUnsupportedMajorRejectBeforeAdoption) {
  const auto IRP = packet();
  const auto Stack = nextStack(IRP);
  put(Stack, uint8_t(RequestMajor::Read), 1);
  rejected(Model->call("IoCallDriver", {Lower, IRP}),
           "internal device control");
  expectUnsubmitted(IRP);
  put(Stack, uint8_t(RequestMajor::InternalDeviceControl), 1);
  put(Stack + StackInputLengthOffset, 4, 4);
  put(Stack + StackType3InputOffset, profile::UserArenaBase);
  rejected(Model->call("IoCallDriver", {Lower, IRP}), "kernel storage");
  expectUnsubmitted(IRP);
  put(Stack + StackInputLengthOffset, 0, 4);
  put(Stack + StackType3InputOffset, 0);
  const auto Dispatch = submit(IRP);
  finishInline(IRP, Dispatch);
}

TEST_F(KernelDriverIRPOwnership,
       LowerSuffixDoesNotAdoptItsAttachedUpperDevice) {
  const auto IRP = packet();
  const auto Dispatch = submit(IRP);
  EXPECT_EQ(Dispatch.Arguments, (std::vector<uint64_t>{Lower, IRP}));
  EXPECT_EQ(observation(IRP).Origin, DriverRequestOrigin::DriverAllocatedIRP);
  rejected(Model->call("IoCallDriver", {Upper, IRP}), "retained device route");
  rejected(Model->call("IoCallDriver", {Other, IRP}), "retained device route");
  status(IRP);
  const auto Complete = complete(IRP);
  EXPECT_EQ(Complete.Arguments, (std::vector<uint64_t>{0, IRP, Context}));
  call("IoFreeIrp", {IRP});
  finish(Complete, StatusMoreProcessingRequired);
  finish(Dispatch, StatusSuccess);
}

TEST_F(KernelDriverIRPOwnership, ForwardingCannotChangeCapturedMajor) {
  const auto IRP = packet(2);
  const auto Dispatch = submit(IRP);
  const auto Next = nextStack(IRP);
  put(Next, uint8_t(RequestMajor::Read), 1);
  rejected(Model->call("IoCallDriver", {Lower, IRP}),
           "changing the request major");
  EXPECT_EQ(Result.Requests.size(), 1u);
  EXPECT_EQ(get(IRP + IRPLocationOffset, 1), 2u);
  finishInline(IRP, Dispatch);
}

TEST_F(KernelDriverIRPOwnership,
       UninitializedCompletionCanBeRepairedWithoutFree) {
  const auto IRP = packet();
  const auto Dispatch = submit(IRP);
  const auto Stack = get(IRP + IRPStackPointerOffset);
  rejected(Model->call("IofCompleteRequest", {IRP, 0}), "initialized IoStatus");
  EXPECT_FALSE(observation(IRP).Completed);
  EXPECT_EQ(get(IRP + IRPStackPointerOffset), Stack);
  EXPECT_FALSE(Model->takeGuestCall());
  rejected(Model->call("IoFreeIrp", {IRP}), "completion");
  success(Model->validateGuestAccess(IRP + IRPDriverContextOffset,
                                     sizeof(uint64_t), false));
  finishInline(IRP, Dispatch);
}

TEST_F(KernelDriverIRPOwnership,
       Windows19041InitializationSurvivesAdoptionWithoutDriverStatusStores) {
  DriverOptions Options;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  Options.IRPInitializationPolicy = DriverIRPInitializationPolicy::Windows19041;
  initialize(Options);
  const auto IRP = packet();
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), StatusSuccess);
  EXPECT_EQ(get(IRP + IRPInformationOffset), 0u);
  const auto Dispatch = submit(IRP);
  // Adoption must retain the allocator's knowledge; there are no guest
  // IoStatus stores and no second initialization when ownership changes.
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), StatusSuccess);
  EXPECT_EQ(get(IRP + IRPInformationOffset), 0u);
  const auto Complete = complete(IRP);
  call("IoFreeIrp", {IRP});
  finish(Complete, StatusMoreProcessingRequired);
  finish(Dispatch, StatusSuccess);
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(observation(IRP).IOStatus, StatusSuccess);
  EXPECT_EQ(observation(IRP).Information, 0u);
}

TEST_F(KernelDriverIRPOwnership,
       Windows19041AdoptionPreservesCallerStatusAndCompletionChecks) {
  DriverOptions Options;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  Options.IRPInitializationPolicy = DriverIRPInitializationPolicy::Windows19041;
  initialize(Options);
  const auto IRP = packet();
  put(IRP + IRPStatusOffset, StatusPending, 4);
  const auto Dispatch = submit(IRP);
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), StatusPending);
  rejected(Model->call("IofCompleteRequest", {IRP, 0}), "STATUS_PENDING");
  EXPECT_FALSE(observation(IRP).Completed);
  put(IRP + IRPStatusOffset, StatusNotSupported, 4);
  const auto Complete = complete(IRP);
  call("IoFreeIrp", {IRP});
  finish(Complete, StatusMoreProcessingRequired);
  finish(Dispatch, StatusNotSupported, StatusNotSupported);
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(observation(IRP).IOStatus, StatusNotSupported);
}

TEST_F(KernelDriverIRPOwnership,
       FailedCompletionFreePreservesEmbeddedDispatcher) {
  const auto IRP = packet();
  const auto Dispatch = submit(IRP);
  status(IRP);
  const auto Complete = complete(IRP);
  const auto Event = IRP + IRPDriverContextOffset;
  call("KeInitializeEvent", {Event, dispatcher::NotificationObject, true});
  put(IRP + IRPStatusOffset, StatusPending, 4);
  rejected(Model->call("IoFreeIrp", {IRP}), "STATUS_PENDING");
  EXPECT_FALSE(observation(IRP).Completed);
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 1u);
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), StatusPending);
  put(IRP + IRPStatusOffset, StatusSuccess, 4);
  call("IoFreeIrp", {IRP});
  finish(Complete, StatusMoreProcessingRequired);
  finish(Dispatch, StatusSuccess);
}

TEST_F(KernelDriverIRPOwnership,
       FreedCompletionMustReturnMoreProcessingRequired) {
  const auto IRP = packet();
  const auto Dispatch = submit(IRP);
  status(IRP);
  const auto Complete = complete(IRP);
  call("IoFreeIrp", {IRP});
  rejected(Model->validateGuestAccess(IRP, 1, false), "freed");
  rejected(Model->finishGuestCall(Complete.Token, StatusSuccess),
           "MORE_PROCESSING_REQUIRED");
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_FALSE(observation(IRP).DispatchStatus);
  (void)Dispatch;
}

TEST_F(KernelDriverIRPOwnership, HeldCompletionKeepsStorageUntilExplicitFree) {
  const auto IRP = packet();
  const auto Dispatch = submit(IRP);
  status(IRP);
  const auto Complete = complete(IRP);
  finish(Complete, StatusMoreProcessingRequired);
  finish(Dispatch, StatusSuccess);
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), StatusSuccess);
  EXPECT_EQ(get(IRP + IRPInformationOffset), 0u);
  Model->enterExecution(profile::CallbackStackBase);
  call("IoFreeIrp", {IRP});
  rejected(Model->validateGuestAccess(IRP, 1, false), "freed");
  rejected(Model->call("IoFreeIrp", {IRP}), "caller-allocated");
}

TEST_F(KernelDriverIRPOwnership,
       SuspendedCompletionCannotBeFreedByAnotherCall) {
  const auto IRP = packet();
  const auto Dispatch = submit(IRP);
  status(IRP);
  const auto Complete = complete(IRP);
  Model->enterExecution(profile::StackBase);
  rejected(Model->call("IoFreeIrp", {IRP}), "completion");
  EXPECT_FALSE(observation(IRP).Completed);
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), StatusSuccess);
  Model->enterExecution(profile::CallbackStackBase, profile::StackBase,
                        Complete.Token);
  call("IoFreeIrp", {IRP});
  finish(Complete, StatusMoreProcessingRequired);
  finish(Dispatch, StatusSuccess);
}

TEST_F(KernelDriverIRPOwnership,
       IntermediateCompletionRetainsPacketUntilCallerResumes) {
  constexpr uint64_t IntermediatePC = CompletionPC + 0x100;
  const auto IRP = packet(2);
  const auto UpperDispatch = submit(IRP, Upper);
  const auto Next = nextStack(IRP);
  put(Next, uint8_t(RequestMajor::InternalDeviceControl), 1);
  put(Next + StackIOControlOffset, InternalCode, 4);
  put(Next + StackCompletionOffset, IntermediatePC);
  put(Next + StackCompletionContextOffset, Context);
  put(Next + StackControlOffset, CompletionFlags, 1);
  const auto LowerDispatch = submit(IRP, Lower);
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  const auto Intermediate = callback(IntermediatePC);
  EXPECT_EQ(Intermediate.Arguments,
            (std::vector<uint64_t>{Upper, IRP, Context}));
  rejected(Model->call("IoFreeIrp", {IRP}), "allocating caller");
  finish(Intermediate, StatusMoreProcessingRequired);
  EXPECT_FALSE(observation(IRP).Completed);
  EXPECT_EQ(get(IRP + IRPLocationOffset, 1), 2u);
  rejected(Model->call("IoFreeIrp", {IRP}), "completion");
  finish(LowerDispatch, StatusSuccess);
  const auto Complete = complete(IRP);
  EXPECT_EQ(Complete.Arguments, (std::vector<uint64_t>{0, IRP, Context}));
  call("IoFreeIrp", {IRP});
  finish(Complete, StatusMoreProcessingRequired);
  finish(UpperDispatch, StatusSuccess);
  EXPECT_TRUE(observation(IRP).Completed);
  rejected(Model->validateGuestAccess(IRP, 1, false), "freed");
}

TEST_F(KernelDriverIRPOwnership,
       CompletionSnapshotsKernelOutputWithoutFreeingIt) {
  const auto IRP = packet();
  const auto Stack = nextStack(IRP);
  constexpr uint64_t Output = Scratch + 0x200;
  constexpr uint32_t OutputSize = 4;
  put(IRP + IRPUserBufferOffset, Output);
  put(Stack + StackParametersOffset, OutputSize, 4);
  const auto Dispatch = submit(IRP);
  put(Output, 0x44332211, OutputSize);
  status(IRP);
  put(IRP + IRPInformationOffset, OutputSize);
  const auto Complete = complete(IRP);
  call("IoFreeIrp", {IRP});
  finish(Complete, StatusMoreProcessingRequired);
  finish(Dispatch, StatusSuccess);
  EXPECT_EQ(observation(IRP).Output,
            (std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44}));
  EXPECT_EQ(observation(IRP).Information, OutputSize);
  EXPECT_EQ(get(Output, OutputSize), 0x44332211u);
  put(Output, 0x88776655, OutputSize);
  EXPECT_EQ(observation(IRP).Output,
            (std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44}));
}

} // namespace
} // namespace neverd::emulation
