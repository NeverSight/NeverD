///
/// \file
/// KernelHalDispatchTests.
///

#include "HalDispatchTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelHalDispatch.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelReferenceImage.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/FormatVariadic.h"

#include <cstdlib>
namespace neverd::emulation {
namespace {
class KernelHalDispatch : public testing::Test {
protected:
  static constexpr uint64_t S = 0x22000000, PC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  KernelExportRegistry Exports;
  DriverOptions Options;
  DriverResult Result;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T value(llvm::Expected<T> V) {
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return {};
    }
    return std::move(*V);
  }
  template <class T> void bad(llvm::Expected<T> V, llvm::StringRef Text) {
    ASSERT_FALSE(bool(V));
    auto M = llvm::toString(V.takeError());
    EXPECT_NE(M.find(Text.str()), std::string::npos) << M;
  }
  void bad(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    auto M = llvm::toString(std::move(E));
    EXPECT_NE(M.find(Text.str()), std::string::npos) << M;
  }
  void initialize(DriverOptions O) {
    Model.reset();
    Exports = {};
    Result = {};
    Options = std::move(O);
    ok(Exports.initialize(Options));
    Memory = value(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(S, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage I;
    I.Base = PC - 4096;
    I.Entry = PC;
    I.Size = 0x3000;
    ok(Model->initialize(I, Options));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void SetUp() override { initialize(test::halDispatchOptions()); }
  uint64_t get(uint64_t A, unsigned W = 8) {
    ok(Model->validateGuestAccess(A, W, false));
    return value(Memory->readInteger(A, W));
  }
  uint64_t table() { return value(Exports.resolve("HalDispatchTable")); }
  const KernelExportRegistry::Export &function() {
    return *Exports.lookup(get(table() + 0xa8));
  }
  llvm::Expected<uint64_t> call(const KernelExportRegistry::Export &E,
                                llvm::ArrayRef<uint64_t> A) {
    return Model->call(E, A, [](unsigned) -> llvm::Expected<uint64_t> {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "extra argument");
    });
  }
  uint64_t query(uint64_t Signature, uint64_t OEM = 0, uint64_t ID = 0) {
    return value(call(function(), {Signature, OEM, ID}));
  }
  void bytes(uint64_t A, llvm::StringRef B) {
    ok(Memory->write(
        A,
        llvm::ArrayRef(reinterpret_cast<const uint8_t *>(B.data()), B.size())));
  }
  uint64_t nt(const char *Name, std::initializer_list<uint64_t> A) {
    return value(Model->call(Name, A));
  }
};
TEST_F(KernelHalDispatch, TypedTablePublishesOnlyKnownVersionAndCacheSlot) {
  EXPECT_EQ(table(), hal_dispatch::Slot);
  EXPECT_EQ(get(table(), 4), 4u);
  auto E = Exports.lookup(table());
  ASSERT_NE(E, nullptr);
  EXPECT_EQ(E->Kind, KernelExportRegistry::ExportKind::HalDispatchData);
  EXPECT_FALSE(KernelModel::argumentCount(*E));
  bad(call(*E, {}), "not callable");
  bad(Model->validateGuestAccess(table(), 8, false), "opaque");
  bad(Model->validateGuestAccess(table() + 8, 8, false), "opaque");
  bad(Model->validateGuestAccess(table() + 0xa8, 8, true), "read-only");
  EXPECT_EQ(function().Kind,
            KernelExportRegistry::ExportKind::HalDispatchFunction);
  EXPECT_EQ(KernelModel::argumentCount(function()), 3u);
  bad(Exports.resolve("HalGetCachedAcpiTable"), "unspecified");
  bad(Model->call("HalGetCachedAcpiTable", {0, 0, 0}), "unknown API");
  bad(call(function(), {0, 0}), "exact arity");
  auto Forged = function();
  Forged.Binding += 8;
  bad(call(Forged, {0, 0, 0}), "live bound");
}
TEST_F(KernelHalDispatch,
       CachedPointersMatchFirmwareQueriesAndHaveSessionLifetime) {
  const auto First = query(0x54445353), Second = query(0x43495041);
  EXPECT_NE(First, 0u);
  EXPECT_NE(First, Second);
  EXPECT_EQ(query(0x100000000ULL | 0x54445353), First);
  EXPECT_EQ(query(0x54445358), 0u);
  EXPECT_EQ(query(0x54445352), 0u);
  std::vector<uint8_t> B(Options.AcpiTables->Tables[1].size());
  ok(Memory->read(First, B));
  EXPECT_EQ(B, Options.AcpiTables->Tables[1]);
  ok(Memory->writeInteger(S, 0x41435049, 4));
  ok(Memory->writeInteger(S + 4, 1, 4));
  ok(Memory->writeInteger(S + 8, 0x54445353, 4));
  EXPECT_EQ(nt("ZwQuerySystemInformation", {76, S, 4096, 0}), 0u);
  std::vector<uint8_t> Out(B.size());
  ok(Memory->read(S + 16, Out));
  EXPECT_EQ(Out, B);
  bad(Model->validateGuestAccess(First, 1, true), "read-only ACPI");
  bad(Model->call("ExFreePool", {First}), "unknown or already freed");
  nt("KfRaiseIrql", {2});
  EXPECT_EQ(get(First, 4), 0x54445353u);
  bad(call(function(), {0x54445353, 0, 0}), "IRQL");
  nt("KeLowerIrql", {0});
  nt("KfRaiseIrql", {1});
  EXPECT_EQ(query(0x54445353), First);
  nt("KeLowerIrql", {0});
  bad(call(function(), {0x53434146, 0, 0}), "FACS");
}
TEST_F(KernelHalDispatch,
       NativeOrderedOemFiltersUseCaseSensitiveBoundedStrncmp) {
  const auto First = query(0x54445353);
  bytes(S, "OTHER ");
  bytes(S + 32, "ALT     ");
  const auto Second = query(0x54445353, S, S + 32);
  EXPECT_NE(First, Second);
  EXPECT_EQ(get(Second + 24, 4), 2u);
  bytes(S, "neverd");
  EXPECT_EQ(query(0x54445353, S), 0u);
  // A missing signature never dereferences even invalid optional filters.
  EXPECT_EQ(query(0x54445358, 1, 1), 0u);
  auto O = Options;
  auto T = acpifixture::table("SSDT", {0xa3}, 3);
  std::copy_n("A\0xxxx", 6, T.begin() + 10);
  std::copy_n("A\0xxxxxx", 8, T.begin() + 16);
  acpifixture::checksum(T);
  O.AcpiTables->Tables.push_back(T);
  initialize(O);
  bytes(S, llvm::StringRef("A\0yyyy", 6));
  bytes(S + 32, llvm::StringRef("A\0yyyyyy", 8));
  EXPECT_EQ(get(query(0x54445353, S, S + 32) + 24, 4), 3u);
}
TEST_F(KernelHalDispatch, EveryAdmittedOriginalCacheSelectionMatchesNative) {
  auto O = Options;
  O.AcpiTables->Tables.clear();
  for (unsigned I = 0; I < 4; ++I) {
    auto T = acpifixture::table(I == 2 ? "APIC" : "SSDT", {0xa3}, I + 1);
    if (I == 1) {
      std::copy_n("OTHER ", 6, T.begin() + 10);
      std::copy_n("ALT     ", 8, T.begin() + 16);
    }
    if (I == 3) {
      std::copy_n("A\0xxxx", 6, T.begin() + 10);
      std::copy_n("A\0xxxxxx", 8, T.begin() + 16);
    }
    acpifixture::checksum(T);
    O.AcpiTables->Tables.push_back(T);
  }
  initialize(O);
  struct Case {
    uint32_t Signature;
    const char *OEM, *ID;
    int Index;
  };
  const Case Cases[] = {
#define NEVERD_HAL_ACPI_NATIVE_CASE(S, O, T, I) {S, O, T, I},
#include "HalAcpiNativeCases.def"
#undef NEVERD_HAL_ACPI_NATIVE_CASE
  };
  ASSERT_EQ(std::size(Cases), 60u);
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.OEM);
    SCOPED_TRACE(C.ID);
    SCOPED_TRACE(C.Signature);
    const auto OEM = llvm::fromHex(C.OEM), ID = llvm::fromHex(C.ID);
    if (!OEM.empty())
      bytes(S, OEM);
    if (!ID.empty())
      bytes(S + 32, ID);
    const auto P =
        query(C.Signature, OEM.empty() ? 0 : S, ID.empty() ? 0 : S + 32);
    if (C.Index < 0)
      EXPECT_EQ(P, 0u);
    else {
      ASSERT_NE(P, 0u);
      EXPECT_EQ(get(P + 24, 4), unsigned(C.Index + 1));
    }
  }
}
TEST_F(KernelHalDispatch, MissingFirmwareAndVersionFactsRemainUnknown) {
  auto O = Options;
  O.AcpiTables.reset();
  initialize(O);
  EXPECT_EQ(get(table(), 4), 4u);
  bad(Model->validateGuestAccess(table() + 0xa8, 8, false),
      "explicit acpi_tables");
  O = Options;
  O.Version.reset();
  initialize(O);
  bad(Model->validateGuestAccess(table(), 4, false), "windows_version");
  O = test::halDispatchOptions();
  O.AcpiTables->Tables.clear();
  initialize(O);
  EXPECT_EQ(query(0x43495041), 0u);
  O = test::halDispatchOptions();
  O.KernelExports["HalDispatchTable"] = false;
  initialize(O);
  EXPECT_EQ(table(), 0u);
}
TEST(HalDispatchRegistry,
     MalformedTypedReferencesDoNotPublishPartialNamespaces) {
  for (auto Address : {0ULL, 0x90000001ULL, 0xfffffffffffffff8ULL}) {
    KernelExportRegistry E;
    auto Error = E.initialize({});
    ASSERT_FALSE(bool(Error));
    auto Before = E.resolve("HalDispatchTable");
    ASSERT_TRUE(bool(Before));
    Error = E.attachReferenceImage({{"HalDispatchTable", 0}}, {}, {}, {}, {},
                                   {}, {{"HalDispatchTable", Address}});
    ASSERT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    auto After = E.resolve("HalDispatchTable");
    ASSERT_TRUE(bool(After));
    EXPECT_EQ(*Before, *After);
  }
  KernelExportRegistry E;
  auto Error = E.initialize({});
  ASSERT_FALSE(bool(Error));
  Error = E.attachReferenceImage(
      {{"HalDispatchTable", 0}, {"NlsMbOemCodePageTag", 0}}, {}, {}, {}, {},
      {{"NlsMbOemCodePageTag", 0x900000a8}},
      {{"HalDispatchTable", 0x90000000}});
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}
void checkHalDriver(DriverOptions O, bool Reference) {
  auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                             "driver_hal_dispatch.sys",
                         O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
  EXPECT_EQ(R->NTStatus, 0u);
  EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
  EXPECT_EQ(R->Messages,
            (std::vector<std::string>{"hal cached acpi pointer passed",
                                      "hal dispatch unload passed"}));
  unsigned Calls = 0;
  bool Mcfg = false;
  for (const auto &C : R->Calls)
    if (C.Name == "HalGetCachedAcpiTable") {
      ++Calls;
      if (Reference)
        EXPECT_EQ(C.PC, 0xfffff800002e77b0ULL);
      else
        EXPECT_GE(C.PC, profile::ThunkBase);
      if (C.Arguments[0] == 0x4746434d) {
        Mcfg = true;
        if (Reference)
          EXPECT_EQ(C.Result, windows::FirmwareMcfgBase);
      }
    }
  EXPECT_TRUE(Mcfg);
  EXPECT_EQ(Calls, 8u);
}
TEST(HalDispatchDriver,
     CompiledPointerClientUsesTheTypedTableAndFirmwareOwner) {
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Paging : {false, true}) {
        auto O = test::halDispatchOptions();
        O.Contract = C;
        O.PageTables = Paging;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        checkHalDriver(O, false);
      }
}
TEST(HalDispatchDriver,
     AuthenticatedBootTargetIsTheActualPrivateKernelAddress) {
  auto *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
       *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = test::halDispatchOptions();
      O.Contract = C;
      O.KernelImage = Image;
      O.KernelPDB = PDB;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      checkHalDriver(O, true);
    }
}
TEST(HalDispatchDriver,
     SampleAvlImportScansOriginalCodeWithoutExecutingAnAvlApi) {
  auto *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
       *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.Contract = C;
      O.KernelImage = Image;
      O.KernelPDB = PDB;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_avl_cfg_scan.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u);
      EXPECT_EQ(R->Messages, (std::vector<std::string>{
                                 "original avl code cfg scan passed"}));
      for (const auto &Call : R->Calls)
        EXPECT_NE(Call.Name, "RtlInsertElementGenericTableFullAvl");
    }
}
} // namespace
} // namespace neverd::emulation
