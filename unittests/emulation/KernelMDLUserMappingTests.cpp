//===- KernelMDLUserMappingTests.cpp - Process-owned MDL views -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelException.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <array>
#include <initializer_list>

namespace neverd::emulation {
namespace {
using namespace windows;

class AliasBudgetMemory : public GuestMemory {
public:
  explicit AliasBudgetMemory(GuestMemory &Storage) : Storage(Storage) {}
  bool Exhausted = false;
  llvm::Error map(uint64_t Address, uint64_t Size,
                  unsigned Permissions) override {
    return Storage.map(Address, Size, Permissions);
  }
  llvm::Error mapAlias(uint64_t Address, uint64_t Source, uint64_t Size,
                       unsigned Permissions) override {
    if (Exhausted)
      return llvm::make_error<GuestMemoryLimitError>();
    return Storage.mapAlias(Address, Source, Size, Permissions);
  }
  llvm::Error unmapAlias(uint64_t Address, uint64_t Size) override {
    return Storage.unmapAlias(Address, Size);
  }
  llvm::Error
  replaceAliases(llvm::ArrayRef<GuestAliasRange> Removing,
                 llvm::ArrayRef<GuestAliasMapping> Adding) override {
    if (Exhausted && !Adding.empty())
      return llvm::make_error<GuestMemoryLimitError>();
    return Storage.replaceAliases(Removing, Adding);
  }
  llvm::Error protect(uint64_t Address, uint64_t Size,
                      unsigned Permissions) override {
    return Storage.protect(Address, Size, Permissions);
  }
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) override {
    return Storage.read(Address, Bytes);
  }
  llvm::Error write(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes) override {
    return Storage.write(Address, Bytes);
  }
  llvm::Error validateBacking(uint64_t Address, uint64_t Size) const override {
    return Storage.validateBacking(Address, Size);
  }
  llvm::Expected<bool> canAccess(uint64_t Address, uint64_t Size,
                                 unsigned Permissions) const override {
    return Storage.canAccess(Address, Size, Permissions);
  }
  llvm::Error readBacking(uint64_t Address,
                          llvm::MutableArrayRef<uint8_t> Bytes) override {
    return Storage.readBacking(Address, Bytes);
  }
  llvm::Error writeBacking(uint64_t Address,
                           llvm::ArrayRef<uint8_t> Bytes) override {
    return Storage.writeBacking(Address, Bytes);
  }
  llvm::Error snapshotBacking(uint64_t Address,
                              llvm::MutableArrayRef<uint8_t> Bytes) override {
    return Storage.snapshotBacking(Address, Bytes);
  }

private:
  GuestMemory &Storage;
};

class KernelMDLUserMapping : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  static constexpr uint32_t PoolTag = 0x55766c4d;
  static constexpr uint32_t ProcessA = DriverRequest::DefaultRequestorProcessID;
  static constexpr uint32_t ProcessB = ProcessA + 1;
  static constexpr const char *DeviceName = "\\Device\\MDLUserMapping";
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<AliasBudgetMemory> Budget;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

