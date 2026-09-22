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

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelExportRegistry.h"
#include "windows/KernelModel.h"

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
  EXPECT_GE(Present, profile::ThunkBase);
  EXPECT_LT(Present,
            profile::ThunkBase + profile::ThunkSize - profile::ThunkStride);
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
  for (const char *Module : {"", "wdf01000.sys", "ntoskrnl", "hal.dll",
                             "path/ntoskrnl.exe", "ntoskrnl.exe "})
    EXPECT_FALSE(KernelExportRegistry::canonicalImportModule(Module));
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
  uint64_t Next = profile::ThunkBase;
  while (Registry.lookup(Next))
    Next += profile::ThunkStride;
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

} // namespace
} // namespace neverd::emulation
