//===- KernelRequestOwnershipTests.cpp - Independent WDM IRP lifetimes ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise explicit IRP identity through the kernel model's public internal
/// interface, including dispatch return, completion, buffers and file state.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelException.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <algorithm>
#include <initializer_list>

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelRequestOwnership : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  static constexpr uint32_t Pending = 0x103;
  static constexpr uint32_t Unsuccessful = 0xc0000001;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t Device = 0;

  void SetUp() override {
    auto Backend = UnicornBackend::create(8 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    success(Model->initialize(Image, DriverOptions{}));
    ASSERT_EQ(call("IoCreateDevice", {Model->driverObject(), 0, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              0u);
    Device = integer(Scratch);
    ASSERT_NE(Device, 0u);
    guestWrite(Device + DeviceFlagsOffset, DeviceBufferedIO, 4);
    for (unsigned Major : {0u, 2u, 3u, 4u, 14u, 18u})
      guestWrite(Model->driverObject() + DriverDispatchOffset + Major * 8,
                 Entry, 8);
    success(Model->finishEntry());
  }

  void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }

  void rejected(llvm::Error E) {
    ASSERT_TRUE(bool(E)) << "operation unexpectedly succeeded";
    EXPECT_FALSE(llvm::toString(std::move(E)).empty());
  }

  template <class T> void rejected(llvm::Expected<T> Value) {
    ASSERT_FALSE(bool(Value)) << "operation unexpectedly succeeded";
    EXPECT_FALSE(llvm::toString(Value.takeError()).empty());
  }

  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    auto Value = Model->call(Name, Arguments);
    if (!Value) {
      ADD_FAILURE() << Name << ": " << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }

  uint64_t integer(uint64_t Address, unsigned Size = 8) {
    auto Value = Memory->readInteger(Address, Size);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }

  void guestWrite(uint64_t Address, uint64_t Value, unsigned Size) {
    // CPU writes are admitted through this hook before reaching guest memory.
    // In particular this records exactly which IRP IoStatus bytes were set.
    if (auto E = Model->validateGuestAccess(Address, Size, true)) {
      ADD_FAILURE() << llvm::toString(std::move(E));
      return;
    }
    success(Memory->writeInteger(Address, Value, Size));
  }

  void guestBytes(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes) {
    if (auto E = Model->validateGuestAccess(Address, Bytes.size(), true)) {
      ADD_FAILURE() << llvm::toString(std::move(E));
      return;
    }
    success(Memory->write(Address, Bytes));
  }

  std::vector<uint8_t> bytes(uint64_t Address, size_t Size) {
    std::vector<uint8_t> Bytes(Size);
    success(Model->validateGuestAccess(Address, Size, false));
    success(Memory->read(Address, Bytes));
    return Bytes;
  }

  DriverRequestResult observation(uint64_t IRP) {
    auto It = std::find_if(Result.Requests.begin(), Result.Requests.end(),
                           [IRP](const auto &R) { return R.IRP == IRP; });
    if (It == Result.Requests.end()) {
      ADD_FAILURE() << "missing request observation for " << IRP;
      return {};
    }
    return *It;
  }

  DriverRequest fileRequest(DriverRequestKind Kind, uint32_t File = 0) {
    DriverRequest Input;
    Input.Kind = Kind;
    Input.File = File;
    return Input;
  }

  DriverRequest ioRequest(uint32_t File = 0, uint32_t Method = 0) {
    auto Input = fileRequest(DriverRequestKind::DeviceControl, File);
    Input.ControlCode = 0x222000 | Method;
    Input.Input = {0x11, 0x22};
    Input.OutputSize = 4;
    if (Method)
      Input.DirectInput = {0x31, 0x32, 0x33, 0x34};
    return Input;
  }

  uint64_t begin(const DriverRequest &Input) {
    auto Invocation = Model->beginRequest(Input);
    if (!Invocation) {
      ADD_FAILURE() << llvm::toString(Invocation.takeError());
      return 0;
    }
    EXPECT_EQ(Invocation->PC, Entry);
    EXPECT_EQ(Invocation->Argument0, Device);
    EXPECT_EQ(Invocation->IRP, Invocation->Argument1);
    return Invocation->IRP;
  }

  void writeStatus(uint64_t IRP, uint32_t Status, uint64_t Information) {
    guestWrite(IRP + IRPStatusOffset, Status, 4);
    guestWrite(IRP + IRPInformationOffset, Information, 8);
  }

  void complete(uint64_t IRP, uint32_t Status = 0, uint64_t Information = 0) {
    writeStatus(IRP, Status, Information);
    call("IofCompleteRequest", {IRP, 0});
    EXPECT_TRUE(observation(IRP).Completed);
  }

  void returnPending(uint64_t IRP) {
    call("IoMarkIrpPending", {IRP});
    success(Model->recordDispatchReturn(IRP, Pending));
    EXPECT_EQ(observation(IRP).DispatchStatus, Pending);
    EXPECT_TRUE(Model->requestPending(IRP));
    EXPECT_TRUE(Model->requestPending());
  }

  uint64_t open(uint32_t File = 0) {
    const uint64_t IRP = begin(fileRequest(DriverRequestKind::Create, File));
    if (!IRP)
      return 0;
    const uint64_t Address = integer(IRP + IRPOriginalFileOffset);
    complete(IRP);
    success(Model->recordDispatchReturn(IRP, 0));
    success(Model->finalizeRequest(IRP));
    return Address;
  }

  void close(uint32_t File = 0) {
    for (auto Kind : {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
      const uint64_t IRP = begin(fileRequest(Kind, File));
      ASSERT_NE(IRP, 0u);
      complete(IRP);
      success(Model->recordDispatchReturn(IRP, 0));
      success(Model->finalizeRequest(IRP));
    }
  }

  uint64_t outputBuffer(uint64_t IRP, uint32_t Method) {
    if (!Method)
      return integer(IRP + IRPSystemBufferOffset);
    const uint64_t MDL = integer(IRP + IRPMdlOffset);
    return call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  }
};

TEST_F(KernelRequestOwnership,
       PendingBufferedAndDirectRequestsCompleteIndependentlyOutOfOrder) {
  ASSERT_NE(open(1), 0u);
  ASSERT_NE(open(2), 0u);
  for (uint32_t FirstMethod : {0u, 2u}) {
    for (uint32_t SecondMethod : {0u, 2u}) {
      SCOPED_TRACE(FirstMethod);
      SCOPED_TRACE(SecondMethod);
      const uint64_t First = begin(ioRequest(1, FirstMethod));
      ASSERT_NE(First, 0u);
      returnPending(First);
      const uint64_t Second = begin(ioRequest(2, SecondMethod));
      ASSERT_NE(Second, 0u);
      returnPending(Second);
      ASSERT_NE(First, Second);
      const uint64_t FirstBuffer = outputBuffer(First, FirstMethod);
      const uint64_t SecondBuffer = outputBuffer(Second, SecondMethod);
      ASSERT_NE(FirstBuffer, 0u);
      ASSERT_NE(SecondBuffer, 0u);
      ASSERT_NE(FirstBuffer, SecondBuffer);
      guestBytes(FirstBuffer, {0xa1, 0xa2, 0xa3});
      guestBytes(SecondBuffer, {0xb1, 0xb2});

      complete(Second, 0, 2);
      success(Model->finalizeRequest(Second));
      EXPECT_FALSE(Model->requestPending(Second));
      EXPECT_TRUE(Model->requestPending(First));
      EXPECT_TRUE(Model->requestPending());
      EXPECT_EQ(observation(Second).Output, (std::vector<uint8_t>{0xb1, 0xb2}));
      EXPECT_FALSE(observation(First).Completed);
      EXPECT_FALSE(observation(First).IOStatus);
      EXPECT_EQ(bytes(FirstBuffer, 3),
                (std::vector<uint8_t>{0xa1, 0xa2, 0xa3}));
      rejected(Model->validateGuestAccess(Second, 1, false));
      rejected(Model->validateGuestAccess(SecondBuffer, 1, false));

      complete(First, 0, 3);
      success(Model->finalizeRequest(First));
      EXPECT_FALSE(Model->requestPending(First));
      EXPECT_FALSE(Model->requestPending());
      EXPECT_EQ(observation(First).Output,
                (std::vector<uint8_t>{0xa1, 0xa2, 0xa3}));
      EXPECT_EQ(observation(Second).Information, 2u);
      success(Model->finalizeRequest(Second));
    }
  }
  close(1);
  close(2);
}

TEST_F(KernelRequestOwnership,
       DirectMDLMapAndUnmapStayWithTheirOriginalPendingIRP) {
  ASSERT_NE(open(1), 0u);
  ASSERT_NE(open(2), 0u);
  const uint64_t First = begin(ioRequest(1, 2));
  ASSERT_NE(First, 0u);
  returnPending(First);
  const uint64_t FirstMDL = integer(First + IRPMdlOffset);
  const uint64_t FirstBuffer = outputBuffer(First, 2);
  const uint64_t Second = begin(ioRequest(2, 1));
  ASSERT_NE(Second, 0u);
  returnPending(Second);
  const uint64_t SecondMDL = integer(Second + IRPMdlOffset);
  const uint64_t SecondBuffer = outputBuffer(Second, 1);
  ASSERT_NE(FirstMDL, SecondMDL);
  ASSERT_NE(FirstBuffer, SecondBuffer);
  EXPECT_EQ(
      call("MmGetSystemAddressForMdlSafe", {FirstMDL, NormalPagePriority}),
      FirstBuffer);
  EXPECT_EQ(bytes(SecondBuffer, 4),
            (std::vector<uint8_t>{0x31, 0x32, 0x33, 0x34}));

  call("MmUnmapLockedPages", {FirstBuffer, FirstMDL});
  rejected(Model->validateGuestAccess(FirstBuffer, 1, false));
  EXPECT_EQ(bytes(SecondBuffer, 4),
            (std::vector<uint8_t>{0x31, 0x32, 0x33, 0x34}));
  const uint64_t Remapped = outputBuffer(First, 2);
  ASSERT_NE(Remapped, 0u);
  guestBytes(Remapped, {0xd1, 0xd2});
  guestBytes(SecondBuffer, {0xe1, 0xe2, 0xe3});
  complete(First, 0, 2);
  success(Model->finalizeRequest(First));
  rejected(Model->call("MmGetSystemAddressForMdlSafe",
                       {FirstMDL, NormalPagePriority}));
  rejected(Model->call("MmUnmapLockedPages", {SecondBuffer, FirstMDL}));
  EXPECT_EQ(
      call("MmGetSystemAddressForMdlSafe", {SecondMDL, NormalPagePriority}),
      SecondBuffer);
  complete(Second, 0, 3);
  success(Model->finalizeRequest(Second));
  EXPECT_EQ(observation(First).Output, (std::vector<uint8_t>{0xd1, 0xd2}));
  EXPECT_EQ(observation(Second).Output,
            (std::vector<uint8_t>{0xe1, 0xe2, 0xe3}));
  close(1);
  close(2);
}

TEST_F(KernelRequestOwnership,
       UnknownAndRetiredIRPsCannotMutateAnotherLiveRequest) {
  ASSERT_NE(open(), 0u);
  const uint64_t Retired = begin(ioRequest());
  ASSERT_NE(Retired, 0u);
  complete(Retired);
  success(Model->recordDispatchReturn(Retired, 0));
  success(Model->finalizeRequest(Retired));
  const uint64_t Live = begin(ioRequest());
  ASSERT_NE(Live, 0u);
  returnPending(Live);

  for (uint64_t Invalid : {Scratch + 0x800, Retired}) {
    SCOPED_TRACE(Invalid);
    rejected(Model->call("IoMarkIrpPending", {Invalid}));
    rejected(Model->call("IofCompleteRequest", {Invalid, 0}));
  }
  rejected(Model->recordDispatchReturn(Scratch + 0x800, 0));
  rejected(Model->finalizeRequest(Scratch + 0x800));
  EXPECT_FALSE(Model->requestPending(Scratch + 0x800));
  success(Model->finalizeRequest(Retired));
  EXPECT_FALSE(observation(Live).Completed);
  EXPECT_FALSE(observation(Live).IOStatus);
  EXPECT_EQ(observation(Live).DispatchStatus, Pending);
  rejected(Model->validateGuestAccess(Live + IRPStatusOffset, 4, false));
  rejected(Model->validateGuestAccess(Live + IRPInformationOffset, 8, false));
  complete(Live, Unsuccessful);
  success(Model->finalizeRequest(Live));
  EXPECT_EQ(observation(Live).IOStatus, Unsuccessful);
  EXPECT_EQ(observation(Retired).IOStatus, 0u);
  close();
}

TEST_F(KernelRequestOwnership, IoStatusInitializationIsTrackedPerIRPAndByte) {
  ASSERT_NE(open(1), 0u);
  ASSERT_NE(open(2), 0u);
  const uint64_t First = begin(ioRequest(1));
  ASSERT_NE(First, 0u);
  returnPending(First);
  const uint64_t Second = begin(ioRequest(2));
  ASSERT_NE(Second, 0u);
  returnPending(Second);
  guestWrite(First + IRPStatusOffset, 0, 2);
  guestWrite(First + IRPStatusOffset + 2, 0, 2);
  guestWrite(Second + IRPInformationOffset, 0, 8);
  success(Model->validateGuestAccess(First + IRPStatusOffset, 4, false));
  success(Model->validateGuestAccess(Second + IRPInformationOffset, 8, false));
  rejected(Model->validateGuestAccess(First + IRPInformationOffset, 8, false));
  rejected(Model->validateGuestAccess(Second + IRPStatusOffset, 4, false));
  rejected(Model->call("IofCompleteRequest", {First, 0}));
  rejected(Model->call("IofCompleteRequest", {Second, 0}));
  EXPECT_FALSE(observation(First).Completed);
  EXPECT_FALSE(observation(Second).Completed);

  guestWrite(First + IRPInformationOffset, 0, 8);
  call("IofCompleteRequest", {First, 0});
  success(Model->finalizeRequest(First));
  rejected(Model->validateGuestAccess(Second + IRPStatusOffset, 4, false));
  guestWrite(Second + IRPStatusOffset, Unsuccessful, 4);
  call("IofCompleteRequest", {Second, 0});
  success(Model->finalizeRequest(Second));
  EXPECT_EQ(observation(First).IOStatus, 0u);
  EXPECT_EQ(observation(Second).IOStatus, Unsuccessful);
  close(1);
  close(2);
}

TEST_F(KernelRequestOwnership,
       SynchronousFileRejectsNewRequestsUntilPriorRequestIsArchived) {
  ASSERT_NE(open(), 0u);
  auto ExpectFileBusy = [&] {
    rejected(Model->beginRequest(ioRequest()));
    auto Read = fileRequest(DriverRequestKind::Read);
    Read.OutputSize = 4;
    rejected(Model->beginRequest(Read));
    auto Write = fileRequest(DriverRequestKind::Write);
    Write.Input = {1, 2, 3, 4};
    rejected(Model->beginRequest(Write));
    rejected(Model->beginRequest(fileRequest(DriverRequestKind::Cleanup)));
    rejected(Model->beginRequest(fileRequest(DriverRequestKind::Close)));
  };

  const uint64_t First = begin(ioRequest());
  ASSERT_NE(First, 0u);
  returnPending(First);
  ExpectFileBusy();
  complete(First);
  ExpectFileBusy();
  success(Model->finalizeRequest(First));

  // Archival, not just completion, permits another I/O on this synchronous
  // FILE_OBJECT. Other independently opened files are tested separately.
  const uint64_t Second = begin(ioRequest());
  ASSERT_NE(Second, 0u);
  complete(Second);
  success(Model->recordDispatchReturn(Second, 0));
  success(Model->finalizeRequest(Second));

  const uint64_t Cleanup = begin(fileRequest(DriverRequestKind::Cleanup));
  ASSERT_NE(Cleanup, 0u);
  returnPending(Cleanup);
  ExpectFileBusy();
  complete(Cleanup);
  ExpectFileBusy();
  success(Model->finalizeRequest(Cleanup));
  const uint64_t Close = begin(fileRequest(DriverRequestKind::Close));
  ASSERT_NE(Close, 0u);
  complete(Close);
  success(Model->recordDispatchReturn(Close, 0));
  success(Model->finalizeRequest(Close));
}

TEST_F(KernelRequestOwnership,
       AnotherFileCanCleanupAndCloseWhileTheFirstHasPendingIO) {
  const uint64_t FirstFile = open(1);
  ASSERT_NE(FirstFile, 0u);
  const uint64_t SecondFile = open(2);
  ASSERT_NE(SecondFile, 0u);
  const uint64_t PendingIRP = begin(ioRequest(1));
  ASSERT_NE(PendingIRP, 0u);
  returnPending(PendingIRP);
  close(2);
  EXPECT_FALSE(observation(PendingIRP).Completed);
  success(Model->validateGuestAccess(FirstFile, 2, false));
  rejected(Model->validateGuestAccess(SecondFile, 2, false));
  complete(PendingIRP);
  success(Model->finalizeRequest(PendingIRP));
  close(1);
}

TEST_F(KernelRequestOwnership, FailedCreateCannotRetireAnotherFilesContext) {
  const uint64_t FirstFile = open(1);
  ASSERT_NE(FirstFile, 0u);
  guestWrite(FirstFile + FileContextOffset, 0x123456789abcdef0, 8);
  const uint64_t Failed = begin(fileRequest(DriverRequestKind::Create, 2));
  ASSERT_NE(Failed, 0u);
  const uint64_t FailedFile = integer(Failed + IRPOriginalFileOffset);
  returnPending(Failed);
  const uint64_t OtherIO = begin(ioRequest(1));
  ASSERT_NE(OtherIO, 0u);
  returnPending(OtherIO);
  EXPECT_EQ(integer(OtherIO + IRPOriginalFileOffset), FirstFile);
  complete(Failed, Unsuccessful);
  success(Model->finalizeRequest(Failed));
  rejected(Model->validateGuestAccess(FailedFile, 2, false));
  success(Model->validateGuestAccess(FirstFile + FileContextOffset, 8, false));
  EXPECT_EQ(integer(FirstFile + FileContextOffset), 0x123456789abcdef0u);
  EXPECT_FALSE(observation(OtherIO).Completed);
  rejected(Model->beginRequest(ioRequest(2)));
  complete(OtherIO);
  success(Model->finalizeRequest(OtherIO));
  ASSERT_NE(open(2), 0u);
  close(2);
  close(1);
}

TEST_F(KernelRequestOwnership, FailedCreatePreservesAnotherOpeningFile) {
  const uint64_t First = begin(fileRequest(DriverRequestKind::Create, 1));
  ASSERT_NE(First, 0u);
  returnPending(First);
  const uint64_t Second = begin(fileRequest(DriverRequestKind::Create, 2));
  ASSERT_NE(Second, 0u);
  const uint64_t SecondFile = integer(Second + IRPOriginalFileOffset);
  returnPending(Second);
  complete(First, Unsuccessful);
  success(Model->finalizeRequest(First));
  success(Model->validateGuestAccess(SecondFile, 2, false));
  EXPECT_FALSE(observation(Second).Completed);
  rejected(Model->beginRequest(ioRequest(2)));
  complete(Second);
  success(Model->finalizeRequest(Second));
  const uint64_t IO = begin(ioRequest(2));
  ASSERT_NE(IO, 0u);
  EXPECT_EQ(integer(IO + IRPOriginalFileOffset), SecondFile);
  complete(IO);
  success(Model->recordDispatchReturn(IO, 0));
  success(Model->finalizeRequest(IO));
  close(2);
}

TEST_F(KernelRequestOwnership,
       UnreturnedDispatchBlocksOtherFilesUntilItsReturnIsRecorded) {
  ASSERT_NE(open(1), 0u);
  ASSERT_NE(open(2), 0u);
  const uint64_t First = begin(ioRequest(1));
  ASSERT_NE(First, 0u);
  rejected(Model->beginRequest(ioRequest(2)));
  returnPending(First);
  const uint64_t Second = begin(ioRequest(2));
  ASSERT_NE(Second, 0u);
  returnPending(Second);
  complete(First);
  success(Model->finalizeRequest(First));
  complete(Second);
  success(Model->finalizeRequest(Second));
  close(1);
  close(2);
}

TEST_F(KernelRequestOwnership,
       FinalizationRequiresCompletionAndARecordedDispatchReturn) {
  ASSERT_NE(open(), 0u);
  const uint64_t First = begin(ioRequest());
  ASSERT_NE(First, 0u);
  rejected(Model->finalizeRequest(First));
  complete(First);
  rejected(Model->finalizeRequest(First));
  success(Model->recordDispatchReturn(First, 0));
  success(Model->finalizeRequest(First));
  success(Model->finalizeRequest(First));

  const uint64_t Second = begin(ioRequest());
  ASSERT_NE(Second, 0u);
  returnPending(Second);
  rejected(Model->finalizeRequest(Second));
  complete(Second);
  success(Model->finalizeRequest(Second));
  rejected(Model->finalizeRequest(Scratch + 0x900));
  close();
}

TEST_F(KernelRequestOwnership,
       NeitherProbesAndUserLocksKeepSeparatePacketAndRAMOwnership) {
  ASSERT_NE(open(), 0u);
  auto Input = ioRequest(0, MethodNeither);
  Input.DirectInput.clear();
  const uint64_t IRP = begin(Input);
  ASSERT_NE(IRP, 0u);
  Model->enterExecution(profile::StackBase);
  Model->setUserRequestContext(true);
  const uint64_t Stack = integer(IRP + IRPStackPointerOffset);
  const uint64_t UserInput = integer(Stack + StackType3InputOffset);
  const uint64_t UserOutput = integer(IRP + IRPUserBufferOffset);
  ASSERT_NE(UserInput, 0u);
  ASSERT_NE(UserOutput, 0u);
  EXPECT_LT(UserInput, profile::UserProbeLimit);
  EXPECT_LT(UserOutput, profile::UserProbeLimit);
  EXPECT_EQ(integer(IRP + IRPMdlOffset), 0u);
  EXPECT_EQ(integer(IRP + IRPSystemBufferOffset), 0u);
  EXPECT_EQ(integer(IRP + IRPRequestorModeOffset, 1), UserMode);
  EXPECT_EQ(integer(IRP + IRPFlagsOffset, 4) &
                (IRPBufferedAllocation | IRPCopyOutput),
            0u);
  EXPECT_EQ(integer(UserInput, 2), 0x2211u);

  auto raised = [&](llvm::Expected<uint64_t> Value) {
    EXPECT_FALSE(bool(Value));
    if (Value)
      return uint32_t(0);
    uint32_t Code = 0;
    llvm::handleAllErrors(
        Value.takeError(),
        [&](const KernelGuestException &E) { Code = E.code(); },
        [&](const llvm::ErrorInfoBase &E) {
          ADD_FAILURE() << "unexpected model error: " << E.message();
        });
    return Code;
  };
  EXPECT_EQ(call("ProbeForRead", {0x20000000, 1, 1}), 0u);
  EXPECT_EQ(call("ProbeForRead", {0x70000001, 0, 0}), 0u);
  EXPECT_EQ(raised(Model->call("ProbeForRead", {0x20000001, 1, 2})),
            exceptions::StatusDatatypeMisalignment);
  EXPECT_EQ(raised(Model->call("ProbeForRead", {0x70000000, 1, 1})),
            exceptions::StatusAccessViolation);
  EXPECT_EQ(raised(Model->call("ProbeForWrite", {0x20000000, 1, 1})),
            exceptions::StatusAccessViolation);
  EXPECT_EQ(raised(Model->call("memcpy", {Scratch, 0x20000000, 1})),
            exceptions::StatusAccessViolation);
  success(Memory->protect(UserOutput, profile::PageSize, Read));
  EXPECT_EQ(raised(Model->call("ProbeForWrite", {UserOutput, 1, 1})),
            exceptions::StatusAccessViolation);
  const uint64_t ReadOnlyMdl =
      call("IoAllocateMdl", {UserOutput, 1, 0, 0, 0});
  ASSERT_NE(ReadOnlyMdl, 0u);
  EXPECT_EQ(raised(Model->call("MmProbeAndLockPages",
                               {ReadOnlyMdl, UserMode, IoWriteAccess})),
            exceptions::StatusAccessViolation);
  call("IoFreeMdl", {ReadOnlyMdl});
  success(Memory->protect(UserOutput, profile::PageSize, Read | Write));
  EXPECT_FALSE(Memory->fault());

  const uint64_t InMdl = call("IoAllocateMdl", {UserInput, 2, 0, 0, 0});
  ASSERT_NE(InMdl, 0u);
  call("MmProbeAndLockPages", {InMdl, UserMode, IoReadAccess});
  rejected(Model->call("MmProbeAndLockPages",
                       {InMdl, UserMode, IoReadAccess}));
  const uint64_t InAlias =
      call("MmGetSystemAddressForMdlSafe", {InMdl, NormalPagePriority});
  ASSERT_NE(InAlias, 0u);
  EXPECT_NE(InAlias, UserInput);
  EXPECT_EQ(integer(InAlias, 2), 0x2211u);
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {InMdl, NormalPagePriority}),
            InAlias);
  const uint64_t SecondInMdl =
      call("IoAllocateMdl", {UserInput, 2, 0, 0, 0});
  ASSERT_NE(SecondInMdl, 0u);
  call("MmProbeAndLockPages", {SecondInMdl, UserMode, IoReadAccess});

  const uint64_t OutMdl = call("IoAllocateMdl", {UserOutput, 1, 0, 0, 0});
  ASSERT_NE(OutMdl, 0u);
  call("MmProbeAndLockPages", {OutMdl, UserMode, IoWriteAccess});
  const uint64_t OutAlias =
      call("MmGetSystemAddressForMdlSafe", {OutMdl, NormalPagePriority});
  ASSERT_NE(OutAlias, 0u);
  guestWrite(OutAlias, 0x42, 1);
  EXPECT_EQ(integer(UserOutput, 1), 0x42u);
  Model->setUserRequestContext(false);
  rejected(Model->validateGuestAccess(UserInput, 1, false));
  rejected(Model->validateGuestAccess(UserOutput, 1, true));
  success(Model->validateGuestAccess(InAlias, 1, false));
  success(Model->validateGuestAccess(OutAlias, 1, true));
  Model->setUserRequestContext(true);
  rejected(Model->call("IoFreeMdl", {OutMdl}));
  call("MmUnlockPages", {OutMdl});
  call("MmUnlockPages", {InMdl});
  call("MmUnlockPages", {SecondInMdl});
  auto OutReadable = Memory->canAccess(OutAlias, 1, Read);
  auto InReadable = Memory->canAccess(InAlias, 1, Read);
  ASSERT_TRUE(bool(OutReadable)) << llvm::toString(OutReadable.takeError());
  ASSERT_TRUE(bool(InReadable)) << llvm::toString(InReadable.takeError());
  EXPECT_FALSE(*OutReadable);
  EXPECT_FALSE(*InReadable);
  call("IoFreeMdl", {OutMdl});
  call("IoFreeMdl", {InMdl});
  call("IoFreeMdl", {SecondInMdl});
  complete(IRP, 0, 1);
  EXPECT_EQ(observation(IRP).Output, (std::vector<uint8_t>{0x42}));
  success(Model->recordDispatchReturn(IRP, 0));
  success(Model->finalizeRequest(IRP));
  Model->setUserRequestContext(false);
  close();
}

} // namespace
} // namespace neverd::emulation
