//===- WindowsProcessTests.cpp - PE64 user execution and environment -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcess.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>

namespace neverd::emulation {
namespace {
namespace win = windows_process;
#define NEVERD_WINDOWS_FIXTURE_VALUE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#define NEVERD_WINDOWS_FIXTURE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsProcessCases.def"
#undef NEVERD_WINDOWS_FIXTURE_TEXT
#undef NEVERD_WINDOWS_FIXTURE_VALUE
#define NEVERD_WINDOWS_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_WINDOWS_TEST_WIDE(Name, Text) constexpr char16_t Name[] = Text;
#define NEVERD_WINDOWS_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "WindowsProcessTestData.def"
#undef NEVERD_WINDOWS_TEST_VALUE
#undef NEVERD_WINDOWS_TEST_WIDE
#undef NEVERD_WINDOWS_TEST_TEXT
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  const char *File;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA, ISA##File},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class WindowsProcess : public testing::TestWithParam<Profile> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_PROCESS_FIXTURE_DIR
    GTEST_SKIP() << MissingTools;
#else
    Path = std::filesystem::path(NEVERD_WINDOWS_PROCESS_FIXTURE_DIR) /
           GetParam().File;
    ExecutionConfiguration Configuration;
    Configuration.Backend = GetParam().Backend;
    Configuration.Architecture = GetParam().ISA;
    Configuration.Contract = GetParam().ISA == GuestArchitecture::X64
                                 ? ExecutionContract::CheckedUserX64
                                 : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Backend = GetParam().Backend;
    Options.Arguments = {Executable, Normal};
    Options.Environment = {Environment};
    Options.InstructionQuantum = Quantum;
    Options.Limits.Instructions = InstructionLimit;
#endif
  }
  ProcessResult run(const char *Mode = Normal) {
    Options.Arguments.back() = Mode;
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::WindowsPE64, Options));
  }
};
TEST_P(WindowsProcess, RunsPEWithStaticAndDynamicTLSHeapAndProcessEnvironment) {
  const auto R = run();
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus) << R.PC;
  EXPECT_EQ(R.StandardOutput, std::string(Message) + Detached);
  EXPECT_EQ(R.StandardError, std::string(Binary, sizeof(Binary) - 1));
  EXPECT_EQ(R.SelectedBackend, GetParam().Backend);
  EXPECT_GT(R.Instructions, Quantum);
  EXPECT_TRUE(R.Services.empty());
  ASSERT_FALSE(R.NativeCalls.empty());
  for (const auto &C : R.NativeCalls) {
    EXPECT_EQ(C.Module, win::text::Kernel32);
    if (C.Name == WriteFileName)
      EXPECT_EQ(C.ArgumentCount, 5u);
  }
  auto JSON = llvm::cantFail(llvm::json::parse(processResultJSON(R)));
  EXPECT_TRUE(JSON.getAsObject()->getObject(WindowsReport));
}
TEST_P(WindowsProcess, EntryReturnRunsDetachAndPreservesExitStatus) {
  const auto R = run(Returned);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus);
  EXPECT_EQ(R.ReturnValue, ExitStatus);
  EXPECT_EQ(R.StandardOutput, std::string(Message) + Detached);
}
TEST_P(WindowsProcess, APIErrorsDoNotPoisonTheNextValidCall) {
  const auto R = run(Errors);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus);
  EXPECT_EQ(R.StandardOutput, std::string(Message) + Detached);
}
TEST_P(WindowsProcess, SharedInstructionBudgetIncludesTLSInitialization) {
  Options.Limits.Instructions = LoopLimit;
  Options.InstructionQuantum = LoopQuantum;
  const auto R = run(InitLoop);
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
  EXPECT_EQ(R.Instructions, LoopLimit);
  EXPECT_FALSE(R.ExitStatus);
  EXPECT_TRUE(R.StandardOutput.empty());
}
TEST_P(WindowsProcess, EventLimitStopsBeforeAPIOutput) {
  Options.Limits.Events = 1;
  const auto R = run();
  EXPECT_EQ(R.Stop, ProcessStopReason::EventLimit) << R.Diagnostic;
  EXPECT_EQ(R.Events, 1u);
  EXPECT_TRUE(R.StandardOutput.empty());
}
TEST_P(WindowsProcess, OutputLimitDoesNotPublishPartialBytes) {
  Options.OutputLimit = 1;
  const auto R = run();
  EXPECT_EQ(R.Stop, ProcessStopReason::OutputLimit) << R.Diagnostic;
  EXPECT_TRUE(R.StandardOutput.empty());
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, DirectSyscallsDoNotSelectWindowsServices) {
  const auto R = run(Unknown);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, PrivilegedInstructionsCannotExecuteAsKernel) {
  const auto R = run(Privileged);
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, InvalidGuestStoresRemainCPUFaults) {
  const auto R = run(Fault);
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, UnsupportedOverlappedWritesDoNotSucceed) {
  const auto R = run(Unsupported);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_TRUE(R.StandardOutput.empty());
}
TEST_P(WindowsProcess, LiveTLSCallbackMutationsAffectAttachAndDetach) {
  auto R = run(TLSMutation);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus);
  EXPECT_EQ(R.StandardOutput, std::string(Message) + Detached);
}
TEST_P(WindowsProcess, WriteFileClearsAliasedCompletionBeforeReadingBytes) {
  auto R = run(AliasedOutput);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus);
  EXPECT_EQ(R.StandardError,
            std::string(4, '\0') + std::string(Binary, sizeof(Binary) - 1));
}
TEST_P(WindowsProcess, NonreturningTailCallDoesNotReadADeadReturnAddress) {
  auto R = run(TailExit);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus);
  EXPECT_EQ(R.StandardOutput, Detached);
}
TEST_P(WindowsProcess, InvalidAPIOutputNeedsExplicitExceptionSupport) {
  auto R = run(BadOutput);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_TRUE(R.StandardOutput.empty());
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, ForgedCallbackGateCannotTerminateAnEntryCall) {
  auto R = run(ForgedGate);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, ReentrantExitDoesNotPublishACompletedStatus) {
  auto R = run(ReentrantExit);
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, APIWritesToTheReturnSlotChangeTheActualReturn) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64ReturnSlotOnly;
  auto R = run(ReturnSlot);
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
  EXPECT_EQ(R.StandardOutput, std::string(Message, 1));
}
TEST_P(WindowsProcess, NTDLLNamedExitHasExplicitProviderEvidence) {
  auto R = run(NativeExit);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus);
  EXPECT_TRUE(std::any_of(R.NativeCalls.begin(), R.NativeCalls.end(),
                          [](const auto &C) {
                            return C.Module == win::text::NTDLL &&
                                   C.Name == NativeExitName && !C.Result;
                          }));
}
TEST_P(WindowsProcess, VirtualMemoryReserveCommitProtectDecommitAndRelease) {
  auto R = run(MemoryLifecycle);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus) << llvm::toHex(R.StandardError);
}
TEST_P(WindowsProcess, VirtualMemoryRoundingAndReservationBoundariesAreAtomic) {
  auto R = run(MemoryAlignment);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus) << llvm::toHex(R.StandardError);
}
TEST_P(WindowsProcess, VirtualQueryReportsOwnedMappingsAndValidatesOutputs) {
  auto R = run(MemoryQuery);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus) << llvm::toHex(R.StandardError);
}
TEST_P(WindowsProcess,
       VirtualMemoryExecutesRewrittenCodeAfterProtectionChanges) {
  auto R = run(MemoryCode);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus) << llvm::toHex(R.StandardError);
}
TEST_P(WindowsProcess, VirtualProtectObservesAliasedOutputPermissions) {
  auto R = run(MemoryAlias);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus) << llvm::toHex(R.StandardError);
}
TEST_P(WindowsProcess, VirtualMemoryTopDownPlacementHonorsSparseReservations) {
  auto R = run(MemoryPlacement);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus) << llvm::toHex(R.StandardError);
}
TEST_P(WindowsProcess, VirtualMemoryReclaimsBackingAcrossLiveCPUResumptions) {
  Options.MemoryLimit = VMReclaimLimit;
  Options.OutputLimit = VMPage;
  auto R = run(MemoryReclaim);
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, ExitStatus) << llvm::toHex(R.StandardError);
}
TEST_P(WindowsProcess, ReservedPagesRemainInaccessible) {
  auto R = run(MemoryReservedFault);
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, NoAccessPagesRejectGuestStores) {
  auto R = run(MemoryNoAccessFault);
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, ReadOnlyPagesRejectGuestStores) {
  auto R = run(MemoryReadOnlyFault);
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, DecommittedPagesRejectGuestStores) {
  auto R = run(MemoryDecommitFault);
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, ReleasedPagesRejectGuestStores) {
  auto R = run(MemoryReleaseFault);
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, NonExecutablePagesRejectGuestInstructions) {
  auto R = run(MemoryExecuteFault);
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
TEST_P(WindowsProcess, GuardPagesRequireExplicitExceptionSupport) {
  auto R = run(MemoryGuard);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_FALSE(R.ExitStatus);
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, WindowsProcess,
                         testing::ValuesIn(Profiles),
                         [](const auto &Info) { return Info.param.Name; });

class WindowsProcessImage : public testing::Test {
protected:
  std::filesystem::path Path, Root;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_PROCESS_FIXTURE_DIR
    GTEST_SKIP() << MissingTools;
#else
    Path = std::filesystem::path(NEVERD_WINDOWS_PROCESS_FIXTURE_DIR) / X64File;
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Directory));
    Root = Directory.str().str();
