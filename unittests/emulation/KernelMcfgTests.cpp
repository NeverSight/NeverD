//===- KernelMcfgTests.cpp - Explicit guest firmware ownership
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit guest firmware ownership.
///

#include "McfgTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelReferenceImage.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
std::filesystem::path mcfgFixture(const char *Name,
                                  const char *Suffix = ".sys") {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + Suffix);
}
DriverOptions mcfgOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.KernelImage = mcfgFixture("driver_kernel_debugger_mcfg");
  O.KernelPDB = mcfgFixture("driver_kernel_debugger_mcfg", ".pdb");
  for (const char *Name : {"RtlInitUnicodeString", "ZwOpenKey",
                           "ZwQueryValueKey", "ZwClose", "DbgPrint"})
    O.KernelExports[Name] = true;
  return O;
}
std::string mcfgScenario(const DriverPciMcfg &F) {
  if (!F.Present)
    return R"({"pci_mcfg":{"present":false}})";
  return "{\"pci_mcfg\":{\"present\":true,\"table_hex\":\"" +
         llvm::toHex(F.Table, true) + "\"}}";
}

TEST(DriverMcfg, ValidatesAbsentEmptyAndMultipleAllocationTables) {
  auto Absent = validateDriverPciMcfg({});
  ASSERT_TRUE(bool(Absent));
  EXPECT_EQ(*Absent, 0u);
  for (unsigned N : {0, 1, 2, 256}) {
    auto Count = validateDriverPciMcfg(mcgffixture::table(N));
    ASSERT_TRUE(bool(Count)) << llvm::toString(Count.takeError());
    EXPECT_EQ(*Count, N);
  }
}

TEST(DriverMcfg, SplitRangesRetainBusZeroBaseAndAllowNonoverlappingBuses) {
  auto F = mcgffixture::table();
  mcgffixture::put(F.Table, 44 + 16, 0x80000000, 8);
  mcgffixture::put(F.Table, 44 + 16 + 8, 0, 2);
  F.Table[44 + 10] = 0;
  F.Table[44 + 11] = 31;
  mcgffixture::checksum(F.Table);
  auto R = validateDriverPciMcfg(F);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(*R, 2u);
}

TEST(DriverMcfg, IncompleteAuthenticatedSymbolPairFailsClearly) {
  auto R = KernelReferenceImage::load(
      mcfgFixture("driver_kernel_debugger_mcfg_partial"), 16 * 1024 * 1024,
      mcfgFixture("driver_kernel_debugger_mcfg_partial", ".pdb"));
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("HalpPciMcfgTable"),
            std::string::npos);
}

TEST(DriverMcfg, RejectsMalformedOrConflictingFirmwareBeforePublication) {
  std::vector<DriverPciMcfg> Bad;
  auto Add = [&](auto Mutate) {
    auto F = mcgffixture::table();
    Mutate(F);
    mcgffixture::checksum(F.Table);
    Bad.push_back(std::move(F));
  };
  Add([](auto &F) { F.Present = false; });
  Add([](auto &F) { F.Table[0] ^= 1; });
  Add([](auto &F) { F.Table[4] ^= 1; });
  Add([](auto &F) { F.Table[8] = 2; });
  Add([](auto &F) { F.Table[36] = 1; });
  Add([](auto &F) { F.Table[44 + 12] = 1; });
  Add([](auto &F) { F.Table[44 + 10] = 64; });
  Add([](auto &F) { mcgffixture::put(F.Table, 44, 0, 8); });
  Add([](auto &F) { mcgffixture::put(F.Table, 44, 0x80000001, 8); });
  Add([](auto &F) { mcgffixture::put(F.Table, 44, 0xfffffffffff00000ULL, 8); });
  Add([](auto &F) { mcgffixture::put(F.Table, 44 + 16 + 8, 0, 2); });
  Add([](auto &F) { mcgffixture::put(F.Table, 44 + 16, 0x80000000, 8); });
  Bad.push_back(mcgffixture::table(257));
  auto Truncated = mcgffixture::table();
  Truncated.Table.resize(43);
  Bad.push_back(Truncated);
  auto Checksum = mcgffixture::table();
  Checksum.Table[9] ^= 1;
  Bad.push_back(Checksum);
  for (const auto &F : Bad) {
    auto R = validateDriverPciMcfg(F);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("pci_mcfg"),
              std::string::npos);
  }
}

