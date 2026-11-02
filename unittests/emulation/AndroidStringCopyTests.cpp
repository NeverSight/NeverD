//===- AndroidStringCopyTests.cpp - Bounded copy boundaries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks exact guest bytes, bounded reads and terminal refusal semantics.
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
constexpr uint64_t Source = Buffer + 4096;
constexpr uint64_t End = Source + 4096;
constexpr std::array<const char *, 2> CheckedNames = {"__strncpy_chk",
                                                      "__strncpy_chk2"};

class AndroidStringCopy : public testing::TestWithParam<
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

  void returned(const ProcessResult &R, uint64_t Value) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
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

TEST_P(AndroidStringCopy, CheckedAppendBytesReturnAndErrno) {
  Options.Android->Libraries["libappend-checked.so"] = {"__strcat_chk",
                                                        "__strncat_chk"};
  for (uint64_t Mode : {0u, 1u}) {
    const char *Name = Mode ? "__strncat_chk" : "__strcat_chk";
    for (uint64_t Count : {uint64_t(0), uint64_t(1), uint64_t(3), uint64_t(4),
                           uint64_t(0x100000001), UINT64_MAX}) {
      if (!Mode && Count != UINT64_MAX)
        continue;
      const uint64_t Copied = Mode ? std::min<uint64_t>(Count, 3) : 3;
      for (uint64_t Size : {Copied + 2, uint64_t(0x100000001), UINT64_MAX}) {
        for (uint64_t Route : {0u, 1u}) {
          SCOPED_TRACE(testing::Message()
                       << Name << ":" << Count << ":" << Size << ":" << Route);
          auto &Bytes = Options.Android->Memory[0].Bytes;
          Bytes.assign(8192, 0xa5);
          Bytes[16] = 'p';
          Bytes[17] = 0;
          const std::array<uint8_t, 4> Text = {0x80, 0xff, 'x', 0};
          std::copy(Text.begin(), Text.end(), Bytes.begin() + 4096);
          auto Expected = Bytes;
          std::copy_n(Text.begin(), Copied, Expected.begin() + 17);
          Expected[17 + Copied] = 0;
          llvm::support::endian::write64le(Expected.data() + 2048, 733);
          auto R = run("append_checked", {Buffer + 16, Source, Count, Size,
                                          Mode, Route, Buffer + 2048});
          returned(R, Buffer + 16);
          ASSERT_EQ(R.MemorySnapshots.size(), 1u);
          EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
          unsigned Calls = 0;
          uint64_t Target = 0;
          for (const auto &Call : R.NativeCalls) {
            if (Call.Name == "dlsym") {
              EXPECT_EQ(Call.Symbol, Name);
              ASSERT_TRUE(Call.Result);
              Target = *Call.Result;
            }
            if (Call.Name != Name)
              continue;
            EXPECT_EQ(Call.Arguments[Mode ? 3 : 2], Size);
            if (Mode)
              EXPECT_EQ(Call.Arguments[2], Count);
            EXPECT_EQ(Call.Result, Buffer + 16);
            EXPECT_EQ(Call.Library, Route ? "libappend-checked.so" : "");
            if (Route)
              EXPECT_EQ(Call.PC, Target);
            ++Calls;
          }
          EXPECT_EQ(Calls, 1u);
        }
      }
    }
  }
}

TEST_P(AndroidStringCopy, CheckedAppendZeroCountAndClosedProvider) {
  Options.Android->Libraries["libappend-checked.so"] = {"__strcat_chk",
                                                        "__strncat_chk"};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  auto Expected = Bytes;
  llvm::support::endian::write64le(Expected.data() + 2048, 733);
  for (uint64_t Route : {0u, 1u}) {
    for (uint64_t Destination : {uint64_t(0), End, UINT64_MAX}) {
      auto R = run("append_checked",
                   {Destination, UINT64_MAX, 0, 0, 1, Route, Buffer + 2048});
      returned(R, Destination);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
  for (uint64_t Mode : {0u, 1u}) {
    auto R = run("append_checked",
                 {UINT64_MAX, UINT64_MAX, 0, 0, Mode, 2, Buffer + 2048});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name,
              Mode ? "__strncat_chk" : "__strcat_chk");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_FALSE(R.ReturnValue);
  }
}

TEST_P(AndroidStringCopy, CheckedAppendDestinationScanPrecedesSource) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[16] = 'a';
  Bytes[17] = 'b';
  Bytes[18] = 'c';
  Bytes[19] = 0;
  for (uint64_t Mode : {0u, 1u}) {
    const char *Name = Mode ? "__strncat_chk" : "__strcat_chk";
    for (uint64_t Size : {0u, 1u, 3u})
      rejected(run("append_checked",
                   {Buffer + 16, UINT64_MAX, 1, Size, Mode, 0, Buffer + 2048}),
               Name, "FORTIFY read exceeds object bounds in __strlen_chk");
    for (uint64_t Destination : {uint64_t(0), End - 1, UINT64_MAX})
      rejected(run("append_checked",
                   {Destination, UINT64_MAX, 1, 0, Mode, 0, Buffer + 2048}),
               Name, "invalid guest pointer");
    rejected(run("append_checked", {Buffer + 16, UINT64_MAX, 1, UINT64_MAX,
                                    Mode, 0, Buffer + 2048}),
             Name, "invalid guest pointer");
  }
}

TEST_P(AndroidStringCopy, CheckedAppendSpaceCheckPrecedesNextSourceByte) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[16] = 'p';
  Bytes[17] = 0;
  Bytes[8191] = 'Z';
  for (uint64_t Mode : {0u, 1u}) {
    const char *Name = Mode ? "__strncat_chk" : "__strcat_chk";
    // There is no readable byte after Z. No terminator slot remains, so the
    // fortify failure must precede either that read or the strncat count end.
    rejected(run("append_checked",
                 {Buffer + 16, End - 1, 1, 2, Mode, 0, Buffer + 2048}),
             Name, "FORTIFY destination extent exceeded");
    rejected(run("append_checked_readonly",
                 {Buffer, Buffer + 16, End - 1, 1, 2, Mode}),
             Name, "invalid guest pointer");
  }
  auto Expected = Bytes;
  Expected[17] = 'Z';
  Expected[18] = 0;
  auto R =
      run("append_checked_readonly", {Source, Buffer + 16, End - 1, 1, 3, 1});
  returned(R, Buffer + 16);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  rejected(
      run("append_checked", {Buffer + 16, End - 1, 1, 3, 0, 0, Buffer + 2048}),
      "__strcat_chk", "invalid guest pointer");
}

