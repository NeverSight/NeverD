///
/// \file
/// KernelFilterNameTests.
///

#include "FilterNamesTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/WindowsUnicode.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <cstdlib>
namespace neverd::emulation {
namespace {
class KernelFilterName : public testing::Test {
protected:
  static constexpr uint64_t S = 0x22000000, PC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  DriverOptions Options;
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
    auto Message = llvm::toString(V.takeError());
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  void bad(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    auto Message = llvm::toString(std::move(E));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A) {
    return value(Model->call(std::string(Name), A));
  }
  llvm::Expected<uint64_t> scoped(llvm::StringRef Name,
                                  llvm::ArrayRef<uint64_t> A,
                                  llvm::StringRef Module = "fltmgr.sys") {
    KernelExportRegistry::Export E{Name.str(), Module.str(), 0};
    return Model->call(E, A, [](unsigned) -> llvm::Expected<uint64_t> {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unexpected extra subsystem argument");
    });
  }
  uint64_t flt(const char *Name, std::initializer_list<uint64_t> A) {
    return value(scoped(Name, A));
  }
  uint64_t get(uint64_t A, unsigned W = 8) {
    ok(Model->validateGuestAccess(A, W, false));
    return value(Memory->readInteger(A, W));
  }
  void put(uint64_t A, uint64_t V, unsigned W = 8) {
    ok(Memory->writeInteger(A, V, W));
  }
  void initialize(DriverOptions O) {
    Model.reset();
    Result = {};
    Options = std::move(O);
    Memory = value(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(S, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = PC - 4096;
    I.Entry = PC;
    I.Size = 0x3000;
    ok(Model->initialize(I, Options));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void SetUp() override {
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.FilterManagerFileNames = true;
    O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
        {"C",
         "\\Device\\HarddiskVolume1",
         {{"\\dir", true, {}}, {"\\dir\\file.txt", false, {1}}}}};
    initialize(O);
  }
  uint64_t file(llvm::StringRef Path = "\\dir\\file.txt") {
    auto Bytes = value(windows_unicode::bytes("\\??\\C:" + Path.str(), true));
    ok(Memory->write(S + 0x200, Bytes));
    put(S + 0x100, Bytes.size() - 2, 2);
    put(S + 0x102, Bytes.size(), 2);
    put(S + 0x108, S + 0x200);
    put(S + 0x300, 48, 4);
    put(S + 0x310, S + 0x100);
    put(S + 0x318, 0x240, 4);
    EXPECT_EQ(call("ZwCreateFile",
                   {S, 0x100001, S + 0x300, S + 0x400, 0, 0, 7, 1, 0x20, 0, 0}),
              0u);
    EXPECT_EQ(
        call("ObReferenceObjectByHandle", {get(S), 0, 0, 0, S + 0x500, 0}), 0u);
    return get(S + 0x500);
  }
  uint64_t query(uint64_t Object, uint32_t Options = 0x101) {
    EXPECT_EQ(
        flt("FltGetFileNameInformationUnsafe", {Object, 0, Options, S + 0x600}),
        0u);
    return get(S + 0x600);
  }
};
TEST_F(KernelFilterName, OriginalParserSpansMatchEveryAdmittedNativeName) {
  struct Case {
    unsigned Format;
    const char *Path;
    unsigned E;
    int EP;
    unsigned S;
    int SP;
    unsigned F;
    int FP;
    unsigned P;
    int PP;
  };
  const Case Cases[] = {
#define NEVERD_FLT_PARSE_NATIVE_CASE(F, N, EL, EP, SL, SP, FL, FP, PL, PP)     \
  {F, N, EL, EP, SL, SP, FL, FP, PL, PP},
#include "FilterNameNativeCases.def"
#undef NEVERD_FLT_PARSE_NATIVE_CASE
  };
  ASSERT_EQ(std::size(Cases), 20u);
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Path);
    SCOPED_TRACE(C.Format);
    auto O = Options;
    auto &Upcase = O.FiniteFileSystem->at(0).UpcaseTable;
    Upcase.resize(65536);
    for (unsigned I = 0; I < Upcase.size(); ++I)
      Upcase[I] = I >= 'a' && I <= 'z' ? I - 'a' + 'A' : I;
    auto &Entries = O.FiniteFileSystem->at(0).Entries;
    Entries.clear();
    const auto Path = llvm::StringRef(C.Path);
    const auto Last = Path.rfind('\\');
    if (Last)
      Entries.push_back({Path.take_front(Last).str(), true, {}});
    if (Path != "\\")
      Entries.push_back({Path.str(), false, {1}});
    initialize(O);
    const auto Object = file(Path);
    const auto Info = query(Object, 0x100 | C.Format);
    const auto Data = get(Info + 16);
    EXPECT_EQ(get(Info, 2), 120u);
    EXPECT_EQ(get(Info + 2, 2), 0u);
    EXPECT_EQ(get(Info + 4, 4), C.Format);
    bad(Model->validateGuestAccess(Info + 56, 2, false),
        "FltParseFileNameInformation");
    flt("FltParseFileNameInformation", {Info});
    EXPECT_EQ(get(Info + 2, 2), 15u);
    for (const auto &[At, Length, Offset] :
         {std::tuple{56, C.E, C.EP}, std::tuple{72, C.S, C.SP},
          std::tuple{88, C.F, C.FP}, std::tuple{104, C.P, C.PP}}) {
      EXPECT_EQ(get(Info + At, 2), Length);
      EXPECT_EQ(get(Info + At + 2, 2), Length);
      EXPECT_EQ(get(Info + At + 8), Offset < 0 ? 0u : Data + Offset);
    }
    flt("FltReleaseFileNameInformation", {Info});
    call("ZwClose", {get(S)});
    call("ObfDereferenceObject", {Object});
  }
}
TEST_F(KernelFilterName, CacheAndReferencesHaveIndependentCallerLifetimes) {
  const auto Object = file();
  put(S + 0x600, 0x11223344);
  EXPECT_EQ(
      flt("FltGetFileNameInformationUnsafe", {Object, 0, 0x201, S + 0x600}),
      0xc01c0018u);
  EXPECT_EQ(get(S + 0x600), 0x11223344u);
  const auto First = query(Object);
  EXPECT_EQ(query(Object, 0x201), First);
  flt("FltReferenceFileNameInformation", {First});
  flt("FltReleaseFileNameInformation", {First});
  flt("FltReleaseFileNameInformation", {First});
  EXPECT_EQ(get(First, 2), 120u);
  flt("FltReleaseFileNameInformation", {First});
  bad(Model->validateGuestAccess(First, 2, false), "caller reference");
  bad(scoped("FltReleaseFileNameInformation", {First}), "caller-owned");
  EXPECT_EQ(query(Object, 0x401), First);
  flt("FltReleaseFileNameInformation", {First});
  const auto Fresh = query(Object, 0x301);
  EXPECT_NE(Fresh, First);
  flt("FltReleaseFileNameInformation", {Fresh});
  bad(Model->validateGuestAccess(Fresh, 2, false), "freed");
}
TEST_F(KernelFilterName, QueryAndReleaseAreScopedAndRejectWrongArity) {
  KernelExportRegistry::Export E{"FltGetFileNameInformationUnsafe",
                                 "fltmgr.sys", 0};
  EXPECT_EQ(KernelModel::argumentCount(E), 4u);
  E.Module = "ntoskrnl.exe";
  EXPECT_FALSE(KernelModel::argumentCount(E));
  E.Module = "ndis.sys";
  EXPECT_FALSE(KernelModel::argumentCount(E));
  bad(scoped("FltReleaseFileNameInformation", {}), "arity");
  bad(Model->call("FltReleaseFileNameInformation", {S}), "unknown API");
}
TEST_F(KernelFilterName, ProviderFactsIrqlAndClosedFileStateAreExplicit) {
  auto O = Options;
  O.FilterManagerFileNames.reset();
  initialize(O);
  bad(scoped("FltGetFileNameInformationUnsafe", {0, 0, 0x101, S}),
      "filter_manager_file_names");
  O.FilterManagerFileNames = false;
  initialize(O);
  EXPECT_EQ(flt("FltGetFileNameInformationUnsafe", {0, 0, 0, 0}), 0xc01c0007u);
  O.FilterManagerFileNames = true;
  initialize(O);
  const auto Object = file();
  const auto Info = query(Object);
  bad(Model->validateGuestAccess(Info, 2, true), "read-only");
  call("KfRaiseIrql", {2});
  bad(scoped("FltReleaseFileNameInformation", {Info}), "IRQL");
  bad(Model->validateGuestAccess(Info, 2, false), "APC_LEVEL");
  call("KeLowerIrql", {0});
  bad(scoped("FltGetFileNameInformationUnsafe", {Object, 1, 0x101, S}),
      "instance");
  bad(scoped("FltGetFileNameInformationUnsafe", {Object, 0, 0x103, S}), "8.3");
  call("ZwClose", {get(S)});
  EXPECT_EQ(
      flt("FltGetFileNameInformationUnsafe", {Object, 0, 0x101, S + 0x600}),
      0xc01c0005u);
  EXPECT_EQ(get(Info, 2), 120u);
  flt("FltReleaseFileNameInformation", {Info});
  call("ObfDereferenceObject", {Object});
}
TEST_F(KernelFilterName,
       UnloadRejectsBorrowedNamesAndDropsOnlyUnborrowedCache) {
  const auto Object = file(), Info = query(Object);
  call("ZwClose", {get(S)});
  call("ObfDereferenceObject", {Object});
  ok(Model->validateGuestAccess(
      Model->driverObject() + windows::DriverUnloadOffset, 8, true));
  put(Model->driverObject() + windows::DriverUnloadOffset, PC);
  ok(Model->finishEntry());
  auto U = Model->beginUnload();
  ASSERT_TRUE(bool(U));
  bad(Model->finishUnload(), "live name references");
  flt("FltReleaseFileNameInformation", {Info});
  ok(Model->finishUnload());
  bad(Model->validateGuestAccess(Info, 2, false), "freed");
}
TEST_F(KernelFilterName, LastFileHandleDropsAnUnborrowedCacheEntry) {
  const auto Object = file(), Info = query(Object);
  flt("FltReleaseFileNameInformation", {Info});
  bad(Model->validateGuestAccess(Info, 2, false), "caller reference");
  call("ZwClose", {get(S)});
  bad(Model->validateGuestAccess(Info, 2, false), "freed");
  call("ObfDereferenceObject", {Object});
}
TEST(FilterNameDriver, CompiledImportsObserveSharedNamesAndCleanUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Paging : {false, true}) {
        SCOPED_TRACE(int(Contract));
        SCOPED_TRACE(Preemptive);
        SCOPED_TRACE(Paging);
        auto O = test::filterNamesOptions();
        O.Contract = Contract;
        O.PageTables = Paging;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_filter_names.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u);
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        EXPECT_EQ(R->Messages,
                  (std::vector<std::string>{"filter name lifetime passed",
                                            "filter name unload passed"}));
        unsigned Calls = 0;
        for (const auto &Call : R->Calls)
          if (llvm::StringRef(Call.Name).starts_with("Flt"))
            ++Calls;
        EXPECT_EQ(Calls, 11u);
      }
  auto I = loadDriverImage(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_filter_names.sys",
                           64 * 1024 * 1024);
  ASSERT_TRUE(bool(I));
  unsigned Scoped = 0;
  for (const auto &Import : I->Imports)
    if (llvm::StringRef(Import.Name).starts_with("Flt")) {
      EXPECT_TRUE(
          llvm::StringRef(Import.Module).equals_insensitive("fltmgr.sys"));
      ++Scoped;
    }
  EXPECT_EQ(Scoped, 4u);
}
TEST(FilterNameDriver, KernelReferencePreservesTheSeparateSubsystemNamespace) {
  const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
             *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = test::filterNamesOptions();
      O.Contract = Contract;
      O.KernelImage = Image;
      O.KernelPDB = PDB;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_filter_names.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u);
      EXPECT_TRUE(R->UnloadCompleted);
      unsigned Calls = 0;
      bool Kernel = false;
      for (const auto &Call : R->Calls) {
        if (llvm::StringRef(Call.Name).starts_with("Flt")) {
          EXPECT_LT(Call.PC, 0xfffff80000000000ULL);
          ++Calls;
        }
        if (Call.Name == "ZwCreateFile") {
          EXPECT_GE(Call.PC, 0xfffff80000000000ULL);
          Kernel = true;
        }
      }
      EXPECT_TRUE(Kernel);
      EXPECT_EQ(Calls, 11u);
    }
}
TEST(FilterNameScenario, OptionalProviderRetainsFalseAndRejectsInvalidTypes) {
  auto J = test::filterNamesScenario();
  for (bool Enabled : {false, true}) {
    J["filter_manager_file_names"] = Enabled;
    auto Copy = J;
    auto O = driverOptionsFromScenarioJSON(
        llvm::formatv("{0}", llvm::json::Value(std::move(Copy))).str());
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    EXPECT_EQ(O->FilterManagerFileNames, Enabled);
    DriverResult R;
    R.Configuration = *O;
    auto Report = llvm::json::parse(driverResultJSON(R));
    ASSERT_TRUE(bool(Report));
    EXPECT_EQ(Report->getAsObject()
                  ->getObject("configuration")
                  ->getBoolean("filter_manager_file_names"),
              Enabled);
  }
  auto O = test::filterNamesOptions();
  O.FiniteFileSystem.reset();
  auto E = validateDriverFilterManager(O);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O = test::filterNamesOptions();
  O.Version.reset();
  E = validateDriverFilterManager(O);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  for (const char *Bad : {"null", "1", "[]", "{}", "\"true\""}) {
    auto Copy = driverOptionsFromScenarioJSON(
        std::string("{\"filter_manager_file_names\":") + Bad + "}");
    ASSERT_FALSE(bool(Copy));
    llvm::consumeError(Copy.takeError());
  }
}
} // namespace
} // namespace neverd::emulation
