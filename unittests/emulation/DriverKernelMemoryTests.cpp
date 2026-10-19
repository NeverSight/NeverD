//===- DriverKernelMemoryTests.cpp - Coherent kernel capacity -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Coherent kernel capacity.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/WindowsUnicode.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/driver/DriverScenario.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelPhysicalMemory.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t Capacity = 8 * 1024 * 1024;
constexpr uint64_t SampleReadSize = 0x22d350;
TEST(DriverKernelMemory, CapacityIsBoundedInheritedAndReported) {
  for (const char *Bad :
       {R"({"kernel_memory_size":true})", R"({"kernel_memory_size":null})",
        R"({"kernel_memory_size":0})", R"({"kernel_memory_size":1048575})",
        R"({"kernel_memory_size":1048577})",
        R"({"kernel_memory_size":8388609})", R"({"kernel_memory_size":-1})"}) {
    auto O = driverOptionsFromScenarioJSON(Bad);
    ASSERT_FALSE(bool(O));
    EXPECT_NE(llvm::toString(O.takeError()).find("kernel_memory_size"),
              std::string::npos);
  }
  auto O = driverOptionsFromScenarioJSON(R"({"kernel_memory_size":8388608})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  EXPECT_EQ(O->KernelMemorySize, Capacity);
  auto Inherited = driverOptionsFromScenarioJSON("{}", *O);
  ASSERT_TRUE(bool(Inherited));
  EXPECT_EQ(Inherited->KernelMemorySize, Capacity);
  auto Default = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(bool(Default));
  EXPECT_FALSE(Default->KernelMemorySize);
  O->MemoryLimit = 4 * 1024 * 1024;
  auto Rejected = driverOptionsFromScenarioJSON("{}", *O);
  ASSERT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
  DriverResult Result;
  Result.Configuration = *Inherited;
  auto Report = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(Report));
  EXPECT_EQ(Report->getAsObject()
                ->getObject("configuration")
                ->getInteger("kernel_memory_size"),
            Capacity);
}

TEST(DriverKernelMemory, ManagedWindowAndOrdinaryFramesNeverOverlap) {
  auto CPU = llvm::cantFail(UnicornBackend::create(4 * 1024 * 1024));
  llvm::cantFail(CPU->map(0x60000000, 8192, Read | Write));
  KernelPhysicalMemory Physical(*CPU);
  llvm::cantFail(Physical.configureManagedCapacity(Capacity));
  llvm::cantFail(Physical.enableGuestFrames(4 * 1024 * 1024));
  auto Frame = llvm::cantFail(Physical.frameAddress(0x60000000));
  EXPECT_EQ(Frame, physical::PhysicalBase + Capacity);
  llvm::cantFail(Physical.registerRegion(1, 0x60001000, 4096));
  auto Managed = llvm::cantFail(Physical.physicalAddress(0x60001000));
  EXPECT_EQ(Managed, physical::PhysicalBase);
  EXPECT_NE(Frame, Managed);
  EXPECT_EQ(Physical.physicalRangeSize(), Capacity + 4 * 1024 * 1024);
  auto Reset = Physical.configureManagedCapacity(Capacity);
  ASSERT_TRUE(bool(Reset));
  llvm::consumeError(std::move(Reset));
  auto Plan = llvm::cantFail(
      Physical.planAllocatedPages(physical::PhysicalBase + Capacity - 4096,
                                  physical::PhysicalBase + Capacity - 1, 0, 2));
  ASSERT_EQ(Plan.size(), 1u);
  EXPECT_EQ(Plan[0], physical::PhysicalBase + Capacity - 4096);
}

TEST(DriverKernelMemory, LargePoolClientPreservesDefaultShortageAndCanOptIn) {
  const auto Image =
      std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "driver_large_pool.sys";
  auto Default = emulateDriver(Image);
  ASSERT_TRUE(bool(Default)) << llvm::toString(Default.takeError());
  EXPECT_EQ(Default->NTStatus, 0xc000009au);
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.KernelMemorySize = Capacity;
      O.Unload = true;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.PageTables = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(Image, O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    }
}

