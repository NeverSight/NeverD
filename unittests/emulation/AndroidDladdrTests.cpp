//===- AndroidDladdrTests.cpp - Explicit LP64 linker observations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent guest callers check module identity, borrowed strings and
/// faults.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/Limits.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000, Query = 0x40001004;
constexpr uint64_t ImageBase = 0x40000000, SymbolAddress = 0x40001000;
constexpr size_t SnapshotBytes = 640, RegionBytes = 8192;
constexpr uint8_t Canary = 0xa5;
constexpr char ImageName[] = "/guest/fixture.so", SymbolName[] = "symbol";
class AndroidDladdr : public testing::TestWithParam<
                          std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->ThreadLimit = 2;
    Options.Android->ObserveMemoryOnFailure = true;
    Options.Android->Memory.push_back(
        {Buffer, RegionBytes, std::vector<uint8_t>(RegionBytes, Canary),
         false});
    Options.Android->ReadMemory = {{Buffer, SnapshotBytes}};
    Options.Android->DladdrObservations = std::vector<NativeDladdrObservation>{
        {Query, ImageBase, ImageName, SymbolName, SymbolAddress},
        {0x40002004, ImageBase, ImageName, std::nullopt, 0}};
    Options.Android->Libraries["libfixture-dl.so"] = {"dladdr"};
    Options.InstructionQuantum = 7;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {ProcessProfile::AndroidNativeAArch64,
              GuestArchitecture::AArch64,
              Options.Backend,
              {}};
    }
    return std::move(*R);
  }
  void returned(const ProcessResult &R) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    ASSERT_EQ(R.ReturnValue, 0u);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    const auto &Bytes = R.MemorySnapshots.front().Bytes;
    ASSERT_EQ(Bytes.size(), SnapshotBytes);
    const uint64_t Name = llvm::support::endian::read64le(Bytes.data() + 64);
    const uint64_t Symbol = llvm::support::endian::read64le(Bytes.data() + 80);
    EXPECT_NE(Name, 0u);
    EXPECT_NE(Symbol, 0u);
    EXPECT_NE(Name, Symbol);
    std::vector<uint8_t> Expected(SnapshotBytes, Canary);
    llvm::support::endian::write64le(Expected.data(), 1);
    for (unsigned Index = 0; Index != 3; ++Index) {
      auto *Info = Expected.data() + 64 + Index * 32;
      llvm::support::endian::write64le(Info, Name);
      llvm::support::endian::write64le(Info + 8, ImageBase);
      llvm::support::endian::write64le(Info + 16, Index == 2 ? 0 : Symbol);
      llvm::support::endian::write64le(Info + 24,
                                       Index == 2 ? 0 : SymbolAddress);
    }
    std::copy_n(ImageName, sizeof(ImageName), Expected.begin() + 512);
    std::copy_n(SymbolName, sizeof(SymbolName), Expected.begin() + 576);
    EXPECT_EQ(Bytes, Expected);
    unsigned Queries = 0;
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "dladdr") {
        ++Queries;
        EXPECT_EQ(Call.Result, 1u);
      }
    EXPECT_EQ(Queries, 3u);
  }
  void untouched(const ProcessResult &R, size_t Bytes = SnapshotBytes) {
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots.front().Bytes,
              std::vector<uint8_t>(Bytes, Canary));
  }
};

