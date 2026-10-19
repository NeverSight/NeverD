//===- KernelNlsDataTests.cpp - Explicit one-byte NLS observation
//----------===//
///
/// \file
/// Explicit one-byte NLS observation.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelNlsData.h"
#include "os/windows/kernel/KernelReferenceImage.h"

#include "llvm/Support/JSON.h"

#include <cstdlib>
#include <filesystem>
#include <tuple>

namespace neverd::emulation {
namespace {
TEST(DriverNlsScenario, PreservesUnknownFalseAndTrueAcrossPublicSchema) {
  auto Unknown = llvm::cantFail(driverOptionsFromScenarioJSON("{}"));
  EXPECT_FALSE(Unknown.NlsOemMultibyte);
  for (bool Multibyte : {false, true}) {
    const std::string Input = Multibyte ? "{\"nls_oem_multibyte\":true}"
                                        : "{\"nls_oem_multibyte\":false}";
    auto O = llvm::cantFail(driverOptionsFromScenarioJSON(Input));
    EXPECT_EQ(O.NlsOemMultibyte, Multibyte);
    auto Inherited = llvm::cantFail(driverOptionsFromScenarioJSON("{}", O));
    EXPECT_EQ(Inherited.NlsOemMultibyte, Multibyte);
    DriverResult R;
    R.Configuration = O;
    auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(R)));
    EXPECT_EQ(JSON.getAsObject()
                  ->getObject("configuration")
                  ->getBoolean("nls_oem_multibyte"),
              Multibyte);
  }
  for (const char *Bad : {"null", "0", "1", "\"false\"", "[]", "{}"}) {
    auto O = driverOptionsFromScenarioJSON(
        std::string("{\"nls_oem_multibyte\":") + Bad + "}");
    ASSERT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
}
TEST(KernelNlsData, ReferenceBindingPreservesTypedIdentityAtomically) {
  KernelExportRegistry Registry;
  DriverOptions O;
  O.KernelExports[nls::OemMultibyteExport.str()] = true;
  llvm::cantFail(Registry.initialize(O));
  const auto Original =
      llvm::cantFail(Registry.resolve(nls::OemMultibyteExport));
  EXPECT_EQ(Original, nls::OemMultibyteSlot);
  EXPECT_EQ(Registry.lookup(Original)->Kind,
            KernelExportRegistry::ExportKind::NlsData);
  EXPECT_FALSE(KernelModel::argumentCount(*Registry.lookup(Original)));
  constexpr uint64_t Native = 0xfffff80000008004;
  for (unsigned Case = 0; Case < 6; ++Case) {
    auto Inventory =
        std::map<std::string, uint64_t>{{nls::OemMultibyteExport.str(), 0}};
    auto Data = std::map<std::string, uint64_t>{
        {nls::OemMultibyteExport.str(), Native}};
    std::map<std::string, uint64_t> Process, Processor;
    if (Case == 0)
      Data.begin()->second = 0;
    if (Case == 1)
      Data.clear();
    if (Case == 2)
      Inventory.begin()->second = 0xfffff80000001000;
    if (Case == 3)
      Data = {{"OtherNlsData", Native}};
    if (Case == 4) {
      Inventory.emplace("PsInitialSystemProcess", 0);
      Process.emplace("PsInitialSystemProcess", Native - 4);
    }
    if (Case == 5) {
      Inventory.emplace("KeNumberProcessors", 0);
      Processor.emplace("KeNumberProcessors", Native);
    }
    auto E = Registry.attachReferenceImage(Inventory, {}, {}, Process,
                                           Processor, Data);
    ASSERT_TRUE(bool(E)) << Case;
    llvm::consumeError(std::move(E));
    EXPECT_EQ(llvm::cantFail(Registry.resolve(nls::OemMultibyteExport)),
              Original);
  }
  llvm::cantFail(Registry.attachReferenceImage(
      {{nls::OemMultibyteExport.str(), 0}}, {}, {}, {}, {},
      {{nls::OemMultibyteExport.str(), Native}}));
  EXPECT_EQ(llvm::cantFail(Registry.bindImport(
                {0x1000, "ntkrnlmp.exe", nls::OemMultibyteExport.str()})),
            Native);
  EXPECT_EQ(Registry.lookup(Native)->Kind,
            KernelExportRegistry::ExportKind::NlsData);
}
TEST(KernelNlsData, OnlyObservedByteIsReadableAndNeverCallable) {
  for (std::optional<bool> Value :
       {std::optional<bool>{}, std::optional(false), std::optional(true)}) {
    KernelExportRegistry Registry;
    llvm::cantFail(Registry.initialize({}));
    auto Memory = llvm::cantFail(UnicornBackend::create(4 * 1024 * 1024));
    DriverResult Result;
    KernelModel Model(*Memory, Result, &Registry);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    DriverOptions O;
    O.NlsOemMultibyte = Value;
    llvm::cantFail(Model.initialize(I, O));
    auto Read = Model.validateGuestAccess(nls::OemMultibyteSlot, 1, false);
    if (Value) {
      ASSERT_FALSE(bool(Read)) << llvm::toString(std::move(Read));
      EXPECT_EQ(llvm::cantFail(Memory->readInteger(nls::OemMultibyteSlot, 1)),
                uint64_t(*Value));
    } else {
      ASSERT_TRUE(bool(Read));
      EXPECT_NE(llvm::toString(std::move(Read)).find("nls_oem_multibyte"),
                std::string::npos);
    }
    for (auto [At, Size, Write] :
         {std::tuple<uint64_t, uint32_t, bool>{nls::OemMultibyteSlot, 1, true},
          {nls::OemMultibyteSlot, 2, false},
          {nls::OemMultibyteSlot - 1, 2, false},
          {nls::OemMultibyteSlot + 1, 1, false}}) {
      auto E = Model.validateGuestAccess(At, Size, Write);
      ASSERT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
    }
    auto Call =
        Model.call(*Registry.lookup(nls::OemMultibyteSlot), {}, nullptr);
    ASSERT_FALSE(bool(Call));
    EXPECT_NE(llvm::toString(Call.takeError()).find("not callable"),
              std::string::npos);
  }
}
TEST(DriverNlsData, CompiledImportLookupAndOpaqueBoundariesAcrossReferences) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (unsigned Reference : {0u, 1u, 2u})
        for (bool Value : {false, true})
          for (const char *Name :
               {"driver_nls_oem", "driver_nls_oem_write",
                "driver_nls_oem_cross", "driver_nls_oem_execute"}) {
            SCOPED_TRACE(std::to_string(unsigned(Contract)) + ":" +
                         std::to_string(Preemptive) + ":" +
                         std::to_string(Reference) + ":" + Name);
            const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
                       *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
            if ((Reference && !Image) || (Reference == 2 && !PDB))
              continue;
            DriverOptions O;
            O.Contract = Contract;
            O.Unload = true;
            O.NlsOemMultibyte = Value;
            O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
            O.PageTables = true;
            if (Preemptive)
              O.Scheduling = DriverScheduling{23, 1};
            if (Reference)
              O.KernelImage = Image;
            if (Reference == 2)
              O.KernelPDB = PDB;
            auto R =
                emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                  (std::string(Name) + ".sys"),
                              O);
            ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
            if (std::string_view(Name) == "driver_nls_oem") {
              EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
              EXPECT_EQ(R->NTStatus, 0u);
              EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
              ASSERT_EQ(R->Messages.size(), 1u);
              EXPECT_EQ(R->Messages[0],
                        Value ? "OEM multibyte" : "OEM single byte");
            } else {
              EXPECT_NE(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
              EXPECT_FALSE(R->UnloadCompleted);
            }
          }
}
TEST(DriverNlsData, AbsentObservationCannotUseReferenceImageInitializer) {
  for (bool Reference : {false, true}) {
    const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE");
    if (Reference && !Image)
      continue;
    DriverOptions O;
    if (Reference)
      O.KernelImage = Image;
    auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_nls_oem.sys",
                           O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, DriverStopReason::ModelError) << R->Diagnostic;
    EXPECT_NE(R->Diagnostic.find("nls_oem_multibyte"), std::string::npos);
    EXPECT_FALSE(R->NTStatus);
  }
}
} // namespace
} // namespace neverd::emulation
