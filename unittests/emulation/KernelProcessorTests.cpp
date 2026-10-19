//===- KernelProcessorTests.cpp - Shared single-processor guest identity
//---===//
// NeverD Decompiler
///
/// \file
/// Shared single-processor guest identity.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelProcessorData.h"
#include "os/windows/kernel/KernelReferenceImage.h"

#include "llvm/ADT/StringExtras.h"

#include <cstdlib>
#include <filesystem>

namespace neverd::emulation {
namespace {
DriverOptions nativeDataOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  return O;
}

TEST(KernelProcessor, TypedDataReferenceIdentityIsAtomicAndNotCallable) {
  KernelExportRegistry R;
  llvm::cantFail(R.initialize(nativeDataOptions()));
  const auto Original = llvm::cantFail(R.resolve("KeNumberProcessors"));
  EXPECT_EQ(Original, processor::CountSlot);
  EXPECT_EQ(R.lookup(Original)->Kind,
            KernelExportRegistry::ExportKind::ProcessorData);
  EXPECT_FALSE(KernelModel::argumentCount(*R.lookup(Original)));
  constexpr uint64_t Native = 0xfffff80000008004;
  for (unsigned Case = 0; Case < 5; ++Case) {
    auto Inventory = std::map<std::string, uint64_t>{{"KeNumberProcessors", 0}};
    auto Data = std::map<std::string, uint64_t>{{"KeNumberProcessors", Native}};
    std::map<std::string, uint64_t> Process;
    if (Case == 0)
      Data.begin()->second = 0;
    if (Case == 1)
      Data.clear();
    if (Case == 2)
      Inventory.begin()->second = 0xfffff80000001000;
    if (Case == 3)
      Data = {{"UnknownProcessorData", Native}};
    if (Case == 4) {
      Inventory.emplace("PsInitialSystemProcess", 0);
      Process.emplace("PsInitialSystemProcess", Native - 4);
    }
    auto E = R.attachReferenceImage(Inventory, {}, {}, Process, Data);
    ASSERT_TRUE(bool(E)) << Case;
    llvm::consumeError(std::move(E));
    EXPECT_EQ(llvm::cantFail(R.resolve("KeNumberProcessors")), Original);
  }
  llvm::cantFail(R.attachReferenceImage({{"KeNumberProcessors", 0}}, {}, {}, {},
                                        {{"KeNumberProcessors", Native}}));
  EXPECT_EQ(llvm::cantFail(
                R.bindImport({0x1000, "ntkrnlmp.exe", "KeNumberProcessors"})),
            Native);
  EXPECT_EQ(R.lookup(Native)->Kind,
            KernelExportRegistry::ExportKind::ProcessorData);
  EXPECT_FALSE(KernelModel::argumentCount(*R.lookup(Native)));
}

TEST(KernelProcessor, ScalarAndNumberQueriesSharePublishedReadOnlyData) {
  KernelExportRegistry Exports;
  llvm::cantFail(Exports.initialize(nativeDataOptions()));
  auto Memory = llvm::cantFail(UnicornBackend::create(4 * 1024 * 1024));
  constexpr uint64_t S = 0x22000000;
  llvm::cantFail(Memory->map(S, 4096, Read | Write));
  DriverResult Result;
  KernelModel Model(*Memory, Result, &Exports);
  DriverImage D;
  D.Base = 0x180000000;
  D.Entry = D.Base + 4096;
  D.Size = 0x3000;
  llvm::cantFail(Model.initialize(D, {}));
  Model.enterForeground();
  Model.enterExecution(profile::StackBase);
  auto call = [&](const char *Name, std::initializer_list<uint64_t> A) {
    return llvm::cantFail(Model.call(Name, A));
  };
  EXPECT_EQ(llvm::cantFail(Memory->readInteger(processor::CountSlot, 1)), 1u);
  llvm::cantFail(Model.validateGuestAccess(processor::CountSlot, 1, false));
  for (bool Write : {false, true}) {
    auto E =
        Model.validateGuestAccess(processor::CountSlot, Write ? 1 : 8, Write);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  auto InvokeData =
      Model.call(*Exports.lookup(processor::CountSlot), {}, nullptr);
  ASSERT_FALSE(bool(InvokeData));
  EXPECT_NE(llvm::toString(InvokeData.takeError()).find("not callable"),
            std::string::npos);
  call("KfRaiseIrql", {15});
  EXPECT_EQ(call("KeQueryActiveProcessors", {}), 1u);
  constexpr uint64_t Groups[] = {0, 1, 2, 0xffff, 0x10000, 0x1ffff, UINT64_MAX};
  for (uint64_t Group : Groups)
    EXPECT_EQ(call("KeQueryActiveProcessorCountEx", {Group}),
              uint16_t(Group) == 0 || uint16_t(Group) == 0xffff ? 1u : 0u);
  EXPECT_EQ(call("KeGetCurrentProcessorNumberEx", {0}), 0u);
  for (unsigned Offset : {0u, 1u, 4u}) {
    std::vector<uint8_t> Bytes(16, 0xa5);
    llvm::cantFail(Memory->write(S, Bytes));
    EXPECT_EQ(call("KeGetCurrentProcessorNumberEx", {S + Offset}), 0u);
    EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + Offset, 4)), 0u);
    EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + Offset + 4, 1)), 0xa5u);
    llvm::cantFail(Memory->write(S, Bytes));
    EXPECT_EQ(call("KeGetProcessorNumberFromIndex", {0, S + Offset}), 0u);
    EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + Offset, 4)), 0u);
    for (uint64_t Index : {1ULL, 0x4ffULL, 0x500ULL, 0xffffffffULL}) {
      llvm::cantFail(Memory->write(S, Bytes));
      EXPECT_EQ(call("KeGetProcessorNumberFromIndex", {Index, S + Offset}),
                0xc000000du);
      std::vector<uint8_t> After(16);
      llvm::cantFail(Memory->read(S, After));
      EXPECT_EQ(After, Bytes);
    }
    EXPECT_EQ(
        call("KeGetProcessorNumberFromIndex", {0x100000000ULL, S + Offset}),
        0u);
  }
  call("KeLowerIrql", {0});
}