TEST_P(AndroidStringCopy, CheckedAppendOnlyTailNeedsWritePermission) {
  for (uint64_t Mode : {0u, 1u}) {
    const char *Name = Mode ? "__strncat_chk" : "__strcat_chk";
    auto &Bytes = Options.Android->Memory[0].Bytes;
    Bytes.assign(8192, 0xa5);
    Bytes[4094] = 'a';
    Bytes[4095] = 'b';
    Bytes[4096] = 0;
    Bytes[4224] = 'c';
    Bytes[4225] = 0;
    auto Expected = Bytes;
    Expected[4096] = 'c';
    Expected[4097] = 0;
    auto R = run("append_checked_readonly",
                 {Buffer, Source - 2, Source + 128, 1, 4, Mode});
    returned(R, Source - 2);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    Bytes[16] = 0;
    // Appending an empty string still writes the existing terminator.
    rejected(run("append_checked_readonly",
                 {Source, Source - 2, Buffer + 16, 1, UINT64_MAX, Mode}),
             Name, "invalid guest pointer");
    Bytes[8190] = 'p';
    Bytes[8191] = 0;
    Expected = Bytes;
    llvm::support::endian::write64le(Expected.data() + 2048, 733);
    R = run("append_checked",
            {End - 2, Buffer + 16, 1, UINT64_MAX, Mode, 0, Buffer + 2048});
    returned(R, End - 2);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    rejected(run("append_checked", {End - 2, Source + 128, 1, UINT64_MAX, Mode,
                                    0, Buffer + 2048}),
             Name, "invalid guest pointer");
  }
}

TEST_P(AndroidStringCopy, CheckedAppendRejectsOverlapIncludingExistingPrefix) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[128] = 'a';
  Bytes[129] = 'b';
  Bytes[130] = 0;
  for (uint64_t Mode : {0u, 1u}) {
    const char *Name = Mode ? "__strncat_chk" : "__strcat_chk";
    for (uint64_t Input : {Buffer + 128, Buffer + 129, Buffer + 130})
      rejected(run("append_checked", {Buffer + 128, Input, 1, UINT64_MAX, Mode,
                                      0, Buffer + 2048}),
               Name, "overlapping string concatenation");
  }
}

TEST_P(AndroidStringCopy, TerminatedCopyWholeStringNoPaddingAndErrno) {
  Options.Android->Libraries["libcopy-terminated.so"] = {"strcpy",
                                                         "__strcpy_chk"};
  for (uint64_t Mode : {0u, 1u}) {
    const char *Name = Mode ? "__strcpy_chk" : "strcpy";
    for (uint64_t Route : {0u, 1u}) {
      for (uint64_t Size :
           {uint64_t(1), uint64_t(4), uint64_t(0x100000001), UINT64_MAX}) {
        SCOPED_TRACE(testing::Message() << Name << ":" << Route << ":" << Size);
        auto &Bytes = Options.Android->Memory[0].Bytes;
        Bytes.assign(8192, 0xa5);
        const std::vector<uint8_t> Text =
            Size == 1 ? std::vector<uint8_t>{0}
                      : std::vector<uint8_t>{0x80, 0xff, 'x', 0};
        std::copy(Text.begin(), Text.end(), Bytes.begin() + 4096);
        auto Expected = Bytes;
        std::copy(Text.begin(), Text.end(), Expected.begin() + 16);
        llvm::support::endian::write64le(Expected.data() + 2048, 733);
        auto R = run("copy_terminated",
                     {Buffer + 16, Source, Size, Mode, Route, Buffer + 2048});
        returned(R, Buffer + 16);
        ASSERT_EQ(R.MemorySnapshots.size(), 1u);
        EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
        unsigned Calls = 0;
        uint64_t Target = 0;
        for (const auto &Call : R.NativeCalls) {
          if (Call.Name == "dlsym") {
            EXPECT_EQ(Call.Symbol, Name);
            ASSERT_TRUE(Call.Result);
            Target = *Call.Result;
          }
          if (Call.Name != Name)
            continue;
          if (Mode)
            EXPECT_EQ(Call.Arguments[2], Size);
          EXPECT_EQ(Call.Result, Buffer + 16);
          EXPECT_EQ(Call.Library, Route ? "libcopy-terminated.so" : "");
          if (Route)
            EXPECT_EQ(Call.PC, Target);
          ++Calls;
        }
        EXPECT_EQ(Calls, 1u);
      }
    }
  }
}

TEST_P(AndroidStringCopy, TerminatedCopyPageEndsAndPermissions) {
  for (uint64_t Mode : {0u, 1u}) {
    SCOPED_TRACE(Mode);
    const char *Name = Mode ? "__strcpy_chk" : "strcpy";
    auto &Bytes = Options.Android->Memory[0].Bytes;
    Bytes.assign(8192, 0xa5);
    Bytes[8189] = 0x80;
    Bytes[8190] = 'v';
    Bytes[8191] = 0;
    auto Expected = Bytes;
    std::copy_n(Bytes.begin() + 8189, 3, Expected.begin() + 16);
    auto R = run("copy_terminated_readonly",
                 {Source, Buffer + 16, End - 3, 3, Mode});
    returned(R, Buffer + 16);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);

    Bytes[128] = 'x';
    Bytes[129] = 0;
    Expected = Bytes;
    Expected[8190] = 'x';
    Expected[8191] = 0;
    llvm::support::endian::write64le(Expected.data() + 2048, 733);
    R = run("copy_terminated",
            {End - 2, Buffer + 128, UINT64_MAX, Mode, 0, Buffer + 2048});
    returned(R, End - 2);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    // The object extent authorizes only the actual copy, not extra padding.
    rejected(run("copy_terminated_readonly",
                 {Source, End - 2, Buffer + 128, UINT64_MAX, Mode}),
             Name, "invalid guest pointer");
    for (uint64_t Destination : {uint64_t(0), End - 1, UINT64_MAX})
      rejected(run("copy_terminated", {Destination, Buffer + 128, UINT64_MAX,
                                       Mode, 0, Buffer + 2048}),
               Name, "invalid guest pointer");
    Bytes[8191] = 'w';
    rejected(run("copy_terminated_readonly",
                 {Source, Buffer, End - 2, UINT64_MAX, Mode}),
             Name, "invalid guest pointer");
  }
}

TEST_P(AndroidStringCopy, CheckedTerminatedCopyScansBeforeObjectSize) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[4096] = 'a';
  Bytes[4097] = 'b';
  Bytes[4098] = 'c';
  Bytes[4099] = 0;
  Options.Android->Libraries["libcopy-terminated.so"] = {"__strcpy_chk"};
  for (uint64_t Route : {0u, 1u}) {
    for (uint64_t Size : {0u, 1u, 3u})
      rejected(run("copy_terminated",
                   {UINT64_MAX, Source, Size, 1, Route, Buffer + 2048}),
               "__strcpy_chk", "FORTIFY destination extent exceeded");
    for (uint64_t Input : {uint64_t(0), End - 1, UINT64_MAX})
      rejected(
          run("copy_terminated", {Buffer, Input, 0, 1, Route, Buffer + 2048}),
          "__strcpy_chk", "invalid guest pointer");
  }
  Bytes[4096] = 0;
  rejected(run("copy_terminated", {Buffer, Source, 0, 1, 0, Buffer + 2048}),
           "__strcpy_chk", "FORTIFY destination extent exceeded");
}

