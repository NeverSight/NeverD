///
/// \file
/// RegistryHivesTests.
///

#include "RegistryHivesTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "llvm/Support/FormatVariadic.h"

#include <cstdlib>
namespace neverd::emulation {
namespace {
TEST(RegistryHivesScenario,
     SharedValidationRejectsAmbiguousOrUnknownProviders) {
  auto O = test::registryHivesOptions();
  auto Text =
      llvm::formatv("{0}", llvm::json::Value(test::registryHivesScenario()))
          .str();
  auto Copy = driverOptionsFromScenarioJSON(Text);
  ASSERT_TRUE(bool(Copy)) << llvm::toString(Copy.takeError());
  ASSERT_TRUE(Copy->RegistryHives);
  EXPECT_EQ(Copy->RegistryHives->size(), 2u);
  auto Inherited = driverOptionsFromScenarioJSON("{}", *Copy);
  ASSERT_TRUE(bool(Inherited));
  EXPECT_EQ(Inherited->RegistryHives->at(0).Path, O.RegistryHives->at(0).Path);
  for (unsigned Case = 0; Case < 7; ++Case) {
    auto Bad = O;
    switch (Case) {
    case 0:
      Bad.Version.reset();
      break;
    case 1:
      Bad.Registry.reset();
      break;
    case 2:
      Bad.RegistryHives->push_back(Bad.RegistryHives->at(0));
      break;
    case 3:
      Bad.RegistryHives->push_back({"\\Registry\\Machine\\Software\\A"});
      break;
    case 4:
      Bad.RegistryHives->at(0).Path = "\\Registry\\Machine";
      break;
    case 5:
      Bad.RegistryHives->at(0).Path = "\\Registry\\Machine\\Absent";
      break;
    case 6:
      Bad.RegistryHives->at(0).Path =
          "\\Registry\\Machine\\Software\\..\\System";
      break;
    }
    auto E = validateDriverRegistryHives(Bad);
    ASSERT_TRUE(bool(E)) << Case;
    llvm::consumeError(std::move(E));
  }
  for (const char *Bad :
       {"null", "{}", "true", R"([{"path":"x","storage":"host"}])",
        R"([{"path":"x"}])",
        R"([{"path":"x","storage":"memory-backed","extra":0}])"}) {
    auto J = test::registryHivesScenario();
    auto V = llvm::json::parse(Bad);
    ASSERT_TRUE(bool(V));
    J["registry_hives"] = std::move(*V);
    auto Parsed = driverOptionsFromScenarioJSON(
        llvm::formatv("{0}", llvm::json::Value(std::move(J))).str());
    ASSERT_FALSE(bool(Parsed));
    llvm::consumeError(Parsed.takeError());
  }
}
TEST(RegistryHivesScenario, TypedInvalidHivesFailDuringModelInitialization) {
  auto B = UnicornBackend::create(4 * 1024 * 1024);
  ASSERT_TRUE(bool(B));
  DriverResult R;
  KernelModel K(**B, R);
  DriverImage I;
  I.Base = 0x180000000;
  I.Entry = I.Base + 4096;
  I.Size = 0x3000;
  auto O = test::registryHivesOptions();
  O.Version.reset();
  auto E = K.initialize(I, O);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("registry_hives"),
            std::string::npos);
}
uint8_t data(const std::vector<DriverRegistryKey> &Keys, llvm::StringRef Name) {
  for (const auto &K : Keys)
    if (llvm::StringRef(K.Path).ends_with(Name))
      return K.Values.at(0).Data.at(0);
  ADD_FAILURE() << Name.str();
  return 255;
}
void verify(const DriverResult &R) {
  EXPECT_EQ(R.Stop, DriverStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.NTStatus, 0u) << R.Diagnostic;
  EXPECT_TRUE(R.UnloadCompleted) << R.Diagnostic;
  EXPECT_EQ(R.Messages,
            (std::vector<std::string>{"registry hive commit passed",
                                      "registry hive unload passed"}));
  ASSERT_TRUE(R.RegistryHives);
  ASSERT_EQ(R.RegistryHives->size(), 2u);
  const auto &A = R.RegistryHives->at(0), &B = R.RegistryHives->at(1);
  EXPECT_EQ(A.FlushCount, 1u);
  EXPECT_EQ(B.FlushCount, 0u);
  EXPECT_EQ(data(A.Committed, "\\A"), 10u);
  EXPECT_EQ(data(A.Committed, "\\B"), 20u);
  EXPECT_EQ(data(B.Committed, "\\C"), 3u);
  ASSERT_TRUE(R.Registry);
  EXPECT_EQ(data(*R.Registry, "\\B"), 99u);
  EXPECT_EQ(data(*R.Registry, "\\C"), 30u);
}
TEST(RegistryHivesDriver, CompiledDriverRetainsUnflushedChangesAcrossModes) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Paging : {false, true}) {
        auto O = test::registryHivesOptions();
        O.Contract = Contract;
        O.PageTables = Paging;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_registry_hives.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        verify(*R);
      }
}
TEST(RegistryHivesDriver,
     MatchedReferenceCallsNativeExportWithSharedPersistence) {
  const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
             *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = test::registryHivesOptions();
      O.Contract = Contract;
      O.KernelImage = Image;
      O.KernelPDB = PDB;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_registry_hives.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      verify(*R);
      bool Found = false;
      for (const auto &Call : R->Calls)
        if (Call.Name == "ZwFlushKey") {
          EXPECT_GE(Call.PC, 0xfffff80000000000ULL);
          Found = true;
        }
      EXPECT_TRUE(Found);
    }
}
} // namespace
} // namespace neverd::emulation
