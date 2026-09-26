//===- KernelMDLChainTests.cpp - IRP MDL chains ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise public WDM chain links, completion cleanup and failed mutations.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <initializer_list>

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelMDLChain : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  static constexpr unsigned CreateDispatchIndex = 0;
  static constexpr unsigned DeviceControlDispatchIndex = 14;
  static constexpr unsigned NonPagedPoolType = 0;
  static constexpr uint32_t PoolTag = 0x436c644d;
  static constexpr uint32_t BufferedIOCTL = 0x222000;
  static constexpr uint32_t DirectIOCTL = BufferedIOCTL | MethodOutDirect;
  static constexpr uint32_t NeitherIOCTL = BufferedIOCTL | MethodNeither;
  static constexpr const char *DeviceName = "\\Device\\MDLChain";
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t Device = 0, Pool = 0;

  static void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <typename T> static T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  static void rejected(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    const auto Diagnostic = llvm::toString(std::move(E));
    EXPECT_NE(Diagnostic.find(Text.str()), std::string::npos) << Diagnostic;
  }
  template <typename T>
  static void rejected(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    rejected(Value.takeError(), Text);
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
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, 0x1000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = Entry - 0x1000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
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
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 0, Name,
                                      UnknownDeviceType, 0, 0, Scratch}),
              StatusSuccess);
    Device = get(Scratch);
    put(Device + DeviceFlagsOffset, DeviceBufferedIO, sizeof(uint32_t));
    for (unsigned Major : {CreateDispatchIndex, DeviceControlDispatchIndex})
      put(Model->driverObject() + DriverDispatchOffset +
              Major * sizeof(uint64_t),
          Entry);
    Pool = call("ExAllocatePoolWithTag", {NonPagedPoolType, 64, PoolTag});
    ASSERT_NE(Pool, 0u);
    success(Model->finishEntry());
  }
  void open(uint32_t File = 1) {
    DriverRequest Create;
    Create.Kind = DriverRequestKind::Create;
    Create.Device = DeviceName;
    Create.File = File;
    const auto Open = take(Model->beginRequest(Create));
    complete(Open.IRP);
    success(Model->recordDispatchReturn(Open.IRP, StatusSuccess));
    success(Model->finalizeRequest(Open.IRP));
  }
  uint64_t begin(DriverRequest IO, uint32_t File = 1) {
    open(File);
    IO.File = File;
    return take(Model->beginRequest(IO)).IRP;
  }
  uint64_t begin(uint32_t Code = BufferedIOCTL, uint32_t File = 1) {
    DriverRequest IO;
    IO.Kind = DriverRequestKind::DeviceControl;
    IO.File = File;
    IO.ControlCode = Code;
    IO.Input = {1, 2, 3, 4};
    IO.OutputSize = IO.Input.size();
    return begin(std::move(IO), File);
  }
  uint64_t allocate(uint64_t IRP, bool Secondary, uint64_t Buffer = 0) {
    return call("IoAllocateMdl",
                {Buffer ? Buffer : Pool, 4, Secondary, 0, IRP});
  }
  void status(uint64_t IRP, uint32_t Information = 0) {
    put(IRP + IRPStatusOffset, StatusSuccess, sizeof(uint32_t));
    put(IRP + IRPInformationOffset, Information);
  }
  void complete(uint64_t IRP, uint32_t Information = 0) {
    status(IRP, Information);
    call("IofCompleteRequest", {IRP, 0});
  }
};