TEST_P(AndroidStringCopy, TerminatedCopyOverlapAndClosedProvider) {
  Options.Android->Libraries["libcopy-terminated.so"] = {"strcpy",
                                                         "__strcpy_chk"};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[128] = 'a';
  Bytes[129] = 'b';
  Bytes[130] = 'c';
  Bytes[131] = 0;
  for (uint64_t Mode : {0u, 1u}) {
    const char *Name = Mode ? "__strcpy_chk" : "strcpy";
    for (uint64_t Destination :
         {Buffer + 127, Buffer + 128, Buffer + 130, Buffer + 131})
      rejected(run("copy_terminated",
                   {Destination, Buffer + 128, 4, Mode, 0, Buffer + 2048}),
               Name, "overlapping strcpy");
    for (uint64_t Destination : {Buffer + 124, Buffer + 132}) {
      auto Expected = Bytes;
      std::copy_n(Bytes.begin() + 128, 4,
                  Expected.begin() + (Destination - Buffer));
      llvm::support::endian::write64le(Expected.data() + 2048, 733);
      auto R = run("copy_terminated",
                   {Destination, Buffer + 128, 4, Mode, 0, Buffer + 2048});
      returned(R, Destination);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
    auto R = run("copy_terminated",
                 {UINT64_MAX, UINT64_MAX, 0, Mode, 2, Buffer + 2048});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Name);
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_FALSE(R.ReturnValue);
  }
}

TEST_P(AndroidStringCopy, SizedCopyLengthTerminationNoPaddingAndErrno) {
  Options.Android->Libraries["libcopy.so"] = {"strlcpy"};
  for (uint64_t Count :
       {uint64_t(0), uint64_t(1), uint64_t(3), uint64_t(4), uint64_t(7),
        uint64_t(1024), uint64_t(0x100000001), UINT64_MAX}) {
    for (uint64_t Route : {0u, 1u}) {
      SCOPED_TRACE(Count);
      SCOPED_TRACE(Route);
      auto &Bytes = Options.Android->Memory[0].Bytes;
      Bytes.assign(8192, 0xa5);
      const std::array<uint8_t, 4> Text = {0x80, 0xff, 'x', 0};
      std::copy(Text.begin(), Text.end(), Bytes.begin() + 4096);
      auto Expected = Bytes;
      if (Count) {
        const size_t Copied = std::min<uint64_t>(3, Count - 1);
        std::copy_n(Text.begin(), Copied, Expected.begin() + 16);
        Expected[16 + Copied] = 0;
      }
      llvm::support::endian::write64le(Expected.data() + 2048, 733);
      auto R =
          run("copy_sized", {Buffer + 16, Source, Count, Route, Buffer + 2048});
      returned(R, 3);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      unsigned Calls = 0;
      for (const auto &Call : R.NativeCalls) {
        if (Call.Name != "strlcpy")
          continue;
        EXPECT_EQ(Call.Arguments[2], Count);
        EXPECT_EQ(Call.Result, 3);
        EXPECT_EQ(Call.Library, Route ? "libcopy.so" : "");
        ++Calls;
      }
      EXPECT_EQ(Calls, 1u);
    }
  }
}
TEST_P(AndroidStringCopy, SizedCopyStillScansWholeReadOnlySourceWhenTruncated) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[8189] = 'u';
  Bytes[8190] = 'v';
  Bytes[8191] = 0;
  for (uint64_t Count : {0u, 1u, 2u, 3u, 9u}) {
    auto Expected = Bytes;
    if (Count) {
      const size_t Copied = std::min<uint64_t>(2, Count - 1);
      std::copy_n(Bytes.begin() + 8189, Copied, Expected.begin() + 16);
      Expected[16 + Copied] = 0;
    }
    auto R = run("copy_sized_readonly",
                 {Source, Count ? Buffer + 16 : UINT64_MAX, End - 3, Count});
    returned(R, 2);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  Bytes[8191] = 'w';
  for (uint64_t Count : {0u, 1u, 2u})
    rejected(run("copy_sized_readonly", {Source, Buffer, End - 2, Count}),
             "strlcpy", "invalid guest pointer");
}
TEST_P(AndroidStringCopy, SizedCopyValidatesOnlyWrittenExtentAndNullRules) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[128] = 'x';
  Bytes[129] = 0;
  auto Expected = Bytes;
  Expected[8190] = 'x';
  Expected[8191] = 0;
  llvm::support::endian::write64le(Expected.data() + 2048, 733);
  auto R =
      run("copy_sized", {End - 2, Buffer + 128, UINT64_MAX, 0, Buffer + 2048});
  returned(R, 1);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  for (uint64_t Destination : {uint64_t(0), uint64_t(1), UINT64_MAX}) {
    R = run("copy_sized", {Destination, Buffer + 128, 0, 0, Buffer + 2048});
    returned(R, 1);
    auto Unchanged = Bytes;
    llvm::support::endian::write64le(Unchanged.data() + 2048, 733);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Unchanged);
  }
  rejected(run("copy_sized", {Buffer, 0, 0, 0, Buffer + 2048}), "strlcpy",
           "invalid guest pointer");
  rejected(run("copy_sized", {UINT64_MAX, Buffer + 128, 1, 0, Buffer + 2048}),
           "strlcpy", "invalid guest pointer");
  rejected(run("copy_sized_readonly", {Source, End - 2, Buffer + 128, 2}),
           "strlcpy", "invalid guest pointer");
}
TEST_P(AndroidStringCopy, SizedCopyOverlapIncludesFullSourceAndNul) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[128] = 'a';
  Bytes[129] = 'b';
  Bytes[130] = 'c';
  Bytes[131] = 0;
  for (uint64_t Destination : {Buffer + 128, Buffer + 130, Buffer + 131})
    rejected(
        run("copy_sized", {Destination, Buffer + 128, 1, 0, Buffer + 2048}),
        "strlcpy", "overlapping strlcpy");
  rejected(run("copy_sized", {Buffer + 127, Buffer + 128, 4, 0, Buffer + 2048}),
           "strlcpy", "overlapping strlcpy");
  auto R = run("copy_sized", {Buffer + 128, Buffer + 128, 0, 0, Buffer + 2048});
  returned(R, 3);
  R = run("copy_sized", {Buffer + 132, Buffer + 128, 16, 0, Buffer + 2048});
  returned(R, 3);
  auto Expected = Bytes;
  std::copy_n(Bytes.begin() + 128, 4, Expected.begin() + 132);
  llvm::support::endian::write64le(Expected.data() + 2048, 733);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}
TEST_P(AndroidStringCopy, SizedCopyEmptySourceTerminatesWithoutPadding) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[4096] = 0;
  for (uint64_t Count : {uint64_t(0), uint64_t(1), UINT64_MAX}) {
    auto Expected = Bytes;
    if (Count)
      Expected[16] = 0;
    llvm::support::endian::write64le(Expected.data() + 2048, 733);
    auto R = run("copy_sized", {Buffer + 16, Source, Count, 0, Buffer + 2048});
    returned(R, 0);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}
