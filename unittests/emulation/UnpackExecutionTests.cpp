//===- UnpackExecutionTests.cpp - Recovery by observing execution ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnpackTestSupport.h"

#include "neverd/emulation/ProcessObserver.h"
#include "neverd/emulation/WindowsProcessState.h"
#include "neverd/unpack/Unpack.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <array>
#include <map>
#include <optional>

namespace neverd::unpack {
namespace {
using namespace test;
using emulation::ExecutionBackendKind;

struct Fixture {
  const char *Name, *Packed, *Original;
  bool HasRuntimeState;
};
constexpr Fixture Fixtures[] = {
#define NEVERD_UNPACK_TEST_FIXTURE(Name, Packed, Original, HasRuntimeState)    \
  {#Name, Packed, Original, HasRuntimeState},
#include "UnpackCases.def"
#undef NEVERD_UNPACK_TEST_FIXTURE
};
struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
};
constexpr Backend Backends[] = {
#define NEVERD_UNPACK_TEST_BACKEND(Name, Kind)                                 \
  {#Name, ExecutionBackendKind::Kind},
#include "UnpackCases.def"
#undef NEVERD_UNPACK_TEST_BACKEND
};
void PrintTo(const Backend &B, std::ostream *OS) { *OS << B.Name; }
void PrintTo(const Fixture &F, std::ostream *OS) { *OS << F.Name; }

/// Run on one backend. An unavailable transport is absent coverage, which the
/// caller reports as a skip; every other error is a failure.
std::optional<UnpackResult> unpackOn(ExecutionBackendKind Kind,
                                     const char *Name,
                                     UnpackOptions Options = {}) {
  Options.Process.Backend = Kind;
  auto Result = unpackFile(fixture(Name), Options);
  if (Result)
    return std::move(*Result);
  auto E = Result.takeError();
  if (!E.isA<emulation::BackendUnavailableError>())
    ADD_FAILURE() << Name << ": " << llvm::toString(std::move(E));
  else
    llvm::consumeError(std::move(E));
  return std::nullopt;
}

class Unpack : public testing::TestWithParam<Backend> {
protected:
  /// Skips the test when the backend is unavailable.
  std::optional<UnpackResult> unpack(const char *Name,
                                     UnpackOptions Options = {}) {
    auto Result = unpackOn(GetParam().Kind, Name, std::move(Options));
    if (!Result && !HasFailure())
      Skipped = true;
    return Result;
  }
  bool Skipped = false;
};
#define NEVERD_REQUIRE(Result)                                                 \
  if (!(Result)) {                                                             \
    if (Skipped)                                                               \
      GTEST_SKIP() << BackendUnavailable;                                      \
    return;                                                                    \
  }

/// One packed fixture on one backend. A whole stub runs in each case, so the
/// cases are separate tests with separate time limits. The pair is one value
/// whose printed form is a single token with no spaces or parentheses, so
/// every CMake test-discovery version registers the same case name.
struct BackendFixture {
  Backend Transport;
  Fixture F;
};
constexpr std::array<BackendFixture, std::size(Backends) * std::size(Fixtures)>
    backendFixtures = [] {
      std::array<BackendFixture, std::size(Backends) * std::size(Fixtures)>
          Cases{};
      size_t At = 0;
      for (const auto &B : Backends)
        for (const auto &F : Fixtures)
          Cases[At++] = {B, F};
      return Cases;
    }();
void PrintTo(const BackendFixture &C, std::ostream *OS) {
  *OS << C.Transport.Name << FixtureSeparator << C.F.Name;
}
class UnpackFixture : public testing::TestWithParam<BackendFixture> {};

std::set<Image::Import> runtimeImports(const UnpackResult &Result) {
  std::set<Image::Import> Out;
  for (const auto &I : Result.Imports)
    if (I.Origin == ImportOrigin::Runtime)
      Out.insert({I.Module, I.Name, I.SlotRVA});
  return Out;
}

/// The independent linked image executes its own initializers before its
/// entry too. Compare the complete state at that same boundary, including
/// initialized BSS, instead of assuming callbacks had no memory effects.
class LinkedEntrySnapshot final : public emulation::ProcessObserver {
public:
  explicit LinkedEntrySnapshot(Image &Program) : Program(Program) {}
  std::optional<std::vector<uint8_t>> ThreadLocal;
  std::shared_ptr<const emulation::ProcessRuntimeState> Runtime;
  llvm::Expected<std::vector<emulation::ExecutionWatch>>
  started(emulation::ProcessView &Process) override {
    return std::vector<emulation::ExecutionWatch>{
        {Program.Base + Program.Entry, 1}};
  }
  llvm::Expected<std::optional<std::vector<emulation::ExecutionWatch>>>
  watched(emulation::ProcessView &Process, uint64_t PC) override {
    for (const auto &S : Program.Sections)
      if (auto E = Process.read(Program.Base + S.RVA,
                                llvm::MutableArrayRef(Program.Mapped)
                                    .slice(S.RVA, S.VirtualSize)))
        return std::move(E);
    auto TLS = Process.threadLocalMemory();
    if (!TLS)
      return TLS.takeError();
    ThreadLocal = std::move(*TLS);
    auto State = Process.runtimeState();
    if (!State)
      return State.takeError();
    Runtime = std::move(*State);
    return std::nullopt;
  }

private:
  Image &Program;
};

TEST_P(UnpackFixture, RecoversEntrySectionsAndImports) {
  const auto &[Transport, F] = GetParam();
  (void)Transport;
  auto Result = unpackOn(Transport.Kind, F.Packed);
  if (!Result) {
    if (!HasFailure())
      GTEST_SKIP() << BackendUnavailable;
    return;
  }
  if (F.HasRuntimeState) {
    ASSERT_EQ(Result->Outcome, UnpackOutcome::UnsupportedState)
        << Result->Diagnostic;
    EXPECT_TRUE(Result->RuntimeState.AdditionalStateInventoryKnown);
    EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
    EXPECT_TRUE(Result->Image.empty());
    UnpackOptions Options;
    Options.SnapshotOnly = true;
    Result = unpackOn(Transport.Kind, F.Packed, Options);
    ASSERT_TRUE(Result);
  }
  ASSERT_EQ(Result->Outcome, F.HasRuntimeState ? UnpackOutcome::Snapshot
                                               : UnpackOutcome::Unpacked)
      << Result->Diagnostic;
  EXPECT_EQ(Result->Packer.Kind, PackerKind::Unidentified);
  EXPECT_EQ(Result->ProcessStop, StopObserver);
  Image Original = readImage(fixture(F.Original));
  const Image Rebuilt = readImage(Result->Image);
  ASSERT_FALSE(HasFailure());
  LinkedEntrySnapshot Snapshot(Original);
  UnpackOptions OracleOptions;
  OracleOptions.Process.Backend = Transport.Kind;
  auto Oracle = emulation::observeProcess(
      fixture(F.Original), emulation::ProcessProfile::WindowsPE64,
      OracleOptions.Process, Snapshot);
  ASSERT_TRUE(bool(Oracle)) << llvm::toString(Oracle.takeError());
  ASSERT_EQ(Oracle->Stop, emulation::ProcessStopReason::Observer)
      << Oracle->Diagnostic;
  EXPECT_EQ(Result->EntryRVA, Original.Entry);
  EXPECT_EQ(Rebuilt.Entry, Original.Entry);
  EXPECT_EQ(Result->ImageBase, Original.Base);
  EXPECT_EQ(Rebuilt.Base, Original.Base);
  // Every byte of the program is back, at the address it was linked for.
  for (const auto &S : Original.Sections)
    EXPECT_EQ(differingBytes(Original, Rebuilt, S), 0u) << S.Name;
  // The cells the stub filled are exactly the program's imports, and the
  // rebuilt directory binds each of them again.
  EXPECT_EQ(runtimeImports(*Result), Original.Imports);
  EXPECT_TRUE(std::includes(Rebuilt.Imports.begin(), Rebuilt.Imports.end(),
                            Original.Imports.begin(), Original.Imports.end()));
  EXPECT_EQ(Rebuilt.Imports.size(), Result->Imports.size());
  // No relocation of generated content was observed, so none is claimed.
  EXPECT_FALSE(Rebuilt.DLLCharacteristics &
               llvm::COFF::IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE);
  EXPECT_TRUE(Rebuilt.FileCharacteristics &
              llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED);
  EXPECT_EQ(Rebuilt.directory(llvm::COFF::BASE_RELOCATION_TABLE).Size, 0u);
  ASSERT_FALSE(Rebuilt.Sections.empty());
  EXPECT_EQ(Rebuilt.Sections.back().Name, MetadataSection);
  EXPECT_EQ(Result->Sections.size(), Rebuilt.Sections.size());
}

// The direct contract runs the stub natively between page faults instead of
// single-stepping it. The unpacker's transfer observer still catches the entry
// because the not-yet-executed image pages are non-executable, so recovery must
// reach the identical rebuilt image on both software and hardware transports.
TEST_P(UnpackFixture, DirectContractRecoversTheSameImage) {
  const auto &[Transport, F] = GetParam();
  if (Transport.Kind != ExecutionBackendKind::Unicorn &&
      Transport.Kind != ExecutionBackendKind::KVM &&
      Transport.Kind != ExecutionBackendKind::WHP)
    GTEST_SKIP() << "this transport has no direct x64 execution";
  UnpackOptions Options;
  Options.SnapshotOnly = F.HasRuntimeState;
  auto Checked = unpackOn(Transport.Kind, F.Packed, Options);
  if (!Checked) {
    if (!HasFailure())
      GTEST_SKIP() << BackendUnavailable;
    return;
  }
  const auto Expected =
      F.HasRuntimeState ? UnpackOutcome::Snapshot : UnpackOutcome::Unpacked;
  ASSERT_EQ(Checked->Outcome, Expected) << Checked->Diagnostic;
  Options.Process.Contract = emulation::ExecutionContract::DirectUserX64;
  auto Direct = unpackOn(Transport.Kind, F.Packed, Options);
  if (!Direct) {
    if (!HasFailure())
      GTEST_SKIP() << BackendUnavailable;
    return;
  }
  ASSERT_EQ(Direct->Outcome, Expected) << Direct->Diagnostic;
  EXPECT_EQ(Direct->EntryRVA, Checked->EntryRVA);
  EXPECT_EQ(Direct->Packer.Kind, Checked->Packer.Kind);
  // Byte-identical rebuilt file: the two contracts observed the same program.
  EXPECT_EQ(Direct->Image, Checked->Image);
  EXPECT_EQ(runtimeImports(*Direct), runtimeImports(*Checked));
}

INSTANTIATE_TEST_SUITE_P(Backends, UnpackFixture,
                         testing::ValuesIn(backendFixtures),
                         [](const auto &Info) {
                           return std::string(Info.param.Transport.Name) +
                                  FixtureSeparator + Info.param.F.Name;
                         });

TEST_P(Unpack, RecoveredImageRunsExactlyLikeTheOriginal) {
  auto Result = unpack(PlainPacked);
  NEVERD_REQUIRE(Result);
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked) << Result->Diagnostic;
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(ScratchPrefix, Directory));
  const auto Path = std::filesystem::path(Directory.str().str()) / Plain;
  writeFile(Path, Result->Image);
  // A single-stepping hardware transport can need longer than the default
  // budget of a guest run for the same program the unpacking run completed.
  emulation::ProcessOptions Options;
  Options.Backend = GetParam().Kind;
  Options.Limits.Instructions = defaults::Instructions;
  Options.Limits.TimeoutMicroseconds = defaults::TimeoutMicroseconds;
  auto Expected = emulation::emulateProcess(
      fixture(Plain), emulation::ProcessProfile::WindowsPE64, Options);
  auto Actual = emulation::emulateProcess(
      Path, emulation::ProcessProfile::WindowsPE64, Options);
  std::filesystem::remove_all(std::filesystem::path(Directory.str().str()));
  ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
  ASSERT_TRUE(bool(Actual)) << llvm::toString(Actual.takeError());
  EXPECT_EQ(Expected->Stop, emulation::ProcessStopReason::Exited);
  EXPECT_EQ(Actual->Stop, Expected->Stop) << Actual->Diagnostic;
  EXPECT_EQ(Actual->ExitStatus, Expected->ExitStatus);
  EXPECT_EQ(Actual->StandardOutput, Expected->StandardOutput);
  EXPECT_FALSE(Expected->StandardOutput.empty());
  // The same program executes the same instructions.
  EXPECT_EQ(Actual->Instructions, Expected->Instructions);
}

TEST_P(Unpack, OSTLSCallbackOnTheEntryStackIsNotTheProgramEntry) {
#ifndef NEVERD_WINDOWS_DEFERRED_FIXTURE_DIR
  GTEST_SKIP() << "generated TLS fixture requires Clang and lld-link";
#else
  const auto Path = std::filesystem::path(NEVERD_WINDOWS_DEFERRED_FIXTURE_DIR) /
                    "X64" / "generated-tls.exe";
  UnpackOptions Options;
  Options.Process.Backend = GetParam().Kind;
  auto Result = unpackFile(Path, Options);
  if (!Result) {
    auto E = Result.takeError();
    const bool Unavailable = E.isA<emulation::BackendUnavailableError>();
    auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  EXPECT_EQ(Result->Outcome, UnpackOutcome::NoEntry);
  EXPECT_TRUE(Result->Image.empty());
  ASSERT_FALSE(Result->Transfers.empty());
  EXPECT_TRUE(Result->Transfers.front().StackBalanced);
  EXPECT_FALSE(Result->Transfers.front().ProgramInvocation);
#endif
}

TEST_P(Unpack, GeneratedProgramEntryAfterOSTLSCallbacksIsObserved) {
#ifndef NEVERD_WINDOWS_DEFERRED_FIXTURE_DIR
  GTEST_SKIP() << "generated TLS fixture requires Clang and lld-link";
#else
  const auto Path = std::filesystem::path(NEVERD_WINDOWS_DEFERRED_FIXTURE_DIR) /
                    "X64" / "generated-tls-entry.exe";
  const Image Original = readImage(Path);
  ASSERT_FALSE(HasFailure());
  for (auto Contract : {emulation::ExecutionContract::CheckedUserX64,
                        emulation::ExecutionContract::DirectUserX64}) {
    SCOPED_TRACE(emulation::executionContractName(Contract));
    UnpackOptions Options;
    Options.Process.Backend = GetParam().Kind;
    Options.Process.Contract = Contract;
    auto Result = unpackFile(Path, Options);
    if (!Result) {
      auto E = Result.takeError();
      const bool Unavailable = E.isA<emulation::BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked) << Result->Diagnostic;
    EXPECT_EQ(Result->EntryRVA, Original.Entry);
    EXPECT_EQ(Result->MaterializedTLSCallbacks, 2u);
    ASSERT_GE(Result->Transfers.size(), 2u);
    EXPECT_TRUE(Result->Transfers.front().StackBalanced);
    EXPECT_FALSE(Result->Transfers.front().ProgramInvocation);
    EXPECT_TRUE(Result->Transfers.back().StackBalanced);
    EXPECT_TRUE(Result->Transfers.back().ProgramInvocation);
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory(ScratchPrefix, Directory));
    const auto Output = std::filesystem::path(Directory.str().str()) / Plain;
    writeFile(Output, Result->Image);
    auto Run = emulation::emulateProcess(
        Output, emulation::ProcessProfile::WindowsPE64, Options.Process);
    std::filesystem::remove_all(std::filesystem::path(Directory.str().str()));
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, emulation::ProcessStopReason::Exited)
        << Run->Diagnostic;
    EXPECT_EQ(Run->ExitStatus, 37u);
  }
#endif
}