  static void success(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }
  template <typename T> static T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  static void rejected(llvm::Expected<uint64_t> Value) {
    ASSERT_FALSE(bool(Value));
    auto Error = Value.takeError();
    EXPECT_FALSE(Error.isA<KernelGuestException>());
    llvm::consumeError(std::move(Error));
  }
  static uint32_t raised(llvm::Expected<uint64_t> Value) {
    if (Value) {
      ADD_FAILURE() << "expected a guest exception";
      return 0;
    }
    uint32_t Code = 0;
    llvm::handleAllErrors(
        Value.takeError(),
        [&](const KernelGuestException &Error) { Code = Error.code(); },
        [&](const llvm::ErrorInfoBase &Error) {
          ADD_FAILURE() << Error.message();
        });
    return Code;
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name, Arguments));
  }
  uint64_t get(uint64_t Address, unsigned Size = sizeof(uint64_t)) {
    return take(Memory->readInteger(Address, Size));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Size = sizeof(uint64_t)) {
    success(Model->validateGuestAccess(Address, Size, true));
    success(Memory->writeInteger(Address, Value, Size));
  }
  bool allowed(uint64_t Address, unsigned Permissions) {
    return take(Memory->canAccess(Address, 1, Permissions));
  }
  void process(uint32_t ID) {
    Model->enterExecution(profile::StackBase);
    success(Model->setUserRequestContext(true, ID));
  }
  uint64_t descriptor(uint64_t Buffer, uint32_t Size, uint64_t IRP = 0) {
    return call("IoAllocateMdl", {Buffer, Size, 0, 0, IRP});
  }
  uint64_t map(uint64_t MDL, uint32_t Priority = NormalPagePriority,
               uint64_t Requested = 0, uint32_t BugCheck = 0) {
    return call("MmMapLockedPagesSpecifyCache",
                {MDL, UserMode, MmCached, Requested, BugCheck, Priority});
  }
  uint64_t pool(uint32_t Size = profile::PageSize) {
    return call("ExAllocatePoolWithTag", {0, Size, PoolTag});
  }
  uint64_t poolMDL(uint64_t Buffer, uint32_t Size = profile::PageSize) {
    const auto MDL = descriptor(Buffer, Size);
    call("MmBuildMdlForNonPagedPool", {MDL});
    return MDL;
  }
  void complete(uint64_t IRP) {
    put(IRP + IRPStatusOffset, StatusSuccess, sizeof(uint32_t));
    put(IRP + IRPInformationOffset, 0);
    call("IofCompleteRequest", {IRP, 0});
  }
  uint64_t begin(uint32_t File = 1, uint32_t Process = ProcessA) {
    DriverRequest Create;
    Create.Kind = DriverRequestKind::Create;
    Create.Device = DeviceName;
    Create.File = File;
    Create.RequestorProcessID = Process;
    const auto Open = take(Model->beginRequest(Create));
    complete(Open.IRP);
    success(Model->recordDispatchReturn(Open.IRP, StatusSuccess));
    success(Model->finalizeRequest(Open.IRP));
    DriverRequest IO;
    IO.Kind = DriverRequestKind::DeviceControl;
    IO.File = File;
    IO.RequestorProcessID = Process;
    IO.ControlCode = 0x222000 | MethodNeither;
    IO.OutputSize = 2 * profile::PageSize;
    const auto IRP = take(Model->beginRequest(IO)).IRP;
    process(Process);
    return IRP;
  }
  void pending(uint64_t IRP) {
    call("IoMarkIrpPending", {IRP});
    success(Model->recordDispatchReturn(IRP, StatusPending));
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, profile::PageSize, Read | Write));
    Budget = std::make_unique<AliasBudgetMemory>(*Memory);
    Model = std::make_unique<KernelModel>(*Budget, Result);
    DriverImage Image;
    Image.Base = Entry - profile::PageSize;
    Image.Entry = Entry;
    Image.Size = 3 * profile::PageSize;
    success(Model->initialize(Image, DriverOptions{}));
    constexpr uint64_t Name = Scratch + 0x100;
    constexpr uint64_t Text = Name + 0x20;
    const llvm::StringRef NameText(DeviceName);
    for (size_t I = 0; I < NameText.size(); ++I)
      put(Text + I * sizeof(uint16_t), uint8_t(NameText[I]), sizeof(uint16_t));
    put(Name, NameText.size() * sizeof(uint16_t), sizeof(uint16_t));
    put(Name + sizeof(uint16_t), (NameText.size() + 1) * sizeof(uint16_t),
        sizeof(uint16_t));
    put(Name + sizeof(uint64_t), Text);
    ASSERT_EQ(call("IoCreateDevice", {Model->driverObject(), 0, Name,
                                      UnknownDeviceType, 0, 0, Scratch}),
              StatusSuccess);
    for (unsigned Major : {0u, 14u})
      put(Model->driverObject() + DriverDispatchOffset +
              Major * sizeof(uint64_t),
          Entry);
    success(Model->finishEntry());
  }
};

TEST_F(KernelMDLUserMapping, PoolViewsShareBytesAndPreserveSystemMapping) {
  begin();
  const auto Pool = pool();
  const auto MDL = poolMDL(Pool);
  const auto Flags = get(MDL + MDLFlagsOffset, sizeof(uint16_t));
  const auto First = map(MDL);
  const auto Second = map(MDL, NormalPagePriority, 0, 1);
  ASSERT_NE(First, 0u);
  ASSERT_NE(First, Second);
  EXPECT_LT(First, profile::UserProbeLimit);
  EXPECT_NE(First, Pool);
  EXPECT_EQ(get(MDL + MDLMappedSystemVAOffset), Pool);
  EXPECT_EQ(get(MDL + MDLFlagsOffset, sizeof(uint16_t)), Flags);
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority}),
            Pool);
  put(Pool + 17, 0x63, 1);
  EXPECT_EQ(get(First + 17, 1), 0x63u);
  put(Second + 17, 0x91, 1);
  EXPECT_EQ(get(Pool + 17, 1), 0x91u);
  EXPECT_EQ(get(First + 17, 1), 0x91u);
  call("MmUnmapLockedPages", {First, MDL});
  EXPECT_FALSE(allowed(First, Read));
  EXPECT_TRUE(allowed(Second, Read | Write));
  EXPECT_TRUE(allowed(Pool, Read | Write));
  call("MmUnmapLockedPages", {Second, MDL});
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping, EachViewKeepsReadOnlyAndNoExecutePermissions) {
  begin();
  const auto Pool = pool();
  const auto MDL = poolMDL(Pool);
  for (uint32_t Flags :
       std::array<uint32_t, 4>{0, MdlMappingNoWrite, MdlMappingNoExecute,
                               MdlMappingNoWrite | MdlMappingNoExecute}) {
    const auto Alias = map(MDL, NormalPagePriority | Flags);
    EXPECT_TRUE(allowed(Alias, Read));
    EXPECT_EQ(allowed(Alias, Write), !(Flags & MdlMappingNoWrite));
    EXPECT_FALSE(allowed(Alias, Execute));
    EXPECT_TRUE(allowed(Pool, Read | Write));
    call("MmUnmapLockedPages", {Alias, MDL});
  }
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping, ProcessorModeUsesOnlyTheArgumentLowByte) {
  constexpr uint64_t RegisterHighBits = 0xaabbccdd12345600;
  const auto IRP = begin();
  const auto User = get(IRP + IRPUserBufferOffset);
  const auto MDL = descriptor(User + 3, 16);
  call("MmProbeAndLockPages",
       {MDL, RegisterHighBits | UserMode, IoWriteAccess});
  const auto System = call(
      "MmMapLockedPagesSpecifyCache",
      {MDL, RegisterHighBits | KernelMode, MmCached, 0, 0, NormalPagePriority});
  const auto Alias = call(
      "MmMapLockedPagesSpecifyCache",
      {MDL, RegisterHighBits | UserMode, MmCached, 0, 0, NormalPagePriority});
  ASSERT_NE(System, 0u);
  ASSERT_NE(Alias, 0u);
  EXPECT_NE(Alias, System);
  EXPECT_LT(Alias, profile::UserProbeLimit);
  EXPECT_EQ(get(MDL + MDLMappedSystemVAOffset), System);
  put(Alias, 0x53, 1);
  EXPECT_EQ(get(User + 3, 1), 0x53u);
  EXPECT_EQ(get(System, 1), 0x53u);
  call("MmUnmapLockedPages", {Alias, MDL});
  call("MmUnmapLockedPages", {System, MDL});
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
}

