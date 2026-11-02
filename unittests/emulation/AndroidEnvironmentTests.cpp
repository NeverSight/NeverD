//===- AndroidEnvironmentTests.cpp - Live guest environment semantics ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent getenv pointer, ordering, lifetime and public input checks.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000, Extent = 8192;
constexpr uint64_t Vector = Buffer + 256, Name = Buffer + 1280;
constexpr uint64_t First = Buffer + 512, Second = Buffer + 544;

class AndroidEnvironment : public testing::TestWithParam<
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
    Options.Android->EnvironmentVector = Vector;
    Options.Android->TraceLimit = 8192;
    Options.Android->Memory.push_back(
        {Buffer, Extent, std::vector<uint8_t>(Extent, 0xa5), false});
    Options.Android->ReadMemory = {{Buffer, Extent}};
    Options.Android->ObserveMemoryOnFailure = true;
    Options.Android->Libraries["libenvironment.so"] = {"getenv"};
    Options.InstructionQuantum = 7;
    text(First, "COLOR=blue");
    text(Second, "COLOR=green");
    text(Buffer + 576, "COLORS=plural");
    text(Buffer + 608, "=empty");
    text(Buffer + 640, "EMPTY=");
    text(Name, "COLOR");
    unsigned I = 0;
    for (uint64_t P :
         {First, Second, Buffer + 576, Buffer + 608, Buffer + 640, uint64_t(0)})
      word(Vector + sizeof(uint64_t) * I++, P);
#endif
  }
  std::vector<uint8_t> &bytes() { return Options.Android->Memory[0].Bytes; }
  void word(uint64_t Address, uint64_t Value) {
    llvm::support::endian::write64le(bytes().data() + Address - Buffer, Value);
  }
  void text(uint64_t Address, llvm::StringRef Value) {
    std::copy(Value.begin(), Value.end(), bytes().begin() + Address - Buffer);
    bytes()[Address - Buffer + Value.size()] = 0;
  }
  ProcessResult run(const char *Entry = "environment_query",
                    uint64_t Argument = Name, uint64_t Mode = 0) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = {Buffer, Argument, Mode};
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  void check(const ProcessResult &R, std::initializer_list<uint64_t> Words,
             std::vector<uint8_t> Expected) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    unsigned I = 0;
    for (uint64_t V : Words)
      llvm::support::endian::write64le(Expected.data() + sizeof(V) * I++, V);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  void fault(const ProcessResult &R) {
    ASSERT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("invalid guest pointer"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "getenv");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, bytes());
  }
};

TEST_P(AndroidEnvironment, FirstExactNameAndBorrowedValuePreserveErrno) {
  const std::array<std::pair<const char *, uint64_t>, 8> Cases = {
      {{"COLOR", First + 6},
       {"COLOR=ignored", First + 6},
       {"COLORS", Buffer + 583},
       {"COL", 0},
       {"EMPTY", Buffer + 646},
       {"", Buffer + 609},
       {"=ignored", Buffer + 609},
       {"MISSING", 0}}};
  for (const auto &[Query, Expected] : Cases) {
    text(Name, Query);
    for (unsigned Mode : {0, 1}) {
      SCOPED_TRACE(testing::Message() << Query << ":" << Mode);
      const auto R = run("environment_query", Name, Mode);
      check(R, {Expected, 77, 0, 0}, bytes());
      const auto C =
          std::find_if(R.NativeCalls.begin(), R.NativeCalls.end(),
                       [](const auto &C) { return C.Name == "getenv"; });
      ASSERT_NE(C, R.NativeCalls.end());
      EXPECT_EQ(C->Library, Mode ? "libenvironment.so" : "");
      EXPECT_EQ(C->Result, Expected);
    }
  }
}

TEST_P(AndroidEnvironment, UnknownNullAndEmptyVectorsRemainDistinct) {
  Options.Android->EnvironmentVector.reset();
  auto R = run("environment_query", UINT64_MAX);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, "getenv requires an explicit environment vector");
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, bytes());
  Options.Android->EnvironmentVector = 0;
  check(run(), {0, 77, 0, 0}, bytes());
  // The API 28 name scan precedes its null-environment check.
  fault(run("environment_query", 0));
  Options.Android->EnvironmentVector = Vector;
  word(Vector, 0);
  check(run(), {0, 77, 0, 0}, bytes());
}

TEST_P(AndroidEnvironment, ReadsOnlyTheNameAndMatchingEntryPrefix) {
  text(Buffer + Extent - 7, "COLOR=");
  // '=' is the final mapped byte; no NUL is required after the query name.
  std::copy_n("COLOR=", 6, bytes().end() - 6);
  check(run("environment_query", Buffer + Extent - 6), {First + 6, 77, 0, 0},
        bytes());
  text(Name, "");
  word(Vector, Buffer + Extent - 1);
  word(Vector + 8, UINT64_MAX);
  bytes().back() = '=';
  // A value can begin at an unreadable address; getenv never reads it.
  check(run(), {Buffer + Extent, 77, 0, 0}, bytes());
  text(Name, "COLOR");
  word(Vector + 8, 0);
  bytes().back() = 'X';
  check(run(), {0, 77, 0, 0}, bytes());
  // An unaligned vector can end with its terminator at the page boundary.
  Options.Android->EnvironmentVector = Buffer + Extent - 17;
  word(Buffer + Extent - 17, First);
  word(Buffer + Extent - 9, 0);
  text(Name, "MISSING");
  check(run(), {0, 77, 0, 0}, bytes());
}

