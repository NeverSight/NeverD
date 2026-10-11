//===- KernelExportTests.cpp - Guest kernel export contracts --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Static and dynamic routine lookup must agree on identity and availability.
/// Counted UTF-16 names retain the guest memory and object-lifetime contracts.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelExportRegistry.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelModuleImages.h"

#include "llvm/Object/COFF.h"

#include <array>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::emulation {
namespace {

uint64_t requireValue(llvm::Expected<uint64_t> Value) {
  if (!Value) {
    ADD_FAILURE() << llvm::toString(Value.takeError());
    return 0;
  }
  return *Value;
}

std::string requireError(llvm::Expected<uint64_t> Value) {
  if (Value) {
    ADD_FAILURE() << "unexpected successful value: " << *Value;
    return {};
  }
  return llvm::toString(Value.takeError());
}

TEST(KernelExports, SeededModeledExportAndDuplicateImportsShareIdentity) {
  DriverOptions Options;
  Options.KernelExports.emplace("ExAllocatePoolWithTag", true);
  KernelExportRegistry Registry;
  ASSERT_EQ(llvm::toString(Registry.initialize(Options)), "");
  const uint64_t Dynamic =
      requireValue(Registry.resolve("ExAllocatePoolWithTag"));
  ASSERT_NE(Dynamic, 0u);
  EXPECT_EQ(requireValue(Registry.bindImport(
                {0x1000, "ntoskrnl.exe", "ExAllocatePoolWithTag"})),
            Dynamic);
  EXPECT_EQ(requireValue(Registry.bindImport(
                {0x2000, "ntkrnlmp.exe", "ExAllocatePoolWithTag"})),
            Dynamic);
  const auto *Export = Registry.lookup(Dynamic);
  ASSERT_NE(Export, nullptr);
  EXPECT_EQ(Export->Name, "ExAllocatePoolWithTag");
  EXPECT_EQ(Export->Address, Dynamic);
  EXPECT_EQ(Registry.lookup(Dynamic + 1), nullptr);
}

TEST(KernelExports, AvailabilityIsIndependentOfAnExecutionModel) {
  DriverOptions Options;
  Options.KernelExports = {{"NeverDOptionalAbsent", false},
                           {"NeverDOptionalPresent", true}};
  KernelExportRegistry Registry;
  ASSERT_EQ(llvm::toString(Registry.initialize(Options)), "");
  EXPECT_EQ(requireValue(Registry.resolve("NeverDOptionalAbsent")), 0u);
  EXPECT_NE(
      requireError(Registry.resolve("NeverDUnspecified")).find("unspecified"),
      std::string::npos);
  const uint64_t Present =
      requireValue(Registry.resolve("NeverDOptionalPresent"));
  const uint64_t Base = *KernelExportRegistry::moduleBase("ntoskrnl.exe");
  EXPECT_GE(Present, Base + profile::KernelModuleCodeRVA);
  EXPECT_LT(Present, Base + profile::KernelModuleExportRVA);
  ASSERT_NE(Registry.lookup(Present), nullptr);
  EXPECT_EQ(Registry.lookup(Present)->Name, "NeverDOptionalPresent");
  EXPECT_FALSE(KernelModel::argumentCount("NeverDOptionalPresent"));
  EXPECT_EQ(requireValue(Registry.bindImport(
                {0x3000, "ntoskrnl.exe", "NeverDOptionalPresent"})),
            Present);

  // A static dependency establishes presence in this concrete environment.
  const uint64_t Imported = requireValue(
      Registry.bindImport({0x4000, "ntoskrnl.exe", "NeverDStaticOnly"}));
  EXPECT_EQ(requireValue(Registry.resolve("NeverDStaticOnly")), Imported);
  EXPECT_NE(Imported, Present);
  EXPECT_FALSE(KernelModel::argumentCount("NeverDStaticOnly"));
}

TEST(KernelExports, ExplicitAbsenceOverridesTheDefaultAndRejectsStaticImports) {
  DriverOptions Options;
  Options.KernelExports.emplace("ExAllocatePoolWithTag", false);
  KernelExportRegistry Registry;
  ASSERT_EQ(llvm::toString(Registry.initialize(Options)), "");
  EXPECT_EQ(requireValue(Registry.resolve("ExAllocatePoolWithTag")), 0u);
  EXPECT_NE(requireError(Registry.bindImport(
                             {0x1000, "ntoskrnl.exe", "ExAllocatePoolWithTag"}))
                .find("explicitly absent"),
            std::string::npos);
  EXPECT_EQ(Registry.lookup(0), nullptr);
  EXPECT_NE(requireValue(Registry.resolve("RtlInitUnicodeString")), 0u);
}

TEST(KernelExports, AModeledHeaderHelperDoesNotImplyExportAvailability) {
  // These WDM helpers are not named exports in the inspected Windows 10 kernel.
  // The version-independent profile therefore requires a scenario declaration,
  // rather than inventing universal presence or absence from a helper model.
  for (const char *Name :
       {"MmGetSystemAddressForMdlSafe", "IoGetCurrentIrpStackLocation"}) {
    SCOPED_TRACE(Name);
    ASSERT_TRUE(KernelModel::argumentCount(Name));
    KernelExportRegistry Default;
    ASSERT_EQ(llvm::toString(Default.initialize({})), "");
    EXPECT_NE(requireError(Default.resolve(Name)).find("unspecified"),
              std::string::npos);
    const uint64_t Imported =
        requireValue(Default.bindImport({0x1000, "ntoskrnl.exe", Name}));
    EXPECT_NE(Imported, 0u);
    EXPECT_EQ(requireValue(Default.resolve(Name)), Imported);

    for (bool Present : {false, true}) {
      DriverOptions Options;
      Options.KernelExports.emplace(Name, Present);
      KernelExportRegistry Declared;
      ASSERT_EQ(llvm::toString(Declared.initialize(Options)), "");
      EXPECT_EQ(requireValue(Declared.resolve(Name)) != 0, Present);
    }
  }
}

TEST(KernelExports, RejectsMalformedInventoryBeforeItCanBecomeAnExport) {
  for (const std::string &Name :
       {std::string(), std::string("Bad Name"), std::string("Bad\nName"),
        std::string(profile::MaxKernelExportNameSize + 1, 'A')}) {
    SCOPED_TRACE(Name);
    DriverOptions Options;
    Options.KernelExports.emplace(Name, true);
    KernelExportRegistry Registry;
    auto Error = Registry.initialize(Options);
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("ASCII"),
              std::string::npos);
  }
}