TEST_F(KernelMDLUserMapping, ReadLocksCannotGainWriteAccessThroughUserViews) {
  const auto IRP = begin();
  const auto User = get(IRP + IRPUserBufferOffset);
  put(User, 0x71, 1);
  const auto MDL = descriptor(User, 16);
  call("MmProbeAndLockPages", {MDL, UserMode, IoReadAccess});
  const auto Alias = map(MDL);
  EXPECT_TRUE(allowed(Alias, Read));
  EXPECT_FALSE(allowed(Alias, Write));
  EXPECT_FALSE(allowed(Alias, Execute));
  const auto Relocked = descriptor(Alias, 16);
  const auto Flags = get(Relocked + MDLFlagsOffset, sizeof(uint16_t));
  EXPECT_EQ(raised(Model->call("MmProbeAndLockPages",
                               {Relocked, UserMode, IoWriteAccess})),
            exceptions::StatusAccessViolation);
  EXPECT_EQ(get(Relocked + MDLFlagsOffset, sizeof(uint16_t)), Flags);
  EXPECT_FALSE(Memory->fault());
  EXPECT_TRUE(allowed(Alias, Read));
  EXPECT_FALSE(allowed(Alias, Write));
  call("MmProbeAndLockPages", {Relocked, UserMode, IoReadAccess});
  EXPECT_EQ(get(Relocked + MDLSize), get(MDL + MDLSize));
  const auto System =
      call("MmGetSystemAddressForMdlSafe", {Relocked, NormalPagePriority});
  EXPECT_TRUE(allowed(System, Read));
  EXPECT_FALSE(allowed(System, Write));
  call("MmUnmapLockedPages", {Alias, MDL});
  EXPECT_EQ(get(System, 1), 0x71u);
  call("MmUnlockPages", {Relocked});
  call("IoFreeMdl", {Relocked});
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
}

TEST_F(KernelMDLUserMapping,
       MappingProcessOwnsViewsIndependentlyOfLockProcess) {
  const auto IRP = begin();
  const auto User = get(IRP + IRPUserBufferOffset);
  const auto MDL = descriptor(User + 3, 16);
  call("MmProbeAndLockPages", {MDL, UserMode, IoWriteAccess});
  const auto System =
      call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  const auto First = map(MDL);
  process(ProcessB);
  const auto Second = map(MDL, NormalPagePriority, First + profile::PageSize);
  EXPECT_EQ(Second & (profile::PageSize - 1), 3u);
  EXPECT_FALSE(allowed(First, Read));
  EXPECT_TRUE(allowed(Second, Read | Write));
  EXPECT_TRUE(allowed(System, Read | Write));
  rejected(Model->call("MmUnmapLockedPages", {First, MDL}));
  EXPECT_TRUE(allowed(Second, Read | Write));
  process(ProcessA);
  EXPECT_TRUE(allowed(First, Read | Write));
  EXPECT_FALSE(allowed(Second, Read));
  rejected(Model->call("MmUnmapLockedPages", {Second, MDL}));
  EXPECT_TRUE(allowed(First, Read | Write));
  call("MmUnmapLockedPages", {First, MDL});
  process(ProcessB);
  call("MmUnmapLockedPages", {Second, MDL});
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
}

