//===- KernelProcessMemoryTests.cpp - Process RAM and foreground attach ---===//
// NeverD Decompiler
///
/// \file
/// Process RAM and foreground attach.
///

#include "ProcessImageTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/driver/DriverProcessImages.h"
#include "os/windows/kernel/KernelException.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <cstdlib>
#include <filesystem>
namespace neverd::emulation {
namespace {
DriverOptions memoryOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.ProcessInventory = DriverProcessInventory{
      {4, 0, "System"}, {100, 4, "A.exe"}, {200, 4, "B.exe"}};
  O.ProcessInventory->at(1).Memory = std::vector<DriverProcessMemoryRegion>{
      {0x400000,
       4096,
       {0x11, 0x11, 0x11, 0x11},
       DriverUserPageAccess::ReadWrite},
      {0x500000, 4096, {0x4d, 0x5a}, DriverUserPageAccess::ReadOnly}};
  O.ProcessInventory->at(2).Memory =
      std::vector<DriverProcessMemoryRegion>{{0x400000,
                                              4096,
                                              {0x22, 0x22, 0x22, 0x22},
                                              DriverUserPageAccess::ReadWrite}};
  return O;
}
class KernelProcessMemory : public ::testing::Test {
protected:
  static constexpr uint64_t S = 0x22000000, PC = 0x180001000;
  static constexpr uint64_t Apc = profile::StackBase + 0xff548;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  void initialize(DriverOptions O = memoryOptions()) {
    Model.reset();
    Memory = llvm::cantFail(UnicornBackend::create(16 * 1024 * 1024));
    ok(Memory->map(S, 0x10000, Read | Write));
    ok(Memory->map(PC, 4096, Read | Execute));
    ok(Memory->map(profile::StackBase, profile::StackSize, Read | Write));
    Result = {};
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = PC - 4096;
    I.Entry = PC;
    I.Size = 0x4000;
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void SetUp() override { initialize(); }
  uint64_t call(llvm::StringRef Name,
                std::initializer_list<uint64_t> Args = {}) {
    auto V = Model->call(Name.str(), Args);
    if (!V) {
      ADD_FAILURE() << Name.str() << ": " << llvm::toString(V.takeError());
      return UINT64_MAX;
    }
    return *V;
  }
  void error(llvm::StringRef Name, std::initializer_list<uint64_t> Args,
             llvm::StringRef Text) {
    auto V = Model->call(Name.str(), Args);
    ASSERT_FALSE(bool(V));
    auto Message = llvm::toString(V.takeError());
    EXPECT_NE(Message.find(Text.str()), std::string::npos)
        << "expected " << Text.str() << ": " << Message;
  }
  uint64_t get(uint64_t At, unsigned Size = 8) {
    return llvm::cantFail(Memory->readInteger(At, Size));
  }
  uint64_t process(uint32_t PID) {
    EXPECT_EQ(call("PsLookupProcessByProcessId", {PID, S}), 0u);
    return get(S);
  }
  bool readable(uint64_t At) {
    return llvm::cantFail(Memory->canAccess(At, 1, Read));
  }
};
DriverOptions imageOptions() {
  auto O = memoryOptions();
  for (unsigned I : {1u, 2u}) {
    O.ProcessInventory->at(I).Memory.reset();
    O.ProcessInventory->at(I).Images = {processImageFixture()};
  }
  return O;
}

class KernelImageSection : public KernelProcessMemory {
protected:
  void SetUp() override {
    auto O = memoryOptions();
    O.ProcessInventory->at(1).Memory = std::vector<DriverProcessMemoryRegion>{};
    O.ProcessInventory->at(2).Memory = std::vector<DriverProcessMemoryRegion>{};
    O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
        {"C",
         "\\Device\\HarddiskVolume1",
         {{"\\app", true, {}},
          {"\\app\\image.exe", false, processImageFixture().FileBytes}},
         false}};
    initialize(O);
  }
  void put(uint64_t At, uint64_t Value, unsigned Width = 8) {
    ok(Memory->writeInteger(At, Value, Width));
  }
  uint64_t open(uint32_t Access = 0x120089, uint32_t Disposition = 1) {
    const std::string Path = "\\??\\C:\\app\\image.exe";
    for (size_t I = 0; I <= Path.size(); ++I)
      put(S + 0x200 + 2 * I, I == Path.size() ? 0 : uint8_t(Path[I]), 2);
    put(S + 0x100, Path.size() * 2, 2);
    put(S + 0x102, (Path.size() + 1) * 2, 2);
    put(S + 0x108, S + 0x200);
    put(S + 0x300, 48, 4);
    put(S + 0x308, 0);
    put(S + 0x310, S + 0x100);
    put(S + 0x318, 0x240, 4);
    return call("ZwCreateFile", {S, Access, S + 0x300, S + 0x400, 0, 0, 7,
                                 Disposition, 0x60, 0, 0});
  }
  uint64_t section(uint64_t File) {
    put(S + 0x600, 48, 4);
    put(S + 0x618, 0x200, 4);
    EXPECT_EQ(call("ZwCreateSection",
                   {S + 8, 0xf001f, S + 0x600, 0, 2, 0x1000000, File}),
              0u);
    return get(S + 8);
  }
  uint64_t map(uint64_t Section, uint64_t Size = 0, uint32_t Protect = 2,
               uint64_t Base = 0, uint64_t Offset = 0) {
    put(S + 0x700, Base);
    put(S + 0x708, Size);
    put(S + 0x710, Offset);
    return call("ZwMapViewOfSection", {Section, UINT64_MAX, S + 0x700, 0, 0,
                                       S + 0x710, S + 0x708, 2, 0, Protect});
  }
  uint64_t info(uint64_t File, uint64_t Value, uint32_t Class) {
    put(S + 0x800, Value);
    return call("ZwSetInformationFile",
                {File, S + 0x400, S + 0x800, Class == 13 ? 1u : 8u, Class});
  }
  void exit(uint32_t PID) {
    auto Event = Model->beginProcessEvent(
        {DriverProcessEvent::Kind::Exit, PID, 4, 4, 0}, 0);
    ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
    ASSERT_FALSE(bool(*Event));
    ok(Model->completeProcessEvent());
  }
};

TEST_F(KernelImageSection, FileAndSectionHandlesCanCloseBeforeViewAndMDL) {
  ASSERT_EQ(open(), 0u);
  const auto File = get(S), Section = section(File), A = process(100);
  EXPECT_EQ(call("ZwClose", {File}), 0u);
  call("KeStackAttachProcess", {A, Apc});
  ASSERT_EQ(map(Section), 0u);
  EXPECT_EQ(get(S + 0x700), 0x400000u);
  EXPECT_EQ(get(S + 0x708), 0x3000u);
  const auto MDL = call("IoAllocateMdl", {0x402000, 4096, 0, 0, 0});
  call("MmProbeAndLockPages", {MDL, 1, 0});
  const auto Alias =
      call("MmMapLockedPagesSpecifyCache", {MDL, 0, 1, 0, 0, 16});
  EXPECT_EQ(call("ZwClose", {Section}), 0u);
  ok(Model->validateGuestAccess(0x402000, 4, true));
  put(0x402000, 0x55667788, 4);
  EXPECT_EQ(get(Alias, 4), 0x31415926u);
  EXPECT_EQ(call("ZwQueryVirtualMemory", {UINT64_MAX, 0x402000, 0, S, 48, 0}),
            0u);
  EXPECT_EQ(get(S + 40, 4), 0x1000000u);
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_EQ(open(0x130003), 0xc0000043u);
  call("KeStackAttachProcess", {A, Apc});
  EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, 0x402123}), 0u);
  EXPECT_FALSE(readable(0x400000));
  EXPECT_EQ(get(Alias, 4), 0x31415926u);
  EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, 0x400000}), 0xc0000019u);
  call("MmUnmapLockedPages", {Alias, MDL});
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_EQ(open(0x130003), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  call("ObfDereferenceObject", {A});
}

