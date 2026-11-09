//===- AndroidLogTests.cpp - Explicit log records and reader lifetime ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent wire-byte oracle over compiled API callers and both backends.
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::emulation {
namespace {
constexpr uint64_t Output = 0x22000000;
constexpr uint64_t NoTime = UINT64_MAX;
std::vector<NativeLogRecord> records() {
  return {{0, 41, 51, 10, 0, 61, {1, 0, 2}},
          {1, 42, 52, 20, 7, 62, {3, 4}},
          {0, 42, 53, 20, 8, 63, {5, 6, 0, 7}},
          {1, 41, 54, 30, 0, 64, {8}}};
}
void integer(std::vector<uint8_t> &Out, size_t Offset, uint64_t Value,
             unsigned Width) {
  for (unsigned I = 0; I < Width; ++I)
    Out[Offset + I] = Value >> (8 * I);
}
std::vector<uint8_t> abortRecord(llvm::StringRef Message) {
  const uint64_t Size = 9 + Message.size();
  std::vector<uint8_t> Bytes((Size + 4095) & ~uint64_t(4095));
  integer(Bytes, 0, Size, 8);
  std::copy(Message.bytes_begin(), Message.bytes_end(), Bytes.begin() + 8);
  return Bytes;
}
uint64_t word(const std::vector<uint8_t> &Bytes, size_t Offset) {
  return llvm::support::endian::read64le(Bytes.data() + Offset);
}
void expected(std::vector<uint8_t> &Out, unsigned Slot,
              const NativeLogRecord *Record) {
  size_t Base = Slot * 8192;
  integer(Out, Base, Record ? 28 + Record->Payload.size() : uint64_t(-11), 8);
  integer(Out, Base + 8, 73, 8);
  std::fill_n(Out.begin() + Base + 16, 5124, 0);
  if (!Record)
    return;
  Base += 16;
  integer(Out, Base, Record->Payload.size(), 2);
  integer(Out, Base + 2, 28, 2);
  integer(Out, Base + 4, Record->PID, 4);
  integer(Out, Base + 8, Record->TID, 4);
  integer(Out, Base + 12, Record->Seconds, 4);
  integer(Out, Base + 16, Record->Nanoseconds, 4);
  integer(Out, Base + 20, Record->ID, 4);
  integer(Out, Base + 24, Record->UID, 4);
  std::copy(Record->Payload.begin(), Record->Payload.end(),
            Out.begin() + Base + 28);
}
constexpr const char *FortifySymbols[] = {
    "__memcpy_chk",    "__memmove_chk", "__memset_chk",   "__memchr_chk",
    "__FD_SET_chk",    "__FD_CLR_chk",  "__FD_ISSET_chk", "__strlen_chk",
    "__strcpy_chk",    "__strncpy_chk", "__strncpy_chk2", "__strlcpy_chk",
    "__strcat_chk",    "__strncat_chk", "__strchr_chk",   "__strrchr_chk",
    "__read_chk",      "__fgets_chk",   "__fread_chk",    "__snprintf_chk",
    "__vsnprintf_chk", "__sprintf_chk", "__vsprintf_chk"};
class AndroidLog : public testing::TestWithParam<
                       std::tuple<const char *, ExecutionBackendKind>> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android cross fixtures unavailable";
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
    Options.Android->EntrySymbol = "log_records";
    Options.Android->LogRecords = records();
    Options.Android->Memory.push_back(
        {Output, 32768, std::vector<uint8_t>(32768, 0xc7), false});
    Options.Android->ReadMemory = {{Output, 32768}};
    Options.Android->Arguments = {Output, 0x800, 0, 0, NoTime, 3, 4};
    Options.InstructionQuantum = 17;
#endif
  }
  void put(size_t Offset, llvm::StringRef Text) {
    auto &Bytes = Options.Android->Memory[0].Bytes;
    std::copy(Text.bytes_begin(), Text.bytes_end(), Bytes.begin() + Offset);
    Bytes[Offset + Text.size()] = 0;
  }
  void bindAbortSlot(size_t Offset = 2048) {
    Options.Android->AbortMessageSlot = Output + Offset;
    integer(Options.Android->Memory[0].Bytes, Offset, 0, 8);
    Options.Android->MallocFillByte = 0x5a;
    Options.Android->Libraries["libc.so"] = {"android_set_abort_message"};
  }
  void prepareAssertion(unsigned Form, uint64_t Mode, llvm::StringRef Message) {
    bindAbortSlot();
    Options.Android->Libraries["libc.so"].push_back("abort");
    Options.Android->Libraries["liblog.so"] = {"__android_log_assert"};
    Options.Android->EntrySymbol = "log_assertion";
    Options.Android->ObserveMemoryAtStop = true;
    Options.Android->ReadMemory = {{Output, 32768}};
    Options.Android->ThreadLimit = Mode & (1u << 24) ? 3 : 1;
    Options.LinuxSignals.emplace();
    Options.LinuxSignals->InitialMask = uint64_t(1) << 5;
    Options.LinuxSignals->AlternateStackDisabled = true;
    Options.LinuxSignals->Actions[6] = {};
    put(4096, "condition%text");
    put(10000, "tag");
    put(16000, "%s");
    put(18000, Message);
    Options.Android->Arguments = {Output,         Output + 2048, Form,
                                  Mode,           Output + 4096, Output + 10000,
                                  Output + 16000, Output + 18000};
    std::vector<uint8_t> Payload{7, 't', 'a', 'g', 0};
    Payload.insert(Payload.end(), Message.bytes_begin(), Message.bytes_end());
    Payload.push_back(0);
    Options.Android->TextLogWrites = {{{0, Payload, -11}}};
  }
  std::string prepareFortify(unsigned Function, uint64_t Mode,
                             llvm::ArrayRef<uint64_t> Arguments,
                             llvm::StringRef Detail) {
    const std::string Message = ("FORTIFY: " + Detail).str();
    prepareAssertion(4 + (Mode & 1), Mode, Message);
    Options.Android->EntrySymbol = "log_fortify";
    Options.Android->Libraries["libc.so"].assign(std::begin(FortifySymbols),
                                                 std::end(FortifySymbols));
    Options.OutputLimit = 65536;
    Options.LinuxFiles.emplace();
    put(8192, "abc");
    put(12000, "x");
    put(16000, "%s");
    for (size_t I = 0; I < Arguments.size(); ++I)
      integer(Options.Android->Memory[0].Bytes, 4096 + 8 * I, Arguments[I], 8);
    Options.Android->Arguments = {Output, Output + 2048, Function, Mode,
                                  Output + 4096};
    std::vector<uint8_t> Payload{7, 'l', 'i', 'b', 'c', 0};
    Payload.insert(Payload.end(), Message.begin(), Message.end());
    Payload.push_back(0);
    Options.Android->TextLogWrites = {{{4, Payload, 1, true, 0}}};
    return Message;
  }
  std::vector<uint8_t> assertionExpected(const ProcessResult &Result) {
    auto Expected = Options.Android->Memory[0].Bytes;
    std::fill_n(Expected.begin(), 256, 0);
    // TLS allocation belongs to the process, not this assertion fixture.
    integer(Expected, 12 * 8, word(Result.MemorySnapshots[0].Bytes, 12 * 8), 8);
    integer(Expected, 13 * 8, 1000, 8);
    return Expected;
  }
  void prepareFormatted(unsigned Form, unsigned Mode, llvm::StringRef Format,
                        llvm::StringRef Message) {
    Options.Android->EntrySymbol = "log_formatted";
    Options.Android->Libraries["liblog.so"] = {"__android_log_print",
                                               "__android_log_buf_print",
                                               "__android_log_vprint"};
    put(4096, Format);
    put(8192, "abcdef");
    put(12288, "tag");
    Options.Android->Arguments = {Output,        Form, Mode,  Output + 4096,
                                  Output + 8192, 3,    0x106, Output + 12288};
    std::vector<uint8_t> Payload{6, 't', 'a', 'g', 0};
    Payload.insert(Payload.end(), Message.bytes_begin(), Message.bytes_end());
    Payload.push_back(0);
    const unsigned Buffer = Form == 1 || Form == 4 ? 3 : 0;
    Options.Android->TextLogWrites = {
        {{Buffer, Payload, -11}, {Buffer, Payload, INT32_MAX}}};
  }
  void checkTwoTextWrites() {
    auto Result =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Returned) << Result->Diagnostic;
    EXPECT_EQ(Result->ReturnValue, 0u);
    EXPECT_EQ(Result->TextLogWritesConsumed, 2u);
    auto Expected = Options.Android->Memory[0].Bytes;
    integer(Expected, 0, uint64_t(-11), 8);
    integer(Expected, 8, 73, 8);
    integer(Expected, 16, INT32_MAX, 8);
    integer(Expected, 24, 73, 8);
    ASSERT_EQ(Result->MemorySnapshots.size(), 1u);
    EXPECT_EQ(Result->MemorySnapshots[0].Bytes, Expected);
  }
  void check(std::initializer_list<int> Indices) {
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
    ASSERT_EQ(R->ReturnValue, 0u);
    std::vector<uint8_t> Bytes(32768, 0xc7);
    unsigned Slot = 0;
    for (int I : Indices)
      expected(Bytes, Slot++,
               I < 0 ? nullptr : &(*Options.Android->LogRecords)[I]);
    ASSERT_EQ(R->MemorySnapshots.size(), 1u);
    EXPECT_EQ(R->MemorySnapshots[0].Bytes, Bytes);
  }
};
TEST_P(AndroidLog, FullRecordsPIDTimeTailAndEOF) {
  check({0, 1, 2, 3});
  Options.Android->Arguments = {Output, 0x800, 1, 42, NoTime, 3, 2};
  check({2, -1});
  Options.Android->Arguments = {Output, 0x800, 0, 0, (uint64_t(8) << 32) | 20,
                                3,      3};
  check({3, -1, -1});
  Options.Android->Arguments = {Output, 0x800, 0, 0, NoTime, 2, 3};
  check({1, 3, -1});
  Options.Android->Arguments = {Output, 0, 0, 0, 0, 3, 4};
  check({0, 1, 2, 3});
  (*Options.Android->LogRecords)[2].Nanoseconds = 7;
  Options.Android->Arguments = {Output, 0x800, 0, 0, (uint64_t(7) << 32) | 20,
                                3,      2};
  check({3, -1});
}
TEST_P(AndroidLog, MaximumPayloadAndEmptySnapshotClearWholeMessage) {
  Options.Android->LogRecords = {{0, 42, UINT32_MAX, UINT32_MAX, 999999999,
                                  UINT32_MAX,
                                  std::vector<uint8_t>(4068, 0xa6)}};
  Options.Android->Arguments.back() = 2;
  check({0, -1});
  Options.Android->LogRecords->clear();
  check({-1, -1});
}
TEST_P(AndroidLog, IndependentListsAndNewBufferResetKeepDuplicateIdentity) {
  Options.Android->EntrySymbol = "log_lifecycle";
  Options.Android->Arguments = {Output};
  check({0, 0, 0, 1});
}
TEST_P(AndroidLog, DynamicProvidersUseTheSameFiveOperations) {
  Options.Android->EntrySymbol = "log_dynamic";
  Options.Android->Arguments = {Output};
  Options.Android->Libraries["liblog.so"] = {
      "android_logger_list_alloc", "android_logger_list_alloc_time",
      "android_logger_open", "android_logger_list_read",
      "android_logger_list_free"};
  check({1});
}
TEST_P(AndroidLog, MissingSnapshotBlockingExhaustionAndModesStayUnsupported) {
  Options.Android->LogRecords.reset();
  auto Missing =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Missing->Diagnostic.find("missing explicit"), std::string::npos);
  EXPECT_EQ(Missing->MemorySnapshots[0].Bytes,
            std::vector<uint8_t>(32768, 0xc7));
  Options.Android->LogRecords.emplace();
  for (uint64_t Mode : {0u, 1u, 0x40000800u, 0x80000800u}) {
    Options.Android->Arguments[1] = Mode;
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R->MemorySnapshots[0].Bytes, std::vector<uint8_t>(32768, 0xc7));
    ASSERT_FALSE(R->NativeCalls.empty());
    EXPECT_FALSE(R->NativeCalls.back().Result);
  }
}
TEST_P(AndroidLog, NullEmptyAndInvalidIDPreserveErrnoAndOutput) {
  Options.Android->EntrySymbol = "log_invalid";
  for (unsigned Which = 0; Which < 3; ++Which) {
    Options.Android->Arguments = {Output, Which};
    check({});
  }
}
TEST_P(AndroidLog, RetiredUnmappedAndTamperedHandlesFailClearly) {
  Options.Android->EntrySymbol = "log_invalid";
  for (unsigned Which = 3; Which < 9; ++Which) {
    Options.Android->Arguments = {Output, Which};
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure) << Which;
    EXPECT_FALSE(R->ReturnValue);
    EXPECT_FALSE(R->Diagnostic.empty());
    ASSERT_FALSE(R->NativeCalls.empty());
    EXPECT_EQ(R->NativeCalls.back().Name, "android_logger_list_read");
    EXPECT_FALSE(R->NativeCalls.back().Result);
    const char *Message = Which == 3                   ? "retired"
                          : Which == 4                 ? "invalid guest pointer"
                          : (Which == 5 || Which == 7) ? "remain opaque"
                                                       : "unmapped";
    EXPECT_NE(R->Diagnostic.find(Message), std::string::npos);
  }
}
TEST_P(AndroidLog, DirectOptionsRejectMalformedAndOversizedSnapshots) {
  for (unsigned Case = 0; Case < 7; ++Case) {
    Options.Android->LogRecords = records();
    auto &Records = *Options.Android->LogRecords;
    switch (Case) {
    case 0:
      Records[0].ID = 6;
      break;
    case 1:
      Records[0].Nanoseconds = 1000000000;
      break;
    case 2:
      Records[0].PID = 0x80000000;
      break;
    case 3:
      Records[0].Payload.resize(4069);
      break;
    case 4:
      Records.assign(4097, NativeLogRecord{});
      break;
    case 5:
      Records.assign(257, NativeLogRecord{});
      for (auto &R : Records)
        R.Payload.resize(4068);
      break;
    case 6:
      std::swap(Records[0], Records[1]);
      break;
    }
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    EXPECT_FALSE(bool(R)) << Case;
    llvm::consumeError(R.takeError());
  }
}
TEST_P(AndroidLog, FormattedWritesShareGrammarStackedArgumentsAndResults) {
  for (unsigned Form = 0; Form != 6; ++Form) {
    SCOPED_TRACE(Form);
    prepareFormatted(Form, 0, "%+08d|%#lx|%.3s|%d|%d|%d|%d|%d|%d",
                     "-0000123|0xfedcba9876543210|abc|7|8|9|10|11|12");
    checkTwoTextWrites();
  }
}

