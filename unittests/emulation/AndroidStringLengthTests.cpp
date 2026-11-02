//===- AndroidStringLengthTests.cpp - Bionic length and fortify boundaries
//--===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks scan-first fortify, shared length semantics and terminal failures.

#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
constexpr uint64_t SecondPage = Buffer + 4096;
constexpr uint64_t End = Buffer + 8192;
constexpr std::array<const char *, 3> LengthNames = {"strlen", "strnlen",
                                                     "__strlen_chk"};

class AndroidStringLength
    : public testing::TestWithParam<
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
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->TraceLimit = 8192;
    Options.Android->Memory.push_back({Buffer, 8192, {}, false});
    Options.Android->ReadMemory = {{Buffer, 8192}};
    Options.InstructionQuantum = 7;
#endif
  }

  ProcessResult run(const char *Entry, std::vector<uint64_t> Arguments) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Arguments);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }

  void returned(const ProcessResult &R, uint64_t Value,
                const std::vector<uint8_t> &Bytes) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
  }

  void rejected(const ProcessResult &R, const char *Name,
                llvm::StringRef Diagnostic) {
    ASSERT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find(Diagnostic.str()), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Name);
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_FALSE(R.ReturnValue);
    EXPECT_TRUE(R.MemorySnapshots.empty());
  }
};

TEST_P(AndroidStringLength, ByteLengthsAndErrnoMatchOrdinaryLength) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  for (unsigned Length : {0u, 1u, 3u, 255u}) {
    SCOPED_TRACE(Length);
    Bytes.assign(8192, 0xa5);
    // Every nonzero byte occurs in the longest input, including high bytes.
    for (unsigned I = 0; I < Length; ++I)
      Bytes[I] = I + 1;
    Bytes[Length] = 0;
    auto Expected = Bytes;
    llvm::support::endian::write32le(Expected.data() + 2048, 733);
    for (unsigned Mode = 0; Mode < LengthNames.size(); ++Mode) {
      SCOPED_TRACE(LengthNames[Mode]);
      auto R =
          run("length_supplied", {Buffer, Length + 1, Mode, Buffer + 2048});
      returned(R, Length, Expected);
      EXPECT_TRUE(R.Services.empty());
      auto Call = std::find_if(
          R.NativeCalls.begin(), R.NativeCalls.end(),
          [&](const auto &C) { return C.Name == LengthNames[Mode]; });
      ASSERT_NE(Call, R.NativeCalls.end());
      EXPECT_EQ(Call->Result, Length);
      EXPECT_EQ(Call->Arguments[0], Buffer);
      if (Mode)
        EXPECT_EQ(Call->Arguments[1], Length + 1);
    }
  }
}

TEST_P(AndroidStringLength, FortifyRequiresTheTerminatorInsideTheObject) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[0] = 'a';
  Bytes[1] = 0x80;
  Bytes[2] = 0xff;
  Bytes[3] = 0;
  for (uint64_t Bound : {0u, 1u, 2u, 3u})
    rejected(run("length_raw", {Buffer, Bound, 2}), "__strlen_chk",
             "FORTIFY read exceeds object bounds in __strlen_chk");
  returned(run("length_raw", {Buffer, 4, 2}), 3, Bytes);
  rejected(run("length_raw", {Buffer + 3, 0, 2}), "__strlen_chk", "FORTIFY");
  returned(run("length_raw", {Buffer + 3, 1, 2}), 0, Bytes);
}

TEST_P(AndroidStringLength, FullWidthExtentDoesNotBoundTheMemoryScan) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[0] = 'a';
  Bytes[1] = 'b';
  Bytes[2] = 'c';
  Bytes[3] = 0;
  for (uint64_t Bound : {uint64_t(0x80000000), uint64_t(0x100000000),
                         uint64_t(0x100000003), UINT64_MAX}) {
    SCOPED_TRACE(Bound);
    returned(run("length_raw", {Buffer, Bound, 2}), 3, Bytes);
  }
}

