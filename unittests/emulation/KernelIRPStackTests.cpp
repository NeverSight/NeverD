//===- KernelIRPStackTests.cpp - WDM stack ABI and completion ownership ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise WDK inline stack operations against the public internal model
/// boundary, keeping dispatch returns, completion returns and IoStatus apart.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <algorithm>
#include <initializer_list>

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelIRPStack : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t DispatchPC = 0x180001000;
  static constexpr uint64_t CompletionPC = 0x180001100;
  static constexpr uint64_t Context = Scratch + 0x4000;
  static constexpr uint32_t Pending = 0x103;
  static constexpr uint32_t Unsuccessful = 0xc0000001;
  static constexpr uint32_t Warning = 0x80000005;
  static constexpr uint8_t AllCompletion =
      StackInvokeOnSuccess | StackInvokeOnError | StackInvokeOnCancel;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t Lower = 0, Upper = 0;
  uint64_t NameStorage = Scratch + 0x100;

  void SetUp() override {
    auto Backend = UnicornBackend::create(8 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = DispatchPC;
    Image.Size = 0x3000;
    success(Model->initialize(Image, DriverOptions{}));
    Lower = createDevice("\\Device\\StackLower");
    Upper = createDevice("\\Device\\StackUpper");
    ASSERT_NE(Lower, 0u);
    ASSERT_NE(Upper, 0u);
    ASSERT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Lower}), Lower);
    for (unsigned Major : {0u, 2u, 3u, 4u, 14u, 18u})
      write(Model->driverObject() + DriverDispatchOffset + Major * 8,
            DispatchPC, 8);
    success(Model->finishEntry());
  }

  void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }

  template <class T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return T{};
    }
    return std::move(*Value);
  }

  void rejected(llvm::Error E, llvm::StringRef Text = {}) {
    ASSERT_TRUE(bool(E)) << "operation unexpectedly succeeded";
    const auto Message = llvm::toString(std::move(E));
    EXPECT_FALSE(Message.empty());
    if (!Text.empty())
      EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }

  template <class T>
  void rejected(llvm::Expected<T> Value, llvm::StringRef Text = {}) {
    ASSERT_FALSE(bool(Value)) << "operation unexpectedly succeeded";
    rejected(Value.takeError(), Text);
  }

  uint64_t call(const char *Name, std::initializer_list<uint64_t> A) {
    return take(Model->call(Name, A));
  }

  uint64_t get(uint64_t Address, unsigned Size = 8) {
    return take(Memory->readInteger(Address, Size));
  }

  void write(uint64_t Address, uint64_t Value, unsigned Size = 8) {
    if (auto E = Model->validateGuestAccess(Address, Size, true)) {
      ADD_FAILURE() << llvm::toString(std::move(E));
      return;
    }
    success(Memory->writeInteger(Address, Value, Size));
  }

  uint64_t createDevice(llvm::StringRef Name) {
    const uint64_t Record = NameStorage;
    NameStorage += 0x200;
    write(Record, Name.size() * 2, 2);
    write(Record + 2, Name.size() * 2 + 2, 2);
    write(Record + 8, Record + 0x20);
    for (size_t I = 0; I < Name.size(); ++I)
      write(Record + 0x20 + I * 2, uint8_t(Name[I]), 2);
    write(Record + 0x20 + Name.size() * 2, 0, 2);
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 16, Record,
                                      UnknownDeviceType, 0, 0, Scratch}),
              0u);
    const uint64_t Device = get(Scratch);
    write(Device + DeviceFlagsOffset, DeviceBufferedIO, 4);
    return Device;
  }

  DriverRequestResult observation(uint64_t IRP) {
    auto It = std::find_if(Result.Requests.begin(), Result.Requests.end(),
                           [IRP](const auto &R) { return R.IRP == IRP; });
    if (It == Result.Requests.end()) {
      ADD_FAILURE() << "missing IRP observation";
      return {};
    }
    return *It;
  }

  uint64_t begin(DriverRequestKind Kind, uint32_t File = 0) {
    DriverRequest Input;
    Input.Kind = Kind;
    Input.File = File;
    Input.Device = "\\Device\\StackLower";
    if (Kind == DriverRequestKind::DeviceControl) {
      Input.ControlCode = 0x222000;
      Input.Input = {0x11, 0x22};
      Input.OutputSize = 4;
    } else if (Kind == DriverRequestKind::Read) {
      Input.OutputSize = 4;
    } else if (Kind == DriverRequestKind::Write) {
      Input.Input = {0x11, 0x22};
    }
    auto Invocation = take(Model->beginRequest(Input));
    EXPECT_EQ(Invocation.PC, DispatchPC);
    EXPECT_EQ(Invocation.Argument0, Upper);
    EXPECT_EQ(Invocation.Argument1, Invocation.IRP);
    return Invocation.IRP;
  }

  void status(uint64_t IRP, uint32_t Status = 0, uint64_t Information = 0) {
    write(IRP + IRPStatusOffset, Status, 4);
    write(IRP + IRPInformationOffset, Information);
  }

  void archive(uint64_t IRP, uint32_t DispatchStatus = 0) {
    success(Model->recordDispatchReturn(IRP, DispatchStatus));
    success(Model->finalizeRequest(IRP));
  }

  void open() {
    const uint64_t Create = begin(DriverRequestKind::Create);
    status(Create);
    call("IofCompleteRequest", {Create, 0});
    EXPECT_FALSE(Model->takeGuestCall());
    archive(Create);
  }

  uint64_t io() {
    open();
    return begin(DriverRequestKind::DeviceControl);
  }

  uint64_t current(uint64_t IRP) {
    return call("IoGetCurrentIrpStackLocation", {IRP});
  }

  // These are the public WDK inline operations, not additional host APIs.
  uint64_t copyNext(uint64_t IRP) {
    const uint64_t Current = current(IRP);
    const uint64_t Next = Current - StackSize;
    std::vector<uint8_t> Bytes(StackCompletionOffset);
    success(Model->validateGuestAccess(Current, Bytes.size(), false));
    success(Memory->read(Current, Bytes));
    success(Model->validateGuestAccess(Next, Bytes.size(), true));
    success(Memory->write(Next, Bytes));
    write(Next + StackControlOffset, 0, 1);
    return Next;
  }

  void skip(uint64_t IRP) {
    write(IRP + IRPLocationOffset, get(IRP + IRPLocationOffset, 1) + 1, 1);
    write(IRP + IRPStackPointerOffset, get(IRP + IRPStackPointerOffset) + StackSize);
  }

  void completion(uint64_t Stack, uint8_t Flags = AllCompletion,
                  uint64_t PC = CompletionPC, uint64_t Cookie = Context) {
    write(Stack + StackCompletionOffset, PC);
    write(Stack + StackCompletionContextOffset, Cookie);
    write(Stack + StackControlOffset, Flags, 1);
  }

  KernelGuestCall callback(uint64_t PC) {
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->Token.Owner, GuestCallOwner::WDM);
    EXPECT_NE(Call->Token.ID, 0u);
    EXPECT_EQ(Call->PC, PC);
    EXPECT_FALSE(Model->takeGuestCall());
    return std::move(*Call);
  }

  KernelGuestCall forwardTo(uint64_t IRP, uint64_t Device) {
    EXPECT_EQ(call("IofCallDriver", {Device, IRP}), 0u);
    auto Call = callback(DispatchPC);
    EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Device, IRP}));
    return Call;
  }

  KernelGuestCall forward(uint64_t IRP) { return forwardTo(IRP, Lower); }

  void finished(const KernelGuestCall &Call, uint64_t Return,
                 uint64_t APIResult = 0) {
    const auto Result = take(Model->finishGuestCall(Call.Token, Return));
    ASSERT_TRUE(Result.has_value());
    EXPECT_EQ(*Result, APIResult);
  }
};

