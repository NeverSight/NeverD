//===- KernelAcpiTests.cpp - ACPI inventory and shared MCFG ownership
//-------===//
///
/// \file
/// ACPI inventory and shared MCFG ownership.
///

#include "AcpiTestFixture.h"
#include "McfgTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelReferenceImage.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
std::string acpiJSON(const DriverAcpiTables &F) {
  llvm::json::Array Tables;
  for (const auto &B : F.Tables)
    Tables.push_back(llvm::toHex(B, true));
  return llvm::formatv(
             "{0}",
             llvm::json::Value(llvm::json::Object{
                 {"acpi_tables",
                  llvm::json::Object{{"tables_hex", std::move(Tables)}}}}))
      .str();
}
TEST(DriverAcpi, ExactBytesOrderDuplicatesAndEmptyInventoryRoundTrip) {
  for (auto F : {DriverAcpiTables{}, acpifixture::inventory()}) {
    auto O = driverOptionsFromScenarioJSON(acpiJSON(F));
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    ASSERT_TRUE(O->AcpiTables);
    EXPECT_EQ(O->AcpiTables->Tables, F.Tables);
    auto P = driverOptionsFromScenarioJSON("{}", *O);
    ASSERT_TRUE(bool(P));
    EXPECT_EQ(P->AcpiTables->Tables, F.Tables);
    DriverResult R;
    R.Configuration = *P;
    auto JSON = llvm::json::parse(driverResultJSON(R));
    ASSERT_TRUE(bool(JSON));
    auto *Tables = JSON->getAsObject()
                       ->getObject("configuration")
                       ->getObject("acpi_tables")
                       ->getArray("tables_hex");
    ASSERT_NE(Tables, nullptr);
    ASSERT_EQ(Tables->size(), F.Tables.size());
    for (size_t I = 0; I < Tables->size(); ++I)
      EXPECT_EQ((*Tables)[I].getAsString(), llvm::toHex(F.Tables[I], true));
  }
  auto O = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(bool(O));
  EXPECT_FALSE(O->AcpiTables);
}
TEST(DriverAcpi, MalformedTransportAndTablesAreRejected) {
  for (const char *Bad :
       {"null", "{}", "{\"tables_hex\":0}", "{\"tables_hex\":[0]}",
        "{\"tables_hex\":[\"zz\"]}", "{\"tables_hex\":[],\"extra\":0}"}) {
    auto O = driverOptionsFromScenarioJSON(std::string("{\"acpi_tables\":") +
                                           Bad + "}");
    ASSERT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
  std::vector<DriverAcpiTables> Bad;
  auto Add = [&](auto Mutate) {
    auto F = acpifixture::inventory();
    Mutate(F.Tables[0]);
    Bad.push_back(std::move(F));
  };
  Add([](auto &B) { B.resize(35); });
  Add([](auto &B) { B[4] ^= 1; });
  Add([](auto &B) { B[9] ^= 1; });
  Add([](auto &B) {
    B[0] = 0;
    acpifixture::checksum(B);
  });
  Add([](auto &B) {
    std::copy_n("FACS", 4, B.begin());
    acpifixture::checksum(B);
  });
  Add([](auto &B) { B.resize(firmware::AcpiMaxBytes + 1); });
  Bad.push_back({std::vector<std::vector<uint8_t>>(
      firmware::AcpiMaxTables + 1, acpifixture::inventory().Tables[0])});
  for (const auto &F : Bad) {
    auto E = validateDriverAcpiTables(F);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    DriverOptions O;
    O.AcpiTables = F;
    auto P = driverOptionsFromScenarioJSON("{}", O);
    ASSERT_FALSE(bool(P));
    llvm::consumeError(P.takeError());
  }
}
TEST(DriverAcpi, MCFGResolutionRejectsContradictionsAndUsesFirstDuplicate) {
  auto F = acpifixture::inventory();
  auto M = mcgffixture::table();
  F.Tables.push_back(M.Table);
  F.Tables.push_back(mcgffixture::table(0).Table);
  auto R = resolveDriverFirmwareMcfg(std::nullopt, F);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_TRUE(*R);
  EXPECT_EQ((**R).Table, M.Table);
  R = resolveDriverFirmwareMcfg(M, F);
  ASSERT_TRUE(bool(R));
  for (auto Legacy : {DriverPciMcfg{}, mcgffixture::table(0)}) {
    DriverOptions O;
    O.AcpiTables = F;
    O.PciMcfg = Legacy;
    auto P = driverOptionsFromScenarioJSON("{}", O);
    ASSERT_FALSE(bool(P));
    EXPECT_NE(llvm::toString(P.takeError()).find("conflicting"),
              std::string::npos);
  }
  auto BadMCFG = M;
  BadMCFG.Table[36] = 1;
  acpifixture::checksum(BadMCFG.Table);
  F.Tables.back() = BadMCFG.Table;
  auto E = validateDriverAcpiTables(F);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  R = resolveDriverFirmwareMcfg(std::nullopt, std::nullopt);
  ASSERT_TRUE(bool(R));
  EXPECT_FALSE(*R);
}
class KernelAcpi : public testing::Test {
protected:
  const uint64_t Buffer = 0x60000000, Returned = 0x60002000;
  DriverResult Result;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelReferenceImage> Reference;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  llvm::Error initialize(DriverOptions O) {
    Model.reset();
    Reference.reset();
    Memory.reset();
    auto B = UnicornBackend::create(16 * 1024 * 1024);
    if (!B)
      return B.takeError();
    Memory = std::move(*B);
    if (!O.KernelImage.empty()) {
      auto Ref =
          KernelReferenceImage::load(O.KernelImage, O.MemoryLimit, O.KernelPDB);
      if (!Ref)
        return Ref.takeError();
      Reference = std::make_unique<KernelReferenceImage>(std::move(*Ref));
      if (auto E = Reference->map(*Memory))
        return E;
    }
    Model = std::make_unique<KernelModel>(*Memory, Result);
    if (Reference)
      if (auto E = Model->setDebuggerReference(*Reference))
        return E;
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    if (auto E = Model->initialize(I, O))
      return E;
    if (auto E = Model->initializeDebuggerState())
      return E;
    if (auto E = Memory->map(Buffer, 0x3000, Read | Write))
      return E;
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    return llvm::Error::success();
  }
  DriverOptions options() {
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.AcpiTables = acpifixture::inventory();
    return O;
  }
  void request(uint32_t Action = 0, uint32_t ID = 0) {
    std::vector<uint8_t> B(4096, 0xa5);
    llvm::support::endian::write32le(B.data(), 0x41435049);
    llvm::support::endian::write32le(B.data() + 4, Action);
    llvm::support::endian::write32le(B.data() + 8, ID);
    llvm::support::endian::write32le(B.data() + 12, UINT32_MAX);
    ok(Memory->write(Buffer, B));
    ok(Memory->writeInteger(Returned, 0xfeedbeef, 4));
  }
  uint64_t get(uint64_t At, unsigned Width = 4) {
    auto R = Memory->readInteger(At, Width);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  uint64_t query(uint32_t Length) {
    auto R =
        Model->call("ZwQuerySystemInformation", {76, Buffer, Length, Returned});
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
};
TEST_F(KernelAcpi, EnumerationPreservesOrderDuplicatesAndTrueOuterCapacity) {
  auto O = options();
  ok(initialize(O));
  O.AcpiTables->Tables.clear();
  for (uint32_t Capacity : {0, 3, 11, 12, 20}) {
    request(0, 0xabcdef01);
    EXPECT_EQ(query(Capacity + 16), Capacity < 12 ? 0xc0000023u : 0u);
    EXPECT_EQ(get(Buffer + 12), 12u);
    EXPECT_EQ(get(Returned), 28u);
    if (Capacity >= 12) {
      EXPECT_EQ(get(Buffer + 16), 0x43495041u);
      EXPECT_EQ(get(Buffer + 20), 0x54445353u);
      EXPECT_EQ(get(Buffer + 24), 0x54445353u);
    } else
      EXPECT_EQ(get(Buffer + 16), 0xa5a5a5a5u);
    EXPECT_EQ(get(Buffer + 28), 0xa5a5a5a5u);
    EXPECT_EQ(get(Buffer + 8), 0xabcdef01u);
    EXPECT_NE(Model->callDetail().find("ACPI action=0"), std::string::npos);
  }
}
TEST_F(KernelAcpi, GetCopiesFirstMatchingTableAndOutputCannotAlterSource) {
  ok(initialize(options()));
  for (uint32_t ID : {0x43495041u, 0x54445353u}) {
    const auto Expected =
        acpifixture::inventory().Tables[ID == 0x43495041u ? 0 : 1];
    for (unsigned Round = 0; Round != 2; ++Round) {
      request(1, ID);
      EXPECT_EQ(query(16), 0xc0000023u);
      EXPECT_EQ(get(Returned), Expected.size() + 16);
      EXPECT_EQ(get(Buffer + 16), 0xa5a5a5a5u);
      EXPECT_EQ(query(Expected.size() + 16), 0u);
      std::vector<uint8_t> Actual(Expected.size());
      ok(Memory->read(Buffer + 16, Actual));
      EXPECT_EQ(Actual, Expected);
      ok(Memory->writeInteger(Buffer + 16, 0, 4));
    }
  }
}
TEST_F(KernelAcpi, NativeErrorPathsRetainCapacityAndDoNotCopyPayload) {
  for (bool Empty : {false, true}) {
    auto O = options();
    if (Empty)
      O.AcpiTables->Tables.clear();
    ok(initialize(O));
    for (uint32_t Action : {0, 1, 2}) {
      if (!Empty && Action == 0)
        continue;
      request(Action, UINT32_MAX);
      const uint32_t Expected = Action == 0   ? 0xc0140019
                                : Action == 1 ? 0xc0000225
                                              : 0xc0000002;
      EXPECT_EQ(query(19), Expected);
      EXPECT_EQ(get(Buffer + 12), 3u);
      EXPECT_EQ(get(Returned), 19u);
      EXPECT_EQ(get(Buffer + 16), 0xa5a5a5a5u);
    }
  }
}
TEST_F(KernelAcpi, MissingInventoryAndBadTypedInputFailClearly) {
  auto O = options();
  O.AcpiTables.reset();
  ok(initialize(O));
  request();
  auto R = Model->call("ZwQuerySystemInformation", {76, Buffer, 16, Returned});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("requires explicit acpi_tables"),
            std::string::npos);
  EXPECT_EQ(get(Buffer + 12), UINT32_MAX);
  EXPECT_EQ(get(Returned), 0xfeedbeefu);
  O = options();
  O.AcpiTables->Tables[0][9] ^= 1;
  auto E = initialize(O);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_EQ(Model->driverObject(), 0u);
}
TEST_F(KernelAcpi, CommonPreflightAndModeRestrictionsApplyToAcpi) {
  ok(initialize(options()));
  request();
  ok(Memory->protect(Returned, 4096, Read));
  auto R =
      Model->call("ZwQuerySystemInformation", {76, Buffer, 4096, Returned});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(Buffer + 12), UINT32_MAX);
  EXPECT_EQ(get(Buffer + 16), 0xa5a5a5a5u);
  ok(Memory->protect(Returned, 4096, Read | Write));
  ok(Model->setUserRequestContext(true, 0x3456));
  R = Model->call("NtQuerySystemInformation", {76, Buffer, 4096, Returned});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("UserMode"), std::string::npos);
  EXPECT_EQ(query(4096), 0u);
}
TEST_F(KernelAcpi, InventoryMCFGPublishesSameBytesThroughAuthenticatedGlobals) {
  for (bool Present : {false, true}) {
    auto O = options();
    auto M = mcgffixture::table();
    if (Present)
      O.AcpiTables->Tables.push_back(M.Table);
    O.KernelImage = std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                    "driver_kernel_debugger_mcfg.sys";
    O.KernelPDB = std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                  "driver_kernel_debugger_mcfg.pdb";
    ok(initialize(O));
    const auto &F = *Reference->debugger();
    EXPECT_EQ(get(F.PciMcfgCount), Present ? 2u : 0u);
    const auto Pointer = get(F.PciMcfgPointer, 8);
    request(1, 0x4746434d);
    EXPECT_EQ(query(4096), Present ? 0u : 0xc0000225u);
    if (Present) {
      std::vector<uint8_t> Native(M.Table.size()), API(M.Table.size());
      ok(Model->validateGuestAccess(Pointer, Native.size(), false));
      ok(Memory->read(Pointer, Native));
      ok(Memory->read(Buffer + 16, API));
      EXPECT_EQ(Native, M.Table);
      EXPECT_EQ(API, Native);
      auto E = Model->validateGuestAccess(Pointer, 1, true);
      ASSERT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
    } else
      EXPECT_EQ(Pointer, 0u);
  }
}
TEST_F(KernelAcpi,
       DerivedMcfgQueriesNeedNoSymbolsButNeverPublishUnownedFields) {
  auto O = options();
  auto M = mcgffixture::table();
  O.AcpiTables->Tables.push_back(M.Table);
  ok(initialize(O));
  request(1, 0x4746434d);
  EXPECT_EQ(query(4096), 0u);
  std::vector<uint8_t> Actual(M.Table.size());
  ok(Memory->read(Buffer + 16, Actual));
  EXPECT_EQ(Actual, M.Table);
  auto E = Model->validateGuestAccess(windows::FirmwareMcfgBase, 1, false);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.PciMcfg = M;
  E = initialize(O);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("matching kernel PE/PDB"),
            std::string::npos);
  EXPECT_EQ(Model->driverObject(), 0u);
}
TEST_F(KernelAcpi,
       LegacyMCFGDoesNotCloseAcpiInventoryAndConflictsNeverPublish) {
  auto O = options();
  O.AcpiTables.reset();
  O.PciMcfg = mcgffixture::table();
  O.KernelImage = std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                  "driver_kernel_debugger_mcfg.sys";
  O.KernelPDB = std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                "driver_kernel_debugger_mcfg.pdb";
  ok(initialize(O));
  request();
  auto R =
      Model->call("ZwQuerySystemInformation", {76, Buffer, 4096, Returned});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  O.AcpiTables = acpifixture::inventory();
  auto E = initialize(O);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("conflicting"),
            std::string::npos);
  EXPECT_EQ(Model->driverObject(), 0u);
}
TEST(KernelAcpiCaller, CompiledClientEnumeratesDuplicatesReadsAndUnloads) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.AcpiTables = acpifixture::inventory();
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_acpi_tables.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u);
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      EXPECT_EQ(R->Messages,
                std::vector<std::string>{"ACPI inventory validated"});
      unsigned Count = 0;
      for (const auto &C : R->Calls)
        if (C.Name == "ZwQuerySystemInformation") {
          EXPECT_TRUE(llvm::StringRef(C.Detail).starts_with("ACPI action="));
          ++Count;
        }
      EXPECT_EQ(Count, 6u);
    }
}
} // namespace
} // namespace neverd::emulation