TEST_F(KernelMDLUserMapping,
       InvalidUnmapsAndElevatedIRQLLeaveLiveViewsUnchanged) {
  begin();
  const auto Pool = pool();
  const auto MDL = poolMDL(Pool);
  const auto Other = poolMDL(Pool);
  const auto Alias = map(MDL);
  for (auto Arguments : {std::array<uint64_t, 2>{Alias + 1, MDL},
                         std::array<uint64_t, 2>{Alias, Other}}) {
    rejected(Model->call("MmUnmapLockedPages", Arguments));
    EXPECT_TRUE(allowed(Alias, Read | Write));
  }
  EXPECT_EQ(call("KfRaiseIrql", {scheduler::DispatchLevel}),
            scheduler::PassiveLevel);
  rejected(Model->call("MmMapLockedPagesSpecifyCache",
                       {MDL, UserMode, MmCached, 0, 0, NormalPagePriority}));
  rejected(Model->call("MmUnmapLockedPages", {Alias, MDL}));
  EXPECT_TRUE(allowed(Alias, Read | Write));
  call("KeLowerIrql", {scheduler::PassiveLevel});
  call("MmUnmapLockedPages", {Alias, MDL});
  rejected(Model->call("MmUnmapLockedPages", {Alias, MDL}));
  EXPECT_FALSE(allowed(Alias, Read));
  call("IoFreeMdl", {Other});
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping,
       RelockingUserViewPreservesPhysicalIdentityAndBytes) {
  begin();
  const auto Pool = pool(2 * profile::PageSize);
  const auto Source = poolMDL(Pool, 2 * profile::PageSize);
  const auto Alias = map(Source);
  const auto Locked = descriptor(Alias + profile::PageSize - 3, 6);
  call("MmProbeAndLockPages", {Locked, UserMode, IoWriteAccess});
  EXPECT_EQ(get(Locked + MDLSize), get(Source + MDLSize));
  EXPECT_EQ(get(Locked + MDLSize + profile::PointerSize),
            get(Source + MDLSize + profile::PointerSize));
  const auto System =
      call("MmGetSystemAddressForMdlSafe", {Locked, NormalPagePriority});
  put(System, 0x7351, sizeof(uint16_t));
  EXPECT_EQ(get(Pool + profile::PageSize - 3, sizeof(uint16_t)), 0x7351u);
  call("MmUnmapLockedPages", {Alias, Source});
  EXPECT_FALSE(allowed(Alias, Read));
  EXPECT_EQ(get(System, sizeof(uint16_t)), 0x7351u);
  rejected(Model->call("ExFreePoolWithTag", {Pool, PoolTag}));
  call("MmUnlockPages", {Locked});
  call("IoFreeMdl", {Locked});
  call("IoFreeMdl", {Source});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping,
       LiveViewsPreventDescriptorUnlockAndPoolRetirement) {
  const auto IRP = begin();
  const auto User = get(IRP + IRPUserBufferOffset);
  const auto Locked = descriptor(User, 16, IRP);
  call("MmProbeAndLockPages", {Locked, UserMode, IoWriteAccess});
  const auto Alias = map(Locked);
  rejected(Model->call("MmUnlockPages", {Locked}));
  rejected(Model->call("IoFreeMdl", {Locked}));
  put(IRP + IRPStatusOffset, StatusSuccess, sizeof(uint32_t));
  rejected(Model->call("IofCompleteRequest", {IRP, 0}));
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_TRUE(allowed(Alias, Read | Write));
  EXPECT_EQ(get(IRP + IRPMdlOffset), Locked);
  call("MmUnmapLockedPages", {Alias, Locked});
  complete(IRP);
  EXPECT_TRUE(Result.Requests.back().Completed);

  const auto Pool = pool();
  const auto MDL = poolMDL(Pool);
  const auto PoolAlias = map(MDL);
  rejected(Model->call("ExFreePoolWithTag", {Pool, PoolTag}));
  rejected(Model->call("IoFreeMdl", {MDL}));
  EXPECT_TRUE(allowed(PoolAlias, Read | Write));
  call("MmUnmapLockedPages", {PoolAlias, MDL});
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping, ProcessExitRevokesItsViewsAndKeepsSystemAliases) {
  const auto First = begin();
  const auto MDL = descriptor(get(First + IRPUserBufferOffset), 16);
  call("MmProbeAndLockPages", {MDL, UserMode, IoWriteAccess});
  const auto System =
      call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  const auto FirstView = map(MDL);
  pending(First);
  const auto Second = begin(2, ProcessB);
  const auto SecondView =
      map(MDL, NormalPagePriority, FirstView + profile::PageSize);
  pending(Second);
  success(Model->exitRequestorProcess(First));
  EXPECT_FALSE(allowed(FirstView, Read));
  EXPECT_TRUE(allowed(SecondView, Read | Write));
  EXPECT_TRUE(allowed(System, Read | Write));
  put(SecondView, 0x72, 1);
  EXPECT_EQ(get(System, 1), 0x72u);
  rejected(Model->call("MmUnlockPages", {MDL}));
  success(Model->exitRequestorProcess(Second));
  EXPECT_FALSE(allowed(SecondView, Read));
  EXPECT_EQ(get(System, 1), 0x72u);
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
  complete(First);
  complete(Second);
}