TEST_F(KernelMDLChain, PrimaryAndSecondaryCompleteWithoutReleasingPoolStorage) {
  const uint64_t IRP = begin();
  const auto Primary = allocate(IRP, false);
  const auto Secondary = allocate(IRP, true, Pool + 4);
  const auto Tail = allocate(IRP, true, Pool + 8);
  call("MmBuildMdlForNonPagedPool", {Primary});
  call("MmBuildMdlForNonPagedPool", {Secondary});
  EXPECT_EQ(get(IRP + IRPMdlOffset), Primary);
  EXPECT_EQ(get(Primary + MDLNextOffset), Secondary);
  EXPECT_EQ(get(Secondary + MDLNextOffset), Tail);
  EXPECT_EQ(get(Tail + MDLNextOffset), 0u);
  complete(IRP);
  EXPECT_TRUE(Result.Requests.back().Completed);
  for (auto MDL : {Primary, Secondary, Tail})
    rejected(Model->validateGuestAccess(MDL, 1, false), "freed");
  success(Model->validateGuestAccess(Pool, 12, true));
  put(Pool, 0x51, 1);
  EXPECT_EQ(get(Pool, 1), 0x51u);
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLChain, SecondaryCanBecomeFirstAndPrimaryReplacementDetaches) {
  const uint64_t IRP = begin();
  const auto First = allocate(IRP, true);
  EXPECT_EQ(get(IRP + IRPMdlOffset), First);
  const auto Replaced = allocate(IRP, true);
  const auto Replacement = allocate(IRP, false);
  EXPECT_EQ(get(IRP + IRPMdlOffset), Replacement);
  EXPECT_EQ(get(First + MDLNextOffset), Replaced);
  complete(IRP);
  success(Model->validateGuestAccess(First, sizeof(uint64_t), false));
  success(Model->validateGuestAccess(Replaced, sizeof(uint64_t), false));
  call("IoFreeMdl", {First});
  success(Model->validateGuestAccess(Replaced, sizeof(uint64_t), false));
  call("IoFreeMdl", {Replaced});
  rejected(Model->validateGuestAccess(Replacement, 1, false), "freed");
}

TEST_F(KernelMDLChain, ManualInsertionUnlinkingAndFreeUseTheCurrentGuestLinks) {
  const uint64_t IRP = begin();
  const auto First = allocate(IRP, false);
  const auto Middle = allocate(IRP, true);
  const auto Last = allocate(0, false);
  put(Middle + MDLNextOffset, Last);
  put(First + MDLNextOffset, Last);
  call("IoFreeMdl", {Middle});
  // IoFreeMdl frees one descriptor, so Last remains associated and live.
  EXPECT_EQ(get(Middle + MDLNextOffset), Last);
  success(Model->validateGuestAccess(Last, sizeof(uint64_t), false));
  complete(IRP);
  rejected(Model->validateGuestAccess(First, 1, false), "freed");
  rejected(Model->validateGuestAccess(Last, 1, false), "freed");
}

TEST_F(KernelMDLChain,
       MalformedChainsFailBeforeAllocationOrCompletionMutation) {
  const uint64_t IRP = begin();
  const auto First = allocate(IRP, false);
  const auto Tail = allocate(IRP, true);
  const auto Stack = get(IRP + IRPStackPointerOffset);
  const auto Location = get(IRP + IRPLocationOffset, 1);
  status(IRP);
  for (uint64_t Invalid : {First, Pool}) {
    put(Tail + MDLNextOffset, Invalid);
    const auto Text = Invalid == First ? "cycle" : "unknown";
    rejected(Model->call("IoAllocateMdl", {Pool, 4, 1, 0, IRP}), Text);
    EXPECT_EQ(get(First + MDLNextOffset), Tail);
    EXPECT_EQ(get(Tail + MDLNextOffset), Invalid);
    rejected(Model->call("IofCompleteRequest", {IRP, 0}), Text);
    EXPECT_FALSE(Result.Requests.back().Completed);
    EXPECT_EQ(get(IRP + IRPStackPointerOffset), Stack);
    EXPECT_EQ(get(IRP + IRPLocationOffset, 1), Location);
    success(Model->validateGuestAccess(First, sizeof(uint64_t), false));
    success(Model->validateGuestAccess(Tail, sizeof(uint64_t), false));
  }
  put(Tail + MDLNextOffset, 0);
  const auto NextAllocation =
      (Tail + get(Tail + MDLSizeOffset, sizeof(uint16_t)) + PoolAlignment - 1) &
      ~(PoolAlignment - 1);
  EXPECT_EQ(allocate(IRP, true), NextAllocation);
  complete(IRP);
  EXPECT_TRUE(Result.Requests.back().Completed);
}

