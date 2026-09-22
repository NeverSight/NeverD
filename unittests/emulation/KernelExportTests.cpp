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
