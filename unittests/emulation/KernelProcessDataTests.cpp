//===- KernelProcessDataTests.cpp - Shared System process data identity
//----===//
///
/// \file
/// Shared System process data identity.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelProcessData.h"
#include "os/windows/kernel/KernelReferenceImage.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace neverd::emulation {
namespace {
DriverOptions nativeDataOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  return O;
}

std::filesystem::path processDataFixture(const char *Name) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}
TEST(KernelProcessData, StaticDynamicAndReferenceNamespaceShareTypedSlots) {
  KernelExportRegistry R;
  llvm::cantFail(R.initialize(nativeDataOptions()));
  const auto Original = llvm::cantFail(R.resolve("PsInitialSystemProcess"));
  EXPECT_EQ(Original, process_data::InitialSystemProcessSlot);
  EXPECT_EQ(llvm::cantFail(R.bindImport(
                {0x1000, "ntkrnlmp.exe", "PsInitialSystemProcess"})),
            Original);
  ASSERT_NE(R.lookup(Original), nullptr);
  EXPECT_EQ(R.lookup(Original)->Kind,
            KernelExportRegistry::ExportKind::ProcessData);
  EXPECT_FALSE(KernelModel::argumentCount(*R.lookup(Original)));
  constexpr uint64_t Native = 0xfffff80000008000;
  llvm::cantFail(R.attachReferenceImage({{"PsInitialSystemProcess", 0}}, {}, {},
                                        {{"PsInitialSystemProcess", Native}}));
  EXPECT_EQ(llvm::cantFail(R.resolve("PsInitialSystemProcess")), Native);
  EXPECT_EQ(llvm::cantFail(R.bindImport(
                {0x1000, "NTOSKRNL.EXE", "PsInitialSystemProcess"})),
            Native);
  EXPECT_EQ(R.lookup(Original), nullptr);
  EXPECT_EQ(R.lookup(Native)->Kind,
            KernelExportRegistry::ExportKind::ProcessData);
  EXPECT_FALSE(KernelModel::argumentCount(*R.lookup(Native)));
}
TEST(KernelProcessData, InvalidReferenceDataDoesNotPartiallyPublish) {
  for (unsigned Case = 0; Case < 6; ++Case) {
    KernelExportRegistry R;
    llvm::cantFail(R.initialize(nativeDataOptions()));
    const auto Original = llvm::cantFail(R.resolve("PsInitialSystemProcess"));
    const auto Capacity = R.availableThunkCount();
    std::map<std::string, uint64_t> Inventory{{"PsInitialSystemProcess", 0}},
        Data{{"PsInitialSystemProcess", 0xfffff80000008000}};
    if (Case == 0)
      Data.begin()->second = 0;
    if (Case == 1)
      ++Data.begin()->second;
    if (Case == 2)
      Inventory.begin()->second = 0xfffff80000001000;
    if (Case == 3)
      Inventory.clear();
    if (Case == 4)
      Data = {{"UnknownProcessData", 0xfffff80000008000}};
    if (Case == 5)
      Data.clear();
    auto E = R.attachReferenceImage(Inventory, {}, {}, Data);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(llvm::cantFail(R.resolve("PsInitialSystemProcess")), Original);
    EXPECT_EQ(R.availableThunkCount(), Capacity);
  }
}
TEST(KernelProcessData, AbsenceAndPresentOverridesRetainTheirMeaning) {
  auto O = nativeDataOptions();
  O.KernelExports["PsInitialSystemProcess"] = false;
  KernelExportRegistry Absent;
  llvm::cantFail(Absent.initialize(O));
  EXPECT_EQ(llvm::cantFail(Absent.resolve("PsInitialSystemProcess")), 0u);
  auto Import =
      Absent.bindImport({0x1000, "ntoskrnl.exe", "PsInitialSystemProcess"});
  ASSERT_FALSE(bool(Import));
  llvm::consumeError(Import.takeError());
  llvm::cantFail(Absent.attachReferenceImage(
      {{"PsInitialSystemProcess", 0}}, O, {},
      {{"PsInitialSystemProcess", 0xfffff80000008000}}));
  EXPECT_EQ(llvm::cantFail(Absent.resolve("PsInitialSystemProcess")), 0u);
  O.KernelExports.begin()->second = true;
  KernelExportRegistry Present;
  llvm::cantFail(Present.initialize(O));
  EXPECT_EQ(llvm::cantFail(Present.resolve("PsInitialSystemProcess")),
            process_data::InitialSystemProcessSlot);
  KernelExportRegistry Missing;
  llvm::cantFail(Missing.initialize(nativeDataOptions()));
  llvm::cantFail(Missing.attachReferenceImage({}, {}));
  EXPECT_EQ(llvm::cantFail(Missing.resolve("PsInitialSystemProcess")), 0u);
}
void checkProcessData(DriverOptions O) {
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  KernelExportRegistry Exports;
  llvm::cantFail(Exports.initialize(O));
  std::optional<KernelReferenceImage> Reference;
  if (!O.KernelImage.empty()) {
    Reference = llvm::cantFail(
        KernelReferenceImage::load(O.KernelImage, O.MemoryLimit, O.KernelPDB));
    auto E = Exports.attachReference(*Reference, O);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    ASSERT_TRUE(
        Reference->processDataExports().contains("PsInitialSystemProcess"));
  }
  auto Memory = llvm::cantFail(UnicornBackend::create(O.MemoryLimit));
  if (Reference)
    llvm::cantFail(Reference->map(*Memory));
  DriverResult Result;
  KernelModel Model(*Memory, Result, &Exports);
  if (Reference)
    llvm::cantFail(Model.setDebuggerReference(*Reference));
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 4096;
  Image.Size = 0x3000;
  llvm::cantFail(Model.initialize(Image, O));
  llvm::cantFail(Model.initializeDebuggerState());
  Model.enterForeground();
  Model.enterExecution(profile::StackBase);
  const auto Slot = llvm::cantFail(Exports.resolve("PsInitialSystemProcess"));
  const auto System = llvm::cantFail(Memory->readInteger(Slot, 8));
  ASSERT_NE(System, 0u);
  EXPECT_NE(System, 0x1122334455667788ULL);
  EXPECT_EQ(System, llvm::cantFail(Model.call("IoGetCurrentProcess", {})));
  EXPECT_EQ(llvm::cantFail(Model.call("PsGetProcessId", {System})), 4u);
  if (Reference)
    EXPECT_EQ(Slot,
              Reference->processDataExports().at("PsInitialSystemProcess"));
  llvm::cantFail(Model.validateGuestAccess(Slot, 8, false));
  llvm::cantFail(Model.validateGuestAccess(Slot + 7, 1, false));
  for (auto [At, Size, Write] :
       {std::tuple<uint64_t, uint32_t, bool>{Slot, 8, true},
        {Slot - 1, 2, false},
        {Slot + 1, 8, false},
        {Slot + 8, 1, false}}) {
    auto E = Model.validateGuestAccess(At, Size, Write);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  auto CannotCall = Model.call(*Exports.lookup(Slot), {}, nullptr);
  ASSERT_FALSE(bool(CannotCall));
  EXPECT_NE(llvm::toString(CannotCall.takeError()).find("not callable"),
            std::string::npos);
  const auto Scratch =
      llvm::cantFail(Model.call("ExAllocatePoolWithTag", {0, 32, 1}));
  llvm::cantFail(Model.setUserRequestContext(true, 100));
  EXPECT_NE(llvm::cantFail(Model.call("IoGetCurrentProcess", {})), System);
  EXPECT_EQ(llvm::cantFail(Memory->readInteger(Slot, 8)), System);
  EXPECT_EQ(
      llvm::cantFail(Model.call("PsLookupProcessByProcessId", {4, Scratch})),
      0u);
  EXPECT_EQ(llvm::cantFail(Memory->readInteger(Scratch, 8)), System);
  llvm::cantFail(Model.call("ObfDereferenceObject", {System}));
  llvm::cantFail(Model.call("ExFreePoolWithTag", {Scratch, 1}));
}
TEST(KernelProcessData, DefaultAndPublicReferenceUseExistingProcessIdentity) {
  checkProcessData({});
  auto O = nativeDataOptions();
  O.KernelImage = processDataFixture("driver_process_data_reference");
  checkProcessData(O);
}
TEST(KernelProcessData, MatchedNativeImageAndPDBUseTheSameSystemProcess) {
  const auto *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
             *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows reference PE/PDB";
  for (bool WithPDB : {false, true}) {
    auto O = nativeDataOptions();
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.KernelImage = Image;
    if (WithPDB)
      O.KernelPDB = PDB;
    checkProcessData(O);
  }
}
TEST(KernelProcessData,
     LoaderRejectsExecutableUnalignedAndExportMetadataSlots) {
  auto Buffer =
      llvm::cantFail(llvm::errorOrToExpected(llvm::MemoryBuffer::getFile(
          processDataFixture("driver_process_data_reference").string())));
  std::vector<uint8_t> Bytes(Buffer->getBufferStart(), Buffer->getBufferEnd());
  auto Object =
      llvm::cantFail(llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                          Bytes.size()),
          "process-data")));
  uint32_t RVA = 0, Index = 0;
  for (const auto &E : Object->export_directories()) {
    llvm::StringRef Name;
    llvm::cantFail(E.getSymbolName(Name));
    if (Name != "PsInitialSystemProcess")
      continue;
    uint32_t Ordinal = 0, Base = 0;
    llvm::cantFail(E.getExportRVA(RVA));
    llvm::cantFail(E.getOrdinal(Ordinal));
    llvm::cantFail(E.getOrdinalBase(Base));
    Index = Ordinal - Base;
  }
  ASSERT_NE(RVA, 0u);
  const auto *Directory = Object->getDataDirectory(llvm::COFF::EXPORT_TABLE);
  uintptr_t Address = 0, TableAddress = 0;
  llvm::cantFail(Object->getRvaPtr(Directory->RelativeVirtualAddress, Address));
  const auto *Table =
      reinterpret_cast<const llvm::object::export_directory_table_entry *>(
          Address);
  llvm::cantFail(Object->getRvaPtr(Table->ExportAddressTableRVA, TableAddress));
  const auto Offset = reinterpret_cast<const uint8_t *>(TableAddress) -
                      Bytes.data() + 4 * Index;
  llvm::SmallString<128> Temp;
  llvm::cantFail(llvm::errorCodeToError(
      llvm::sys::fs::createUniqueDirectory("neverd-process-data", Temp)));
  const auto Path = std::filesystem::path(Temp.str().str()) / "reference.sys";
  for (uint32_t Replacement :
       {uint32_t(Object->getPE32PlusHeader()->AddressOfEntryPoint), RVA + 1,
        uint32_t(Directory->RelativeVirtualAddress)}) {
    llvm::support::endian::write32le(Bytes.data() + Offset, Replacement);
    {
      std::ofstream Out(Path, std::ios::binary);
      Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
      ASSERT_TRUE(Out);
    }
    auto Rejected = KernelReferenceImage::load(Path, 4 * 1024 * 1024);
    ASSERT_FALSE(bool(Rejected));
    llvm::consumeError(Rejected.takeError());
  }
  std::error_code Ignored;
  std::filesystem::remove_all(Path.parent_path(), Ignored);
}
TEST(KernelProcessData, CompiledStaticAndDynamicClientsCompleteAndUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Reference : {false, true}) {
      auto O = nativeDataOptions();
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.Contract = Contract;
      O.Unload = true;
      if (Reference) {
        O.KernelImage = processDataFixture("driver_process_data_reference");
        for (const char *Name :
             {"IoGetCurrentProcess", "PsGetProcessId",
              "PsLookupProcessByProcessId", "ObfDereferenceObject",
              "MmGetSystemRoutineAddress", "DbgPrint"})
          O.KernelExports[Name] = true;
      }
      auto R = emulateDriver(processDataFixture("driver_process_data"), O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted);
    }
}
TEST(KernelProcessData, CompiledWritesCrossReadsAndExecutionRemainRejected) {
  for (const char *Name :
       {"driver_process_data_write", "driver_process_data_cross",
        "driver_process_data_execute"}) {
    auto R = emulateDriver(processDataFixture(Name), nativeDataOptions());
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_NE(R->Stop, DriverStopReason::Returned);
    EXPECT_FALSE(R->NTStatus.has_value());
    EXPECT_FALSE(R->Diagnostic.empty());
  }
}
} // namespace
} // namespace neverd::emulation
