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

#include <tuple>

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
    if (mode == 3)
      Options.Android->Arguments[1] = 2;
    if (mode == 4)
      Options.Android->TextLogWrites->front().ID = 2;
    if (mode == 5)
      Options.Android->TextLogWrites->front().Payload.back() = 'x';
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
    EXPECT_EQ(result->Stop, mode == 0 ? ProcessStopReason::Returned
                                      : ProcessStopReason::UnsupportedService)
        << result->Diagnostic;
    EXPECT_EQ(result->TextLogWritesConsumed, mode == 0 ? 1u : 0u);
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
