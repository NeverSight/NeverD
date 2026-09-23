//===- KernelDeviceStackTests.cpp - Device stack identity and lifetime
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check device topology through the public WDM ABI and kernel entry points.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <initializer_list>
#include <string_view>

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelDeviceStack : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    success(Model->initialize(Image, DriverOptions{}));
    for (unsigned Major : {0u, 2u, 3u, 4u, 14u, 18u}) {
      const auto Address =
          Model->driverObject() + DriverDispatchOffset + Major * 8;
      success(Model->validateGuestAccess(Address, 8, true));
      put(Address, Entry);
    }
  }

  void success(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }

  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return T{};
    }
    return std::move(*Value);
  }

  void reject(llvm::Error Error, llvm::StringRef Text) {
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find(Text.str()),
              std::string::npos);
  }

  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name, Arguments));
  }

  void rejectedCall(const char *Name, std::initializer_list<uint64_t> Arguments,
                    llvm::StringRef Text) {
    auto Value = Model->call(Name, Arguments);
    ASSERT_FALSE(bool(Value));
    reject(Value.takeError(), Text);
  }

  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    success(Memory->writeInteger(Address, Value, Width));
  }

  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }

  uint64_t device(std::string_view Name = {}, bool Exclusive = false) {
    uint64_t NameRecord = 0;
    if (!Name.empty()) {
      NameRecord = Scratch + 0x100;
      std::vector<uint8_t> Bytes((Name.size() + 1) * 2);
      for (size_t I = 0; I < Name.size(); ++I)
        Bytes[I * 2] = Name[I];
      success(Memory->write(NameRecord + 0x20, Bytes));
      put(NameRecord, Name.size() * 2, 2);
      put(NameRecord + 2, Bytes.size(), 2);
      put(NameRecord + 8, NameRecord + 0x20);
    }
    EXPECT_EQ(
        call("IoCreateDevice", {Model->driverObject(), 16, NameRecord,
                                UnknownDeviceType, 0, Exclusive, Scratch}),
        StatusSuccess);
    const auto Address = get(Scratch);
    put(Address + DeviceFlagsOffset,
        DeviceBufferedIO | (Exclusive ? DeviceExclusive : 0), 4);
    return Address;
  }

  KernelModel::Invocation begin(DriverRequestKind Kind, std::string Name = {},
                                uint64_t File = 0) {
    DriverRequest Input;
    Input.Kind = Kind;
    Input.Device = std::move(Name);
    Input.File = File;
    if (Kind == DriverRequestKind::Read)
      Input.OutputSize = 4;
    return take(Model->beginRequest(Input));
  }

  void complete(uint64_t IRP, uint32_t Status = StatusSuccess) {
    success(Model->validateGuestAccess(IRP + IRPStatusOffset, 16, true));
    put(IRP + IRPStatusOffset, Status, 4);
    put(IRP + IRPInformationOffset, 0);
    call("IofCompleteRequest", {IRP, 0});
  }

  void finalize(uint64_t IRP, uint32_t Status = StatusSuccess) {
    success(Model->recordDispatchReturn(IRP, Status));
    success(Model->finalizeRequest(IRP));
  }
};

TEST_F(KernelDeviceStack,
       AttachmentFindsActualTopWithoutChangingDriverInventory) {
  const auto Lower = device("\\Device\\StackLower");
  const auto Middle = device();
  const auto Upper = device();
  put(Lower + DeviceAlignmentOffset, 0x1ff, 4);
  put(Middle + DeviceFlagsOffset, DeviceDirectIO, 4);
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Middle, Lower}), Lower);
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Lower}), Middle);
  EXPECT_EQ(get(Lower + DeviceAttachedOffset), Middle);
  EXPECT_EQ(get(Middle + DeviceAttachedOffset), Upper);
  EXPECT_EQ(get(Upper + DeviceAttachedOffset), 0u);
  EXPECT_EQ(get(Middle + DeviceStackCountOffset, 1), 2u);
  EXPECT_EQ(get(Upper + DeviceStackCountOffset, 1), 3u);
  EXPECT_EQ(get(Upper + DeviceAlignmentOffset, 4), 0x1ffu);
  EXPECT_EQ(get(Middle + DeviceFlagsOffset, 4), DeviceDirectIO);
  EXPECT_EQ(get(Upper + DeviceFlagsOffset, 4), DeviceBufferedIO);
  EXPECT_EQ(get(Model->driverObject() + DriverDeviceHead), Upper);
  EXPECT_EQ(get(Upper + DeviceNext), Middle);
  EXPECT_EQ(get(Middle + DeviceNext), Lower);
  EXPECT_EQ(get(Lower + DeviceNext), 0u);
  success(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 3u);
  EXPECT_EQ(Result.Devices[0].Address, Upper);
  EXPECT_EQ(Result.Devices[1].Address, Middle);
  EXPECT_EQ(Result.Devices[2].Address, Lower);
  reject(Model->validateGuestAccess(Lower + DeviceAttachedOffset, 8, false),
         "unmodeled");
  reject(Model->validateGuestAccess(Lower + DeviceAttachedOffset, 8, true),
         "opaque");
}

