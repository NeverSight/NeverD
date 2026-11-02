//===- AndroidFormattingTests.cpp - Compiled AAPCS64 formatting calls
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
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
constexpr std::array<const char *, 4> FortifyNames = {
    "__snprintf_chk", "__vsnprintf_chk", "__sprintf_chk", "__vsprintf_chk"};
class AndroidFormatting : public testing::TestWithParam<
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
    Options.Android->TraceLimit = 4096;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 1024});
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 500000;
#endif
  }
  void put(uint64_t Offset, llvm::StringRef Text) {
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
  void output(const ProcessResult &R, llvm::StringRef Text, size_t Offset = 0) {
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    const auto &Bytes = R.MemorySnapshots[0].Bytes;
    ASSERT_LT(Offset + Text.size() + 1, Bytes.size());
    EXPECT_EQ(std::vector<uint8_t>(Bytes.begin() + Offset,
                                   Bytes.begin() + Offset + Text.size()),
              std::vector<uint8_t>(Text.bytes_begin(), Text.bytes_end()));
    EXPECT_EQ(Bytes[Offset + Text.size()], 0);
    EXPECT_EQ(Bytes[Offset + Text.size() + 1], 0xa5);
  }
  void unsupported(const ProcessResult &R, unsigned Mode,
                   llvm::StringRef Diagnostic) {
    ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find(Diagnostic.str()), std::string::npos);
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, FortifyNames[Mode]);
    EXPECT_FALSE(R.NativeCalls.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    const auto &Bytes = Options.Android->Memory[0].Bytes;
    EXPECT_EQ(R.MemorySnapshots[0].Bytes,
              std::vector<uint8_t>(Bytes.begin(), Bytes.begin() + 1024));
  }
};