TEST(KernelExports, CanonicalImportProvidersShareOneExplicitPolicy) {
  for (const char *Module : {"ntoskrnl.exe", "NTOSKRNL.EXE", "NtKrNlMp.ExE"}) {
    auto Canonical = KernelExportRegistry::canonicalImportModule(Module);
    ASSERT_TRUE(Canonical);
    EXPECT_EQ(*Canonical, "ntoskrnl.exe");
  }
  for (const char *Module : {"wdfldr.sys", "WDFLDR.SYS", "WdFLdr.Sys"}) {
    auto Canonical = KernelExportRegistry::canonicalImportModule(Module);
    ASSERT_TRUE(Canonical);
    EXPECT_EQ(*Canonical, "wdfldr.sys");
  }
  for (const char *Module : {"hal.dll", "HAL.DLL", "Hal.Dll"}) {
    auto Canonical = KernelExportRegistry::canonicalImportModule(Module);
    ASSERT_TRUE(Canonical);
    EXPECT_EQ(*Canonical, "hal.dll");
  }
  for (const char *Module : {"", "wdf01000.sys", "ntoskrnl", "hal",
                             "path/ntoskrnl.exe", "ntoskrnl.exe "})
    EXPECT_FALSE(KernelExportRegistry::canonicalImportModule(Module));
}

TEST(KernelExports, HALStaticAndDynamicLookupRetainProviderIdentity) {
  KernelExportRegistry Registry;
  ASSERT_EQ(llvm::toString(Registry.initialize({})), "");
  const uint64_t Dynamic =
      requireValue(Registry.resolve("KeQueryPerformanceCounter"));
  ASSERT_NE(Dynamic, 0u);
  EXPECT_EQ(requireValue(Registry.bindImport(
                {0x1000, "HaL.DlL", "KeQueryPerformanceCounter"})),
            Dynamic);
  const auto *Entry = Registry.lookup(Dynamic);
  ASSERT_NE(Entry, nullptr);
  EXPECT_EQ(Entry->Module, "hal.dll");
  EXPECT_EQ(KernelModel::argumentCount(*Entry), 1u);
  const uint64_t Kernel = requireValue(Registry.bindImport(
      {0x2000, "ntoskrnl.exe", "KeQueryPerformanceCounter"}));
  EXPECT_NE(Kernel, Dynamic);
  EXPECT_FALSE(KernelModel::argumentCount(*Registry.lookup(Kernel)));
  EXPECT_NE(requireError(Registry.resolve("KeQueryPerformanceCounter"))
                .find("ambiguous"),
            std::string::npos);
}

TEST(KernelExports, HALAbsenceAndUnknownImportsDoNotInventSemantics) {
  DriverOptions Options;
  Options.KernelExports.emplace("KeQueryPerformanceCounter", false);
  KernelExportRegistry Registry;
  ASSERT_EQ(llvm::toString(Registry.initialize(Options)), "");
  EXPECT_EQ(requireValue(Registry.resolve("KeQueryPerformanceCounter")), 0u);
  EXPECT_NE(requireError(Registry.bindImport(
                             {0x1000, "hal.dll", "KeQueryPerformanceCounter"}))
                .find("explicitly absent"),
            std::string::npos);
  for (const char *Name :
       {"UnknownHALRoutine", "DbgPrint", "keQueryPerformanceCounter"}) {
    const uint64_t Address =
        requireValue(Registry.bindImport({0x2000, "HAL.DLL", Name}));
    ASSERT_NE(Address, 0u);
    EXPECT_FALSE(KernelModel::argumentCount(*Registry.lookup(Address)));
    if (llvm::StringRef(Name) != "DbgPrint")
      EXPECT_EQ(requireValue(Registry.resolve(Name)), Address);
  }
}

TEST(KernelExports, KernelAndLoaderSameSpellingHaveDistinctIdentities) {
  KernelExportRegistry Registry;
  ASSERT_EQ(llvm::toString(Registry.initialize({})), "");
  const uint64_t Kernel = requireValue(Registry.resolve("DbgPrint"));
  const uint64_t Loader =
      requireValue(Registry.bindImport({0x1000, "WDFLDR.SYS", "DbgPrint"}));
  ASSERT_NE(Loader, Kernel);
  EXPECT_EQ(
      requireValue(Registry.bindImport({0x2000, "NtKrNlMp.ExE", "DbgPrint"})),
      Kernel);
  EXPECT_EQ(
      requireValue(Registry.bindImport({0x3000, "WdFlDr.SyS", "DbgPrint"})),
      Loader);
  EXPECT_EQ(requireValue(Registry.resolve("DbgPrint")), Kernel);
  ASSERT_NE(Registry.lookup(Loader), nullptr);
  EXPECT_EQ(Registry.lookup(Loader)->Module, "wdfldr.sys");
  EXPECT_EQ(Registry.lookup(Kernel)->Module, "ntoskrnl.exe");
  EXPECT_EQ(Registry.lookup(Loader)->Kind,
            KernelExportRegistry::ExportKind::ModuleExport);
  EXPECT_EQ(Registry.lookup(Loader)->Binding, 0u);
}

