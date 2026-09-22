//===- DriverSessionTests.cpp - Original bounded-driver fixtures ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original bounded-driver fixtures.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/DriverImage.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace neverd::emulation {
namespace {
std::filesystem::path fixture(const char *Name) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}

const DriverAPIEvent *findCall(const DriverResult &Result, const char *Name) {
  auto Found =
      std::find_if(Result.Calls.begin(), Result.Calls.end(),
                   [&](const auto &Call) { return Call.Name == Name; });
  return Found == Result.Calls.end() ? nullptr : &*Found;
}

TEST(DriverSession, RejectsPendingInitializationBeforeDeliveringScenario) {
  DriverOptions Options;
  Options.Requests.push_back({DriverRequestKind::Create});
  Options.Unload = true;
  auto Result = emulateDriver(fixture("pendingentry"), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_EQ(Result->NTStatus, 0x103u);
  EXPECT_TRUE(Result->Requests.empty());
  EXPECT_FALSE(Result->UnloadCompleted);
  EXPECT_NE(Result->Diagnostic.find("DriverEntry must return STATUS_SUCCESS"),
            std::string::npos);
}

TEST(DriverEmulation, ReturnsSuccessAndObservesDriverObjectWrites) {
  auto Result = emulateDriver(fixture("success"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus);
  EXPECT_EQ(*Result->NTStatus, 0u);
  EXPECT_GT(Result->Instructions, 0u);
  EXPECT_EQ(Result->ImageBase, 0x180000000ULL);
  EXPECT_NE(Result->DriverObject, 0u);
  EXPECT_GE(Result->DriverUnload, Result->ImageBase);
  EXPECT_GE(Result->MajorFunctions[0], Result->ImageBase);
  EXPECT_FALSE(Result->Writes.empty());
}

TEST(DriverEmulation, PreservesFailingNTStatusAsNormalReturn) {
  auto Result = emulateDriver(fixture("failure"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus);
  EXPECT_EQ(*Result->NTStatus, 0xc0000001u);
}

TEST(DriverEmulation, UnsupportedImportStopsWithoutInventingSuccess) {
  auto Result = emulateDriver(fixture("unknown"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedAPI)
      << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
  EXPECT_NE(Result->Diagnostic.find("ZwOpenFile"), std::string::npos);
}

TEST(DriverEmulation, InvalidGuestMemoryStopsWithObservations) {
  auto Result = emulateDriver(fixture("fault"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault) << Result->Diagnostic;
  EXPECT_GT(Result->Instructions, 0u);
  EXPECT_FALSE(Result->NTStatus);
  ASSERT_TRUE(Result->Fault);
  EXPECT_EQ(Result->Fault->Kind, "unmapped_memory");
  EXPECT_EQ(Result->Fault->Address, 0x12345000ULL);
  EXPECT_EQ(Result->Fault->Size, 8u);
  EXPECT_EQ(Result->Fault->Access, "write");
  EXPECT_EQ(Result->Fault->PC, Result->PC);
}

TEST(DriverEmulation, SectionWriteProtectionIsEnforced) {
  auto Result = emulateDriver(fixture("readonly"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault) << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, UnknownCPUEnvironmentDoesNotSilentlyExecuteCLI) {
  auto Result = emulateDriver(fixture("privileged"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedInstruction)
      << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, ReadingOpaqueDriverSectionStopsInsteadOfInventingState) {
  auto Result = emulateDriver(fixture("opaque"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, StackPivotIntoDriverObjectStopsBeforeAPIDispatch) {
  auto Result = emulateDriver(fixture("opaqueargument"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(
      Result->Diagnostic.find("stack pointer exceeds the invocation stack"),
      std::string::npos);
  EXPECT_FALSE(Result->Fault);
  EXPECT_TRUE(Result->Calls.empty());
}

TEST(DriverEmulation, UnsupportedGSAccessStopsBeforeReadingUnknownThreadState) {
  auto Result = emulateDriver(fixture("gs"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedInstruction)
      << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, KernelAPIPointersUseCheckedGuestMemory) {
  auto Result = emulateDriver(fixture("apifault"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault) << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, InfiniteLoopExhaustsExactInstructionBudget) {
  DriverOptions Options;
  Options.InstructionLimit = 50;
  auto Result = emulateDriver(fixture("loop"), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::InstructionLimit)
      << Result->Diagnostic;
  EXPECT_EQ(Result->Instructions, 50u);
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, ModelsStringsPoolAndDeviceCreation) {
  auto Result = emulateDriver(fixture("devices"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus);
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Devices.size(), 1u);
  EXPECT_EQ(Result->Devices[0].Name, "\\Device\\NeverDTest");
  EXPECT_EQ(Result->Devices[0].Type, 0x22u);
  EXPECT_NE(Result->Devices[0].Address, 0u);
  EXPECT_NE(Result->Devices[0].Extension, 0u);
  EXPECT_GE(Result->DriverUnload, Result->ImageBase);
  EXPECT_EQ(Result->MajorFunctions[0], Result->MajorFunctions[14]);
  EXPECT_GE(Result->MajorFunctions[14], Result->ImageBase);
  EXPECT_NE(findCall(*Result, "RtlInitUnicodeString"), nullptr);
  EXPECT_NE(findCall(*Result, "ExFreePoolWithTag"), nullptr);
  EXPECT_NE(findCall(*Result, "IoCreateSymbolicLink"), nullptr);
  const auto *Allocation = findCall(*Result, "ExAllocatePoolWithTag");
  ASSERT_NE(Allocation, nullptr);
  ASSERT_TRUE(Allocation->Result);
  EXPECT_NE(*Allocation->Result, 0u);
  EXPECT_TRUE(std::any_of(Result->Writes.begin(), Result->Writes.end(),
                          [&](const auto &Write) {
                            return Write.Address == *Allocation->Result &&
                                   Write.Value == 0x1122334455667788ULL;
                          }));
}

TEST(DriverEmulation, ObservationBudgetStopsExecution) {
  DriverOptions Options;
  Options.EventLimit = 1;
  auto Result = emulateDriver(fixture("devices"), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::EventLimit) << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, WallClockDeadlineStopsWithNoInstructionBudgetExhaustion) {
  DriverOptions Options;
  Options.InstructionLimit = UINT64_MAX;
  Options.EventLimit = 1000000;
  Options.TimeoutMilliseconds = 1;
  auto Result = emulateDriver(fixture("loop"), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Timeout) << Result->Diagnostic;
  EXPECT_LT(Result->Instructions, Options.InstructionLimit);
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, InvalidResourceLimitsAreRequestErrors) {
  for (unsigned Case = 0; Case < 7; ++Case) {
    SCOPED_TRACE(Case);
    DriverOptions Options;
    switch (Case) {
    case 0:
      Options.InstructionLimit = 0;
      break;
    case 1:
      Options.MemoryLimit = 0;
      break;
    case 2:
      Options.EventLimit = 0;
      break;
    case 3:
      Options.TimeoutMilliseconds = 0;
      break;
    case 4:
      Options.MemoryLimit = UINT64_MAX;
      break;
    case 5:
      Options.EventLimit = UINT64_MAX;
      break;
    case 6:
      Options.TimeoutMilliseconds = UINT64_MAX;
      break;
    }
    auto Result = emulateDriver(fixture("success"), Options);
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_FALSE(llvm::toString(Result.takeError()).empty());
  }
}

TEST(DriverEmulation, RegistryServiceNameCannotInjectAPath) {
  DriverOptions Options;
  Options.ServiceName = "..\\OtherService";
  auto Result = emulateDriver(fixture("success"), Options);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("service name"),
            std::string::npos);
}

TEST(DriverEmulation, MemoryBudgetIncludesStackAndKernelEnvironment) {
  DriverOptions Options;
  Options.MemoryLimit = 1024 * 1024;
  auto Result = emulateDriver(fixture("success"), Options);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("memory limit"),
            std::string::npos);
}

class DriverImageValidation : public ::testing::Test {
protected:
  std::vector<uint8_t> Bytes;
  std::unique_ptr<llvm::object::COFFObjectFile> Object;
  std::filesystem::path Directory;

  void SetUp() override {
    auto Buffer = llvm::MemoryBuffer::getFile(fixture("devices").string());
    ASSERT_TRUE(static_cast<bool>(Buffer));
    const auto Data = (*Buffer)->getBuffer();
    Bytes.assign(Data.bytes_begin(), Data.bytes_end());
    llvm::MemoryBufferRef Reference(
        llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                        Bytes.size()),
        "driver-image-test");
    auto Parsed = llvm::object::COFFObjectFile::create(Reference);
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError());
    Object = std::move(*Parsed);
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-driver-test", Temporary));
    Directory = Temporary.str().str();
  }

  void TearDown() override {
    std::error_code Ignored;
    std::filesystem::remove_all(Directory, Ignored);
  }

  size_t offset(const void *Address) const {
    return static_cast<const uint8_t *>(Address) - Bytes.data();
  }

  void put16(const void *Address, uint16_t Value) {
    llvm::support::endian::write16le(Bytes.data() + offset(Address), Value);
  }

  void put32(const void *Address, uint32_t Value) {
    llvm::support::endian::write32le(Bytes.data() + offset(Address), Value);
  }

  void put64(const void *Address, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + offset(Address), Value);
  }

  std::filesystem::path writeModified() {
    const auto Path = Directory / "invalid.sys";
    std::ofstream Output(Path, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Output.close();
    EXPECT_TRUE(Output);
    return Path;
  }

  // Append a real DIR64 block to existing file padding, leaving all original
  // imports and code intact. The target is the first eight bytes of that data
  // section; at the preferred base the loader must not alter those bytes.
  struct RelocationFixture {
    size_t BlockOffset;
    uint64_t TargetAddress;
    std::array<uint8_t, 8> TargetBytes;
  };

  std::optional<RelocationFixture> addBaseRelocation() {
    const auto *Directory =
        Object->getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE);
    if (!Directory)
      return std::nullopt;
    for (const auto &Reference : Object->sections()) {
      const auto *Section = Object->getCOFFSection(Reference);
      const uint64_t Offset =
          (uint64_t(Section->VirtualSize) + 3) & ~uint64_t(3);
      if ((Section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) ||
          Section->VirtualSize < 8 || Offset + 12 > Section->SizeOfRawData)
        continue;
      const size_t Raw = Section->PointerToRawData;
      const size_t Block = Raw + Offset;
      const uint32_t RVA = Section->VirtualAddress;
      llvm::support::endian::write32le(Bytes.data() + Block, RVA & ~4095u);
      llvm::support::endian::write32le(Bytes.data() + Block + 4, 12);
      llvm::support::endian::write16le(
          Bytes.data() + Block + 8,
          (llvm::COFF::IMAGE_REL_BASED_DIR64 << 12) | (RVA & 4095));
      llvm::support::endian::write16le(Bytes.data() + Block + 10, 0);
      put32(&Section->VirtualSize, Offset + 12);
      put32(&Directory->RelativeVirtualAddress, RVA + Offset);
      put32(&Directory->Size, 12);
      put16(&Object->getCOFFHeader()->Characteristics,
            Object->getCharacteristics() &
                ~llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED);
      RelocationFixture Result{Block, Object->getImageBase() + RVA, {}};
      std::copy_n(Bytes.begin() + Raw, 8, Result.TargetBytes.begin());
      return Result;
    }
    return std::nullopt;
  }

  void rejects() {
    const auto Path = writeModified();
    auto Image = loadDriverImage(Path, 64 * 1024 * 1024);
    ASSERT_FALSE(static_cast<bool>(Image));
    const std::string Message = llvm::toString(Image.takeError());
    EXPECT_FALSE(Message.empty());
  }
};

TEST_F(DriverImageValidation, ExecuteOnlyCodeDoesNotRequireDataReadPermission) {
  bool Changed = false;
  for (const auto &Reference : Object->sections()) {
    const auto *Section = Object->getCOFFSection(Reference);
    if (Section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) {
      put32(&Section->Characteristics,
            Section->Characteristics & ~(llvm::COFF::IMAGE_SCN_MEM_READ |
                                         llvm::COFF::IMAGE_SCN_MEM_WRITE));
      Changed = true;
    }
  }
  ASSERT_TRUE(Changed);
  auto Result = emulateDriver(writeModified());
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus);
  EXPECT_EQ(*Result->NTStatus, 0u);
  EXPECT_EQ(Result->Devices.size(), 1u);
}

TEST_F(DriverImageValidation, RejectsNonNativeSubsystem) {
  put16(&Object->getPE32PlusHeader()->Subsystem,
        llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI);
  rejects();
}

TEST_F(DriverImageValidation, RejectsTruncatedRawSection) {
  Bytes.pop_back();
  rejects();
}

TEST_F(DriverImageValidation, RejectsEntryInHeaders) {
  put32(&Object->getPE32PlusHeader()->AddressOfEntryPoint, 0x100);
  rejects();
}

TEST_F(DriverImageValidation, RejectsOverlappingVirtualSections) {
  auto Section = Object->section_begin();
  const auto FirstRVA = Object->getCOFFSection(*Section)->VirtualAddress;
  ++Section;
  ASSERT_NE(Section, Object->section_end());
  put32(&Object->getCOFFSection(*Section)->VirtualAddress, FirstRVA);
  rejects();
}

TEST_F(DriverImageValidation, RejectsReservedKernelModelAddressRange) {
  put64(&Object->getPE32PlusHeader()->ImageBase, 0x70000000);
  rejects();
}

TEST_F(DriverImageValidation, PreferredBasePreservesDIR64TargetBytes) {
  const auto Relocation = addBaseRelocation();
  ASSERT_TRUE(Relocation);
  auto Image = loadDriverImage(writeModified(), 64 * 1024 * 1024);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const auto &Region : Image->Regions) {
    if (Region.Address != Relocation->TargetAddress)
      continue;
    ASSERT_GE(Region.Bytes.size(), 8u);
    EXPECT_TRUE(std::equal(Relocation->TargetBytes.begin(),
                           Relocation->TargetBytes.end(),
                           Region.Bytes.begin()));
    return;
  }
  FAIL() << "relocation target region was not mapped";
}

TEST_F(DriverImageValidation, RejectsOversizedBaseRelocationBlock) {
  const auto Relocation = addBaseRelocation();
  ASSERT_TRUE(Relocation);
  llvm::support::endian::write32le(Bytes.data() + Relocation->BlockOffset + 4,
                                   0x10000000);
  rejects();
}

TEST_F(DriverImageValidation, RejectsNonX64BaseRelocationType) {
  const auto Relocation = addBaseRelocation();
  ASSERT_TRUE(Relocation);
  llvm::support::endian::write16le(Bytes.data() + Relocation->BlockOffset + 8,
                                   llvm::COFF::IMAGE_REL_BASED_HIGHLOW << 12);
  rejects();
}

TEST_F(DriverImageValidation, RejectsImportDirectoryWithoutNullDescriptor) {
  const auto *Directory = Object->getDataDirectory(llvm::COFF::IMPORT_TABLE);
  ASSERT_NE(Directory, nullptr);
  ASSERT_NE(uint32_t(Directory->RelativeVirtualAddress), 0u);
  put32(&Directory->Size,
        sizeof(llvm::object::coff_import_directory_table_entry));
  rejects();
}

TEST_F(DriverImageValidation, RejectsUnmappedImportLookupWithoutIteratingIt) {
  const auto *Directory = Object->getDataDirectory(llvm::COFF::IMPORT_TABLE);
  ASSERT_NE(Directory, nullptr);
  uintptr_t Address = 0;
  auto Error = Object->getRvaPtr(Directory->RelativeVirtualAddress, Address);
  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  const auto *Descriptor =
      reinterpret_cast<const llvm::object::coff_import_directory_table_entry *>(
          Address);
  put32(&Descriptor->ImportLookupTableRVA, 0xfffff000);
  rejects();
}

TEST_F(DriverImageValidation, RejectsNonKernelImportProvider) {
  const auto *Directory = Object->getDataDirectory(llvm::COFF::IMPORT_TABLE);
  ASSERT_NE(Directory, nullptr);
  uintptr_t Address = 0;
  auto Error = Object->getRvaPtr(Directory->RelativeVirtualAddress, Address);
  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  const auto *Descriptor =
      reinterpret_cast<const llvm::object::coff_import_directory_table_entry *>(
          Address);
  Error = Object->getRvaPtr(Descriptor->NameRVA, Address);
  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  ASSERT_EQ(std::string(reinterpret_cast<const char *>(Address)),
            "ntoskrnl.exe");
  const char OtherProvider[] = "user32xx.dll";
  std::copy_n(OtherProvider, sizeof(OtherProvider),
              Bytes.data() + offset(reinterpret_cast<const void *>(Address)));
  rejects();
}

TEST_F(DriverImageValidation, RejectsInvalidLoadConfigurationSize) {
  const auto *Directory =
      Object->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
  ASSERT_NE(Directory, nullptr);
  for (const auto &Reference : Object->sections()) {
    const auto *Section = Object->getCOFFSection(Reference);
    if (Section->SizeOfRawData >= 8 &&
        !(Section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE)) {
      // Size zero avoids requiring any cookie layout during LLVM's parse. The
      // execution profile still rejects presence of the directory itself.
      llvm::support::endian::write32le(Bytes.data() + Section->PointerToRawData,
                                       0);
      put32(&Directory->RelativeVirtualAddress, Section->VirtualAddress);
      put32(&Directory->Size, 8);
      rejects();
      return;
    }
  }
  FAIL() << "fixture needs a file-backed nonexecutable section";
}

TEST(DriverImage, MapsVirtualZeroFillAndOriginalPermissions) {
  auto Image = loadDriverImage(fixture("success"), 64 * 1024 * 1024);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Base, 0x180000000ULL);
  ASSERT_FALSE(Image->Regions.empty());
  EXPECT_EQ(Image->Regions.front().Address, Image->Base);
  EXPECT_EQ(Image->Regions.front().Permissions, 1u);
  bool HasExecutable = false;
  bool HasWritableZeroFill = false;
  for (const auto &Region : Image->Regions) {
    EXPECT_EQ(Region.Address % 4096, 0u);
    EXPECT_EQ(Region.Bytes.size() % 4096, 0u);
    HasExecutable |= (Region.Permissions & 4) != 0;
    if ((Region.Permissions & 2) &&
        std::all_of(Region.Bytes.begin(), Region.Bytes.end(),
                    [](uint8_t Byte) { return Byte == 0; }))
      HasWritableZeroFill = true;
  }
  EXPECT_TRUE(HasExecutable);
  EXPECT_TRUE(HasWritableZeroFill);
}
} // namespace
} // namespace neverd::emulation