TEST_P(AndroidFormatting, FourEntrypointsPromotionsLongValuesAndVACopy) {
  const std::string Expected =
      "-0000123|0xfedcba9876543210|abc    |-128/255/-32768/65535|"
      "-9223372036854775808/18446744073709551615|4294967297/-4294967298/-5|"
      "\x80/%|0x123456789abcdef0";
  for (unsigned Mode = 0; Mode < 4; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("format_integers", {Buffer, Mode});
    returned(R, Expected.size());
    output(R, Expected);
    if (Mode % 2)
      output(R, Expected, 256);
    unsigned Calls = 0;
    const char *Names[] = {"snprintf", "vsnprintf", "sprintf", "vsprintf"};
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == Names[Mode]) {
        ++Calls;
        EXPECT_EQ(Call.Result, Expected.size());
      }
    EXPECT_EQ(Calls, Mode % 2 ? 2u : 1u);
  }
}
TEST_P(AndroidFormatting, ExhaustedGPRegisterAreaStartsOnStack) {
  const char Expected[] = "-17:4294967298:stack:002a    ";
  auto R = run("format_stacked", {Buffer});
  returned(R, sizeof(Expected) - 1);
  output(R, Expected);
}
TEST_P(AndroidFormatting, FlagsPrecisionNullPointersAndAndroidOctalBehavior) {
  const char Expected[] =
      "[00023   ][     002a][011][][][+][0x][0x0][(nu][whole]";
  auto R = run("format_flags", {Buffer});
  returned(R, sizeof(Expected) - 1);
  output(R, Expected);
}
TEST_P(AndroidFormatting, BinaryCharactersAndTruncationReportRequiredLength) {
  const std::vector<uint8_t> Full = {'A', 0, 'B', 0x80, 0xff, 'Z'};
  for (uint64_t Capacity : {0, 1, 2, 4, 6, 7, 64}) {
    auto R = run("format_binary", {Capacity ? Buffer : 0, Capacity});
    returned(R, Full.size());
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    std::vector<uint8_t> Expected(1024, 0xa5);
    if (Capacity) {
      size_t Count = std::min(Capacity - 1, uint64_t(Full.size()));
      std::copy_n(Full.begin(), Count, Expected.begin());
      Expected[Count] = 0;
    }
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}
TEST_P(AndroidFormatting, HugePaddingCountsWithoutHugeAllocation) {
  put(512, "%2147483647d");
  auto R = run("format_supplied", {Buffer, 4, Buffer + 512, 1, 0, 0});
  returned(R, 2147483647);
  output(R, "   ");
}
TEST_P(AndroidFormatting, PrecisionNeverReadsBeyondTheLastMappedByte) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  std::copy_n("tail", 4, Bytes.begin() + 8188);
  auto R = run("format_precision_page", {Buffer, Buffer + 8188});
  returned(R, 4);
  output(R, "tail");
  put(512, "%.0s");
  R = run("format_one_string", {Buffer, 16, Buffer + 512, UINT64_MAX});
  returned(R, 0);
  output(R, "");
}
TEST_P(AndroidFormatting, CapacityDoesNotRequireUnusedWritableMemory) {
  Options.Android->ReadMemory = {{Buffer + 4094, 16}};
  auto R = run("format_readonly_tail", {Buffer + 4094});
  returned(R, 1);
  output(R, "x");
}
TEST_P(AndroidFormatting, UnsupportedConversionsAndOverflowPublishNoResult) {
  for (unsigned Mode = 0; Mode < 9; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("format_unsafe", {Buffer, Mode});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "snprintf");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(1024, 0xa5));
  }
}
TEST_P(AndroidFormatting, BadFormatStringAndDestinationFailExplicitly) {
  put(512, "%s");
  for (auto Args : {std::vector<uint64_t>{Buffer, 16, UINT64_MAX, 0},
                    {Buffer, 16, Buffer + 512, UINT64_MAX},
                    {Buffer + 8191, 16, Buffer + 512, Buffer + 512}}) {
    auto R = run("format_one_string", Args);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
  }
}
TEST_P(AndroidFormatting, DiscardedOutputStillRequiresTerminatedInput) {
  // An accessible but unterminated input must still fail for snprintf(NULL,0).
  put(512, "%s");
  auto R = run("format_one_string", {0, 0, Buffer + 512, Buffer + 1024});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
}
TEST_P(AndroidFormatting, ExplicitVAListUsesItsOwnSavedRegistersAndStack) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  // One remaining GP slot, then the separately named overflow stack.
  llvm::support::endian::write64le(Bytes.data() + 512, Buffer + 704);
  llvm::support::endian::write64le(Bytes.data() + 520, Buffer + 704);
  llvm::support::endian::write64le(Bytes.data() + 528, 1); // Unused FP area.
  llvm::support::endian::write32le(Bytes.data() + 536, -8);
  llvm::support::endian::write32le(Bytes.data() + 540, -128);
  llvm::support::endian::write64le(Bytes.data() + 696, 0xfedcba98fffffff9);
  llvm::support::endian::write64le(Bytes.data() + 704, 0x10000000a);
  put(576, "%d/%lx");
  auto R = run("format_explicit_va", {Buffer, Buffer + 512, Buffer + 576});
  returned(R, 12);
  output(R, "-7/10000000a");
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(std::vector<uint8_t>(R.MemorySnapshots[0].Bytes.begin() + 512,
                                 R.MemorySnapshots[0].Bytes.end()),
            std::vector<uint8_t>(Bytes.begin() + 512, Bytes.begin() + 1024));
  for (int Offset : {-72, -7}) {
    llvm::support::endian::write32le(Bytes.data() + 536, Offset);
    R = run("format_explicit_va", {Buffer, Buffer + 512, Buffer + 576});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  }
  llvm::support::endian::write32le(Bytes.data() + 536, 0);
  llvm::support::endian::write64le(Bytes.data() + 512, UINT64_MAX - 7);
  R = run("format_explicit_va", {Buffer, Buffer + 512, Buffer + 576});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
}
TEST_P(AndroidFormatting, DynamicResolutionRetainsProviderAndClosedCallFails) {
  Options.Android->Libraries["libformat-model.so"] = {"snprintf"};
  auto R = run("format_dynamic", {Buffer, 0});
  returned(R, 18);
  output(R, "symbol=0x10000000a");
  std::optional<uint64_t> Address;
  for (const auto &Call : R.NativeCalls) {
    if (Call.Name == "dlsym")
      Address = Call.Result;
    if (Call.Name == "snprintf") {
      EXPECT_EQ(Call.Library, "libformat-model.so");
      EXPECT_EQ(Call.PC, Address);
      EXPECT_EQ(Call.Result, 18u);
    }
  }
  ASSERT_TRUE(Address);
  R = run("format_dynamic", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_FALSE(R.NativeCalls.back().Result);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(1024, 0xa5));
}

