//===- KernelNativeMDLTests.cpp - Ordinary Windows nonpaged MDL aliases ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Ordinary Windows nonpaged MDL aliases.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "neverd/emulation/AddressSpace.h"

namespace neverd::emulation {
namespace {
class KernelNativeMDL : public ::testing::Test {
protected:
  std::unique_ptr<UnicornBackend> CPU;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t Pool = 0, MDL = 0;
  void SetUp() override {
    CPU = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
    Model = std::make_unique<KernelModel>(*CPU, Result);
    DriverOptions Options;
    Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    Options.MDLMappingPolicy = DriverMDLMappingPolicy::Windows19041;
    Options.PageTables = true;
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    llvm::cantFail(Model->initialize(Image, Options));
    Model->enterExecution(profile::StackBase);
    llvm::cantFail(Model->synchronizeGuestPageTables());
    Pool = call("ExAllocatePoolWithTag", {0, 128, 1});
    MDL = call("IoAllocateMdl", {Pool, 128, 0, 0, 0});
    call("MmBuildMdlForNonPagedPool", {MDL});
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Args) {
    auto Value = Model->call(Name, Args);
    if (!Value) {
      ADD_FAILURE() << Name << ": " << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }
  std::string denied(const char *Name, std::initializer_list<uint64_t> Args) {
    auto Value = Model->call(Name, Args);
    if (Value) {
      ADD_FAILURE() << Name << " unexpectedly succeeded";
      return {};
    }
    return llvm::toString(Value.takeError());
  }
  uint64_t integer(uint64_t VA, unsigned Size = 8) {
    return llvm::cantFail(CPU->readInteger(VA, Size));
  }
  uint64_t map() {
    return call("MmMapLockedPagesSpecifyCache", {MDL, 0x1000, 0, 0, 0, 16});
  }
};

TEST_F(KernelNativeMDL, AliasPinsPoolAndUnmapRestoresItsBorrowedSystemAddress) {
  const auto Alias = map();
  ASSERT_NE(Alias, 0u);
  ASSERT_NE(Alias, Pool);
  EXPECT_EQ(integer(MDL + 0x18), Alias);
  EXPECT_EQ(integer(MDL + 0xa, 2), 5u);
  EXPECT_EQ(call("MmGetPhysicalAddress", {Pool}),
            call("MmGetPhysicalAddress", {Alias}));
  llvm::cantFail(CPU->writeInteger(Alias + 17, 0x1234, 8));
  EXPECT_EQ(integer(Pool + 17), 0x1234u);
  EXPECT_NE(denied("ExFreePoolWithTag", {Pool, 1}).find("pinned"),
            std::string::npos);
  EXPECT_NE(denied("IoFreeMdl", {MDL}).find("unmapping"), std::string::npos);
  EXPECT_NE(denied("MmMapLockedPagesSpecifyCache", {MDL, 0, 1, 0, 0, 16})
                .find("already"),
            std::string::npos);
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {MDL, 0x40000010}), Alias);
  EXPECT_TRUE(llvm::cantFail(CPU->canAccess(Alias, 128, Write)));
  EXPECT_FALSE(denied("MmUnmapLockedPages", {Pool, MDL}).empty());
  call("MmProtectMdlSystemAddress", {MDL, 2});
  EXPECT_FALSE(llvm::cantFail(CPU->canAccess(Alias, 128, Write)));
  EXPECT_TRUE(llvm::cantFail(CPU->canAccess(Pool, 128, Write)));
  call("MmUnmapLockedPages", {Alias, MDL});
  EXPECT_EQ(integer(MDL + 0x18), Pool);
  EXPECT_EQ(integer(MDL + 0xa, 2), 4u);
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {MDL, 16}), Pool);
  EXPECT_FALSE(llvm::cantFail(CPU->canAccess(Alias, 1, Read)));
  const auto Again = map();
  ASSERT_NE(Again, 0u);
  EXPECT_EQ(integer(Again + 17), 0x1234u);
  call("MmUnmapLockedPages", {Again, MDL});
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, 1});
}