TEST(DriverMcfgScenario, StrictTypesAndFieldCombinationsHaveNoDefaults) {
  for (const char *Value :
       {"null", "false", "[]", "{}", R"({"present":0})", R"({"present":true})",
        R"({"present":true,"table_hex":""})",
        R"({"present":true,"table_hex":"xyz"})",
        R"({"present":false,"table_hex":""})",
        R"({"present":false,"unknown":0})"}) {
    auto O = driverOptionsFromScenarioJSON(std::string("{\"pci_mcfg\":") +
                                           Value + "}");
    ASSERT_FALSE(bool(O)) << Value;
    EXPECT_NE(llvm::toString(O.takeError()).find("pci_mcfg"),
              std::string::npos);
  }
  auto Empty = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(bool(Empty));
  EXPECT_FALSE(Empty->PciMcfg.has_value());
  DriverOptions Invalid;
  Invalid.PciMcfg = mcgffixture::table();
  Invalid.PciMcfg->Table[9] ^= 1;
  auto O = driverOptionsFromScenarioJSON("{}", Invalid);
  ASSERT_FALSE(bool(O));
  llvm::consumeError(O.takeError());
}

TEST(DriverMcfgScenario, RawBytesAndExplicitAbsenceRoundTripAndPreserveBase) {
  for (const auto &F :
       {DriverPciMcfg{}, mcgffixture::table(0), mcgffixture::table()}) {
    auto O = driverOptionsFromScenarioJSON(mcfgScenario(F));
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    auto P = driverOptionsFromScenarioJSON("{}", *O);
    ASSERT_TRUE(bool(P));
    ASSERT_TRUE(P->PciMcfg);
    EXPECT_EQ(P->PciMcfg->Present, F.Present);
    EXPECT_EQ(P->PciMcfg->Table, F.Table);
    DriverResult R;
    R.Configuration = *P;
    auto JSON = llvm::json::parse(driverResultJSON(R));
    ASSERT_TRUE(bool(JSON));
    const auto *M =
        JSON->getAsObject()->getObject("configuration")->getObject("pci_mcfg");
    ASSERT_NE(M, nullptr);
    EXPECT_EQ(M->getBoolean("present"), F.Present);
    if (F.Present)
      EXPECT_EQ(M->getString("table_hex"), llvm::toHex(F.Table, true));
    else
      EXPECT_EQ(M->get("table_hex"), nullptr);
  }
}