TEST(KernelExports, KernelPresenceAndAbsenceOverridesCannotOverrideLoader) {
  for (bool Present : {false, true}) {
    DriverOptions Options;
    Options.KernelExports.emplace("WdfVersionBind", Present);
    KernelExportRegistry Registry;
    ASSERT_EQ(llvm::toString(Registry.initialize(Options)), "");
    const uint64_t Kernel = requireValue(Registry.resolve("WdfVersionBind"));
    const uint64_t Loader = requireValue(
        Registry.bindImport({0x1000, "WDFLDR.SYS", "WdfVersionBind"}));
    ASSERT_NE(Loader, 0u);
    EXPECT_NE(Loader, Kernel);
    EXPECT_EQ(Kernel != 0, Present);
    EXPECT_EQ(requireValue(Registry.resolve("WdfVersionBind")), Kernel);
    if (Present) {
      EXPECT_EQ(requireValue(Registry.bindImport(
                    {0x2000, "ntoskrnl.exe", "WdfVersionBind"})),
                Kernel);
    } else {
      EXPECT_NE(requireError(Registry.bindImport(
                                 {0x2000, "ntoskrnl.exe", "WdfVersionBind"}))
                    .find("explicitly absent"),
                std::string::npos);
    }
    EXPECT_EQ(Registry.lookup(Loader)->Module, "wdfldr.sys");
  }
}

TEST(KernelExports, UnknownLoaderImportsRemainLazyAndCaseSensitive) {
  KernelExportRegistry Registry;
  ASSERT_EQ(llvm::toString(Registry.initialize({})), "");
  const uint64_t First = requireValue(
      Registry.bindImport({0x1000, "wdfldr.sys", "UnknownFrameworkRoutine"}));
  EXPECT_NE(requireError(Registry.resolve("UnknownFrameworkRoutine"))
                .find("unspecified"),
            std::string::npos);
  const uint64_t DifferentSpelling = requireValue(
      Registry.bindImport({0x2000, "wdfldr.sys", "unknownFrameworkRoutine"}));
  EXPECT_NE(First, DifferentSpelling);
  EXPECT_EQ(Registry.lookup(First)->Name, "UnknownFrameworkRoutine");
  EXPECT_EQ(Registry.lookup(First)->Module, "wdfldr.sys");
  const uint64_t Kernel = requireValue(
      Registry.bindImport({0x3000, "ntoskrnl.exe", "UnknownFrameworkRoutine"}));
  EXPECT_NE(First, Kernel);
  EXPECT_EQ(requireValue(Registry.resolve("UnknownFrameworkRoutine")), Kernel);
}

TEST(KernelExports, FrameworkFunctionsRetainBindingAndDoNotAliasImports) {
  constexpr uint64_t FirstBinding = 0x70004000;
  constexpr uint64_t SecondBinding = 0x70005000;
  KernelExportRegistry Registry;
  DriverOptions Options;
  Options.KernelExports.emplace("WdfDriverCreate", true);
  ASSERT_EQ(llvm::toString(Registry.initialize(Options)), "");
  const uint64_t Kernel = requireValue(Registry.resolve("WdfDriverCreate"));
  const uint64_t Loader = requireValue(
      Registry.bindImport({0x1000, "wdfldr.sys", "WdfDriverCreate"}));
  const uint64_t First = requireValue(
      Registry.insertFrameworkFunction(FirstBinding, "WdfDriverCreate"));
  const uint64_t Second = requireValue(
      Registry.insertFrameworkFunction(SecondBinding, "WdfDriverCreate"));
  EXPECT_NE(First, Second);
  EXPECT_NE(First, Kernel);
  EXPECT_NE(First, Loader);
  EXPECT_NE(Second, Kernel);
  EXPECT_NE(Second, Loader);
  EXPECT_EQ(requireValue(Registry.insertFrameworkFunction(FirstBinding,
                                                          "WdfDriverCreate")),
            First);
  EXPECT_EQ(requireValue(Registry.insertFrameworkFunction(SecondBinding,
                                                          "WdfDriverCreate")),
            Second);
  const auto *Export = Registry.lookup(First);
  ASSERT_NE(Export, nullptr);
  EXPECT_EQ(Export->Name, "WdfDriverCreate");
  EXPECT_EQ(Export->Module, "wdf01000.sys");
  EXPECT_EQ(Export->Binding, FirstBinding);
  EXPECT_EQ(Export->Kind, KernelExportRegistry::ExportKind::FrameworkFunction);
  EXPECT_EQ(requireValue(Registry.resolve("WdfDriverCreate")), Kernel);
  EXPECT_EQ(Registry.lookup(Second)->Binding, SecondBinding);
}

TEST(KernelExports, FullFrameworkTablePreservesAllThunkIdentities) {
  KernelExportRegistry Registry;
  ASSERT_EQ(llvm::toString(Registry.initialize({})), "");
  const auto *Kernel =
      Registry.lookup(requireValue(Registry.resolve("DbgPrint")));
  ASSERT_NE(Kernel, nullptr);
  std::vector<uint64_t> Table;
  for (unsigned I = 0; I < 458; ++I) {
    const std::string Name = "WdfTableSlot" + std::to_string(I);
    Table.push_back(
        requireValue(Registry.insertFrameworkFunction(0x70001000, Name)));
    if (I)
      EXPECT_EQ(Table[I], Table[I - 1] + profile::ThunkStride);
  }
  EXPECT_EQ(Kernel->Name, "DbgPrint");
  EXPECT_EQ(Registry.lookup(Kernel->Address), Kernel);
  for (unsigned I = 0; I < Table.size(); ++I) {
    const std::string Name = "WdfTableSlot" + std::to_string(I);
    EXPECT_EQ(requireValue(Registry.insertFrameworkFunction(0x70001000, Name)),
              Table[I]);
    ASSERT_NE(Registry.lookup(Table[I]), nullptr);
    EXPECT_EQ(Registry.lookup(Table[I])->Name, Name);
    EXPECT_EQ(Registry.lookup(Table[I])->Binding, 0x70001000u);
  }
  EXPECT_NE(requireError(Registry.resolve("WdfTableSlot0")).find("unspecified"),
            std::string::npos);
}

