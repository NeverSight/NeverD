//===- AndroidAllocationTests.cpp - Explicit allocation contents ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check chosen heap contents separately from zero-initialized allocations,
/// realloc preservation, allocator padding and failure ownership.
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x22000000;
class AndroidAllocation : public testing::TestWithParam<
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
    Options.Android->EntrySymbol = "allocation_contents";
    Options.Android->Arguments = {Buffer};
    Options.Android->Memory.push_back(
        {Buffer, 12288, std::vector<uint8_t>(12288, 0xc7), false});
    Options.Android->ReadMemory = {{Buffer, 12288}};
    Options.InstructionQuantum = 17;
#endif
  }
};

TEST_P(AndroidAllocation, FillChoicePreservesCallocPrefixesAndFailedRealloc) {
  for (uint8_t Fill : {0, 1, 165, 255}) {
    SCOPED_TRACE(unsigned(Fill));
    Options.Android->MallocFillByte = Fill;
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
    EXPECT_EQ(R->ReturnValue, 0u);
    ASSERT_EQ(R->MemorySnapshots.size(), 1u);
    std::vector<uint8_t> Expected(12288, 0xc7);
    std::fill_n(Expected.begin(), 8214, Fill);
    for (unsigned I = 0; I < 8; ++I)
      Expected[4099 + I] = 0x30 + I;
    std::fill_n(Expected.begin() + 8214, 21, 0);
    for (unsigned I = 0; I < 5; ++I) {
      Expected[8235 + I] = 0x30 + I;
      Expected[8251 + I] = 0x30 + I;
    }
    std::fill_n(Expected.begin() + 8240, 11, Fill);
    EXPECT_EQ(R->MemorySnapshots[0].Bytes, Expected);
    // Model-only allocation fill does not invent native calls or raw syscalls.
    EXPECT_TRUE(R->Services.empty());
    size_t Frees = 0;
    for (const auto &Call : R->NativeCalls) {
      ASSERT_TRUE(Call.Result) << Call.Name;
      Frees += Call.Name == "free";
    }
    EXPECT_EQ(Frees, 5u);
  }
}

TEST_P(AndroidAllocation, RequestedBytesExcludeMappedPagePadding) {
  Options.Android->MallocFillByte = 0x69;
  Options.Android->EntrySymbol = "allocation_hold";
  Options.Android->Arguments.clear();
  constexpr uint64_t FirstAllocation = 0x100000000;
  Options.Android->ReadMemory = {{FirstAllocation, 8192, false}};
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
  EXPECT_EQ(R->ReturnValue, FirstAllocation);
  ASSERT_EQ(R->MemorySnapshots.size(), 1u);
  std::vector<uint8_t> Expected(8192);
  std::fill_n(Expected.begin(), 4099, 0x69);
  EXPECT_EQ(R->MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidAllocation, ReallocChecksOwnershipBeforeReadingStorage) {
  Options.Android->EntrySymbol = "allocation_foreign";
  Options.Android->ObserveMemoryOnFailure = true;
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Buffer, Mode};
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_NE(R->Diagnostic.find(
                  Mode == 4 ? "invalid guest pointer"
                            : "realloc does not name a live allocation"),
              std::string::npos)
        << R->Diagnostic;
    ASSERT_EQ(R->MemorySnapshots.size(), 1u);
    EXPECT_EQ(R->MemorySnapshots[0].Bytes, std::vector<uint8_t>(12288, 0xc7));
  }
}

TEST(AndroidAllocationOptions, ByteRangeAndDefaultAreExplicit) {
  auto Default = processOptionsFromJSON(R"({"android":{}})");
  ASSERT_TRUE(bool(Default)) << llvm::toString(Default.takeError());
  EXPECT_EQ(Default->Android->MallocFillByte, 0u);
  for (const char *Value : {"0", "255", "\"0xa5\""}) {
    auto O = processOptionsFromJSON(
        std::string("{\"android\":{\"malloc_fill_byte\":") + Value + "}}");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    EXPECT_EQ(O->Android->MallocFillByte, std::string(Value) == "0"     ? 0
                                          : std::string(Value) == "255" ? 255
                                                                        : 165);
  }
  for (const char *Value : {"-1", "256", "true", "null", "1.5", "{}", "[]",
                            "\"0x100\"", "\"bad\"", "18446744073709551615"}) {
    auto O = processOptionsFromJSON(
        std::string("{\"android\":{\"malloc_fill_byte\":") + Value + "}}");
    EXPECT_FALSE(bool(O)) << Value;
    llvm::consumeError(O.takeError());
  }
}

INSTANTIATE_TEST_SUITE_P(
    Relocations, AndroidAllocation,
    testing::Combine(testing::Values("allocation-O0-none",
                                     "allocation-O0-android",
                                     "allocation-O0-relr", "allocation-O2-none",
                                     "allocation-O2-android",
                                     "allocation-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF)));
} // namespace
} // namespace neverd::emulation
