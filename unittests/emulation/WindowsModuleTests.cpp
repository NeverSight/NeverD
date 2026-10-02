//===- WindowsModuleTests.cpp - Original PE startup graph and admission ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessModules.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

namespace neverd::emulation {
namespace {
namespace win = windows_process;
#define NEVERD_MODULE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MODULE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsModuleCases.def"
#undef NEVERD_MODULE_TEXT
#undef NEVERD_MODULE_VALUE
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  const char *Directory;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA, ISA##Dir},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
ProcessOptions options(const std::filesystem::path &Directory) {
  ProcessOptions O;
  O.Windows = WindowsProcessOptions{
      {{LeafFile, Directory / LeafFile}, {MiddleFile, Directory / MiddleFile}}};
  return O;
}
class WindowsModules : public testing::TestWithParam<Profile> {};
TEST_P(WindowsModules, LinksRelocatedDLLCodeDataOrdinalAPIsAndExecutableTLS) {
#ifndef NEVERD_WINDOWS_MODULE_FIXTURE_DIR
  GTEST_SKIP() << MissingTools;
#else
  const auto &P = GetParam();
  ExecutionConfiguration Config;
  Config.Backend = P.Backend;
  Config.Architecture = P.ISA;
  Config.Contract = P.ISA == GuestArchitecture::X64
                        ? ExecutionContract::CheckedUserX64
                        : ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Config);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  const auto Directory =
      std::filesystem::path(NEVERD_WINDOWS_MODULE_FIXTURE_DIR) / P.Directory;
  auto O = options(Directory);
  O.Backend = P.Backend;
  auto R =
      emulateProcess(Directory / ProgramFile, ProcessProfile::WindowsPE64, O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
  EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
  EXPECT_EQ(R->StandardOutput, std::string(Message) + Detached);
  EXPECT_TRUE(R->StandardError.empty());
  EXPECT_EQ(R->SelectedBackend, P.Backend);
#endif
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, WindowsModules,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
class WindowsModuleImage : public testing::Test {
protected:
  std::filesystem::path Directory, Root;
  ProcessOptions Options;
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<win::VirtualMemory> Memory;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_MODULE_FIXTURE_DIR
    GTEST_SKIP() << MissingTools;
#else
    Directory =
        std::filesystem::path(NEVERD_WINDOWS_MODULE_FIXTURE_DIR) / X64Dir;
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
    Root = Temporary.str().str();
    Options = options(Directory);
#endif
  }
  void TearDown() override {
    if (!Root.empty())
      std::filesystem::remove_all(Root);
  }
  llvm::Expected<win::Program> load(const std::filesystem::path &Input = {}) {
    auto RAM = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
    Space = llvm::cantFail(AddressSpace::create(RAM, Options.MemoryLimit));
    Memory = std::make_unique<win::VirtualMemory>(*Space, Options);
    auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    return win::loadProgram(Input.empty() ? Directory / ProgramFile : Input,
                            Options, *Budget, *Memory);
  }
  std::string bytes(const char *Name) {
    auto Buffer = llvm::cantFail(llvm::errorOrToExpected(
        llvm::MemoryBuffer::getFile((Directory / Name).string())));
    return Buffer->getBuffer().str();
  }
  void supply(const char *Name, llvm::StringRef Bytes) {
    const auto File = Root / Name;
    std::error_code EC;
    llvm::raw_fd_ostream OS(File.string(), EC);
    ASSERT_FALSE(EC);
    OS << Bytes;
    for (auto &M : Options.Windows->Modules)
      if (M.Name == Name)
        M.Path = File;
  }
  void rejects(llvm::StringRef Reason) {
    auto R = load();
    EXPECT_FALSE(bool(R));
    if (!R)
      EXPECT_NE(llvm::toString(R.takeError()).find(Reason.str()),
                std::string::npos);
  }
  struct PE {
    std::string &Bytes;
    std::unique_ptr<llvm::object::COFFObjectFile> Object;
    explicit PE(std::string &Bytes) : Bytes(Bytes) {
      Object = llvm::cantFail(llvm::object::COFFObjectFile::create(
          llvm::MemoryBufferRef(Bytes, ProgramFile)));
    }
    char *rva(uint64_t RVA) {
      for (const auto &Reference : Object->sections()) {
        const auto *S = Object->getCOFFSection(Reference);
        if (RVA >= S->VirtualAddress &&
            RVA - S->VirtualAddress < S->SizeOfRawData)
          return Bytes.data() + S->PointerToRawData + RVA - S->VirtualAddress;
      }
      return nullptr;
    }
    template <class T> T *mutableRecord(const T *Record) {
      return reinterpret_cast<T *>(
          Bytes.data() +
          (reinterpret_cast<const char *>(Record) - Bytes.data()));
    }
    llvm::object::export_directory_table_entry *exports() {
      return reinterpret_cast<llvm::object::export_directory_table_entry *>(
          rva(Object->getDataDirectory(llvm::COFF::EXPORT_TABLE)
                  ->RelativeVirtualAddress));
    }
  };
};
TEST_F(WindowsModuleImage,
       OwnsImageReservationsAndDeduplicatesDiamondDependencies) {
  auto P = load();
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  ASSERT_EQ(P->Modules.size(), 3u);
  EXPECT_EQ(Space->mappedBytes(), 0u);
  std::set<uint64_t> Bases, Gates;
  size_t Moved = 0;
  for (size_t I = 0; I < P->Modules.size(); ++I) {
    const auto &M = P->Modules[I].Loaded;
    EXPECT_TRUE(Bases.insert(M.Base).second);
    auto Info = llvm::cantFail(Memory->query(M.Base));
    ASSERT_TRUE(Info);
    EXPECT_EQ(Info->AllocationBase, M.Base);
    EXPECT_EQ(Info->Type, MemImage);
    EXPECT_TRUE(llvm::cantFail(Memory->free(M.Base, 0, Release)).Unsupported);
    if (I && M.Base != PreferredDLLBase)
      ++Moved;
  }
  EXPECT_EQ(Moved, 1u);
  for (const auto &G : P->Gates)
    EXPECT_TRUE(Gates.insert(G.Gate).second);
  ASSERT_EQ(P->InitializationOrder.size(), 2u);
  EXPECT_EQ(P->Identities[P->InitializationOrder.front()].Name, LeafFile);
  EXPECT_EQ(P->Identities[P->InitializationOrder.back()].Name, MiddleFile);
}
TEST_F(WindowsModuleImage, RequiresExplicitCatalogueAndRejectsAmbiguousNames) {
  const auto Valid = Options;
  Options.Windows.reset();
  rejects(win::text::ModuleMissing);
  for (const char *Name : {DuplicateModule, PathModule, SystemModule}) {
    Options = Valid;
    Options.Windows->Modules.push_back({Name, Directory / LeafFile});
    rejects(win::text::ModuleName);
  }
  Options = Valid;
  Options.Windows->Modules.front().Name = DuplicateModule;
  auto Accepted = load();
  ASSERT_TRUE(bool(Accepted)) << llvm::toString(Accepted.takeError());
  Options.Windows->Modules.push_back({MissingModule, Root / MissingModule});
  auto Unused = load();
  ASSERT_TRUE(bool(Unused)) << llvm::toString(Unused.takeError());
}
TEST_F(WindowsModuleImage, RejectsDLLInitializersTLSAndMixedArchitectures) {
  auto Bytes = bytes(LeafFile);
  {
    PE P(Bytes);
    P.mutableRecord(P.Object->getPE32PlusHeader())->AddressOfEntryPoint =
        P.Object->getPE32PlusHeader()->BaseOfCode;
  }
  supply(LeafFile, Bytes);
  rejects(win::text::ModuleInit);
  Bytes = bytes(LeafFile);
  {
    PE P(Bytes);
    *P.mutableRecord(P.Object->getDataDirectory(llvm::COFF::TLS_TABLE)) =
        *P.Object->getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE);
  }
  supply(LeafFile, Bytes);
  rejects(win::text::ModuleInit);
  Options.Windows->Modules.front().Path =
      Directory.parent_path() / AArch64Dir / LeafFile;
  rejects(win::text::ModuleISA);
}
TEST_F(WindowsModuleImage, RejectsMissingSymbolsOrdinalHolesAndForwarders) {
  auto Bytes = bytes(MiddleFile);
  auto Position = Bytes.find(ProbeSymbol);
  ASSERT_NE(Position, std::string::npos);
  static_assert(sizeof(UnknownSymbol) == sizeof(ProbeSymbol));
  Bytes.replace(Position, sizeof(ProbeSymbol), UnknownSymbol,
                sizeof(UnknownSymbol));
  supply(MiddleFile, Bytes);
  rejects(win::text::ModuleExport);
  Options = options(Directory);
  Bytes = bytes(LeafFile);
  {
    PE P(Bytes);
    auto *D = P.exports();
    // The last export is the explicitly ordinal-only function.
    auto *Slot = P.rva(D->ExportAddressTableRVA) +
                 (D->AddressTableEntries - 1) * sizeof(uint32_t);
    llvm::support::endian::write32le(Slot, 0);
  }
  supply(LeafFile, Bytes);
  rejects(win::text::ModuleExport);
  Bytes = bytes(LeafFile);
  {
    PE P(Bytes);
    auto *D = P.exports();
    auto *Slot = P.rva(D->ExportAddressTableRVA) +
                 (D->AddressTableEntries - 1) * sizeof(uint32_t);
    llvm::support::endian::write32le(Slot, D->NameRVA);
  }
  supply(LeafFile, Bytes);
  rejects(win::text::ModuleForwarder);
}
TEST_F(WindowsModuleImage, RejectsCyclesFixedCollisionsAndMetadataFixups) {
  auto Bytes = bytes(LeafFile);
  auto Position = Bytes.find(SystemModule);
  ASSERT_NE(Position, std::string::npos);
  std::copy_n(MiddleFile, sizeof(MiddleFile), Bytes.data() + Position);
  supply(LeafFile, Bytes);
  rejects(win::text::ModuleCycle);
  Bytes = bytes(LeafFile);
  {
    PE P(Bytes);
    auto *H = P.mutableRecord(P.Object->getCOFFHeader());
    H->Characteristics =
        H->Characteristics | llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED;
  }
  supply(LeafFile, Bytes);
  rejects(win::text::Layout);
  Bytes = bytes(LeafFile);
  {
    PE P(Bytes);
    const uint64_t Target = P.exports()->ExportAddressTableRVA;
    auto *Block =
        reinterpret_cast<llvm::object::coff_base_reloc_block_header *>(
            P.rva(P.Object->getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE)
                      ->RelativeVirtualAddress));
    Block->PageRVA = Target & ~(win::value::PageSize - 1);
    llvm::support::endian::write16le(
        reinterpret_cast<char *>(Block + 1),
        (llvm::COFF::IMAGE_REL_BASED_DIR64 << win::value::RelocationTypeShift) |
            (Target & (win::value::PageSize - 1)));
  }
  supply(LeafFile, Bytes);
  rejects(win::text::ModuleFixup);
}
TEST_F(WindowsModuleImage, BoundsAggregateImagesMetadataAndPreparationTime) {
  win::ImageReadBudget Initial{process_defaults::Memory,
                               process_defaults::Memory};
  auto Main = llvm::cantFail(
      win::loadProgramImage(Directory / ProgramFile, Initial, false));
  Options.MemoryLimit = Main.Size + Options.StackSize +
                        win::value::EnvironmentEnd - win::value::TEB +
                        win::value::GateSize;
  auto TooSmall = load();
  EXPECT_FALSE(bool(TooSmall));
  llvm::consumeError(TooSmall.takeError());
  win::ImageReadBudget Reads{process_defaults::Memory,
                             process_defaults::Memory};
  Reads.Records = 1;
  auto Metadata = win::loadProgramImage(Directory / LeafFile, Reads, true);
  EXPECT_FALSE(bool(Metadata));
  llvm::consumeError(Metadata.takeError());
  Options = options(Directory);
  auto RAM = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
  auto S = llvm::cantFail(AddressSpace::create(RAM, Options.MemoryLimit));
  win::VirtualMemory V(*S, Options);
  const auto Past =
      ExecutionBudget::Clock::now() -
      std::chrono::microseconds(Options.Limits.TimeoutMicroseconds + 1);
  auto B = llvm::cantFail(ExecutionBudget::create(Options.Limits, Past));
  auto Expired = win::loadProgram(Directory / ProgramFile, Options, *B, V);
  EXPECT_FALSE(bool(Expired));
  if (!Expired)
    EXPECT_NE(
        llvm::toString(Expired.takeError()).find(win::text::ModuleTimeout),
        std::string::npos);
}
TEST_F(WindowsModuleImage, NativeWindowsLoaderRunsTheSameDLLGraph) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#else
  for (const char *File : {ProgramFile, LeafFile, MiddleFile})
    ASSERT_FALSE(llvm::sys::fs::copy_file((Directory / File).string(),
                                          (Root / File).string()));
  const auto Program = (Root / ProgramFile).string();
  const auto Output = (Root / StdoutFile).string();
  const auto Error = (Root / StderrFile).string();
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Error};
  std::string Diagnostic;
  bool Failed = false;
  const int Status =
      llvm::sys::ExecuteAndWait(Program, {Program}, std::nullopt, Redirects,
                                NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
  ASSERT_FALSE(Failed) << Diagnostic;
  auto Out = llvm::MemoryBuffer::getFile(Output);
  auto Err = llvm::MemoryBuffer::getFile(Error);
  ASSERT_TRUE(bool(Out));
  ASSERT_TRUE(bool(Err));
  EXPECT_EQ(Status, ExitStatus) << llvm::toHex((*Err)->getBuffer());
  EXPECT_EQ((*Out)->getBuffer(), std::string(Message) + Detached);
  EXPECT_TRUE((*Err)->getBuffer().empty());
#endif
}
} // namespace
} // namespace neverd::emulation