TEST_P(AndroidStringCopy, SizedCopyClosedProviderRemainsUnsupported) {
  Options.Android->Libraries["libcopy.so"] = {"strlcpy"};
  Options.Android->Memory[0].Bytes.assign(8192, 0);
  auto R = run("copy_sized", {Buffer, Source, 0, 2, Buffer + 2048});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "strlcpy");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  EXPECT_FALSE(R.ReturnValue);
}
TEST_P(AndroidStringCopy, CheckedSizedCopyLengthTerminationAndErrno) {
  Options.Android->Libraries["libcopy-sized-checked.so"] = {"__strlcpy_chk"};
  for (uint64_t Count : {uint64_t(0), uint64_t(1), uint64_t(3), uint64_t(4),
                         uint64_t(7), uint64_t(0x100000001), UINT64_MAX}) {
    for (uint64_t Route : {0u, 1u}) {
      SCOPED_TRACE(testing::Message() << Count << ":" << Route);
      auto &Bytes = Options.Android->Memory[0].Bytes;
      Bytes.assign(8192, 0xa5);
      const std::array<uint8_t, 4> Text = {0x80, 0xff, 'x', 0};
      std::copy(Text.begin(), Text.end(), Bytes.begin() + 4096);
      auto Expected = Bytes;
      if (Count) {
        const size_t Copied = std::min<uint64_t>(3, Count - 1);
        std::copy_n(Text.begin(), Copied, Expected.begin() + 16);
        Expected[16 + Copied] = 0;
      }
      llvm::support::endian::write64le(Expected.data() + 2048, 733);
      auto R = run("copy_sized_checked",
                   {Buffer + 16, Source, Count, Count, Route, Buffer + 2048});
      returned(R, 3);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      unsigned Calls = 0;
      uint64_t Target = 0;
      for (const auto &Call : R.NativeCalls) {
        if (Call.Name == "dlsym") {
          EXPECT_EQ(Call.Symbol, "__strlcpy_chk");
          ASSERT_TRUE(Call.Result);
          Target = *Call.Result;
        }
        if (Call.Name != "__strlcpy_chk")
          continue;
        EXPECT_EQ(Call.Arguments[2], Count);
        EXPECT_EQ(Call.Arguments[3], Count);
        EXPECT_EQ(Call.Result, 3);
        EXPECT_EQ(Call.Library, Route ? "libcopy-sized-checked.so" : "");
        if (Route)
          EXPECT_EQ(Call.PC, Target);
        ++Calls;
      }
      EXPECT_EQ(Calls, 1u);
    }
  }
}
TEST_P(AndroidStringCopy, CheckedSizedCopyBoundsPrecedeAccessAndTruncation) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[4096] = 0;
  Options.Android->Libraries["libcopy-sized-checked.so"] = {"__strlcpy_chk"};
  for (uint64_t Route : {0u, 1u}) {
    for (uint64_t Count : {uint64_t(1), uint64_t(0x100000001), UINT64_MAX})
      rejected(run("copy_sized_checked", {UINT64_MAX, UINT64_MAX, Count,
                                          Count - 1, Route, Buffer + 2048}),
               "__strlcpy_chk", "FORTIFY destination extent exceeded");
    // Even an empty source cannot reduce the declared capacity check.
    rejected(
        run("copy_sized_checked", {Buffer, Source, 7, 1, Route, Buffer + 2048}),
        "__strlcpy_chk", "FORTIFY destination extent exceeded");
  }
  auto Expected = Bytes;
  Expected[0] = 0;
  llvm::support::endian::write64le(Expected.data() + 2048, 733);
  auto R = run("copy_sized_checked",
               {Buffer, Source, 7, UINT64_MAX, 0, Buffer + 2048});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}
TEST_P(AndroidStringCopy, CheckedSizedCopyPageEndsAndZeroCapacity) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0xa5);
  Bytes[8189] = 'u';
  Bytes[8190] = 'v';
  Bytes[8191] = 0;
  for (uint64_t Count : {0u, 1u, 2u, 3u, 9u}) {
    auto Expected = Bytes;
    if (Count) {
      const size_t Copied = std::min<uint64_t>(2, Count - 1);
      std::copy_n(Bytes.begin() + 8189, Copied, Expected.begin() + 16);
      Expected[16 + Copied] = 0;
    }
    auto R =
        run("copy_sized_checked_readonly",
            {Source, Count ? Buffer + 16 : UINT64_MAX, End - 3, Count, Count});
    returned(R, 2);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  Bytes[128] = 'x';
  Bytes[129] = 0;
  auto Expected = Bytes;
  Expected[8190] = 'x';
  Expected[8191] = 0;
  llvm::support::endian::write64le(Expected.data() + 2048, 733);
  auto R = run("copy_sized_checked", {End - 2, Buffer + 128, UINT64_MAX,
                                      UINT64_MAX, 0, Buffer + 2048});
  returned(R, 1);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  rejected(
      run("copy_sized_checked_readonly", {Source, End - 2, Buffer + 128, 2, 2}),
      "__strlcpy_chk", "invalid guest pointer");
  rejected(
      run("copy_sized_checked", {Buffer, UINT64_MAX, 0, 0, 0, Buffer + 2048}),
      "__strlcpy_chk", "invalid guest pointer");
  Bytes[8191] = 'w';
  for (uint64_t Count : {0u, 1u, 2u})
    rejected(run("copy_sized_checked_readonly",
                 {Source, Buffer, End - 2, Count, Count}),
             "__strlcpy_chk", "invalid guest pointer");
}
TEST_P(AndroidStringCopy, CheckedSizedCopyClosedProviderPrecedesFortify) {
  Options.Android->Libraries["libcopy-sized-checked.so"] = {"__strlcpy_chk"};
  Options.Android->Memory[0].Bytes.assign(8192, 0);
  auto R = run("copy_sized_checked",
               {UINT64_MAX, UINT64_MAX, 1, 0, 2, Buffer + 2048});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "__strlcpy_chk");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  EXPECT_FALSE(R.ReturnValue);
}
TEST_P(AndroidStringCopy, ExactBytesPaddingTruncationReturnAndErrno) {
  for (uint64_t Count : {0u, 1u, 3u, 4u, 7u, 1024u}) {
    SCOPED_TRACE(Count);
    auto &Bytes = Options.Android->Memory[0].Bytes;
    Bytes.assign(8192, 0x5a);
    const std::array<uint8_t, 5> Text = {0x80, 0xff, 'x', 0, 'q'};
    std::copy(Text.begin(), Text.end(), Bytes.begin() + 4096);
    auto Expected = Bytes;
    const std::array<uint8_t, 3> Prefix = {0x80, 0xff, 'x'};
    for (size_t I = 0; I < Count; ++I)
      Expected[16 + I] = I < Prefix.size() ? Prefix[I] : 0;
    llvm::support::endian::write32le(Expected.data() + 2048, 733);
    auto R = run("copy_supplied", {Buffer + 16, Source, Count, Buffer + 2048});
    returned(R, Buffer + 16);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    EXPECT_TRUE(R.Services.empty());
  }
}

TEST_P(AndroidStringCopy, SourceStopsAtNulOrCountEvenAtReadOnlyPageEnd) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[8189] = 'u';
  Bytes[8190] = 'v';
  Bytes[8191] = 0;
  auto Expected = Bytes;
  std::fill(Expected.begin() + 16, Expected.begin() + 26, 0);
  Expected[16] = 'u';
  Expected[17] = 'v';
  auto R = run("copy_readonly", {Source, Buffer + 16, End - 3, 10});
  returned(R, Buffer + 16);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);

  Bytes[8191] = 'w';
  Expected = Bytes;
  Expected[16] = 'v';
  Expected[17] = 'w';
  R = run("copy_readonly", {Source, Buffer + 16, End - 2, 2});
  returned(R, Buffer + 16);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  rejected(run("copy_readonly", {Source, Buffer + 16, End - 2, 3}), "strncpy",
           "invalid guest pointer");
}