TEST_P(Unpack, DirectJumpToTheProgramIsObservedAsItsEntry) {
  auto Result = unpack(PlainPacked);
  NEVERD_REQUIRE(Result);
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked) << Result->Diagnostic;
  ASSERT_EQ(Result->Transfers.size(), 1u);
  EXPECT_TRUE(Result->Transfers[0].StackBalanced);
  EXPECT_EQ(Result->Transfers[0].Generation, 1u);
  EXPECT_EQ(Result->Transfers[0].RVA, Result->EntryRVA);
  EXPECT_EQ(Result->Source, EntrySource::Transfer);
  EXPECT_TRUE(Result->Diagnostic.empty());
}

TEST_P(Unpack, StubCallIntoTheProgramIsNotItsEntry) {
  UnpackOptions SnapshotOptions;
  SnapshotOptions.SnapshotOnly = true;
  auto Result = unpack(RuntimePacked, SnapshotOptions);
  NEVERD_REQUIRE(Result);
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Snapshot) << Result->Diagnostic;
  EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
  const Image Original = readImage(fixture(RuntimeOriginal));
  // The stub calls the program's TLS callbacks before it leaves. That call
  // enters generated code on a deeper stack and is reported, not accepted.
  ASSERT_GE(Result->Transfers.size(), 2u);
  EXPECT_FALSE(Result->Transfers.front().StackBalanced);
  EXPECT_NE(Result->Transfers.front().RVA, Original.Entry);
  EXPECT_TRUE(Result->Transfers.back().StackBalanced);
  EXPECT_EQ(Result->Transfers.back().RVA, Original.Entry);
  EXPECT_EQ(Result->Source, EntrySource::Transfer);
  EXPECT_EQ(Result->EntryRVA, Original.Entry);
  // The stub already completed the program's callbacks. Preserve the original
  // allocation and callback storage, with adapters that skip repeated attach.
  Image Rebuilt = readImage(Result->Image);
  const auto Expected = Original.directory(llvm::COFF::TLS_TABLE);
  const auto Actual = Rebuilt.directory(llvm::COFF::TLS_TABLE);
  ASSERT_NE(Expected.RelativeVirtualAddress, 0u);
  EXPECT_EQ(Actual.Size, Expected.Size);
  EXPECT_NE(readImage(fixture(RuntimePacked))
                .directory(llvm::COFF::TLS_TABLE)
                .RelativeVirtualAddress,
            Expected.RelativeVirtualAddress);
  const auto *Metadata = Rebuilt.section(MetadataSection);
  ASSERT_NE(Metadata, nullptr);
  EXPECT_GE(Actual.RelativeVirtualAddress, Metadata->RVA);
  EXPECT_LT(Actual.RelativeVirtualAddress,
            Metadata->RVA + Metadata->VirtualSize);
  using llvm::object::coff_tls_directory64;
  coff_tls_directory64 Before, After;
  ASSERT_LE(uint64_t(Expected.RelativeVirtualAddress) + sizeof(Before),
            Original.Mapped.size());
  ASSERT_LE(uint64_t(Actual.RelativeVirtualAddress) + sizeof(After),
            Rebuilt.Mapped.size());
  std::memcpy(&Before, Original.Mapped.data() + Expected.RelativeVirtualAddress,
              sizeof(Before));
  std::memcpy(&After, Rebuilt.Mapped.data() + Actual.RelativeVirtualAddress,
              sizeof(After));
  EXPECT_EQ(After.StartAddressOfRawData, Before.StartAddressOfRawData);
  EXPECT_EQ(After.EndAddressOfRawData, Before.EndAddressOfRawData);
  EXPECT_EQ(After.AddressOfIndex, Before.AddressOfIndex);
  EXPECT_EQ(After.SizeOfZeroFill, Before.SizeOfZeroFill);
  EXPECT_EQ(After.Characteristics, Before.Characteristics);
  EXPECT_EQ(std::memcmp(&Before,
                        Rebuilt.Mapped.data() + Expected.RelativeVirtualAddress,
                        sizeof(Before)),
            0);
  const uint64_t OldArray = uint64_t(Before.AddressOfCallBacks) - Original.Base;
  const uint64_t NewArray = uint64_t(After.AddressOfCallBacks) - Rebuilt.Base;
  ASSERT_LE(OldArray, Original.Mapped.size());
  ASSERT_LE(NewArray, Rebuilt.Mapped.size());
  uint64_t Callbacks = 0;
  for (;; ++Callbacks) {
    const uint64_t Offset = Callbacks * PointerBytes;
    ASSERT_LE(Offset + PointerBytes, Original.Mapped.size() - OldArray);
    ASSERT_LE(Offset + PointerBytes, Rebuilt.Mapped.size() - NewArray);
    const uint64_t Target = llvm::support::endian::read64le(
        Original.Mapped.data() + OldArray + Offset);
    const uint64_t Adapter = llvm::support::endian::read64le(
        Rebuilt.Mapped.data() + NewArray + Offset);
    if (!Target) {
      EXPECT_EQ(Adapter, 0u);
      break;
    }
    EXPECT_GE(Adapter, Rebuilt.Base + Metadata->RVA);
    EXPECT_LT(Adapter, Rebuilt.Base + Metadata->RVA + Metadata->VirtualSize);
  }
  EXPECT_GT(Callbacks, 0u);
  EXPECT_EQ(Result->MaterializedTLSCallbacks, Callbacks);

  Image Initialized = Original;
  LinkedEntrySnapshot OriginalState(Initialized), RebuiltState(Rebuilt);
  UnpackOptions Options;
  Options.Process.Backend = GetParam().Kind;
  Options.Process.Contract = emulation::ExecutionContract::DirectUserX64;
  auto OriginalRun = emulation::observeProcess(
      fixture(RuntimeOriginal), emulation::ProcessProfile::WindowsPE64,
      Options.Process, OriginalState);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(ScratchPrefix, Directory));
  const auto Path = std::filesystem::path(Directory.str().str()) / Plain;
  writeFile(Path, Result->Image);
  auto RebuiltRun =
      emulation::observeProcess(Path, emulation::ProcessProfile::WindowsPE64,
                                Options.Process, RebuiltState);
  std::filesystem::remove_all(std::filesystem::path(Directory.str().str()));
  ASSERT_TRUE(bool(OriginalRun)) << llvm::toString(OriginalRun.takeError());
  ASSERT_TRUE(bool(RebuiltRun)) << llvm::toString(RebuiltRun.takeError());
  EXPECT_EQ(OriginalRun->Stop, emulation::ProcessStopReason::Observer);
  EXPECT_EQ(RebuiltRun->Stop, emulation::ProcessStopReason::Observer);
  EXPECT_EQ(OriginalState.ThreadLocal, RebuiltState.ThreadLocal);
  ASSERT_TRUE(OriginalState.Runtime);
  ASSERT_EQ(OriginalState.Runtime->Profile,
            emulation::ProcessRuntimeState::Kind::WindowsPE64);
  const auto &Owned = static_cast<const emulation::WindowsProcessState &>(
      *OriginalState.Runtime);
  ASSERT_EQ(Owned.CriticalSections.size(), 1u);
  EXPECT_EQ(Owned.CriticalSections.front().Recursion, 0u);
  // The bytes agree, but a fresh process has no initialized critical section.
  // This is why the image is an explicit snapshot, not a recovery success.
  ASSERT_TRUE(RebuiltState.Runtime);
  EXPECT_FALSE(RebuiltState.Runtime->hasAdditionalDependencies());
  for (const auto &S : Initialized.Sections)
    EXPECT_EQ(differingBytes(Initialized, Rebuilt, S), 0u) << S.Name;
}