TEST_F(KernelImageSection, NativeBasicQueryPreservesPaddingAndShortOutputs) {
  ASSERT_EQ(open(), 0u);
  const auto File = get(S), Section = section(File);
  for (uint32_t Length : {0u, 8u, 23u, 24u, 25u, 48u}) {
    std::vector<uint8_t> Bytes(64, 0xa5);
    ok(Memory->write(S + 0x900, Bytes));
    put(S + 0xa00, 0xa5a5a5a5, 4);
    EXPECT_EQ(
        call("ZwQuerySection", {Section, 0, S + 0x900, Length, S + 0xa00}),
        Length < 24 ? 0xc0000004u : 0u);
    EXPECT_EQ(get(S + 0x90c, 4), 0xa5a5a5a5u);
    EXPECT_EQ(get(S + 0x918), UINT64_C(0xa5a5a5a5a5a5a5a5));
    EXPECT_EQ(get(S + 0xa00, 4), Length < 24 ? 0xa5a5a5a5u : 24u);
    if (Length >= 24) {
      EXPECT_EQ(get(S + 0x900), 0u);
      EXPECT_EQ(get(S + 0x908, 4), 0x1800000u);
      EXPECT_EQ(get(S + 0x910), 0x3000u);
    } else
      EXPECT_EQ(get(S + 0x900), UINT64_C(0xa5a5a5a5a5a5a5a5));
  }
  EXPECT_EQ(call("ZwClose", {Section}), 0u);
  EXPECT_EQ(call("ZwClose", {File}), 0u);
}

TEST_F(KernelImageSection, PartialViewsUsePEPermissionsAndExactExtent) {
  ASSERT_EQ(open(), 0u);
  const auto File = get(S), Section = section(File), A = process(100);
  call("KeStackAttachProcess", {A, Apc});
  for (auto [Size, Protect] :
       {std::pair{0u, 1u}, std::pair{4096u, 4u}, std::pair{8192u, 0x80u}}) {
    ASSERT_EQ(map(Section, Size, Protect), 0u);
    const uint64_t Extent = Size ? Size : 0x3000;
    EXPECT_EQ(get(S + 0x708), Extent);
    EXPECT_EQ(call("ZwQueryVirtualMemory", {UINT64_MAX, 0x400000, 0, S, 48, 0}),
              0u);
    EXPECT_EQ(get(S + 36, 4), 2u);
    EXPECT_EQ(get(0x400000, 2), 0x5a4du);
    EXPECT_EQ(readable(0x401000), Extent > 4096);
    EXPECT_EQ(readable(0x402000), Extent > 8192);
    EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, 0x400000}), 0u);
  }
  EXPECT_EQ(map(Section, 0, 0x20, 0, 4096), 0xc0000220u);
  EXPECT_EQ(get(S + 0x700), 0u);
  EXPECT_EQ(get(S + 0x710), 4096u);
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_EQ(call("ZwClose", {Section}), 0u);
  EXPECT_EQ(call("ZwClose", {File}), 0u);
  call("ObfDereferenceObject", {A});
}

TEST_F(KernelImageSection, SourceMutationUsesSectionLifetimeAndNativeStatuses) {
  ASSERT_EQ(open(0x130003), 0u);
  const auto File = get(S), Section = section(File);
  EXPECT_EQ(open(0x130003), 0xc0000043u);
  EXPECT_EQ(open(0x130003, 4), 0xc0000043u);
  put(S + 0x400, UINT64_C(0xa5a5a5a5a5a5a5a5));
  put(S + 0x408, UINT64_C(0xa5a5a5a5a5a5a5a5));
  EXPECT_EQ(info(File, 0, 20), 0xc0000243u);
  EXPECT_EQ(get(S + 0x400), UINT64_C(0xa5a5a5a5a5a5a5a5));
  EXPECT_EQ(get(S + 0x408), UINT64_C(0xa5a5a5a5a5a5a5a5));
  EXPECT_EQ(info(File, 1, 13), 0xc0000121u);
  EXPECT_EQ(get(S + 0x400), UINT64_C(0xa5a5a5a5a5a5a5a5));
  EXPECT_EQ(info(File, 1536, 20), 0u);
  EXPECT_EQ(info(File, 8192, 20), 0u);
  EXPECT_EQ(call("ZwClose", {Section}), 0u);
  EXPECT_EQ(info(File, 0, 20), 0u);
  EXPECT_EQ(info(File, 1, 13), 0u);
  EXPECT_EQ(call("ZwClose", {File}), 0u);
  EXPECT_EQ(open(), 0xc0000034u);
}

