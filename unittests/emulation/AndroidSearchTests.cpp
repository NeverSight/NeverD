//===- AndroidSearchTests.cpp - Guest character-search boundaries --------===//
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
#include <utility>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
constexpr uint64_t PageEnd = Buffer + 4096;
constexpr std::array<const char *, 4> SearchNames = {
    "strchr", "strrchr", "__strchr_chk", "__strrchr_chk"};

class AndroidSearch : public testing::TestWithParam<
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
    Options.Android->Memory.push_back({Buffer, 4096, {}, false});
    Options.Android->ReadMemory = {{Buffer, 128}, {PageEnd - 16, 16}};
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

TEST_P(AndroidSearch, MemorySearchFirstByteCountsErrnoAndProvider) {
  Options.Android->Libraries["libmemory-search.so"] = {"memchr",
                                                       "__memchr_chk"};
  Options.Android->ReadMemory = {{Buffer, 4096}};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 0xa5);
  const std::array<uint8_t, 6> Input = {0x80, 0, 'x', 0xff, 'x', 0};
  std::copy(Input.begin(), Input.end(), Bytes.begin() + 64);
  auto Expected = Bytes;
  llvm::support::endian::write64le(Expected.data() + 128, 733);
  struct Case {
    uint64_t Character, Count, Result;
  };
  for (uint64_t Checked : {0u, 1u}) {
    const char *Name = Checked ? "__memchr_chk" : "memchr";
    for (uint64_t Route : {0u, 1u}) {
      for (const auto C : {Case{0x180, 0, 0},
                           {0, 1, 0},
                           {0, 2, Buffer + 65},
                           {'x', 2, 0},
                           {'x', 5, Buffer + 66},
                           {UINT64_MAX, 4, Buffer + 67},
                           {'z', 6, 0},
                           {0xabcdef0180, 0x100000001, Buffer + 64},
                           {0x80, UINT64_MAX, Buffer + 64}}) {
        SCOPED_TRACE(testing::Message() << Name << ":" << Route << ":"
                                        << C.Character << ":" << C.Count);
        auto R = run("memory_search", {Buffer + 64, C.Character, C.Count,
                                       C.Count, Checked, Route, Buffer + 128});
        returned(R, C.Result);
        ASSERT_EQ(R.MemorySnapshots.size(), 1u);
        EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
        unsigned Matches = 0;
        uint64_t Target = 0;
        for (const auto &Call : R.NativeCalls) {
          if (Call.Name == "dlsym") {
            EXPECT_EQ(Call.Symbol, Name);
            ASSERT_TRUE(Call.Result);
            Target = *Call.Result;
          }
          if (Call.Name != Name)
            continue;
          EXPECT_EQ(Call.Arguments[2], C.Count);
          if (Checked)
            EXPECT_EQ(Call.Arguments[3], C.Count);
          EXPECT_EQ(Call.Result, C.Result);
          EXPECT_EQ(Call.Library, Route ? "libmemory-search.so" : "");
          if (Route)
            EXPECT_EQ(Call.PC, Target);
          ++Matches;
        }
        EXPECT_EQ(Matches, 1u);
      }
    }
  }
}

TEST_P(AndroidSearch, MemorySearchBoundsAndReadOnlyPageTail) {
  Options.Android->ReadMemory = {{Buffer, 4096}};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 0xa5);
  Bytes.back() = 'x';
  for (uint64_t Checked : {0u, 1u}) {
    const char *Name = Checked ? "__memchr_chk" : "memchr";
    for (uint64_t Pointer : {uint64_t(0), PageEnd, UINT64_MAX}) {
      auto R =
          run("memory_search", {Pointer, 0, 0, 0, Checked, 0, Buffer + 128});
      returned(R, 0);
      auto Expected = Bytes;
      llvm::support::endian::write64le(Expected.data() + 128, 733);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      rejected(run("memory_search",
                   {Pointer, 0, 1, UINT64_MAX, Checked, 0, Buffer + 128}),
               Name, "invalid guest pointer");
    }
    for (const auto C : {std::array<uint64_t, 3>{'x', UINT64_MAX, PageEnd - 1},
                         {'z', 1, 0},
                         {0, 1, 0}}) {
      auto R = run("memory_search_readonly",
                   {Buffer, PageEnd - 1, C[0], C[1], UINT64_MAX, Checked});
      returned(R, C[2]);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
    }
    rejected(run("memory_search_readonly",
                 {Buffer, PageEnd - 1, 'z', 2, UINT64_MAX, Checked}),
             Name, "invalid guest pointer");
  }
  // The whole declared count must fit, even if byte zero already matches.
  for (uint64_t Count : {uint64_t(2), uint64_t(0x100000001), UINT64_MAX})
    rejected(
        run("memory_search", {PageEnd - 1, 'x', Count, 1, 1, 0, Buffer + 128}),
        "__memchr_chk", "FORTIFY read exceeds object bounds");
  rejected(run("memory_search", {UINT64_MAX, 0, 1, 0, 1, 0, Buffer + 128}),
           "__memchr_chk", "FORTIFY read exceeds object bounds");
}

TEST_P(AndroidSearch, MemorySearchClosedProviderPrecedesZeroCount) {
  Options.Android->Libraries["libmemory-search.so"] = {"memchr",
                                                       "__memchr_chk"};
  for (uint64_t Checked : {0u, 1u}) {
    auto R =
        run("memory_search", {UINT64_MAX, 0, 0, 0, Checked, 2, Buffer + 128});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Checked ? "__memchr_chk" : "memchr");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_FALSE(R.ReturnValue);
  }
}

TEST_P(AndroidSearch, FirstLastNulUnsignedCharactersAndFullWidthBounds) {
  auto R = run("search_sequence", {Buffer});
  returned(R, 0);
  const std::array<uint64_t, 16> Expected = {2, 4, 2, 4, 1, 5, 6,   6,
                                             0, 0, 0, 0, 2, 4, 733, 1};
  ASSERT_EQ(R.MemorySnapshots.size(), 2u);
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(llvm::support::endian::read64le(
                  R.MemorySnapshots[0].Bytes.data() + 8 * I),
              Expected[I]);
  EXPECT_TRUE(R.Services.empty());
}