TEST_P(AndroidLog, FormattedWritesClipTheBufferBeforeCStringLogging) {
  for (unsigned Form = 0; Form != 6; ++Form) {
    SCOPED_TRACE(Form);
    for (unsigned Width : {0u, 1022u, 1023u, 1024u, 1000000000u}) {
      SCOPED_TRACE(Width);
      // API 28's 1024-byte buffer includes NUL. The transcript result, not
      // the untruncated formatting count, is returned to the caller.
      prepareFormatted(Form, Width + 10, "",
                       std::string(std::min(Width, 1023u), ' '));
      checkTwoTextWrites();
    }
    prepareFormatted(Form, 2, "%cignored", "");
    checkTwoTextWrites();
  }
}

TEST_P(AndroidLog, FormattedPrecisionDoesNotReadPastTheGuestPage) {
  for (unsigned Form = 0; Form != 6; ++Form) {
    SCOPED_TRACE(Form);
    prepareFormatted(Form, 1, "%.*s", "ABCD");
    auto &Bytes = Options.Android->Memory[0].Bytes;
    std::copy_n("ABCD", 4,
                Bytes.end() - 4); // No NUL; the next page is unmapped.
    Options.Android->Arguments[4] = Output + Bytes.size() - 4;
    checkTwoTextWrites();
  }
}

TEST_P(AndroidLog, FormattedUnsupportedConversionsPrecedeTagAndLogReads) {
  for (unsigned Form = 0; Form != 3; ++Form)
    for (const char *Format : {"%n", "%f", "%ls", "bad%", "%c%n"}) {
      SCOPED_TRACE(Form);
      SCOPED_TRACE(Format);
      prepareFormatted(Form, 3, Format, "");
      Options.Android->Arguments[7] =
          1; // Would fault if the tag were read first.
      auto Result =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, ProcessStopReason::UnsupportedService)
          << Result->Diagnostic;
      const std::string ExpectedReason =
          std::string(Format) == "%ls"
              ? "wide or length-qualified text conversion"
          : std::string(Format) == "bad%" ? "incomplete format conversion"
                                          : "unsupported format conversion";
      EXPECT_NE(Result->Diagnostic.find(ExpectedReason), std::string::npos)
          << Result->Diagnostic;
      EXPECT_EQ(Result->TextLogWritesConsumed, 0u);
      EXPECT_FALSE(Result->ReturnValue);
      ASSERT_EQ(Result->MemorySnapshots.size(), 1u);
      EXPECT_EQ(Result->MemorySnapshots[0].Bytes,
                Options.Android->Memory[0].Bytes);
    }
}

TEST_P(AndroidLog, FormattedWritesKeepExplicitTranscriptBoundaries) {
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    SCOPED_TRACE(Mode);
    prepareFormatted(2, 0, "%d", "-123");
    if (Mode == 0)
      Options.Android->TextLogWrites.reset();
    else if (Mode == 1)
      Options.Android->TextLogWrites->clear();
    else if (Mode == 2)
      Options.Android->TextLogWrites->front().Payload[5] = '+';
    else if (Mode == 3)
      Options.Android->TextLogWrites->back().ID = 3;
    else
      Options.Android->Arguments[6] =
          7; // Fatal abort-message state remains explicit.
    auto Result =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, ProcessStopReason::UnsupportedService)
        << Result->Diagnostic;
    if (Mode == 0)
      EXPECT_FALSE(Result->TextLogWritesConsumed);
    else
      EXPECT_EQ(Result->TextLogWritesConsumed, Mode == 3 ? 1u : 0u);
    auto Expected = Options.Android->Memory[0].Bytes;
    if (Mode == 3) {
      integer(Expected, 0, uint64_t(-11), 8);
      integer(Expected, 8, 73, 8);
    }
    ASSERT_EQ(Result->MemorySnapshots.size(), 1u);
    EXPECT_EQ(Result->MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidLog, TextWritesShareOrderedResultsRoutingAndErrno) {
  Options.Android->EntrySymbol = "log_text_write";
  Options.Android->Libraries["liblog.so"] = {"__android_log_write"};
  for (unsigned mode = 0; mode != 5; ++mode)
    for (unsigned form = 0; form != 3; ++form) {
      SCOPED_TRACE(mode);
      SCOPED_TRACE(form);
      const std::string tag = mode == 1 ? ""
                              : mode >= 3
                                  ? "RIL-abcdefghijklmnopqrstuvwxyz012345"
                                  : "tag";
      const std::string routed = mode == 4 && form == 1 ? tag
                                 : mode >= 3 ? "use-Rlog/RLOG-RIL-abcdefghijklm"
                                             : tag;
      auto &memory = Options.Android->Memory[0].Bytes;
      std::copy(tag.begin(), tag.end(), memory.begin() + 4096);
      memory[4096 + tag.size()] = 0;
      memory[8192] = 'h';
      memory[8193] = 'i';
      memory[8194] = 0;
      const unsigned id = mode == 4 ? 1 : 3;
      Options.Android->Arguments = {Output,
                                    id,
                                    mode == 2 ? 0x106u : 6u,
                                    mode == 1 ? 0 : Output + 4096,
                                    Output + 8192,
                                    3,
                                    form};
      std::vector<uint8_t> payload{6};
      payload.insert(payload.end(), routed.begin(), routed.end());
      payload.insert(payload.end(), {0, 'h', 'i', 0});
      const unsigned buffer = mode >= 3 ? 1 : form == 1 ? id : 0;
      Options.Android->TextLogWrites = {{{buffer, payload, INT32_MAX},
                                         {buffer, payload, -11},
                                         {buffer, payload, 0}}};
      auto result =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      ASSERT_TRUE(bool(result)) << llvm::toString(result.takeError());
      ASSERT_EQ(result->Stop, ProcessStopReason::Returned)
          << result->Diagnostic;
      EXPECT_EQ(result->ReturnValue, 0u);
      EXPECT_EQ(result->TextLogWritesConsumed, 3u);
      auto expected = memory;
      const uint64_t returns[] = {INT32_MAX, uint64_t(-11), 0};
      for (unsigned i = 0; i != 3; ++i) {
        integer(expected, 16 * i, returns[i], 8);
        integer(expected, 16 * i + 8, 73, 8);
      }
      ASSERT_EQ(result->MemorySnapshots.size(), 1u);
      EXPECT_EQ(result->MemorySnapshots[0].Bytes, expected);
    }
}

