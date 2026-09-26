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
#include "windows/KernelException.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <initializer_list>

namespace neverd::emulation {
namespace {
using namespace windows;
namespace pool {
#define NEVERD_KERNEL_POOL_FLAG(Name, Value) constexpr uint64_t Name = Value;
#include "windows/KernelPoolFlags.def"
#undef NEVERD_KERNEL_POOL_FLAG
} // namespace pool

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

TEST_F(KernelMDLChain, IndependentPagesKeepDescriptorUntilExplicitPoolFree) {
  constexpr uint64_t Page = profile::PageSize;
  const uint64_t Low = physical::PhysicalBase + 32 * Page;
  const auto MDL = call("MmAllocatePagesForMdlEx",
                        {Low, Low + 2 * Page - 1, 0, 2 * Page, MmWriteCombined,
                         MmAllocateFullyRequired});
  ASSERT_NE(MDL, 0u);
  EXPECT_EQ(get(MDL + MDLByteCountOffset, 4), 2 * Page);
  EXPECT_EQ(get(MDL + MDLSize), Low / Page);
  EXPECT_EQ(get(MDL + MDLSize + profile::PointerSize), Low / Page + 1);
  EXPECT_EQ(get(MDL + MDLMappedSystemVAOffset), 0u);
  rejected(Model->call("MmUnlockPages", {MDL}), "locked user MDL");
  rejected(Model->call("IoFreeMdl", {MDL}), "ExFreePool");
  rejected(Model->call("ExFreePool", {MDL}), "MmFreePagesFromMdl");
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  ASSERT_NE(Alias, 0u);
  EXPECT_EQ(get(Alias), 0u);
  EXPECT_EQ(get(Alias + Page), 0u);
  put(Alias + Page, 0x57);
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority}),
            Alias);
  call("MmFreePagesFromMdl", {MDL});
  EXPECT_EQ(get(MDL + MDLByteCountOffset, 4), 0u);
  EXPECT_EQ(get(MDL + MDLMappedSystemVAOffset), 0u);
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
  rejected(Model->validateGuestAccess(MDL + MDLSize, 8, false), "released MDL");
  rejected(Model->call("MmFreePagesFromMdl", {MDL}), "live independent");
  rejected(Model->call("ExFreePoolWithTag", {MDL, PoolTag}), "untagged");
  call("ExFreePoolWithTag", {MDL, 0});
  rejected(Model->call("ExFreePool", {MDL}), "unknown or already freed");
}

TEST_F(KernelMDLChain,
       IndependentPagesPartialAndRequiredAllocationAreDistinct) {
  constexpr uint64_t Page = profile::PageSize;
  const uint64_t Low = physical::PhysicalBase + 40 * Page;
  EXPECT_EQ(
      call("MmAllocatePagesForMdlEx", {Low, Low + Page - 1, 0, 2 * Page,
                                       MmCached, MmAllocateFullyRequired}),
      0u);
  const auto MDL =
      call("MmAllocatePagesForMdl", {Low, Low + Page - 1, 0, 2 * Page});
  ASSERT_NE(MDL, 0u);
  EXPECT_EQ(get(MDL + MDLByteCountOffset, 4), Page);
  EXPECT_EQ(get(MDL + MDLSize), Low / Page);
  EXPECT_EQ(call("MmAllocatePagesForMdl", {Low, Low + Page - 1, 0, Page}), 0u);
  call("MmFreePagesFromMdl", {MDL});
  call("ExFreePool", {MDL});
  const auto Again =
      call("MmAllocatePagesForMdl", {Low, Low + Page - 1, 0, Page});
  ASSERT_NE(Again, 0u);
  EXPECT_EQ(get(Again + MDLSize), Low / Page);
  call("MmFreePagesFromMdl", {Again});
  call("ExFreePool", {Again});
}

TEST_F(KernelMDLChain,
       IndependentPagePartialDependencyRejectsReleaseAtomically) {
  constexpr uint64_t Page = profile::PageSize;
  const auto MDL = call("MmAllocatePagesForMdl", {0, UINT64_MAX, 0, 2 * Page});
  ASSERT_NE(MDL, 0u);
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  ASSERT_NE(Alias, 0u);
  const auto Partial = call("IoAllocateMdl", {0, Page, 0, 0, 0});
  ASSERT_NE(Partial, 0u);
  call("IoBuildPartialMdl", {MDL, Partial, 0, Page});
  const auto Flags = get(MDL + MDLFlagsOffset, 2);
  const auto PFN = get(MDL + MDLSize);
  rejected(Model->call("MmFreePagesFromMdl", {MDL}), "dependent partial");
  EXPECT_EQ(get(MDL + MDLFlagsOffset, 2), Flags);
  EXPECT_EQ(get(MDL + MDLByteCountOffset, 4), 2 * Page);
  EXPECT_EQ(get(MDL + MDLSize), PFN);
  put(Alias, 0x35);
  EXPECT_EQ(get(Alias), 0x35u);
  call("IoFreeMdl", {Partial});
  call("MmFreePagesFromMdl", {MDL});
  call("ExFreePool", {MDL});
}