TEST_P(AndroidSearch, PageTailMatchDoesNotReadBeyondTheObject) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  for (auto Character : {uint8_t(':'), uint8_t(0), uint8_t(0xff)}) {
    SCOPED_TRACE(unsigned(Character));
    Bytes.assign(4096, 'x');
    Bytes.back() = Character;
    for (unsigned Mode : {0u, 2u}) {
      SCOPED_TRACE(Mode);
      auto R = run(
          "search_readonly",
          {Buffer, PageEnd - 1, 0xabcdef0100ULL | Character, UINT64_MAX, Mode});
      returned(R, PageEnd - 1);
      ASSERT_EQ(R.MemorySnapshots.size(), 2u);
      EXPECT_EQ(R.MemorySnapshots[1].Bytes,
                std::vector<uint8_t>(Bytes.end() - 16, Bytes.end()));
    }
  }
  for (unsigned Mode : {1u, 3u}) {
    auto R = run("search_supplied", {PageEnd - 1, 0, 1, Mode});
    // Reverse search cannot use a non-NUL match as a substitute for
    // termination.
    rejected(R, SearchNames[Mode],
             Mode == 3 ? "FORTIFY" : "invalid guest pointer");
  }
}

TEST_P(AndroidSearch, TerminatorAtTheLastByteIsIncludedInTheSearch) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 'x');
  Bytes[4092] = ':';
  Bytes[4094] = ':';
  Bytes[4095] = 0;
  for (unsigned Mode = 0; Mode < SearchNames.size(); ++Mode) {
    for (auto [Character, Expected] :
         {std::pair{uint64_t(':'), Mode % 2 ? PageEnd - 2 : PageEnd - 4},
          {0x100, PageEnd - 1},
          {'z', 0}}) {
      SCOPED_TRACE(SearchNames[Mode]);
      auto R =
          run("search_readonly", {Buffer, PageEnd - 4, Character, 4, Mode});
      returned(R, Expected);
      ASSERT_EQ(R.MemorySnapshots.size(), 2u);
      EXPECT_EQ(R.MemorySnapshots[1].Bytes,
                std::vector<uint8_t>(Bytes.end() - 16, Bytes.end()));
    }
  }
}

TEST_P(AndroidSearch, FortifyChecksExtentBeforePointerOrNextCharacter) {
  Options.Android->Memory[0].Bytes.assign(4096, 'x');
  for (unsigned Mode : {2u, 3u}) {
    for (uint64_t Address : {uint64_t(0), uint64_t(1), UINT64_MAX, PageEnd - 1})
      rejected(run("search_supplied", {Address, 'x', 0, Mode}),
               SearchNames[Mode], "FORTIFY");
    // The inaccessible next byte must never be read after exhausting the bound.
    rejected(run("search_supplied", {PageEnd - 1, 'z', 1, Mode}),
             SearchNames[Mode], "FORTIFY");
    // Reading a still-in-bounds inaccessible byte remains a pointer failure.
    rejected(run("search_supplied", {PageEnd - 1, 'z', 2, Mode}),
             SearchNames[Mode], "invalid guest pointer");
  }
  rejected(run("search_supplied", {PageEnd - 1, 'x', 1, 3}), "__strrchr_chk",
           "FORTIFY");
  returned(run("search_supplied", {PageEnd - 1, 'x', 1, 2}), PageEnd - 1);
}

TEST_P(AndroidSearch, BoundsExcludeAnOtherwiseAccessibleTerminator) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes = {'x', ':', 'x', 0};
  returned(run("search_supplied", {Buffer, ':', 2, 2}), Buffer + 1);
  for (unsigned Mode : {2u, 3u}) {
    rejected(run("search_supplied", {Buffer, 0, 3, Mode}), SearchNames[Mode],
             "FORTIFY");
    rejected(run("search_supplied", {Buffer, 'z', 3, Mode}), SearchNames[Mode],
             "FORTIFY");
    returned(run("search_supplied", {Buffer, 0, 4, Mode}), Buffer + 3);
    returned(run("search_supplied", {Buffer, 'z', 4, Mode}), 0);
  }
}

TEST_P(AndroidSearch, NamedCallsRetainProviderAndRejectClosedLibrary) {
  Options.Android->Libraries = {
      {"libsearch.so", {"__strchr_chk", "__strrchr_chk"}}};
  for (unsigned Reverse : {0u, 1u}) {
    const char *Name = SearchNames[Reverse + 2];
    auto R = run("search_dynamic", {Buffer, 0, Reverse});
    returned(R, 0);
    uint64_t Target = 0;
    unsigned Count = 0;
    for (const auto &Call : R.NativeCalls) {
      if (Call.Name == "dlsym") {
        EXPECT_EQ(Call.Library, "libsearch.so");
        EXPECT_EQ(Call.Symbol, Name);
        ASSERT_TRUE(Call.Result);
        Target = *Call.Result;
      }
      if (Call.Name == Name) {
        EXPECT_EQ(Call.Library, "libsearch.so");
        EXPECT_EQ(Call.PC, Target);
        EXPECT_TRUE(Call.Result);
        ++Count;
      }
    }
    EXPECT_NE(Target, 0u);
    EXPECT_EQ(Count, 2u);
    ASSERT_EQ(R.MemorySnapshots.size(), 2u);
    const std::array<uint64_t, 3> Expected = {Reverse ? 10u : 4u, 15, 733};
    for (size_t I = 0; I < Expected.size(); ++I)
      EXPECT_EQ(llvm::support::endian::read64le(
                    R.MemorySnapshots[0].Bytes.data() + 8 * I),
                Expected[I]);
    R = run("search_dynamic", {Buffer, 1, Reverse});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Name);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}