TEST_P(AndroidStringLength, ReadOnlyCrossPageAndPageTailNulStopAllScans) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[4097] = 0;
  Bytes[8191] = 0;
  for (unsigned Mode = 0; Mode < LengthNames.size(); ++Mode) {
    returned(run("length_protected",
                 {Buffer, 8192, 1, SecondPage - 2, UINT64_MAX, Mode}),
             3, Bytes);
    returned(
        run("length_protected", {Buffer, 8192, 1, End - 3, UINT64_MAX, Mode}),
        2, Bytes);
    returned(run("length_protected", {Buffer, 8192, 1, End - 1, 1, Mode}), 0,
             Bytes);
  }
}

TEST_P(AndroidStringLength, InvalidReadsPrecedeEvenAnExhaustedObjectBound) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 'x');
  for (uint64_t Address : {uint64_t(0), uint64_t(1), UINT64_MAX, End - 1}) {
    for (uint64_t Bound : {uint64_t(0), uint64_t(1), UINT64_MAX}) {
      SCOPED_TRACE(testing::Message() << Address << ":" << Bound);
      auto R = run("length_raw", {Address, Bound, 2});
      rejected(R, "__strlen_chk", "invalid guest pointer");
      EXPECT_EQ(R.Diagnostic.find("FORTIFY"), std::string::npos);
    }
  }
  rejected(run("length_protected", {Buffer, 4096, 0, Buffer, 0, 2}),
           "__strlen_chk", "invalid guest pointer");
}

TEST_P(AndroidStringLength, BoundedLengthRetainsZeroAndNonterminatingBehavior) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 'x');
  for (uint64_t Address : {uint64_t(0), UINT64_MAX, End})
    returned(run("length_raw", {Address, 0, 1}), 0, Bytes);
  returned(run("length_raw", {End - 1, 1, 1}), 1, Bytes);
  rejected(run("length_raw", {End - 1, 2, 1}), "strnlen",
           "invalid guest pointer");
  rejected(run("length_raw", {End - 1, 0, 0}), "strlen",
           "invalid guest pointer");
}

TEST_P(AndroidStringLength, DynamicProviderPreservesResultsAndFortifyOrder) {
  Options.Android->Libraries = {{"liblength-model.so", {"__strlen_chk"}}};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[3] = 0;
  auto R = run("length_dynamic", {Buffer, 4, 0});
  returned(R, 3, Bytes);
  uint64_t Target = 0;
  unsigned Calls = 0;
  for (const auto &Call : R.NativeCalls) {
    if (Call.Name == "dlsym") {
      EXPECT_EQ(Call.Library, "liblength-model.so");
      EXPECT_EQ(Call.Symbol, "__strlen_chk");
      ASSERT_TRUE(Call.Result);
      Target = *Call.Result;
    }
    if (Call.Name == "__strlen_chk") {
      EXPECT_EQ(Call.Library, "liblength-model.so");
      EXPECT_EQ(Call.PC, Target);
      EXPECT_EQ(Call.Arguments[0], Buffer);
      EXPECT_EQ(Call.Arguments[1], 4u);
      EXPECT_EQ(Call.Result, 3u);
      ++Calls;
    }
  }
  EXPECT_NE(Target, 0u);
  EXPECT_EQ(Calls, 1u);
  rejected(run("length_dynamic", {Buffer, 3, 0}), "__strlen_chk", "FORTIFY");
  rejected(run("length_dynamic", {UINT64_MAX, 0, 0}), "__strlen_chk",
           "invalid guest pointer");
}