TEST_F(KernelDeviceStack,
       DetachUsesSavedLowerAndPreservesReservedStackCapacity) {
  const auto Lower = device();
  const auto Middle = device();
  const auto Upper = device();
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Middle, Lower}), Lower);
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Lower}), Middle);
  rejectedCall("IoDetachDevice", {Upper}, "saved lower");
  rejectedCall("IoDetachDevice", {Lower}, "intermediate");
  EXPECT_EQ(get(Lower + DeviceAttachedOffset), Middle);
  EXPECT_EQ(get(Middle + DeviceAttachedOffset), Upper);
  put(Upper + DeviceStackCountOffset, 7, 1);
  success(Model->snapshot());
  call("IoDetachDevice", {Middle});
  EXPECT_EQ(get(Middle + DeviceAttachedOffset), 0u);
  EXPECT_EQ(get(Upper + DeviceStackCountOffset, 1), 7u);
  call("IoDetachDevice", {Lower});
  EXPECT_EQ(get(Lower + DeviceAttachedOffset), 0u);
  EXPECT_EQ(get(Middle + DeviceStackCountOffset, 1), 2u);
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Lower}), Lower);
  EXPECT_EQ(get(Upper + DeviceStackCountOffset, 1), 2u);
  success(Model->snapshot());
}

TEST_F(KernelDeviceStack, InvalidAttachmentAndOverflowDoNotPublishAnyEdge) {
  const auto Lower = device();
  const auto Upper = device();
  rejectedCall("IoAttachDeviceToDeviceStack", {Lower, Lower}, "itself");
  rejectedCall("IoAttachDeviceToDeviceStack", {Upper, Scratch}, "live devices");
  put(Lower + DeviceStackCountOffset, MaxIRPStackCount, 1);
  rejectedCall("IoAttachDeviceToDeviceStack", {Upper, Lower}, "capacity");
  EXPECT_EQ(get(Lower + DeviceAttachedOffset), 0u);
  EXPECT_EQ(get(Upper + DeviceStackCountOffset, 1), 1u);
  put(Lower + DeviceStackCountOffset, 1, 1);
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Lower}), Lower);
  rejectedCall("IoAttachDeviceToDeviceStack", {Upper, Lower}, "unattached");
  rejectedCall("IoAttachDeviceToDeviceStack", {Lower, Upper}, "unattached");
  EXPECT_EQ(get(Lower + DeviceAttachedOffset), Upper);
  EXPECT_EQ(get(Upper + DeviceAttachedOffset), 0u);
  success(Model->snapshot());
}

TEST_F(KernelDeviceStack, SnapshotRejectsForgedIdentityAndUndersizedStack) {
  const auto Lower = device();
  const auto Upper = device();
  call("IoAttachDeviceToDeviceStack", {Upper, Lower});
  put(Upper + DeviceStackCountOffset, 1, 1);
  reject(Model->snapshot(), "lower stack");
  put(Upper + DeviceStackCountOffset, 2, 1);
  put(Lower + DeviceAttachedOffset, 0);
  reject(Model->snapshot(), "attachment was corrupted");
  put(Lower + DeviceAttachedOffset, Upper);
  put(Upper + DeviceDriverOffset, Scratch);
  reject(Model->snapshot(), "identity");
  put(Upper + DeviceDriverOffset, Model->driverObject());
  success(Model->snapshot());
}