TEST_P(AndroidFormatting, FortifyEntrypointsPromotionsStackArgumentsAndVACopy) {
  const std::string Expected =
      "-0000123|0xfedcba9876543210|abc    |-128/255/-32768/65535|"
      "-9223372036854775808/18446744073709551615|4294967297/-4294967298/-5|"
      "\x80/%|0x123456789abcdef0";
  for (unsigned Mode = 0; Mode < FortifyNames.size(); ++Mode) {
    SCOPED_TRACE(FortifyNames[Mode]);
    auto R = run("format_integers", {Buffer, Mode + 4});
    ASSERT_NO_FATAL_FAILURE(returned(R, Expected.size()));
    output(R, Expected);
    if (Mode % 2)
      output(R, Expected, 256);
    unsigned Calls = 0;
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == FortifyNames[Mode]) {
        ++Calls;
        EXPECT_EQ(Call.Result, Expected.size());
      }
    EXPECT_EQ(Calls, Mode % 2 ? 2u : 1u);
  }
}

TEST_P(AndroidFormatting, FortifySnprintfTruncatesAndCountsBinaryOutput) {
  put(512, "A%cB%sZ");
  put(600, "\x80\xff");
  const std::vector<uint8_t> Full = {'A', 0, 'B', 0x80, 0xff, 'Z'};
  for (unsigned Mode : {0u, 1u})
    for (uint64_t Capacity : {0, 1, 2, 4, 6, 7, 64}) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Capacity);
      auto R = run("format_fortify_supplied",
                   {Capacity ? Buffer : 0, Mode, Capacity, Capacity,
                    Buffer + 512, 0, Buffer + 600, 0});
      ASSERT_NO_FATAL_FAILURE(returned(R, Full.size()));
      const auto &Bytes = Options.Android->Memory[0].Bytes;
      std::vector<uint8_t> Expected(Bytes.begin(), Bytes.begin() + 1024);
      if (Capacity) {
        size_t Count = std::min(Capacity - 1, uint64_t(Full.size()));
        std::copy_n(Full.begin(), Count, Expected.begin());
        Expected[Count] = 0;
      }
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
}

TEST_P(AndroidFormatting, FortifySprintfRequiresSpaceForTheFinalNul) {
  put(512, "A%cB%sZ");
  put(600, "\x80\xff");
  const std::vector<uint8_t> Full = {'A', 0, 'B', 0x80, 0xff, 'Z', 0};
  for (unsigned Mode : {2u, 3u}) {
    SCOPED_TRACE(Mode);
    for (uint64_t Capacity : {0, 1, 5, 6, 7, 64}) {
      SCOPED_TRACE(Capacity);
      auto R =
          run("format_fortify_supplied",
              {Buffer, Mode, 0, Capacity, Buffer + 512, 0, Buffer + 600, 0});
      if (Capacity < Full.size()) {
        unsupported(R, Mode, "fortify buffer-size failure");
      } else {
        ASSERT_NO_FATAL_FAILURE(returned(R, Full.size() - 1));
        const auto &Bytes = Options.Android->Memory[0].Bytes;
        std::vector<uint8_t> Expected(Bytes.begin(), Bytes.begin() + 1024);
        std::copy(Full.begin(), Full.end(), Expected.begin());
        ASSERT_EQ(R.MemorySnapshots.size(), 1u);
        EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      }
    }
    put(640, "");
    auto R = run("format_fortify_supplied",
                 {Buffer, Mode, 0, 0, Buffer + 640, 0, 0, 0});
    unsupported(R, Mode, "fortify buffer-size failure");
    R = run("format_fortify_supplied",
            {Buffer, Mode, 0, 1, Buffer + 640, 0, 0, 0});
    ASSERT_NO_FATAL_FAILURE(returned(R, 0));
    output(R, "");
  }
}

