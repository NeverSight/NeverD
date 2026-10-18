//===- AndroidFDSetTests.cpp - Checked Bionic descriptor bitmap calls -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate LP64 fd_set bytes, FORTIFY ordering and native provider lifetimes.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000, Set = Buffer + 4096, End = Set + 4096;
constexpr std::array<const char *, 3> Names = {"__FD_SET_chk", "__FD_CLR_chk",
                                               "__FD_ISSET_chk"};

class AndroidFDSet : public testing::TestWithParam<
                         std::tuple<const char *, ExecutionBackendKind>> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android cross compiler unavailable";
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
    Options.Android->TraceLimit = 8192;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.Android->Libraries["libfdset.so"] = {Names[0], Names[1], Names[2]};
    Options.InstructionQuantum = 7;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Arguments) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Arguments);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R)
      llvm::report_fatal_error(R.takeError());
    return std::move(*R);
  }
  void check(const ProcessResult &R, const std::vector<uint8_t> &Expected) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  void rejected(const ProcessResult &R, unsigned Mode, const char *Diagnostic) {
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find(Diagnostic), std::string::npos);
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Names[Mode]);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
  std::vector<uint8_t> expected(uint64_t Result) {
    auto Bytes = Options.Android->Memory[0].Bytes;
    llvm::support::endian::write64le(Bytes.data(), Result);
    llvm::support::endian::write64le(Bytes.data() + 8, 73);
    llvm::support::endian::write64le(Bytes.data() + 16, 0x12345678);
    return Bytes;
  }
};

TEST_P(AndroidFDSet, WholeBitmapAndCanariesAcrossWordBoundaries) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  std::fill(Bytes.begin() + 4096, Bytes.begin() + 4096 + 128, 0);
  auto Expected = Bytes;
  // Independent byte-level oracle, without host fd_set or the model's mask
  // math.
  Expected[4096] = 1;
  Expected[4104] = 1;
  Expected[4223] = 128;
  const std::array<uint64_t, 6> Words = {1, 1, 1, 1, 0, 73};
  for (unsigned I = 0; I < Words.size(); ++I)
    llvm::support::endian::write64le(Expected.data() + I * 8, Words[I]);
  check(run("fdset_sequence", {Buffer, Set}), Expected);
}

TEST_P(AndroidFDSet, StaticNamedFullWidthExtentsAndBooleanResults) {
  for (unsigned Dynamic : {0u, 4u})
    for (unsigned Mode = 0; Mode != 3; ++Mode)
      for (uint64_t Size : {uint64_t(128), uint64_t(0x100000080), UINT64_MAX}) {
        auto Expected = expected(Mode == 2 ? 1 : 0);
        // Bit 1023 is already set; bit 1022 is initially clear in 0xa5.
        Expected[4223] = Mode == 0 ? 0xe5 : Mode == 1 ? 0x25 : 0xa5;
        auto R = run("fdset_call", {Buffer, Mode == 0 ? 1022u : 1023u, Set,
                                    Size, Dynamic | Mode});
        check(R, Expected);
        if (Dynamic) {
          unsigned Count = 0;
          for (const auto &C : R.NativeCalls)
            if (C.Name == Names[Mode]) {
              EXPECT_EQ(C.Library, "libfdset.so");
              ++Count;
            }
          EXPECT_EQ(Count, 1u);
        }
      }
  check(run("fdset_call", {Buffer, 1, Set, 128, 2}), expected(0));
}

TEST_P(AndroidFDSet, OnlyTheSelectedWordIsReadOrModified) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    auto Expected = expected(Mode == 2 ? 1 : 0);
    Expected[8184] = Mode == 0 ? 0xa7 : Mode == 1 ? 0xa4 : 0xa5;
    check(run("fdset_call", {Buffer, Mode == 0 ? 1u : 0u, End - 8, 128, Mode}),
          Expected);
  }
  // An unaligned bitmap remains a byte-memory operation on AArch64.
  auto Expected = expected(0);
  Expected[4097] = 0xa7;
  check(run("fdset_call", {Buffer, 1, Set + 1, 128, 0}), Expected);
}

TEST_P(AndroidFDSet, FortifyChecksIndexThenObjectExtentBeforePointer) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    for (uint64_t FD : {UINT64_MAX, uint64_t(1024), uint64_t(0x80000000)})
      rejected(run("fdset_call", {Buffer, FD, 0, 0, Mode}), Mode,
               "FORTIFY fd_set descriptor out of range");
    for (uint64_t Size : {0u, 8u, 127u})
      rejected(run("fdset_call", {Buffer, 0, 0, Size, Mode}), Mode,
               "FORTIFY fd_set object extent too small");
    rejected(run("fdset_call", {Buffer, 1023, Set, 127, Mode}), Mode,
             "FORTIFY fd_set object extent too small");
  }
}

TEST_P(AndroidFDSet, InaccessibleAndWrappingWordPointersStop) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    for (uint64_t Address : {uint64_t(0), End, End - 7, UINT64_MAX})
      rejected(run("fdset_call", {Buffer, 0, Address, 128, Mode}), Mode,
               "invalid guest pointer");
    rejected(run("fdset_call", {Buffer, 1023, UINT64_MAX - 7, 128, Mode}), Mode,
             "invalid guest pointer");
  }
}

TEST_P(AndroidFDSet, QueryAcceptsReadonlyMemoryButMutationDoesNot) {
  check(run("fdset_call", {Buffer, 1023, Set, 128, 18}), expected(1));
  for (unsigned Mode : {0u, 1u})
    rejected(run("fdset_call", {Buffer, 1023, Set, 128, 16 | Mode}), Mode,
             "invalid guest pointer");
}

TEST_P(AndroidFDSet, IndexUsesSignedLow32Bits) {
  auto R = run("fdset_wide_index", {0x12345678000003ff, Set, 128});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 1u);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Options.Android->Memory[0].Bytes);
  rejected(run("fdset_wide_index", {0x12345678ffffffff, Set, 128}), 2,
           "FORTIFY fd_set descriptor out of range");
}

TEST_P(AndroidFDSet, ClosedProvidersCannotModifyOrQueryBitmaps) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    auto R = run("fdset_call", {Buffer, 0, Set, 128, Mode | 12});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Names[Mode]);
    EXPECT_FALSE(R.NativeCalls.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Options.Android->Memory[0].Bytes);
  }
}

TEST_P(AndroidFDSet, RawAndBionicPselectShareKernelStateAndErrnoBoundary) {
  Options.LinuxFiles.emplace().Files["/select-input"] = {1, 2};
  for (unsigned Mode : {0u, 1u}) {
    auto Expected = Options.Android->Memory[0].Bytes;
    const std::array<uint64_t, 7> Words = {
        2, 73, 1, 1, 0, Mode ? UINT64_MAX : uint64_t(-9), Mode ? 9u : 73u};
    for (unsigned I = 0; I != Words.size(); ++I)
      llvm::support::endian::write64le(Expected.data() + 8 * I, Words[I]);
    check(run("fdset_select", {Buffer, Mode}), Expected);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Native, AndroidFDSet,
    testing::Combine(testing::Values("fdset-O0-none", "fdset-O0-android",
                                     "fdset-O0-relr", "fdset-O2-none",
                                     "fdset-O2-android", "fdset-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