TEST_F(KernelDeviceStack, UnwritableLowerDoesNotPartiallyInitializeSource) {
  const auto Lower = device();
  ASSERT_NE(call("ExAllocatePoolWithTag", {512, profile::PageSize, 0x74657374}),
            0u);
  const auto Upper = device();
  const auto LowerPage = Lower & ~(profile::PageSize - 1);
  ASSERT_NE(Upper & ~(profile::PageSize - 1), LowerPage);
  put(Lower + DeviceAlignmentOffset, 0x1ff, 4);
  const auto UpperAlignment = get(Upper + DeviceAlignmentOffset, 4);
  success(Memory->protect(LowerPage, profile::PageSize, Read));
  rejectedCall("IoAttachDeviceToDeviceStack", {Upper, Lower},
               "guest write fault");
  EXPECT_EQ(get(Lower + DeviceAttachedOffset), 0u);
  EXPECT_EQ(get(Upper + DeviceStackCountOffset, 1), 1u);
  EXPECT_EQ(get(Upper + DeviceAlignmentOffset, 4), UpperAlignment);
  ASSERT_TRUE(Memory->fault());
  EXPECT_EQ(Memory->fault()->Address, Lower + DeviceAttachedOffset);
  EXPECT_EQ(Memory->fault()->Kind, BackendFaultKind::Protection);
  success(Model->snapshot());
}

TEST_F(KernelDeviceStack, DeletePendingTargetReturnsNullWithoutAttaching) {
  const auto Lower = device();
  const auto Upper = device();
  const auto Item = call("IoAllocateWorkItem", {Lower});
  call("IoQueueWorkItem", {Item, Entry, profile::DelayedWorkQueue, Scratch});
  call("IoDeleteDevice", {Lower});
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Lower}), 0u);
  EXPECT_EQ(get(Lower + DeviceAttachedOffset), 0u);
  EXPECT_EQ(get(Upper + DeviceStackCountOffset, 1), 1u);
  success(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 2u);
}

TEST_F(KernelDeviceStack, QueuedWorkAndAttachmentKeepDeletePendingLowerAlive) {
  const auto Lower = device();
  const auto Upper = device();
  call("IoAttachDeviceToDeviceStack", {Upper, Lower});
  const auto Item = call("IoAllocateWorkItem", {Lower});
  call("IoQueueWorkItem", {Item, Entry, profile::DelayedWorkQueue, Scratch});
  call("IoDeleteDevice", {Lower});
  EXPECT_EQ(get(Lower + DeviceReferenceCount, 4), 0u);
  call("IoDetachDevice", {Lower});
  success(Model->validateGuestAccess(Lower, 2, false));
  auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->Arguments, (std::vector<uint64_t>{Lower, Scratch}));
  call("IoFreeWorkItem", {Item});
  success(Model->validateGuestAccess(Lower, 2, false));
  success(Model->finishScheduled(Next->ID));
  reject(Model->validateGuestAccess(Lower, 2, false), "freed");
  success(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(Result.Devices[0].Address, Upper);
  call("IoDeleteDevice", {Upper});
  success(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
}

TEST_F(KernelDeviceStack,
       DetachedCompletedRequestRetainsEveryDeviceUntilReturn) {
  const auto Lower = device("\\Device\\StackLower");
  const auto Upper = device();
  call("IoAttachDeviceToDeviceStack", {Upper, Lower});
  const auto Invocation =
      begin(DriverRequestKind::Create, "\\Device\\StackLower");
  EXPECT_EQ(Invocation.Argument0, Upper);
  const auto IRP = Invocation.IRP;
  ASSERT_NE(IRP, 0u);
  const auto File = get(IRP + IRPOriginalFileOffset);
  EXPECT_EQ(get(File + FileDeviceOffset), Lower);
  EXPECT_EQ(Result.Requests.back().Device, "\\Device\\StackLower");
  call("IoDetachDevice", {Lower});
  call("IoDeleteDevice", {Upper});
  call("IoDeleteDevice", {Lower});
  success(Model->validateGuestAccess(Upper, 2, false));
  success(Model->validateGuestAccess(Lower, 2, false));
  constexpr uint32_t FailedCreate = 0xc0000001;
  complete(IRP, FailedCreate);
  success(Model->validateGuestAccess(Upper, 2, false));
  success(Model->validateGuestAccess(Lower, 2, false));
  finalize(IRP, FailedCreate);
  reject(Model->validateGuestAccess(Upper, 2, false), "freed");
  reject(Model->validateGuestAccess(Lower, 2, false), "freed");
  success(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
}

TEST_F(KernelDeviceStack, NamedLowerKeepsFileIdentityAndUsesTopBufferingFlags) {
  const auto Lower = device("\\Device\\StackLower");
  const auto Upper = device();
  put(Lower + DeviceFlagsOffset, DeviceDirectIO, 4);
  call("IoAttachDeviceToDeviceStack", {Upper, Lower});
  const auto Create = begin(DriverRequestKind::Create, "\\Device\\StackLower");
  EXPECT_EQ(Create.Argument0, Upper);
  complete(Create.IRP);
  finalize(Create.IRP);
  const auto Read = begin(DriverRequestKind::Read);
  EXPECT_EQ(Read.Argument0, Upper);
  EXPECT_EQ(get(Read.IRP + IRPMdlOffset), 0u);
  EXPECT_NE(get(Read.IRP + IRPSystemBufferOffset), 0u);
  EXPECT_EQ(get(get(Read.IRP + IRPOriginalFileOffset) + FileDeviceOffset),
            Lower);
  EXPECT_EQ(get(Lower + DeviceReferenceCount, 4), 1u);
  EXPECT_EQ(get(Upper + DeviceReferenceCount, 4), 0u);
  complete(Read.IRP);
  finalize(Read.IRP);
  const auto Extra = device();
  rejectedCall("IoAttachDeviceToDeviceStack", {Extra, Lower}, "live files");
  EXPECT_EQ(get(Upper + DeviceAttachedOffset), 0u);
  for (auto Kind : {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    const auto Call = begin(Kind);
    complete(Call.IRP);
    finalize(Call.IRP);
  }
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Extra, Lower}), Upper);
}