TEST_P(AndroidSearch, DecimalIntegersPreserveWidthsPrefixesAndErrno) {
  struct Case {
    const char *Text;
    int64_t Expected;
  };
  const Case Cases[] = {{"", 0},
                        {" \t\n\r\v\f-123tail", -123},
                        {"+007", 7},
                        {"-0", 0},
                        {"-+12", 0},
                        {" +", 0},
                        {"0x10", 0},
                        {"012", 12},
                        {"7.5", 7},
                        {"\xff"
                         "123",
                         0},
                        {"2147483647", INT32_MAX},
                        {"-2147483648", INT32_MIN},
                        {"9223372036854775807", INT64_MAX},
                        {"-9223372036854775808", INT64_MIN}};
  for (const auto &C : Cases) {
    auto &Bytes = Options.Android->Memory[0].Bytes;
    llvm::StringRef Text(C.Text);
    Bytes.assign(Text.begin(), Text.end());
    Bytes.push_back(0);
    for (uint64_t Mode = 0; Mode != 3; ++Mode) {
      SCOPED_TRACE(testing::Message() << Mode << ':' << C.Text);
      if (Mode == 0 && (C.Expected > INT32_MAX || C.Expected < INT32_MIN))
        continue;
      auto R = run("decimal_supplied", {Buffer, Mode, Buffer + 64});
      returned(R, uint64_t(C.Expected));
      ASSERT_FALSE(R.MemorySnapshots.empty());
      EXPECT_EQ(llvm::support::endian::read32le(
                    R.MemorySnapshots[0].Bytes.data() + 64),
                733u);
    }
  }
  Options.Android->Libraries["libsearch.so"] = {"atoi"};
  Options.Android->Memory[0].Bytes = {'-', '3', 0};
  auto R = run("decimal_dynamic", {Buffer, 0});
  returned(R, uint64_t(-3));
  EXPECT_EQ(R.NativeCalls.back().Library, "libsearch.so");
  auto Closed = run("decimal_dynamic", {Buffer, 1});
  EXPECT_EQ(Closed.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_FALSE(Closed.NativeCalls.back().Result);
}
TEST_P(AndroidSearch,
       DecimalConversionStopsAtTheFirstNondigitAndRejectsOverflow) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  for (uint64_t Mode = 0; Mode != 3; ++Mode) {
    Bytes.assign(4096, 0xa5);
    Bytes[4094] = '3';
    Bytes[4095] = 'x';
    returned(run("decimal_supplied", {PageEnd - 2, Mode, Buffer}), 3);
    Bytes[4095] = 0;
    returned(run("decimal_supplied", {PageEnd - 2, Mode, Buffer}), 3);
    Bytes[4095] = '4';
    rejected(run("decimal_supplied", {PageEnd - 2, Mode, Buffer}),
             Mode == 0   ? "atoi"
             : Mode == 1 ? "atol"
                         : "atoll",
             "invalid guest pointer");
    for (const char *Text :
         {Mode == 0 ? "2147483648" : "9223372036854775808",
          Mode == 0 ? "-2147483649" : "-9223372036854775809"}) {
      llvm::StringRef Input(Text);
      Bytes.assign(Input.begin(), Input.end());
      Bytes.push_back(0);
      auto R = run("decimal_supplied", {Buffer, Mode, Buffer + 64});
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_NE(R.Diagnostic.find("out-of-range decimal"), std::string::npos);
      EXPECT_FALSE(R.NativeCalls.back().Result);
      ASSERT_FALSE(R.MemorySnapshots.empty());
      EXPECT_EQ(llvm::support::endian::read32le(
                    R.MemorySnapshots[0].Bytes.data() + 64),
                0u);
    }
  }
}

TEST_P(AndroidSearch, BinarySubstringCountsBytesErrnoAndProvider) {
  struct Case {
    std::vector<uint8_t> Text, Needle;
    int Offset;
  };
  const Case Cases[] = {
      {{}, {}, 0},
      {{'a'}, {}, 0},
      {{}, {'a'}, -1},
      {{0x80, 0, 0xff, 0, 0xff}, {0, 0xff}, 1},
      {{'a', 'b', 'a', 'b', 'a', 'b', 'a', 'c'}, {'a', 'b', 'a', 'c'}, 4},
      {{'a', 'a', 'a', 'b'}, {'a', 'a', 'b'}, 1},
      {{0xff, 0x80}, {0x80}, 1},
      {{0, 'b'}, {'b', 0}, -1},
      {{'a', 'b'}, {'a', 'b', 'c'}, -1},
      {{'a', 'b'}, {'a', 'b'}, 0},
      {{'a', 'b', 'c'}, {'x'}, -1}};
  Options.Android->Libraries["libsearch.so"] = {"memmem"};
  Options.Android->ReadMemory = {{Buffer, 4096}};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  for (const auto &C : Cases) {
    for (uint64_t Route : {0u, 1u}) {
      SCOPED_TRACE(testing::Message() << C.Offset << ":" << C.Text.size() << ":"
                                      << C.Needle.size() << ":" << Route);
      Bytes.assign(4096, 0xa5);
      std::copy(C.Text.begin(), C.Text.end(), Bytes.begin());
      std::copy(C.Needle.begin(), C.Needle.end(), Bytes.begin() + 64);
      auto Expected = Bytes;
      llvm::support::endian::write64le(Expected.data() + 128, 733);
      const uint64_t Address = C.Offset < 0 ? 0 : Buffer + C.Offset;
      auto R = run("binary_substring", {Buffer, C.Text.size(), Buffer + 64,
                                        C.Needle.size(), Route, Buffer + 128});
      returned(R, Address);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      unsigned Matches = 0;
      uint64_t Target = 0;
      for (const auto &Call : R.NativeCalls) {
        if (Call.Name == "dlsym") {
          EXPECT_EQ(Call.Symbol, "memmem");
          ASSERT_TRUE(Call.Result);
          Target = *Call.Result;
        }
        if (Call.Name != "memmem")
          continue;
        EXPECT_EQ(Call.Arguments[1], C.Text.size());
        EXPECT_EQ(Call.Arguments[3], C.Needle.size());
        EXPECT_EQ(Call.Result, Address);
        EXPECT_EQ(Call.Library, Route ? "libsearch.so" : "");
        if (Route)
          EXPECT_EQ(Call.PC, Target);
        ++Matches;
      }
      EXPECT_EQ(Matches, 1u);
    }
  }
}

TEST_P(AndroidSearch, BinarySubstringBoundsAndReadOnlyPageTail) {
  Options.Android->ReadMemory = {{Buffer, 4096}};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 0xa5);
  Bytes[0] = Bytes[4093] = 'x';
  Bytes[1] = Bytes[4094] = 0;
  Bytes[2] = Bytes[4095] = 'y';
  for (const auto C : {std::array<uint64_t, 4>{3, Buffer, 3, PageEnd - 3},
                       {2, Buffer, 3, 0},
                       {2, Buffer + 1, 1, PageEnd - 2},
                       {3, Buffer + 3, 1, 0},
                       {UINT64_MAX, Buffer, 3, PageEnd - 3},
                       {0x100000001, Buffer, 3, PageEnd - 3},
                       {3, PageEnd - 2, 2, PageEnd - 2}}) {
    auto R = run("binary_substring_readonly",
                 {Buffer, PageEnd - 3, C[0], C[1], C[2]});
    returned(R, C[3]);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
  }
  rejected(
      run("binary_substring_readonly", {Buffer, PageEnd - 3, 4, Buffer + 3, 1}),
      "memmem", "invalid guest pointer");
  rejected(
      run("binary_substring_readonly", {Buffer, Buffer, 3, PageEnd - 2, 3}),
      "memmem", "invalid guest pointer");
  for (uint64_t Pointer : {uint64_t(0), PageEnd, UINT64_MAX}) {
    returned(run("binary_substring",
                 {Pointer, UINT64_MAX, Pointer, 0, 0, Buffer + 128}),
             Pointer);
    returned(run("binary_substring", {Pointer, 1, Pointer, 2, 0, Buffer + 128}),
             0);
    rejected(run("binary_substring", {Pointer, 1, Buffer, 1, 0, Buffer + 128}),
             "memmem", "invalid guest pointer");
    rejected(run("binary_substring", {Buffer, 1, Pointer, 1, 0, Buffer + 128}),
             "memmem", "invalid guest pointer");
  }
}