TEST(KernelExports, NamespaceCapacityNeverAllocatesTheReturnSentinel) {
  KernelExportRegistry Registry;
  ASSERT_EQ(llvm::toString(Registry.initialize({})), "");
  const uint64_t Kernel = requireValue(Registry.resolve("DbgPrint"));
  const uint64_t Sentinel =
      profile::ThunkBase + profile::ThunkSize - profile::ThunkStride;
  uint64_t Next =
      profile::ThunkBase + Registry.entries().size() * profile::ThunkStride;
  unsigned Index = 0;
  while (Next < Sentinel) {
    EXPECT_EQ(requireValue(Registry.insertFrameworkFunction(
                  0x70001000, "WdfCapacity" + std::to_string(Index++))),
              Next);
    Next += profile::ThunkStride;
  }
  EXPECT_EQ(Registry.lookup(Sentinel), nullptr);
  EXPECT_NE(
      requireError(Registry.insertFrameworkFunction(0x70001000, "TooMany"))
          .find("capacity"),
      std::string::npos);
  EXPECT_NE(requireError(
                Registry.bindImport({0x1000, "wdfldr.sys", "UnknownTooMany"}))
                .find("capacity"),
            std::string::npos);
  EXPECT_EQ(requireValue(Registry.resolve("DbgPrint")), Kernel);
  EXPECT_NE(requireValue(
                Registry.insertFrameworkFunction(0x70001000, "WdfCapacity0")),
            0u);
  EXPECT_EQ(Registry.lookup(Sentinel), nullptr);
}

TEST(KernelExports, InvalidIdentitiesDoNotConsumeAThunkOrEstablishPresence) {
  KernelExportRegistry Registry;
  ASSERT_EQ(llvm::toString(Registry.initialize({})), "");
  const uint64_t Before = requireValue(
      Registry.bindImport({0x1000, "wdfldr.sys", "BeforeInvalidInputs"}));
  for (const std::string &Name :
       {std::string(), std::string("Bad Name"), std::string("Bad\0Name", 8),
        std::string(profile::MaxKernelExportNameSize + 1, 'A')}) {
    EXPECT_NE(requireError(Registry.bindImport({0x1000, "wdfldr.sys", Name}))
                  .find("ASCII"),
              std::string::npos);
    EXPECT_NE(requireError(Registry.insertFrameworkFunction(0x70001000, Name))
                  .find("ASCII"),
              std::string::npos);
  }
  EXPECT_NE(requireError(Registry.insertFrameworkFunction(0, "WdfValid"))
                .find("binding identity"),
            std::string::npos);
  EXPECT_NE(
      requireError(Registry.bindImport({0x1000, "wdf01000.sys", "WdfValid"}))
          .find("provider"),
      std::string::npos);
  const uint64_t After = requireValue(
      Registry.bindImport({0x2000, "wdfldr.sys", "AfterInvalidInputs"}));
  EXPECT_EQ(After, Before + profile::ThunkStride);
}

TEST(KernelExports, InitializationFailurePublishesNoPartialNamespace) {
  KernelExportRegistry Registry;
  EXPECT_NE(requireError(Registry.bindImport({0x1000, "wdfldr.sys", "Any"}))
                .find("not initialized"),
            std::string::npos);
  EXPECT_NE(requireError(Registry.insertFrameworkFunction(1, "Any"))
                .find("not initialized"),
            std::string::npos);
  DriverOptions Options;
  Options.KernelExports = {{"AlreadySeenAbsent", false}, {"Bad Name", true}};
  auto Error = Registry.initialize(Options);
  ASSERT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(Registry.lookup(profile::ThunkBase), nullptr);
  EXPECT_NE(requireError(Registry.resolve("AlreadySeenAbsent"))
                .find("not initialized"),
            std::string::npos);
  Options.KernelExports.clear();
  for (unsigned I = 0; I < profile::MaxImports; ++I)
    Options.KernelExports.emplace("Explicit" + std::to_string(I), true);
  Error = Registry.initialize(Options);
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("capacity"),
            std::string::npos);
  EXPECT_EQ(Registry.lookup(profile::ThunkBase), nullptr);
  ASSERT_EQ(llvm::toString(Registry.initialize({})), "");
  const uint64_t Valid = requireValue(Registry.resolve("DbgPrint"));
  Error = Registry.initialize({});
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("already initialized"),
            std::string::npos);
  EXPECT_EQ(requireValue(Registry.resolve("DbgPrint")), Valid);
}