TEST_F(KernelIRPStack, AsynchronousFileOverlapsReadsWithoutImplicitPosition) {
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Create.Device = "\\Device\\StackLower";
  Create.AsynchronousFile = true;
  const auto Open = take(Model->beginRequest(Create));
  const uint64_t File =
      get(get(Open.IRP + IRPStackPointerOffset) + StackFileOffset);
  EXPECT_EQ(get(File + FileFlagsOffset, 4) & FileSynchronousIO, 0u);
  EXPECT_EQ(get(Open.IRP + IRPFlagsOffset, 4) & IRPSynchronous, 0u);
  status(Open.IRP);
  call("IofCompleteRequest", {Open.IRP, 0});
  archive(Open.IRP);

  DriverRequest ReadRequest;
  ReadRequest.Kind = DriverRequestKind::Read;
  ReadRequest.Device = "\\Device\\StackLower";
  ReadRequest.OutputSize = 4;
  ReadRequest.ByteOffset = 5;
  const auto First = take(Model->beginRequest(ReadRequest));
  EXPECT_EQ(get(First.IRP + IRPFlagsOffset, 4) & IRPSynchronous, 0u);
  call("IoMarkIrpPending", {First.IRP});
  success(Model->recordDispatchReturn(First.IRP, Pending));

  ReadRequest.ByteOffset = 7;
  const auto Second = take(Model->beginRequest(ReadRequest));
  EXPECT_NE(First.IRP, Second.IRP);
  EXPECT_EQ(get(Second.IRP + IRPFlagsOffset, 4) & IRPSynchronous, 0u);
  call("IoMarkIrpPending", {Second.IRP});
  success(Model->recordDispatchReturn(Second.IRP, Pending));

  status(Second.IRP, 0, 4);
  call("IofCompleteRequest", {Second.IRP, 0});
  success(Model->finalizeRequest(Second.IRP));
  EXPECT_EQ(get(File + FileCurrentByteOffset), 0u);
  status(First.IRP, 0, 4);
  call("IofCompleteRequest", {First.IRP, 0});
  success(Model->finalizeRequest(First.IRP));
  EXPECT_EQ(get(File + FileCurrentByteOffset), 0u);
}