TEST_P(AndroidSearch, BinarySubstringClosedProviderPrecedesEmptyNeedle) {
  Options.Android->Libraries["libsearch.so"] = {"memmem"};
  auto R = run("binary_substring",
               {UINT64_MAX, UINT64_MAX, UINT64_MAX, 0, 2, Buffer + 128});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "memmem");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  EXPECT_FALSE(R.ReturnValue);
}

TEST_P(AndroidSearch, SubstringUsesFirstByteMatchAndPreservesErrnoAndInputs) {
  struct Case {
    const char *Text, *Needle;
    int Offset;
  };
  const Case Cases[] = {{"", "", 0},
                        {"a", "", 0},
                        {"", "a", -1},
                        {"banana", "ana", 1},
                        {"aaaab", "aaab", 1},
                        {"ababababac", "ababac", 4},
                        {"abc", "abcd", -1},
                        {"a\\b", "\\b", 1},
                        {"abc", "bc", 1},
                        {"abc", "z", -1},
                        {"\x80\xff\x80\xff", "\x80\xff", 0}};
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Text);
    auto &Bytes = Options.Android->Memory[0].Bytes;
    Bytes.assign(4096, 0xa5);
    auto Put = [&](unsigned At, const char *Text) {
      llvm::StringRef S(Text);
      std::copy(S.begin(), S.end(), Bytes.begin() + At);
      Bytes[At + S.size()] = 0;
    };
    Put(0, C.Text);
    Put(64, C.Needle);
    const auto Before = Bytes;
    auto R = run("substring_supplied", {Buffer, Buffer + 64, Buffer + 124});
    returned(R, C.Offset < 0 ? 0 : Buffer + C.Offset);
    ASSERT_EQ(R.MemorySnapshots.size(), 2u);
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + 124),
              733u);
    EXPECT_TRUE(std::equal(Before.begin(), Before.begin() + 124,
                           R.MemorySnapshots[0].Bytes.begin()));
  }
}
TEST_P(AndroidSearch, SubstringStopsAtNulOrMatchBeforeAnUnmappedPage) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 0xa5);
  Bytes[0] = 'x';
  Bytes[1] = 'y';
  Bytes[2] = 0;
  Bytes[4094] = 'x';
  Bytes[4095] = 'y';
  returned(run("substring_supplied", {PageEnd - 2, Buffer, Buffer + 124}),
           PageEnd - 2);
  Bytes[4095] = 0;
  returned(run("substring_supplied", {PageEnd - 2, Buffer, Buffer + 124}), 0);
  // Empty needle does not inspect even an inaccessible haystack.
  returned(run("substring_supplied", {UINT64_MAX, PageEnd - 1, Buffer + 124}),
           UINT64_MAX);
  returned(run("substring_supplied", {Buffer, Buffer, Buffer + 124}), Buffer);
  Bytes[4095] = 'z';
  rejected(run("substring_supplied", {PageEnd - 2, Buffer, Buffer + 124}),
           "strstr", "invalid guest pointer");
  rejected(run("substring_supplied", {Buffer, PageEnd - 1, Buffer + 124}),
           "strstr", "invalid guest pointer");
}
TEST_P(AndroidSearch, SubstringDynamicProviderRetainsLifetime) {
  Options.Android->Memory[0].Bytes = {'a', 'b', 'c', 0, 'b', 0};
  Options.Android->Libraries["libsearch.so"] = {"strstr"};
  auto R = run("substring_dynamic", {Buffer, Buffer + 4, 0});
  returned(R, Buffer + 1);
  EXPECT_EQ(R.NativeCalls.back().Library, "libsearch.so");
  auto Closed = run("substring_dynamic", {Buffer, Buffer + 4, 1});
  EXPECT_EQ(Closed.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Closed.Diagnostic.find("inactive dynamic library"),
            std::string::npos);
  EXPECT_FALSE(Closed.NativeCalls.back().Result);
}

TEST_P(AndroidSearch, CaseInsensitiveSubstringUsesCLocaleBytesAndFirstMatch) {
  struct Case {
    const char *Text, *Needle;
    int Offset;
  };
  const Case Cases[] = {{"", "", 0},
                        {"a", "", 0},
                        {"", "A", -1},
                        {"BanAna", "ANA", 1},
                        {"AaAAaB", "aaAB", 2},
                        {"ababABaC", "ABac", 4},
                        {"Abc", "ABCD", -1},
                        {"abc", "Z", -1},
                        {"\x80"
                         "A\xff"
                         "a",
                         "a\xff", 1},
                        {"\xc0"
                         "A",
                         "\xe0"
                         "a",
                         -1}};
  Options.Android->Libraries["libsearch.so"] = {"strcasestr"};
  Options.Android->ReadMemory = {{Buffer, 4096}};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  for (const auto &C : Cases) {
    for (uint64_t Route : {0u, 1u}) {
      SCOPED_TRACE(testing::Message() << C.Offset << ":" << Route);
      Bytes.assign(4096, 0xa5);
      auto Put = [&](unsigned At, const char *Text) {
        llvm::StringRef S(Text);
        std::copy(S.begin(), S.end(), Bytes.begin() + At);
        Bytes[At + S.size()] = 0;
      };
      Put(0, C.Text);
      Put(64, C.Needle);
      auto Expected = Bytes;
      llvm::support::endian::write64le(Expected.data() + 128, 733);
      const uint64_t Address = C.Offset < 0 ? 0 : Buffer + C.Offset;
      auto R =
          run("case_substring", {Buffer, Buffer + 64, Route, Buffer + 128});
      returned(R, Address);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      uint64_t Target = 0;
      unsigned Matches = 0;
      for (const auto &Call : R.NativeCalls) {
        if (Call.Name == "dlsym") {
          EXPECT_EQ(Call.Symbol, "strcasestr");
          ASSERT_TRUE(Call.Result);
          Target = *Call.Result;
        }
        if (Call.Name != "strcasestr")
          continue;
        EXPECT_EQ(Call.Result, Address);
        EXPECT_EQ(Call.Library, Route ? "libsearch.so" : "");
        if (Route)
          EXPECT_EQ(Call.PC, Target);
        ++Matches;
      }
      EXPECT_EQ(Matches, 1u);
    }
  }
}