TEST_P(AndroidFormatting, FortifySnprintfChecksObjectSizeBeforeAnyInputRead) {
  for (unsigned Mode : {0u, 1u}) {
    SCOPED_TRACE(Mode);
    for (auto Bounds : {std::array<uint64_t, 2>{64, 63},
                        {0x100000008ULL, 8},
                        {UINT64_MAX, 0}}) {
      auto R = run("format_fortify_supplied", {UINT64_MAX, Mode, Bounds[0],
                                               Bounds[1], UINT64_MAX, 0, 0, 0});
      unsupported(R, Mode, "fortify buffer-size failure");
    }
  }
}

TEST_P(AndroidFormatting, FortifyKeepsFullWidthBoundsAndExistingCapacityLimit) {
  put(512, "ok");
  for (unsigned Mode = 0; Mode < FortifyNames.size(); ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("format_fortify_supplied",
                 {Buffer, Mode, 0x7fffffff, 0x7fffffff, Buffer + 512, 0, 0, 0});
    ASSERT_NO_FATAL_FAILURE(returned(R, 2));
    output(R, "ok");
    for (uint64_t Bound :
         std::array<uint64_t, 3>{0x80000000, 0x100000000, UINT64_MAX}) {
      R = run("format_fortify_supplied",
              {Buffer, Mode, Bound, Bound, UINT64_MAX, 0, 0, 0});
      unsupported(R, Mode, "capacity exceeds the supported INT_MAX bound");
    }
    if (Mode < 2) {
      for (uint64_t ObjectSize :
           std::array<uint64_t, 2>{0x100000000, UINT64_MAX}) {
        R = run("format_fortify_supplied",
                {Buffer, Mode, 64, ObjectSize, Buffer + 512, 0, 0, 0});
        ASSERT_NO_FATAL_FAILURE(returned(R, 2));
        output(R, "ok");
      }
    }
  }
}

TEST_P(AndroidFormatting, FortifyReusesConversionsAndRejectsWithoutWrites) {
  for (unsigned Mode = 0; Mode < FortifyNames.size(); ++Mode) {
    SCOPED_TRACE(Mode);
    put(512, "%2147483647d");
    auto R = run("format_fortify_supplied",
                 {Buffer, Mode, 4, 4, Buffer + 512, 1, 0, 0});
    if (Mode < 2) {
      ASSERT_NO_FATAL_FAILURE(returned(R, 2147483647));
      output(R, "   ");
    } else {
      unsupported(R, Mode, "fortify buffer-size failure");
    }
    for (const char *Format : {"prefix:%f", "prefix:%ls", "prefix:%n",
                               "prefix:%2$d", "prefix:%2147483648d"}) {
      put(512, Format);
      R = run("format_fortify_supplied",
              {Buffer, Mode, 64, 64, Buffer + 512, Buffer, 0, 0});
      unsupported(R, Mode,
                  llvm::StringRef(Format).contains("%ls")
                      ? "wide or length-qualified text conversion"
                  : llvm::StringRef(Format).contains("2147483648")
                      ? "format width or precision exceeds INT_MAX"
                      : "unsupported format conversion");
    }
  }
}