TEST_P(AndroidStringLength, ClosedProviderFailsBeforeStringAccess) {
  Options.Android->Libraries = {{"liblength-model.so", {"__strlen_chk"}}};
  auto R = run("length_dynamic", {UINT64_MAX, 0, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "__strlen_chk");
  EXPECT_EQ(R.NativeCalls.back().Library, "liblength-model.so");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  EXPECT_FALSE(R.ReturnValue);
}

INSTANTIATE_TEST_SUITE_P(
    API28, AndroidStringLength,
    testing::Combine(testing::Values("length-O0-none", "length-O0-android",
                                     "length-O0-relr", "length-O2-none",
                                     "length-O2-android", "length-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));

TEST(AndroidStringLengthMemory, ScanLimitAndDeadlinePrecedeFortify) {
  ExecutionConfiguration Configuration;
  Configuration.Backend = ExecutionBackendKind::Unicorn;
  Configuration.Architecture = GuestArchitecture::AArch64;
  Configuration.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Configuration);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Backend = createExecutionBackend(Configuration, 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  auto &Space = *CPU.addressSpace();
  llvm::cantFail(Space.map(Buffer, 8192, Read | Write | UserAccessible));
  std::vector<uint8_t> Before(8192, 'x');
  Before[8] = 0;
  llvm::cantFail(CPU.write(Buffer, Before));
  llvm::cantFail(Space.protect(Buffer, 8192, Read | UserAccessible));
  ProcessOptions Options;
  Options.Android.emplace();
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, End, Options, Result);
  const android_model::LinkedImage Linked{};
  struct Case {
    unsigned Mode;
    uint64_t Limit, Address, Bound;
    bool Expired;
    const char *Error;
    uint64_t Expected = 0;
  };
  for (const auto C : {
           Case{2, 8, Buffer, 0, false, "scan exceeds memory limit"},
           {2, 8, Buffer, 8, false, "scan exceeds memory limit"},
           {2, 8, Buffer, UINT64_MAX, false, "scan exceeds memory limit"},
           {2, 9, Buffer, 8, false, "FORTIFY"},
           {2, 9, Buffer, 9, false, nullptr, 8},
           {2, 9, Buffer, 0x100000000, false, nullptr, 8},
           {2, 1, Buffer + 8, 0, false, "FORTIFY"},
           {2, 1, Buffer + 8, 1, false, nullptr, 0},
           {2, 0, Buffer + 8, 0, false, "scan exceeds memory limit"},
           {2, 9, Buffer, 0, true, "timed out"},
           {2, 9, Buffer + 8, UINT64_MAX, true, "timed out"},
           {2, 9, UINT64_MAX, 0, false, "invalid guest pointer"},
           {0, 8, Buffer, 0, false, "scan exceeds memory limit"},
           {0, 9, Buffer, 0, false, nullptr, 8},
           {0, 9, Buffer, 0, true, "timed out"},
           {1, 8, Buffer, 8, false, nullptr, 8},
           {1, 8, Buffer, 9, false, "scan exceeds memory limit"},
           {1, 9, Buffer, UINT64_MAX, false, nullptr, 8},
           {1, 0, UINT64_MAX, 0, false, nullptr, 0},
       }) {
    SCOPED_TRACE(testing::Message() << C.Mode << ":" << C.Limit << ":"
                                    << C.Bound << ":" << C.Expired);
    Options.MemoryLimit = C.Limit;
    auto Budget = llvm::cantFail(ExecutionBudget::create(
        Options.Limits, C.Expired ? ExecutionBudget::Clock::time_point::min()
                                  : ExecutionBudget::Clock::now()));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = LengthNames[C.Mode];
    Call.Arguments = {C.Address, C.Bound};
    auto Value = Model.invoke(Call);
    if (C.Error) {
      ASSERT_FALSE(bool(Value));
      EXPECT_NE(llvm::toString(Value.takeError()).find(C.Error),
                std::string::npos);
    } else {
      ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
      ASSERT_TRUE(*Value);
      EXPECT_EQ(std::get<uint64_t>(**Value), C.Expected);
    }
    EXPECT_EQ(Model.timedOut(), C.Expired);
    std::vector<uint8_t> After(Before.size());
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
}
} // namespace
} // namespace neverd::emulation