TEST(KernelExports, ReadablePEExportsMatchTheAuthoritativeNamespace) {
  KernelExportRegistry Registry;
  DriverOptions Options;
  Options.KernelExports = {{"NeverDOptionalAbsent", false},
                           {"NeverDOptionalPresent", true}};
  ASSERT_EQ(llvm::toString(Registry.initialize(Options)), "");
  requireValue(Registry.bindImport({0x1000, "wdfldr.sys", "StaticUnknown"}));
  requireValue(Registry.insertFrameworkFunction(0x70005000, "WdfDriverCreate"));
  auto Images = makeKernelModuleImages(Registry);
  ASSERT_TRUE(bool(Images)) << llvm::toString(Images.takeError());
  ASSERT_EQ(Images->size(), 3u);
  size_t Seen = 0;
  for (const auto &Image : *Images) {
    const llvm::MemoryBufferRef Buffer(
        llvm::StringRef(reinterpret_cast<const char *>(Image.Bytes.data()),
                        Image.Bytes.size()),
        Image.Identity.Name);
    auto PE = llvm::object::COFFObjectFile::create(Buffer);
    ASSERT_TRUE(bool(PE)) << llvm::toString(PE.takeError());
    ASSERT_NE((*PE)->getPE32PlusHeader(), nullptr);
    EXPECT_EQ((*PE)->getMachine(), llvm::COFF::IMAGE_FILE_MACHINE_AMD64);
    EXPECT_EQ((*PE)->getPE32PlusHeader()->ImageBase, Image.Identity.Base);
    EXPECT_EQ((*PE)->getPE32PlusHeader()->SizeOfImage, Image.Identity.Size);
    std::string Previous;
    for (const auto &Export : (*PE)->export_directories()) {
      llvm::StringRef Name, Module;
      uint32_t RVA = 0;
      bool Forwarder = true;
      ASSERT_EQ(llvm::toString(Export.getSymbolName(Name)), "");
      ASSERT_EQ(llvm::toString(Export.getDllName(Module)), "");
      ASSERT_EQ(llvm::toString(Export.getExportRVA(RVA)), "");
      ASSERT_EQ(llvm::toString(Export.isForwarder(Forwarder)), "");
      EXPECT_FALSE(Forwarder);
      EXPECT_EQ(Module, Image.Identity.Name);
      EXPECT_GT(Name.str(), Previous);
      Previous = Name.str();
      const auto *Entry = Registry.lookup(Image.Identity.Base + RVA);
      ASSERT_NE(Entry, nullptr);
      EXPECT_EQ(Entry->Name, Name);
      EXPECT_EQ(Entry->Module, Module);
      EXPECT_EQ(Entry->Kind, KernelExportRegistry::ExportKind::ModuleExport);
      ++Seen;
    }
  }
  // The callback function is present only in its binding table, and the
  // explicitly absent export has no address or export-table row.
  EXPECT_EQ(Seen + 1, Registry.entries().size());
  EXPECT_EQ(requireValue(Registry.resolve("NeverDOptionalAbsent")), 0u);
}

TEST(KernelExports, OpaqueCodeRangesExcludeReadableHeadersAndExportTables) {
  for (const char *Module : {"ntoskrnl.exe", "hal.dll", "wdfldr.sys"}) {
    const uint64_t Base = *KernelExportRegistry::moduleBase(Module);
    const uint64_t Code = Base + profile::KernelModuleCodeRVA;
    const uint64_t End = Base + profile::KernelModuleExportRVA;
    EXPECT_FALSE(KernelExportRegistry::overlapsThunk(Base, 4096));
    EXPECT_FALSE(KernelExportRegistry::overlapsThunk(End, 4096));
    EXPECT_FALSE(KernelExportRegistry::overlapsThunk(Code, 0));
    EXPECT_FALSE(KernelExportRegistry::overlapsThunk(UINT64_MAX - 8, 8));
    EXPECT_TRUE(KernelExportRegistry::overlapsThunk(Code));
    EXPECT_TRUE(KernelExportRegistry::overlapsThunk(Code - 1, 2));
    EXPECT_TRUE(KernelExportRegistry::overlapsThunk(End - 1, 2));
  }
}

class KernelExportLookup : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint32_t Tag = 0x74736554;
  std::unique_ptr<UnicornBackend> Memory;
  KernelExportRegistry Exports;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

  void check(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }

  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(static_cast<bool>(Backend))
        << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    check(Memory->map(Scratch, profile::PageSize, Read | Write));
    DriverOptions Options;
    Options.KernelExports = {{"NeverDOptionalAbsent", false},
                             {"NeverDOptionalPresent", true}};
    check(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Name = "lookup-test.sys";
    Image.Base = 0x180000000;
    Image.Entry = 0x180001000;
    Image.Size = 0x3000;
    check(Model->initialize(Image, Options));
  }

  uint64_t invoke(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return requireValue(Model->call(Name, Arguments));
  }

  void record(uint64_t Address, uint64_t Buffer, uint16_t Length,
              uint16_t Maximum) {
    // The public x64 UNICODE_STRING ABI, constructed independently of lookup.
    check(Memory->writeInteger(Address, Length, 2));
    check(Memory->writeInteger(Address + 2, Maximum, 2));
    check(Memory->writeInteger(Address + 8, Buffer, 8));
  }

  void name(uint64_t Record, uint64_t Buffer, std::string_view Name) {
    std::vector<uint8_t> Bytes(Name.size() * 2);
    for (size_t I = 0; I < Name.size(); ++I)
      Bytes[I * 2] = static_cast<uint8_t>(Name[I]);
    check(Memory->write(Buffer, Bytes));
    record(Record, Buffer, Bytes.size(), Bytes.size());
  }

  uint64_t lookup(uint64_t Record = Scratch) {
    return invoke("MmGetSystemRoutineAddress", {Record});
  }

  std::string lookupError(uint64_t Record = Scratch) {
    return requireError(Model->call("MmGetSystemRoutineAddress", {Record}));
  }
};

TEST_F(KernelExportLookup, CountedNameAtMappingEndNeedsNoNullTerminator) {
  constexpr std::string_view Routine = "ExAllocatePoolWithTag";
  const uint64_t Buffer = Scratch + profile::PageSize - Routine.size() * 2;
  name(Scratch, Buffer, Routine);
  const uint64_t Address = lookup();
  EXPECT_EQ(Address, requireValue(Exports.bindImport(
                         {0x1000, "ntoskrnl.exe", std::string(Routine)})));
  EXPECT_EQ(Address, lookup());
  EXPECT_FALSE(Memory->hasMemoryFault());
}