TEST(DriverKernelMemory, ExpandedPhysicalWindowRejectsOverlappingResources) {
  DriverPnpDevice Device;
  DriverMemoryResource Resource;
  Resource.TranslatedStart = DriverDmaPhysicalBase + 2 * 1024 * 1024;
  Resource.Length = 4096;
  Device.Resources.push_back(Resource);
  EXPECT_EQ(
      llvm::toString(validateDriverDma({Device}, profile::KernelArenaSize)),
      "");
  auto E = validateDriverDma({Device}, Capacity);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("physical RAM"),
            std::string::npos);
}
TEST(DriverKernelMemory, FullSampleSizedFileReadAndMdlAliasShareTheSameBytes) {
  auto CPU = llvm::cantFail(UnicornBackend::create(32 * 1024 * 1024));
  constexpr uint64_t Scratch = 0x60000000;
  llvm::cantFail(CPU->map(Scratch, 4096, Read | Write));
  DriverOptions O;
  O.KernelMemorySize = Capacity;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.PageTables = true;
  O.MDLMappingPolicy = DriverMDLMappingPolicy::Windows19041;
  std::vector<uint8_t> Bytes(SampleReadSize);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = uint8_t(I * 37 + 11);
  O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
      {"C", "\\Device\\NeverDVolumeC", {{"\\large.bin", false, Bytes}}}};
  DriverResult Result;
  KernelModel Model(*CPU, Result);
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 4096;
  Image.Size = 0x3000;
  llvm::cantFail(Model.initialize(Image, O));
  Model.enterForeground();
  Model.enterExecution(profile::StackBase);
  auto Call = [&](const char *Name, std::initializer_list<uint64_t> A) {
    return llvm::cantFail(Model.call(Name, A));
  };
  auto Put = [&](uint64_t Address, uint64_t Value, unsigned Width = 8) {
    llvm::cantFail(CPU->writeInteger(Address, Value, Width));
  };
  auto Text = llvm::cantFail(
      windows_unicode::bytes("\\DosDevices\\C:\\large.bin", true));
  llvm::cantFail(CPU->write(Scratch + 0x200, Text));
  Put(Scratch + 0x100, Text.size() - 2, 2);
  Put(Scratch + 0x102, Text.size(), 2);
  Put(Scratch + 0x108, Scratch + 0x200);
  Put(Scratch + 0x300, 48, 4);
  Put(Scratch + 0x310, Scratch + 0x100);
  Put(Scratch + 0x318, 0x240, 4);
  ASSERT_EQ(Call("ZwCreateFile", {Scratch, 0x100001, Scratch + 0x300,
                                  Scratch + 0x400, 0, 0, 1, 1, 0x60, 0, 0}),
            0u);
  auto Handle = llvm::cantFail(CPU->readInteger(Scratch, 8));
  auto Pool = Call("ExAllocatePoolWithTag", {0, SampleReadSize, 0x1234});
  ASSERT_NE(Pool, 0u);
  ASSERT_EQ(Call("ZwReadFile", {Handle, 0, 0, 0, Scratch + 0x400, Pool,
                                SampleReadSize, 0, 0}),
            0u);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Scratch + 0x408, 8)),
            SampleReadSize);
  std::vector<uint8_t> Actual(Bytes.size());
  llvm::cantFail(CPU->read(Pool, Actual));
  EXPECT_EQ(Actual, Bytes);
  auto MDL = Call("IoAllocateMdl", {Pool, SampleReadSize, 0, 0, 0});
  ASSERT_NE(MDL, 0u);
  Call("MmBuildMdlForNonPagedPool", {MDL});
  auto Alias = Call("MmMapLockedPagesSpecifyCache", {MDL, 0, 1, 0, 0, 16});
  ASSERT_NE(Alias, 0u);
  ASSERT_NE(Alias, Pool);
  for (uint64_t Offset :
       {uint64_t(0), uint64_t(0x100000), SampleReadSize - 1}) {
    EXPECT_EQ(Call("MmGetPhysicalAddress", {Pool + Offset}),
              Call("MmGetPhysicalAddress", {Alias + Offset}));
    Put(Alias + Offset, 0xa5, 1);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Pool + Offset, 1)), 0xa5u);
  }
  llvm::cantFail(Model.synchronizeGuestPageTables());
  Call("MmUnmapLockedPages", {Alias, MDL});
  Call("IoFreeMdl", {MDL});
  Call("ExFreePoolWithTag", {Pool, 0x1234});
  EXPECT_EQ(Call("ZwClose", {Handle}), 0u);
}
} // namespace
} // namespace neverd::emulation