TEST_F(KernelMDLUserMapping, AliasBudgetShortageRaisesBeforePublishingAView) {
  begin();
  const auto Pool = pool();
  const auto MDL = poolMDL(Pool);
  const auto Flags = get(MDL + MDLFlagsOffset, sizeof(uint16_t));
  Budget->Exhausted = true;
  EXPECT_EQ(
      raised(Model->call("MmMapLockedPagesSpecifyCache",
                         {MDL, UserMode, MmCached, 0, 0, NormalPagePriority})),
      StatusInsufficientResources);
  EXPECT_EQ(get(MDL + MDLMappedSystemVAOffset), Pool);
  EXPECT_EQ(get(MDL + MDLFlagsOffset, sizeof(uint16_t)), Flags);
  EXPECT_FALSE(Memory->fault());
  Budget->Exhausted = false;
  const auto Alias = map(MDL);
  EXPECT_TRUE(allowed(Alias, Read | Write));
  call("MmUnmapLockedPages", {Alias, MDL});
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping, RequestedViewPreservesTheDescriptorPageOffset) {
  const auto IRP = begin();
  const auto User = get(IRP + IRPUserBufferOffset);
  const auto MDL = descriptor(User + 3, 16);
  call("MmProbeAndLockPages", {MDL, UserMode, IoWriteAccess});
  const uint64_t Requested =
      profile::UserMappedAliasBase + 4 * profile::PageSize + 3;
  const auto Alias = map(MDL, NormalPagePriority, Requested);
  EXPECT_EQ(Alias, Requested);
  put(Alias, 0x53, 1);
  EXPECT_EQ(get(User + 3, 1), 0x53u);
  rejected(Model->call(
      "MmMapLockedPagesSpecifyCache",
      {MDL, UserMode, MmCached, Requested + 1, 0, NormalPagePriority}));
  EXPECT_EQ(raised(Model->call(
                "MmMapLockedPagesSpecifyCache",
                {MDL, UserMode, MmCached, Requested, 0, NormalPagePriority})),
            StatusInsufficientResources);
  EXPECT_TRUE(allowed(Alias, Read | Write));
  call("MmUnmapLockedPages", {Alias, MDL});
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
}

TEST_F(KernelMDLUserMapping, SubpagePoolOwnersCannotExposeNeighbouringStorage) {
  begin();
  const auto Pool = pool(16);
  const auto MDL = poolMDL(Pool, 16);
  rejected(Model->call("MmMapLockedPagesSpecifyCache",
                       {MDL, UserMode, MmCached, 0, 0, NormalPagePriority}));
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority}),
            Pool);
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping,
       ReusedAddressChangesBackingAndPermissionsWithoutRetargetingPins) {
  begin();
  const auto FirstPool = pool();
  const auto SecondPool = pool();
  const auto First = poolMDL(FirstPool);
  const auto Second = poolMDL(SecondPool);
  put(FirstPool, 0x31, 1);
  put(SecondPool, 0x72, 1);
  const auto Address = map(First);
  const auto Locked = descriptor(Address, 16);
  call("MmProbeAndLockPages", {Locked, UserMode, IoWriteAccess});
  const auto System =
      call("MmGetSystemAddressForMdlSafe", {Locked, NormalPagePriority});
  const auto FirstPFN = get(Locked + MDLSize);
  call("MmUnmapLockedPages", {Address, First});
  EXPECT_FALSE(allowed(Address, Read));
  EXPECT_TRUE(Model->canCatchUserAccess(Address, 1));
  success(Model->validateGuestAccess(Address, 1, false));
  const auto NewLock = descriptor(Address, 16);
  EXPECT_EQ(raised(Model->call("MmProbeAndLockPages",
                               {NewLock, UserMode, IoReadAccess})),
            exceptions::StatusAccessViolation);
  EXPECT_FALSE(Memory->fault());

  EXPECT_EQ(map(Second, NormalPagePriority | MdlMappingNoWrite, Address),
            Address);
  EXPECT_EQ(get(Address, 1), 0x72u);
  EXPECT_FALSE(allowed(Address, Write));
  EXPECT_FALSE(allowed(Address, Execute));
  EXPECT_EQ(get(System, 1), 0x31u);
  EXPECT_EQ(get(Locked + MDLSize), FirstPFN);
  call("MmProbeAndLockPages", {NewLock, UserMode, IoReadAccess});
  EXPECT_EQ(get(NewLock + MDLSize), get(Second + MDLSize));
  EXPECT_NE(get(NewLock + MDLSize), FirstPFN);
  rejected(Model->call("MmUnmapLockedPages", {Address, First}));
  EXPECT_TRUE(allowed(Address, Read));
  call("MmUnmapLockedPages", {Address, Second});
  call("MmUnlockPages", {NewLock});
  call("IoFreeMdl", {NewLock});

  EXPECT_EQ(map(Second, NormalPagePriority, Address), Address);
  put(Address, 0x43, 1);
  EXPECT_EQ(get(SecondPool, 1), 0x43u);
  EXPECT_EQ(get(System, 1), 0x31u);
  call("MmUnmapLockedPages", {Address, Second});
  call("MmUnlockPages", {Locked});
  call("IoFreeMdl", {Locked});
  for (auto MDL : {First, Second})
    call("IoFreeMdl", {MDL});
  for (auto Pool : {FirstPool, SecondPool})
    call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping,
       RetiredViewAddressesAreReusableAfterProcessExitAndContextChanges) {
  const auto IRP = begin();
  const auto Pool = pool();
  const auto MDL = poolMDL(Pool);
  const auto Address = map(MDL);
  pending(IRP);
  success(Model->exitRequestorProcess(IRP));
  EXPECT_FALSE(allowed(Address, Read));
  process(ProcessB);
  EXPECT_EQ(map(MDL, NormalPagePriority | MdlMappingNoWrite, Address), Address);
  process(ProcessA);
  EXPECT_FALSE(allowed(Address, Read));
  rejected(Model->call("MmUnmapLockedPages", {Address, MDL}));
  process(ProcessB);
  EXPECT_TRUE(allowed(Address, Read));
  EXPECT_FALSE(allowed(Address, Write));
  call("MmUnmapLockedPages", {Address, MDL});
  for (unsigned I = 0; I != 4; ++I) {
    const auto Reused = map(MDL);
    EXPECT_EQ(Reused, Address);
    EXPECT_TRUE(allowed(Reused, Read | Write));
    call("MmUnmapLockedPages", {Reused, MDL});
  }
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping,
       FailedRemappingLeavesAddressAndBackingAvailableForRetry) {
  begin();
  const auto Pool = pool();
  const auto MDL = poolMDL(Pool);
  const auto Address = map(MDL);
  call("MmUnmapLockedPages", {Address, MDL});
  Budget->Exhausted = true;
  EXPECT_EQ(raised(Model->call(
                "MmMapLockedPagesSpecifyCache",
                {MDL, UserMode, MmCached, Address, 0, NormalPagePriority})),
            StatusInsufficientResources);
  EXPECT_FALSE(allowed(Address, Read));
  EXPECT_TRUE(allowed(Pool, Read | Write));
  Budget->Exhausted = false;
  EXPECT_EQ(map(MDL, NormalPagePriority, Address), Address);
  call("MmUnmapLockedPages", {Address, MDL});
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping, CacheRequestsInheritExistingRamPageAttributes) {
  const auto IRP = begin();
  const auto User = get(IRP + IRPUserBufferOffset);
  const auto MDL = descriptor(User + 3, 16);
  call("MmProbeAndLockPages", {MDL, UserMode, IoWriteAccess});
  const auto PFN = get(MDL + MDLSize);
  for (uint32_t Cache : {MmNonCached, MmCached, MmWriteCombined}) {
    const auto System =
        call("MmMapLockedPagesSpecifyCache",
             {MDL, KernelMode, Cache, 0, 0, NormalPagePriority});
    const auto UserView =
        call("MmMapLockedPagesSpecifyCache",
             {MDL, UserMode, Cache, 0, 0, NormalPagePriority});
    put(System, Cache + 1, 1);
    EXPECT_EQ(get(UserView, 1), Cache + 1);
    EXPECT_EQ(get(User + 3, 1), Cache + 1);
    EXPECT_EQ(get(MDL + MDLSize), PFN);
    call("MmUnmapLockedPages", {UserView, MDL});
    call("MmUnmapLockedPages", {System, MDL});
  }
  constexpr uint32_t ReservedCacheType = MmWriteCombined + 1;
  for (uint32_t Mode : {KernelMode, UserMode}) {
    rejected(
        Model->call("MmMapLockedPagesSpecifyCache",
                    {MDL, Mode, ReservedCacheType, 0, 0, NormalPagePriority}));
    EXPECT_EQ(get(MDL + MDLMappedSystemVAOffset), 0u);
    EXPECT_EQ(get(MDL + MDLSize), PFN);
  }
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
}