TEST_P(Unpack, ExplicitTransferOverridesTheEntryStack) {
  UnpackOptions Options;
  Options.Transfer = 1;
  auto Result = unpack(RuntimePacked, Options);
  NEVERD_REQUIRE(Result);
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked) << Result->Diagnostic;
  ASSERT_EQ(Result->Transfers.size(), 1u);
  EXPECT_FALSE(Result->Transfers[0].StackBalanced);
  EXPECT_EQ(Result->EntryRVA, Result->Transfers[0].RVA);
  EXPECT_EQ(Result->Source, EntrySource::Transfer);
}

TEST_P(Unpack, RunThatEndsInsideTheStubReportsWhy) {
  UnpackOptions Options;
  Options.Process.Limits.Instructions = TinyInstructionLimit;
  auto Result = unpack(PlainPacked, Options);
  NEVERD_REQUIRE(Result);
  EXPECT_EQ(Result->Outcome, UnpackOutcome::NoEntry);
  EXPECT_EQ(Result->ProcessStop, StopInstructionLimit);
  EXPECT_EQ(Result->Instructions, TinyInstructionLimit);
  EXPECT_FALSE(Result->EntryRVA);
  EXPECT_TRUE(Result->Transfers.empty());
  EXPECT_TRUE(Result->Image.empty());
  EXPECT_NE(Result->Diagnostic.find(StopInstructionLimit), std::string::npos);
}