class KernelMcfg : public testing::Test {
protected:
  static constexpr uint64_t S = 0x26000000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelReferenceImage> Reference;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  llvm::Error initialize(DriverOptions O, bool LoadReference = true) {
    Model.reset();
    Reference.reset();
    auto B = UnicornBackend::create(16 * 1024 * 1024);
    if (!B)
      return B.takeError();
    Memory = std::move(*B);
    if (auto E = Memory->map(S, 0x10000, Read | Write))
      return E;
    if (LoadReference) {
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
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    return llvm::Error::success();
  }
  uint64_t get(uint64_t A, unsigned W = 8) {
    auto R = Memory->readInteger(A, W);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  void put(uint64_t A, uint64_t V, unsigned W = 8) {
    ok(Memory->writeInteger(A, V, W));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A) {
    auto R = Model->call(Name, A);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
};

TEST_F(KernelMcfg, MissingObservationDoesNotExposeFileInitializers) {
  auto E = initialize(mcfgOptions());
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  const auto &F = *Reference->debugger();
  ASSERT_NE(F.PciMcfgCount, 0u);
  ASSERT_NE(F.PciMcfgPointer, 0u);
  for (auto [A, N] : {std::pair{F.PciMcfgCount, 4u}, {F.PciMcfgPointer, 8u}}) {
    E = Model->validateGuestAccess(A, N, false);
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find("require pci_mcfg"),
              std::string::npos);
  }
}

TEST_F(KernelMcfg, ExplicitAbsencePublishesOnlyTwoReadOnlyFields) {
  auto O = mcfgOptions();
  O.PciMcfg.emplace();
  auto E = initialize(O);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  const auto &F = *Reference->debugger();
  ok(Model->validateGuestAccess(F.PciMcfgCount, 4, false));
  ok(Model->validateGuestAccess(F.PciMcfgPointer, 8, false));
  EXPECT_EQ(get(F.PciMcfgCount, 4), 0u);
  EXPECT_EQ(get(F.PciMcfgPointer), 0u);
  for (auto [A, N] :
       {std::pair{F.PciMcfgCount, 5u}, {F.PciMcfgPointer - 1, 8u}}) {
    E = Model->validateGuestAccess(A, N, false);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  E = Model->validateGuestAccess(F.PciMcfgCount, 4, true);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  auto Mapped = Memory->canAccess(windows::FirmwareMcfgBase, 1, Read);
  ASSERT_TRUE(bool(Mapped));
  EXPECT_FALSE(*Mapped);
}

TEST_F(KernelMcfg, PresentTablesRetainExactBytesCountAndCopiedOwnership) {
  for (unsigned N : {0, 2, 256}) {
    auto O = mcfgOptions();
    O.PciMcfg = mcgffixture::table(N);
    const auto Bytes = O.PciMcfg->Table;
    auto E = initialize(O);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    O.PciMcfg->Table[0] = 0;
    const auto &F = *Reference->debugger();
    EXPECT_EQ(get(F.PciMcfgCount, 4), N);
    const auto P = get(F.PciMcfgPointer);
    EXPECT_EQ(P, windows::FirmwareMcfgBase);
    std::vector<uint8_t> Observed(Bytes.size());
    ok(Model->validateGuestAccess(P, Bytes.size(), false));
    ok(Memory->read(P, Observed));
    EXPECT_EQ(Observed, Bytes);
    auto Writable = Memory->canAccess(P, Bytes.size(), Write);
    auto Executable = Memory->canAccess(P, Bytes.size(), Execute);
    ASSERT_TRUE(bool(Writable));
    ASSERT_TRUE(bool(Executable));
    EXPECT_FALSE(*Writable);
    EXPECT_FALSE(*Executable);
    E = Model->validateGuestAccess(P + Bytes.size() - 1, 2, false);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    E = Model->validateGuestAccess(P, 1, true);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    put(S, 0x1122334455667788ULL);
    auto Copy = Model->call("RtlCopyMemory", {S, P + Bytes.size() - 1, 8});
    ASSERT_FALSE(bool(Copy));
    llvm::consumeError(Copy.takeError());
    EXPECT_EQ(get(S), 0x1122334455667788ULL);
  }
}

TEST_F(KernelMcfg, InvalidFactsAndUnknownProfilesFailBeforeKernelObjects) {
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    auto O = mcfgOptions();
    O.PciMcfg = mcgffixture::table();
    if (Mode == 0)
      O.PciMcfg->Table[9] ^= 1;
    if (Mode == 1)
      O.Version.reset();
    if (Mode == 2)
      O.Version->Build = 22621;
    if (Mode == 3) {
      O.KernelImage = mcfgFixture("driver_kernel_debugger_reference");
      O.KernelPDB = mcfgFixture("driver_kernel_debugger_reference", ".pdb");
    }
    auto E = initialize(O, Mode != 4);
    ASSERT_TRUE(bool(E)) << Mode;
    EXPECT_NE(llvm::toString(std::move(E)).find("pci_mcfg"), std::string::npos);
    EXPECT_EQ(Model->driverObject(), 0u);
  }
}

TEST_F(KernelMcfg, PhysicalAliasesPreserveFieldAndTableOpacity) {
  auto O = mcfgOptions();
  O.PciMcfg = mcgffixture::table();
  O.PageTables = true;
  auto E = initialize(O);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  ok(Model->synchronizeGuestPageTables());
  const char *Name = "\\Device\\PhysicalMemory";
  for (unsigned I = 0; I <= 22; ++I)
    put(S + 0x200 + I * 2, Name[I], 2);
  put(S + 0x100, 44, 2);
  put(S + 0x102, 46, 2);
  put(S + 0x108, S + 0x200);
  put(S + 0x300, 48, 4);
  put(S + 0x310, S + 0x100);
  put(S + 0x318, 0x240, 4);
  ASSERT_EQ(call("ZwOpenSection", {S, 0xf001f, S + 0x300}), 0u);
  const auto H = get(S);
  const auto &F = *Reference->debugger();
  for (auto [A, N] :
       {std::pair{windows::FirmwareMcfgBase, uint64_t(O.PciMcfg->Table.size())},
        {F.PciMcfgCount, uint64_t(4)},
        {F.PciMcfgPointer, uint64_t(8)}}) {
    const auto PA = call("MmGetPhysicalAddress", {A});
    put(S + 0x400, 0);
    put(S + 0x408, 4096);
    put(S + 0x410, PA & ~uint64_t(4095));
    ASSERT_EQ(call("ZwMapViewOfSection", {H, UINT64_MAX, S + 0x400, 0, 0,
                                          S + 0x410, S + 0x408, 2, 0, 2}),
              0u);
    const auto V = get(S + 0x400), Alias = V + (PA & 4095);
    ok(Model->validateGuestAccess(Alias, N, false));
    EXPECT_EQ(get(Alias, 4), get(A, 4));
    auto Writable = Memory->canAccess(Alias, 1, Write);
    ASSERT_TRUE(bool(Writable));
    EXPECT_FALSE(*Writable);
    E = Model->validateGuestAccess(Alias + N - 1, 2, false);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, V}), 0u);
    if (A != windows::FirmwareMcfgBase) {
      put(S + 0x400, 0);
      put(S + 0x408, 4096);
      put(S + 0x410, PA & ~uint64_t(4095));
      ASSERT_EQ(call("ZwMapViewOfSection", {H, UINT64_MAX, S + 0x400, 0, 0,
                                            S + 0x410, S + 0x408, 2, 0, 4}),
                0u);
      const auto WriteView = get(S + 0x400);
      E = Model->validateGuestAccess(WriteView + (PA & 4095), N, true);
      ASSERT_TRUE(bool(E));
      EXPECT_NE(llvm::toString(std::move(E)).find("MCFG"), std::string::npos);
      EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, WriteView}), 0u);
    }
  }
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  auto ECAM = Memory->canAccess(0x82000000, 1, Read);
  ASSERT_TRUE(bool(ECAM));
  EXPECT_FALSE(*ECAM);
}