TEST_F(KernelMDLChain, FreeDoesNotUnlinkAndDanglingHeadMustBeRepaired) {
  const uint64_t IRP = begin();
  const auto First = allocate(IRP, false);
  const auto Tail = allocate(IRP, true);
  call("IoFreeMdl", {First});
  EXPECT_EQ(get(IRP + IRPMdlOffset), First);
  status(IRP);
  rejected(Model->call("IofCompleteRequest", {IRP, 0}), "freed");
  success(Model->validateGuestAccess(Tail, sizeof(uint64_t), false));
  put(IRP + IRPMdlOffset, Tail);
  complete(IRP);
  rejected(Model->validateGuestAccess(Tail, 1, false), "freed");
}

TEST_F(KernelMDLChain, SharedDescriptorCannotBeRetiredByTwoRequests) {
  const uint64_t FirstIRP = begin(BufferedIOCTL, 1);
  const auto Shared = allocate(FirstIRP, false);
  call("IoMarkIrpPending", {FirstIRP});
  success(Model->recordDispatchReturn(FirstIRP, StatusPending));
  const uint64_t SecondIRP = begin(BufferedIOCTL, 2);
  put(SecondIRP + IRPMdlOffset, Shared);
  status(SecondIRP);
  rejected(Model->call("IofCompleteRequest", {SecondIRP, 0}), "multiple IRPs");
  success(Model->validateGuestAccess(Shared, sizeof(uint64_t), false));
  put(SecondIRP + IRPMdlOffset, 0);
  complete(SecondIRP);
  complete(FirstIRP);
  rejected(Model->validateGuestAccess(Shared, 1, false), "freed");
}

TEST_F(KernelMDLChain, DirectRequestRetainsOutputAndOwnsAppendedDescriptors) {
  const uint64_t IRP = begin(DirectIOCTL);
  const auto Direct = get(IRP + IRPMdlOffset);
  const auto Output =
      call("MmGetSystemAddressForMdlSafe", {Direct, NormalPagePriority});
  put(Output, 0x77665544, sizeof(uint32_t));
  const auto Extra = allocate(IRP, true);
  rejected(Model->call("IoAllocateMdl", {Pool, 4, 0, 0, IRP}), "direct-I/O");
  EXPECT_EQ(get(IRP + IRPMdlOffset), Direct);
  EXPECT_EQ(get(Direct + MDLNextOffset), Extra);
  complete(IRP, sizeof(uint32_t));
  EXPECT_EQ(Result.Requests.back().Output,
            (std::vector<uint8_t>{0x44, 0x55, 0x66, 0x77}));
  rejected(Model->validateGuestAccess(Direct, 1, false), "freed");
  rejected(Model->validateGuestAccess(Extra, 1, false), "freed");
}

TEST_F(KernelMDLChain, CompletionUnlocksEveryUserDescriptorAndRevokesAliases) {
  const uint64_t IRP = begin(NeitherIOCTL);
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  const auto User = get(IRP + IRPUserBufferOffset);
  const auto First = allocate(IRP, false, User);
  const auto Second = allocate(IRP, true, User);
  for (auto MDL : {First, Second})
    call("MmProbeAndLockPages", {MDL, UserMode, IoWriteAccess});
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {First, NormalPagePriority});
  const auto SecondAlias =
      call("MmGetSystemAddressForMdlSafe", {Second, NormalPagePriority});
  put(Alias, 0x55443322, sizeof(uint32_t));
  EXPECT_EQ(get(SecondAlias, sizeof(uint32_t)), 0x55443322u);
  complete(IRP, sizeof(uint32_t));
  EXPECT_EQ(Result.Requests.back().Output,
            (std::vector<uint8_t>{0x22, 0x33, 0x44, 0x55}));
  for (auto MDL : {First, Second})
    rejected(Model->validateGuestAccess(MDL, 1, false), "freed");
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
  EXPECT_FALSE(take(Memory->canAccess(SecondAlias, 1, Read)));
  EXPECT_EQ(get(User, sizeof(uint32_t)), 0x55443322u);
}