TEST_P(Unpack, ProgramThatGeneratesNoCodeHasNoEntryToRecover) {
  auto Result = unpack(Plain);
  NEVERD_REQUIRE(Result);
  EXPECT_EQ(Result->Packer.Kind, PackerKind::Unidentified);
  EXPECT_EQ(Result->Outcome, UnpackOutcome::NoEntry);
  EXPECT_EQ(Result->ProcessStop, StopExited);
  EXPECT_TRUE(Result->Transfers.empty());
  EXPECT_TRUE(Result->Image.empty());
}

TEST_P(Unpack, ProgramCodeBeyondTheModelStopsInsteadOfBeingSkipped) {
  // The initializers now execute normally. Continuing past the entry still
  // reaches unmodeled program services; they must stop with a diagnostic.
  UnpackOptions Options;
  Options.Transfer = defaults::MaxTransfers;
  auto Result = unpack(RuntimePacked, Options);
  NEVERD_REQUIRE(Result);
  EXPECT_EQ(Result->Outcome, UnpackOutcome::NoEntry);
  EXPECT_EQ(Result->ProcessStop, StopUnsupportedService);
  EXPECT_FALSE(Result->ProcessDiagnostic.empty());
  ASSERT_GE(Result->Transfers.size(), 3u);
  EXPECT_TRUE(Result->Image.empty());
}