TEST_P(AndroidSearch, CaseInsensitiveSubstringStopsAtNulAndReadOnlyPageTail) {
  Options.Android->ReadMemory = {{Buffer, 4096}};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 0xa5);
  Bytes[0] = 'x';
  Bytes[1] = 'Y';
  Bytes[2] = 0;
  Bytes[4094] = 'X';
  Bytes[4095] = 'y';
  auto R = run("case_substring_readonly", {Buffer, PageEnd - 2, Buffer});
  returned(R, PageEnd - 2);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
  // Read-only overlap is permitted, and the first NUL ends the haystack.
  returned(run("case_substring_readonly", {Buffer, Buffer, Buffer}), Buffer);
  Bytes[4095] = 0;
  returned(run("case_substring_readonly", {Buffer, PageEnd - 2, Buffer}), 0);
  returned(run("case_substring_readonly", {Buffer, UINT64_MAX, PageEnd - 1}),
           UINT64_MAX);
  Bytes[4095] = 'z';
  rejected(run("case_substring_readonly", {Buffer, PageEnd - 2, Buffer}),
           "strcasestr", "invalid guest pointer");
  rejected(run("case_substring_readonly", {Buffer, Buffer, PageEnd - 1}),
           "strcasestr", "invalid guest pointer");
}

TEST_P(AndroidSearch,
       CaseInsensitiveSubstringClosedProviderPrecedesEmptyNeedle) {
  Options.Android->Libraries["libsearch.so"] = {"strcasestr"};
  auto R = run("case_substring", {UINT64_MAX, Buffer, 2, Buffer + 128});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "strcasestr");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  EXPECT_FALSE(R.ReturnValue);
}

TEST_P(AndroidSearch, MemberSearchReturnsFirstByteAndPreservesErrnoAndInputs) {
  struct Case {
    const char *Text, *Accept;
    int Offset;
  };
  const Case Cases[] = {{"", "", -1},
                        {"abc", "", -1},
                        {"", "abc", -1},
                        {"abca", "ca", 0},
                        {"baab", "a", 1},
                        {"abc", "z", -1},
                        {"a\x80\xff", "x\xff\x80\x80", 1},
                        {"x:y:z", ":", 1}};
  Options.Android->Libraries["libsearch.so"] = {"strpbrk"};
  Options.Android->ReadMemory = {{Buffer, 4096}};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  for (const auto &C : Cases) {
    for (uint64_t Route : {0u, 1u}) {
      SCOPED_TRACE(testing::Message() << C.Offset << ":" << Route);
      Bytes.assign(4096, 0xa5);
      for (auto [Offset, Text] :
           {std::pair{64, C.Text}, std::pair{128, C.Accept}}) {
        llvm::StringRef S(Text);
        std::copy(S.begin(), S.end(), Bytes.begin() + Offset);
        Bytes[Offset + S.size()] = 0;
      }
      auto Expected = Bytes;
      llvm::support::endian::write64le(Expected.data() + 256, 733);
      auto R = run("member_search",
                   {Buffer + 64, Buffer + 128, Route, Buffer + 256});
      returned(R, C.Offset < 0 ? 0 : Buffer + 64 + C.Offset);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      ASSERT_FALSE(R.NativeCalls.empty());
      unsigned Matches = 0;
      uint64_t Target = 0;
      for (const auto &Call : R.NativeCalls) {
        if (Call.Name == "dlsym") {
          EXPECT_EQ(Call.Symbol, "strpbrk");
          ASSERT_TRUE(Call.Result);
          Target = *Call.Result;
        }
        if (Call.Name != "strpbrk")
          continue;
        EXPECT_EQ(Call.Result, R.ReturnValue);
        EXPECT_EQ(Call.Library, Route ? "libsearch.so" : "");
        if (Route)
          EXPECT_EQ(Call.PC, Target);
        ++Matches;
      }
      EXPECT_EQ(Matches, 1u);
    }
  }
}

TEST_P(AndroidSearch, MemberSearchReadOnlyTailsNulOverlapAndFaults) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 0xa5);
  Bytes[0] = ':';
  Bytes[1] = 0;
  Bytes[4093] = 'x';
  Bytes[4094] = 'y';
  Bytes[4095] = ':';
  Options.Android->ReadMemory = {{Buffer, 4096}};
  for (uint8_t Last : {uint8_t(':'), uint8_t(0)}) {
    Bytes[4095] = Last;
    auto R = run("member_search_readonly", {Buffer, PageEnd - 3, Buffer});
    returned(R, Last ? PageEnd - 1 : 0);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
  }
  // NUL is not a set member, and bytes after it are never searched.
  Bytes[4093] = 0;
  Bytes[4095] = ':';
  returned(run("member_search_readonly", {Buffer, PageEnd - 3, Buffer}), 0);
  // The accepted string may overlap the source without any writes.
  Bytes[64] = 'x';
  Bytes[65] = 'y';
  Bytes[66] = 0;
  returned(run("member_search_readonly", {Buffer, Buffer + 64, Buffer + 65}),
           Buffer + 65);
  Bytes[4093] = 'x';
  Bytes[4095] = 'z';
  rejected(run("member_search_readonly", {Buffer, PageEnd - 3, Buffer}),
           "strpbrk", "invalid guest pointer");
  rejected(run("member_search_readonly", {Buffer, Buffer, PageEnd - 1}),
           "strpbrk", "invalid guest pointer");
  for (uint64_t Invalid : {uint64_t(0), PageEnd, UINT64_MAX}) {
    rejected(run("member_search", {Invalid, Buffer, 0, Buffer + 256}),
             "strpbrk", "invalid guest pointer");
    rejected(run("member_search", {Buffer, Invalid, 0, Buffer + 256}),
             "strpbrk", "invalid guest pointer");
  }
}