TEST_F(KernelExportLookup, ReturnsNullOnlyForDeclaredAbsenceOrAnEmptyName) {
  name(Scratch, Scratch + 32, "NeverDOptionalAbsent");
  EXPECT_EQ(lookup(), 0u);
  name(Scratch, Scratch + 32, "NeverDOptionalPresent");
  EXPECT_EQ(lookup(), requireValue(Exports.resolve("NeverDOptionalPresent")));
  name(Scratch, Scratch + 32, "NeverDUnspecified");
  EXPECT_NE(lookupError().find("unspecified"), std::string::npos);
  record(Scratch, 0, 0, 0);
  EXPECT_EQ(lookup(), 0u);
}

TEST_F(KernelExportLookup, HALCounterRetainsEnvironmentRecoveryDependency) {
  check(Model->captureUnpackBaseline());
  const auto HasDependencies = [&]() {
    auto Value = Model->unpackDependencies();
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return true;
    }
    return !Value->empty();
  };
  name(Scratch, Scratch + 32, "KeQueryPerformanceCounter");
  const auto *Entry = Exports.lookup(lookup());
  ASSERT_NE(Entry, nullptr);
  EXPECT_EQ(Entry->Module, "hal.dll");
  EXPECT_FALSE(HasDependencies());
  // Even without an output pointer, this observes environment-specific time.
  EXPECT_EQ(requireValue(Model->call(*Entry, {0}, nullptr)), 0u);
  EXPECT_TRUE(HasDependencies());
}

TEST_F(KernelExportLookup, HALCounterSharesSchedulerTimeAndChecksOutputMemory) {
  name(Scratch, Scratch + 32, "KeQueryPerformanceCounter");
  const auto *Entry = Exports.lookup(lookup());
  ASSERT_NE(Entry, nullptr);
  EXPECT_EQ(Entry->Module, "hal.dll");
  const auto Counter = [&](uint64_t Frequency) {
    return Model->call(*Entry, {Frequency}, nullptr);
  };
  constexpr uint64_t Frequency = Scratch + 256;
  check(Memory->writeInteger(Frequency - 8, UINT64_MAX, 8));
  check(Memory->writeInteger(Frequency + 8, UINT64_MAX, 8));
  EXPECT_EQ(requireValue(Counter(Frequency)), 0u);
  EXPECT_EQ(requireValue(Memory->readInteger(Frequency, 8)), 10000000u);
  EXPECT_EQ(requireValue(Memory->readInteger(Frequency - 8, 8)), UINT64_MAX);
  EXPECT_EQ(requireValue(Memory->readInteger(Frequency + 8, 8)), UINT64_MAX);
  EXPECT_EQ(requireValue(Counter(0)), 0u);
  check(Model->advanceExecutionTo100ns(12345));
  EXPECT_EQ(requireValue(Counter(0)), 12345u);
  Model->enterExecution(1);
  for (unsigned IRQL = 0; IRQL <= 15; ++IRQL) {
    SCOPED_TRACE(IRQL);
    EXPECT_EQ(invoke("KfRaiseIrql", {IRQL}), 0u);
    EXPECT_EQ(requireValue(Counter(Frequency)), 12345u);
    invoke("KeLowerIrql", {0});
  }
  const uint64_t Pool = invoke("ExAllocatePoolWithTag", {512, 16, Tag});
  invoke("ExFreePoolWithTag", {Pool, Tag});
  EXPECT_NE(requireError(Counter(Pool)).find("freed"), std::string::npos);
  EXPECT_NE(requireError(Model->call(*Entry, {}, nullptr)).find("arguments"),
            std::string::npos);
  EXPECT_EQ(Model->now100ns(), 12345u);
}

TEST_F(KernelExportLookup, DynamicLookupRemainsKernelOnlyAfterWDFBinding) {
  const uint64_t Loader = requireValue(
      Exports.bindImport({0x1000, "WDFLDR.SYS", "WdfVersionBind"}));
  const uint64_t Function = requireValue(
      Exports.insertFrameworkFunction(0x70005000, "WdfDriverCreate"));
  EXPECT_NE(Loader, Function);
  for (const char *Name : {"WdfVersionBind", "WdfDriverCreate"}) {
    name(Scratch, Scratch + 32, Name);
    EXPECT_NE(lookupError().find("unspecified"), std::string::npos);
  }
  requireValue(
      Exports.bindImport({0x2000, "wdfldr.sys", "NeverDOptionalAbsent"}));
  name(Scratch, Scratch + 32, "NeverDOptionalAbsent");
  EXPECT_EQ(lookup(), 0u);
  const uint64_t Kernel =
      requireValue(Exports.resolve("ExAllocatePoolWithTag"));
  EXPECT_NE(requireValue(Exports.bindImport(
                {0x3000, "wdfldr.sys", "ExAllocatePoolWithTag"})),
            Kernel);
  name(Scratch, Scratch + 32, "ExAllocatePoolWithTag");
  EXPECT_EQ(lookup(), Kernel);
}

TEST_F(KernelExportLookup, RejectsMalformedCountedStringsAndEmbeddedNulls) {
  constexpr uint64_t Buffer = Scratch + 32;
  for (auto [Length, Maximum] :
       {std::pair<uint16_t, uint16_t>{3, 4}, {4, 2}, {1026, 1026}}) {
    record(Scratch, Buffer, Length, Maximum);
    EXPECT_NE(lookupError().find("UNICODE_STRING"), std::string::npos);
  }
  record(Scratch, UINT64_MAX - 1, 4, 4);
  EXPECT_NE(lookupError().find("UNICODE_STRING"), std::string::npos);
  EXPECT_NE(lookupError(UINT64_MAX - 8).find("overflows"), std::string::npos);
  constexpr char EmbeddedNull[] = "RtlInitUnicodeString\0Extra";
  name(Scratch, Buffer,
       std::string_view(EmbeddedNull, sizeof(EmbeddedNull) - 1));
  EXPECT_NE(lookupError().find("encoding"), std::string::npos);
  record(Scratch, Buffer, 2, 2);
  check(Memory->writeInteger(Buffer, 0x0401, 2));
  EXPECT_NE(lookupError().find("encoding"), std::string::npos);
  EXPECT_FALSE(Memory->hasMemoryFault());
}