TEST_P(AndroidDladdr, StaticAndDynamicQueriesPreserveABIErrorAndStringState) {
  for (uint64_t Mode : {0u, 1u}) {
    SCOPED_TRACE(Mode);
    Options.Android->Libraries.clear();
    if (Mode)
      Options.Android->Libraries["libfixture-dl.so"] = {"dladdr"};
    ASSERT_NO_FATAL_FAILURE(returned(run("dladdr_fixture", {Buffer, Mode})));
  }
}
TEST_P(AndroidDladdr, ThreadsShareBorrowedStringIdentities) {
  auto R = run("dladdr_fixture", {Buffer, 3});
  ASSERT_NO_FATAL_FAILURE(returned(R));
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  for (const auto &T : R.NativeThreads)
    EXPECT_TRUE(T.Finished);
}
TEST_P(AndroidDladdr, ClosingTheProviderInvalidatesItsBorrowedFunction) {
  auto R = run("dladdr_fixture", {Buffer, 2});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  untouched(R);
}
TEST_P(AndroidDladdr, ResidentProvidersSurviveClosingTheirHandle) {
  Options.Android->DefaultScope = std::vector<std::string>{"libfixture-dl.so"};
  returned(run("dladdr_fixture", {Buffer, 2}));
}
TEST_P(AndroidDladdr, UnknownAddressesAndAbsentRecordsDoNotGuessFailure) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    SCOPED_TRACE(Mode);
    if (Mode == 1)
      Options.Android->DladdrObservations->clear();
    if (Mode == 2)
      Options.Android->DladdrObservations.reset();
    auto R = run("dladdr_call", {Query + 1, Buffer});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("explicit successful address observation"),
              std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "dladdr");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    untouched(R);
  }
}
TEST_P(AndroidDladdr, InvalidOutputExtentsDoNotPublishPartialInfo) {
  Options.Android->ReadMemory = {{Buffer + RegionBytes - 48, 48}};
  for (uint64_t Address :
       {uint64_t(0), Buffer + RegionBytes - 16, UINT64_MAX - 15}) {
    SCOPED_TRACE(Address);
    auto R = run("dladdr_call", {Query, Address});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    untouched(R, 48);
  }
}
TEST_P(AndroidDladdr, ReadOnlyTailDoesNotPublishAValidPrefix) {
  Options.Android->ReadMemory = {{Buffer + 4096 - 32, 64}};
  auto R = run("dladdr_readonly_tail", {Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  untouched(R, 64);
}
TEST_P(AndroidDladdr, ValidCrossPageOutputPreservesBothGuards) {
  Options.Android->ReadMemory = {{Buffer + 4096 - 32, 64}};
  auto R = run("dladdr_call", {Query, Buffer + 4096 - 16});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 1u);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const auto &Bytes = R.MemorySnapshots.front().Bytes;
  ASSERT_EQ(Bytes.size(), 64u);
  std::vector<uint8_t> Expected(64, Canary);
  const uint64_t Name = llvm::support::endian::read64le(Bytes.data() + 16);
  const uint64_t Symbol = llvm::support::endian::read64le(Bytes.data() + 32);
  EXPECT_NE(Name, 0u);
  EXPECT_NE(Symbol, 0u);
  llvm::support::endian::write64le(Expected.data() + 16, Name);
  llvm::support::endian::write64le(Expected.data() + 24, ImageBase);
  llvm::support::endian::write64le(Expected.data() + 32, Symbol);
  llvm::support::endian::write64le(Expected.data() + 40, SymbolAddress);
  EXPECT_EQ(Bytes, Expected);
}
TEST_P(AndroidDladdr, BorrowedStringsAreReadOnlyToGuestStores) {
  auto R = run("dladdr_fixture", {Buffer, 4});
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const auto &Bytes = R.MemorySnapshots.front().Bytes;
  EXPECT_TRUE(std::all_of(Bytes.begin() + 128, Bytes.end(),
                          [](uint8_t B) { return B == Canary; }));
}
TEST_P(AndroidDladdr, ChangedOrUnmappedStringsCannotBecomeInvisibleFallbacks) {
  for (uint64_t Mode : {5u, 6u}) {
    SCOPED_TRACE(Mode);
    auto R = run("dladdr_fixture", {Buffer, Mode});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "dladdr");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    const auto &Bytes = R.MemorySnapshots.front().Bytes;
    EXPECT_TRUE(std::all_of(Bytes.begin() + 128, Bytes.end(),
                            [](uint8_t B) { return B == Canary; }));
  }
}

TEST(AndroidDladdrOptions, CountStringAndAggregateLimitsShareOnePolicy) {
  namespace limits = ::neverd::limits::emulation;
  auto Parse = [](unsigned Count, unsigned Length, bool Symbols) {
    llvm::json::Array Records;
    for (unsigned I = 0; I != Count; ++I)
      Records.push_back(llvm::json::Object{
          {"address", I + 2},
          {"image_base", 0},
          {"image_name", std::string(Length, 'x')},
          {"symbol_name", Symbols ? llvm::json::Value(std::string(Length, 's'))
                                  : llvm::json::Value(nullptr)},
          {"symbol_address", Symbols ? 1 : 0}});
    llvm::json::Value V(llvm::json::Object{
        {"android",
         llvm::json::Object{{"entry_symbol", "dladdr_call"},
                            {"dladdr_observations", std::move(Records)}}}});
    return processOptionsFromJSON(llvm::formatv("{0}", V).str());
  };
  auto Count = Parse(limits::kAndroidDladdrObservationCount, 1, false);
  ASSERT_TRUE(bool(Count)) << llvm::toString(Count.takeError());
  auto Length = Parse(1, limits::kAndroidDladdrStringBytes, false);
  ASSERT_TRUE(bool(Length)) << llvm::toString(Length.takeError());
  for (auto [N, L, Symbols] :
       {std::tuple{limits::kAndroidDladdrObservationCount + 1, 1u, false},
        std::tuple{1u, limits::kAndroidDladdrStringBytes + 1, false},
        std::tuple{limits::kAndroidDladdrObservationCount,
                   limits::kAndroidDladdrStringBytes, true}}) {
    auto Invalid = Parse(N, L, Symbols);
    EXPECT_FALSE(bool(Invalid));
    llvm::consumeError(Invalid.takeError());
  }
}

INSTANTIATE_TEST_SUITE_P(
    OptimizationPackingAndBackend, AndroidDladdr,
    testing::Combine(testing::Values("dladdr-O0-none", "dladdr-O0-android",
                                     "dladdr-O0-relr", "dladdr-O2-none",
                                     "dladdr-O2-android", "dladdr-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF)),
    [](const testing::TestParamInfo<AndroidDladdr::ParamType> &Info) {
      std::string Name = std::get<0>(Info.param);
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name + "_" + executionBackendName(std::get<1>(Info.param));
    });
} // namespace
} // namespace neverd::emulation