TEST_F(KernelMDLUserMapping,
       LeavingProcessContextRevokesOriginalAndMappedUserAddresses) {
  const auto IRP = begin();
  const auto User = get(IRP + IRPUserBufferOffset);
  const auto MDL = descriptor(User, 16);
  call("MmProbeAndLockPages", {MDL, UserMode, IoWriteAccess});
  const auto Alias = map(MDL);
  const auto System =
      call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  success(Model->setUserRequestContext(false));
  EXPECT_FALSE(allowed(User, Read));
  EXPECT_FALSE(allowed(Alias, Read));
  EXPECT_TRUE(allowed(System, Read | Write));
  put(System, 0x51, 1);
  process(ProcessA);
  EXPECT_EQ(get(User, 1), 0x51u);
  EXPECT_EQ(get(Alias, 1), 0x51u);
  call("MmUnmapLockedPages", {Alias, MDL});
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
}

TEST_F(KernelMDLUserMapping,
       ConcurrentProcessesBindTheSameAddressToIndependentPagesAndPermissions) {
  begin();
  const auto FirstPool = pool();
  const auto SecondPool = pool(2 * profile::PageSize);
  const auto First = poolMDL(FirstPool);
  const auto Second = poolMDL(SecondPool, 2 * profile::PageSize);
  put(FirstPool, 0x31, 1);
  put(SecondPool, 0x72, 1);
  const auto Address = map(First);
  const auto FirstLock = descriptor(Address, 16);
  call("MmProbeAndLockPages", {FirstLock, UserMode, IoWriteAccess});
  const auto FirstSystem =
      call("MmGetSystemAddressForMdlSafe", {FirstLock, NormalPagePriority});
  process(ProcessB);
  EXPECT_FALSE(allowed(Address, Read));
  EXPECT_EQ(map(Second, NormalPagePriority | MdlMappingNoWrite, Address),
            Address);
  EXPECT_EQ(get(Address, 1), 0x72u);
  EXPECT_FALSE(allowed(Address, Write));
  EXPECT_TRUE(allowed(Address + profile::PageSize, Read));
  EXPECT_FALSE(allowed(Address, Execute));
  const auto SecondLock = descriptor(Address, 16);
  EXPECT_EQ(raised(Model->call("MmProbeAndLockPages",
                               {SecondLock, UserMode, IoWriteAccess})),
            exceptions::StatusAccessViolation);
  call("MmProbeAndLockPages", {SecondLock, UserMode, IoReadAccess});
  const auto SecondSystem =
      call("MmGetSystemAddressForMdlSafe", {SecondLock, NormalPagePriority});
  EXPECT_NE(get(FirstLock + MDLSize), get(SecondLock + MDLSize));
  EXPECT_EQ(get(SecondLock + MDLSize), get(Second + MDLSize));
  rejected(Model->call("MmUnmapLockedPages", {Address, First}));
  EXPECT_EQ(get(Address, 1), 0x72u);
  EXPECT_EQ(
      raised(Model->call("MmMapLockedPagesSpecifyCache",
                         {Second, UserMode, MmCached,
                          Address + profile::PageSize, 0, NormalPagePriority})),
      StatusInsufficientResources);

  process(ProcessA);
  EXPECT_EQ(get(Address, 1), 0x31u);
  EXPECT_TRUE(allowed(Address, Write));
  EXPECT_FALSE(allowed(Address + profile::PageSize, Read));
  put(Address, 0x43, 1);
  EXPECT_EQ(get(FirstSystem, 1), 0x43u);
  EXPECT_EQ(get(SecondSystem, 1), 0x72u);
  rejected(Model->call("MmUnmapLockedPages", {Address, Second}));
  success(Model->setUserRequestContext(false));
  EXPECT_FALSE(allowed(Address, Read));
  EXPECT_EQ(get(FirstSystem, 1), 0x43u);
  EXPECT_EQ(get(SecondSystem, 1), 0x72u);
  process(ProcessB);
  EXPECT_EQ(get(Address, 1), 0x72u);
  EXPECT_FALSE(allowed(Address, Write));
  call("MmUnmapLockedPages", {Address, Second});
  process(ProcessA);
  EXPECT_EQ(get(Address, 1), 0x43u);
  call("MmUnmapLockedPages", {Address, First});
  for (auto Locked : {FirstLock, SecondLock}) {
    call("MmUnlockPages", {Locked});
    call("IoFreeMdl", {Locked});
  }
  for (auto MDL : {First, Second})
    call("IoFreeMdl", {MDL});
  for (auto Pool : {FirstPool, SecondPool})
    call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping,
       ExitingAnInactiveProcessDoesNotUnmapAnotherProcessAtTheSameAddress) {
  const auto FirstIRP = begin();
  const auto FirstPool = pool();
  const auto First = poolMDL(FirstPool);
  const auto Address = map(First);
  pending(FirstIRP);
  const auto SecondIRP = begin(2, ProcessB);
  const auto SecondPool = pool();
  const auto Second = poolMDL(SecondPool);
  put(SecondPool, 0x72, 1);
  EXPECT_EQ(map(Second, NormalPagePriority | MdlMappingNoWrite, Address),
            Address);
  pending(SecondIRP);
  success(Model->exitRequestorProcess(FirstIRP));
  EXPECT_EQ(get(Address, 1), 0x72u);
  EXPECT_FALSE(allowed(Address, Write));
  call("IoFreeMdl", {First});
  call("ExFreePoolWithTag", {FirstPool, PoolTag});
  rejected(Model->call("IoFreeMdl", {Second}));
  process(ProcessA);
  EXPECT_FALSE(allowed(Address, Read));
  process(ProcessB);
  EXPECT_EQ(get(Address, 1), 0x72u);
  call("MmUnmapLockedPages", {Address, Second});
  call("IoFreeMdl", {Second});
  call("ExFreePoolWithTag", {SecondPool, PoolTag});
  complete(FirstIRP);
  complete(SecondIRP);
}