TEST_F(KernelExportLookup, EnforcesFreedBorrowedAndOpaqueMemoryLifetimes) {
  const uint64_t Allocation = invoke("ExAllocatePoolWithTag", {512, 128, Tag});
  name(Allocation, Allocation + 32, "RtlInitUnicodeString");
  EXPECT_NE(lookup(Allocation), 0u);
  invoke("ExFreePoolWithTag", {Allocation, Tag});
  EXPECT_NE(lookupError(Allocation).find("freed"), std::string::npos);
  record(Scratch, Allocation + 32, 4, 4);
  EXPECT_NE(lookupError().find("freed"), std::string::npos);

  check(Model->finishEntry());
  EXPECT_NE(lookupError(Model->registryPath()).find("freed"),
            std::string::npos);
  // DRIVER_OBJECT.DriverSection is deliberately outside the public model.
  record(Scratch, Model->driverObject() + 0x28, 4, 4);
  EXPECT_NE(lookupError().find("unmodeled"), std::string::npos);
  record(Scratch, profile::ThunkBase, 4, 4);
  EXPECT_NE(lookupError().find("thunks"), std::string::npos);
  EXPECT_FALSE(Memory->hasMemoryFault());
}

TEST_F(KernelExportLookup, ModuleQueryPublishesWin64RecordsWithBoundedWrites) {
  Model->enterExecution(profile::StackBase);
  const uint64_t Length = Scratch + 32, Buffer = Scratch + 128;
  check(Memory->writeInteger(Length, UINT64_MAX, 8));
  // Both scalar arguments use the declared ULONG width. The zero-length
  // query must not inspect an arbitrary SystemInformation pointer.
  EXPECT_EQ(invoke("NtQuerySystemInformation",
                   {0xabc0000000bULL, UINT64_MAX, 1ULL << 32, Length}),
            0xc0000004u);
  const uint64_t Required = requireValue(Memory->readInteger(Length, 4));
  ASSERT_EQ(Required, 8u + 3u * 296u);
  EXPECT_EQ(requireValue(Memory->readInteger(Length + 4, 4)), 0xffffffffu);
  EXPECT_EQ(invoke("ZwQuerySystemInformation", {11, UINT64_MAX, 0, 0}),
            0xc0000004u);
  check(Memory->writeInteger(Buffer - 8, UINT64_MAX, 8));
  check(Memory->writeInteger(Buffer + Required, UINT64_MAX, 8));
  EXPECT_EQ(
      invoke("ZwQuerySystemInformation", {11, Buffer, Required + 8, Length}),
      0u);
  EXPECT_EQ(requireValue(Memory->readInteger(Length, 4)), Required);
  EXPECT_EQ(requireValue(Memory->readInteger(Buffer, 4)), 3u);
  EXPECT_EQ(requireValue(Memory->readInteger(Buffer + 4, 4)), 0u);
  EXPECT_EQ(requireValue(Memory->readInteger(Buffer - 8, 8)), UINT64_MAX);
  EXPECT_EQ(requireValue(Memory->readInteger(Buffer + Required, 8)),
            UINT64_MAX);
  unsigned I = 0;
  for (const char *File : {"ntoskrnl.exe", "hal.dll", "lookup-test.sys"}) {
    const uint64_t Row = Buffer + 8 + I * 296;
    EXPECT_EQ(requireValue(Memory->readInteger(Row, 8)), 0u);
    EXPECT_EQ(requireValue(Memory->readInteger(Row + 8, 8)), 0u);
    EXPECT_EQ(requireValue(Memory->readInteger(Row + 28, 4)), 0u);
    EXPECT_EQ(requireValue(Memory->readInteger(Row + 32, 2)), I);
    EXPECT_EQ(requireValue(Memory->readInteger(Row + 34, 2)), I);
    EXPECT_EQ(requireValue(Memory->readInteger(Row + 36, 2)), 1u);
    const uint64_t Base = requireValue(Memory->readInteger(Row + 16, 8));
    const uint64_t Size = requireValue(Memory->readInteger(Row + 24, 4));
    const uint64_t Offset = requireValue(Memory->readInteger(Row + 38, 2));
    std::array<uint8_t, 256> Path{};
    check(Memory->read(Row + 40, Path));
    const std::string Prefix = I == 2 ? "\\SystemRoot\\System32\\drivers\\"
                                      : "\\SystemRoot\\System32\\";
    ASSERT_EQ(Offset, Prefix.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(Path.data())),
              Prefix + File);
    if (I == 2) {
      EXPECT_EQ(Base, 0x180000000u);
      EXPECT_EQ(Size, 0x3000u);
    } else {
      EXPECT_EQ(Base, *KernelExportRegistry::moduleBase(File));
      EXPECT_EQ(requireValue(Memory->readInteger(Base, 2)), 0x5a4du);
      const auto PE = requireValue(Memory->readInteger(Base + 0x3c, 4));
      EXPECT_EQ(requireValue(Memory->readInteger(Base + PE + 0x50, 4)), Size);
    }
    ++I;
  }
}