TEST_P(AndroidEnvironment, InvalidNameVectorAndEntryPreserveGuestBytes) {
  for (uint64_t Invalid : {uint64_t(0), uint64_t(1), UINT64_MAX})
    fault(run("environment_query", Invalid));
  for (uint64_t Invalid : {uint64_t(1), UINT64_MAX, Buffer + Extent - 4}) {
    Options.Android->EnvironmentVector = Invalid;
    fault(run());
  }
  Options.Android->EnvironmentVector = Vector;
  word(Vector, UINT64_MAX);
  fault(run());
  word(Vector, Buffer + Extent - 1);
  bytes().back() = 'C';
  fault(run());
}

TEST_P(AndroidEnvironment, VectorAndValuesStayLiveAcrossCalls) {
  auto Expected = bytes();
  Expected[First + 6 - Buffer] = 'R';
  llvm::support::endian::write64le(Expected.data() + Vector - Buffer, 0);
  check(run("environment_mutate", Vector),
        {First + 6, First + 6, 'R', Second + 6, 0}, Expected);
}

TEST_P(AndroidEnvironment, ConstructorsAndThreadsShareTheGuestVector) {
  Options.Android->Initialize = true;
  check(run(), {First + 6, 77, First + 6, 91}, bytes());
  Options.Android->ThreadLimit = 2;
  auto Expected = bytes();
  Expected[First + 6 - Buffer] = 'T';
  check(run("environment_thread"), {First + 6, 88, First + 6, 77, 'T'},
        Expected);
}

TEST_P(AndroidEnvironment, ClosedProviderRefusesBeforeReadingEnvironment) {
  Options.Android->EnvironmentVector.reset();
  auto R = run("environment_query", UINT64_MAX, 2);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "getenv");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST(AndroidEnvironmentOptions, PresenceAndFullWidthPointerUseStrictJSON) {
  auto Absent = processOptionsFromJSON(R"({"android":{}})");
  ASSERT_TRUE(bool(Absent));
  EXPECT_FALSE(Absent->Android->EnvironmentVector);
  for (const auto &[Text, Pointer] :
       std::array<std::pair<const char *, uint64_t>, 3>{
           {{R"({"android":{"environment_vector":0}})", 0},
            {R"({"android":{"environment_vector":4294967297}})", 4294967297},
            {R"({"android":{"environment_vector":"0xffffffffffffffff"}})",
             UINT64_MAX}}}) {
    auto Options = processOptionsFromJSON(Text);
    ASSERT_TRUE(bool(Options)) << llvm::toString(Options.takeError());
    ASSERT_TRUE(Options->Android->EnvironmentVector);
    EXPECT_EQ(*Options->Android->EnvironmentVector, Pointer);
  }
  for (const char *Value :
       {"null", "false", "[]", "{}", "-1", "1.5", "\"0x10000000000000000\""}) {
    auto Options = processOptionsFromJSON(
        std::string("{\"android\":{\"environment_vector\":") + Value + "}}");
    ASSERT_FALSE(bool(Options)) << Value;
    llvm::consumeError(Options.takeError());
  }
}

TEST(AndroidEnvironmentMemory, TotalWorkAndDeadlineNeverBecomeMissingKeys) {
  ExecutionConfiguration C;
  C.Backend = ExecutionBackendKind::Unicorn;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(C);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Backend = llvm::cantFail(createExecutionBackend(C, 4 * 1024 * 1024));
  auto &CPU = *Backend.CPU;
  auto &Space = *CPU.addressSpace();
  llvm::cantFail(Space.map(Buffer, Extent, Read | Write | UserAccessible));
  std::vector<uint8_t> Before(Extent, 0);
  std::copy_n("A", 2, Before.data() + Name - Buffer);
  std::copy_n("B=", 3, Before.data() + First - Buffer);
  for (unsigned I = 0; I != 2; ++I)
    llvm::support::endian::write64le(Before.data() + Vector - Buffer + I * 8,
                                     First);
  llvm::cantFail(CPU.write(Buffer, Before));
  llvm::cantFail(Space.protect(Buffer, Extent, Read | UserAccessible));
  ProcessOptions Options;
  Options.Android.emplace();
  Options.Android->EnvironmentVector = Vector;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer + Extent, Options,
                                    Result);
  const android_model::LinkedImage Linked{};
  // Two name bytes, two pointer/prefix reads, then the terminating pointer.
  for (const auto &[Limit, Expired] : std::array<std::pair<uint64_t, bool>, 4>{
           {{19, false}, {27, false}, {28, false}, {28, true}}}) {
    Options.MemoryLimit = Limit;
    auto Budget = llvm::cantFail(ExecutionBudget::create(
        Options.Limits, Expired ? ExecutionBudget::Clock::time_point::min()
                                : ExecutionBudget::Clock::now()));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = "getenv";
    Call.Arguments[0] = Name;
    auto Value = Model.invoke(Call);
    if (Limit == 28 && !Expired) {
      ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
      ASSERT_TRUE(*Value);
      EXPECT_EQ(std::get<uint64_t>(**Value), 0u);
    } else {
      ASSERT_FALSE(bool(Value));
      EXPECT_NE(llvm::toString(Value.takeError())
                    .find(Expired ? "timed out" : "memory scan budget"),
                std::string::npos);
    }
    EXPECT_EQ(Model.timedOut(), Expired);
    std::vector<uint8_t> After(Extent);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
}

INSTANTIATE_TEST_SUITE_P(
    API28, AndroidEnvironment,
    testing::Combine(
        testing::Values("environment-O0-none", "environment-O0-android",
                        "environment-O0-relr", "environment-O2-none",
                        "environment-O2-android", "environment-O2-relr"),
        testing::Values(ExecutionBackendKind::Unicorn,
                        ExecutionBackendKind::HVF, ExecutionBackendKind::KVM,
                        ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
