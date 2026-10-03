//===- WindowsLifetimeTests.cpp - DLL initialization and exit
//--------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "WindowsNativeTestSupport.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessModules.h"

#include "neverd/emulation/ExecutionConfiguration.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace neverd::emulation {
namespace {
namespace win = windows_process;
#define NEVERD_LIFETIME_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LIFETIME_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsLifetimeCases.def"
#undef NEVERD_LIFETIME_TEXT
#undef NEVERD_LIFETIME_VALUE
struct Case {
  const char *Name, *Argument;
  uint32_t Status;
  const char *Variant = nullptr;
  bool Returns = false;
  const char *Output = nullptr;
};
constexpr Case Cases[] = {
#define NEVERD_LIFETIME_CASE(Name, Argument, Status)                           \
  {#Name, Argument, Status, nullptr, false, Name##Trace},
#define NEVERD_LIFETIME_RETURN_CASE(Name, Argument, Status)                    \
  {#Name, Argument, Status, nullptr, true, Name##Trace},
#define NEVERD_LIFETIME_NOENTRY_CASE(Name, Argument, Status)                   \
  {#Name, Argument, Status, NoEntryDirectory, false, Name##Trace},
#include "fixtures/WindowsLifetimeCases.def"
#undef NEVERD_LIFETIME_NOENTRY_CASE
#undef NEVERD_LIFETIME_RETURN_CASE
#undef NEVERD_LIFETIME_CASE
};
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
class WindowsLifetimes : public testing::TestWithParam<Profile> {
protected:
  std::filesystem::path Directory;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_LIFETIME_FIXTURE_DIR
    if (requireHvf(GetParam().Backend, GetParam().ISA))
      FAIL() << MissingTools;
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
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Directory = std::filesystem::path(NEVERD_WINDOWS_LIFETIME_FIXTURE_DIR) /
                P.Directory;
    Options.Backend = P.Backend;
    Options.Windows = WindowsProcessOptions{
        { {LeafFile, Directory / LeafFile},
          { MiddleFile,
            Directory / MiddleFile } }};
#endif
  }
  llvm::Expected<ProcessResult> run(const char *Argument) {
    Options.Arguments = {ProgramFile, Argument};
    return emulateProcess(Directory / ProgramFile, ProcessProfile::WindowsPE64,
                          Options);
  }
};
TEST_P(WindowsLifetimes, ExecutesStartupAndTerminationNotifications) {
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    const auto Inputs = C.Variant ? Directory / C.Variant : Directory;
    Options.Windows = WindowsProcessOptions{
        {{LeafFile, Inputs / LeafFile}, {MiddleFile, Inputs / MiddleFile}}};
    auto R = run(C.Argument);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    if (C.Returns) {
      EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure);
      EXPECT_FALSE(R->ExitStatus);
      EXPECT_EQ(R->ReturnValue, ExitStatus);
      EXPECT_NE(R->Diagnostic.find(win::text::EntryThreadExit),
                std::string::npos);
    } else {
      EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, C.Status) << llvm::toHex(R->StandardError);
    }
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(R->StandardOutput, C.Output);
  }
}
TEST_P(WindowsLifetimes, SharesInstructionAndEventBudgetsAcrossInitialization) {
  Options.Limits.Instructions = ShortInstructions;
  auto Instructions = run(Cases[0].Argument);
  ASSERT_TRUE(bool(Instructions)) << llvm::toString(Instructions.takeError());
  EXPECT_EQ(Instructions->Stop, ProcessStopReason::InstructionLimit);
  EXPECT_EQ(Instructions->Instructions, Options.Limits.Instructions);
  EXPECT_FALSE(Instructions->ExitStatus);
  EXPECT_TRUE(Instructions->StandardOutput.empty());
  Options.Limits.Instructions = process_defaults::Instructions;
  Options.Limits.Events = 1;
  auto Events = run(Cases[0].Argument);
  ASSERT_TRUE(bool(Events)) << llvm::toString(Events.takeError());
  EXPECT_EQ(Events->Stop, ProcessStopReason::EventLimit);
  EXPECT_EQ(Events->Events, Options.Limits.Events);
  EXPECT_FALSE(Events->ExitStatus);
  EXPECT_TRUE(Events->StandardOutput.empty());
}
TEST_P(WindowsLifetimes, CallbackFaultDoesNotInventDetachNotifications) {
  auto R = run(FaultArgument);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::CPUFailure) << R->Diagnostic;
  EXPECT_FALSE(R->ExitStatus);
  EXPECT_EQ(R->StandardOutput, FirstAttach);
  EXPECT_TRUE(R->StandardError.empty());
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, WindowsLifetimes,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
class WindowsModuleTLS : public testing::Test {
protected:
  std::filesystem::path Directory;
  ProcessOptions Options;
  std::shared_ptr<AddressSpace> Space;
  std::optional<win::Program> Program;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_LIFETIME_FIXTURE_DIR
    GTEST_SKIP() << MissingTools;
#else
    Directory =
        std::filesystem::path(NEVERD_WINDOWS_LIFETIME_FIXTURE_DIR) / X64Dir;
    Options.Windows = WindowsProcessOptions{
        { {LeafFile, Directory / LeafFile},
          { MiddleFile,
            Directory / MiddleFile } }};
    auto Physical = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
    Space = llvm::cantFail(AddressSpace::create(Physical, Options.MemoryLimit));
    win::VirtualMemory Memory(*Space, Options);
    auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    auto Loaded =
        win::loadProgram(Directory / ProgramFile, Options, *Budget, Memory);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    Program = std::move(*Loaded);
    for (const auto &M : Program->Modules)
      for (const auto &R : M.Loaded.Regions) {
        llvm::cantFail(Space->map(R.Address, R.Bytes.size(),
                                  Read | Write | UserAccessible));
        llvm::cantFail(Space->write(R.Address, R.Bytes));
      }
#endif
  }
};
TEST_F(WindowsModuleTLS, CopiesRelocatedLinkedTemplatesIntoIndependentSlots) {
  ASSERT_EQ(Program->Modules.size(), 3u);
  const auto &Changed = Program->Modules.back().Loaded;
  llvm::cantFail(Space->writeInteger(Changed.TLSTemplate, TemplateMarker, 1));
  auto Env = win::prepareEnvironment(*Space, *Program, Options);
  ASSERT_TRUE(bool(Env)) << llvm::toString(Env.takeError());
  std::set<uint64_t> Indices, Blocks;
  bool Rebased = false;
  for (const auto &M : Program->Modules) {
    const auto &I = M.Loaded;
    EXPECT_GE(I.Entry, I.Base);
    EXPECT_LT(I.Entry, I.Base + I.Size);
    const auto Index =
        llvm::cantFail(Space->readInteger(I.TLSIndex, win::value::DWordSize));
    ASSERT_TRUE(Indices.insert(Index).second);
    const auto Block = llvm::cantFail(Space->readInteger(
        win::value::TLSVector + Index * win::value::PointerSize,
        win::value::PointerSize));
    ASSERT_TRUE(Blocks.insert(Block).second);
    EXPECT_EQ(Block % I.TLSAlignment, 0u);
    std::vector<uint8_t> Template(I.TLSTemplateSize), Copy(I.TLSSize);
    llvm::cantFail(Space->read(I.TLSTemplate, Template));
    llvm::cantFail(Space->read(Block, Copy));
    EXPECT_TRUE(std::equal(Template.begin(), Template.end(), Copy.begin()));
    EXPECT_TRUE(std::all_of(Copy.begin() + I.TLSTemplateSize, Copy.end(),
                            [](uint8_t B) { return !B; }));
    if (&I == &Changed)
      EXPECT_EQ(Copy.front(), TemplateMarker);
    if (I.Base != PreferredDLLBase && &M != &Program->Modules.front()) {
      Rebased = true;
      bool Pointer = false;
      for (size_t Offset = 0;
           Offset + win::value::PointerSize <= Template.size(); ++Offset) {
        const uint64_t Value =
            llvm::support::endian::read64le(Template.data() + Offset);
        Pointer |= Value >= I.Base && Value < I.Base + I.Size;
      }
      EXPECT_TRUE(Pointer);
    }
  }
  EXPECT_TRUE(Rebased);
}
TEST_F(WindowsModuleTLS, BoundsAggregateCapacityAcrossIndividuallyValidImages) {
  for (auto &M : Program->Modules)
    M.Loaded.TLSSize = win::value::TLSCapacity;
  auto Env = win::prepareEnvironment(*Space, *Program, Options);
  EXPECT_FALSE(bool(Env));
  if (!Env)
    EXPECT_NE(llvm::toString(Env.takeError()).find(win::text::ModuleTLSBudget),
              std::string::npos);
}
TEST_F(WindowsModuleTLS,
       RejectsUnrelocatedFieldsInsteadOfBiasingCachedPointers) {
  for (uint64_t Field = 0; Field < TLSVAFields; ++Field) {
    win::ImageReadBudget Budget{Options.MemoryLimit, Options.MemoryLimit};
    auto I = win::loadProgramImage(Directory / LeafFile, Budget, true);
    ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
    const uint64_t Target = I->TLSDirectory + Field * win::value::PointerSize;
    const auto Before = I->Relocations.size();
    std::erase(I->Relocations, Target);
    ASSERT_EQ(I->Relocations.size() + 1, Before);
    auto E = win::relocateImage(*I, I->Base + RebaseDistance, Budget);
    EXPECT_TRUE(bool(E));
    if (E)
      EXPECT_NE(llvm::toString(std::move(E)).find(win::text::TLS),
                std::string::npos);
  }
}
TEST_F(WindowsModuleTLS, RejectsUnprovenZeroFillMetadataBeforeExecution) {
  auto Original = llvm::MemoryBuffer::getFile((Directory / LeafFile).string());
  ASSERT_TRUE(bool(Original));
  std::string Bytes = (*Original)->getBuffer().str();
  auto Object = llvm::object::COFFObjectFile::create(
      llvm::MemoryBufferRef(Bytes, LeafFile));
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  const auto *TLS = (*Object)->getTLSDirectory64();
  ASSERT_NE(TLS, nullptr);
  const uint64_t Offset =
      reinterpret_cast<const char *>(TLS) - Bytes.data() +
      offsetof(llvm::object::coff_tls_directory64, SizeOfZeroFill);
  llvm::support::endian::write32le(Bytes.data() + Offset, ZeroTail);
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  const auto File = Root / InvalidTLSFile;
  {
    std::error_code EC;
    llvm::raw_fd_ostream Output(File.string(), EC);
    ASSERT_FALSE(EC);
    Output << Bytes;
  }
  win::ImageReadBudget Budget{Options.MemoryLimit, Options.MemoryLimit};
  auto Loaded = win::loadProgramImage(File, Budget, true);
  std::filesystem::remove_all(Root);
  EXPECT_FALSE(bool(Loaded));
  if (!Loaded)
    EXPECT_NE(llvm::toString(Loaded.takeError()).find(win::text::TLSZeroFill),
              std::string::npos);
}
TEST(WindowsModuleLifetime, NativeWindowsObservesStartupAndTermination) {
#if !defined(NEVERD_WINDOWS_LIFETIME_FIXTURE_DIR)
  GTEST_SKIP() << MissingTools;
#elif !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  struct Cleanup {
    const std::filesystem::path &Path;
    ~Cleanup() { std::filesystem::remove_all(Path); }
  } Remove{Root};
  const auto Directory =
      std::filesystem::path(NEVERD_WINDOWS_LIFETIME_FIXTURE_DIR) / X64Dir;
  llvm::sys::Process::PreventCoreFiles();
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    const auto CaseRoot = Root / C.Name;
    ASSERT_TRUE(std::filesystem::create_directory(CaseRoot));
    const auto Inputs = C.Variant ? Directory / C.Variant : Directory;
    for (const char *File : {LeafFile, MiddleFile})
      ASSERT_FALSE(llvm::sys::fs::copy_file((Inputs / File).string(),
                                            (CaseRoot / File).string()));
    ASSERT_FALSE(llvm::sys::fs::copy_file((Directory / ProgramFile).string(),
                                          (CaseRoot / ProgramFile).string()));
    const auto Program = (CaseRoot / ProgramFile).string();
    const auto Output = (CaseRoot / StdoutFile).string();
    const auto Error = (CaseRoot / StderrFile).string();
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                        Error};
    std::string Diagnostic;
    bool Failed = false;
    const unsigned Repetitions = C.Returns ? NativeReturnRepetitions : 1;
    for (unsigned I = 0; I < Repetitions; ++I) {
      SCOPED_TRACE(I);
      uint32_t Status;
      if (C.Returns) {
        auto Thread = native_test::observeNativeThread(
            Program, C.Argument, Output, Error, NativeTimeoutSeconds);
        ASSERT_TRUE(bool(Thread)) << llvm::toString(Thread.takeError());
        Status = *Thread;
      } else {
        Status = uint32_t(llvm::sys::ExecuteAndWait(
            Program, {Program, C.Argument}, std::nullopt, Redirects,
            NativeTimeoutSeconds, 0, &Diagnostic, &Failed));
        EXPECT_FALSE(Failed) << Diagnostic;
      }
      auto Out = llvm::MemoryBuffer::getFile(Output);
      auto Err = llvm::MemoryBuffer::getFile(Error);
      ASSERT_TRUE(bool(Out));
      ASSERT_TRUE(bool(Err));
      llvm::outs() << C.Name << ": " << llvm::toHex((*Out)->getBuffer())
                   << " status=" << llvm::utohexstr(Status) << "\n";
      EXPECT_EQ(Status, C.Status) << llvm::toHex((*Err)->getBuffer());
      if (C.Returns) {
        // Process teardown can begin before thread teardown or after its
        // TLS has been released. Remaining threads may also keep it alive.
        EXPECT_TRUE((*Out)->getBuffer() == ReturnedThreadTrace ||
                    (*Out)->getBuffer() == ReturnedProcessTrace ||
                    (*Out)->getBuffer() == NormalTrace)
            << llvm::toHex((*Out)->getBuffer());
      } else {
        EXPECT_EQ((*Out)->getBuffer(), llvm::StringRef(C.Output));
      }
      EXPECT_TRUE((*Err)->getBuffer().empty());
    }
  }
#endif
}
} // namespace
} // namespace neverd::emulation