INSTANTIATE_TEST_SUITE_P(Backends, Unpack, testing::ValuesIn(Backends),
                         [](const auto &Info) {
                           return std::string(Info.param.Name);
                         });

// Keep one finite comparison allowance per fixture, as for recovery tests.
class UnpackBackends : public testing::TestWithParam<Fixture> {};

TEST_P(UnpackBackends, ProduceIdenticalImages) {
  const auto &F = GetParam();
  std::map<std::string, std::vector<uint8_t>> Images;
  UnpackOptions Options;
  Options.SnapshotOnly = F.HasRuntimeState;
  for (const auto &B : Backends)
    if (auto Result = unpackOn(B.Kind, F.Packed, Options)) {
      ASSERT_EQ(Result->Outcome, F.HasRuntimeState ? UnpackOutcome::Snapshot
                                                   : UnpackOutcome::Unpacked)
          << Result->Diagnostic;
      Images[B.Name] = std::move(Result->Image);
    }
  if (HasFailure())
    return;
  if (Images.size() < 2)
    GTEST_SKIP() << SingleBackend;
  for (const auto &[Name, Bytes] : Images)
    EXPECT_EQ(Bytes, Images.begin()->second)
        << Name << " differs from " << Images.begin()->first;
}

INSTANTIATE_TEST_SUITE_P(Fixtures, UnpackBackends, testing::ValuesIn(Fixtures),
                         [](const auto &Info) { return Info.param.Name; });