TEST_F(KernelIRPStack, CopyUsesCountedSlotsAndPreservesTheOriginalOperation) {
  const uint64_t IRP = io();
  const uint64_t Top = IRP + IRPSize + StackSize;
  EXPECT_EQ(get(IRP + IRPStackCountOffset, 1), 2u);
  EXPECT_EQ(get(IRP + IRPLocationOffset, 1), 2u);
  EXPECT_EQ(current(IRP), Top);
  const uint64_t Next = copyNext(IRP);
  EXPECT_EQ(Next, IRP + IRPSize);
  EXPECT_EQ(get(Next + StackIOControlOffset, 4), 0x222000u);
  EXPECT_EQ(get(Next + StackInputLengthOffset, 4), 2u);
  EXPECT_EQ(get(Next + StackParametersOffset, 4), 4u);
  EXPECT_EQ(get(Next + StackControlOffset, 1), 0u);
  auto Dispatch = forward(IRP);
  EXPECT_EQ(get(IRP + IRPLocationOffset, 1), 1u);
  EXPECT_EQ(current(IRP), Next);
  EXPECT_EQ(get(Next + StackDeviceOffset), Lower);
  EXPECT_EQ(get(Top + StackDeviceOffset), Upper);
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  EXPECT_TRUE(observation(IRP).Completed);
  finished(Dispatch, 0);
  archive(IRP);
}

TEST_F(KernelIRPStack, SkipAllowsOnePastCursorWithoutAllocatingAnotherSlot) {
  const uint64_t IRP = io();
  const uint64_t Top = current(IRP);
  skip(IRP);
  EXPECT_EQ(get(IRP + IRPLocationOffset, 1), 3u);
  EXPECT_EQ(get(IRP + IRPStackPointerOffset), Top + StackSize);
  rejected(Model->call("IoGetCurrentIrpStackLocation", {IRP}), "one-past");
  // The next allocation may begin at this numeric address. Cursor bounds do
  // not grant the model pointer provenance or invalidate that live buffer.
  EXPECT_EQ(Top + StackSize, get(IRP + IRPSystemBufferOffset));
  success(Model->validateGuestAccess(Top + StackSize, 1, false));
  auto Dispatch = forward(IRP);
  EXPECT_EQ(current(IRP), Top);
  EXPECT_EQ(get(IRP + IRPLocationOffset, 1), 2u);
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  finished(Dispatch, 0);
  archive(IRP);
}