TEST_F(KernelNativeMDL, ReadOnlyDescriptorFailsBeforePublishingAnAliasOrPin) {
  const uint64_t Page = MDL & ~4095ULL;
  llvm::cantFail(CPU->protect(Page, 4096, Read));
  const auto Before = CPU->addressSpace()->mappingGeneration();
  EXPECT_NE(denied("MmMapLockedPagesSpecifyCache", {MDL, 0, 1, 0, 0, 16})
                .find("writable"),
            std::string::npos);
  EXPECT_EQ(CPU->addressSpace()->mappingGeneration(), Before);
  EXPECT_EQ(integer(MDL + 0x18), Pool);
  llvm::cantFail(CPU->protect(Page, 4096, Read | Write));
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, 1});
}

TEST_F(KernelNativeMDL, PartialAliasesKeepTheRootMappingAndPinAlive) {
  const auto Alias = map();
  const auto Child = call("IoAllocateMdl", {Pool + 16, 32, 0, 0, 0});
  call("IoBuildPartialMdl", {MDL, Child, Pool + 16, 32});
  EXPECT_EQ(call("MmGetSystemAddressForMdlSafe", {Child, 16}), Alias + 16);
  EXPECT_NE(denied("MmUnmapLockedPages", {Alias, MDL}).find("partial"),
            std::string::npos);
  EXPECT_NE(denied("IoFreeMdl", {MDL}).find("unmapping"), std::string::npos);
  call("IoFreeMdl", {Child});
  call("MmUnmapLockedPages", {Alias, MDL});
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, 1});
}
TEST_F(KernelNativeMDL, IRPCompletionRetiresAnAttachedAliasAndItsPoolPin) {
  constexpr uint64_t Scratch = 0x60000000;
  llvm::cantFail(CPU->map(Scratch, 4096, Read | Write));
  call("IoCreateDevice", {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch});
  const auto Slot = Model->driverObject() + windows::DriverDispatchOffset;
  llvm::cantFail(Model->validateGuestAccess(Slot, 8, true));
  llvm::cantFail(CPU->writeInteger(Slot, 0x180001000, 8));
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  const auto Invocation = llvm::cantFail(Model->beginRequest(Create));
  const auto IRP = Invocation.Argument1;
  const auto Alias = map();
  ASSERT_NE(Alias, 0u);
  llvm::cantFail(
      Model->validateGuestAccess(IRP + windows::IRPMdlOffset, 8, true));
  llvm::cantFail(CPU->writeInteger(IRP + windows::IRPMdlOffset, MDL, 8));
  llvm::cantFail(
      Model->validateGuestAccess(IRP + windows::IRPStatusOffset, 16, true));
  llvm::cantFail(CPU->writeInteger(IRP + windows::IRPStatusOffset, 0, 4));
  llvm::cantFail(CPU->writeInteger(IRP + windows::IRPInformationOffset, 0, 8));
  call("IofCompleteRequest", {IRP, 0});
  llvm::cantFail(Model->recordDispatchReturn(IRP, 0));
  llvm::cantFail(Model->finalizeRequest(IRP));
  EXPECT_FALSE(llvm::cantFail(CPU->canAccess(Alias, 1, Read)));
  call("ExFreePoolWithTag", {Pool, 1});
}
TEST_F(KernelNativeMDL,
       PagedAndNonpagedReservationsKeepBoundsIRQLAndLifetimeChecks) {
  const auto Paged = call("ExAllocatePoolWithTag", {1, 64, 2});
  ASSERT_NE(Paged, 0u);
  EXPECT_GE(Paged, systemva::PagedPoolBase);
  EXPECT_LT(Paged, systemva::PagedPoolBase + systemva::PagedPoolSize);
  EXPECT_NE(Paged & ~4095ULL, Pool & ~4095ULL);
  auto Padding = Model->validateGuestAccess(Paged + 64, 1, false);
  ASSERT_TRUE(bool(Padding));
  llvm::consumeError(std::move(Padding));
  call("KfRaiseIrql", {2});
  auto Elevated = Model->validateGuestAccess(Paged, 1, false);
  ASSERT_TRUE(bool(Elevated));
  llvm::consumeError(std::move(Elevated));
  call("KeLowerIrql", {0});
  call("ExFreePoolWithTag", {Paged, 2});
  auto Freed = Model->validateGuestAccess(Paged, 1, false);
  ASSERT_TRUE(bool(Freed));
  llvm::consumeError(std::move(Freed));
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, 1});
}
} // namespace
} // namespace neverd::emulation