TEST_F(KernelDeviceStack, ExclusiveOpenUsesTheNamedObjectInADeviceStack) {
  const auto Lower = device("\\Device\\ExclusiveLower", true);
  const auto Upper = device();
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Lower}), Lower);
  const auto First =
      begin(DriverRequestKind::Create, "\\Device\\ExclusiveLower", 1);
  EXPECT_EQ(First.Argument0, Upper);
  complete(First.IRP);
  finalize(First.IRP);

  DriverRequest Second;
  Second.Kind = DriverRequestKind::Create;
  Second.Device = "\\Device\\ExclusiveLower";
  Second.File = 2;
  auto Rejected = Model->beginRequest(Second);
  ASSERT_FALSE(bool(Rejected));
  reject(Rejected.takeError(), "exclusive device already has a live file");

  for (auto Kind : {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    const auto Request = begin(Kind, {}, 1);
    complete(Request.IRP);
    finalize(Request.IRP);
  }
  const auto Reopened =
      begin(DriverRequestKind::Create, "\\Device\\ExclusiveLower", 2);
  complete(Reopened.IRP);
  finalize(Reopened.IRP);
  for (auto Kind : {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    const auto Request = begin(Kind, {}, 2);
    complete(Request.IRP);
    finalize(Request.IRP);
  }
}

TEST_F(KernelDeviceStack, ExclusiveUpperDoesNotRestrictItsNamedLower) {
  const auto Lower = device("\\Device\\SharedLower");
  const auto Upper = device({}, true);
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Lower}), Lower);
  for (uint64_t File : {1u, 2u}) {
    const auto Create =
        begin(DriverRequestKind::Create, "\\Device\\SharedLower", File);
    EXPECT_EQ(Create.Argument0, Upper);
    complete(Create.IRP);
    finalize(Create.IRP);
  }
  for (uint64_t File : {1u, 2u})
    for (auto Kind : {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
      const auto Request = begin(Kind, {}, File);
      complete(Request.IRP);
      finalize(Request.IRP);
    }
}

TEST_F(KernelDeviceStack,
       DetachRequiresPassiveIRQLAndPreservesTopologyOnFailure) {
  const auto Lower = device();
  const auto Upper = device();
  call("IoAttachDeviceToDeviceStack", {Upper, Lower});
  const auto Dpc = Scratch + 0x800;
  call("KeInitializeDpc", {Dpc, Entry, Scratch});
  EXPECT_EQ(call("KeInsertQueueDpc", {Dpc, 0, 0}), 1u);
  auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Model->currentIRQL(), 2u);
  rejectedCall("IoDetachDevice", {Lower}, "IRQL");
  EXPECT_EQ(get(Lower + DeviceAttachedOffset), Upper);
  success(Model->finishScheduled(Next->ID));
  call("IoDetachDevice", {Lower});
  EXPECT_EQ(get(Lower + DeviceAttachedOffset), 0u);
}
} // namespace
} // namespace neverd::emulation