TEST_P(AndroidStringCopy,
       DestinationCanSpanPagesAndPartialWriteFaultIsRejected) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[128] = 'p';
  Bytes[129] = 0;
  auto Expected = Bytes;
  std::fill(Expected.begin() + 4094, Expected.begin() + 4102, 0);
  Expected[4094] = 'p';
  auto R = run("copy_raw", {Source - 2, Buffer + 128, 8});
  returned(R, Source - 2);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  rejected(run("copy_readonly", {Source, Source - 2, Buffer + 128, 8}),
           "strncpy", "invalid guest pointer");
}

TEST_P(AndroidStringCopy, ZeroCountDoesNotAccessEitherPointer) {
  for (auto Address : {uint64_t(0), uint64_t(1), UINT64_MAX}) {
    auto R = run("copy_raw", {Address, UINT64_MAX, 0});
    returned(R, Address);
  }
}

TEST_P(AndroidStringCopy, DestinationBoundsPermissionsAndFullWidthCount) {
  Options.Android->Memory[0].Bytes.assign(8192, 0);
  rejected(run("copy_readonly", {Buffer, Buffer, Source, 1}), "strncpy",
           "invalid guest pointer");
  rejected(run("copy_raw", {End - 1, Buffer, 2}), "strncpy",
           "invalid guest pointer");
  rejected(run("copy_raw", {UINT64_MAX, Buffer, 2}), "strncpy",
           "invalid guest pointer");
  rejected(run("copy_raw", {Buffer, UINT64_MAX, 1}), "strncpy",
           "invalid guest pointer");
  for (uint64_t Count : {UINT64_MAX, uint64_t(0x100000001)})
    rejected(run("copy_raw", {Buffer, Source, Count}), "strncpy",
             "memory limit");
}

TEST_P(AndroidStringCopy, UndefinedOverlapIncludesPaddingButNotUnreadSource) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[128] = 'a';
  Bytes[129] = 'b';
  Bytes[130] = 'c';
  Bytes[131] = 0;
  for (auto Destination : {Buffer + 128, Buffer + 129, Buffer + 127})
    rejected(run("copy_raw", {Destination, Buffer + 128, 4}), "strncpy",
             "overlapping strncpy");
  rejected(run("copy_raw", {Buffer + 112, Buffer + 128, 32}), "strncpy",
           "overlapping strncpy");
  // Disjoint accessed ranges in one mapped allocation are allowed.
  auto Expected = Bytes;
  const std::array<uint8_t, 8> Copy = {'a', 'b', 'c', 0, 0, 0, 0, 0};
  std::copy(Copy.begin(), Copy.end(), Expected.begin() + 132);
  auto R = run("copy_raw", {Buffer + 132, Buffer + 128, 8});
  returned(R, Buffer + 132);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidStringCopy, DynamicProviderIdentityAndClosedLibrary) {
  Options.Android->Libraries = {{"libcopy.so", {"strncpy"}}};
  Options.Android->Memory[0].Bytes.assign(8192, 0x5a);
  auto R = run("copy_dynamic", {Buffer, 0});
  returned(R, Buffer);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const std::vector<uint8_t> Expected = {'a', 'b', 0, 0, 0, 0, 0x5a};
  EXPECT_EQ(std::vector<uint8_t>(R.MemorySnapshots[0].Bytes.begin(),
                                 R.MemorySnapshots[0].Bytes.begin() + 7),
            Expected);
  unsigned Calls = 0;
  uint64_t Target = 0;
  for (const auto &C : R.NativeCalls) {
    if (C.Name == "dlsym") {
      EXPECT_EQ(C.Symbol, "strncpy");
      ASSERT_TRUE(C.Result);
      Target = *C.Result;
    }
    if (C.Name == "strncpy") {
      EXPECT_EQ(C.Library, "libcopy.so");
      EXPECT_EQ(C.PC, Target);
      EXPECT_EQ(C.Result, Buffer);
      ++Calls;
    }
  }
  EXPECT_EQ(Calls, 1u);
  R = run("copy_dynamic", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "strncpy");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidStringCopy, CheckedBytesPaddingTruncationAndErrno) {
  for (unsigned Mode : {0u, 1u}) {
    for (const auto [Count, SourceSize] : {std::pair{0u, 0u},
                                           {1u, 1u},
                                           {3u, 3u},
                                           {4u, 4u},
                                           {7u, 4u},
                                           {1024u, 4u}}) {
      SCOPED_TRACE(testing::Message() << Mode << ":" << Count);
      auto &Bytes = Options.Android->Memory[0].Bytes;
      Bytes.assign(8192, 0x5a);
      const std::array<uint8_t, 5> Text = {0x80, 0xff, 'x', 0, 'q'};
      std::copy(Text.begin(), Text.end(), Bytes.begin() + 4096);
      auto Expected = Bytes;
      const std::array<uint8_t, 3> Prefix = {0x80, 0xff, 'x'};
      for (size_t I = 0; I < Count; ++I)
        Expected[16 + I] = I < Prefix.size() ? Prefix[I] : 0;
      llvm::support::endian::write32le(Expected.data() + 2048, 733);
      auto R = run("copy_checked_supplied", {Buffer + 16, Source, Count, Count,
                                             SourceSize, Mode, Buffer + 2048});
      returned(R, Buffer + 16);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      EXPECT_TRUE(R.Services.empty());
      auto Call = std::find_if(
          R.NativeCalls.begin(), R.NativeCalls.end(),
          [&](const auto &C) { return C.Name == CheckedNames[Mode]; });
      ASSERT_NE(Call, R.NativeCalls.end());
      EXPECT_EQ(Call->Result, Buffer + 16);
      EXPECT_EQ(Call->Arguments[2], Count);
      EXPECT_EQ(Call->Arguments[3], Count);
      if (Mode)
        EXPECT_EQ(Call->Arguments[4], SourceSize);
    }
  }
}

TEST_P(AndroidStringCopy, CheckedDestinationExtentPrecedesMemoryAccess) {
  Options.Android->Memory[0].Bytes.assign(8192, 'x');
  for (unsigned Mode : {0u, 1u}) {
    for (uint64_t Count : {uint64_t(1), uint64_t(0x100000001), UINT64_MAX}) {
      auto R = run("copy_checked",
                   {UINT64_MAX, UINT64_MAX, Count, Count - 1, 0, Mode});
      rejected(R, CheckedNames[Mode], "FORTIFY destination extent exceeded");
    }
    // A source NUL cannot reduce the count written, because strncpy pads.
    Options.Android->Memory[0].Bytes[4096] = 0;
    rejected(
        run("copy_checked_readonly", {Buffer, Buffer, Source, 7, 1, 1, Mode}),
        CheckedNames[Mode], "FORTIFY destination extent exceeded");
  }
}