#endif
  }
  void TearDown() override {
    if (!Root.empty())
      std::filesystem::remove_all(Root);
  }
  std::string bytes() {
    auto B = llvm::MemoryBuffer::getFile(Path.string());
    EXPECT_TRUE(bool(B));
    return (*B)->getBuffer().str();
  }
  void rejects(std::string Bytes) {
    const auto Input = Root / InvalidFile;
    std::error_code EC;
    {
      llvm::raw_fd_ostream OS(Input.string(), EC);
      ASSERT_FALSE(EC);
      OS << Bytes;
    }
    auto R = win::loadImage(Input, process_defaults::Memory);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
  }
};
TEST_F(WindowsProcessImage,
       RejectsMalformedHeadersAndUnsupportedLoaderRequirements) {
  auto Bytes = bytes();
  const auto COFF =
      llvm::support::endian::read32le(Bytes.data() + DOSNewHeaderOffset) + 4;
  const auto Optional = COFF + sizeof(llvm::object::coff_file_header);
  for (auto [Offset, Value] :
       {std::pair{size_t(COFF), uint16_t(llvm::COFF::IMAGE_FILE_MACHINE_I386)},
        std::pair{size_t(Optional), uint16_t(llvm::COFF::PE32Header::PE32)},
        std::pair{size_t(Optional +
                         offsetof(llvm::object::pe32plus_header, Subsystem)),
                  uint16_t(llvm::COFF::IMAGE_SUBSYSTEM_NATIVE)}}) {
    auto Copy = Bytes;
    llvm::support::endian::write16le(Copy.data() + Offset, Value);
    rejects(Copy);
  }
  auto Copy = Bytes;
  const auto Directory =
      Optional + sizeof(llvm::object::pe32plus_header) +
      llvm::COFF::LOAD_CONFIG_TABLE * sizeof(llvm::object::data_directory);
  llvm::support::endian::write32le(Copy.data() + Directory, InvalidConfigRVA);
  llvm::support::endian::write32le(Copy.data() + Directory + 4,
                                   InvalidConfigSize);
  rejects(Copy);
  for (size_t Length :
       {size_t(0), size_t(64), size_t(Optional), Bytes.size() / 2})
    rejects(Bytes.substr(0, Length));
  Copy = Bytes;
  llvm::support::endian::write64le(
      Copy.data() + Optional +
          offsetof(llvm::object::pe32plus_header, ImageBase),
      win::value::UserLimit);
  rejects(Copy);
}
TEST_F(WindowsProcessImage, RejectsImportAndTLSMetadataBeforeExecution) {
  const auto Original = bytes();
  auto Object = llvm::cantFail(llvm::object::COFFObjectFile::create(
      llvm::MemoryBufferRef(Original, Executable)));
  auto Offset = [&](uint64_t RVA) -> uint64_t {
    for (const auto &Reference : Object->sections()) {
      const auto *S = Object->getCOFFSection(Reference);
      if (RVA >= S->VirtualAddress &&
          RVA - S->VirtualAddress < S->SizeOfRawData)
        return S->PointerToRawData + RVA - S->VirtualAddress;
    }
    llvm_unreachable(InvalidOriginalRVA);
  };
  const auto TLS = Offset(
      Object->getDataDirectory(llvm::COFF::TLS_TABLE)->RelativeVirtualAddress);
  const auto Imports = Offset(Object->getDataDirectory(llvm::COFF::IMPORT_TABLE)
                                  ->RelativeVirtualAddress);
  const auto Base = Object->getPE32PlusHeader()->ImageBase;
  auto Mutate64 = [&](uint64_t At, uint64_t Value) {
    auto Copy = Original;
    llvm::support::endian::write64le(Copy.data() + At, Value);
    rejects(std::move(Copy));
  };
  using TLSHeader = llvm::object::coff_tls_directory64;
  Mutate64(TLS + offsetof(TLSHeader, StartAddressOfRawData), UINT64_MAX);
  Mutate64(TLS + offsetof(TLSHeader, AddressOfIndex), Base + 1);
  Mutate64(TLS + offsetof(TLSHeader, AddressOfCallBacks), UINT64_MAX);
  const auto CallbackArray = llvm::support::endian::read64le(
      Original.data() + TLS + offsetof(TLSHeader, AddressOfCallBacks));
  Mutate64(Offset(CallbackArray - Base), Base);
  using ImportHeader = llvm::object::coff_import_directory_table_entry;
  const auto IAT = llvm::support::endian::read32le(
      Original.data() + Imports +
      offsetof(ImportHeader, ImportAddressTableRVA));
  Mutate64(Offset(IAT), UINT64_MAX);
  auto Copy = Original;
  auto Name = Copy.find(ExitName);
  ASSERT_NE(Name, std::string::npos);
  Copy[Name] = 'Q';
  rejects(Copy);
}
TEST_F(WindowsProcessImage, RejectsUnknownDependenciesWithEmptyImportTables) {
  auto Bytes = bytes();
  auto Object = llvm::cantFail(llvm::object::COFFObjectFile::create(
      llvm::MemoryBufferRef(Bytes, Executable)));
  auto Offset = [&](uint64_t RVA) -> uint64_t {
    for (const auto &Reference : Object->sections()) {
      const auto *S = Object->getCOFFSection(Reference);
      if (RVA >= S->VirtualAddress &&
          RVA - S->VirtualAddress < S->SizeOfRawData)
        return S->PointerToRawData + RVA - S->VirtualAddress;
    }
    llvm_unreachable(InvalidOriginalRVA);
  };
  llvm::object::coff_import_directory_table_entry Import;
  std::memcpy(&Import,
              Bytes.data() +
                  Offset(Object->getDataDirectory(llvm::COFF::IMPORT_TABLE)
                             ->RelativeVirtualAddress),
              sizeof(Import));
  ASSERT_NE(Import.ImportLookupTableRVA, 0u);
  Bytes[Offset(Import.NameRVA)] = 'Q';
  llvm::support::endian::write64le(
      Bytes.data() + Offset(Import.ImportLookupTableRVA), 0);
  llvm::support::endian::write64le(
      Bytes.data() + Offset(Import.ImportAddressTableRVA), 0);
  rejects(Bytes);
}
TEST_F(WindowsProcessImage, RejectsInconsistentExtendedDLLCharacteristics) {
  auto Bytes = bytes();
  auto Object = llvm::cantFail(llvm::object::COFFObjectFile::create(
      llvm::MemoryBufferRef(Bytes, Executable)));
  using DebugRecord = llvm::object::debug_directory;
  const uint64_t Extra = sizeof(DebugRecord) + 2 * sizeof(uint32_t);
  const llvm::object::coff_section *Section = nullptr;
  for (const auto &Reference : Object->sections()) {
    const auto *S = Object->getCOFFSection(Reference);
    if (S->SizeOfRawData >= S->VirtualSize &&
        S->SizeOfRawData - S->VirtualSize >= Extra) {
      Section = S;
      break;
    }
  }
  ASSERT_NE(Section, nullptr);
  const uint64_t Offset = Section->PointerToRawData + Section->VirtualSize;
  const uint64_t RVA = Section->VirtualAddress + Section->VirtualSize;
  const uint64_t SectionOffset =
      reinterpret_cast<const char *>(Section) - Bytes.data();
  auto *Directory = Object->getDataDirectory(llvm::COFF::DEBUG_DIRECTORY);
  const uint64_t DirectoryOffset =
      reinterpret_cast<const char *>(Directory) - Bytes.data();
  llvm::support::endian::write32le(
      Bytes.data() + SectionOffset +
          offsetof(llvm::object::coff_section, VirtualSize),
      Section->VirtualSize + Extra);
  llvm::object::data_directory Entry{};
  Entry.RelativeVirtualAddress = RVA;
  Entry.Size = sizeof(DebugRecord);
  std::memcpy(Bytes.data() + DirectoryOffset, &Entry, sizeof(Entry));
  DebugRecord Debug{};
  Debug.Type = llvm::COFF::IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS;
  Debug.SizeOfData = sizeof(uint32_t);
  Debug.AddressOfRawData = RVA + sizeof(Debug);
  Debug.PointerToRawData = Offset + sizeof(Debug);
  std::fill_n(Bytes.data() + Offset, Extra, 0);
  std::memcpy(Bytes.data() + Offset, &Debug, sizeof(Debug));
  const auto Input = Root / InvalidFile;
  auto Accepts = [&](llvm::StringRef Data) {
    std::error_code EC;
    {
      llvm::raw_fd_ostream OS(Input.string(), EC);
      ASSERT_FALSE(EC);
      OS << Data;
    }
    auto Valid = win::loadImage(Input, process_defaults::Memory);
    ASSERT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
  };
  Accepts(Bytes);
  auto Unmapped = Bytes;
  auto FileOnly = Debug;
  FileOnly.AddressOfRawData = 0;
  FileOnly.PointerToRawData = Unmapped.size();
  std::memcpy(Unmapped.data() + Offset, &FileOnly, sizeof(FileOnly));
  Unmapped.append(sizeof(uint32_t), '\0');
  Accepts(Unmapped);
  auto Copy = Bytes;
  llvm::support::endian::write32le(Copy.data() + Debug.PointerToRawData,
                                   ExtendedCET);
  rejects(Copy);
  Debug.AddressOfRawData += sizeof(uint32_t);
  std::memcpy(Bytes.data() + Offset, &Debug, sizeof(Debug));
  // Matching zero bytes cannot legitimize contradictory file/RVA identities.
  rejects(Bytes);
  llvm::support::endian::write32le(
      Bytes.data() + Debug.PointerToRawData + sizeof(uint32_t), ExtendedCET);
  rejects(Bytes);
  Debug.AddressOfRawData = UINT32_MAX;
  std::memcpy(Bytes.data() + Offset, &Debug, sizeof(Debug));
  rejects(Bytes);
}
TEST_F(WindowsProcessImage, BuildsUTF16CommandLineEnvironmentAndLoaderLists) {
  const auto Image =
      llvm::cantFail(win::loadImage(Path, process_defaults::Memory));
  auto Physical =
      llvm::cantFail(PhysicalMemory::create(process_defaults::Memory));
  auto Space =
      llvm::cantFail(AddressSpace::create(Physical, process_defaults::Memory));
  for (const auto &R : Image.Regions) {
    llvm::cantFail(
        Space->map(R.Address, R.Bytes.size(), Read | Write | UserAccessible));
    llvm::cantFail(Space->write(R.Address, R.Bytes));
  }
  ProcessOptions O;
  O.Arguments = {Executable, Empty, SpacedArgument, QuotedArgument,
                 UnicodeArgument};
  O.Environment = {UnicodeEnvironment, SortedEnvironment};
  auto Env =
      llvm::cantFail(win::prepareEnvironment(*Space, Image, O, Executable));
  auto String = [&](uint64_t Address, uint64_t Count) {
    std::u16string Text;
    for (uint64_t I = 0; I < Count; ++I)
      Text += char16_t(llvm::cantFail(Space->readInteger(Address + I * 2, 2)));
    return Text;
  };
  const std::u16string Expected = ExpectedCommandLine;
  EXPECT_EQ(String(Env.CommandLine, Expected.size() + 1), Expected + u'\0');
  const auto EnvironmentAddress = llvm::cantFail(Space->readInteger(
      win::value::Parameters + win::value::ParamsEnvironment, 8));
  const auto &ExpectedEnvironment = ExpectedEnvironmentBlock;
  EXPECT_EQ(
      String(EnvironmentAddress, std::size(ExpectedEnvironment)),
      std::u16string(ExpectedEnvironment, std::size(ExpectedEnvironment)));
  EXPECT_EQ(llvm::cantFail(Space->readInteger(
                win::value::Ldr + win::value::LdrMemoryList, 8)),
            win::value::ModuleEntry + win::value::ModuleMemoryLink);
}
TEST_F(WindowsProcessImage,
       RejectsAmbiguousEnvironmentAndInvalidUnicodeBeforeCPUEntry) {
  for (const auto &Environment :
       {std::vector<std::string>{DuplicateFirst, DuplicateSecond},
        std::vector<std::string>{MissingEquals},
        std::vector<std::string>{InvalidUTF8}}) {
    ProcessOptions O;
    O.Environment = Environment;
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, O);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
  }
}
TEST_F(WindowsProcessImage, NativeWindowsOracleRunsTheSameExecutable) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeWindowsOnly;
#else
  const auto Program = (Root / NativeFile).string();
  const auto EC = llvm::sys::fs::copy_file(Path.string(), Program);
  ASSERT_FALSE(EC) << EC.message();
  for (const char *Mode :
       {Normal, Returned, Errors, AliasedOutput, TLSMutation, TailExit,
        NativeExit, MemoryLifecycle, MemoryAlignment, MemoryQuery, MemoryCode,
        MemoryAlias, MemoryPlacement, MemoryReclaim}) {
    SCOPED_TRACE(Mode);
    const auto Output = (Root / StdoutFile).string();
    const auto Error = (Root / StderrFile).string();
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                        Error};
    std::string LaunchError;
    bool ExecutionFailed = false;
    const auto Status = llvm::sys::ExecuteAndWait(
        Program, {Program, Mode}, std::nullopt, Redirects, NativeTimeoutSeconds,
        0, &LaunchError, &ExecutionFailed);
    ASSERT_FALSE(ExecutionFailed) << LaunchError;
    const auto Out = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Out));
    const auto Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Err));
    EXPECT_EQ(Status, ExitStatus)
        << LaunchError << llvm::toHex((*Err)->getBuffer());
    if (Status != ExitStatus)
      continue;
    EXPECT_EQ((*Out)->getBuffer(),
              std::string(Mode == TailExit ? Empty : Message) + Detached);
    EXPECT_EQ((*Err)->getBuffer(),
              Mode == TailExit
                  ? std::string()
                  : std::string(Mode == AliasedOutput ? 4 : 0, '\0') +
                        std::string(Binary, sizeof(Binary) - 1));
  }
#endif
}
} // namespace
} // namespace neverd::emulation