TEST_F(KernelIRPStack, CompletionPopsBeforeCallbackAndKeepsThreeResultsIndependent) {
  const uint64_t IRP = io();
  const uint64_t Top = current(IRP);
  completion(copyNext(IRP));
  auto Dispatch = forward(IRP);
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  auto Complete = callback(CompletionPC);
  EXPECT_NE(Complete.Token.ID, Dispatch.Token.ID);
  EXPECT_EQ(Complete.Arguments, (std::vector<uint64_t>{Upper, IRP, Context}));
  EXPECT_EQ(current(IRP), Top);
  EXPECT_EQ(get(IRP + IRPLocationOffset, 1), 2u);
  std::vector<uint8_t> Popped(StackSize);
  success(Memory->read(IRP + IRPSize, Popped));
  EXPECT_EQ(Popped, std::vector<uint8_t>(StackSize, 0));
  EXPECT_FALSE(observation(IRP).Completed);
  status(IRP, Unsuccessful);
  // Any completion return except MORE_PROCESSING_REQUIRED continues; this
  // value must become neither IoStatus nor the lower dispatch return.
  finished(Complete, 0xabcdef01);
  EXPECT_EQ(observation(IRP).IOStatus, Unsuccessful);
  finished(Dispatch, 0x87654321, 0x87654321);
  archive(IRP, 0x87654321);
  EXPECT_EQ(observation(IRP).DispatchStatus, 0x87654321u);
}

TEST_F(KernelIRPStack, CompletionMasksUseNTSuccessAndCancellationIsAnOrCondition) {
  struct Case {
    uint32_t Status;
    uint8_t Flags;
    bool Cancel;
    bool Invoke;
  };
  const Case Cases[] = {{0, StackInvokeOnSuccess, false, true},
                        {0, StackInvokeOnError, false, false},
                        {Warning, StackInvokeOnError, false, true},
                        {Warning, StackInvokeOnSuccess, false, false},
                        {Unsuccessful, StackInvokeOnError, false, true},
                        {Unsuccessful, StackInvokeOnCancel, false, false},
                        {0, StackInvokeOnCancel, true, true},
                        {Unsuccessful, StackInvokeOnSuccess, true, false},
                        {Unsuccessful, AllCompletion, true, true}};
  uint32_t File = 0;
  for (const auto &C : Cases) {
    SCOPED_TRACE(File);
    const uint64_t IRP = begin(DriverRequestKind::Create, File++);
    completion(copyNext(IRP), C.Flags);
    auto Dispatch = forward(IRP);
    // Inject the I/O manager's Cancel fact directly. Guest writes to this
    // field remain forbidden; this profile has no WDM cancellation producer.
    success(Memory->writeInteger(IRP + IRPCancelOffset, C.Cancel, 1));
    status(IRP, C.Status);
    call("IofCompleteRequest", {IRP, 0});
    if (C.Invoke)
      finished(callback(CompletionPC), 0);
    else
      EXPECT_FALSE(Model->takeGuestCall());
    EXPECT_TRUE(observation(IRP).Completed);
    finished(Dispatch, C.Status, C.Status);
    archive(IRP, C.Status);
  }
}

TEST_F(KernelIRPStack, MissingCompletionPropagatesPendingToTheUpperSlot) {
  const uint64_t IRP = io();
  const uint64_t Top = current(IRP);
  copyNext(IRP);
  auto Dispatch = forward(IRP);
  call("IoMarkIrpPending", {IRP});
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(get(Top + StackControlOffset, 1), 0u);
  EXPECT_EQ(get(IRP + IRPPendingOffset, 1), 1u);
  finished(Dispatch, Pending, Pending);
  archive(IRP, Pending);
}

TEST_F(KernelIRPStack, InvokedCompletionOwnsPendingPropagation) {
  const uint64_t IRP = io();
  const uint64_t Top = current(IRP);
  completion(copyNext(IRP));
  auto Dispatch = forward(IRP);
  call("IoMarkIrpPending", {IRP});
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  auto Complete = callback(CompletionPC);
  EXPECT_EQ(get(IRP + IRPPendingOffset, 1), 1u);
  EXPECT_EQ(get(Top + StackControlOffset, 1), 0u);
  call("IoMarkIrpPending", {IRP});
  EXPECT_EQ(get(Top + StackControlOffset, 1), StackPendingReturned);
  finished(Complete, 0);
  finished(Dispatch, Pending, Pending);
  archive(IRP, Pending);
}

