//===- AndroidScanningTests.cpp - Android integer scanning ---------------===//
//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <array>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x180000000;
class AndroidScanning : public testing::TestWithParam<const char *> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;

  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(GetParam()) + ".so");
    Options.Backend = ExecutionBackendKind::Unicorn;
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
    Options.Android->TraceLimit = 4096;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 500000;
#endif
  }
  void put(size_t Offset, llvm::StringRef Text) {
    auto &Bytes = Options.Android->Memory[0].Bytes;
    std::copy(Text.bytes_begin(), Text.bytes_end(), Bytes.begin() + Offset);
    Bytes[Offset + Text.size()] = 0;
  }
  ProcessResult run(llvm::StringRef Entry, std::vector<uint64_t> Arguments) {
    Options.Android->EntrySymbol = Entry.str();
    Options.Android->Arguments = std::move(Arguments);
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
  void returned(const ProcessResult &R, uint64_t Value) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
  }
  struct Write {
    size_t Offset;
    unsigned Size;
    uint64_t Value;
  };
  void memory(const ProcessResult &R, std::initializer_list<Write> Writes) {
    auto Expected = Options.Android->Memory[0].Bytes;
    for (const auto &W : Writes)
      for (unsigned I = 0; I < W.Size; ++I)
        Expected[W.Offset + I] = W.Value >> (8 * I);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  ProcessResult scan(llvm::StringRef Input, llvm::StringRef Format,
                     unsigned Mode = 0, uint64_t Destination = Buffer) {
    put(1024, Input);
    put(2048, Format);
    return run("scan_supplied",
               {Destination, Buffer + 1024, Buffer + 2048, Mode});
  }
};

TEST_P(AndroidScanning, DirectSavedRegistersStackAndVACopyPreserveWidths) {
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("scan_integers", {Buffer, Mode});
    returned(R, 8);
    memory(R, {{0, 1, 0x80},
               {16, 2, 65535},
               {32, 4, 0x80000000},
               {48, 4, UINT32_MAX},
               {64, 8, uint64_t(1) << 63},
               {80, 8, UINT64_MAX},
               {96, 8, uint64_t(0) - 4294967298},
               {112, 8, 0x100000002},
               {128, 4, 98}});
    unsigned Calls = 0;
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == (Mode ? "vsscanf" : "sscanf")) {
        ++Calls;
        EXPECT_EQ(Call.Result, 8u);
      }
    EXPECT_EQ(Calls, Mode == 1 ? 2u : 1u);
  }
}

TEST_P(AndroidScanning, MatchingFailureInputFailureAndCountAreDistinct) {
  struct Case {
    const char *Input, *Format;
    uint64_t Result;
    std::initializer_list<Write> Writes;
  };
  const Case Cases[] = {
      {"", "%u", UINT32_MAX, {}},
      {" \t\n", "%u", UINT32_MAX, {}},
      {"", " \n", 0, {}},
      {"", "%n%u", UINT32_MAX, {{0, 4, 0}}},
      {"+", "%u", 0, {}},
      {"x", "%u", 0, {}},
      {"19x", "%u:%u", 1, {{0, 4, 19}}},
      {"19:", "%u:%u", 1, {{0, 4, 19}}},
      {"19", "%*u%u", UINT32_MAX, {}},
      {"19", "%*u", 0, {}},
      {"19", "%n%u%n", 1, {{0, 4, 0}, {16, 4, 19}, {32, 4, 2}}},
      {"19%", "%u%%%n", 1, {{0, 4, 19}, {16, 4, 3}}},
      {"19 %", "%u%%%n", 1, {{0, 4, 19}}},
      {" 19 %", " %u %% %n", 1, {{0, 4, 19}, {16, 4, 5}}}};
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Input);
    SCOPED_TRACE(C.Format);
    auto R = scan(C.Input, C.Format);
    returned(R, C.Result);
    memory(R, C.Writes);
  }
}