TEST_P(AndroidStringCopy, CheckedZeroCountAndFullWidthExtents) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[4096] = 'a';
  Bytes[4097] = 0;
  for (unsigned Mode : {0u, 1u}) {
    for (uint64_t Address : {uint64_t(0), uint64_t(1), UINT64_MAX}) {
      auto R = run("copy_checked", {Address, UINT64_MAX, 0, 0, 0, Mode});
      returned(R, Address);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
    }
    auto Expected = Bytes;
    Expected[16] = 'a';
    std::fill(Expected.begin() + 17, Expected.begin() + 23, 0);
    for (uint64_t Extent : {uint64_t(0x100000000), UINT64_MAX}) {
      auto R =
          run("copy_checked", {Buffer + 16, Source, 7, Extent, Extent, Mode});
      returned(R, Buffer + 16);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidStringCopy, CheckedSourceExtentCountsReadNulButNotPadding) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[4096] = 'a';
  Bytes[4097] = 'b';
  Bytes[4098] = 'c';
  Bytes[4099] = 0;
  // Truncation needs no terminator. Padding only needs the source through NUL.
  for (const auto [Count, SourceSize] :
       {std::pair{3u, 3u}, {4u, 4u}, {16u, 4u}}) {
    auto Expected = Bytes;
    std::fill(Expected.begin(), Expected.begin() + Count, 0);
    Expected[0] = 'a';
    Expected[1] = 'b';
    Expected[2] = 'c';
    auto R = run("copy_checked", {Buffer, Source, Count, Count, SourceSize, 1});
    returned(R, Buffer);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  for (uint64_t SourceSize : {0u, 2u, 3u})
    rejected(run("copy_checked", {Buffer, Source, 16, 16, SourceSize, 1}),
             "__strncpy_chk2", "FORTIFY read exceeds object bounds");
  rejected(run("copy_checked", {Buffer, Source + 3, 7, 7, 0, 1}),
           "__strncpy_chk2", "FORTIFY read exceeds object bounds");
  auto Expected = Bytes;
  std::fill(Expected.begin(), Expected.begin() + 7, 0);
  auto R = run("copy_checked", {Buffer, Source + 3, 7, 7, 1, 1});
  returned(R, Buffer);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  // The four-argument entry has no source-object check.
  R = run("copy_checked", {Buffer, Source + 3, 7, 7, 0, 0});
  returned(R, Buffer);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidStringCopy, CheckedSourceScanFaultPrecedesSourceFortify) {
  Options.Android->Memory[0].Bytes.assign(8192, 'x');
  for (uint64_t SourceSize : {0u, 1u}) {
    // The next byte is inaccessible, even though the source extent is
    // exhausted.
    rejected(run("copy_checked", {Buffer, End - 1, 2, 2, SourceSize, 1}),
             "__strncpy_chk2", "invalid guest pointer");
  }
  rejected(run("copy_checked", {Buffer, End - 1, 1, 1, 0, 1}), "__strncpy_chk2",
           "FORTIFY read exceeds object bounds");
  rejected(run("copy_checked", {Buffer, UINT64_MAX, 1, 1, 0, 1}),
           "__strncpy_chk2", "invalid guest pointer");
}

TEST_P(AndroidStringCopy, CheckedPageTailReadOnlyInputsAndDestinationFaults) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[8189] = 'u';
  Bytes[8190] = 'v';
  Bytes[8191] = 0;
  for (unsigned Mode : {0u, 1u}) {
    auto Expected = Bytes;
    std::fill(Expected.begin() + 16, Expected.begin() + 26, 0);
    Expected[16] = 'u';
    Expected[17] = 'v';
    auto R = run("copy_checked_readonly",
                 {Source, Buffer + 16, End - 3, 10, 10, 3, Mode});
    returned(R, Buffer + 16);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    rejected(run("copy_checked_readonly",
                 {Source, Source - 1, Buffer, 2, 2, 2, Mode}),
             CheckedNames[Mode], "invalid guest pointer");
    rejected(
        run("copy_checked_readonly", {Buffer, Buffer, End - 1, 1, 1, 1, Mode}),
        CheckedNames[Mode], "invalid guest pointer");
  }
}

TEST_P(AndroidStringCopy, CheckedDynamicProviderIdentityAndLifetime) {
  Options.Android->Libraries = {
      {"libcopy-checked.so", {"__strncpy_chk", "__strncpy_chk2"}}};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[4096] = 'a';
  Bytes[4097] = 0;
  auto Expected = Bytes;
  Expected[0] = 'a';
  std::fill(Expected.begin() + 1, Expected.begin() + 7, 0);
  for (unsigned Mode : {0u, 1u}) {
    auto R = run("copy_checked_dynamic", {Buffer, Source, 7, 7, 2, Mode, 0});
    returned(R, Buffer);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    uint64_t Target = 0;
    unsigned Calls = 0;
    for (const auto &Call : R.NativeCalls) {
      if (Call.Name == "dlsym") {
        EXPECT_EQ(Call.Symbol, CheckedNames[Mode]);
        ASSERT_TRUE(Call.Result);
        Target = *Call.Result;
      }
      if (Call.Name == CheckedNames[Mode]) {
        EXPECT_EQ(Call.Library, "libcopy-checked.so");
        EXPECT_EQ(Call.PC, Target);
        EXPECT_EQ(Call.Result, Buffer);
        ++Calls;
      }
    }
    EXPECT_NE(Target, 0u);
    EXPECT_EQ(Calls, 1u);
    rejected(run("copy_checked_dynamic", {Buffer, Source, 7, 6, 2, Mode, 0}),
             CheckedNames[Mode], "FORTIFY destination extent exceeded");
    R = run("copy_checked_dynamic", {UINT64_MAX, UINT64_MAX, 1, 0, 0, Mode, 1});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, CheckedNames[Mode]);
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_FALSE(R.ReturnValue);
  }
}

INSTANTIATE_TEST_SUITE_P(
    API28, AndroidStringCopy,
    testing::Combine(testing::Values("copy-O0-none", "copy-O0-android",
                                     "copy-O0-relr", "copy-O2-none",
                                     "copy-O2-android", "copy-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));

TEST(AndroidStringCopyMemory, CheckedAppendRefusalsPreserveCompleteBacking) {
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
  Before[16] = 'a';
  Before[17] = 0;
  Before[128] = 'x';
  Before[129] = 'y';
  Before[130] = 0;
  Before[4094] = 'p';
  Before[4095] = 0;
  llvm::cantFail(CPU.write(Buffer, Before));
  llvm::cantFail(Space.protect(Source, 4096, Read | UserAccessible));
  ProcessOptions Options;
  Options.Android.emplace();
  Options.MemoryLimit = 16;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, End, Options, Result);
  const android_model::LinkedImage Linked{};
  for (bool Bounded : {false, true}) {
    SCOPED_TRACE(Bounded);
    auto makeCall = [&](uint64_t Destination, uint64_t Input, uint64_t Count,
                        uint64_t Extent) {
      NativeCallEvent Call{};
      Call.Name = Bounded ? "__strncat_chk" : "__strcat_chk";
      Call.Arguments = {Destination, Input, Bounded ? Count : Extent, Extent};
      return Call;
    };
    struct Case {
      uint64_t Destination, Input, Count, Extent;
      const char *Diagnostic;
    };
    for (auto C :
         {Case{Buffer + 256, UINT64_MAX, 1, 0, "string scan exceeds"},
          {Buffer + 16, Buffer + 256, 17, UINT64_MAX, "string scan exceeds"},
          {Buffer + 16, Buffer + 256, 16, UINT64_MAX,
           Bounded ? "native memory operation exceeds" : "string scan exceeds"},
          {Buffer + 16, End - 1, 1, 2, "FORTIFY destination"},
          {Buffer + 16, UINT64_MAX, 1, 1, "FORTIFY read"},
          {Buffer + 16, UINT64_MAX, 1, UINT64_MAX, "invalid guest pointer"},
          {Source - 2, Buffer + 128, 3, UINT64_MAX, "invalid guest pointer"},
          {Buffer + 16, Buffer + 16, 1, UINT64_MAX,
           "overlapping string concatenation"}}) {
      SCOPED_TRACE(testing::Message()
                   << C.Destination << ":" << C.Input << ":" << C.Count);
      auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
      android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                  Linked);
      auto Call = makeCall(C.Destination, C.Input, C.Count, C.Extent);
      auto Value = Model.invoke(Call);
      ASSERT_FALSE(bool(Value));
      EXPECT_NE(llvm::toString(Value.takeError()).find(C.Diagnostic),
                std::string::npos);
      std::vector<uint8_t> After(8192);
      llvm::cantFail(Space.snapshotBacking(Buffer, After));
      EXPECT_EQ(After, Before);
    }
    for (uint64_t Count : {0u, 1u}) {
      auto Budget = llvm::cantFail(ExecutionBudget::create(
          {1, 1, 1}, ExecutionBudget::Clock::time_point::min()));
      android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                  Linked);
      auto Call = makeCall(UINT64_MAX, UINT64_MAX, Count, 0);
      auto Value = Model.invoke(Call);
      if (Bounded && !Count) {
        ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
        ASSERT_TRUE(*Value);
        EXPECT_EQ(std::get<uint64_t>(**Value), UINT64_MAX);
        EXPECT_FALSE(Model.timedOut());
      } else {
        ASSERT_FALSE(bool(Value));
        EXPECT_NE(llvm::toString(Value.takeError()).find("timed out"),
                  std::string::npos);
        EXPECT_TRUE(Model.timedOut());
      }
      std::vector<uint8_t> After(8192);
      llvm::cantFail(Space.snapshotBacking(Buffer, After));
      EXPECT_EQ(After, Before);
    }
  }
}