TEST_F(KernelMDLChain,
       IndependentPageSelectionHonorsSkipAndRejectsUnknownPolicy) {
  constexpr uint64_t Page = profile::PageSize;
  const uint64_t Low = physical::PhysicalBase + 48 * Page;
  const auto MDL =
      call("MmAllocatePagesForMdl", {Low, Low + Page - 1, 2 * Page, 3 * Page});
  ASSERT_NE(MDL, 0u);
  for (uint64_t I = 0; I != 3; ++I)
    EXPECT_EQ(get(MDL + MDLSize + I * profile::PointerSize),
              Low / Page + 2 * I);
  rejected(Model->call("MmAllocatePagesForMdlEx",
                       {0, UINT64_MAX, 0, Page, MmCached, 0x80000000}),
           "allocation flags");
  rejected(Model->call("MmAllocatePagesForMdlEx",
                       {0, UINT64_MAX, 0, Page, UINT32_MAX, 0}),
           "cache type");
  call("MmFreePagesFromMdl", {MDL});
  call("ExFreePool", {MDL});
}

TEST_F(KernelMDLChain, IndependentPageViewsAndRelocksPreventPrematureRelease) {
  const auto IRP = begin(NeitherIOCTL);
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  const auto MDL =
      call("MmAllocatePagesForMdl", {0, UINT64_MAX, 0, profile::PageSize});
  ASSERT_NE(MDL, 0u);
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  const auto User = call("MmMapLockedPagesSpecifyCache",
                         {MDL, UserMode, MmCached, 0, 0, NormalPagePriority});
  ASSERT_NE(Alias, 0u);
  ASSERT_NE(User, 0u);
  const auto Relock = call("IoAllocateMdl", {User, 4, 0, 0, 0});
  ASSERT_NE(Relock, 0u);
  call("MmProbeAndLockPages", {Relock, UserMode, IoWriteAccess});
  EXPECT_EQ(get(Relock + MDLSize), get(MDL + MDLSize));
  const auto RelockAlias =
      call("MmGetSystemAddressForMdlSafe", {Relock, NormalPagePriority});
  ASSERT_NE(RelockAlias, 0u);
  std::vector<uint8_t> Before(get(MDL + MDLSizeOffset, 2));
  success(Memory->read(MDL, Before));
  auto Unchanged = [&] {
    std::vector<uint8_t> After(Before.size());
    success(Memory->read(MDL, After));
    EXPECT_EQ(After, Before);
    put(Alias, 0x78563412, 4);
    EXPECT_EQ(get(RelockAlias, 4), 0x78563412u);
  };
  rejected(Model->call("MmFreePagesFromMdl", {MDL}), "user mapping");
  Unchanged();
  call("MmUnmapLockedPages", {User, MDL});
  rejected(Model->call("MmFreePagesFromMdl", {MDL}), "pinned owner");
  Unchanged();
  call("MmUnlockPages", {Relock});
  call("IoFreeMdl", {Relock});
  call("MmFreePagesFromMdl", {MDL});
  call("ExFreePool", {MDL});
  complete(IRP);
}

TEST_F(KernelMDLChain, IndependentPageOwnershipCannotBeTransferredToIrp) {
  const auto IRP = begin();
  const auto MDL =
      call("MmAllocatePagesForMdl", {0, UINT64_MAX, 0, profile::PageSize});
  ASSERT_NE(MDL, 0u);
  put(IRP + IRPMdlOffset, MDL);
  status(IRP);
  rejected(Model->call("IofCompleteRequest", {IRP, 0}),
           "cannot transfer ownership");
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_EQ(get(IRP + IRPMdlOffset), MDL);
  EXPECT_EQ(get(MDL + MDLByteCountOffset, 4), profile::PageSize);
  put(IRP + IRPMdlOffset, 0);
  complete(IRP);
  call("MmFreePagesFromMdl", {MDL});
  call("ExFreePool", {MDL});
}

TEST_F(KernelMDLChain, ContiguousChunksAllowOnlyWholeAlignedPartialResults) {
  constexpr uint64_t Page = profile::PageSize;
  const uint64_t End = physical::PhysicalBase + physical::PhysicalSize;
  const uint64_t Low = End - 5 * Page;
  EXPECT_EQ(call("MmAllocatePagesForMdlEx",
                 {Low, End - 1, 2 * Page, 6 * Page, MmCached,
                  MmAllocateRequireContiguousChunks | MmAllocateFullyRequired}),
            0u);
  const auto MDL = call("MmAllocatePagesForMdlEx",
                        {Low, End - 1, 2 * Page, 6 * Page, MmCached,
                         MmAllocateRequireContiguousChunks});
  ASSERT_NE(MDL, 0u);
  EXPECT_EQ(get(MDL + MDLByteCountOffset, 4), 4 * Page);
  for (uint64_t I = 0; I != 4; ++I)
    EXPECT_EQ(get(MDL + MDLSize + I * profile::PointerSize),
              End / Page - 4 + I);
  rejected(Model->call("MmAllocatePagesForMdlEx",
                       {Low, End - 1, 2 * Page, 3 * Page, MmCached,
                        MmAllocateRequireContiguousChunks}),
           "whole chunk");
  call("MmFreePagesFromMdl", {MDL});
  call("ExFreePool", {MDL});
}