TEST_P(AndroidLog, AbortMessageKeepsTheFirstRecordAcrossCallsAndThreads) {
  Options.Android->EntrySymbol = "log_abort_message";
  for (unsigned Form = 0; Form != 4; ++Form)
    for (unsigned Length : {0u, 5u, 5000u}) {
      SCOPED_TRACE(Form);
      SCOPED_TRACE(Length);
      bindAbortSlot();
      Options.Android->ThreadLimit = Form >= 2 ? 3 : 1;
      const std::string Message(Length, 'm');
      put(4096, Message);
      auto Record = abortRecord(Message);
      // The occupied slot must avoid even reading the second argument.
      Options.Android->Arguments = {
          Output, Output + 2048, Output + 4096, 1, Record.size(), Form, 0};
      auto Result =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, ProcessStopReason::Returned)
          << Result->Diagnostic;
      EXPECT_EQ(Result->ReturnValue, 0u);
      EXPECT_FALSE(Result->TextLogWritesConsumed);
      ASSERT_EQ(Result->MemorySnapshots.size(), 1u);
      auto Expected = Options.Android->Memory[0].Bytes;
      const auto Address = word(Result->MemorySnapshots[0].Bytes, 2048);
      ASSERT_NE(Address, 0u);
      EXPECT_EQ(Address % 4096, 0u);
      integer(Expected, 2048, Address, 8);
      for (unsigned I = 0; I != 2; ++I) {
        integer(Expected, 16 * I, 73, 8);
        integer(Expected, 16 * I + 8, Address, 8);
      }
      for (unsigned Offset : {10000u, 24000u})
        std::copy(Record.begin(), Record.end(), Expected.begin() + Offset);
      EXPECT_EQ(Result->MemorySnapshots[0].Bytes, Expected);
    }
}

TEST_P(AndroidLog, AbortMessageUsesTheLiveSlotWithoutRetiringTheOldMapping) {
  Options.Android->EntrySymbol = "log_abort_message";
  for (unsigned Form = 0; Form != 2; ++Form) {
    SCOPED_TRACE(Form);
    bindAbortSlot();
    put(4096, "first");
    put(16384, "later");
    Options.Android->Arguments = {
        Output, Output + 2048, Output + 4096, Output + 16384, 4096, Form, 1};
    auto Result =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->MemorySnapshots.size(), 1u);
    const auto &Bytes = Result->MemorySnapshots[0].Bytes;
    const auto First = word(Bytes, 8), Second = word(Bytes, 24);
    ASSERT_NE(First, 0u);
    ASSERT_NE(Second, 0u);
    EXPECT_NE(First, Second);
    auto Expected = Options.Android->Memory[0].Bytes;
    integer(Expected, 0, 73, 8);
    integer(Expected, 8, First, 8);
    integer(Expected, 16, 73, 8);
    integer(Expected, 24, Second, 8);
    integer(Expected, 2048, Second, 8);
    auto Before = abortRecord("first"), After = abortRecord("later");
    std::copy(Before.begin(), Before.end(), Expected.begin() + 10000);
    std::copy(After.begin(), After.end(), Expected.begin() + 24000);
    EXPECT_EQ(Bytes, Expected);
  }
}

TEST_P(AndroidLog, AbortMessageBindingAndGuestAccessRemainLazy) {
  Options.Android->EntrySymbol = "log_abort_message";
  Options.Android->ObserveMemoryAtStop = true;
  for (unsigned Mode = 0; Mode != 7; ++Mode) {
    SCOPED_TRACE(Mode);
    bindAbortSlot(8192);
    put(4096, "fatal");
    const bool ReadOnly = Mode == 4 || Mode == 5;
    const bool Occupied = Mode == 2 || Mode == 5;
    const bool Returned = Mode == 1 || Occupied;
    if (Mode == 0)
      Options.Android->AbortMessageSlot.reset();
    else if (Mode == 1 || Mode == 3)
      Options.Android->AbortMessageSlot = Mode == 1 ? 0 : 1;
    if (Occupied)
      integer(Options.Android->Memory[0].Bytes, 8192, 0xdeadbeef, 8);
    Options.Android->Arguments = {
        Output, Output + 8192,     Mode == 4 ? Output + 4096 : 1, 1, 0,
        0,      ReadOnly ? 2u : 0u};
    // The anonymous mmap policy chooses this free address in this profile.
    Options.Android->ReadMemory = {{Output, 32768}, {0x100000000, 4096, false}};
    auto Result =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, Returned    ? ProcessStopReason::Returned
                            : Mode == 0 ? ProcessStopReason::UnsupportedService
                                        : ProcessStopReason::RuntimeFailure)
        << Result->Diagnostic;
    ASSERT_EQ(Result->MemorySnapshots.size(), 2u);
    auto Expected = Options.Android->Memory[0].Bytes;
    if (Returned)
      for (unsigned I = 0; I != 2; ++I) {
        integer(Expected, 16 * I, 73, 8);
        integer(Expected, 16 * I + 8, Occupied ? 0xdeadbeef : 0, 8);
      }
    EXPECT_EQ(Result->MemorySnapshots[0].Bytes, Expected);
    if (Mode == 4) {
      // Read-only slot publication fails after the complete mmap/copy.
      EXPECT_EQ(Result->MemorySnapshots[1].Bytes, abortRecord("fatal"));
      EXPECT_TRUE(Result->MemorySnapshots[1].Diagnostic.empty());
    } else {
      EXPECT_TRUE(Result->MemorySnapshots[1].Bytes.empty());
      EXPECT_FALSE(Result->MemorySnapshots[1].Diagnostic.empty());
    }
  }
}

TEST_P(AndroidLog, FatalRawWritesKeepUnclippedAbortBytesAndExplicitResults) {
  Options.Android->EntrySymbol = "log_fatal_write";
  Options.Android->Libraries["liblog.so"] = {"__android_log_write",
                                             "__android_log_buf_write"};
  for (unsigned Form = 0; Form != 4; ++Form)
    for (unsigned Mode = 0; Mode != 4; ++Mode) {
      SCOPED_TRACE(Form);
      SCOPED_TRACE(Mode);
      bindAbortSlot();
      const std::string Tag = Mode == 2   ? std::string(5000, 't')
                              : Mode == 3 ? "RIL"
                                          : "tag";
      const std::string Message = Mode == 0   ? ""
                                  : Mode == 1 ? "fatal"
                                              : std::string(5000, 'm');
      put(4096, Tag);
      put(16384, Message);
      const auto Record = abortRecord(Message);
      const std::string WireTag = Mode == 3 ? "use-Rlog/RLOG-RIL" : Tag;
      std::vector<uint8_t> Payload{7};
      Payload.insert(Payload.end(), WireTag.begin(), WireTag.end());
      Payload.push_back(0);
      Payload.insert(Payload.end(), Message.begin(), Message.end());
      Payload.push_back(0);
      if (Payload.size() > 4068)
        Payload.resize(4068);
      const unsigned ID = Mode == 3 ? 1 : Form == 1 || Form == 3 ? 3 : 0;
      Options.Android->TextLogWrites = {
          {{ID, Payload, -11}, {ID, Payload, INT32_MAX}}};
      Options.Android->Arguments = {Output,        Output + 2048,
                                    Output + 4096, Output + 16384,
                                    Form,          Record.size()};
      auto Result =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, ProcessStopReason::Returned)
          << Result->Diagnostic;
      EXPECT_EQ(Result->ReturnValue, 0u);
      EXPECT_EQ(Result->TextLogWritesConsumed, 2u);
      ASSERT_EQ(Result->MemorySnapshots.size(), 1u);
      auto Expected = Options.Android->Memory[0].Bytes;
      const auto Address = word(Result->MemorySnapshots[0].Bytes, 2048);
      ASSERT_NE(Address, 0u);
      EXPECT_EQ(Address % 4096, 0u);
      integer(Expected, 2048, Address, 8);
      integer(Expected, 0, uint64_t(-11), 8);
      integer(Expected, 8, 73, 8);
      integer(Expected, 16, INT32_MAX, 8);
      integer(Expected, 24, 73, 8);
      std::copy(Record.begin(), Record.end(), Expected.begin() + 24000);
      EXPECT_EQ(Result->MemorySnapshots[0].Bytes, Expected);
    }
}

TEST_P(AndroidLog, FatalFormattedWritesStoreTheFormattedBufferBeforeTransport) {
  for (unsigned Form = 0; Form != 6; ++Form)
    for (unsigned TagSize : {3u, 5000u}) {
      SCOPED_TRACE(Form);
      SCOPED_TRACE(TagSize);
      prepareFormatted(Form, 5010, "", "");
      bindAbortSlot();
      Options.Android->EntrySymbol = "log_fatal_formatted";
      const std::string Tag(TagSize, 't'), Message(1023, ' ');
      put(12288, Tag);
      auto Record = abortRecord(Message);
      std::vector<uint8_t> Payload{7};
      Payload.insert(Payload.end(), Tag.begin(), Tag.end());
      Payload.push_back(0);
      Payload.insert(Payload.end(), Message.begin(), Message.end());
      Payload.push_back(0);
      if (Payload.size() > 4068)
        Payload.resize(4068);
      for (auto &Write : *Options.Android->TextLogWrites)
        Write.Payload = Payload;
      Options.Android->Arguments = {
          Output,        Output + 2048, Form,           5010,
          Output + 4096, Output + 8192, Output + 12288, Record.size()};
      auto Result =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, ProcessStopReason::Returned)
          << Result->Diagnostic;
      EXPECT_EQ(Result->TextLogWritesConsumed, 2u);
      ASSERT_EQ(Result->MemorySnapshots.size(), 1u);
      auto Expected = Options.Android->Memory[0].Bytes;
      const auto Address = word(Result->MemorySnapshots[0].Bytes, 2048);
      ASSERT_NE(Address, 0u);
      EXPECT_EQ(Address % 4096, 0u);
      integer(Expected, 2048, Address, 8);
      integer(Expected, 0, uint64_t(-11), 8);
      integer(Expected, 8, 73, 8);
      integer(Expected, 16, INT32_MAX, 8);
      integer(Expected, 24, 73, 8);
      std::copy(Record.begin(), Record.end(), Expected.begin() + 24000);
      EXPECT_EQ(Result->MemorySnapshots[0].Bytes, Expected);
    }
}