TEST_P(AndroidSearch, MemberSearchClosedProviderPrecedesGuestAccess) {
  Options.Android->Libraries["libsearch.so"] = {"strpbrk"};
  auto R = run("member_search", {UINT64_MAX, UINT64_MAX, 2, Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "strpbrk");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidSearch, SpanCountsBytePrefixesAndPreservesErrnoAndInputs) {
  struct Case {
    const char *Text, *Accept;
    uint64_t Count, Complement;
  };
  const Case Cases[] = {{"", "", 0, 0},
                        {"abc", "", 0, 3},
                        {"", "abc", 0, 0},
                        {"cabba!abc", "abc", 5, 0},
                        {"abca", "aaabbb", 2, 0},
                        {"abc", "cba", 3, 0},
                        {"zabc", "abc", 0, 1},
                        {"abc", "abcdef", 3, 0},
                        {" \t\r\nx", "\n\r\t ", 4, 0},
                        {"\xff\x80\x7f!", "\x7f\xff\x80", 3, 0},
                        {"\xff\x80", "\xff", 1, 0},
                        {"ab\0c", "abc", 2, 0},
                        {"abc", "ab\0c", 2, 0},
                        {"aaaab", "bb", 0, 4},
                        {"\xff\xff\x80", "\x80", 0, 2}};
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Text);
    auto &Bytes = Options.Android->Memory[0].Bytes;
    Bytes.assign(4096, 0xa5);
    auto Put = [&](unsigned At, const char *Text) {
      llvm::StringRef S(Text);
      std::copy(S.begin(), S.end(), Bytes.begin() + At);
      Bytes[At + S.size()] = 0;
    };
    Put(0, C.Text);
    Put(64, C.Accept);
    const auto Before = Bytes;
    for (bool Complement : {false, true}) {
      SCOPED_TRACE(Complement);
      auto R = run(Complement ? "reject_span_supplied" : "span_supplied",
                   {Buffer, Buffer + 64, Buffer + 124});
      returned(R, Complement ? C.Complement : C.Count);
      ASSERT_EQ(R.MemorySnapshots.size(), 2u);
      EXPECT_EQ(llvm::support::endian::read32le(
                    R.MemorySnapshots[0].Bytes.data() + 124),
                733u);
      EXPECT_TRUE(std::equal(Before.begin(), Before.begin() + 124,
                             R.MemorySnapshots[0].Bytes.begin()));
      EXPECT_EQ(R.MemorySnapshots[1].Bytes,
                std::vector<uint8_t>(Before.end() - 16, Before.end()));
      EXPECT_TRUE(R.Services.empty());
    }
  }
}

TEST_P(AndroidSearch, SpanUsesAllUnsignedBytesAndReadOnlyAliasedStrings) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 0xa5);
  // Independently constructed permutations contain every nonzero byte.
  for (unsigned I = 0; I < 255; ++I) {
    Bytes[I] = uint8_t(I + 1);
    Bytes[512 + I] = uint8_t(255 - I);
  }
  Bytes[255] = Bytes[767] = 0;
  Options.Android->ReadMemory = {{Buffer, 4096}};
  for (uint64_t Accept : {Buffer, Buffer + 512}) {
    for (bool Complement : {false, true}) {
      auto R = run(Complement ? "reject_span_readonly" : "span_readonly",
                   {Buffer, Buffer, Accept});
      returned(R, Complement ? 0 : 255);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
    }
  }
  auto Empty = run("reject_span_readonly", {Buffer, Buffer, Buffer + 255});
  returned(Empty, 255);
  ASSERT_EQ(Empty.MemorySnapshots.size(), 1u);
  EXPECT_EQ(Empty.MemorySnapshots[0].Bytes, Bytes);
  const char Text[] = "abcabc";
  std::copy(std::begin(Text), std::end(Text), Bytes.begin());
  auto R = run("span_readonly", {Buffer, Buffer, Buffer + 3});
  returned(R, 6);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
  R = run("reject_span_readonly", {Buffer, Buffer, Buffer + 3});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
}

TEST_P(AndroidSearch, SpanStopsAtRejectOrNulAndRequiresReadableBoundedInputs) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 0xa5);
  Bytes[0] = 'a';
  Bytes[1] = 0;
  Bytes[4093] = Bytes[4094] = 'a';
  for (uint8_t Last : {uint8_t('b'), uint8_t(0)}) {
    Bytes[4095] = Last;
    returned(run("span_readonly", {Buffer, PageEnd - 3, Buffer}), 2);
  }
  // A set ending at the final readable byte is also sufficient.
  returned(run("span_readonly", {Buffer, Buffer, PageEnd - 2}), 1);
  // Empty sets still need the first source byte; they never read past it.
  returned(run("span_readonly", {Buffer, PageEnd - 2, Buffer + 1}), 0);
  for (uint64_t Invalid : {uint64_t(0), PageEnd, UINT64_MAX}) {
    rejected(run("span_supplied", {Invalid, Buffer + 1, Buffer + 124}),
             "strspn", "invalid guest pointer");
    rejected(run("span_supplied", {Buffer, Invalid, Buffer + 124}), "strspn",
             "invalid guest pointer");
  }
  Bytes[4095] = 'a';
  rejected(run("span_supplied", {PageEnd - 3, Buffer, Buffer + 124}), "strspn",
           "invalid guest pointer");
  rejected(run("span_supplied", {Buffer, PageEnd - 1, Buffer + 124}), "strspn",
           "invalid guest pointer");
}

TEST_P(AndroidSearch, ComplementSpanStopsAtMemberOrNulAndChecksBothInputs) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 0xa5);
  Bytes[0] = 'b';
  Bytes[1] = 0;
  Bytes[4093] = Bytes[4094] = 'a';
  for (uint8_t Last : {uint8_t('b'), uint8_t(0)}) {
    Bytes[4095] = Last;
    returned(run("reject_span_readonly", {Buffer, PageEnd - 3, Buffer}), 2);
  }
  // An empty set spans the entire terminated source, not a zero-length prefix.
  returned(run("reject_span_readonly", {Buffer, PageEnd - 3, Buffer + 1}), 2);
  // Reject-set termination at the page tail and partial input overlap.
  returned(run("reject_span_readonly", {Buffer, Buffer, PageEnd - 2}), 1);
  Bytes[4095] = 'a';
  rejected(run("reject_span_supplied", {PageEnd - 3, Buffer, Buffer + 124}),
           "strcspn", "invalid guest pointer");
  rejected(run("reject_span_supplied", {Buffer, PageEnd - 1, Buffer + 124}),
           "strcspn", "invalid guest pointer");
  for (uint64_t Invalid : {uint64_t(0), PageEnd, UINT64_MAX}) {
    rejected(run("reject_span_supplied", {Invalid, Buffer + 1, Buffer + 124}),
             "strcspn", "invalid guest pointer");
    rejected(run("reject_span_supplied", {Buffer, Invalid, Buffer + 124}),
             "strcspn", "invalid guest pointer");
  }
  const char Text[] = "xxabc";
  std::copy(std::begin(Text), std::end(Text), Bytes.begin());
  Options.Android->ReadMemory = {{Buffer, 4096}};
  auto R = run("reject_span_readonly", {Buffer, Buffer, Buffer + 2});
  returned(R, 2);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
}