TEST(UnpackOptions, DefaultsDeferUnmodeledLoaderFacts) {
  const UnpackOptions Options;
  EXPECT_EQ(Options.Transfer, 0u);
  EXPECT_FALSE(Options.SnapshotOnly);
  EXPECT_EQ(Options.Process.Limits.Instructions, defaults::Instructions);
  EXPECT_EQ(Options.Process.MemoryLimit, defaults::Memory);
  ASSERT_TRUE(Options.Process.Windows);
  EXPECT_TRUE(Options.Process.Windows->DeferUnmodeled);
}

TEST(UnpackOptions, DecodingIsStrictAndKeepsUnpackingDefaults) {
  auto Defaults = unpackOptionsFromJSON("{}");
  ASSERT_TRUE(bool(Defaults)) << llvm::toString(Defaults.takeError());
  EXPECT_EQ(Defaults->Process.Limits.Instructions, defaults::Instructions);
  EXPECT_EQ(Defaults->Process.InstructionQuantum, defaults::Quantum);
  ASSERT_TRUE(Defaults->Process.Windows);
  EXPECT_TRUE(Defaults->Process.Windows->DeferUnmodeled);

  auto Chosen = unpackOptionsFromJSON(SecondTransfer);
  ASSERT_TRUE(bool(Chosen)) << llvm::toString(Chosen.takeError());
  EXPECT_EQ(Chosen->Transfer, 2u);
  EXPECT_EQ(Chosen->Process.Limits.Instructions, RequestedInstructionLimit);
  EXPECT_EQ(Chosen->Process.Limits.Events, defaults::Events);

  auto Strict = unpackOptionsFromJSON(StrictWindows);
  ASSERT_TRUE(bool(Strict)) << llvm::toString(Strict.takeError());
  ASSERT_TRUE(Strict->Process.Windows);
  EXPECT_FALSE(Strict->Process.Windows->DeferUnmodeled);

  auto Snapshot = unpackOptionsFromJSON("{\"snapshot_only\":true}");
  ASSERT_TRUE(bool(Snapshot)) << llvm::toString(Snapshot.takeError());
  EXPECT_TRUE(Snapshot->SnapshotOnly);
  for (const char *Invalid :
       {"{\"snapshot_only\":null}", "{\"snapshot_only\":1}",
        "{\"snapshot_only\":\"true\"}"}) {
    auto Rejected = unpackOptionsFromJSON(Invalid);
    EXPECT_FALSE(bool(Rejected)) << Invalid;
    llvm::consumeError(Rejected.takeError());
  }

  for (const char *Invalid : {UnknownOption, ZeroTransfer, "[]", ""}) {
    auto Rejected = unpackOptionsFromJSON(Invalid);
    EXPECT_FALSE(bool(Rejected)) << Invalid;
    llvm::consumeError(Rejected.takeError());
  }
}