TEST_P(AndroidScanning, BasesSignsWidthPrefixRollbackAndSuppression) {
  auto R = scan("+0x2f/077/-3", "%i/%i/%u%n");
  returned(R, 3);
  memory(R, {{0, 4, 47}, {16, 4, 63}, {32, 4, UINT32_MAX - 2}, {48, 4, 12}});
  R = scan("0x/0x8/+0x3", "%i x/%2i x%*u/%3i x%n");
  returned(R, 3);
  memory(R, {{0, 4, 0}, {16, 4, 0}, {32, 4, 0}, {48, 4, 10}});
  R = scan("4294967297 65538 258", "%u %hu %hhu");
  returned(R, 3);
  memory(R, {{0, 4, 1}, {16, 2, 2}, {32, 1, 2}});
  R = scan("23 999999999999999999999999999999999 45", "%u %*u %u");
  returned(R, 2);
  memory(R, {{0, 4, 23}, {16, 4, 45}});
}

TEST_P(AndroidScanning, NumericBufferLimitAndAliasedDestinations) {
  auto R = scan(std::string(512, '0') + "7", "%u%n%u");
  returned(R, 2);
  memory(R, {{0, 4, 0}, {16, 4, 512}, {32, 4, 7}});
  R = run("scan_alias", {Buffer});
  returned(R, 2);
  memory(R, {{0, 4, 34}});
}

TEST_P(AndroidScanning, PointersLegacyLengthsAndNarrowCountsKeepExactWidths) {
  auto R =
      scan("0xfedcba9876543210 -4294967298 100000002 077", "%p %qd %lx %O");
  returned(R, 4);
  memory(R, {{0, 8, 0xfedcba9876543210},
             {16, 8, uint64_t(0) - 4294967298},
             {32, 8, 0x100000002},
             {48, 8, 63}});
  R = scan("-4294967298 37", "%D %ld%hhn%hn");
  returned(R, 2);
  memory(R, {{0, 8, uint64_t(0) - 4294967298},
             {16, 8, 37},
             {32, 1, 14},
             {48, 2, 14}});
}

TEST_P(AndroidScanning, UnsupportedFormatsAndOverflowPublishNoWrites) {
  for (const char *Format :
       {"%u %f", "%u %ls", "%u %[0-9]", "%u %lc", "%u %ms", "%u %2$u", "%u %lp",
        "%u %*n", "%u %1n", "%u %0n", "%u %", "%u %18446744073709551616u"}) {
    SCOPED_TRACE(Format);
    auto R = scan("12 34", Format);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    memory(R, {});
  }
  for (const char *Value : {"18446744073709551616", "-18446744073709551616"}) {
    auto R = scan(std::string("12 ") + Value, "%u %lu");
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    memory(R, {});
  }
  auto R = scan("12 9223372036854775808", "%u %ld");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  memory(R, {});
}