TEST_F(KernelMDLUserMapping,
       FailedProcessSwitchPreservesTheOriginalBindingsAndProcessAuthority) {
  const auto FirstIRP = begin();
  const auto FirstUser = get(FirstIRP + IRPUserBufferOffset);
  const auto FirstPool = pool();
  const auto First = poolMDL(FirstPool);
  put(FirstPool, 0x31, 1);
  const auto Address = map(First);
  pending(FirstIRP);
  const auto SecondIRP = begin(2, ProcessB);
  const auto SecondUser = get(SecondIRP + IRPUserBufferOffset);
  const auto SecondPool = pool(2 * profile::PageSize);
  const auto Second = poolMDL(SecondPool, 2 * profile::PageSize);
  put(SecondPool, 0x72, 1);
  EXPECT_EQ(map(Second, NormalPagePriority | MdlMappingNoWrite, Address),
            Address);
  process(ProcessA);
  Budget->Exhausted = true;
  auto Error = Model->setUserRequestContext(true, ProcessB);
  ASSERT_TRUE(bool(Error));
  EXPECT_TRUE(Error.isA<GuestMemoryLimitError>());
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(get(Address, 1), 0x31u);
  EXPECT_TRUE(allowed(Address, Write));
  EXPECT_FALSE(allowed(Address + profile::PageSize, Read));
  EXPECT_TRUE(allowed(FirstUser, Read | Write));
  EXPECT_FALSE(allowed(SecondUser, Read));
  EXPECT_EQ(call("PsGetCurrentProcessId", {}), ProcessA);
  EXPECT_FALSE(Memory->fault());
  Budget->Exhausted = false;
  process(ProcessB);
  EXPECT_EQ(get(Address, 1), 0x72u);
  EXPECT_FALSE(allowed(Address, Write));
  EXPECT_FALSE(allowed(FirstUser, Read));
  EXPECT_TRUE(allowed(SecondUser, Read | Write));
  call("MmUnmapLockedPages", {Address, Second});
  process(ProcessA);
  call("MmUnmapLockedPages", {Address, First});
  for (auto MDL : {First, Second})
    call("IoFreeMdl", {MDL});
  for (auto Pool : {FirstPool, SecondPool})
    call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping,
       DispatcherObjectsKeepTheirBindingUntilTheViewIsExplicitlyUnmapped) {
  begin();
  const auto Pool = pool();
  const auto MDL = poolMDL(Pool);
  const auto Address = map(MDL);
  call("KeInitializeEvent", {Address, dispatcher::NotificationObject, 1});
  auto Error = Model->setUserRequestContext(true, ProcessB);
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("dispatcher object"),
            std::string::npos);
  EXPECT_TRUE(allowed(Address, Read | Write));
  EXPECT_EQ(call("PsGetCurrentProcessId", {}), ProcessA);
  EXPECT_EQ(call("KeReadStateEvent", {Address}), 1u);
  call("MmUnmapLockedPages", {Address, MDL});
  process(ProcessB);
  EXPECT_EQ(map(MDL, NormalPagePriority, Address), Address);
  rejected(Model->call("KeReadStateEvent", {Address}));
  call("MmUnmapLockedPages", {Address, MDL});
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLUserMapping,
       DispatcherAccessChecksTheOriginalUserBufferProcessAndPermissions) {
  const auto IRP = begin();
  const auto User = get(IRP + IRPUserBufferOffset);
  call("KeInitializeEvent", {User, dispatcher::NotificationObject, 1});
  process(ProcessB);
  EXPECT_EQ(raised(Model->call("KeResetEvent", {User})),
            exceptions::StatusAccessViolation);
  EXPECT_FALSE(Memory->fault());
  process(ProcessA);
  EXPECT_EQ(call("KeReadStateEvent", {User}), 1u);
}