TEST_P(AndroidLog,
       AbortMessageAllocationFailurePreservesTheSlotAndPermitsRetry) {
  Options.MemoryLimit = 8 * 1024 * 1024;
  Options.Android->EntrySymbol = "log_abort_exhaust";
  for (unsigned Form = 0; Form != 2; ++Form) {
    SCOPED_TRACE(Form);
    bindAbortSlot();
    put(4096, "retry");
    auto Record = abortRecord("retry");
    Options.Android->Arguments = {Output, Output + 2048, Output + 4096, Form,
                                  Record.size()};
    Options.Android->TextLogWrites = {
        {{0, {7, 't', 'a', 'g', 0, 'r', 'e', 't', 'r', 'y', 0}, -11},
         {0, {7, 't', 'a', 'g', 0, 'r', 'e', 't', 'r', 'y', 0}, 19}}};
    auto Result =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Returned) << Result->Diagnostic;
    EXPECT_EQ(Result->ReturnValue, 0u);
    EXPECT_EQ(Result->TextLogWritesConsumed, Form ? 2u : 0u);
    ASSERT_EQ(Result->MemorySnapshots.size(), 1u);
    auto Expected = Options.Android->Memory[0].Bytes;
    const auto Address = word(Result->MemorySnapshots[0].Bytes, 2048);
    ASSERT_NE(Address, 0u);
    integer(Expected, 2048, Address, 8);
    integer(Expected, 0, 12,
            8); // ENOMEM from Bionic mmap, even if log returns.
    integer(Expected, 8, 0, 8);
    integer(Expected, 16, 73, 8);
    integer(Expected, 24, Address, 8);
    integer(Expected, 32, Form ? uint64_t(-11) : 0, 8);
    integer(Expected, 40, Form ? 19 : 0, 8);
    std::copy(Record.begin(), Record.end(), Expected.begin() + 24000);
    EXPECT_EQ(Result->MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidLog, FatalPriorityUsesTheWholeIntBeforeTheWireByte) {
  Options.Android->EntrySymbol = "log_text_write";
  bindAbortSlot();
  put(4096, "tag");
  put(8192, "message");
  Options.Android->Arguments = {Output,        0, 0x107, Output + 4096,
                                Output + 8192, 2, 0};
  std::vector<uint8_t> Payload{7,   't', 'a', 'g', 0,   'm', 'e',
                               's', 's', 'a', 'g', 'e', 0};
  Options.Android->TextLogWrites = {
      {{0, Payload, -11}, {0, Payload, INT32_MAX}}};
  checkTwoTextWrites(); // In particular, the empty abort slot stays empty.
}

TEST_P(AndroidLog, FatalPublicationPrecedesWireReadsAndTranscriptAdmission) {
  Options.Android->EntrySymbol = "log_text_write";
  Options.Android->ObserveMemoryAtStop = true;
  Options.Android->ReadMemory = {{Output, 32768}, {0x100000000, 4096, false}};
  for (unsigned Mode = 0; Mode != 9; ++Mode) {
    SCOPED_TRACE(Mode);
    bindAbortSlot();
    put(4096, "fatal");
    put(16384, "tag");
    uint64_t Tag = Output + 16384, Message = Output + 4096;
    if (Mode == 3 || Mode == 4) {
      auto &Bytes = Options.Android->Memory[0].Bytes;
      std::fill(Bytes.end() - 128, Bytes.end(), 'X');
      Tag = Output + Bytes.size() - 128;
      if (Mode == 4)
        std::copy_n("RIL", 3, Bytes.end() - 128);
    }
    if (Mode == 5)
      Tag = 1;
    if (Mode == 6)
      Message = 1;
    if (Mode == 7)
      Tag = Output + 2048 + 4;
    if (Mode == 8)
      Message = Output + 2048 + 4;
    Options.Android->Arguments = {Output, Mode == 2 ? 2u : 0u, 7, Tag, Message,
                                  1,      Mode == 2 ? 1u : 0u};
    Options.Android->TextLogWrites = {{{0, {7, 'x', 0, 0}, -11}}};
    if (Mode == 0)
      Options.Android->TextLogWrites.reset();
    if (Mode == 7)
      Options.Android->TextLogWrites = {
          {{0, {7, 1, 0, 'f', 'a', 't', 'a', 'l', 0}, -11}}};
    if (Mode == 8)
      Options.Android->TextLogWrites = {
          {{0, {7, 't', 'a', 'g', 0, 1, 0}, -11}}};
    const bool Stored = Mode < 4 || Mode >= 7;
    auto Result =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, Mode >= 7  ? ProcessStopReason::Returned
                            : Mode < 3 ? ProcessStopReason::UnsupportedService
                                       : ProcessStopReason::RuntimeFailure)
        << Result->Diagnostic;
    if (Mode == 0)
      EXPECT_FALSE(Result->TextLogWritesConsumed);
    else
      EXPECT_EQ(Result->TextLogWritesConsumed, Mode >= 7 ? 1u : 0u);
    ASSERT_EQ(Result->MemorySnapshots.size(), 2u);
    auto Expected = Options.Android->Memory[0].Bytes;
    if (Stored)
      integer(Expected, 2048, 0x100000000, 8);
    if (Mode >= 7) {
      integer(Expected, 0, uint64_t(-11), 8);
      integer(Expected, 8, 73, 8);
    }
    EXPECT_EQ(Result->MemorySnapshots[0].Bytes, Expected);
    if (Stored)
      EXPECT_EQ(Result->MemorySnapshots[1].Bytes,
                abortRecord(Mode == 8 ? "" : "fatal"));
    else {
      EXPECT_TRUE(Result->MemorySnapshots[1].Bytes.empty());
      EXPECT_FALSE(Result->MemorySnapshots[1].Diagnostic.empty());
    }
  }
}

TEST_P(AndroidLog,
       AssertFormatsFallbacksAndOverflowArgumentsBeforeTermination) {
  for (unsigned Form : {0u, 1u})
    for (unsigned Mode = 0; Mode != 9; ++Mode) {
      SCOPED_TRACE(Form);
      SCOPED_TRACE(Mode);
      std::string Message = Mode == 5 ? std::string(5000, 'm') : "message";
      prepareAssertion(Form, Mode == 4 ? 1 : Mode == 6 ? 2 : 0, Message);
      std::string ExpectedMessage = Message;
      if (Mode == 0)
        Options.Android->Arguments[4] = 1; // Unused when format is non-null.
      if (Mode >= 1 && Mode <= 3) {
        Options.Android->Arguments[6] = 0;
        if (Mode == 1)
          ExpectedMessage = "Assertion failed: condition%text";
        if (Mode == 2) {
          Options.Android->Arguments[4] = 0;
          ExpectedMessage = "Unspecified assertion failed";
        }
        if (Mode == 3) {
          put(4096, std::string(5000, 'c'));
          ExpectedMessage = "Assertion failed: " + std::string(1005, 'c');
        }
      }
      if (Mode == 4) {
        put(16000, "%d %llx %s %d %d %d %d %d %d");
        ExpectedMessage = "-123 fedcba9876543210 message 7 8 9 10 11 12";
      }
      if (Mode == 5)
        ExpectedMessage.resize(1023);
      if (Mode == 6) {
        put(16000, "before%cafter%s");
        ExpectedMessage = "before";
      }
      if (Mode == 7) {
        put(16000, "");
        ExpectedMessage = "";
      }
      if (Mode == 8) {
        Options.Android->Arguments[6] = 0;
        put(4096, "");
        ExpectedMessage = "Assertion failed: ";
      }
      std::vector<uint8_t> Payload{7, 't', 'a', 'g', 0};
      Payload.insert(Payload.end(), ExpectedMessage.begin(),
                     ExpectedMessage.end());
      Payload.push_back(0);
      Options.Android->TextLogWrites = {{{0, Payload, -11}}};
      Options.Android->ReadMemory.push_back({0x100000000, 4096, false});
      auto R =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Signaled) << R->Diagnostic;
      EXPECT_EQ(R->TerminationSignal, 6u);
      EXPECT_FALSE(R->ReturnValue);
      EXPECT_FALSE(R->ExitStatus);
      EXPECT_EQ(R->StandardError, ExpectedMessage + "\n");
      EXPECT_TRUE(R->StandardOutput.empty());
      EXPECT_EQ(R->TextLogWritesConsumed, 1u);
      ASSERT_EQ(R->MemorySnapshots.size(), 2u);
      auto Expected = assertionExpected(*R);
      integer(Expected, 2048, 0x100000000, 8);
      EXPECT_EQ(R->MemorySnapshots[0].Bytes, Expected);
      EXPECT_EQ(R->MemorySnapshots[1].Bytes, abortRecord(ExpectedMessage));
      ASSERT_FALSE(R->NativeCalls.empty());
      EXPECT_EQ(R->NativeCalls.back().Name, "__android_log_assert");
      EXPECT_FALSE(R->NativeCalls.back().Result);
    }
}

TEST_P(AndroidLog, AssertKeepsStderrBeforeFatalRoutingAndTransportClipping) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    SCOPED_TRACE(Mode);
    prepareAssertion(Mode & 1, 0, "message");
    const std::string Tag = Mode == 0   ? "RIL"
                            : Mode == 1 ? std::string(5000, 't')
                                        : "";
    put(10000, Tag);
    if (Mode == 2)
      Options.Android->Arguments[5] = 0;
    const std::string WireTag = Mode == 0 ? "use-Rlog/RLOG-RIL" : Tag;
    std::vector<uint8_t> Payload{7};
    Payload.insert(Payload.end(), WireTag.begin(), WireTag.end());
    Payload.push_back(0);
    for (auto C : std::string("message\0", 8))
      Payload.push_back(C);
    if (Payload.size() > 4068)
      Payload.resize(4068);
    Options.Android->TextLogWrites = {{{Mode == 0 ? 1u : 0u, Payload, 0}}};
    Options.Android->ReadMemory.push_back({0x100000000, 4096, false});
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Signaled) << R->Diagnostic;
    EXPECT_EQ(R->TerminationSignal, 6u);
    EXPECT_EQ(R->StandardError, "message\n");
    EXPECT_EQ(R->TextLogWritesConsumed, 1u);
    ASSERT_EQ(R->MemorySnapshots.size(), 2u);
    EXPECT_EQ(R->MemorySnapshots[1].Bytes, abortRecord("message"));
  }
}