TEST_P(AndroidSearch, SpanDynamicProviderRetainsLifetime) {
  Options.Android->Memory[0].Bytes = {'b', 'a', '!', 0, 'a', 'b', 0};
  for (bool Complement : {false, true}) {
    SCOPED_TRACE(Complement);
    const char *Name = Complement ? "strcspn" : "strspn";
    const char *Entry = Complement ? "reject_span_dynamic" : "span_dynamic";
    Options.Android->Libraries["libsearch.so"] = {Name};
    auto R = run(Entry, {Buffer, Buffer + 4, 0});
    returned(R, Complement ? 0 : 2);
    uint64_t Target = 0;
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "dlsym") {
        EXPECT_EQ(Call.Library, "libsearch.so");
        EXPECT_EQ(Call.Symbol, Name);
        ASSERT_TRUE(Call.Result);
        Target = *Call.Result;
      }
    ASSERT_NE(Target, 0u);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Name);
    EXPECT_EQ(R.NativeCalls.back().PC, Target);
    EXPECT_EQ(R.NativeCalls.back().Library, "libsearch.so");
    auto Closed = run(Entry, {Buffer, Buffer + 4, 1});
    EXPECT_EQ(Closed.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Closed.Diagnostic.find("inactive dynamic library"),
              std::string::npos);
    EXPECT_FALSE(Closed.NativeCalls.back().Result);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Compiled, AndroidSearch,
    testing::Combine(testing::Values("search-O0-none", "search-O0-android",
                                     "search-O0-relr", "search-O2-none",
                                     "search-O2-android", "search-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));

TEST(AndroidSearchMemory, MemorySearchBudgetsAndCompleteBacking) {
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
  std::vector<uint8_t> Before(4096, 'x');
  Before[8] = 0;
  llvm::cantFail(CPU.write(Buffer, Before));
  llvm::cantFail(Space.protect(Buffer, 4096, Read | UserAccessible));
  ProcessOptions Options;
  Options.Android.emplace();
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, PageEnd, Options, Result);
  const android_model::LinkedImage Linked{};
  struct Case {
    uint64_t Limit, Pointer, Character, Count, Extent;
    bool Expired;
    const char *Error;
    uint64_t Expected;
  };
  for (bool Checked : {false, true}) {
    for (const auto C :
         {Case{8, Buffer, 0, 8, 8, false, nullptr, 0},
          {8, Buffer, 0, 9, 9, false, "native memory operation exceeds", 0},
          {9, Buffer, 0, 9, 9, false, nullptr, Buffer + 8},
          {0, UINT64_MAX, 0, 0, 0, false, nullptr, 0},
          {0, Buffer, 'x', 1, 1, false, "native memory operation exceeds", 0},
          {8, Buffer, 0x12345678, UINT64_MAX, UINT64_MAX, false, nullptr,
           Buffer},
          {8, PageEnd, 0, 1, 1, false, "invalid guest pointer", 0},
          {8, PageEnd, 0, 1, 1, true, "timed out", 0},
          {8, UINT64_MAX, 0, 0, 0, true, nullptr, 0},
          {8, Buffer, 'x', 9, 8, false, Checked ? "FORTIFY" : nullptr, Buffer},
          {8, UINT64_MAX, 0, 1, 0, true, Checked ? "FORTIFY" : "timed out",
           0}}) {
      SCOPED_TRACE(testing::Message()
                   << Checked << ":" << C.Limit << ":" << C.Count << ":"
                   << C.Extent << ":" << C.Expired);
      Options.MemoryLimit = C.Limit;
      auto Budget = llvm::cantFail(ExecutionBudget::create(
          Options.Limits, C.Expired ? ExecutionBudget::Clock::time_point::min()
                                    : ExecutionBudget::Clock::now()));
      android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                  Linked);
      NativeCallEvent Call{};
      Call.Name = Checked ? "__memchr_chk" : "memchr";
      Call.Arguments = {C.Pointer, C.Character, C.Count, C.Extent};
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
      EXPECT_EQ(Model.timedOut(),
                C.Expired && C.Count && !(Checked && C.Count > C.Extent));
      std::vector<uint8_t> After(4096);
      llvm::cantFail(Space.snapshotBacking(Buffer, After));
      EXPECT_EQ(After, Before);
    }
  }
}

TEST(AndroidSearchMemory, BinarySubstringBudgetsAndCompleteBacking) {
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
  std::vector<uint8_t> Before(4096, 'x');
  Before[8] = Before[64] = 0;
  Before[65] = 'y';
  llvm::cantFail(CPU.write(Buffer, Before));
  llvm::cantFail(Space.protect(Buffer, 4096, Read | UserAccessible));
  ProcessOptions Options;
  Options.Android.emplace();
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, PageEnd, Options, Result);
  const android_model::LinkedImage Linked{};
  struct Case {
    uint64_t Limit, Text, Count, Needle, Length;
    bool Expired;
    const char *Error;
    uint64_t Expected;
  };
  for (const auto C :
       {Case{8, Buffer, 8, Buffer + 64, 1, false, nullptr, 0},
        {8, Buffer, 9, Buffer + 64, 1, false, "scan exceeds memory limit", 0},
        {9, Buffer, 9, Buffer + 64, 1, false, nullptr, Buffer + 8},
        {16, Buffer, 10, Buffer + 64, 2, false, nullptr, 0},
        {8, Buffer, UINT64_MAX, Buffer, 1, false, nullptr, Buffer},
        {8, Buffer, 2, Buffer, 2, false, "scan exceeds memory limit", 0},
        {16, Buffer, 2, Buffer, 2, false, nullptr, Buffer},
        {0, UINT64_MAX, UINT64_MAX, UINT64_MAX, 0, false, nullptr, UINT64_MAX},
        {0, UINT64_MAX, UINT64_MAX, UINT64_MAX, 0, true, nullptr, UINT64_MAX},
        {0, UINT64_MAX, 1, UINT64_MAX, 2, true, nullptr, 0},
        {8, Buffer, UINT64_MAX, Buffer, 0x100000001, false,
         "scan exceeds memory limit", 0},
        {0, Buffer, 1, Buffer, 1, false, "scan exceeds memory limit", 0},
        {8, PageEnd, 1, Buffer, 1, false, "invalid guest pointer", 0},
        {8, Buffer, 1, PageEnd, 1, false, "invalid guest pointer", 0},
        {16, Buffer, 2, PageEnd - 1, 2, false, "invalid guest pointer", 0},
        {8, PageEnd, 1, Buffer, 1, true, "timed out", 0},
        {8, Buffer, 1, PageEnd, 1, true, "timed out", 0}}) {
    SCOPED_TRACE(testing::Message() << C.Limit << ":" << C.Count << ":"
                                    << C.Length << ":" << C.Expired);
    Options.MemoryLimit = C.Limit;
    auto Budget = llvm::cantFail(ExecutionBudget::create(
        Options.Limits, C.Expired ? ExecutionBudget::Clock::time_point::min()
                                  : ExecutionBudget::Clock::now()));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = "memmem";
    Call.Arguments = {C.Text, C.Count, C.Needle, C.Length};
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
    EXPECT_EQ(Model.timedOut(), C.Expired && C.Length && C.Count >= C.Length);
    std::vector<uint8_t> After(4096);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
}