TEST_F(KernelMDLChain, IrpPartialBorrowsIndependentPagesUntilCompletion) {
  for (bool MappedRoot : {false, true}) {
    SCOPED_TRACE(MappedRoot);
    const auto IRP = begin(BufferedIOCTL, MappedRoot ? 2 : 1);
    const auto Root = call("MmAllocatePagesForMdl",
                           {0, UINT64_MAX, 0, 2 * profile::PageSize});
    ASSERT_NE(Root, 0u);
    uint64_t System = 0;
    if (MappedRoot)
      System = call("MmGetSystemAddressForMdlSafe", {Root, NormalPagePriority});
    const auto Partial = call(
        "IoAllocateMdl", {profile::PageSize, profile::PageSize, 0, 0, IRP});
    ASSERT_NE(Partial, 0u);
    call("IoBuildPartialMdl",
         {Root, Partial, profile::PageSize, profile::PageSize});
    EXPECT_EQ(get(Partial + MDLSize),
              get(Root + MDLSize + profile::PointerSize));
    const auto Alias =
        call("MmGetSystemAddressForMdlSafe", {Partial, NormalPagePriority});
    ASSERT_NE(Alias, 0u);
    put(Alias, 0x78563412, 4);
    complete(IRP);
    rejected(Model->validateGuestAccess(Partial, 1, false), "freed");
    EXPECT_EQ(get(Root + MDLByteCountOffset, 4), 2 * profile::PageSize);
    if (!MappedRoot) {
      EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
      System = call("MmGetSystemAddressForMdlSafe", {Root, NormalPagePriority});
      EXPECT_EQ(System, Alias);
    }
    EXPECT_EQ(get(System + profile::PageSize, 4), 0x78563412u);
    call("MmFreePagesFromMdl", {Root});
    call("ExFreePool", {Root});
    success(Model->recordDispatchReturn(IRP, StatusSuccess));
    success(Model->finalizeRequest(IRP));
  }
}

TEST_F(KernelMDLChain, SystemMappingReleaseReusesAddressWithNewPageAuthority) {
  constexpr uint64_t Page = profile::PageSize;
  const auto First =
      call("MmAllocatePagesForMdl", {0, UINT64_MAX, 0, 2 * Page});
  const auto Second = call("MmAllocatePagesForMdl", {0, UINT64_MAX, 0, Page});
  ASSERT_NE(First, 0u);
  ASSERT_NE(Second, 0u);
  const auto A =
      call("MmGetSystemAddressForMdlSafe", {First, NormalPagePriority});
  const auto B =
      call("MmGetSystemAddressForMdlSafe", {Second, NormalPagePriority});
  ASSERT_NE(A, 0u);
  ASSERT_NE(B, 0u);
  EXPECT_EQ(B, A + 2 * Page);
  put(A, 0x51, 1);
  put(B, 0x72, 1);
  call("MmUnmapLockedPages", {A, First});
  EXPECT_FALSE(take(Memory->canAccess(A, 1, Read)));
  const auto Partial = call("IoAllocateMdl", {0, Page, 0, 0, 0});
  call("IoBuildPartialMdl", {First, Partial, Page, Page});
  const auto Reused = call("MmGetSystemAddressForMdlSafe",
                           {Partial, NormalPagePriority | MdlMappingNoWrite});
  EXPECT_EQ(Reused, A);
  EXPECT_EQ(get(Reused, 1), 0u);
  EXPECT_FALSE(take(Memory->canAccess(Reused, 1, Write)));
  EXPECT_EQ(get(B, 1), 0x72u);
  call("IoFreeMdl", {Partial});
  EXPECT_FALSE(take(Memory->canAccess(Reused, 1, Read)));
  const auto Again =
      call("MmGetSystemAddressForMdlSafe", {First, NormalPagePriority});
  EXPECT_EQ(Again, A);
  EXPECT_TRUE(take(Memory->canAccess(Again, 1, Write)));
  EXPECT_EQ(get(Again, 1), 0x51u);
  for (auto MDL : {First, Second}) {
    call("MmFreePagesFromMdl", {MDL});
    call("ExFreePool", {MDL});
  }
}

TEST_F(KernelMDLChain, ProtectionUsesActualPagesAcrossBorrowedPartialMappings) {
  const auto Root =
      call("MmAllocatePagesForMdl", {0, UINT64_MAX, 0, 2 * profile::PageSize});
  ASSERT_NE(Root, 0u);
  EXPECT_EQ(call("MmProtectMdlSystemAddress", {Root, PageReadOnly}),
            StatusNotMappedView);
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {Root, NormalPagePriority});
  const auto Partial = call("IoAllocateMdl", {profile::PageSize, 8, 0, 0, 0});
  ASSERT_NE(Alias, 0u);
  ASSERT_NE(Partial, 0u);
  call("IoBuildPartialMdl", {Root, Partial, profile::PageSize, 8});
  EXPECT_EQ(call("MmProtectMdlSystemAddress", {Root, PageReadOnly}),
            StatusSuccess);
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Write)));
  EXPECT_FALSE(take(Memory->canAccess(Alias + profile::PageSize, 1, Write)));
  EXPECT_EQ(call("MmProtectMdlSystemAddress", {Partial, PageReadWrite}),
            StatusSuccess);
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Write)));
  put(Alias + profile::PageSize, 0x48, 1);
  EXPECT_EQ(get(Alias + profile::PageSize, 1), 0x48u);
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {Root, NormalPagePriority}),
            Alias);
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Write)));
  call("IoFreeMdl", {Partial});
  call("MmFreePagesFromMdl", {Root});
  call("ExFreePool", {Root});
}