TEST(UnpackOptions, TransferPositionIsBounded) {
  UnpackOptions Options;
  Options.Transfer = defaults::MaxTransfers + 1;
  auto Result = unpackFile(fixture(PlainPacked), Options);
  EXPECT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
}

TEST(UnpackReport, DescribesTheRunWithoutTheImageBytes) {
  std::optional<UnpackResult> Result;
  for (const auto &B : Backends)
    if ((Result = unpackOn(B.Kind, PlainPacked)))
      break;
  if (!Result)
    GTEST_SKIP() << NoBackend;
  auto Parsed = llvm::json::parse(unpackResultJSON(*Result, ReportOutput));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Root = Parsed->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getInteger(key::SchemaVersion), int64_t(SchemaVersion));
  EXPECT_EQ(Root->getString(key::Format), SupportedFormat);
  EXPECT_EQ(Root->getString(key::Architecture), X64Architecture);
  EXPECT_EQ(Root->getString(key::Outcome),
            unpackOutcomeName(UnpackOutcome::Unpacked));
  EXPECT_EQ(Root->getString(key::EntrySource),
            entrySourceName(EntrySource::Transfer));
  EXPECT_EQ(Root->getString(key::EntryRVA),
            llvm::utohexstr(*Result->EntryRVA, true));
  const auto *Packer = Root->getObject(key::Packer);
  ASSERT_NE(Packer, nullptr);
  EXPECT_EQ(Packer->getString(key::Kind),
            packerKindName(PackerKind::Unidentified));
  EXPECT_EQ(Packer->getArray(key::Evidence)->size(),
            Result->Packer.Evidence.size());
  EXPECT_EQ(Root->getArray(key::Transfers)->size(), Result->Transfers.size());
  EXPECT_EQ(Root->getArray(key::Imports)->size(), Result->Imports.size());
  EXPECT_EQ(Root->getArray(key::Sections)->size(), Result->Sections.size());
  const auto *Output = Root->getObject(key::Output);
  ASSERT_NE(Output, nullptr);
  EXPECT_EQ(Output->getString(key::Path), ReportOutput);
  EXPECT_EQ(Output->getInteger(key::Size), int64_t(Result->Image.size()));
  EXPECT_EQ(Output->getString(key::SHA256)->size(), DigestCharacters);
  const auto *Execution = Root->getObject(key::Execution);
  ASSERT_NE(Execution, nullptr);
  EXPECT_EQ(Execution->getString(key::StopReason), StopObserver);
  EXPECT_EQ(Execution->getString(key::Profile), ProcessProfile);

  UnpackResult Empty;
  auto Without = llvm::json::parse(unpackResultJSON(Empty));
  ASSERT_TRUE(bool(Without)) << llvm::toString(Without.takeError());
  EXPECT_EQ(Without->getAsObject()->getString(key::Outcome),
            unpackOutcomeName(UnpackOutcome::NoEntry));
  EXPECT_NE(Without->getAsObject()->get(key::EntryRVA), nullptr);
  EXPECT_FALSE(Without->getAsObject()->getString(key::EntryRVA));
  EXPECT_FALSE(Without->getAsObject()->getObject(key::Output));
}
} // namespace
} // namespace neverd::unpack