TEST_F(KernelImageSection,
       ExistingSourceWriteDoesNotRewriteResidentImagePages) {
  ASSERT_EQ(open(0x130003), 0u);
  const auto File = get(S), Section = section(File), A = process(100);
  call("KeStackAttachProcess", {A, Apc});
  ASSERT_EQ(map(Section), 0u);
  EXPECT_EQ(get(0x402000, 4), 0x31415926u);
  call("KeUnstackDetachProcess", {Apc});
  put(S + 0x800, 0x400);
  put(S + 0x900, 0x55667788, 4);
  EXPECT_EQ(call("ZwWriteFile",
                 {File, 0, 0, 0, S + 0x400, S + 0x900, 4, S + 0x800, 0}),
            0u);
  call("KeStackAttachProcess", {A, Apc});
  EXPECT_EQ(get(0x402000, 4), 0x31415926u);
  EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, 0x400000}), 0u);
  ASSERT_EQ(map(Section), 0u);
  EXPECT_EQ(get(0x402000, 4), 0x31415926u);
  EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, 0x400000}), 0u);
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_EQ(call("ZwClose", {Section}), 0u);
  const auto Fresh = section(File);
  call("KeStackAttachProcess", {A, Apc});
  ASSERT_EQ(map(Fresh), 0u);
  EXPECT_EQ(get(0x402000, 4), 0x55667788u);
  EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, 0x400000}), 0u);
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_EQ(call("ZwClose", {Fresh}), 0u);
  EXPECT_EQ(call("ZwClose", {File}), 0u);
  call("ObfDereferenceObject", {A});
}

TEST_F(KernelImageSection, ProcessRundownDropsOnlyItsOwnView) {
  ASSERT_EQ(open(), 0u);
  const auto File = get(S), Section = section(File), A = process(100),
             B = process(200);
  for (auto P : {A, B}) {
    call("KeStackAttachProcess", {P, Apc});
    ASSERT_EQ(map(Section), 0u);
    call("KeUnstackDetachProcess", {Apc});
  }
  EXPECT_EQ(call("ZwClose", {Section}), 0u);
  EXPECT_EQ(call("ZwClose", {File}), 0u);
  exit(100);
  EXPECT_EQ(open(0x130003), 0xc0000043u);
  call("KeStackAttachProcess", {B, Apc});
  EXPECT_EQ(get(0x402000, 4), 0x31415926u);
  EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, 0x400000}), 0u);
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_EQ(open(0x130003), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  call("ObfDereferenceObject", {A});
  call("ObfDereferenceObject", {B});
}

TEST_F(KernelImageSection, InitialSnapshotsBindToSameFileLifetime) {
  auto O = imageOptions();
  O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
      {"C",
       "\\Device\\HarddiskVolume1",
       {{"\\app", true, {}},
        {"\\app\\image.exe", false, processImageFixture().FileBytes}},
       false}};
  auto Bad = O;
  Bad.FiniteFileSystem->at(0).Entries[1].Data[0] = 0;
  auto Rejected = validateDriverProcessInventory(Bad);
  ASSERT_TRUE(bool(Rejected));
  EXPECT_NE(llvm::toString(std::move(Rejected)).find("disagrees"),
            std::string::npos);
  initialize(O);
  EXPECT_EQ(open(0x130003), 0xc0000043u);
  exit(100);
  EXPECT_EQ(open(0x130003), 0xc0000043u);
  exit(200);
  EXPECT_EQ(open(0x130003), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
}

TEST_F(KernelImageSection, EachProcessGetsItsOwnCOWAndRemapUsesSource) {
  ASSERT_EQ(open(), 0u);
  const auto File = get(S), Section = section(File), A = process(100),
             B = process(200);
  for (auto [P, Value] :
       {std::pair{A, 0x55667788u}, std::pair{B, 0x11223344u}}) {
    call("KeStackAttachProcess", {P, Apc});
    ASSERT_EQ(map(Section), 0u);
    EXPECT_EQ(get(0x402000, 4), 0x31415926u);
    ok(Model->validateGuestAccess(0x402000, 4, true));
    put(0x402000, Value, 4);
    call("KeUnstackDetachProcess", {Apc});
  }
  for (auto [P, Value] :
       {std::pair{A, 0x55667788u}, std::pair{B, 0x11223344u}}) {
    call("KeStackAttachProcess", {P, Apc});
    EXPECT_EQ(get(0x402000, 4), Value);
    EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, 0x400000}), 0u);
    ASSERT_EQ(map(Section), 0u);
    EXPECT_EQ(get(0x402000, 4), 0x31415926u);
    EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, 0x400000}), 0u);
    call("KeUnstackDetachProcess", {Apc});
    call("ObfDereferenceObject", {P});
  }
  EXPECT_EQ(call("ZwClose", {Section}), 0u);
  EXPECT_EQ(call("ZwClose", {File}), 0u);
}

TEST_F(KernelImageSection, UnsupportedPlacementFailsWithoutPublishingView) {
  ASSERT_EQ(open(), 0u);
  const auto File = get(S), Section = section(File), A = process(100);
  call("KeStackAttachProcess", {A, Apc});
  put(S + 0x700, 0x600000);
  put(S + 0x708, 0);
  error("ZwMapViewOfSection",
        {Section, UINT64_MAX, S + 0x700, 0, 0, 0, S + 0x708, 2, 0, 2},
        "relocation support");
  EXPECT_FALSE(readable(0x400000));
  EXPECT_EQ(get(S + 0x700), 0x600000u);
  ASSERT_EQ(map(Section), 0u);
  EXPECT_EQ(map(Section, 0, 2, 0x400000), 0xc0000018u);
  EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, 0x400000}), 0u);
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_EQ(call("ZwClose", {Section}), 0u);
  EXPECT_EQ(call("ZwClose", {File}), 0u);
  call("ObfDereferenceObject", {A});
}