TEST(KernelMcfgDriver, RealClientReadsBothPresentAndAbsentFirmware) {
  auto O = mcfgOptions();
  auto Ref =
      KernelReferenceImage::load(O.KernelImage, O.MemoryLimit, O.KernelPDB);
  ASSERT_TRUE(bool(Ref)) << llvm::toString(Ref.takeError());
  const auto &F = *Ref->debugger();
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (const auto &Input :
           {DriverPciMcfg{}, mcgffixture::table(0), mcgffixture::table()}) {
        O.Contract = Contract;
        O.PciMcfg = Input;
        O.Unload = true;
        O.Scheduling = Preemptive ? std::optional<DriverScheduling>{{23, 1}}
                                  : std::nullopt;
        const auto Count = Input.Present ? (Input.Table.size() - 44) / 16 : 0;
        O.Registry = std::vector{mcgffixture::parameters(
            F.PciMcfgCount, F.PciMcfgPointer, Count, Input.Present)};
        auto R = emulateDriver(mcfgFixture("driver_mcfg"), O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u);
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        EXPECT_EQ(R->Messages, (std::vector<std::string>{
                                   Input.Present ? "mcfg-present-pass"
                                                 : "mcfg-absent-pass"}));
      }
}

TEST(KernelMcfgDriver, RealClientCannotWriteFieldsTableOrReadPadding) {
  auto O = mcfgOptions();
  auto Ref =
      KernelReferenceImage::load(O.KernelImage, O.MemoryLimit, O.KernelPDB);
  ASSERT_TRUE(bool(Ref)) << llvm::toString(Ref.takeError());
  const auto &F = *Ref->debugger();
  O.PciMcfg = mcgffixture::table();
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (unsigned Mode = 1; Mode != 5; ++Mode) {
      O.Contract = Contract;
      O.Registry = std::vector{mcgffixture::parameters(
          F.PciMcfgCount, F.PciMcfgPointer, 2, true, Mode)};
      auto R = emulateDriver(mcfgFixture("driver_mcfg"), O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_NE(R->Stop, DriverStopReason::Returned);
      EXPECT_FALSE(R->NTStatus);
      EXPECT_TRUE(R->Messages.empty());
    }
}
} // namespace
} // namespace neverd::emulation