TEST_F(KernelMDLChain,
       ProtectionModesPreserveCacheAndIndependentUserPermissions) {
  const auto IRP = begin(NeitherIOCTL);
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  const auto MDL =
      call("MmAllocatePagesForMdl", {0, UINT64_MAX, 0, profile::PageSize});
  const auto Alias = call("MmGetSystemAddressForMdlSafe",
                          {MDL, NormalPagePriority | MdlMappingNoWrite});
  const auto User = call("MmMapLockedPagesSpecifyCache",
                         {MDL, UserMode, MmCached, 0, 0, NormalPagePriority});
  ASSERT_NE(Alias, 0u);
  ASSERT_NE(User, 0u);
  const auto PFN = get(MDL + MDLSize);
  const std::array<std::pair<uint32_t, unsigned>, 6> Protections{
      {{PageNoAccess, 0},
       {PageReadOnly, Read},
       {PageReadWrite, Read | Write},
       {PageExecute, Execute},
       {PageExecuteRead, Execute | Read},
       {PageExecuteReadWrite, Execute | Read | Write}}};
  for (auto [Protection, Permissions] : Protections) {
    SCOPED_TRACE(Protection);
    EXPECT_EQ(call("MmProtectMdlSystemAddress", {MDL, Protection}),
              StatusSuccess);
    for (auto Bit : {Read, Write, Execute})
      EXPECT_EQ(take(Memory->canAccess(Alias, 1, Bit)),
                bool(Permissions & Bit));
    EXPECT_TRUE(take(Memory->canAccess(User, 1, Read | Write)));
    EXPECT_FALSE(take(Memory->canAccess(User, 1, Execute)));
    EXPECT_EQ(get(MDL + MDLSize), PFN);
  }
  EXPECT_EQ(
      call("MmProtectMdlSystemAddress", {MDL, PageReadWrite | PageNoCache}),
      StatusInvalidPageProtection);
  EXPECT_TRUE(take(Memory->canAccess(Alias, 1, Read | Write | Execute)));
  call("MmUnmapLockedPages", {User, MDL});
  call("MmUnmapLockedPages", {Alias, MDL});
  EXPECT_EQ(call("MmProtectMdlSystemAddress", {MDL, PageReadOnly}),
            StatusNotMappedView);
  call("MmFreePagesFromMdl", {MDL});
  call("ExFreePool", {MDL});
  complete(IRP);
}

TEST_F(KernelMDLChain, KernelPoolLocksPreserveResidencyAndRejectEarlyFree) {
  Model->enterExecution(profile::StackBase);
  constexpr uint64_t Page = profile::PageSize;
  const auto Paged = call("ExAllocatePool2", {pool::Paged, 2 * Page, PoolTag});
  ASSERT_NE(Paged, 0u);
  const auto MDL = call("IoAllocateMdl", {Paged + 17, 4, 0, 0, 0});
  ASSERT_NE(MDL, 0u);
  const auto Old = call("KfRaiseIrql", {scheduler::DispatchLevel});
  rejected(Model->call("MmProbeAndLockPages", {MDL, KernelMode, IoWriteAccess}),
           "APC_LEVEL");
  EXPECT_EQ(get(MDL + MDLFlagsOffset, 2), 0u);
  call("KeLowerIrql", {Old});
  call("MmProbeAndLockPages", {MDL, KernelMode, IoWriteAccess});
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  ASSERT_NE(Alias, 0u);
  rejected(Model->call("ExFreePool", {Paged}), "pinned view");
  call("KfRaiseIrql", {scheduler::DispatchLevel});
  put(Paged, 0x67, 1);
  put(Alias, 0x12345678, 4);
  EXPECT_EQ(get(Paged + 17, 4), 0x12345678u);
  rejected(Model->validateGuestAccess(Paged + Page, 1, false),
           "paged pool access");
  call("MmUnlockPages", {MDL});
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
  rejected(Model->validateGuestAccess(Paged, 1, false), "paged pool access");
  call("KeLowerIrql", {Old});
  call("IoFreeMdl", {MDL});
  rejected(Model->call("ExFreePoolWithTag", {Paged, PoolTag + 1}),
           "tag does not match");
  call("ExFreePoolWithTag", {Paged, 0});
}

TEST_F(KernelMDLChain,
       KernelProbeFaultAndReadLockKeepDescriptorAndAccessContract) {
  const auto Paged =
      call("ExAllocatePool2", {pool::Paged, profile::PageSize, PoolTag});
  const auto MDL = call("IoAllocateMdl", {Paged, profile::PageSize, 0, 0, 0});
  ASSERT_NE(Paged, 0u);
  ASSERT_NE(MDL, 0u);
  success(Memory->protect(Paged, profile::PageSize, Read));
  auto Failed =
      Model->call("MmProbeAndLockPages", {MDL, KernelMode, IoWriteAccess});
  ASSERT_FALSE(bool(Failed));
  auto Error = Failed.takeError();
  EXPECT_TRUE(Error.isA<KernelGuestException>());
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(get(MDL + MDLFlagsOffset, 2), 0u);
  call("MmProbeAndLockPages", {MDL, KernelMode, IoReadAccess});
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority});
  EXPECT_TRUE(take(Memory->canAccess(Alias, 1, Read)));
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Write)));
  rejected(Model->call("MmProtectMdlSystemAddress", {MDL, PageReadWrite}),
           "write-access contract");
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
  success(Memory->protect(Paged, profile::PageSize, Read | Write));
  call("ExFreePool", {Paged});
}

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