TEST(KernelProcessor,
     NativeAffinityMatrixPreservesThreadStateAndAliasedOutputs) {
  auto Memory = llvm::cantFail(UnicornBackend::create(4 * 1024 * 1024));
  constexpr uint64_t S = 0x22000000;
  llvm::cantFail(Memory->map(S, 4096, Read | Write));
  DriverResult Result;
  KernelModel Model(*Memory, Result);
  DriverImage D;
  D.Base = 0x180000000;
  D.Entry = D.Base + 4096;
  D.Size = 0x3000;
  llvm::cantFail(Model.initialize(D, {}));
  Model.enterForeground();
  Model.enterExecution(profile::StackBase);
  auto call = [&](const char *Name, std::initializer_list<uint64_t> A) {
    return llvm::cantFail(Model.call(Name, A));
  };
  auto input = [&](uint64_t Mask) {
    std::vector<uint8_t> Zero(16);
    llvm::cantFail(Memory->write(S, Zero));
    llvm::cantFail(Memory->writeInteger(S, Mask, 8));
  };
  struct Case {
    const char *API;
    bool Active;
    uint64_t Mask;
    const char *Mode, *Source, *Previous;
    bool After;
  };
  static constexpr Case Cases[] = {
#define NEVERD_PROCESSOR_AFFINITY_CASE(API, Active, Mask, Mode, Source,        \
                                       Previous, After)                        \
  {#API, Active, Mask, #Mode, Source, Previous, After},
#include "ProcessorAffinityCases.def"
#undef NEVERD_PROCESSOR_AFFINITY_CASE
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(::testing::Message()
                 << C.API << " " << C.Active << " " << C.Mask << " " << C.Mode);
    input(0);
    call("KeRevertToUserGroupAffinityThread", {S});
    if (C.Active) {
      input(1);
      call("KeSetSystemGroupAffinityThread", {S, 0});
    }
    input(C.Mask);
    std::vector<uint8_t> Bytes(16, 0xa5);
    llvm::cantFail(Memory->write(S + 0x100, Bytes));
    if (llvm::StringRef(C.API) == "set") {
      const uint64_t Previous = llvm::StringRef(C.Mode) == "same"   ? S
                                : llvm::StringRef(C.Mode) == "null" ? 0
                                                                    : S + 0x100;
      call("KeSetSystemGroupAffinityThread", {S, Previous});
    } else
      call("KeRevertToUserGroupAffinityThread", {S});
    llvm::cantFail(Memory->read(S, Bytes));
    EXPECT_EQ(llvm::toHex(Bytes, true), C.Source);
    llvm::cantFail(Memory->read(S + 0x100, Bytes));
    EXPECT_EQ(llvm::toHex(Bytes, true), C.Previous);
    input(1);
    call("KeSetSystemGroupAffinityThread", {S, S + 0x100});
    EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + 0x100, 8)),
              C.After ? 1u : 0u);
  }
  input(0);
  call("KeRevertToUserGroupAffinityThread", {S});
  call("KeRevertToUserGroupAffinityThread", {0});
  // Affinity belongs to the execution thread, independently of the frame VA.
  input(1);
  call("KeSetSystemGroupAffinityThread", {S, 0});
  Model.enterExecution(profile::StackBase, 0x1234);
  input(1);
  call("KeSetSystemGroupAffinityThread", {S, S + 0x100});
  EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + 0x100, 8)), 0u);
  Model.enterExecution(profile::StackBase);
  input(1);
  call("KeSetSystemGroupAffinityThread", {S, S + 0x100});
  EXPECT_EQ(llvm::cantFail(Memory->readInteger(S + 0x100, 8)), 1u);
}

TEST(DriverProcessor, CompiledGuestAndReferenceDataAgree) {
  const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
             *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (unsigned Reference : {0u, 1u, 2u}) {
        if (Reference && (!Image || !PDB))
          continue;
        SCOPED_TRACE(::testing::Message() << unsigned(Contract) << " "
                                          << Preemptive << " " << Reference);
        auto O = nativeDataOptions();
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.Unload = true;
        O.Contract = Contract;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        if (Reference)
          O.KernelImage = Image;
        if (Reference == 2)
          O.KernelPDB = PDB;
        for (bool Write : {false, true}) {
          SCOPED_TRACE(Write);
          auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                     (Write ? "driver_processors_write.sys"
                                            : "driver_processors.sys"),
                                 O);
          ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
          EXPECT_EQ(R->Stop, Write ? DriverStopReason::ModelError
                                   : DriverStopReason::Returned)
              << R->Diagnostic;
          EXPECT_EQ(R->UnloadCompleted, !Write) << R->Diagnostic;
          if (Write)
            EXPECT_FALSE(R->NTStatus) << R->Diagnostic;
          else
            EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        }
      }
}
} // namespace
} // namespace neverd::emulation