TEST_F(KernelMDLUserMapping,
       FailedAttachmentDoesNotPublishSavedApcStateOrProcessPermissions) {
  const auto IRP = begin();
  const auto Process = call("IoGetRequestorProcess", {IRP});
  const auto Pool = pool();
  const auto MDL = poolMDL(Pool);
  const auto Address = map(MDL);
  const auto ApcState = pool(KAPCStateSize);
  std::array<uint8_t, KAPCStateSize> Original;
  Original.fill(0x63);
  success(Memory->write(ApcState, Original));
  const auto Work = call("IoAllocateWorkItem", {get(Scratch)});
  call("IoQueueWorkItem", {Work, Entry, profile::DelayedWorkQueue, 0});
  pending(IRP);
  success(Model->setUserRequestContext(false));
  auto Scheduled = take(Model->nextScheduled(false));
  ASSERT_TRUE(Scheduled);
  Model->enterExecution(profile::CallbackStackBase);
  const auto SystemProcess = call("IoGetCurrentProcess", {});
  Budget->Exhausted = true;
  auto Attached = Model->call("KeStackAttachProcess", {Process, ApcState});
  ASSERT_FALSE(bool(Attached));
  auto Error = Attached.takeError();
  EXPECT_TRUE(Error.isA<GuestMemoryLimitError>());
  llvm::consumeError(std::move(Error));
  std::array<uint8_t, KAPCStateSize> Actual;
  success(Memory->read(ApcState, Actual));
  EXPECT_EQ(Actual, Original);
  EXPECT_EQ(call("IoGetCurrentProcess", {}), SystemProcess);
  EXPECT_FALSE(allowed(Address, Read));
  rejected(Model->call("KeUnstackDetachProcess", {ApcState}));
  EXPECT_FALSE(Memory->fault());
  Budget->Exhausted = false;
  call("KeStackAttachProcess", {Process, ApcState});
  EXPECT_EQ(call("IoGetCurrentProcess", {}), Process);
  EXPECT_TRUE(allowed(Address, Read | Write));
  call("MmUnmapLockedPages", {Address, MDL});
  call("KeUnstackDetachProcess", {ApcState});
  EXPECT_EQ(call("IoGetCurrentProcess", {}), SystemProcess);
  success(Model->finishScheduled(Scheduled->ID));
  call("IoFreeWorkItem", {Work});
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
  call("ExFreePoolWithTag", {ApcState, PoolTag});
}

} // namespace
} // namespace neverd::emulation