TEST_P(AndroidLog, AssertStopsAtTheFirstIncompleteEffect) {
  for (unsigned Mode = 0; Mode != 9; ++Mode) {
    SCOPED_TRACE(Mode);
    prepareAssertion(0, 0, "message");
    Options.OutputLimit = Mode == 0 ? 7 : 65536;
    if (Mode == 1)
      put(16000, "%n");
    if (Mode == 2)
      Options.Android->Arguments[6] = 1;
    if (Mode == 3)
      Options.Android->Arguments[5] = 1;
    if (Mode == 4)
      Options.Android->AbortMessageSlot.reset();
    if (Mode == 5)
      Options.Android->TextLogWrites.reset();
    if (Mode == 6)
      (*Options.Android->TextLogWrites)[0].Payload[1] = 'x';
    if (Mode == 7)
      Options.LinuxSignals.reset();
    if (Mode == 8) {
      auto &Bytes = Options.Android->Memory[0].Bytes;
      std::fill(Bytes.end() - 2048, Bytes.end(), 'x');
      Options.Android->Arguments[4] = Output + Bytes.size() - 2048;
      Options.Android->Arguments[6] = 0;
    }
    if (Mode == 0)
      Options.Android->ReadMemory.clear();
    else
      Options.Android->ReadMemory.push_back({0x100000000, 4096, false});
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    const bool Fault = Mode == 2 || Mode == 3 || Mode == 8;
    EXPECT_EQ(R->Stop, Mode == 0 ? ProcessStopReason::OutputLimit
                       : Fault   ? ProcessStopReason::RuntimeFailure
                                 : ProcessStopReason::UnsupportedService)
        << R->Diagnostic;
    EXPECT_FALSE(R->TerminationSignal);
    EXPECT_EQ(R->StandardError, Mode >= 3 && Mode != 8 ? "message\n" : "");
    if (Mode == 5)
      EXPECT_FALSE(R->TextLogWritesConsumed);
    else
      EXPECT_EQ(R->TextLogWritesConsumed, Mode == 7 ? 1u : 0u);
    if (Mode == 0) {
      EXPECT_TRUE(R->MemorySnapshots.empty());
      continue;
    }
    ASSERT_EQ(R->MemorySnapshots.size(), 2u);
    auto Expected = assertionExpected(*R);
    if (Mode >= 5 && Mode <= 7) {
      integer(Expected, 2048, 0x100000000, 8);
      EXPECT_EQ(R->MemorySnapshots[1].Bytes, abortRecord("message"));
    } else {
      EXPECT_TRUE(R->MemorySnapshots[1].Bytes.empty());
      EXPECT_FALSE(R->MemorySnapshots[1].Diagnostic.empty());
    }
    EXPECT_EQ(R->MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidLog, AssertUsesLiveStderrDescriptorsAndSharedOutputLimits) {
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    SCOPED_TRACE(Mode);
    prepareAssertion(Mode & 1, (Mode << 16) | (2u << 8), "message");
    Options.LinuxFiles.emplace();
    Options.LinuxFiles->Files["/assert-output"] = {};
    Options.LinuxFiles->WriteLimits["/assert-output"] = 4096;
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, Mode == 4 ? ProcessStopReason::UnsupportedService
                                 : ProcessStopReason::Signaled)
        << R->Diagnostic;
    EXPECT_EQ(R->StandardError, Mode == 0 ? "message\n" : "");
    EXPECT_EQ(R->StandardOutput, Mode == 2 ? "message\n" : "");
    EXPECT_EQ(R->TextLogWritesConsumed, Mode == 4 ? 0u : 1u);
    ASSERT_EQ(R->MemorySnapshots.size(), 1u);
    const auto &Bytes = R->MemorySnapshots[0].Bytes;
    EXPECT_EQ(word(Bytes, 0), Mode == 4 ? 0u : 1u);
    if (Mode != 4) {
      EXPECT_EQ(R->TerminationSignal, 6u);
      EXPECT_EQ(word(Bytes, 5 * 8), Mode == 1 || Mode == 3 ? 9u : 55u);
      auto Record = abortRecord("message");
      EXPECT_TRUE(
          std::equal(Record.begin(), Record.end(), Bytes.begin() + 24000));
    } else {
      EXPECT_FALSE(R->TerminationSignal);
      EXPECT_EQ(word(Bytes, 2048), 0u);
      EXPECT_NE(R->Diagnostic.find("vectored mutable-file"), std::string::npos);
    }
    EXPECT_EQ(word(Bytes, 17 * 8), 0u);
  }
}

TEST_P(AndroidLog, AbortRunsTheCurrentThreadsHandlerBeforeForcingDefault) {
  for (unsigned Form : {0u, 1u, 2u, 3u})
    for (unsigned Handling : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u})
      for (bool Child : {false, true}) {
        SCOPED_TRACE(Form);
        SCOPED_TRACE(Handling);
        SCOPED_TRACE(Child);
        prepareAssertion(Form, (Handling << 8) | (Child ? 1u << 24 : 0),
                         "message");
        if (Form >= 2) {
          Options.Android->AbortMessageSlot.reset();
          Options.Android->TextLogWrites.reset();
        }
        if (!Child)
          Options.LinuxSignals->InitialMask.reset(); // abort establishes it.
        auto R =
            emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        ASSERT_EQ(R->Stop, Handling == 3 || Handling == 7
                               ? ProcessStopReason::Exited
                           : Handling == 4 ? ProcessStopReason::Returned
                                           : ProcessStopReason::Signaled)
            << R->Diagnostic;
        if (Handling == 3 || Handling == 7)
          EXPECT_EQ(R->ExitStatus, 23u);
        else if (Handling == 4)
          EXPECT_EQ(R->ReturnValue, 99u);
        else {
          EXPECT_EQ(R->TerminationSignal, 6u);
          EXPECT_FALSE(R->ReturnValue);
          EXPECT_FALSE(R->ExitStatus);
          if (Child) {
            ASSERT_EQ(R->NativeThreads.size(), 2u);
            for (const auto &T : R->NativeThreads) {
              EXPECT_EQ(T.TerminationSignal, 6u);
              EXPECT_FALSE(T.Finished);
            }
          }
        }
        EXPECT_EQ(R->StandardError, Form < 2 ? "message\n" : "");
        ASSERT_EQ(R->MemorySnapshots.size(), 1u);
        const auto &Bytes = R->MemorySnapshots[0].Bytes;
        const uint64_t Count = Handling < 2 ? 0 : Handling == 5 ? 2 : 1;
        EXPECT_EQ(word(Bytes, 0), Count);
        EXPECT_EQ(word(Bytes, 17 * 8), Handling == 4 ? 99u : 0u);
        if (Count) {
          EXPECT_EQ(word(Bytes, 8), 6u);
          EXPECT_EQ(word(Bytes, 2 * 8), uint64_t(-6)); // SI_TKILL.
          EXPECT_EQ(word(Bytes, 3 * 8), 1000u);
          EXPECT_EQ(word(Bytes, 4 * 8), 1000u);
          EXPECT_EQ(word(Bytes, 5 * 8), 55u);
          EXPECT_EQ(
              word(Bytes, 6 * 8),
              ~(uint64_t(0xf) << 31 | uint64_t(1) << 8 | uint64_t(1) << 18));
          EXPECT_EQ(word(Bytes, 8 * 8), Child ? 1001u : 1000u);
        }
        for (const auto &Call : R->NativeCalls)
          if (Call.Name == "abort" || Call.Name == "__android_log_assert")
            EXPECT_FALSE(Call.Result);
      }
}

TEST_P(AndroidLog, AbortDeliversUnblockedPendingSignalBeforePrivateSend) {
  for (unsigned Form : {0u, 1u, 2u, 3u})
    for (unsigned Handling : {2u, 3u, 4u, 6u}) {
      SCOPED_TRACE(Form);
      SCOPED_TRACE(Handling);
      prepareAssertion(Form, (Handling << 8) | (1u << 25), "message");
      auto R =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, Handling == 3   ? ProcessStopReason::Exited
                         : Handling == 4 ? ProcessStopReason::Returned
                                         : ProcessStopReason::Signaled)
          << R->Diagnostic;
      ASSERT_EQ(R->MemorySnapshots.size(), 1u);
      const auto &Bytes = R->MemorySnapshots[0].Bytes;
      EXPECT_EQ(word(Bytes, 0), Handling == 2 ? 2u : 1u);
      EXPECT_EQ(word(Bytes, 18 * 8), 0u); // Previously pending kill: SI_USER.
      if (Handling == 2)
        EXPECT_EQ(word(Bytes, 19 * 8), uint64_t(-6)); // Later tgkill: SI_TKILL.
      EXPECT_EQ(R->StandardError, Form < 2 ? "message\n" : "");
      if (Handling == 3)
        EXPECT_EQ(R->ExitStatus, 23u);
      else if (Handling == 4)
        EXPECT_EQ(R->ReturnValue, 99u);
      else
        EXPECT_EQ(R->TerminationSignal, 6u);
      for (const auto &Call : R->NativeCalls)
        if (Call.Name == "abort" || Call.Name == "__android_log_assert")
          EXPECT_FALSE(Call.Result);
    }
}

TEST_P(AndroidLog, TextTransportClipsTagAndMessageWithoutRepairingTerminators) {
  Options.Android->EntrySymbol = "log_text_write";
  Options.Android->Libraries["liblog.so"] = {"__android_log_write",
                                             "__android_log_buf_write"};
  std::vector<std::pair<std::string, std::string>> Cases;
  for (auto [TagSize, MessageSize] : {std::pair{0u, 0u},
                                      {3u, 4062u},
                                      {3u, 4063u},
                                      {3u, 4064u},
                                      {4065u, 0u},
                                      {4066u, 0u},
                                      {4067u, 0u},
                                      {4068u, 32u},
                                      {5000u, 5000u}})
    Cases.emplace_back(std::string(TagSize, 't'),
                       std::string(MessageSize, 'm'));
  Cases.emplace_back(std::string(4064, 't'),
                     "\xe2\x82\xac"); // Cuts UTF-8 bytes.
  Cases.emplace_back("RIL" + std::string(5000, 't'), std::string(5000, 'm'));
  for (unsigned Form = 0; Form != 4; ++Form)
    for (const auto &[Tag, Message] : Cases) {
      SCOPED_TRACE(Form);
      SCOPED_TRACE(Tag.size());
      SCOPED_TRACE(Message.size());
      put(4096, Tag);
      put(16384, Message);
      const bool Radio = Tag.starts_with("RIL");
      const auto WireTag =
          Radio ? "use-Rlog/RLOG-RIL" + std::string(14, 't') : Tag;
      // Independent oracle: the wire is a prefix of three complete iovecs.
      std::vector<uint8_t> Payload{6};
      Payload.insert(Payload.end(), WireTag.begin(), WireTag.end());
      Payload.push_back(0);
      Payload.insert(Payload.end(), Message.begin(), Message.end());
      Payload.push_back(0);
      if (Payload.size() > 4068)
        Payload.resize(4068);
      const unsigned ID = Radio ? 1 : Form == 1 || Form == 3 ? 3 : 0;
      Options.Android->Arguments = {Output,         3, 0x106, Output + 4096,
                                    Output + 16384, 2, Form};
      Options.Android->TextLogWrites = {
          {{ID, Payload, -11}, {ID, Payload, INT32_MAX}}};
      checkTwoTextWrites();
    }
}

TEST_P(AndroidLog, FormattedTransportClipsAfterTheFormattingBuffer) {
  for (unsigned Form = 0; Form != 6; ++Form)
    for (unsigned TagSize : {3042u, 3043u, 4066u, 4067u, 5000u}) {
      SCOPED_TRACE(Form);
      SCOPED_TRACE(TagSize);
      prepareFormatted(Form, 5010, "", ""); // %5000s first retains 1023 spaces.
      const std::string Tag(TagSize, 't');
      put(12288, Tag);
      std::vector<uint8_t> Payload{6};
      Payload.insert(Payload.end(), Tag.begin(), Tag.end());
      Payload.push_back(0);
      Payload.insert(Payload.end(), 1023, ' ');
      Payload.push_back(0);
      Payload.resize(4068);
      for (auto &Write : *Options.Android->TextLogWrites)
        Write.Payload = Payload;
      checkTwoTextWrites();
    }
}