TEST_P(AndroidScanning, BadPointersAndInputAliasesDoNotPublishAssignments) {
  auto R = scan("12 34", "%u %u", 0, Buffer + 8188);
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  EXPECT_TRUE(R.MemorySnapshots.empty());
  R = scan("12 34", "%u %u", 0, Buffer + 1024);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  memory(R, {});
  R = scan("12", "%u", 0, Buffer + 2048);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  memory(R, {});
  R = scan("x", "%u", 0, UINT64_MAX);
  returned(R, 0);
  memory(R, {});
  R = run("scan_supplied", {Buffer, UINT64_MAX, Buffer + 2048, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  EXPECT_TRUE(R.MemorySnapshots.empty());
}

TEST_P(AndroidScanning, NarrowCharactersStringsWidthsAndCounts) {
  for (unsigned Mode : {0u, 1u}) {
    SCOPED_TRACE(Mode);
    auto R = scan(" \tab cd!", "%2c%2s%n%c", Mode);
    returned(R, 3);
    memory(R, {{0, 2, 0x0920}, {16, 3, 0x6261}, {32, 4, 4}, {48, 1, ' '}});
    R = scan("ab", "%5c%n", Mode);
    returned(R, 1);
    memory(R, {{0, 2, 0x6261}, {16, 4, 2}});
    R = scan(" \talpha\n", "%3s%n%c", Mode);
    returned(R, 2);
    memory(R, {{0, 4, 0x706c61}, {16, 4, 5}, {32, 1, 'h'}});
    R = scan(" ", "%c%c", Mode);
    returned(R, 1);
    memory(R, {{0, 1, ' '}});
    R = scan(" \t", "%s", Mode);
    returned(R, UINT32_MAX);
    memory(R, {});
    R = scan("x tail", "%*c %s%n", Mode);
    returned(R, 1);
    memory(R, {{0, 5, 0x6c696174}, {16, 4, 6}});
    R = scan("x", "%*c%c", Mode);
    returned(R, UINT32_MAX);
    memory(R, {});
  }
}

TEST_P(AndroidScanning, TextAssignmentsLargerThanIntegerAndAliasRefusal) {
  auto R = scan(std::string(73, 'x'), "%s");
  returned(R, 1);
  auto Expected = Options.Android->Memory[0].Bytes;
  std::fill_n(Expected.begin(), 73, 'x');
  Expected[73] = 0;
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  for (const char *Format : {"%s", "%2c"}) {
    SCOPED_TRACE(Format);
    for (uint64_t Offset : {1024u, 2048u}) {
      R = scan("ab", Format, 0, Buffer + Offset);
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      memory(R, {});
    }
    R = scan("ab", Format, 0, Buffer + 8191);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_TRUE(R.MemorySnapshots.empty());
  }
}

TEST_P(AndroidScanning,
       ExplicitVAListRetainsItsOwnStorageAndRejectsBadOffsets) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  put(1024, "42 43");
  put(2048, "%u %u");
  llvm::support::endian::write64le(Bytes.data() + 512, Buffer + 704);
  llvm::support::endian::write64le(Bytes.data() + 520, Buffer + 704);
  llvm::support::endian::write64le(Bytes.data() + 528, 1);
  llvm::support::endian::write32le(Bytes.data() + 536, -8);
  llvm::support::endian::write32le(Bytes.data() + 540, -128);
  llvm::support::endian::write64le(Bytes.data() + 696, Buffer);
  llvm::support::endian::write64le(Bytes.data() + 704, Buffer + 16);
  auto R =
      run("scan_explicit_va", {Buffer + 1024, Buffer + 2048, Buffer + 512});
  returned(R, 2);
  memory(R, {{0, 4, 42}, {16, 4, 43}});
  for (int Offset : {-72, -7}) {
    llvm::support::endian::write32le(Bytes.data() + 536, Offset);
    R = run("scan_explicit_va", {Buffer + 1024, Buffer + 2048, Buffer + 512});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_TRUE(R.MemorySnapshots.empty());
  }
  llvm::support::endian::write32le(Bytes.data() + 536, -8);
  llvm::support::endian::write64le(Bytes.data() + 696, Buffer + 704);
  R = run("scan_explicit_va", {Buffer + 1024, Buffer + 2048, Buffer + 512});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  memory(R, {});
}

TEST_P(AndroidScanning, DynamicProviderIdentityAndLifetimeArePreserved) {
  Options.Android->Libraries["libscan-model.so"] = {"sscanf", "vsscanf"};
  for (unsigned Variadic = 0; Variadic < 2; ++Variadic) {
    auto R = run("scan_dynamic", {Buffer, 0, Variadic});
    returned(R, 1);
    memory(R, {{0, 4, 15}});
    std::optional<uint64_t> Address;
    for (const auto &Call : R.NativeCalls) {
      if (Call.Name == "dlsym")
        Address = Call.Result;
      if (Call.Name == (Variadic ? "vsscanf" : "sscanf")) {
        EXPECT_EQ(Call.Library, "libscan-model.so");
        EXPECT_EQ(Call.PC, Address);
        EXPECT_EQ(Call.Result, 1u);
      }
    }
    ASSERT_TRUE(Address);
    R = run("scan_dynamic", {Buffer, 1, Variadic});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
    memory(R, {});
  }
}

INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidScanning,
                         testing::Values("scan-O0-none", "scan-O0-android",
                                         "scan-O0-relr", "scan-O2-none",
                                         "scan-O2-android", "scan-O2-relr"));