TEST_P(AndroidFormatting, FortifyVAListUsesSavedRegistersThenStack) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  llvm::support::endian::write64le(Bytes.data() + 512, Buffer + 704);
  llvm::support::endian::write64le(Bytes.data() + 520, Buffer + 704);
  llvm::support::endian::write64le(Bytes.data() + 528, 1);
  llvm::support::endian::write32le(Bytes.data() + 540, -128);
  llvm::support::endian::write64le(Bytes.data() + 696, 0xfedcba98fffffff9);
  llvm::support::endian::write64le(Bytes.data() + 704, 0x10000000a);
  put(576, "%d/%lx");
  for (unsigned Mode : {1u, 3u}) {
    SCOPED_TRACE(Mode);
    llvm::support::endian::write32le(Bytes.data() + 536, -8);
    auto R = run("format_fortify_explicit_va",
                 {Buffer, Mode, 64, 64, Buffer + 576, Buffer + 512});
    ASSERT_NO_FATAL_FAILURE(returned(R, 12));
    output(R, "-7/10000000a");
    EXPECT_EQ(std::vector<uint8_t>(R.MemorySnapshots[0].Bytes.begin() + 512,
                                   R.MemorySnapshots[0].Bytes.end()),
              std::vector<uint8_t>(Bytes.begin() + 512, Bytes.begin() + 1024));
    for (int Offset : {-72, -7}) {
      llvm::support::endian::write32le(Bytes.data() + 536, Offset);
      R = run("format_fortify_explicit_va",
              {Buffer, Mode, 64, 64, Buffer + 576, Buffer + 512});
      EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
      EXPECT_FALSE(R.ReturnValue);
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_EQ(R.NativeCalls.back().Name, FortifyNames[Mode]);
      EXPECT_FALSE(R.NativeCalls.back().Result);
    }
  }
  auto R = run("format_fortify_explicit_va",
               {UINT64_MAX, 1, 64, 63, UINT64_MAX, Buffer + 512});
  unsupported(R, 1, "fortify buffer-size failure");
}

TEST_P(AndroidFormatting, FortifyValidatesOnlyTheWrittenDestinationPrefix) {
  put(512, "x");
  Options.Android->ReadMemory = {{Buffer + 8190, 2}};
  for (unsigned Mode = 0; Mode < FortifyNames.size(); ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("format_fortify_supplied",
                 {Buffer + 8190, Mode, 64, 64, Buffer + 512, 0, 0, 0});
    ASSERT_NO_FATAL_FAILURE(returned(R, 1));
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, (std::vector<uint8_t>{'x', 0}));
    for (auto Args : {std::vector<uint64_t>{Buffer + 8191, Mode, 64, 64,
                                            Buffer + 512, 0, 0, 0},
                      {Buffer, Mode, 64, 64, UINT64_MAX, 0, 0, 0}}) {
      R = run("format_fortify_supplied", Args);
      EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
      EXPECT_FALSE(R.ReturnValue);
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_EQ(R.NativeCalls.back().Name, FortifyNames[Mode]);
      EXPECT_FALSE(R.NativeCalls.back().Result);
    }
  }
}

TEST_P(AndroidFormatting,
       FortifyDynamicResolutionRetainsNamesAndProviderLifetime) {
  for (unsigned Mode = 0; Mode < FortifyNames.size(); ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Libraries["libformat-model.so"] = {FortifyNames[Mode]};
    auto R = run("format_fortify_dynamic", {Buffer, Mode, 0, 64});
    ASSERT_NO_FATAL_FAILURE(returned(R, 18));
    output(R, "symbol=0x10000000a");
    std::optional<uint64_t> Address;
    unsigned Calls = 0;
    for (const auto &Call : R.NativeCalls) {
      if (Call.Name == "dlsym")
        Address = Call.Result;
      if (Call.Name == FortifyNames[Mode]) {
        ++Calls;
        EXPECT_EQ(Call.Library, "libformat-model.so");
        EXPECT_EQ(Call.PC, Address);
        EXPECT_EQ(Call.Result, 18u);
      }
    }
    EXPECT_TRUE(Address);
    EXPECT_EQ(Calls, 1u);
    R = run("format_fortify_dynamic", {Buffer, Mode, 1, 64});
    unsupported(R, Mode, "inactive dynamic library");
    R = run("format_fortify_dynamic", {Buffer, Mode, 0, 17});
    unsupported(R, Mode, "fortify buffer-size failure");
  }
}