TEST_F(KernelMDLChain, PartialOfNonpagedMdlBorrowsItsMappingAndPhysicalPage) {
  const auto Source = call("IoAllocateMdl", {Pool, 64, 0, 0, 0});
  const auto Partial = call("IoAllocateMdl", {Pool + 8, 32, 0, 0, 0});
  call("MmBuildMdlForNonPagedPool", {Source});
  call("IoBuildPartialMdl", {Source, Partial, Pool + 8, 16});
  EXPECT_EQ(get(Partial + MDLStartVAOffset), Pool & ~(profile::PageSize - 1));
  EXPECT_EQ(get(Partial + MDLByteOffsetOffset, 4),
            (Pool + 8) & (profile::PageSize - 1));
  EXPECT_EQ(get(Partial + MDLByteCountOffset, 4), 16u);
  EXPECT_EQ(get(Partial + MDLSize), get(Source + MDLSize));
  EXPECT_EQ(get(Partial + MDLMappedSystemVAOffset), Pool + 8);
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {Partial, NormalPagePriority}),
            Pool + 8);
  rejected(Model->call("MmUnmapLockedPages", {Pool + 8, Partial}), "borrows");
  call("IoFreeMdl", {Source});
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {Partial, NormalPagePriority}),
            Pool + 8);
  put(Pool + 8, 0x78563412, 4);
  EXPECT_EQ(get(Pool + 8, 4), 0x78563412u);
  call("IoFreeMdl", {Partial});
  call("ExFreePoolWithTag", {Pool, PoolTag});
}

TEST_F(KernelMDLChain, PartialUserMdlOwnsOnlyItsNewSystemMapping) {
  const uint64_t IRP = begin(NeitherIOCTL);
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  const auto User = get(IRP + IRPUserBufferOffset);
  const auto Source = call("IoAllocateMdl", {User, 4, 0, 0, 0});
  const auto Partial = call("IoAllocateMdl", {User + 1, 3, 0, 0, 0});
  call("MmProbeAndLockPages", {Source, UserMode, IoWriteAccess});
  call("IoBuildPartialMdl", {Source, Partial, User + 1, 0});
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {Partial, NormalPagePriority});
  EXPECT_NE(Alias, 0u);
  put(Alias, 0x4738, 2);
  EXPECT_EQ(get(User + 1, 2), 0x4738u);
  rejected(Model->call("MmUnlockPages", {Source}), "dependent partial");
  call("MmUnmapLockedPages", {Alias, Partial});
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
  call("IoFreeMdl", {Partial});
  call("MmUnlockPages", {Source});
  call("IoFreeMdl", {Source});
  complete(IRP);
}

TEST_F(KernelMDLChain, PartialPoolViewsOutliveIntermediateDescriptors) {
  const auto Source = call("IoAllocateMdl", {Pool, 64, 0, 0, 0});
  const auto Partial = call("IoAllocateMdl", {Pool, 64, 0, 0, 0});
  const auto Leaf = call("IoAllocateMdl", {Pool, 64, 0, 0, 0});
  call("MmBuildMdlForNonPagedPool", {Source});
  call("IoBuildPartialMdl", {Source, Partial, Pool + 8, 32});
  call("IoBuildPartialMdl", {Partial, Leaf, Pool + 16, 8});
  const auto PFN = get(Source + MDLSize);
  call("IoFreeMdl", {Source});
  call("IoFreeMdl", {Partial});
  EXPECT_EQ(get(Leaf + MDLSize), PFN);
  EXPECT_EQ(get(Leaf + MDLFlagsOffset, 2),
            MDLPartial | MDLMappedToSystemVA | MDLParentMappedSystemVA);
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe",
                 {Leaf, NormalPagePriority | MdlMappingNoWrite}),
            Pool + 16);
  // A child view must not narrow the parent allocation's existing mapping.
  put(Pool, 0x19, 1);
  put(Pool + 63, 0x27, 1);
  call("KeFlushIoBuffers", {Leaf, 0, 0});
  call("ExFreePoolWithTag", {Pool, PoolTag});
  rejected(
      Model->call("MmGetSystemAddressForMdlSafe", {Leaf, NormalPagePriority}),
      "physical");
  rejected(Model->call("KeFlushIoBuffers", {Leaf, 0, 0}), "physical");
  call("IoFreeMdl", {Leaf});
}