TEST(AndroidSearchMemory, ObjectBoundsBudgetsAndFailuresPreserveGuestBytes) {
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
      llvm::toString(Space.map(Buffer, 4096, Read | Write | UserAccessible)),
      "");
  std::array<uint8_t, 16> Before{};
  Before.fill('x');
  Before[8] = 0;
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, Before)), "");
  ASSERT_EQ(llvm::toString(Space.protect(Buffer, 4096, Read | UserAccessible)),
            "");
  ProcessOptions Options;
  Options.Android.emplace();
  Options.MemoryLimit = 8;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, PageEnd, Options, Result);
  const android_model::LinkedImage Linked{};
  struct Case {
    unsigned Mode;
    uint64_t Character, Bound;
    const char *Error;
  };
  for (auto C : {Case{2, 'z', 0, "FORTIFY"},
                 {2, 'z', 8, "FORTIFY"},
                 {3, 'x', 8, "FORTIFY"},
                 {2, 'z', 9, "scan exceeds memory limit"},
                 {3, 'x', UINT64_MAX, "scan exceeds memory limit"},
                 {0, 'z', 0, "scan exceeds memory limit"},
                 {1, 'x', 0, "scan exceeds memory limit"},
                 {2, 0xfedcba9876543278, UINT64_MAX, nullptr}}) {
    SCOPED_TRACE(C.Mode);
    SCOPED_TRACE(C.Bound);
    auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = SearchNames[C.Mode];
    Call.Arguments = {Buffer, C.Character, C.Bound};
    auto Value = Model.invoke(Call);
    if (C.Error) {
      ASSERT_FALSE(bool(Value));
      EXPECT_NE(llvm::toString(Value.takeError()).find(C.Error),
                std::string::npos);
    } else {
      ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
      ASSERT_TRUE(*Value);
      EXPECT_EQ(std::get<uint64_t>(**Value), Buffer);
    }
    EXPECT_FALSE(Model.timedOut());
    std::array<uint8_t, 16> After{};
    ASSERT_EQ(llvm::toString(Space.snapshotBacking(Buffer, After)), "");
    EXPECT_EQ(After, Before);
  }
  // The model budget bounds both scans and the prefix table. Failures are
  // terminal, never a guessed no-match, and do not modify either string.
  for (const char *Name : {"strstr", "strcasestr"}) {
    SCOPED_TRACE(Name);
    for (unsigned Limit : {8u, 16u, 64u}) {
      Options.MemoryLimit = Limit;
      auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
      android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                  Linked);
      NativeCallEvent Call{};
      Call.Name = Name;
      Call.Arguments = {Buffer, Buffer};
      auto Value = Model.invoke(Call);
      if (Limit < 64) {
        EXPECT_FALSE(bool(Value));
        llvm::consumeError(Value.takeError());
      } else {
        ASSERT_TRUE(bool(Value));
        ASSERT_TRUE(*Value);
        EXPECT_EQ(std::get<uint64_t>(**Value), Buffer);
      }
      std::array<uint8_t, 16> After{};
      llvm::cantFail(Space.snapshotBacking(Buffer, After));
      EXPECT_EQ(After, Before);
    }
    auto Budget = llvm::cantFail(ExecutionBudget::create(
        {1, 1, 1}, ExecutionBudget::Clock::time_point::min()));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = Name;
    Call.Arguments = {Buffer, Buffer, 0};
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(
        llvm::toString(Value.takeError()).find("native call model timed out"),
        std::string::npos);
    EXPECT_TRUE(Model.timedOut());
    std::array<uint8_t, 16> After{};
    ASSERT_EQ(llvm::toString(Space.snapshotBacking(Buffer, After)), "");
    EXPECT_EQ(After, Before);
  }
}
TEST(AndroidSearchMemory, SpanScanLimitsAndTimeoutDoNotPublishAResultOrWrites) {
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
  std::array<uint8_t, 16> Before{};
  Before.fill('x');
  Before[8] = 0;
  llvm::cantFail(CPU.write(Buffer, Before));
  llvm::cantFail(Space.protect(Buffer, 4096, Read | UserAccessible));
  ProcessOptions Options;
  Options.Android.emplace();
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, PageEnd, Options, Result);
  const android_model::LinkedImage Linked{};
  struct Case {
    uint64_t Limit, Text, Accept;
    bool Expired;
    const char *Error;
    uint64_t Expected;
    const char *Name = "strspn";
  };
  for (const auto C :
       {Case{8, Buffer, Buffer, false, "scan exceeds", 0},
        {8, Buffer, Buffer + 7, false, "scan exceeds", 0},
        {9, Buffer, Buffer + 7, false, nullptr, 8},
        {1, Buffer + 8, Buffer + 8, false, nullptr, 0},
        {0, Buffer, Buffer, false, "scan exceeds", 0},
        {9, Buffer, Buffer + 7, true, "timed out", 0},
        {8, Buffer, Buffer, false, "scan exceeds", 0, "strcspn"},
        {8, Buffer, Buffer + 8, false, "scan exceeds", 0, "strcspn"},
        {9, Buffer, Buffer + 8, false, nullptr, 8, "strcspn"},
        {2, Buffer, Buffer + 7, false, nullptr, 0, "strcspn"},
        {1, Buffer + 8, Buffer + 8, false, nullptr, 0, "strcspn"},
        {0, Buffer, Buffer, false, "scan exceeds", 0, "strcspn"},
        {9, Buffer, Buffer + 8, true, "timed out", 0, "strcspn"},
        {8, Buffer, Buffer, false, "scan exceeds", 0, "strpbrk"},
        {8, Buffer, Buffer + 8, false, "scan exceeds", 0, "strpbrk"},
        {9, Buffer, Buffer + 8, false, nullptr, 0, "strpbrk"},
        {2, Buffer, Buffer + 7, false, nullptr, Buffer, "strpbrk"},
        {1, Buffer + 8, Buffer + 8, false, nullptr, 0, "strpbrk"},
        {0, Buffer, Buffer, false, "scan exceeds", 0, "strpbrk"},
        {9, Buffer, Buffer + 8, true, "timed out", 0, "strpbrk"}}) {
    SCOPED_TRACE(C.Limit);
    SCOPED_TRACE(C.Accept);
    SCOPED_TRACE(C.Expired);
    SCOPED_TRACE(C.Name);
    Options.MemoryLimit = C.Limit;
    auto Budget = llvm::cantFail(ExecutionBudget::create(
        Options.Limits, C.Expired ? ExecutionBudget::Clock::time_point::min()
                                  : ExecutionBudget::Clock::now()));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = C.Name;
    Call.Arguments = {C.Text, C.Accept};
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
    std::array<uint8_t, 16> After{};
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
}
} // namespace
} // namespace neverd::emulation