TEST_P(AndroidLog, TextTransportStillReadsCompleteGuestStringsBeforeClipping) {
  Options.Android->EntrySymbol = "log_text_write";
  Options.Android->ObserveMemoryOnFailure = true;
  Options.Android->Libraries["liblog.so"] = {"__android_log_write",
                                             "__android_log_buf_write"};
  for (unsigned Form = 0; Form != 4; ++Form)
    for (unsigned Mode = 0; Mode != 3; ++Mode) {
      SCOPED_TRACE(Form);
      SCOPED_TRACE(Mode);
      put(4096, std::string(5000, 't'));
      put(16384, "message");
      auto &Bytes = Options.Android->Memory[0].Bytes;
      std::fill(Bytes.end() - 5000, Bytes.end(), 'x'); // Unmapped next page.
      const uint64_t Unterminated = Output + Bytes.size() - 5000;
      Options.Android->Arguments = {Output,
                                    3,
                                    6,
                                    Mode == 1 ? Unterminated : Output + 4096,
                                    Mode == 0   ? 1
                                    : Mode == 2 ? Unterminated
                                                : Output + 16384,
                                    1,
                                    Form};
      std::vector<uint8_t> Payload(4068, 't');
      Payload[0] = 6;
      Options.Android->TextLogWrites = {
          {{Form == 1 || Form == 3 ? 3u : 0u, Payload, 0}}};
      auto Result =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, ProcessStopReason::RuntimeFailure)
          << Result->Diagnostic;
      EXPECT_EQ(Result->TextLogWritesConsumed, 0u);
      ASSERT_EQ(Result->MemorySnapshots.size(), 1u);
      EXPECT_EQ(Result->MemorySnapshots[0].Bytes, Bytes);
    }
}

TEST_P(AndroidLog, TextTransportMismatchPreservesTheCompletedPrefix) {
  Options.Android->EntrySymbol = "log_text_write";
  put(4096, "");
  put(16384, std::string(5000, 'm'));
  Options.Android->Arguments = {Output,         0, 6, Output + 4096,
                                Output + 16384, 2, 0};
  std::vector<uint8_t> Payload(4068, 'm');
  Payload[0] = 6;
  Payload[1] = 0;
  Options.Android->TextLogWrites = {{{0, Payload, -11}, {0, Payload, 0}}};
  Options.Android->TextLogWrites->back().Payload.back() = 'x';
  auto Result =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::UnsupportedService)
      << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find("does not match the next entry"),
            std::string::npos);
  EXPECT_EQ(Result->TextLogWritesConsumed, 1u);
  auto Expected = Options.Android->Memory[0].Bytes;
  integer(Expected, 0, uint64_t(-11), 8);
  integer(Expected, 8, 73, 8);
  ASSERT_EQ(Result->MemorySnapshots.size(), 1u);
  EXPECT_EQ(Result->MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidLog, TextWriteBoundariesAndInputAdmissionStayExplicit) {
  Options.Android->EntrySymbol = "log_text_write";
  auto &memory = Options.Android->Memory[0].Bytes;
  memory[4096] = 0;
  std::fill_n(memory.begin() + 8192, 4065, 'x');
  memory[8192 + 4065] = 0;
  std::vector<uint8_t> payload(4068, 'x');
  payload[0] = 6;
  payload[1] = 0;
  payload.back() = 0;
  for (unsigned mode = 0; mode != 9; ++mode) {
    SCOPED_TRACE(mode);
    Options.Android->Arguments = {
        Output, 0, mode == 1 ? 7u : 6u, Output + 4096, Output + 8192, 1, 1};
    Options.Android->TextLogWrites = {{{0, payload, 4068}}};
    memory[8192 + 4065] = mode == 2 ? 'x' : 0;
    memory[8192 + 4066] = 0;
    if (mode == 2)
      Options.Android->TextLogWrites->front().Payload.back() = 'x';
    if (mode == 3)
      Options.Android->Arguments[1] = 2;
    if (mode == 4)
      Options.Android->TextLogWrites->front().ID = 2;
    if (mode == 5)
      Options.Android->TextLogWrites->front().Payload.pop_back();
    if (mode == 6)
      Options.Android->TextLogWrites->front().Payload[4] = 0;
    if (mode == 7)
      Options.Android->TextLogWrites->assign(4097, {0, {6, 0, 0}, 0});
    if (mode == 8)
      Options.Android->TextLogWrites->assign(258, {0, payload, 0});
    auto result =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (mode >= 4) {
      EXPECT_FALSE(bool(result));
      llvm::consumeError(result.takeError());
      continue;
    }
    ASSERT_TRUE(bool(result)) << llvm::toString(result.takeError());
    EXPECT_EQ(result->Stop, mode == 0 || mode == 2
                                ? ProcessStopReason::Returned
                                : ProcessStopReason::UnsupportedService)
        << result->Diagnostic;
    EXPECT_EQ(result->TextLogWritesConsumed, mode == 0 || mode == 2 ? 1u : 0u);
  }
}

TEST_P(AndroidLog, EventWritesShareExactOrderedResultsAndPreserveErrno) {
  Options.Android->EntrySymbol = "log_event_write";
  Options.Android->Libraries["liblog.so"] = {"__android_log_bwrite"};
  const std::vector<uint8_t> Payload{0xf9, 0xff, 0xff, 0xff, 0xc7, 0xc7, 0xc7};
  Options.Android->EventLogWrites = {
      {{Payload, 7}, {Payload, -11}, {Payload, 0}}};
  for (unsigned Dynamic : {0u, 1u}) {
    Options.Android->Arguments = {Output, Output + 4096, 3, 3, Dynamic};
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
    EXPECT_EQ(R->ReturnValue, 0u);
    EXPECT_EQ(R->EventLogWritesConsumed, 3u);
    auto Expected = Options.Android->Memory[0].Bytes;
    const uint64_t Returns[] = {7, uint64_t(-11), 0};
    for (unsigned I = 0; I != 3; ++I) {
      integer(Expected, 16 * I, Returns[I], 8);
      integer(Expected, 16 * I + 8, 73, 8);
    }
    ASSERT_EQ(R->MemorySnapshots.size(), 1u);
    EXPECT_EQ(R->MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidLog, EventWriteUnknownExhaustedAndMismatchDoNotConsume) {
  Options.Android->EntrySymbol = "log_event_write";
  Options.Android->Arguments = {Output, Output + 4096, 1, 2, 0};
  const std::vector<uint8_t> Payload{0xf9, 0xff, 0xff, 0xff, 0xc7};
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    Options.Android->EventLogWrites.reset();
    if (Mode)
      Options.Android->EventLogWrites.emplace();
    if (Mode >= 2)
      Options.Android->EventLogWrites->push_back({Payload, -11});
    if (Mode == 3)
      Options.Android->EventLogWrites->front().Payload[0] = 7;
    if (Mode == 4)
      Options.Android->EventLogWrites->front().Payload.back() = 0;
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R->Diagnostic.find(Mode == 0   ? "not supplied"
                                 : Mode <= 2 ? "exhausted"
                                             : "does not match"),
              std::string::npos);
    if (!Mode)
      EXPECT_FALSE(R->EventLogWritesConsumed);
    else
      EXPECT_EQ(R->EventLogWritesConsumed, Mode == 2 ? 1u : 0u);
  }
}

TEST_P(AndroidLog, FortifyDiagnosticsAndRetainedWritesMatchAPI28) {
  constexpr uint64_t D = Output + 12000, S = Output + 8192, F = Output + 16000;
  struct Case {
    unsigned Function;
    std::vector<uint64_t> Arguments;
    const char *Detail;
    std::vector<uint8_t> Prefix;
  };
  const Case Cases[] = {
      {0,
       {1, 1, 4, 3},
       "memcpy: prevented 4-byte write into 3-byte buffer",
       {}},
      {0,
       {1, 1, UINT64_MAX, 3},
       "memcpy: count 18446744073709551615 > SSIZE_MAX",
       {}},
      {1,
       {1, 1, 4, 3},
       "memmove: prevented 4-byte write into 3-byte buffer",
       {}},
      {2,
       {1, 65, 4, 3},
       "memset: prevented 4-byte write into 3-byte buffer",
       {}},
      {2,
       {1, 65, UINT64_MAX, 3},
       "memset: count 18446744073709551615 > SSIZE_MAX",
       {}},
      {3,
       {1, 65, 4, 3},
       "memchr: prevented 4-byte read from 3-byte buffer",
       {}},
      {4, {UINT64_MAX, 1, 0}, "FD_SET: file descriptor -1 < 0", {}},
      {5, {1024, 1, 0}, "FD_CLR: file descriptor 1024 >= FD_SETSIZE 0", {}},
      {6,
       {0, 1, 127},
       "FD_ISSET: set size 127 is too small to be an fd_set",
       {}},
      {7, {S, 3}, "strlen: detected read past end of buffer", {}},
      {8, {1, S, 3}, "strcpy: prevented 4-byte write into 3-byte buffer", {}},
      {9,
       {1, 1, 4, 3},
       "strncpy: prevented 4-byte write into 3-byte buffer",
       {}},
      {10,
       {D, S, 8, 8, 3},
       "strncpy: detected read past end of 3-byte buffer",
       {'a', 'b', 'c', 0, 0, 0, 0, 0}},
      {11,
       {1, 1, 4, 3},
       "strlcpy: prevented 4-byte write into 3-byte buffer",
       {}},
      {12,
       {D, S, 4},
       "strcat: prevented write past end of 0-byte buffer",
       {'x', 'a', 'b', 'c'}},
      {13,
       {D, S, 3, 4},
       "strncat: prevented write past end of buffer",
       {'x', 'a', 'b', 'c'}},
      {14, {S, 'z', 3}, "strchr: prevented read past end of buffer", {}},
      {15, {S, 'a', 3}, "strrchr: prevented read past end of buffer", {}},
      {16,
       {UINT64_MAX, 1, UINT64_MAX, 0},
       "read: count 18446744073709551615 > SSIZE_MAX",
       {}},
      {16,
       {UINT64_MAX, 1, 4, 3},
       "read: prevented 4-byte write into 3-byte buffer",
       {}},
      {17, {1, UINT64_MAX, 1, 0}, "fgets: buffer size -1 < 0", {}},
      {17,
       {1, 4, 1, 3},
       "fgets: prevented 4-byte write into 3-byte buffer",
       {}},
      {18,
       {1, 2, 2, 1, 3},
       "fread: prevented 4-byte write into 3-byte buffer",
       {}},
      {19,
       {1, 1, 4, 3},
       "vsnprintf: prevented 4-byte write into 3-byte buffer",
       {}},
      {20,
       {1, 1, 4, 3},
       "vsnprintf: prevented 4-byte write into 3-byte buffer",
       {}},
      {21,
       {D, F, 0, 3, S},
       "vsprintf: prevented 4-byte write into 3-byte buffer",
       {'a', 'b', 0}},
      {22,
       {D, F, 0, 3, S},
       "vsprintf: prevented 4-byte write into 3-byte buffer",
       {'a', 'b', 0}}};
  for (const auto &C : Cases)
    for (unsigned Dynamic : {0u, 1u}) {
      SCOPED_TRACE(C.Detail);
      SCOPED_TRACE(Dynamic);
      const auto Message = prepareFortify(C.Function, (2u << 8) | Dynamic,
                                          C.Arguments, C.Detail);
      auto R =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Stop, ProcessStopReason::Signaled) << R->Diagnostic;
      EXPECT_EQ(R->TerminationSignal, 6u);
      EXPECT_FALSE(R->ReturnValue);
      EXPECT_EQ(R->StandardError, Message + "\n");
      EXPECT_EQ(R->TextLogWritesConsumed, 1u);
      ASSERT_EQ(R->MemorySnapshots.size(), 1u);
      const auto &Bytes = R->MemorySnapshots[0].Bytes;
      EXPECT_EQ(word(Bytes, 0), 1u);
      EXPECT_EQ(word(Bytes, 2 * 8), uint64_t(-6));
      EXPECT_EQ(word(Bytes, 5 * 8), 55u);
      EXPECT_EQ(word(Bytes, 17 * 8), 0u);
      EXPECT_EQ(
          std::vector<uint8_t>(Bytes.begin() + 24000, Bytes.begin() + 28096),
          abortRecord(Message));
      auto Expected = Options.Android->Memory[0].Bytes;
      std::copy(C.Prefix.begin(), C.Prefix.end(), Expected.begin() + 12000);
      EXPECT_EQ(
          std::vector<uint8_t>(Bytes.begin() + 12000, Bytes.begin() + 12016),
          std::vector<uint8_t>(Expected.begin() + 12000,
                               Expected.begin() + 12016));
      unsigned Calls = 0;
      for (const auto &Call : R->NativeCalls)
        if (Call.Name == FortifySymbols[C.Function]) {
          ++Calls;
          EXPECT_FALSE(Call.Result);
          EXPECT_EQ(Call.Library, Dynamic ? "libc.so" : "");
        }
      EXPECT_EQ(Calls, 1u);
    }
}