TEST_F(KernelMDLChain,
       PartialReusePreservesNextAndChecksCapacityBeforeMutation) {
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = NeitherIOCTL;
  IO.OutputSize = 3 * profile::PageSize;
  const auto IRP = begin(IO);
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  const auto User = get(IRP + IRPUserBufferOffset);
  const auto Source = call("IoAllocateMdl", {User, IO.OutputSize, 0, 0, 0});
  const auto Partial =
      call("IoAllocateMdl", {User, profile::PageSize, 0, 0, 0});
  const auto Tail = allocate(0, false);
  put(Partial + MDLNextOffset, Tail);
  call("MmProbeAndLockPages", {Source, UserMode, IoWriteAccess});
  call("IoBuildPartialMdl", {Source, Partial, User + 8, 16});
  const auto PFN = get(Partial + MDLSize);
  const auto Capacity = get(Partial + MDLSizeOffset, 2);
  auto Record = [&] {
    std::vector<uint8_t> Bytes(Capacity);
    success(Memory->read(Partial, Bytes));
    return Bytes;
  };
  const auto Before = Record();
  rejected(Model->call("IoBuildPartialMdl",
                       {Source, Partial, User + profile::PageSize - 1, 2}),
           "PFN capacity");
  EXPECT_EQ(Record(), Before);
  rejected(Model->call("IoBuildPartialMdl",
                       {Source, Partial, User + IO.OutputSize, 0}),
           "outside");
  EXPECT_EQ(Record(), Before);
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {Partial, NormalPagePriority});
  const auto Mapped = Record();
  rejected(Model->call("IoBuildPartialMdl", {Source, Partial, User + 32, 8}),
           "MmPrepareMdlForReuse");
  EXPECT_EQ(Record(), Mapped);
  // MmPrepareMdlForReuse's WDK inline calls MmUnmapLockedPages only for a
  // partial MDL carrying MDL_PARTIAL_HAS_BEEN_MAPPED.
  ASSERT_NE(get(Partial + MDLFlagsOffset, 2) & MDLPartialHasBeenMapped, 0u);
  call("MmUnmapLockedPages", {get(Partial + MDLMappedSystemVAOffset), Partial});
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
  EXPECT_EQ(get(Partial + MDLFlagsOffset, 2), MDLPartial);
  EXPECT_EQ(get(Partial + MDLByteCountOffset, 4), 16u);
  EXPECT_EQ(get(Partial + MDLSize), PFN);
  call("IoBuildPartialMdl",
       {Source, Partial, User + profile::PageSize + 4, 12});
  EXPECT_EQ(get(Partial + MDLNextOffset), Tail);
  EXPECT_EQ(get(Partial + MDLSizeOffset, 2), Capacity);
  EXPECT_EQ(get(Partial + MDLSize),
            get(Source + MDLSize + profile::PointerSize));
  const auto ReusedAlias =
      call("MmGetSystemAddressForMdlSafe", {Partial, NormalPagePriority});
  put(ReusedAlias, 0x62, 1);
  EXPECT_EQ(get(User + profile::PageSize + 4, 1), 0x62u);
  call("IoFreeMdl", {Partial});
  EXPECT_FALSE(take(Memory->canAccess(ReusedAlias, 1, Read)));
  call("IoFreeMdl", {Tail});
  call("MmUnlockPages", {Source});
  call("IoFreeMdl", {Source});
  complete(IRP);
}

TEST_F(KernelMDLChain,
       BorrowedAndIndependentPartialMappingsHaveSeparateOwners) {
  const auto IRP = begin(NeitherIOCTL);
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  const auto User = get(IRP + IRPUserBufferOffset);
  const auto Source = allocate(0, false, User);
  const auto Independent = allocate(0, false, User);
  const auto Borrowed = allocate(0, false, User);
  const auto Leaf = allocate(0, false, User);
  call("MmProbeAndLockPages", {Source, UserMode, IoReadAccess});
  call("IoBuildPartialMdl", {Source, Independent, User + 1, 3});
  const auto IndependentAlias =
      call("MmGetSystemAddressForMdlSafe", {Independent, NormalPagePriority});
  call("IoBuildPartialMdl", {Independent, Leaf, User + 2, 2});
  const auto SourceAlias =
      call("MmGetSystemAddressForMdlSafe", {Source, NormalPagePriority});
  call("IoBuildPartialMdl", {Source, Borrowed, User + 1, 3});
  EXPECT_EQ(get(Borrowed + MDLMappedSystemVAOffset), SourceAlias + 1);
  EXPECT_EQ(get(Leaf + MDLMappedSystemVAOffset), IndependentAlias + 1);
  rejected(Model->call("MmUnmapLockedPages", {SourceAlias, Source}),
           "dependent partial");
  rejected(Model->call("IoFreeMdl", {Independent}), "dependent partial");
  rejected(Model->call("MmUnlockPages", {Leaf}), "locked user MDL");
  EXPECT_FALSE(take(Memory->canAccess(IndependentAlias, 1, Write)));
  call("IoFreeMdl", {Borrowed});
  call("MmUnmapLockedPages", {SourceAlias, Source});
  EXPECT_TRUE(take(Memory->canAccess(IndependentAlias, 1, Read)));
  // Dropping the intermediate descriptor is safe after its only mapping
  // borrower is rebuilt against the unmapped root.
  call("IoBuildPartialMdl", {Source, Leaf, User, 4});
  call("IoFreeMdl", {Independent});
  EXPECT_FALSE(take(Memory->canAccess(IndependentAlias, 1, Read)));
  rejected(Model->call("MmUnlockPages", {Source}), "dependent partial");
  call("IoFreeMdl", {Leaf});
  call("MmUnlockPages", {Source});
  call("IoFreeMdl", {Source});
  complete(IRP);
}