TEST_F(KernelIRPStack, MoreProcessingRequiredRetainsPacketAndResumesAtUpperSlot) {
  const uint64_t IRP = io();
  const uint64_t Buffer = get(IRP + IRPSystemBufferOffset);
  const uint64_t Top = current(IRP);
  completion(copyNext(IRP));
  auto Dispatch = forward(IRP);
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  auto Complete = callback(CompletionPC);
  finished(Complete, StatusMoreProcessingRequired);
  EXPECT_FALSE(observation(IRP).Completed);
  success(Model->validateGuestAccess(IRP + IRPStatusOffset, 4, false));
  success(Model->validateGuestAccess(Buffer, 4, true));
  EXPECT_EQ(current(IRP), Top);
  finished(Dispatch, 0);
  write(Buffer, 0xdecafbad, 4);
  status(IRP, 0, 4);
  call("IofCompleteRequest", {IRP, 0});
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(observation(IRP).Output,
            (std::vector<uint8_t>{0xad, 0xfb, 0xca, 0xde}));
  rejected(Model->validateGuestAccess(Buffer, 1, false));
  archive(IRP);
}

TEST_F(KernelIRPStack, NestedCompletionMayRetirePacketBeforeOuterMPRReturn) {
  const uint64_t IRP = io();
  completion(copyNext(IRP));
  auto Dispatch = forward(IRP);
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  auto Outer = callback(CompletionPC);
  call("IofCompleteRequest", {IRP, 0});
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_TRUE(observation(IRP).Completed);
  rejected(Model->validateGuestAccess(IRP + IRPStatusOffset, 4, false));
  // Destroy every retired packet byte to catch accidental read-after-retire
  // in outer completion and lower dispatch continuations.
  success(Memory->write(IRP, std::vector<uint8_t>(IRPSize + 2 * StackSize, 0xee)));
  finished(Outer, StatusMoreProcessingRequired);
  finished(Dispatch, 0);
  archive(IRP);
  rejected(Model->finishGuestCall(Outer.Token, StatusMoreProcessingRequired));
}

TEST_F(KernelIRPStack, NestedCompletionRequiresOuterMoreProcessingRequired) {
  const uint64_t IRP = io();
  completion(copyNext(IRP));
  auto Dispatch = forward(IRP);
  (void)Dispatch;
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  auto Outer = callback(CompletionPC);
  call("IofCompleteRequest", {IRP, 0});
  rejected(Model->finishGuestCall(Outer.Token, 0), "retirement");
}

TEST_F(KernelIRPStack, MalformedCursorIsRejectedBeforeForwarding) {
  const uint64_t IRP = io();
  const uint64_t Top = current(IRP);
  copyNext(IRP);
  for (uint64_t Bad : {Top - 1, Top + 1, Scratch}) {
    write(IRP + IRPStackPointerOffset, Bad);
    rejected(Model->call("IofCallDriver", {Lower, IRP}), "cursor");
    EXPECT_EQ(get(IRP + IRPLocationOffset, 1), 2u);
    EXPECT_FALSE(Model->takeGuestCall());
  }
  write(IRP + IRPStackPointerOffset, Top);
  write(IRP + IRPLocationOffset, 0, 1);
  rejected(Model->call("IofCallDriver", {Lower, IRP}), "cursor");
  write(IRP + IRPLocationOffset, 2, 1);
  rejected(Model->validateGuestAccess(IRP + IRPStackCountOffset, 1, true));
  success(Memory->writeInteger(IRP + IRPStackCountOffset, 3, 1));
  rejected(Model->call("IofCallDriver", {Lower, IRP}), "cursor");
}