TEST(AndroidStringCopyMemory, TerminatedCopyRefusalsPreserveCompleteBacking) {
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
  Before[128] = 'a';
  Before[129] = 'b';
  Before[130] = 'c';
  Before[131] = 0;
  llvm::cantFail(CPU.write(Buffer, Before));
  llvm::cantFail(Space.protect(Source, 4096, Read | UserAccessible));
  ProcessOptions Options;
  Options.Android.emplace();
  Options.MemoryLimit = 16;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, End, Options, Result);
  const android_model::LinkedImage Linked{};
  for (const char *Name : {"strcpy", "__strcpy_chk"}) {
    SCOPED_TRACE(Name);
    struct Case {
      uint64_t Destination, Input, Extent;
      const char *Diagnostic;
    };
    for (auto C : {Case{Buffer, Buffer + 256, 0, "unterminated"},
                   {Buffer, End - 1, 0, "invalid guest pointer"},
                   {Buffer, UINT64_MAX, 0, "invalid guest pointer"},
                   {UINT64_MAX, Buffer + 128, 4, "invalid guest pointer"},
                   {Source - 1, Buffer + 128, 4, "invalid guest pointer"},
                   {Buffer + 127, Buffer + 128, 4, "overlapping strcpy"},
                   {Buffer + 131, Buffer + 128, 4, "overlapping strcpy"}}) {
      SCOPED_TRACE(testing::Message() << C.Destination << ":" << C.Input);
      auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
      android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                  Linked);
      NativeCallEvent Call{};
      Call.Name = Name;
      Call.Arguments = {C.Destination, C.Input, C.Extent};
      auto Value = Model.invoke(Call);
      ASSERT_FALSE(bool(Value));
      EXPECT_NE(llvm::toString(Value.takeError()).find(C.Diagnostic),
                std::string::npos);
      std::vector<uint8_t> After(8192);
      llvm::cantFail(Space.snapshotBacking(Buffer, After));
      EXPECT_EQ(After, Before);
    }
    auto Budget = llvm::cantFail(ExecutionBudget::create(
        {1, 1, 1}, ExecutionBudget::Clock::time_point::min()));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = Name;
    // Even an insufficient fortify bound must not skip the source scan.
    Call.Arguments = {Buffer, Buffer + 128, 0};
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find("timed out"),
              std::string::npos);
    EXPECT_TRUE(Model.timedOut());
    std::vector<uint8_t> After(8192);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
  for (uint64_t Extent : {0u, 3u}) {
    auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = "__strcpy_chk";
    Call.Arguments = {Buffer, Buffer + 128, Extent};
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find("FORTIFY destination"),
              std::string::npos);
    std::vector<uint8_t> After(8192);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
}

