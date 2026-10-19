///
/// \file
/// KernelPciConfigurationTests.
///

#include "PciConfigurationTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelReferenceImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"

#include <cstdlib>
namespace neverd::emulation {
namespace {
TEST(DriverPciScenario, RoundTripsExplicitRegistersAndRejectsIncompleteFacts) {
  auto O = pciConfigurationOptions();
  DriverResult R;
  R.Configuration = O;
  auto ParsedJSON = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(ParsedJSON)) << llvm::toString(ParsedJSON.takeError());
  auto J = std::move(*ParsedJSON);
  auto Text = llvm::formatv("{0}", *J.getAsObject()
                                        ->getObject("configuration")
                                        ->get("pci_configuration"))
                  .str();
  const auto Prefix = std::string(
      R"({"windows_version":{"major":10,"minor":0,"build":19041},"pci_configuration":)");
  auto Copy = driverOptionsFromScenarioJSON(Prefix + Text + "}");
  ASSERT_TRUE(bool(Copy)) << llvm::toString(Copy.takeError());
  ASSERT_TRUE(Copy->PciConfiguration);
  EXPECT_EQ(Copy->PciConfiguration->Devices[0].Configuration,
            O.PciConfiguration->Devices[0].Configuration);
  EXPECT_EQ(Copy->PciConfiguration->Devices[0].WriteMask,
            O.PciConfiguration->Devices[0].WriteMask);
  auto Inherited = driverOptionsFromScenarioJSON("{}", *Copy);
  ASSERT_TRUE(bool(Inherited));
  EXPECT_EQ(Inherited->PciConfiguration->Devices[0].WriteOneToClearMask,
            O.PciConfiguration->Devices[0].WriteOneToClearMask);
  for (
      const char *Bad :
      {"null", "true", "[]", "{}",
       R"({"minimum_bus_number":0,"maximum_bus_number":0,"devices":[],"extra":1})",
       R"({"minimum_bus_number":2,"maximum_bus_number":1,"devices":[]})",
       R"({"minimum_bus_number":0,"maximum_bus_number":16777216,"devices":[]})",
       R"({"minimum_bus_number":0,"maximum_bus_number":0,"devices":[{"segment":0,"bus":0,"device":0,"function":0,"config_hex":"00"}]})"}) {
    auto BadO = driverOptionsFromScenarioJSON(Prefix + Bad + "}");
    ASSERT_FALSE(bool(BadO)) << Bad;
    llvm::consumeError(BadO.takeError());
  }
  for (unsigned Case = 0; Case < 10; ++Case) {
    auto Bad = O;
    auto &D = Bad.PciConfiguration->Devices[0];
    switch (Case) {
    case 0:
      Bad.Version.reset();
      break;
    case 1:
      D.Configuration.resize(4095);
      break;
    case 2:
      D.Configuration[0] = D.Configuration[1] = 255;
      break;
    case 3:
      D.Configuration[14] = 2;
      break;
    case 4:
      D.WriteMask.reset();
      break;
    case 5:
      (*D.WriteMask)[6] = 0xf0;
      break;
    case 6:
      (*D.WriteMask)[16] = 255;
      break;
    case 7:
      D.Function = 1;
      break;
    case 8:
      Bad.PciConfiguration->Devices.push_back(D);
      break;
    case 9:
      D.Bus = 4;
      break;
    }
    auto E = validateDriverPciConfiguration(Bad);
    ASSERT_TRUE(bool(E)) << Case;
    llvm::consumeError(std::move(E));
  }
}
class KernelPci : public testing::Test {
protected:
  static constexpr uint64_t S = 0x60000000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void initialize(DriverOptions O = pciConfigurationOptions()) {
    Model.reset();
    Result = {};
    auto Backend = UnicornBackend::create(8 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    ok(Memory->map(S, 8192, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  llvm::Expected<uint64_t> access(bool Write, uint64_t Offset, uint64_t Length,
                                  uint64_t Buffer = S, uint64_t Bus = 0x203,
                                  uint64_t Slot = 5, uint64_t Type = 4) {
    return Model->call(Write ? "HalSetBusDataByOffset"
                             : "HalGetBusDataByOffset",
                       {Type, Bus, Slot, Buffer, Offset, Length});
  }
  uint64_t value(llvm::Expected<uint64_t> R) {
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  void rejected(llvm::Expected<uint64_t> R) {
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
};
TEST_F(KernelPci, OriginalWrapperMatrixPreservesReadPayloadAndWriteCounts) {
  struct Case {
    bool Write, Present;
    uint32_t Header, Offset, Length, Count;
    const char *Digest;
  };
  const Case Cases[] = {
#define NEVERD_PCI_NATIVE_CASE(W, P, H, O, L, C, S) {W, P, H, O, L, C, S},
#include "PciConfigurationNativeCases.def"
#undef NEVERD_PCI_NATIVE_CASE
  };
  EXPECT_EQ(std::size(Cases), 104u);
  for (const auto &C : Cases) {
    SCOPED_TRACE(std::to_string(C.Write) + ":" + std::to_string(C.Present) +
                 ":" + std::to_string(C.Header) + ":" +
                 std::to_string(C.Offset) + ":" + std::to_string(C.Length));
    auto O = pciConfigurationOptions();
    O.PciConfiguration->Devices[0].Configuration[14] = C.Header;
    if (!C.Present)
      O.PciConfiguration->Devices.clear();
    initialize(O);
    ok(Memory->write(S, std::vector<uint8_t>(4096, 0xa5)));
    auto R = access(C.Write, C.Offset, C.Length);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(*R, C.Count);
    std::vector<uint8_t> Bytes(4096);
    ok(Memory->read(S, Bytes));
    EXPECT_EQ(llvm::toHex(llvm::ArrayRef(llvm::SHA256::hash(Bytes)), true),
              C.Digest);
  }
}
TEST_F(KernelPci, MaskedWritesReadOnlyBytesAndSnapshotShareOneOwner) {
  auto O = pciConfigurationOptions();
  initialize(O);
  O.PciConfiguration->Devices[0].Configuration[0] =
      0; // Input changes cannot mutate the model.
  ok(Memory->write(S, std::vector<uint8_t>{5, 0, 0xf0, 0xff}));
  EXPECT_EQ(value(access(true, 4, 4)), 4u);
  EXPECT_EQ(value(access(false, 4, 4)), 4u);
  EXPECT_EQ(value(Memory->readInteger(S, 4)), 0x00060005u);
  ok(Memory->write(S, std::vector<uint8_t>(64, 0xa5)));
  EXPECT_EQ(value(access(true, 0, 64)), 64u);
  EXPECT_EQ(value(access(false, 0, 4)), 4u);
  EXPECT_EQ(value(Memory->readInteger(S, 4)), 0x56781234u);
  ok(Memory->writeInteger(S, 0xddccbbaa, 4));
  EXPECT_EQ(value(access(true, 0x100, 4)), 4u);
  ok(Model->snapshot());
  ASSERT_TRUE(Result.PciConfiguration);
  EXPECT_EQ(Result.PciConfiguration->Devices[0].Configuration[0x100], 0xaa);
  EXPECT_EQ(Result.PciConfiguration->Devices[0].Configuration[0x103], 0xdd);
}
TEST_F(KernelPci, MissingFactsAndInvalidBuffersHaveNoPartialDeviceWrites) {
  auto O = pciConfigurationOptions();
  O.PciConfiguration.reset();
  initialize(O);
  rejected(access(false, 0, 4));
  O = pciConfigurationOptions();
  O.PciConfiguration->Devices[0].WriteMask.reset();
  O.PciConfiguration->Devices[0].WriteOneToClearMask.reset();
  initialize(O);
  EXPECT_EQ(value(access(false, 0, 4)), 4u);
  rejected(access(true, 0x100, 4));
  initialize();
  rejected(access(false, 4096, 1));
  rejected(access(true, 4094, 4));
  rejected(access(false, 0, 4, 0));
  rejected(access(false, 0, 4, S, 0x203, 256));
  rejected(access(false, 0, 1, S, 0x203, 5, 0));
  ok(Memory->protect(S + 4096, 4096, 0));
  rejected(access(true, 0x100, 4, S + 4094));
  ok(Model->snapshot());
  ASSERT_TRUE(Result.PciConfiguration);
  EXPECT_EQ(Result.PciConfiguration->Devices[0].Configuration[0x100], 0u);
  auto Paged = Model->call("ExAllocatePoolWithTag", {1, 16, 0x504349});
  ASSERT_TRUE(bool(Paged));
  rejected(access(false, 0, 4, *Paged));
  EXPECT_EQ(value(Model->call("ExFreePoolWithTag", {*Paged, 0x504349})), 0u);
}
TEST_F(KernelPci, BoundariesTruncateUlongAndPreserveIrqlAndBufferPermissions) {
  initialize();
  EXPECT_EQ(value(access(false, 0, 0, 0)), 0u);
  EXPECT_EQ(value(access(false, 0, 4, 0, 0x204)), 0u);
  EXPECT_EQ(value(access(true, 0, 4, 0, 0x204)), 0u);
  EXPECT_EQ(value(access(false, 0, 4, 0, 0x203, 5, 8)), 0u);
  EXPECT_EQ(value(access(false, 0x100000000, 0x100000004, S, 0x100000203,
                         0x100000005, 0x100000004)),
            4u);
  for (uint8_t Level : {0, 1, 2, 15}) {
    EXPECT_EQ(value(Model->call("KfRaiseIrql", {Level})), 0u);
    EXPECT_EQ(value(access(false, 0, 4)), 4u);
    EXPECT_EQ(Model->currentIRQL(), Level);
    EXPECT_EQ(value(Model->call("KeLowerIrql", {0})), 0u);
  }
  ok(Memory->protect(S, 4096, Read));
  rejected(access(false, 0, 4));
  EXPECT_EQ(value(access(true, 0x100, 4)), 4u);
}
TEST_F(KernelPci, ScopedHalContractsDoNotAliasOtherModules) {
  initialize();
  for (const char *Name : {"HalGetBusDataByOffset", "HalSetBusDataByOffset"}) {
    KernelExportRegistry::Export Export{Name, "hal.dll"};
    EXPECT_EQ(Model->argumentCount(Export), 6u);
    const uint64_t A[] = {4, 0x203, 5, S, 0, 4};
    auto R = Model->call(Export, A, [](unsigned) -> llvm::Expected<uint64_t> {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unexpected stack fetch");
    });
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(*R, 4u);
    for (const char *Module : {"ndis.sys", "fltmgr.sys"}) {
      Export.Module = Module;
      EXPECT_FALSE(Model->argumentCount(Export));
    }
  }
}
TEST_F(KernelPci, MultifunctionAndSegmentIdentitiesRemainIndependent) {
  auto O = pciConfigurationOptions();
  auto D = O.PciConfiguration->Devices[0];
  O.PciConfiguration->Devices[0].Configuration[14] = 0x80;
  D.Function = 1;
  D.Configuration[0] = 0x78;
  D.Configuration[1] = 0x56;
  O.PciConfiguration->Devices.push_back(D);
  initialize(O);
  EXPECT_EQ(value(access(false, 0, 2, S, 0x203, 5)), 2u);
  EXPECT_EQ(value(Memory->readInteger(S, 2)), 0x1234u);
  EXPECT_EQ(value(access(false, 0, 2, S, 0x203, 5 | (1 << 5))), 2u);
  EXPECT_EQ(value(Memory->readInteger(S, 2)), 0x5678u);
  EXPECT_EQ(value(access(false, 0, 4, S, 3, 5)), 2u);
  EXPECT_EQ(value(Memory->readInteger(S, 2)), 0xffffu);
  ok(Memory->writeInteger(S, 0xddccbbaa, 4));
  EXPECT_EQ(value(access(true, 0x100, 4, S, 0x203, 5 | (1 << 5))), 4u);
  EXPECT_EQ(value(access(false, 0x100, 4, S, 0x203, 5)), 4u);
  EXPECT_EQ(value(Memory->readInteger(S, 4)), 0x03020100u);
}
TEST(KernelPciDriver,
     MatchedReferencePreservesModuleNamespacesAndRealKernelEntry) {
  const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
             *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = pciConfigurationOptions();
      O.Contract = Contract;
      O.Unload = true;
      O.KernelImage = Image;
      O.KernelPDB = PDB;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_pci_configuration.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u);
      EXPECT_TRUE(R->UnloadCompleted);
      bool Found = false;
      for (const auto &C : R->Calls)
        if (C.Name == "HalGetBusDataByOffset" && C.PC >= 0xfffff80000000000ULL)
          Found = true;
      EXPECT_TRUE(Found);
      ASSERT_TRUE(R->PciConfiguration);
      EXPECT_EQ(R->PciConfiguration->Devices[0].Configuration[0x100], 0xaa);
    }
}
TEST(KernelPciDriver, HalImportsAndDynamicKernelEntryObserveTheSameRegisters) {
  const auto Root = std::filesystem::path(NEVERD_DRIVER_FIXTURES);
  auto Image =
      loadDriverImage(Root / "driver_pci_configuration.sys", 64 * 1024 * 1024);
  ASSERT_TRUE(bool(Image));
  unsigned Hal = 0;
  for (const auto &I : Image->Imports)
    if (I.Name == "HalGetBusDataByOffset" ||
        I.Name == "HalSetBusDataByOffset") {
      EXPECT_EQ(I.Module, "HAL.dll");
      ++Hal;
    }
  EXPECT_EQ(Hal, 2u);
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Paging : {false, true}) {
        auto O = pciConfigurationOptions();
        O.Contract = Contract;
        O.PageTables = Paging;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        auto R = emulateDriver(Root / "driver_pci_configuration.sys", O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u);
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        EXPECT_EQ(R->Messages,
                  (std::vector<std::string>{"PCI configuration passed",
                                            "PCI unload passed"}));
        ASSERT_TRUE(R->PciConfiguration);
        EXPECT_EQ(R->PciConfiguration->Devices[0].Configuration[0x100], 0xaa);
      }
}
} // namespace
} // namespace neverd::emulation