TEST_F(KernelIRPStack, MalformedFlagsAndNullCompletionDoNotConsumeTheNextSlot) {
  const uint64_t IRP = io();
  const uint64_t Top = current(IRP);
  const uint64_t Next = copyNext(IRP);
  for (uint8_t Invalid : {uint8_t(0x02), uint8_t(0x10)}) {
    completion(Next, Invalid);
    rejected(Model->call("IofCallDriver", {Lower, IRP}), "control flags");
    EXPECT_EQ(current(IRP), Top);
    EXPECT_FALSE(Model->takeGuestCall());
  }
  completion(Next, StackInvokeOnSuccess, 0);
  rejected(Model->call("IofCallDriver", {Lower, IRP}), "require a callback");
  EXPECT_EQ(current(IRP), Top);
  completion(Next);
  auto Dispatch = forward(IRP);
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  finished(callback(CompletionPC), 0);
  finished(Dispatch, 0);
  archive(IRP);
}

TEST_F(KernelIRPStack, PendingDescriptorCannotBeReplacedAndTokenOwnerIsChecked) {
  const uint64_t IRP = io();
  copyNext(IRP);
  call("IofCallDriver", {Lower, IRP});
  rejected(Model->call("IofCallDriver", {Lower, IRP}), "pending guest callback");
  auto Dispatch = callback(DispatchPC);
  const GuestCallToken Foreign{GuestCallOwner::Framework, Dispatch.Token.ID};
  rejected(Model->finishGuestCall(Foreign, 0), "owning model");
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  finished(Dispatch, 0);
  archive(IRP);
}

TEST_F(KernelIRPStack, ExtraReservedSlotsAreOwnedAndAllRetireTogether) {
  write(Upper + DeviceStackCountOffset, 4, 1);
  const uint64_t IRP = io();
  EXPECT_EQ(get(IRP + IRPStackCountOffset, 1), 4u);
  EXPECT_EQ(current(IRP), IRP + IRPSize + 3 * StackSize);
  success(Model->validateGuestAccess(IRP + IRPSize, 4 * StackSize, true));
  const uint64_t End = IRP + IRPSize + 4 * StackSize;
  EXPECT_EQ(End, get(IRP + IRPSystemBufferOffset));
  success(Model->validateGuestAccess(End, 1, false));
  skip(IRP);
  rejected(Model->call("IoGetCurrentIrpStackLocation", {IRP}), "one-past");
  write(IRP + IRPLocationOffset, 4, 1);
  write(IRP + IRPStackPointerOffset, End - StackSize);
  completion(copyNext(IRP));
  auto Dispatch = forward(IRP);
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  finished(callback(CompletionPC), 0);
  for (unsigned Slot = 0; Slot < 4; ++Slot)
    rejected(Model->validateGuestAccess(IRP + IRPSize + Slot * StackSize, 1,
                                       false));
  finished(Dispatch, 0);
  archive(IRP);
}

TEST_F(KernelIRPStack, RetainedRouteSurvivesDetachAndDeleteUntilDispatchFinalizes) {
  const uint64_t IRP = io();
  completion(copyNext(IRP));
  auto Dispatch = forward(IRP);
  call("IoDetachDevice", {Lower});
  call("IoDeleteDevice", {Upper});
  success(Model->validateGuestAccess(Upper, 2, false));
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  auto Complete = callback(CompletionPC);
  EXPECT_EQ(Complete.Arguments, (std::vector<uint64_t>{Upper, IRP, Context}));
  finished(Complete, 0);
  success(Model->validateGuestAccess(Upper, 2, false));
  finished(Dispatch, 0);
  success(Model->validateGuestAccess(Upper, 2, false));
  archive(IRP);
  rejected(Model->validateGuestAccess(Upper, 2, false));
}

TEST_F(KernelIRPStack, ForwardingOutsideCapturedRouteCannotStealTheRequest) {
  const uint64_t Outsider = createDevice("\\Device\\StackOutsider");
  const uint64_t IRP = io();
  const uint64_t Top = current(IRP);
  copyNext(IRP);
  rejected(Model->call("IofCallDriver", {Outsider, IRP}), "retained device route");
  EXPECT_EQ(current(IRP), Top);
  EXPECT_FALSE(Model->takeGuestCall());
}