INSTANTIATE_TEST_SUITE_P(
    OptimizationAndPacking, AndroidFormatting,
    testing::Combine(testing::Values("format-O0-none", "format-O0-android",
                                     "format-O0-relr", "format-O2-none",
                                     "format-O2-android", "format-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF)));

TEST(AndroidFormattingMemory, FortifyBudgetsAndFaultsPreserveDestinationBytes) {
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
  llvm::cantFail(Space.map(Buffer, 4096, Read | Write | UserAccessible));
  std::vector<uint8_t> Before(4096, 0xa5);
  std::copy_n("%64s", 5, Before.begin() + 128);
  std::copy_n("x", 2, Before.begin() + 256);
  llvm::support::endian::write64le(Before.data() + 512, Buffer + 600);
  llvm::support::endian::write32le(Before.data() + 536, 0);
  llvm::support::endian::write64le(Before.data() + 600, Buffer + 256);
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  const android_model::LinkedImage Linked{};
  struct Case {
    uint64_t MemoryLimit, Destination;
    bool Expired;
    const char *Error;
  };
  const Case Cases[] = {
      {2, Buffer, false, "formatting input exceeds memory limit"},
      {64, Buffer, false, "formatting output exceeds memory limit"},
      {65, Buffer, false, nullptr},
      {128, Buffer + 4095, false, "invalid guest pointer"},
      {128, Buffer, true, "native call model timed out"}};
  for (unsigned Mode = 0; Mode < FortifyNames.size(); ++Mode) {
    SCOPED_TRACE(FortifyNames[Mode]);
    for (const auto &C : Cases) {
      SCOPED_TRACE(C.MemoryLimit);
      SCOPED_TRACE(C.Destination);
      SCOPED_TRACE(C.Expired);
      llvm::cantFail(CPU.write(Buffer, Before));
      ProcessOptions Options;
      Options.Android.emplace();
      Options.MemoryLimit = C.MemoryLimit;
      ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                           GuestArchitecture::AArch64,
                           ExecutionBackendKind::Unicorn,
                           {}};
      linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 4096, Options,
                                        Result);
      auto Budget = llvm::cantFail(ExecutionBudget::create(
          Options.Limits, C.Expired ? ExecutionBudget::Clock::time_point::min()
                                    : ExecutionBudget::Clock::now()));
      android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                  Linked);
      NativeCallEvent Call{};
      Call.Name = FortifyNames[Mode];
      const uint64_t Argument = Buffer + (Mode % 2 ? 512 : 256);
      if (Mode < 2)
        Call.Arguments = {C.Destination, 128, 7, 128, Buffer + 128, Argument};
      else
        Call.Arguments = {C.Destination, 7, 128, Buffer + 128, Argument};
      auto Value = Model.invoke(Call);
      auto Expected = Before;
      if (C.Error) {
        ASSERT_FALSE(bool(Value));
        EXPECT_NE(llvm::toString(Value.takeError()).find(C.Error),
                  std::string::npos);
      } else {
        ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
        ASSERT_TRUE(*Value);
        EXPECT_EQ(std::get<uint64_t>(**Value), 64u);
        std::fill_n(Expected.begin(), 63, ' ');
        Expected[63] = 'x';
        Expected[64] = 0;
      }
      EXPECT_EQ(Model.timedOut(), C.Expired);
      std::vector<uint8_t> After(Before.size());
      llvm::cantFail(Space.snapshotBacking(Buffer, After));
      EXPECT_EQ(After, Expected);
    }
  }
}
} // namespace
} // namespace neverd::emulation