TEST_F(KernelMDLChain, CompletionRetiresPartialDependenciesInEitherChainOrder) {
  for (bool SourceFirst : {false, true}) {
    DriverRequest IO;
    IO.Kind = DriverRequestKind::DeviceControl;
    IO.ControlCode = NeitherIOCTL;
    IO.OutputSize = 32;
    const auto IRP = begin(IO, SourceFirst ? 2 : 1);
    Model->enterExecution(profile::StackBase);
    success(Model->setUserRequestContext(true));
    const auto User = get(IRP + IRPUserBufferOffset);
    const auto Source = call("IoAllocateMdl", {User, 32, 0, 0, IRP});
    const auto Partial = call("IoAllocateMdl", {User, 32, 1, 0, IRP});
    const auto Leaf = call("IoAllocateMdl", {User, 32, 1, 0, IRP});
    call("MmProbeAndLockPages", {Source, UserMode, IoWriteAccess});
    call("IoBuildPartialMdl", {Source, Partial, User, 32});
    const auto Alias =
        call("MmGetSystemAddressForMdlSafe", {Partial, NormalPagePriority});
    call("IoBuildPartialMdl", {Partial, Leaf, User + 8, 16});
    if (!SourceFirst) {
      put(IRP + IRPMdlOffset, Leaf);
      put(Leaf + MDLNextOffset, Partial);
      put(Partial + MDLNextOffset, Source);
      put(Source + MDLNextOffset, 0);
    }
    call("KeInitializeSpinLock", {Alias});
    const auto PreviousIRQL = call("KfRaiseIrql", {scheduler::DispatchLevel});
    call("KeAcquireSpinLockAtDpcLevel", {Alias});
    status(IRP);
    const auto Stack = get(IRP + IRPStackPointerOffset);
    const auto Location = get(IRP + IRPLocationOffset, 1);
    rejected(Model->call("IofCompleteRequest", {IRP, 0}), "spin lock");
    EXPECT_FALSE(Result.Requests.back().Completed);
    EXPECT_TRUE(Result.Requests.back().Output.empty());
    EXPECT_EQ(get(IRP + IRPStackPointerOffset), Stack);
    EXPECT_EQ(get(IRP + IRPLocationOffset, 1), Location);
    for (auto MDL : {Source, Partial, Leaf})
      success(Model->validateGuestAccess(MDL, profile::PointerSize, false));
    EXPECT_TRUE(take(Memory->canAccess(Alias, 32, Read | Write)));
    call("KeReleaseSpinLockFromDpcLevel", {Alias});
    call("KeLowerIrql", {PreviousIRQL});
    put(Alias + 8, 0x72, 1);
    complete(IRP, 16);
    ASSERT_EQ(Result.Requests.back().Output.size(), 16u);
    EXPECT_EQ(Result.Requests.back().Output[8], 0x72u);
    for (auto MDL : {Source, Partial, Leaf})
      rejected(Model->validateGuestAccess(MDL, 1, false), "freed");
    EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
    success(Model->recordDispatchReturn(IRP, StatusSuccess));
    success(Model->finalizeRequest(IRP));
  }
}

TEST_F(KernelMDLChain, CompletionRejectsPartialOutsideTheRetiringChain) {
  const auto IRP = begin(DirectIOCTL);
  const auto Source = get(IRP + IRPMdlOffset);
  const auto Original =
      get(Source + MDLStartVAOffset) + get(Source + MDLByteOffsetOffset, 4);
  const auto Partial = call("IoAllocateMdl", {Original, 4, 0, 0, 0});
  call("IoBuildPartialMdl", {Source, Partial, Original, 4});
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {Partial, NormalPagePriority});
  put(Alias, 0x87654321, 4);
  status(IRP, 4);
  const auto Stack = get(IRP + IRPStackPointerOffset);
  const auto Location = get(IRP + IRPLocationOffset, 1);
  rejected(Model->call("IofCompleteRequest", {IRP, 0}), "dependent partial");
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_EQ(get(IRP + IRPStackPointerOffset), Stack);
  EXPECT_EQ(get(IRP + IRPLocationOffset, 1), Location);
  EXPECT_TRUE(Result.Requests.back().Output.empty());
  EXPECT_EQ(Result.Requests.back().Information, 0u);
  EXPECT_EQ(get(Alias, 4), 0x87654321u);
  success(Model->validateGuestAccess(Source, profile::PointerSize, false));
  put(Source + MDLNextOffset, Partial);
  complete(IRP, 4);
  EXPECT_EQ(Result.Requests.back().Output,
            (std::vector<uint8_t>{0x21, 0x43, 0x65, 0x87}));
  EXPECT_FALSE(take(Memory->canAccess(Alias, 1, Read)));
  success(Model->recordDispatchReturn(IRP, StatusSuccess));
  success(Model->finalizeRequest(IRP));
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