TEST(AndroidStringCopyMemory, SizedCopyRefusalsPreserveCompleteBacking) {
  for (const char *Name : {"strlcpy", "__strlcpy_chk"}) {
    SCOPED_TRACE(Name);
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
    Before[128] = 'a';
    Before[129] = 'b';
    Before[130] = 'c';
    Before[131] = 0;
    llvm::cantFail(CPU.write(Buffer, Before));
    ProcessOptions Options;
    Options.Android.emplace();
    Options.MemoryLimit = 16;
    const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
    ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                         GuestArchitecture::AArch64,
                         ExecutionBackendKind::Unicorn,
                         {}};
    linux_model::LinuxServices Kernel(CPU, Layout, End, Options, Result);
    const android_model::LinkedImage Linked{};
    struct Case {
      uint64_t Destination, Input, Capacity;
    };
    for (auto C : {Case{Buffer, Buffer + 256, 0},
                   {Buffer, Buffer + 256, 1},
                   {Buffer, End - 1, 1},
                   {End - 1, Buffer + 128, 3},
                   {UINT64_MAX, Buffer + 128, 1},
                   {Buffer + 130, Buffer + 128, 1},
                   {Buffer, UINT64_MAX, 0}}) {
      auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
      android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                  Linked);
      NativeCallEvent Call{};
      Call.Name = Name;
      Call.Arguments = {C.Destination, C.Input, C.Capacity, UINT64_MAX};
      auto Value = Model.invoke(Call);
      ASSERT_FALSE(bool(Value));
      llvm::consumeError(Value.takeError());
      std::vector<uint8_t> After(8192);
      llvm::cantFail(Space.snapshotBacking(Buffer, After));
      EXPECT_EQ(After, Before);
    }
    llvm::cantFail(Space.protect(Source, 4096, Read | UserAccessible));
    {
      auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
      android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                  Linked);
      NativeCallEvent Call{};
      Call.Name = Name;
      Call.Arguments = {Source - 1, Buffer + 128, 3, UINT64_MAX};
      auto Value = Model.invoke(Call);
      ASSERT_FALSE(bool(Value));
      EXPECT_NE(llvm::toString(Value.takeError()).find("invalid guest pointer"),
                std::string::npos);
      std::vector<uint8_t> After(8192);
      llvm::cantFail(Space.snapshotBacking(Buffer, After));
      EXPECT_EQ(After, Before);
    }
    auto Budget = llvm::cantFail(ExecutionBudget::create(
        {1, 1, 1}, ExecutionBudget::Clock::time_point::min()));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = Name;
    Call.Arguments = {Buffer, Buffer + 128, 4, UINT64_MAX};
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find("timed out"),
              std::string::npos);
    EXPECT_TRUE(Model.timedOut());
    std::vector<uint8_t> After(8192);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
    if (llvm::StringRef(Name) == "__strlcpy_chk") {
      // The object-size check precedes source access, even after expiry.
      auto Expired = llvm::cantFail(ExecutionBudget::create(
          {1, 1, 1}, ExecutionBudget::Clock::time_point::min()));
      android_model::Bionic Checked(CPU, Kernel, Layout, Options, Result,
                                    *Expired, Linked);
      Call.Arguments = {Buffer, UINT64_MAX, 1, 0};
      auto Rejected = Checked.invoke(Call);
      ASSERT_FALSE(bool(Rejected));
      EXPECT_NE(
          llvm::toString(Rejected.takeError()).find("FORTIFY destination"),
          std::string::npos);
      EXPECT_FALSE(Checked.timedOut());
      llvm::cantFail(Space.snapshotBacking(Buffer, After));
      EXPECT_EQ(After, Before);
    }
  }
}
TEST(AndroidStringCopyMemory, RefusalsAreAtomicAndBudgetIsNotTruncated) {
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
  Before[4096] = 0;
  llvm::cantFail(CPU.write(Buffer, Before));
  ProcessOptions Options;
  Options.Android.emplace();
  Options.MemoryLimit = 16;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, End, Options, Result);
  const android_model::LinkedImage Linked{};
  struct Case {
    uint64_t Destination, Input, Count;
    const char *Error;
  };
  for (auto C : {Case{Buffer, Source, 17, "memory limit"},
                 {Buffer, Source, 0x100000001, "memory limit"},
                 {End - 1, Source, 2, "invalid guest pointer"},
                 {Buffer, End - 1, 2, "invalid guest pointer"},
                 {Buffer, Buffer + 1, 16, "overlapping strncpy"}}) {
    SCOPED_TRACE(C.Count);
    auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = "strncpy";
    Call.Arguments = {C.Destination, C.Input, C.Count};
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(C.Error),
              std::string::npos);
    std::vector<uint8_t> After(8192);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
  // A writable prefix followed by a read-only page cannot be half-published.
  llvm::cantFail(Space.protect(Source, 4096, Read | UserAccessible));
  {
    auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = "strncpy";
    Call.Arguments = {Source - 1, Buffer, 2};
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find("invalid guest pointer"),
              std::string::npos);
    std::vector<uint8_t> After(8192);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
    // The exact limit is admitted and the read-only source NUL needs one byte.
    Call.Arguments = {Buffer, Source, 16};
    Value = Model.invoke(Call);
    ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
    ASSERT_TRUE(*Value);
    EXPECT_EQ(std::get<uint64_t>(**Value), Buffer);
    std::fill(Before.begin(), Before.begin() + 16, 0);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
  auto Budget = llvm::cantFail(ExecutionBudget::create(
      {1, 1, 1}, ExecutionBudget::Clock::time_point::min()));
  android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                              Linked);
  NativeCallEvent Call{};
  Call.Name = "strncpy";
  Call.Arguments = {Buffer, Source, 16};
  auto Value = Model.invoke(Call);
  ASSERT_FALSE(bool(Value));
  EXPECT_NE(llvm::toString(Value.takeError()).find("timed out"),
            std::string::npos);
  EXPECT_TRUE(Model.timedOut());
  std::vector<uint8_t> After(8192);
  llvm::cantFail(Space.snapshotBacking(Buffer, After));
  EXPECT_EQ(After, Before);
}
TEST(AndroidStringCopyMemory, CheckedBoundsAndRefusalsAreAtomic) {
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
  Before[4099] = 0;
  llvm::cantFail(CPU.write(Buffer, Before));
  llvm::cantFail(Space.protect(Source, 4096, Read | UserAccessible));
  ProcessOptions Options;
  Options.Android.emplace();
  Options.MemoryLimit = 16;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, End, Options, Result);
  const android_model::LinkedImage Linked{};
  struct Case {
    unsigned Mode;
    uint64_t Destination, Input, Count, DestinationSize, SourceSize;
    bool Expired, TimesOut;
    const char *Error;
  };
  for (const auto C : {
           Case{0, Buffer, Source, 17, 17, UINT64_MAX, false, false,
                "memory limit"},
           {1, Buffer, Source, 17, 17, 0, false, false, "memory limit"},
           {0, UINT64_MAX, UINT64_MAX, 17, 16, 0, true, false,
            "FORTIFY destination"},
           {1, UINT64_MAX, UINT64_MAX, 17, 16, 0, true, false,
            "FORTIFY destination"},
           {0, Buffer, Source, 0x100000001, 1, 0, false, false,
            "FORTIFY destination"},
           {1, Buffer, Source, 0x100000001, 1, 0, false, false,
            "FORTIFY destination"},
           {0, Buffer, Source, 0x100000001, UINT64_MAX, UINT64_MAX, false,
            false, "memory limit"},
           {1, Buffer, Source, 0x100000001, UINT64_MAX, UINT64_MAX, false,
            false, "memory limit"},
           {0, Source - 1, Buffer, 2, 2, 2, false, false,
            "invalid guest pointer"},
           {1, Source - 1, Buffer, 2, 2, 0, false, false,
            "invalid guest pointer"},
           {0, Buffer, End - 1, 2, 2, 0, false, false, "invalid guest pointer"},
           {1, Buffer, End - 1, 2, 2, 0, false, false, "invalid guest pointer"},
           {0, Buffer, Buffer + 1, 4, 4, 4, false, false,
            "overlapping strncpy"},
           {1, Buffer, Buffer + 1, 4, 4, 4, false, false,
            "overlapping strncpy"},
           {1, Buffer, Source, 16, 16, 3, false, false, "FORTIFY read"},
           {1, Buffer, Source + 3, 16, 16, 0, false, false, "FORTIFY read"},
           {0, Buffer, Source, 4, 4, 0, true, true, "timed out"},
           {1, Buffer, Source, 4, 4, 0, true, true, "timed out"},
           {0, UINT64_MAX, UINT64_MAX, 0, 0, 0, true, false, nullptr},
           {1, UINT64_MAX, UINT64_MAX, 0, 0, 0, true, false, nullptr},
       }) {
    SCOPED_TRACE(testing::Message()
                 << C.Mode << ":" << C.Count << ":" << C.DestinationSize << ":"
                 << C.SourceSize << ":" << C.Expired);
    auto Budget = llvm::cantFail(ExecutionBudget::create(
        Options.Limits, C.Expired ? ExecutionBudget::Clock::time_point::min()
                                  : ExecutionBudget::Clock::now()));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = CheckedNames[C.Mode];
    Call.Arguments = {C.Destination, C.Input, C.Count, C.DestinationSize,
                      C.SourceSize};
    auto Value = Model.invoke(Call);
    if (C.Error) {
      ASSERT_FALSE(bool(Value));
      EXPECT_NE(llvm::toString(Value.takeError()).find(C.Error),
                std::string::npos);
    } else {
      ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
      ASSERT_TRUE(*Value);
      EXPECT_EQ(std::get<uint64_t>(**Value), C.Destination);
    }
    EXPECT_EQ(Model.timedOut(), C.TimesOut);
    std::vector<uint8_t> After(Before.size());
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
}
} // namespace
} // namespace neverd::emulation