TEST(DriverImageSections, CompiledCPUStoresAndViewLeakGuard) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Leak : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "contract=" << unsigned(Contract)
                     << " preemptive=" << Preemptive << " leak=" << Leak);
        auto O = memoryOptions();
        O.ProcessInventory->at(1).Memory =
            std::vector<DriverProcessMemoryRegion>{};
        O.ProcessInventory->at(2).Memory =
            std::vector<DriverProcessMemoryRegion>{};
        O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
            {"C",
             "\\Device\\HarddiskVolume1",
             {{"\\app", true, {}},
              {"\\app\\image.exe", false, processImageFixture().FileBytes}},
             false}};
        O.Contract = Contract;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   (Leak ? "driver_image_sections_leak.sys"
                                         : "driver_image_sections.sys"),
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        EXPECT_EQ(R->UnloadCompleted, !Leak) << R->Diagnostic;
        EXPECT_EQ(R->Stop, Leak ? DriverStopReason::ModelError
                                : DriverStopReason::Returned)
            << R->Diagnostic;
        if (Leak)
          EXPECT_NE(R->Diagnostic.find("live dynamic image view"),
                    std::string::npos)
              << R->Diagnostic;
      }
}

TEST_F(KernelProcessMemory, ImageAllocationAndFileNameComeFromValidatedSource) {
  initialize(imageOptions());
  const auto A = process(100);
  call("KeStackAttachProcess", {A, Apc});
  for (auto [Offset, Protect] :
       {std::pair{0u, 2u}, std::pair{4096u, 0x20u}, std::pair{8192u, 8u}}) {
    EXPECT_EQ(call("ZwQueryVirtualMemory",
                   {UINT64_MAX, 0x400000 + Offset, 0, S, 48, S + 0x80}),
              0u);
    EXPECT_EQ(get(S + 8), 0x400000u);
    EXPECT_EQ(get(S + 16, 4), 0x80u);
    EXPECT_EQ(get(S + 24), 4096u);
    EXPECT_EQ(get(S + 36, 4), Protect);
    EXPECT_EQ(get(S + 40, 4), 0x1000000u);
  }
  EXPECT_EQ(get(0x400000, 2), 0x5a4du);
  EXPECT_EQ(get(0x401000, 1), 0xc3u);
  EXPECT_EQ(get(0x402000, 4), 0x31415926u);
  EXPECT_EQ(
      call("ZwQueryVirtualMemory", {UINT64_MAX, 0x402123, 2, 0, 0, S + 0x80}),
      0xc0000004u);
  const auto Length = get(S + 0x80);
  EXPECT_EQ(Length, 16 + (processImageFixture().MappedFile.size() + 1) * 2);
  EXPECT_EQ(call("ZwQueryVirtualMemory",
                 {UINT64_MAX, 0x401000, 2, S, Length, S + 0x80}),
            0u);
  EXPECT_EQ(get(S, 2), Length - 18);
  EXPECT_EQ(get(S + 2, 2), Length - 16);
  EXPECT_EQ(get(S + 8), S + 16);
  EXPECT_EQ(get(S + 16, 2), '\\');
  EXPECT_EQ(get(S + Length - 2, 2), 0u);
  call("KeUnstackDetachProcess", {Apc});
  call("ObfDereferenceObject", {A});
}

TEST_F(KernelProcessMemory, ImageCOWPreservesOtherProcessAndReadLockedPage) {
  initialize(imageOptions());
  const auto A = process(100), B = process(200);
  call("KeStackAttachProcess", {A, Apc});
  const auto MDL = call("IoAllocateMdl", {0x402000, 4096, 0, 0, 0});
  call("MmProbeAndLockPages", {MDL, 1, 0});
  const auto Alias =
      call("MmMapLockedPagesSpecifyCache", {MDL, 0, 1, 0, 0, 16});
  ok(Model->validateGuestAccess(0x402000, 4, true));
  ok(Memory->writeInteger(0x402000, 0x55667788, 4));
  EXPECT_EQ(get(0x402000, 4), 0x55667788u);
  EXPECT_EQ(get(Alias, 4), 0x31415926u);
  EXPECT_EQ(call("ZwQueryVirtualMemory", {UINT64_MAX, 0x402000, 0, S, 48, 0}),
            0u);
  EXPECT_EQ(get(S + 36, 4), 4u);
  EXPECT_EQ(get(S + 16, 4), 0x80u);
  EXPECT_EQ(get(S + 40, 4), 0x1000000u);
  call("KeStackAttachProcess", {B, Apc + 0x80});
  EXPECT_EQ(get(0x402000, 4), 0x31415926u);
  EXPECT_EQ(get(Alias, 4), 0x31415926u);
  EXPECT_EQ(call("ZwQueryVirtualMemory", {UINT64_MAX, 0x402000, 0, S, 48, 0}),
            0u);
  EXPECT_EQ(get(S + 36, 4), 8u);
  call("KeUnstackDetachProcess", {Apc + 0x80});
  EXPECT_EQ(get(0x402000, 4), 0x55667788u);
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_EQ(get(Alias, 4), 0x31415926u);
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
  call("ObfDereferenceObject", {A});
  call("ObfDereferenceObject", {B});
}