TEST_P(AndroidLog, FortifyTransportFallbackErrnoAndSharedAbortHandlers) {
  for (unsigned Mode = 0; Mode != 7; ++Mode)
    for (unsigned Handling : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Handling);
      const bool Child = (Mode & 1) != 0;
      const unsigned Descriptor = Mode == 4 || Mode == 6 ? 1
                                  : Mode == 5            ? 2
                                                         : 0;
      const auto Message = prepareFortify(
          9, 1 | (Handling << 8) | (Descriptor << 16) | (Child ? 1u << 24 : 0),
          {1, 1, 4, 3}, "strncpy: prevented 4-byte write into 3-byte buffer");
      auto &Writes = *Options.Android->TextLogWrites;
      if (Mode == 1)
        Writes[0].Result = -11;
      if (Mode == 2) {
        auto Last = Writes[0];
        Writes[0].Result = -4;
        Writes.push_back(Last);
      }
      if (Mode == 3 || Mode == 6) {
        Writes[0].OpenErrno = 2;
        Writes[0].Result = Mode == 6 ? -1 : Message.size() + 7;
      }
      auto R =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, Handling == 3 || Handling == 7
                             ? ProcessStopReason::Exited
                         : Handling == 4 ? ProcessStopReason::Returned
                                         : ProcessStopReason::Signaled)
          << R->Diagnostic;
      EXPECT_EQ(R->TextLogWritesConsumed, Mode == 2 ? 2u : 1u);
      const std::string Error =
          Descriptor
              ? ""
              : Message + "\n" + (Mode == 3 ? "libc: " + Message + "\n" : "");
      EXPECT_EQ(R->StandardError, Error);
      EXPECT_EQ(R->StandardOutput, Mode == 5 ? Message + "\n" : "");
      ASSERT_EQ(R->MemorySnapshots.size(), 1u);
      const auto &Bytes = R->MemorySnapshots[0].Bytes;
      EXPECT_EQ(word(Bytes, 17 * 8), Handling == 4 ? 99u : 0u);
      if (Handling >= 2) {
        EXPECT_EQ(word(Bytes, 8 * 8), Child ? 1001u : 1000u);
        const uint64_t ErrorNumber = Mode == 1                ? 11
                                     : Mode == 2              ? 4
                                     : Mode == 3              ? 2
                                     : Mode == 4 || Mode == 6 ? 9
                                                              : 55;
        EXPECT_EQ(word(Bytes, 5 * 8), ErrorNumber);
      }
    }
}

TEST_P(AndroidLog, AsyncSafeRecordsDoNotCompleteLiblogWrites) {
  prepareAssertion(0, 0, "message");
  auto &Entry = Options.Android->TextLogWrites->front();
  Entry.AsyncSafe = true;
  Entry.Result = 1;
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::UnsupportedService) << R->Diagnostic;
  EXPECT_EQ(R->StandardError, "message\n");
  EXPECT_EQ(R->TextLogWritesConsumed, 0u);
  ASSERT_EQ(R->MemorySnapshots.size(), 1u);
  EXPECT_NE(word(R->MemorySnapshots[0].Bytes, 2048), 0u);
  EXPECT_FALSE(R->TerminationSignal);
}

TEST_P(AndroidLog, FortifyStopsBeforeEveryUndeclaredOrFaultingEffect) {
  for (unsigned Mode = 0; Mode != 9; ++Mode) {
    SCOPED_TRACE(Mode);
    const auto Message =
        prepareFortify(0, 0, {1, 1, 4, 3},
                       "memcpy: prevented 4-byte write into 3-byte buffer");
    auto &Writes = *Options.Android->TextLogWrites;
    if (Mode == 0)
      Options.Android->TextLogWrites.reset();
    if (Mode == 1)
      Writes.clear();
    if (Mode == 2)
      Writes[0].AsyncSafe = false;
    if (Mode == 3)
      Writes[0].Payload.back() = 'x';
    if (Mode == 4)
      Options.Android->AbortMessageSlot.reset();
    if (Mode == 5)
      Options.LinuxSignals.reset();
    if (Mode == 6)
      Options.Android->AbortMessageSlot = Output + 32768;
    if (Mode == 7) {
      Options.OutputLimit = 1;
      Options.Android->ReadMemory.clear();
    }
    if (Mode == 8) {
      Writes[0].OpenErrno = 2;
      Writes[0].Result = 0;
    }
    // Keep a well-formed but mismatching payload for the admission case.
    if (Mode == 3) {
      Writes[0].Payload[6] = 'X';
      Writes[0].Payload.back() = 0;
    }
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, Mode == 6   ? ProcessStopReason::RuntimeFailure
                       : Mode == 7 ? ProcessStopReason::OutputLimit
                                   : ProcessStopReason::UnsupportedService)
        << R->Diagnostic;
    EXPECT_EQ(R->StandardError,
              Mode == 7 ? ""
                        : Message + "\n" +
                              (Mode == 8 ? "libc: " + Message + "\n" : ""));
    EXPECT_FALSE(R->TerminationSignal);
    if (Mode != 0)
      EXPECT_EQ(R->TextLogWritesConsumed, Mode >= 4 && Mode <= 6 ? 1u : 0u);
    if (Mode != 7) {
      ASSERT_EQ(R->MemorySnapshots.size(), 1u);
      EXPECT_EQ(word(R->MemorySnapshots[0].Bytes, 2048) != 0, Mode == 5);
    }
  }
}

TEST_P(AndroidLog, EventWritePayloadLimitsAndFaultsAreExplicit) {
  Options.Android->EntrySymbol = "log_event_write";
  for (uint64_t Size :
       {uint64_t(0), uint64_t(4064), uint64_t(4065), UINT64_MAX}) {
    std::vector<uint8_t> Payload{0xf9, 0xff, 0xff, 0xff};
    Payload.resize(4 + std::min<uint64_t>(Size, 4064), 0xc7);
    Options.Android->EventLogWrites = {{{Payload, int32_t(Payload.size())}}};
    Options.Android->Arguments = {Output, Size ? Output + 32768 - 4064 : 0,
                                  Size, 1, 0};
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
    EXPECT_EQ(R->EventLogWritesConsumed, 1u);
  }
  Options.Android->Arguments = {Output, 1, 1, 1, 0};
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_EQ(R->EventLogWritesConsumed, 0u);
}

TEST_P(AndroidLog, EventWriteDirectInputsUseSharedAdmission) {
  for (unsigned Mode = 0; Mode != 4; ++Mode) {
    Options.Android->EventLogWrites = {{{{0, 0, 0, 0}, 0}}};
    auto &Writes = *Options.Android->EventLogWrites;
    if (Mode == 0)
      Writes.front().Payload.resize(3);
    if (Mode == 1)
      Writes.front().Payload.resize(4069);
    if (Mode == 2)
      Writes.resize(4097, Writes.front());
    if (Mode == 3)
      Writes.assign(258, NativeEventLogWrite{std::vector<uint8_t>(4068), 0});
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_FALSE(bool(R)) << Mode;
    EXPECT_NE(llvm::toString(R.takeError()).find("Android event log"),
              std::string::npos);
  }
  Options.Android->EventLogWrites = std::vector<NativeEventLogWrite>(
      4096, NativeEventLogWrite{{0, 0, 0, 0}, 0});
  Options.Android->EntrySymbol = "log_event_write";
  Options.Android->Arguments = {Output, 0, 0, 0, 0};
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
  EXPECT_EQ(R->EventLogWritesConsumed, 0u);
}

TEST(AndroidLogOptions, TextWriteSchemaKeepsStrictBytesBuffersAndResults) {
  auto parse = [](const std::string &items) {
    return processOptionsFromJSON("{\"android\":{\"text_log_writes\":" + items +
                                  "}}");
  };
  auto absent = processOptionsFromJSON(R"({"android":{}})");
  ASSERT_TRUE(bool(absent));
  EXPECT_FALSE(absent->Android->TextLogWrites);
  auto empty = parse("[]");
  ASSERT_TRUE(bool(empty));
  ASSERT_TRUE(empty->Android->TextLogWrites);
  EXPECT_TRUE(empty->Android->TextLogWrites->empty());
  for (unsigned id : {0u, 1u, 3u, 4u}) {
    auto valid = parse("[{\"log_id\":" + std::to_string(id) +
                       ",\"payload\":\"06007800\",\"result\":-2147483648}]");
    ASSERT_TRUE(bool(valid)) << llvm::toString(valid.takeError());
    const auto &write = valid->Android->TextLogWrites->front();
    EXPECT_EQ(write.ID, id);
    EXPECT_EQ(write.Payload, (std::vector<uint8_t>{6, 0, 'x', 0}));
    EXPECT_EQ(write.Result, INT32_MIN);
  }
  for (const char *invalid :
       {"null", "{}", "[{}]", R"([{"payload":"060000","result":0}])",
        R"([{"log_id":2,"payload":"060000","result":0}])",
        R"([{"log_id":-1,"payload":"060000","result":0}])",
        R"([{"log_id":4294967296,"payload":"060000","result":0}])",
        R"([{"log_id":0,"payload":"0600","result":0}])",
        R"([{"log_id":0,"payload":"060001","result":0}])",
        R"([{"log_id":0,"payload":"06000000","result":0}])",
        R"([{"log_id":0,"payload":"060z00","result":0}])",
        R"([{"log_id":0,"payload":"060000","result":2147483648}])",
        R"([{"log_id":0,"payload":"060000","result":0,"extra":0}])"}) {
    auto result = parse(invalid);
    EXPECT_FALSE(bool(result)) << invalid;
    llvm::consumeError(result.takeError());
  }
}