TEST(AndroidScanningMemory, FaultsLimitsAndMalformedListsPreserveAllBytes) {
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
  ASSERT_EQ(
      llvm::toString(Space.map(Buffer, 8192, Read | Write | UserAccessible)),
      "");
  enum Case {
    SecondWriteDenied,
    CrossingWrite,
    BadInput,
    BadFormat,
    UnalignedList,
    BadOffset,
    BadSaveArea,
    WrappingStack,
    InputLimit,
    ExpiredDeadline,
    UnterminatedInput,
    StringWriteDenied,
    StringCrossingWrite,
    CharacterWriteDenied,
    CharacterCrossingWrite
  };
  for (unsigned C = SecondWriteDenied; C <= CharacterCrossingWrite; ++C) {
    SCOPED_TRACE(C);
    std::array<uint8_t, 8192> Before;
    Before.fill(0xa5);
    std::copy_n("12 34", 6, Before.begin() + 1024);
    std::copy_n("%u %u", 6, Before.begin() + 2048);
    if (C == StringWriteDenied || C == StringCrossingWrite)
      std::copy_n("%s %s", 6, Before.begin() + 2048);
    if (C == CharacterWriteDenied || C == CharacterCrossingWrite)
      std::copy_n("%2c %2c", 8, Before.begin() + 2048);
    llvm::support::endian::write64le(Before.data() + 512, Buffer + 704);
    llvm::support::endian::write64le(Before.data() + 520, Buffer + 704);
    llvm::support::endian::write32le(Before.data() + 536, -8);
    llvm::support::endian::write64le(Before.data() + 696, Buffer);
    llvm::support::endian::write64le(Before.data() + 704, Buffer + 4096);
    NativeCallEvent Call{};
    Call.Name = "sscanf";
    Call.Arguments = {Buffer + 1024, Buffer + 2048, Buffer, Buffer + 4096};
    if (C == CrossingWrite)
      Call.Arguments[2] = Buffer + 4094;
    if (C == StringCrossingWrite || C == CharacterCrossingWrite)
      Call.Arguments[2] = Buffer + 4095;
    if (C == BadInput)
      Call.Arguments[0] = UINT64_MAX;
    if (C == BadFormat)
      Call.Arguments[1] = UINT64_MAX;
    if (C >= UnalignedList && C <= WrappingStack) {
      Call.Name = "vsscanf";
      Call.Arguments[2] = Buffer + 512;
    }
    if (C == UnalignedList)
      ++Call.Arguments[2];
    if (C == BadOffset)
      llvm::support::endian::write32le(Before.data() + 536, -7);
    if (C == BadSaveArea)
      llvm::support::endian::write64le(Before.data() + 520, 0);
    if (C == WrappingStack) {
      llvm::support::endian::write32le(Before.data() + 536, 0);
      llvm::support::endian::write64le(Before.data() + 512, UINT64_MAX - 7);
    }
    if (C == UnterminatedInput)
      Call.Arguments[0] = Buffer + 8191;
    ASSERT_EQ(llvm::toString(
                  Space.protect(Buffer, 8192, Read | Write | UserAccessible)),
              "");
    ASSERT_EQ(llvm::toString(CPU.write(Buffer, Before)), "");
    ASSERT_EQ(llvm::toString(
                  Space.protect(Buffer + 4096, 4096, Read | UserAccessible)),
              "");
    ProcessOptions Options;
    Options.Android.emplace();
    if (C == InputLimit)
      Options.MemoryLimit = 4;
    auto Budget = llvm::cantFail(ExecutionBudget::create(
        Options.Limits, C == ExpiredDeadline
                            ? ExecutionBudget::Clock::time_point::min()
                            : ExecutionBudget::Clock::now()));
    const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
    ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                         GuestArchitecture::AArch64,
                         ExecutionBackendKind::Unicorn,
                         {}};
    linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 8192, Options,
                                      Result);
    const android_model::LinkedImage Linked{};
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_FALSE(llvm::toString(Value.takeError()).empty());
    EXPECT_EQ(Model.timedOut(), C == ExpiredDeadline);
    std::array<uint8_t, 8192> After{};
    ASSERT_EQ(llvm::toString(Space.snapshotBacking(Buffer, After)), "");
    EXPECT_EQ(After, Before);
  }
}
} // namespace
} // namespace neverd::emulation