TEST_F(KernelExportLookup, ModuleQueryPreflightsBothOutputsAndPoolLifetimes) {
  constexpr uint64_t Buffer = Scratch + 128, Length = Scratch + 32;
  constexpr uint64_t Required = 8 + 3 * 296;
  std::vector<uint8_t> Before(profile::PageSize, 0xa7), After(Before.size());
  check(Memory->write(Scratch, Before));
  for (auto A : {std::array<uint64_t, 4>{11, Buffer, Required - 1, Length},
                 {11, Buffer, Required, Buffer + 1},
                 {11, Buffer, Required, Buffer - 3},
                 {11, Buffer, Required, Scratch + profile::PageSize - 2},
                 {11, Scratch + profile::PageSize - 16, Required, Length},
                 {11, UINT64_MAX - 16, Required, Length},
                 {11, 0, Required, Length},
                 {11, profile::KernelModuleBase, Required, Length},
                 {11, Buffer, Required, profile::KernelModuleBase},
                 {12, Buffer, Required, Length}}) {
    EXPECT_FALSE(
        requireError(Model->call("ZwQuerySystemInformation", A)).empty());
    check(Memory->read(Scratch, After));
    EXPECT_EQ(After, Before);
  }
  const uint64_t Freed = invoke("ExAllocatePool", {0, Required});
  invoke("ExFreePool", {Freed});
  for (auto A : {std::array<uint64_t, 4>{11, Freed, Required, Length},
                 {11, Buffer, Required, Freed}})
    EXPECT_NE(
        requireError(Model->call("ZwQuerySystemInformation", A)).find("freed"),
        std::string::npos);
  check(Memory->read(Scratch, After));
  EXPECT_EQ(After, Before);
}

TEST_F(KernelExportLookup, ModuleQueryPreservesPreviousModeAndPassiveIRQL) {
  EXPECT_NE(
      requireError(Model->call("NtQuerySystemInformation", {11, 0, 0, Scratch}))
          .find("unavailable"),
      std::string::npos);
  EXPECT_NE(requireError(Model->call("ZwQuerySystemInformation", {11, 0, 0}))
                .find("argument"),
            std::string::npos);
  Model->enterExecution(profile::StackBase);
  check(Model->setUserRequestContext(true));
  EXPECT_NE(
      requireError(Model->call("NtQuerySystemInformation", {11, 0, 0, Scratch}))
          .find("probing"),
      std::string::npos);
  EXPECT_EQ(invoke("ZwQuerySystemInformation", {11, 0, 0, Scratch}),
            0xc0000004u);
  EXPECT_EQ(invoke("ExGetPreviousMode", {}), 1u);
  check(Model->setUserRequestContext(false));
  EXPECT_EQ(invoke("NtQuerySystemInformation", {11, 0, 0, Scratch}),
            0xc0000004u);
  invoke("KfRaiseIrql", {1});
  for (const char *Name :
       {"NtQuerySystemInformation", "ZwQuerySystemInformation"})
    EXPECT_NE(requireError(Model->call(Name, {11, 0, 0, Scratch}))
                  .find("PASSIVE_LEVEL"),
              std::string::npos);
}

TEST_F(KernelExportLookup, ModuleMetadataIsReadOnlyAndFunctionBodiesAreOpaque) {
  for (const char *Name : {"ntoskrnl.exe", "hal.dll"}) {
    const uint64_t Base = *KernelExportRegistry::moduleBase(Name);
    auto Readable = Memory->canAccess(Base, 4096, Read);
    ASSERT_TRUE(bool(Readable)) << llvm::toString(Readable.takeError());
    EXPECT_TRUE(*Readable);
    for (unsigned Permissions : {unsigned(Write), unsigned(Execute)}) {
      auto Allowed = Memory->canAccess(Base, 4096, Permissions);
      ASSERT_TRUE(bool(Allowed)) << llvm::toString(Allowed.takeError());
      EXPECT_FALSE(*Allowed);
    }
    check(Model->validateGuestAccess(Base, 4096, false));
    check(Model->validateGuestAccess(Base + profile::KernelModuleExportRVA,
                                     4096, false));
    EXPECT_NE(
        llvm::toString(Model->validateGuestAccess(
                           Base + profile::KernelModuleCodeRVA - 1, 2, false))
            .find("thunks"),
        std::string::npos);
    EXPECT_NE(
        llvm::toString(Model->validateGuestAccess(
                           Base + profile::KernelModuleExportRVA - 1, 2, true))
            .find("thunks"),
        std::string::npos);
    // API implementations obey the same opacity contract as CPU reads.
    EXPECT_NE(
        requireError(
            Model->call("RtlCopyMemory",
                        {Scratch, Base + profile::KernelModuleCodeRVA, 1}))
            .find("thunks"),
        std::string::npos);
  }
}

TEST_F(KernelExportLookup, ModuleQueryRetainsBorrowedRecoveryDependencies) {
  check(Model->captureUnpackBaseline());
  const auto HasDependencies = [&]() {
    auto Value = Model->unpackDependencies();
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return true;
    }
    return !Value->empty();
  };
  EXPECT_FALSE(HasDependencies());
  EXPECT_EQ(invoke("ZwQuerySystemInformation", {11, 0, 0, Scratch}),
            0xc0000004u);
  EXPECT_FALSE(HasDependencies());
  const uint64_t Required = requireValue(Memory->readInteger(Scratch, 4));
  const uint64_t Buffer = invoke("ExAllocatePool", {0, Required});
  EXPECT_EQ(invoke("ZwQuerySystemInformation", {11, Buffer, Required, 0}), 0u);
  invoke("ExFreePool", {Buffer});
  EXPECT_TRUE(HasDependencies());
  const auto Borrowed = Model->unpackAllocations();
  EXPECT_TRUE(
      Borrowed.count(*KernelExportRegistry::moduleBase("ntoskrnl.exe")));
  EXPECT_TRUE(Borrowed.count(*KernelExportRegistry::moduleBase("hal.dll")));
  EXPECT_FALSE(Borrowed.count(0x180000000));
  const auto Contains = [&](uint64_t Pointer) {
    for (const auto &[Base, Size] : Borrowed)
      if (Pointer >= Base && Pointer - Base < Size)
        return true;
    return false;
  };
  for (const auto &[Address, Export] : Exports.entries()) {
    EXPECT_FALSE(Contains(Address));
    EXPECT_TRUE(Contains(Address + 1));
  }
}

} // namespace
} // namespace neverd::emulation