TEST(AndroidLogOptions, AbortMessageSlotPreservesAbsenceNullAndGuestBits) {
  auto Absent = processOptionsFromJSON("{\"android\":{}}");
  ASSERT_TRUE(bool(Absent)) << llvm::toString(Absent.takeError());
  EXPECT_FALSE(Absent->Android->AbortMessageSlot);
  for (const char *Value : {"0", "\"0\"", "\"0xffffffffffffffff\"", "4097"}) {
    auto Result = processOptionsFromJSON(
        std::string("{\"android\":{\"abort_message_slot\":") + Value + "}}");
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_TRUE(Result->Android->AbortMessageSlot);
    EXPECT_EQ(*Result->Android->AbortMessageSlot,
              std::string(Value) == "\"0xffffffffffffffff\"" ? UINT64_MAX
              : std::string(Value) == "4097"                 ? 4097u
                                                             : 0u);
  }
  for (const char *Value :
       {"null", "true", "-1", "1.25", "{}", "[]", "\"bad\""}) {
    auto Result = processOptionsFromJSON(
        std::string("{\"android\":{\"abort_message_slot\":") + Value + "}}");
    EXPECT_FALSE(bool(Result)) << Value;
    if (!Result)
      llvm::consumeError(Result.takeError());
  }
}

TEST(AndroidLogOptions, AsyncSafeTransportRequiresExplicitTypedOutcomes) {
  for (const char *Extra :
       {",\"async_safe\":true", ",\"async_safe\":true,\"open_errno\":2",
        ",\"async_safe\":false"}) {
    auto R = processOptionsFromJSON(
        std::string("{\"android\":{\"text_log_writes\":[{\"log_id\":4,"
                    "\"payload\":\"076c696263007800\",\"result\":1") +
        Extra + "}]}}");
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Android->TextLogWrites->front().AsyncSafe,
              llvm::StringRef(Extra).contains("true"));
  }
  for (const char *Extra : {",\"async_safe\":1", ",\"open_errno\":2",
                            ",\"async_safe\":true,\"open_errno\":4",
                            ",\"async_safe\":true,\"open_errno\":4096",
                            ",\"async_safe\":true,\"open_errno\":-1",
                            ",\"async_safe\":true,\"unknown\":0"}) {
    auto R = processOptionsFromJSON(
        std::string("{\"android\":{\"text_log_writes\":[{\"log_id\":4,"
                    "\"payload\":\"076c696263007800\",\"result\":1") +
        Extra + "}]}}");
    EXPECT_FALSE(bool(R)) << Extra;
    if (!R)
      llvm::consumeError(R.takeError());
  }
}

TEST(AndroidLogOptions, TextTransportAcceptsOnlyCompleteOrFullSizePrefixes) {
  for (unsigned Size : {4067u, 4068u, 4069u})
    for (unsigned Shape = 0; Shape != 6; ++Shape) {
      SCOPED_TRACE(Size);
      SCOPED_TRACE(Shape);
      std::string Payload(Size, 'x');
      Payload[0] = 0; // A zero priority byte is independent of the terminators.
      if (Shape == 1)
        Payload.back() = 0; // Tag ends at the transport boundary.
      if (Shape >= 2)
        Payload[1] = 0;
      if (Shape == 3 || Shape == 5)
        Payload.back() = 0;
      if (Shape >= 4)
        Payload[2] = 0; // Trailing data after message NUL or a third NUL.
      auto Result = processOptionsFromJSON(
          "{\"android\":{\"text_log_writes\":[{\"log_id\":0,\"payload\":\"" +
          llvm::toHex(Payload) + "\",\"result\":-11}]}}");
      const bool Accepted =
          Size <= 4068 && (Shape == 3 || (Size == 4068 && Shape < 3));
      EXPECT_EQ(bool(Result), Accepted);
      if (Result)
        EXPECT_EQ(Result->Android->TextLogWrites->front().Payload,
                  std::vector<uint8_t>(Payload.begin(), Payload.end()));
      else
        llvm::consumeError(Result.takeError());
    }
}

TEST(AndroidLogOptions, EventWriteSchemaPreservesAbsenceBytesAndSignedResults) {
  auto Parse = [](const std::string &Items) {
    return processOptionsFromJSON(
        "{\"android\":{\"event_log_writes\":" + Items + "}}");
  };
  auto Absent = processOptionsFromJSON(R"({"android":{}})");
  ASSERT_TRUE(bool(Absent));
  EXPECT_FALSE(Absent->Android->EventLogWrites);
  auto Empty = Parse("[]");
  ASSERT_TRUE(bool(Empty));
  ASSERT_TRUE(Empty->Android->EventLogWrites);
  EXPECT_TRUE(Empty->Android->EventLogWrites->empty());
  for (int64_t Result :
       {int64_t(INT32_MIN), int64_t(-11), int64_t(0), int64_t(INT32_MAX)}) {
    auto Good = Parse("[{\"payload\":\"008000ff\",\"result\":" +
                      std::to_string(Result) + "}]");
    ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
    ASSERT_EQ(Good->Android->EventLogWrites->size(), 1u);
    EXPECT_EQ(Good->Android->EventLogWrites->front().Result, Result);
    EXPECT_EQ(Good->Android->EventLogWrites->front().Payload,
              (std::vector<uint8_t>{0, 128, 0, 255}));
  }
  for (const char *Bad :
       {"null", "true", "{}", "[{}]", "[1]", R"([{"payload":"00000000"}])",
        R"([{"result":0}])", R"([{"payload":"00000000","result":0,"extra":1}])",
        R"([{"payload":"000000","result":0}])",
        R"([{"payload":"0000000z","result":0}])",
        R"([{"payload":"000000000","result":0}])",
        R"([{"payload":"00000000","result":2147483648}])",
        R"([{"payload":"00000000","result":-2147483649}])",
        R"([{"payload":"00000000","result":false}])",
        R"([{"payload":"00000000","result":1.5}])"}) {
    auto R = Parse(Bad);
    EXPECT_FALSE(bool(R)) << Bad;
    llvm::consumeError(R.takeError());
  }
  auto Long =
      Parse("[{\"payload\":\"" + std::string(8138, '0') + "\",\"result\":0}]");
  EXPECT_FALSE(bool(Long));
  llvm::consumeError(Long.takeError());
}

TEST(AndroidLogOptions, EventWriteSchemaBoundsCountAndAggregateBytes) {
  for (auto [Count, Size, Accepted] :
       {std::tuple{2048u, 4u, true}, std::tuple{4096u, 4u, false},
        std::tuple{4097u, 4u, false}, std::tuple{257u, 4068u, true},
        std::tuple{258u, 4068u, false}}) {
    const auto Entry =
        "{\"payload\":\"" + std::string(Size * 2, '0') + "\",\"result\":0}";
    std::string Input = "{\"android\":{\"event_log_writes\":[";
    for (unsigned I = 0; I != Count; ++I) {
      if (I)
        Input += ',';
      Input += Entry;
    }
    Input += "]}}";
    auto Result = processOptionsFromJSON(Input);
    const bool Valid = bool(Result);
    const auto Diagnostic =
        Valid ? std::string() : llvm::toString(Result.takeError());
    EXPECT_EQ(Valid, Accepted) << Count << " x " << Size << ": " << Diagnostic;
    if (Count == 4096)
      EXPECT_NE(Diagnostic.find("64 KiB"), std::string::npos);
  }
}

TEST(AndroidLogOptions, StrictFieldsBoundsOrderAndAbsentVersusEmpty) {
  auto Empty = processOptionsFromJSON(R"({"android":{"log_records":[]}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  ASSERT_TRUE(Empty->Android->LogRecords);
  EXPECT_TRUE(Empty->Android->LogRecords->empty());
  auto Absent = processOptionsFromJSON(R"({"android":{}})");
  ASSERT_TRUE(bool(Absent)) << llvm::toString(Absent.takeError());
  EXPECT_FALSE(Absent->Android->LogRecords);
  std::string Record =
      R"({"log_id":1,"pid":42,"tid":9,"seconds":20,"nanoseconds":7,"uid":10,"payload":"00ff"})";
  auto Parse = [](const std::string &Items) {
    return processOptionsFromJSON("{\"android\":{\"log_records\":" + Items +
                                  "}}");
  };
  auto Good = Parse("[" + Record + "]");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ((*Good->Android->LogRecords)[0].Payload,
            (std::vector<uint8_t>{0, 255}));
  for (const char *Bad : {"null", "true", "{}", "[{}]", "[1]"}) {
    auto R = Parse(Bad);
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  for (auto [Before, After] : std::vector<std::pair<std::string, std::string>>{
           {"\"log_id\":1", "\"log_id\":6"},
           {"\"pid\":42", "\"pid\":2147483648"},
           {"\"tid\":9", "\"tid\":4294967296"},
           {"\"nanoseconds\":7", "\"nanoseconds\":1000000000"},
           {"\"uid\":10", "\"uid\":-1"},
           {"\"payload\":\"00ff\"", "\"payload\":\"xyz\""},
           {"\"log_id\":1", "\"wrong\":1"}}) {
    auto Bad = Record;
    Bad.replace(Bad.find(Before), Before.size(), After);
    auto R = Parse("[" + Bad + "]");
    EXPECT_FALSE(bool(R)) << Bad;
    llvm::consumeError(R.takeError());
  }
  auto Earlier = Record;
  Earlier.replace(Earlier.find("\"seconds\":20"), 12, "\"seconds\":19");
  auto Reversed = Parse("[" + Record + "," + Earlier + "]");
  EXPECT_FALSE(bool(Reversed));
  llvm::consumeError(Reversed.takeError());
}
INSTANTIATE_TEST_SUITE_P(
    Relocations, AndroidLog,
    testing::Combine(testing::Values("log-O0-none", "log-O0-android",
                                     "log-O0-relr", "log-O2-none",
                                     "log-O2-android", "log-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF)));
} // namespace
} // namespace neverd::emulation