TEST_F(KernelProcessMemory, ImageWriteProbeAndWriteMDLUseThePrivateCopy) {
  initialize(imageOptions());
  const auto A = process(100), B = process(200);
  call("KeStackAttachProcess", {A, Apc});
  const auto MDL = call("IoAllocateMdl", {0x402000, 4096, 0, 0, 0});
  call("MmProbeAndLockPages", {MDL, 1, 1});
  const auto Alias =
      call("MmMapLockedPagesSpecifyCache", {MDL, 0, 1, 0, 0, 16});
  ok(Model->validateGuestAccess(Alias, 4, true));
  ok(Memory->writeInteger(Alias, 0xaabbccdd, 4));
  EXPECT_EQ(get(0x402000, 4), 0xaabbccddu);
  EXPECT_EQ(call("ZwQueryVirtualMemory", {UINT64_MAX, 0x402000, 0, S, 48, 0}),
            0u);
  EXPECT_EQ(get(S + 36, 4), 4u);
  call("KeStackAttachProcess", {B, Apc + 0x80});
  EXPECT_EQ(get(0x402000, 4), 0x31415926u);
  call("ProbeForWrite", {0x402000, 4, 4});
  EXPECT_EQ(call("ZwQueryVirtualMemory", {UINT64_MAX, 0x402000, 0, S, 48, 0}),
            0u);
  EXPECT_EQ(get(S + 36, 4), 4u);
  EXPECT_EQ(get(0x402000, 4), 0x31415926u);
  call("KeUnstackDetachProcess", {Apc + 0x80});
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
  call("KeUnstackDetachProcess", {Apc});
  call("ObfDereferenceObject", {A});
  call("ObfDereferenceObject", {B});
}
TEST(DriverProcessImages, OriginalCPUStoresPerformCopyOnWrite) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = imageOptions();
      O.Contract = Contract;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_process_images.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    }
}
TEST(DriverProcessImages,
     SourceValidationRejectsMalformedAndOverlappingImages) {
  auto Source = processImageFixture();
  auto Plan = planDriverProcessImage(Source);
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  EXPECT_EQ(Plan->Bytes.size(), 0x3000u);
  EXPECT_EQ(Plan->PageProtection, (std::vector<uint8_t>{2, 0x20, 8}));
  for (unsigned Case = 0; Case < 8; ++Case) {
    auto Bad = Source;
    switch (Case) {
    case 0:
      Bad.FileBytes[0] = 0;
      break;
    case 1:
      Bad.Base = 0x500000;
      break;
    case 2:
      Bad.DeviceName = "\\Device\\OtherVolume";
      break;
    case 3:
      Bad.FileBytes.resize(0x220);
      break;
    case 4:
      llvm::support::endian::write32le(Bad.FileBytes.data() + 0x98 + 32, 512);
      break;
    case 5:
      llvm::support::endian::write32le(
          Bad.FileBytes.data() + 0x98 + 240 + 40 + 12, 0x1000);
      break;
    case 6:
      llvm::support::endian::write32le(
          Bad.FileBytes.data() + 0x98 + 240 + 40 + 36, 0xd0000040);
      break;
    case 7:
      llvm::support::endian::write32le(Bad.FileBytes.data() + 0x98 + 56,
                                       0x4000);
      break;
    }
    auto Invalid = planDriverProcessImage(Bad);
    ASSERT_FALSE(bool(Invalid)) << Case;
    llvm::consumeError(Invalid.takeError());
  }
  auto O = imageOptions();
  O.ProcessInventory->at(1).Memory =
      std::vector<DriverProcessMemoryRegion>{{0x402000, 4096, {}}};
  auto E = validateDriverProcessInventory(O);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("overlap"), std::string::npos);
}
TEST(DriverProcessImages,
     ScenarioPreservesFileProvenanceAndRejectsUnknownFields) {
  auto Source = processImageFixture();
  std::string JSON =
      R"({"windows_version":{"major":10,"minor":0,"build":19041},"process_inventory":[{"process_id":4,"image_name":"System"},{"process_id":100,"image_name":"A.exe","images":[{"mapped_file":"\\Device\\HarddiskVolume1\\app\\image.exe","device_name":"\\Device\\HarddiskVolume1","file_bytes_hex":")" +
      llvm::toHex(llvm::ArrayRef(Source.FileBytes), true) + R"("}]}]})";
  auto O = driverOptionsFromScenarioJSON(JSON);
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_EQ(O->ProcessInventory->at(1).Images.size(), 1u);
  EXPECT_EQ(O->ProcessInventory->at(1).Images[0].FileBytes, Source.FileBytes);
  EXPECT_EQ(O->ProcessInventory->at(1).Images[0].DeviceName, Source.DeviceName);
  auto At = JSON.find("file_bytes_hex");
  JSON.replace(At, 14, "image_data_hex");
  auto Bad = driverOptionsFromScenarioJSON(JSON);
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
}
TEST_F(KernelProcessMemory, NativeFilenameShortBuffersAndUserProbeOrdering) {
  auto O = imageOptions();
  O.ProcessInventory->at(1).Memory = std::vector<DriverProcessMemoryRegion>{
      {0x500000, 4096, {}, DriverUserPageAccess::ReadWrite}};
  initialize(O);
  ok(Model->setUserRequestContext(true, 100));
  const uint64_t Out = 0x500000, Ret = 0x500800;
  const auto Source = processImageFixture();
  const uint64_t Needed = 16 + (Source.MappedFile.size() + 1) * 2;
  const uint64_t DeviceNeed = 16 + (Source.DeviceName.size() + 1) * 2;
  for (uint64_t Length : std::array<uint64_t, 9>{
           0, 15, 16, DeviceNeed - 1, DeviceNeed, DeviceNeed + 1,
           DeviceNeed + 2, Needed - 1, Needed}) {
    std::vector<uint8_t> Guard(512, 0xa5);
    ok(Memory->write(Out, Guard));
    const auto Status = call("NtQueryVirtualMemory",
                             {UINT64_MAX, 0x401000, 2, Out, Length, Ret});
    EXPECT_EQ(Status, Length < 16               ? 0xc0000004u
                      : Length < DeviceNeed     ? 0x80000005u
                      : Length < DeviceNeed + 2 ? 0xc0000039u
                      : Length < Needed         ? 0x80000005u
                                                : 0u);
    EXPECT_EQ(get(Ret),
              Length >= DeviceNeed && Length < DeviceNeed + 2 ? 0u : Needed);
    if (Length < DeviceNeed)
      EXPECT_EQ(get(Out), 0xa5a5a5a5a5a5a5a5ULL);
    else
      EXPECT_EQ(get(Out + 4, 4), Length < Needed ? 0xa5a5a5a5u : 0u);
    EXPECT_EQ(get(Out + Length, 1), 0xa5u);
  }
  EXPECT_EQ(
      call("NtQueryVirtualMemory", {UINT64_MAX, 0x401000, 2, Out + 1, 0, Ret}),
      0xc0000004u);
  EXPECT_EQ(
      call("NtQueryVirtualMemory", {UINT64_MAX, 0x401000, 2, Out + 1, 1, Ret}),
      0x80000002u);
  ok(Memory->writeInteger(Ret, 0xfeedface, 8));
  EXPECT_EQ(call("NtQueryVirtualMemory", {UINT64_MAX, Out, 2, Out, 0, Ret}),
            0xc0000141u);
  EXPECT_EQ(get(Ret), 0xfeedfaceu);
}
TEST_F(KernelProcessMemory,
       ForegroundNestedAliasesPreserveBytesAndThreadIdentity) {
  const auto System = call("IoGetCurrentProcess"),
             Thread = call("PsGetCurrentThread");
  const auto TID = call("PsGetCurrentThreadId"), A = process(100),
             B = process(200);
  EXPECT_FALSE(readable(0x400000));
  call("KeStackAttachProcess", {A, Apc});
  EXPECT_EQ(call("IoGetCurrentProcess"), A);
  EXPECT_EQ(call("PsGetCurrentProcessId"), 4u);
  EXPECT_EQ(call("PsGetCurrentThread"), Thread);
  EXPECT_EQ(call("PsGetCurrentThreadId"), TID);
  EXPECT_EQ(call("ExGetPreviousMode"), 0u);
  EXPECT_EQ(get(0x400000, 4), 0x11111111u);
  ok(Model->validateGuestAccess(0x400000, 4, true));
  ok(Memory->writeInteger(0x400000, 0x44444444, 4));
  error("KeStackAttachProcess", {B, Apc + 8}, "owned");
  call("KeStackAttachProcess", {B, Apc + 0x80});
  EXPECT_EQ(get(0x400000, 4), 0x22222222u);
  error("KeUnstackDetachProcess", {Apc}, "most recent");
  call("KeUnstackDetachProcess", {Apc + 0x80});
  EXPECT_EQ(get(0x400000, 4), 0x44444444u);
  call("KeStackAttachProcess", {System, Apc + 0x80});
  EXPECT_EQ(call("IoGetCurrentProcess"), System);
  EXPECT_EQ(call("PsGetCurrentProcessId"), 4u);
  EXPECT_FALSE(readable(0x400000));
  call("KeUnstackDetachProcess", {Apc + 0x80});
  EXPECT_EQ(get(0x400000, 4), 0x44444444u);
  auto E = Model->validateExecutionReturn(profile::StackBase, 0);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_FALSE(readable(0x400000));
  EXPECT_EQ(call("IoGetCurrentProcess"), System);
  call("KeStackAttachProcess", {A, Apc});
  EXPECT_EQ(get(0x400000, 4), 0x44444444u);
  call("KeUnstackDetachProcess", {Apc});
  call("ObfDereferenceObject", {A});
  call("ObfDereferenceObject", {B});
}
TEST_F(KernelProcessMemory,
       UserOriginKeepsCreatingPIDAndPreviousModeAcrossAttach) {
  ok(Model->setUserRequestContext(true, 100));
  const auto Thread = call("PsGetCurrentThread"),
             A = call("IoGetCurrentProcess"), B = process(200);
  call("KeStackAttachProcess", {B, Apc});
  EXPECT_EQ(call("IoGetCurrentProcess"), B);
  EXPECT_EQ(call("PsGetCurrentProcessId"), 100u);
  EXPECT_EQ(call("ExGetPreviousMode"), 1u);
  EXPECT_EQ(call("PsGetCurrentThread"), Thread);
  EXPECT_EQ(call("PsGetThreadProcess", {Thread}), A);
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_EQ(call("IoGetCurrentProcess"), A);
  EXPECT_EQ(call("PsGetCurrentProcessId"), 100u);
  EXPECT_EQ(get(0x400000, 4), 0x11111111u);
  call("ObfDereferenceObject", {B});
}
TEST_F(KernelProcessMemory, ReadonlyAndNoAccessViewsRetainTheirPermissions) {
  auto O = memoryOptions();
  O.ProcessInventory->at(1).Memory->push_back(
      {0x600000, 4096, {}, DriverUserPageAccess::NoAccess});
  initialize(O);
  const auto A = process(100);
  call("KeStackAttachProcess", {A, Apc});
  EXPECT_EQ(call("MmIsAddressValid", {0x500000}), 1u);
  EXPECT_EQ(call("MmIsAddressValid", {0x600000}), 0u);
  EXPECT_EQ(get(0x500000, 2), 0x5a4du);
  auto P = Model->call("ProbeForWrite", {0x500000, 4, 4});
  ASSERT_FALSE(bool(P));
  auto E = P.takeError();
  EXPECT_TRUE(E.isA<KernelGuestException>());
  llvm::consumeError(std::move(E));
  call("KeUnstackDetachProcess", {Apc});
  call("ObfDereferenceObject", {A});
}
TEST_F(KernelProcessMemory, FailedAliasAdmissionPreservesContextAndAPCBytes) {
  const auto A = process(100), System = call("IoGetCurrentProcess");
  ok(Memory->map(0x400000, 4096, Read | Write));
  ok(Memory->writeInteger(0x400000, 0xdeadbeef, 4));
  ok(Memory->writeInteger(Apc, 0x1122334455667788, 8));
  auto V = Model->call("KeStackAttachProcess", {A, Apc});
  ASSERT_FALSE(bool(V));
  llvm::consumeError(V.takeError());
  EXPECT_EQ(call("IoGetCurrentProcess"), System);
  EXPECT_EQ(get(Apc), 0x1122334455667788u);
  EXPECT_EQ(get(0x400000, 4), 0xdeadbeefu);
  call("ObfDereferenceObject", {A});
}
TEST_F(KernelProcessMemory,
       MissingCatalogueAndIRQLAndStackOverflowFailBeforeWrite) {
  auto O = memoryOptions();
  O.ProcessInventory->at(1).Memory.reset();
  initialize(O);
  const auto A = process(100);
  error("KeStackAttachProcess", {A, Apc}, "declared process memory");
  call("ObfDereferenceObject", {A});
  initialize();
  const auto B = process(200);
  error("KeStackAttachProcess",
        {B, profile::StackBase + profile::StackSize - 8},
        "current thread stack");
  call("KfRaiseIrql", {2});
  error("KeStackAttachProcess", {B, Apc}, "IRQL");
  call("KeLowerIrql", {0});
  call("ObfDereferenceObject", {B});
}
TEST_F(KernelProcessMemory, RetiredProcessRemovesViewsAndLockedPagesSurvive) {
  const auto A = process(100);
  const auto MDL = call("IoAllocateMdl", {0x400000, 4096, 0, 0, 0});
  call("KeStackAttachProcess", {A, Apc});
  call("MmProbeAndLockPages", {MDL, 1, 0});
  const auto Alias =
      call("MmMapLockedPagesSpecifyCache", {MDL, 0, 1, 0, 0, 16});
  ASSERT_NE(Alias, 0u);
  call("KeUnstackDetachProcess", {Apc});
  auto Event = Model->beginProcessEvent(
      {DriverProcessEvent::Kind::Exit, 100, 4, 4, 0, 0}, 0);
  ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
  ok(Model->completeProcessEvent());
  EXPECT_FALSE(readable(0x400000));
  EXPECT_EQ(get(Alias, 4), 0x11111111u);
  error("KeStackAttachProcess", {A, Apc}, "exited");
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
  call("ObfDereferenceObject", {A});
}
TEST_F(KernelProcessMemory,
       VirtualQueryUsesAllocationBoundariesAndPermissions) {
  auto O = memoryOptions();
  O.ProcessInventory->at(1).Memory->at(0).Size = 8192;
  O.ProcessInventory->at(1).Memory->push_back(
      {0x600000, 4096, {}, DriverUserPageAccess::NoAccess});
  initialize(O);
  const auto A = process(100);
  call("KeStackAttachProcess", {A, Apc});
  EXPECT_EQ(call("ZwQueryVirtualMemory",
                 {UINT64_MAX, 0x400137, 0, S + 0x100, 64, S + 0x200}),
            0u);
  EXPECT_EQ(get(S + 0x100), 0x400000u);
  EXPECT_EQ(get(S + 0x108), 0x400000u);
  EXPECT_EQ(get(S + 0x110, 4), 4u);
  EXPECT_EQ(get(S + 0x118), 8192u);
  EXPECT_EQ(get(S + 0x120, 4), 0x1000u);
  EXPECT_EQ(get(S + 0x124, 4), 4u);
  EXPECT_EQ(get(S + 0x128, 4), 0x20000u);
  EXPECT_EQ(get(S + 0x200), 48u);
  EXPECT_EQ(
      call("ZwQueryVirtualMemory", {UINT64_MAX, 0x401003, 0, S + 0x100, 48, 0}),
      0u);
  EXPECT_EQ(get(S + 0x100), 0x401000u);
  EXPECT_EQ(get(S + 0x108), 0x400000u);
  EXPECT_EQ(get(S + 0x118), 4096u);
  for (auto [At, Protect] :
       {std::pair{0x500000u, 2u}, std::pair{0x600000u, 1u}}) {
    EXPECT_EQ(
        call("ZwQueryVirtualMemory", {UINT64_MAX, At, 0, S + 0x100, 48, 0}),
        0u);
    EXPECT_EQ(get(S + 0x110, 4), Protect);
    EXPECT_EQ(get(S + 0x124, 4), Protect);
    EXPECT_EQ(get(S + 0x128, 4), 0x20000u);
  }
  call("KeUnstackDetachProcess", {Apc});
  error("ZwQueryVirtualMemory", {UINT64_MAX, 0x400000, 0, S, 48, 0},
        "no modeled virtual allocation");
  call("ObfDereferenceObject", {A});
}
TEST_F(KernelProcessMemory, VirtualQueryFailurePriorityAndOutputs) {
  ok(Memory->writeInteger(S, 0x1122334455667788, 8));
  ok(Memory->writeInteger(S + 64, 0x8877665544332211, 8));
  EXPECT_EQ(call("ZwQueryVirtualMemory",
                 {0x1234, 0xfffff80000000000, 0, S, 47, S + 64}),
            0xc0000004u);
  EXPECT_EQ(call("ZwQueryVirtualMemory",
                 {0x1234, 0xfffff80000000000, 0, S, 48, S + 64}),
            0xc000000du);
  EXPECT_EQ(call("ZwQueryVirtualMemory", {0x1234, 0x400000, 0, S, 48, S + 64}),
            0xc0000008u);
  EXPECT_EQ(call("ZwQueryVirtualMemory",
                 {UINT64_MAX - 1, 0x400000, 0, S, 48, S + 64}),
            0xc0000024u);
  EXPECT_EQ(call("ZwQueryVirtualMemory",
                 {UINT64_MAX, 0x400000, 0xffffffff, S, 0, S + 64}),
            0xc0000003u);
  error("ZwQueryVirtualMemory", {UINT64_MAX, 0x400000, 1, S, 48, S + 64},
        "unmodeled memory information class");
  EXPECT_EQ(get(S), 0x1122334455667788u);
  EXPECT_EQ(get(S + 64), 0x8877665544332211u);
  call("KfRaiseIrql", {1});
  error("ZwQueryVirtualMemory", {UINT64_MAX, 0x400000, 0, S, 48, 0}, "IRQL");
  call("KeLowerIrql", {0});
}
TEST_F(KernelProcessMemory, VirtualQueryPreservesNativeOutputAliasOrdering) {
  ok(Model->setUserRequestContext(true, 100));
  for (uint64_t Offset : {0u, 8u, 24u, 40u, 48u}) {
    EXPECT_EQ(call("ZwQueryVirtualMemory",
                   {UINT64_MAX, 0x400000, 0, S, 64, S + Offset}),
              0u);
    EXPECT_EQ(get(S + Offset), 48u);
  }
  // Zw is a KernelMode syscall even when the captured caller PreviousMode is
  // User.
  EXPECT_EQ(
      call("ZwQueryVirtualMemory", {UINT64_MAX, 0x400000, 0, S + 1, 48, 0}),
      0u);
  EXPECT_EQ(get(S + 1), 0x400000u);
  EXPECT_EQ(call("ExGetPreviousMode"), 1u);
}
TEST_F(KernelProcessMemory,
       NtVirtualQueryProbesUserOutputsBeforeAddressAndHandle) {
  ok(Model->setUserRequestContext(true, 100));
  for (uint64_t Offset : {1u, 2u, 4u})
    EXPECT_EQ(call("NtQueryVirtualMemory",
                   {0x1234, UINT64_MAX, 0, 0x400000 + Offset, 48, 0}),
              0x80000002u);
  EXPECT_EQ(call("NtQueryVirtualMemory",
                 {UINT64_MAX, 0x400000, 0, profile::StackBase, 48, 0}),
            0xc0000005u);
  EXPECT_EQ(
      call("NtQueryVirtualMemory", {UINT64_MAX, 0x400000, 0, 0x500000, 48, 0}),
      0xc0000005u);
  EXPECT_EQ(
      call("NtQueryVirtualMemory", {UINT64_MAX, 0x400000, 0, 0x400000, 47, S}),
      0xc0000004u);
  EXPECT_EQ(call("NtQueryVirtualMemory",
                 {UINT64_MAX, 0x400000, 0, 0x400100, 64, 0x400201}),
            0u);
  EXPECT_EQ(get(0x400100), 0x400000u);
  EXPECT_EQ(get(0x400201), 48u);
  EXPECT_EQ(call("ExGetPreviousMode"), 1u);
}
TEST_F(KernelProcessMemory,
       VirtualQueryUnknownMetadataAndFailedOutputRemainExplicit) {
  const auto A = process(100);
  call("KeStackAttachProcess", {A, Apc});
  error("ZwQueryVirtualMemory", {UINT64_MAX, 0x450000, 0, S, 48, 0},
        "no modeled virtual allocation");
  error("ZwQueryVirtualMemory", {UINT64_MAX, 0x400000, 0, 0x500000, 48, 0},
        "permissions");
  ok(Memory->writeInteger(S, 0x1122334455667788, 8));
  error("ZwQueryVirtualMemory", {UINT64_MAX, 0x400000, 0, S, 48, 0x500000},
        "permissions");
  EXPECT_EQ(get(S), 0x1122334455667788u);
  call("KeUnstackDetachProcess", {Apc});
  call("ObfDereferenceObject", {A});
  auto O = memoryOptions();
  O.Version.reset();
  O.ProcessInventory.reset();
  initialize(O);
  error("ZwQueryVirtualMemory", {UINT64_MAX, 0x400000, 0, S, 48, 0}, "19041");
}
TEST_F(KernelProcessMemory,
       NtVirtualQueryDoesNotExposeKernelHandlesToUserMode) {
  const auto Thread = call("PsGetCurrentThread");
  EXPECT_EQ(call("ObOpenObjectByPointer", {Thread, 0x200, 0, 0x40, 0, 0, S}),
            0u);
  const auto KernelHandle = get(S);
  EXPECT_EQ(call("ObOpenObjectByPointer", {Thread, 0, 0, 0x40, 0, 0, S}), 0u);
  const auto SystemHandle = get(S);
  ok(Model->setUserRequestContext(true, 100));
  EXPECT_EQ(call("NtQueryVirtualMemory",
                 {KernelHandle, 0x400000, 0, 0x400100, 48, 0}),
            0xc0000008u);
  EXPECT_EQ(call("ZwQueryVirtualMemory", {KernelHandle, 0x400000, 0, S, 48, 0}),
            0xc0000024u);
  EXPECT_EQ(call("NtQueryVirtualMemory",
                 {SystemHandle, 0x400000, 0, 0x400100, 48, 0}),
            0xc0000008u);
  EXPECT_EQ(call("ZwQueryVirtualMemory", {SystemHandle, 0x400000, 0, S, 48, 0}),
            0xc0000008u);
  ok(Model->setUserRequestContext(false));
  call("ZwClose", {SystemHandle});
  call("ZwClose", {KernelHandle});
}
TEST(DriverProcessMemoryScenario, StrictRegionsAndRoundTrip) {
  const std::string Prefix =
      R"({"windows_version":{"major":10,"minor":0,"build":19041},"process_inventory":[{"process_id":4,"image_name":"System"},{"process_id":100,"image_name":"A","memory":)";
  for (const char *Bad :
       {"null", "true", "{}", R"([{"base":4194304,"size":1}])",
        R"([{"base":0,"size":4096}])", R"([{"base":4194305,"size":4096}])",
        R"([{"base":268435456,"size":4096}])",
        R"([{"base":4194304,"size":4096,"unknown":1}])",
        R"([{"base":4194304,"size":4096,"bytes_hex":"gg"}])",
        R"([{"base":4194304,"size":4096,"access":"execute"}])",
        R"([{"base":4194304,"size":4096},{"base":4194304,"size":4096}])"}) {
    auto O = driverOptionsFromScenarioJSON(Prefix + Bad + "}]}");
    ASSERT_FALSE(bool(O)) << Bad;
    llvm::consumeError(O.takeError());
  }
  auto O = memoryOptions();
  DriverResult R;
  R.Configuration = O;
  auto JSON = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(JSON));
  const auto *C = JSON->getAsObject()->getObject("configuration");
  ASSERT_NE(C, nullptr);
  const auto *Inventory = C->getArray("process_inventory");
  ASSERT_NE(Inventory, nullptr);
  ASSERT_GT(Inventory->size(), 1u);
  EXPECT_TRUE((*Inventory)[1].getAsObject()->getArray("memory"));
  auto RoundTrip = driverOptionsFromScenarioJSON(
      "{\"windows_version\":{\"major\":10,\"minor\":0,\"build\":19041},"
      "\"process_inventory\":" +
      llvm::formatv("{0}", *C->get("process_inventory")).str() + "}");
  ASSERT_TRUE(bool(RoundTrip)) << llvm::toString(RoundTrip.takeError());
  ASSERT_TRUE(RoundTrip->ProcessInventory->at(1).Memory);
  const auto &Regions = *RoundTrip->ProcessInventory->at(1).Memory;
  ASSERT_EQ(Regions.size(), 2u);
  EXPECT_EQ(Regions[0].Base, 0x400000u);
  EXPECT_EQ(Regions[0].Size, 4096u);
  EXPECT_EQ(Regions[0].Bytes, O.ProcessInventory->at(1).Memory->at(0).Bytes);
  EXPECT_EQ(Regions[0].Access, DriverUserPageAccess::ReadWrite);
  EXPECT_EQ(Regions[1].Access, DriverUserPageAccess::ReadOnly);
  O.ProcessInventory->at(1).Memory = std::vector<DriverProcessMemoryRegion>{};
  EXPECT_FALSE(bool(validateDriverProcessInventory(O)));
}
TEST(DriverProcessMemory, CompiledForegroundClients) {
  const auto *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
             *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (unsigned Reference : {0u, 1u, 2u}) {
        if (Reference && (!Image || !PDB))
          continue;
        SCOPED_TRACE(::testing::Message()
                     << "reference=" << Reference << " preemptive="
                     << Preemptive << " contract=" << unsigned(Contract));
        auto O = memoryOptions();
        O.Contract = Contract;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        if (Reference)
          O.KernelImage = Image;
        if (Reference == 2)
          O.KernelPDB = PDB;
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_process_memory.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      }
}
} // namespace
} // namespace neverd::emulation
