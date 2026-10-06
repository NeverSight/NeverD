//===- UnpackExecutionTests.cpp - Recovery by observing execution ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnpackTestSupport.h"

#include "neverd/emulation/ProcessSession.h"
#include "neverd/unpack/Unpack.h"

#include "llvm/ADT/StringExtras.h"
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
};
constexpr Fixture Fixtures[] = {
#define NEVERD_UNPACK_TEST_FIXTURE(Name, Packed, Original)                     \
  {#Name, Packed, Original},
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

TEST_P(UnpackFixture, RecoversEntrySectionsAndImports) {
  const auto &[Transport, F] = GetParam();
  (void)Transport;
  auto Result = unpackOn(Transport.Kind, F.Packed);
  if (!Result) {
    if (!HasFailure())
      GTEST_SKIP() << BackendUnavailable;
    return;
  }
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked) << Result->Diagnostic;
  EXPECT_EQ(Result->Packer.Kind, PackerKind::UPX);
  EXPECT_EQ(Result->ProcessStop, StopObserver);
  const Image Original = readImage(fixture(F.Original));
  const Image Rebuilt = readImage(Result->Image);
  ASSERT_FALSE(HasFailure());
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
  auto Result = unpack(RuntimePacked);
  NEVERD_REQUIRE(Result);
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked) << Result->Diagnostic;
  const Image Original = readImage(fixture(RuntimeOriginal));
  // The stub calls the program's TLS callbacks before it leaves. That call
  // enters generated code on a deeper stack and is reported, not accepted.
  ASSERT_EQ(Result->Transfers.size(), 1u);
  EXPECT_FALSE(Result->Transfers[0].StackBalanced);
  EXPECT_NE(Result->Transfers[0].RVA, Original.Entry);
  EXPECT_EQ(Result->Source, EntrySource::Stub);
  EXPECT_EQ(Result->EntryRVA, Original.Entry);
  // An image that starts at the program must name the program's own TLS
  // directory again, or its callbacks would never run.
  const Image Rebuilt = readImage(Result->Image);
  const auto Expected = Original.directory(llvm::COFF::TLS_TABLE);
  const auto Actual = Rebuilt.directory(llvm::COFF::TLS_TABLE);
  ASSERT_NE(Expected.RelativeVirtualAddress, 0u);
  EXPECT_EQ(Actual.RelativeVirtualAddress, Expected.RelativeVirtualAddress);
  EXPECT_EQ(Actual.Size, Expected.Size);
  EXPECT_NE(readImage(fixture(RuntimePacked))
                .directory(llvm::COFF::TLS_TABLE)
                .RelativeVirtualAddress,
            Expected.RelativeVirtualAddress);
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
  // Following the stub's call into the runtime's TLS callback reaches an
  // import outside the process model. The run names it and stops.
  UnpackOptions Options;
  Options.Transfer = 2;
  auto Result = unpack(RuntimePacked, Options);
  NEVERD_REQUIRE(Result);
  EXPECT_EQ(Result->Outcome, UnpackOutcome::NoEntry);
  EXPECT_EQ(Result->ProcessStop, StopUnsupportedService);
  EXPECT_NE(Result->ProcessDiagnostic.find(KernelModule), std::string::npos);
  ASSERT_EQ(Result->Transfers.size(), 1u);
  EXPECT_TRUE(Result->Image.empty());
}

INSTANTIATE_TEST_SUITE_P(Backends, Unpack, testing::ValuesIn(Backends),
                         [](const auto &Info) {
                           return std::string(Info.param.Name);
                         });

TEST(UnpackBackends, ProduceIdenticalImages) {
  for (const auto &F : Fixtures) {
    SCOPED_TRACE(F.Name);
    std::map<std::string, std::vector<uint8_t>> Images;
    for (const auto &B : Backends)
      if (auto Result = unpackOn(B.Kind, F.Packed)) {
        ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked);
        Images[B.Name] = std::move(Result->Image);
      }
    if (Images.size() < 2)
      GTEST_SKIP() << SingleBackend;
    for (const auto &[Name, Bytes] : Images)
      EXPECT_EQ(Bytes, Images.begin()->second)
          << Name << " differs from " << Images.begin()->first;
  }
}

TEST(UnpackOptions, DefaultsDeferUnmodeledLoaderFacts) {
  const UnpackOptions Options;
  EXPECT_EQ(Options.Transfer, 0u);
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
  EXPECT_EQ(Packer->getString(key::Kind), packerKindName(PackerKind::UPX));
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