TEST_F(KernelMDLChain, CompletionRetiresRelockedDirectViewsInEitherChainOrder) {
  for (bool RootFirst : {false, true}) {
    SCOPED_TRACE(RootFirst);
    const auto IRP = begin(DirectIOCTL, RootFirst ? 2 : 1);
    Model->enterExecution(profile::StackBase);
    success(Model->setUserRequestContext(true));
    const auto Root = get(IRP + IRPMdlOffset);
    const auto User =
        call("MmMapLockedPagesSpecifyCache",
             {Root, UserMode, MmCached, 0, 0, NormalPagePriority});
    const auto First = allocate(IRP, true, User);
    const auto Second = allocate(IRP, true, User);
    for (auto MDL : {First, Second})
      call("MmProbeAndLockPages", {MDL, UserMode, IoWriteAccess});
    const auto Partial = allocate(IRP, true, User);
    call("IoBuildPartialMdl", {First, Partial, User, sizeof(uint32_t)});
    const auto Alias =
        call("MmGetSystemAddressForMdlSafe", {Partial, NormalPagePriority});
    const auto PeerAlias =
        call("MmGetSystemAddressForMdlSafe", {Second, NormalPagePriority});
    if (!RootFirst) {
      put(IRP + IRPMdlOffset, Partial);
      put(Partial + MDLNextOffset, Second);
      put(Second + MDLNextOffset, First);
      put(First + MDLNextOffset, Root);
      put(Root + MDLNextOffset, 0);
    }
    call("MmUnmapLockedPages", {User, Root});
    EXPECT_FALSE(take(Memory->canAccess(User, 1, Read)));
    put(Alias, 0x78563412, sizeof(uint32_t));
    EXPECT_EQ(get(PeerAlias, sizeof(uint32_t)), 0x78563412u);
    complete(IRP, sizeof(uint32_t));
    EXPECT_TRUE(Result.Requests.back().Completed);
    EXPECT_EQ(Result.Requests.back().Output,
              (std::vector<uint8_t>{0x12, 0x34, 0x56, 0x78}));
    for (auto MDL : {Root, First, Second, Partial})
      rejected(Model->validateGuestAccess(MDL, 1, false), "freed");
    for (auto Address : {User, Alias, PeerAlias})
      EXPECT_FALSE(take(Memory->canAccess(Address, 1, Read)));
    success(Model->recordDispatchReturn(IRP, StatusSuccess));
    success(Model->finalizeRequest(IRP));
  }
}

TEST_F(KernelMDLChain, ExternalDirectViewLockRejectsCompletionBeforeMutation) {
  const auto IRP = begin(DirectIOCTL);
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true));
  const auto Root = get(IRP + IRPMdlOffset);
  const auto User = call("MmMapLockedPagesSpecifyCache",
                         {Root, UserMode, MmCached, 0, 0, NormalPagePriority});
  const auto First = allocate(IRP, true, User);
  const auto Second = allocate(IRP, true, User);
  const auto External = allocate(0, false, User);
  for (auto MDL : {First, Second, External})
    call("MmProbeAndLockPages", {MDL, UserMode, IoWriteAccess});
  const auto Alias =
      call("MmGetSystemAddressForMdlSafe", {First, NormalPagePriority});
  const auto ExternalAlias =
      call("MmGetSystemAddressForMdlSafe", {External, NormalPagePriority});
  call("MmUnmapLockedPages", {User, Root});
  put(Alias, 0x78563412, sizeof(uint32_t));
  status(IRP, sizeof(uint32_t));
  auto Bytes = [&](uint64_t Address, uint64_t Size) {
    std::vector<uint8_t> Value(Size);
    success(Memory->read(Address, Value));
    return Value;
  };
  const auto IRPBefore = Bytes(IRP, IRPSize);
  const std::array Descriptors{Root, First, Second, External};
  std::array<std::vector<uint8_t>, Descriptors.size()> Before;
  for (size_t I = 0; I < Descriptors.size(); ++I)
    Before[I] = Bytes(Descriptors[I], get(Descriptors[I] + MDLSizeOffset, 2));
  rejected(Model->call("IofCompleteRequest", {IRP, 0}), "pinned owner");
  EXPECT_EQ(Bytes(IRP, IRPSize), IRPBefore);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_TRUE(Result.Requests.back().Output.empty());
  EXPECT_EQ(Result.Requests.back().Information, 0u);
  for (size_t I = 0; I < Descriptors.size(); ++I) {
    success(Model->validateGuestAccess(Descriptors[I], 1, false));
    EXPECT_EQ(Bytes(Descriptors[I], Before[I].size()), Before[I]);
  }
  EXPECT_EQ(get(Alias, sizeof(uint32_t)), 0x78563412u);
  EXPECT_EQ(get(ExternalAlias, sizeof(uint32_t)), 0x78563412u);
  call("MmUnlockPages", {External});
  call("IoFreeMdl", {External});
  complete(IRP, sizeof(uint32_t));
  EXPECT_TRUE(Result.Requests.back().Completed);
  EXPECT_EQ(Result.Requests.back().Output,
            (std::vector<uint8_t>{0x12, 0x34, 0x56, 0x78}));
  for (auto MDL : Descriptors)
    rejected(Model->validateGuestAccess(MDL, 1, false), "freed");
  for (auto Address : {User, Alias, ExternalAlias})
    EXPECT_FALSE(take(Memory->canAccess(Address, 1, Read)));
  success(Model->recordDispatchReturn(IRP, StatusSuccess));
  success(Model->finalizeRequest(IRP));
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