TEST_F(KernelMDLChain, ExplicitNestedPointersPreserveDeclaredPagePermissions) {
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = NeitherIOCTL;
  IO.Input.resize(2 * profile::PointerSize);
  IO.OutputSize = profile::PointerSize;
  IO.UserInputAccess = DriverUserPageAccess::ReadOnly;
  IO.UserOutputAccess = DriverUserPageAccess::NoAccess;
  IO.UserBuffers = {
      {"descriptor", 24, {}, DriverUserPageAccess::ReadOnly},
      {"payload", 12, {0x42, 0x51}, DriverUserPageAccess::ReadWrite},
      {"sealed", 16, {0x63}, DriverUserPageAccess::NoAccess}};
  using Kind = DriverUserBufferKind;
  IO.UserPointers = {
      {{Kind::Input, {}, 8}, {Kind::Memory, "descriptor", 8}},
      {{Kind::Memory, "descriptor", 8}, {Kind::Memory, "payload", 3}},
      {{Kind::Memory, "sealed", 8}, {Kind::Memory, "payload", 12}},
      {{Kind::Output, {}, 0}, {Kind::Memory, "sealed", 0}}};
  const auto IRP = begin(IO);
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  const auto Stack = get(IRP + IRPStackPointerOffset);
  const auto Root = get(Stack + StackType3InputOffset);
  const auto Output = get(IRP + IRPUserBufferOffset);
  const auto Descriptor = Result.Requests.back().UserBuffers[0].Address;
  const auto Payload = Result.Requests.back().UserBuffers[1].Address;
  const auto Sealed = Result.Requests.back().UserBuffers[2].Address;
  EXPECT_EQ(get(Root + 8), Descriptor + 8);
  EXPECT_EQ(get(Descriptor + 8), Payload + 3);
  EXPECT_TRUE(take(Memory->canAccess(Root, IO.Input.size(), Read)));
  EXPECT_FALSE(take(Memory->canAccess(Root, IO.Input.size(), Write)));
  EXPECT_FALSE(take(Memory->canAccess(Descriptor, 24, Write)));
  EXPECT_FALSE(take(Memory->canAccess(Sealed, 16, Read)));
  EXPECT_FALSE(take(Memory->canAccess(Output, IO.OutputSize, Read)));
  std::array<uint8_t, profile::PointerSize> Pointer;
  success(Memory->readBacking(Output, Pointer));
  EXPECT_EQ(llvm::support::endian::read64le(Pointer.data()), Sealed);
  success(Model->snapshot());
  const auto &Buffers = Result.Requests.back().UserBuffers;
  EXPECT_EQ(Buffers[1].Backing,
            (std::vector<uint8_t>{0x42, 0x51, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQ(Buffers[2].Backing.size(), 16u);
  EXPECT_EQ(Buffers[2].Backing[0], 0x63u);
  EXPECT_EQ(llvm::support::endian::read64le(Buffers[2].Backing.data() + 8),
            Payload + 12);
  EXPECT_FALSE(Buffers[2].Revoked);
  EXPECT_FALSE(Result.Requests.back().Completed);
  complete(IRP);
  success(Model->recordDispatchReturn(IRP, StatusSuccess));
  success(Model->finalizeRequest(IRP));
  success(Model->snapshot());
  EXPECT_EQ(Result.Requests.back().UserBuffers[2].Backing[0], 0x63u);
}

TEST_F(KernelMDLChain, NestedAliasesSurviveUnmapAndFinalSnapshotKeepsBacking) {
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = NeitherIOCTL;
  IO.Input.resize(profile::PointerSize);
  IO.OutputSize = 4;
  IO.UserBuffers = {{"payload", 12, {0x41}}};
  IO.UserPointers = {{{DriverUserBufferKind::Input, {}, 0},
                      {DriverUserBufferKind::Memory, "payload", 4}}};
  const auto IRP = begin(IO);
  const auto Payload = Result.Requests.back().UserBuffers[0].Address;
  const auto Output = get(IRP + IRPUserBufferOffset);
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  const auto MDL = allocate(IRP, false, Payload + 4);
  const auto PeerMDL = allocate(IRP, true, Payload + 4);
  for (auto Descriptor : {MDL, PeerMDL})
    call("MmProbeAndLockPages", {Descriptor, UserMode, IoWriteAccess});
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  const auto PeerAlias =
      call("MmGetSystemAddressForMdlSafe", {PeerMDL, NormalPagePriority});
  call("IoMarkIrpPending", {IRP});
  success(Model->recordDispatchReturn(IRP, StatusPending));
  success(Model->revokeRequestUserBuffers(IRP));
  EXPECT_FALSE(take(Memory->canAccess(Payload, 1, Read)));
  EXPECT_FALSE(take(Memory->canAccess(Output, 1, Read)));
  put(Alias, 0x88776655, sizeof(uint32_t));
  EXPECT_EQ(get(PeerAlias, sizeof(uint32_t)), 0x88776655u);
  success(Model->snapshot());
  EXPECT_TRUE(Result.Requests.back().UserBuffers[0].Revoked);
  EXPECT_EQ(Result.Requests.back().UserBuffers[0].Backing,
            (std::vector<uint8_t>{0x41, 0, 0, 0, 0x55, 0x66, 0x77, 0x88, 0, 0,
                                  0, 0}));
  complete(IRP, IO.OutputSize);
  success(Model->finalizeRequest(IRP));
  success(Model->snapshot());
  EXPECT_TRUE(Result.Requests.back().Output.empty());
  EXPECT_EQ(Result.Requests.back().UserBuffers[0].Backing[4], 0x55u);
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
  EXPECT_FALSE(take(Memory->canAccess(PeerAlias, 1, Read)));
}

TEST_F(KernelMDLChain,
       UserRegionsShareProcessContextAndExitRevokesAllRequests) {
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = NeitherIOCTL;
  IO.UserBuffers = {{"shared", 8, {0x11}}};
  const auto First = begin(IO, 1);
  const auto FirstAddress = Result.Requests.back().UserBuffers[0].Address;
  call("IoMarkIrpPending", {First});
  success(Model->recordDispatchReturn(First, StatusPending));
  const auto Second = begin(IO, 2);
  const auto SecondAddress = Result.Requests.back().UserBuffers[0].Address;
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  // Raw WDM probing uses the current process, independent of request-local IDs.
  const auto SharedMDL = allocate(Second, false, FirstAddress);
  call("MmProbeAndLockPages", {SharedMDL, UserMode, IoWriteAccess});
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {SharedMDL, NormalPagePriority});
  put(Alias, 0x35, 1);
  EXPECT_EQ(get(FirstAddress, 1), 0x35u);
  EXPECT_TRUE(take(Memory->canAccess(SecondAddress, 1, Read)));
  success(Model->setUserRequestContext(
      true, DriverRequest::DefaultRequestorProcessID + 1));
  EXPECT_FALSE(take(Memory->canAccess(FirstAddress, 1, Read)));
  EXPECT_FALSE(take(Memory->canAccess(SecondAddress, 1, Read)));
  EXPECT_EQ(get(Alias, 1), 0x35u);
  success(Model->setUserRequestContext(true));
  EXPECT_EQ(get(FirstAddress, 1), 0x35u);
  call("IoMarkIrpPending", {Second});
  success(Model->recordDispatchReturn(Second, StatusPending));
  success(Model->exitRequestorProcess(First));
  EXPECT_FALSE(take(Memory->canAccess(FirstAddress, 1, Read)));
  EXPECT_FALSE(take(Memory->canAccess(SecondAddress, 1, Read)));
  put(Alias, 0x72, 1);
  complete(Second);
  complete(First);
  success(Model->finalizeRequest(First));
  success(Model->finalizeRequest(Second));
  success(Model->snapshot());
  unsigned Seen = 0;
  for (const auto &Request : Result.Requests)
    for (const auto &Buffer : Request.UserBuffers) {
      EXPECT_TRUE(Buffer.Revoked);
      if (Buffer.Address == FirstAddress)
        EXPECT_EQ(Buffer.Backing[0], 0x72u);
      ++Seen;
    }
  EXPECT_EQ(Seen, 2u);
}

TEST_F(KernelMDLChain, DeviceSelectedBufferedReadRejectsDeclaredUserMemory) {
  open();
  DriverRequest IO;
  IO.Kind = DriverRequestKind::Read;
  IO.File = 1;
  IO.OutputSize = profile::PointerSize;
  IO.UserBuffers = {{"nested", 8, {}}};
  rejected(Model->beginRequest(IO), "requires neither-I/O");
  EXPECT_TRUE(Result.Requests.back().UserBuffers.empty());
  EXPECT_EQ(Result.Requests.back().IRP, 0u);
}

TEST_F(KernelMDLChain, ExitingOneProcessPreservesAnotherProcessUserRegions) {
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = NeitherIOCTL;
  IO.UserBuffers = {{"payload", 8, {0x11}}};
  const auto First = begin(IO, 1);
  const auto FirstAddress = Result.Requests.back().UserBuffers[0].Address;
  call("IoMarkIrpPending", {First});
  success(Model->recordDispatchReturn(First, StatusPending));
  const uint32_t SecondProcess = IO.RequestorProcessID + 1;
  IO.RequestorProcessID = SecondProcess;
  IO.UserBuffers[0].Input = {0x22};
  const auto Second = begin(IO, 2);
  const auto SecondAddress = Result.Requests.back().UserBuffers[0].Address;
  EXPECT_NE(FirstAddress, SecondAddress);
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  EXPECT_EQ(get(FirstAddress, 1), 0x11u);
  EXPECT_FALSE(take(Memory->canAccess(SecondAddress, 1, Read)));
  success(Model->setUserRequestContext(true, SecondProcess));
  EXPECT_FALSE(take(Memory->canAccess(FirstAddress, 1, Read)));
  EXPECT_EQ(get(SecondAddress, 1), 0x22u);
  const auto SecondMDL = allocate(Second, false, SecondAddress);
  call("MmProbeAndLockPages", {SecondMDL, UserMode, IoWriteAccess});
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {SecondMDL, NormalPagePriority});
  call("IoMarkIrpPending", {Second});
  success(Model->recordDispatchReturn(Second, StatusPending));
  success(Model->exitRequestorProcess(First));
  EXPECT_TRUE(take(Memory->canAccess(SecondAddress, 1, Read | Write)));
  put(Alias, 0x44, 1);
  EXPECT_EQ(get(SecondAddress, 1), 0x44u);
  success(Model->setUserRequestContext(true));
  EXPECT_FALSE(take(Memory->canAccess(FirstAddress, 1, Read)));
  EXPECT_FALSE(take(Memory->canAccess(SecondAddress, 1, Read)));
  success(Model->setUserRequestContext(true, SecondProcess));
  EXPECT_FALSE(take(Memory->canAccess(FirstAddress, 1, Read)));
  EXPECT_EQ(get(SecondAddress, 1), 0x44u);
  complete(First);
  complete(Second);
  success(Model->finalizeRequest(First));
  success(Model->finalizeRequest(Second));
  success(Model->snapshot());
  unsigned Seen = 0;
  for (const auto &Request : Result.Requests)
    for (const auto &Buffer : Request.UserBuffers) {
      EXPECT_EQ(Buffer.Revoked, Buffer.Address == FirstAddress);
      EXPECT_EQ(Buffer.Backing[0],
                Buffer.Address == FirstAddress ? 0x11u : 0x44u);
      ++Seen;
    }
  EXPECT_EQ(Seen, 2u);
}

TEST_F(KernelMDLChain, NativeInvalidPageAccessFailsBeforeAllocatingRequest) {
  open();
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.File = 1;
  IO.ControlCode = NeitherIOCTL;
  IO.Input = {0x11};
  IO.OutputSize = 1;
  const auto Invalid = static_cast<DriverUserPageAccess>(-1);
  IO.UserInputAccess = Invalid;
  rejected(Model->beginRequest(IO), "unsupported value");
  EXPECT_EQ(Result.Requests.back().IRP, 0u);
  IO.UserInputAccess.reset();
  IO.UserOutputAccess = Invalid;
  rejected(Model->beginRequest(IO), "unsupported value");
  EXPECT_EQ(Result.Requests.back().IRP, 0u);
  IO.UserOutputAccess.reset();
  const auto IRP = take(Model->beginRequest(IO)).IRP;
  EXPECT_NE(IRP, 0u);
  complete(IRP);
  success(Model->recordDispatchReturn(IRP, StatusSuccess));
  success(Model->finalizeRequest(IRP));
}
} // namespace
} // namespace neverd::emulation