TEST_F(KernelIRPStack, CopyIncludesTheFullReadWriteCleanupAndClosePrefix) {
  open();
  for (auto Kind : {DriverRequestKind::Read, DriverRequestKind::Write,
                    DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    const uint64_t IRP = begin(Kind);
    const uint64_t Top = current(IRP);
    std::vector<uint8_t> Before(StackCompletionOffset);
    success(Model->validateGuestAccess(Top, Before.size(), false));
    success(Memory->read(Top, Before));
    const uint64_t Next = copyNext(IRP);
    std::vector<uint8_t> Copied(StackCompletionOffset);
    success(Model->validateGuestAccess(Next, Copied.size(), false));
    success(Memory->read(Next, Copied));
    EXPECT_EQ(Copied, Before);
    auto Dispatch = forward(IRP);
    status(IRP);
    call("IofCompleteRequest", {IRP, 0});
    finished(Dispatch, 0);
    archive(IRP);
  }
}

TEST_F(KernelIRPStack, ThreeLayerPendingReturnsPrecedeCompletionPropagation) {
  const uint64_t Middle = Upper;
  Upper = createDevice("\\Device\\StackThird");
  ASSERT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Middle}), Middle);
  const uint64_t IRP = io();
  const uint64_t TopSlot = current(IRP);
  const uint64_t MiddleSlot = copyNext(IRP);
  completion(MiddleSlot, AllCompletion, CompletionPC + 0x10);
  auto MiddleDispatch = forwardTo(IRP, Middle);
  completion(copyNext(IRP));
  auto LowerDispatch = forward(IRP);
  call("IoMarkIrpPending", {IRP});
  finished(LowerDispatch, Pending, Pending);
  EXPECT_EQ(get(MiddleSlot + StackControlOffset, 1), AllCompletion);
  // The middle and top dispatch routines return the lower driver's pending
  // result before their completion routines get a chance to propagate it.
  finished(MiddleDispatch, Pending, Pending);
  success(Model->recordDispatchReturn(IRP, Pending));
  EXPECT_TRUE(Model->requestPending(IRP));
  EXPECT_EQ(get(TopSlot + StackControlOffset, 1), 0u);

  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  auto MiddleComplete = callback(CompletionPC);
  EXPECT_EQ(MiddleComplete.Arguments,
            (std::vector<uint64_t>{Middle, IRP, Context}));
  EXPECT_EQ(get(IRP + IRPPendingOffset, 1), 1u);
  call("IoMarkIrpPending", {IRP});
  EXPECT_FALSE(take(Model->finishGuestCall(MiddleComplete.Token, 0)));
  auto TopComplete = callback(CompletionPC + 0x10);
  EXPECT_EQ(TopComplete.Arguments,
            (std::vector<uint64_t>{Upper, IRP, Context}));
  EXPECT_EQ(get(IRP + IRPPendingOffset, 1), 1u);
  call("IoMarkIrpPending", {IRP});
  finished(TopComplete, 0);
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(observation(IRP).IOStatus, 0u);
  EXPECT_EQ(observation(IRP).DispatchStatus, Pending);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelIRPStack, EachCompletionMaskUsesTheStatusLeftByThePreviousCallback) {
  const uint64_t Middle = Upper;
  Upper = createDevice("\\Device\\StackThird");
  ASSERT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Middle}), Middle);
  const uint64_t IRP = io();
  completion(copyNext(IRP), StackInvokeOnError, CompletionPC + 0x10);
  auto MiddleDispatch = forwardTo(IRP, Middle);
  completion(copyNext(IRP));
  auto LowerDispatch = forward(IRP);
  status(IRP);
  call("IofCompleteRequest", {IRP, 0});
  auto MiddleComplete = callback(CompletionPC);
  status(IRP, Warning);
  EXPECT_FALSE(take(Model->finishGuestCall(MiddleComplete.Token, 0)));
  auto TopComplete = callback(CompletionPC + 0x10);
  EXPECT_EQ(TopComplete.Arguments,
            (std::vector<uint64_t>{Upper, IRP, Context}));
  EXPECT_EQ(get(IRP + IRPStatusOffset, 4), Warning);
  finished(TopComplete, 0);
  finished(LowerDispatch, 0);
  finished(MiddleDispatch, 0);
  archive(IRP);
  EXPECT_EQ(observation(IRP).IOStatus, Warning);
}

} // namespace
} // namespace neverd::emulation
