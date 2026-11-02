//===- AndroidServiceManagerTests.cpp - Shared service cache state --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// ServiceManager initialization and explicit Binder identities for JNI/DEX.
//===----------------------------------------------------------------------===//
#include "../../lib/dalvik/AndroidFramework.h"
#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <bit>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
constexpr auto Manager = "Landroid/os/ServiceManager;";
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
using Snapshot = AndroidApplicationSnapshot;
using ServiceState = Snapshot::ServiceManager;
Snapshot emptyCache() {
  Snapshot input;
  input.service_manager.emplace().cache.emplace();
  input.service_manager->manager_proxy.emplace(std::nullopt);
  return input;
}
struct State {
  Program program{{}, std::map<std::string, std::string>{}};
  std::shared_ptr<Heap> heap = std::make_shared<Heap>();
  unsigned properties = 0, clocks = 0;
  std::optional<int32_t> slow_ms;
  unsigned uid_reads = 0, uptime_reads = 0;
  uint64_t uid = 10000, boot = 50987654321, step = 0, uptime = 2000;
  ExecutionStop uid_stop = ExecutionStop::Returned;
  std::vector<std::vector<uint8_t>> logs;
  uint64_t log_result = 0;
  ExecutionStop log_stop = ExecutionStop::Returned;
  unsigned stop_log = 0;
  RuntimeServices services;
  std::unique_ptr<Runtime> runtime;
  explicit State(Snapshot input = emptyCache(), bool logging = false) {
    services.property_integer = [&](std::string_view key, int32_t value) {
      ++properties;
      if (key == "debug.servicemanager.slow_call_ms" && slow_ms)
        value = *slow_ms;
      return ExecutionResult{ExecutionStop::Returned, uint32_t(value)};
    };
    services.elapsed_realtime_nanoseconds = [&] {
      ++clocks;
      auto value = boot;
      boot += step;
      return ExecutionResult{ExecutionStop::Returned, value};
    };
    services.process_uid = [&] {
      ++uid_reads;
      return ExecutionResult{uid_stop, uid};
    };
    services.uptime_milliseconds = [&] {
      ++uptime_reads;
      return ExecutionResult{ExecutionStop::Returned, uptime};
    };
    if (logging)
      services.event_log_write = [&](std::span<const uint8_t> payload) {
        logs.emplace_back(payload.begin(), payload.end());
        return ExecutionResult{
            logs.size() == stop_log ? log_stop : ExecutionStop::Returned,
            log_result};
      };
    runtime = std::make_unique<Runtime>(program, heap, input, services);
  }
  ExecutionResult call(MethodRef method, bool is_static, uint64_t receiver = 0,
                       std::initializer_list<uint64_t> args = {}) {
    const auto *entry = libraryMethod(method, is_static);
    EXPECT_NE(entry, nullptr) << method.identity();
    return entry ? runtime->library(*entry, receiver, args, Admit)
                 : ExecutionResult{ExecutionStop::Invalid};
  }
  ExecutionResult lookup(const char *method, uint64_t key) {
    return call(
        {Manager, method, {"Ljava/lang/String;"}, "Landroid/os/IBinder;"}, true,
        0, {key});
  }
  uint64_t field(const char *name, const char *type) {
    const auto *entry = program.findField({Manager, name, type}, true);
    EXPECT_NE(entry, nullptr);
    if (!entry)
      return 0;
    auto result = runtime->field(*entry, 0, {}, Admit, 65536);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  }
  void set(const char *name, const char *type, uint64_t value) {
    const auto *entry = program.findField({Manager, name, type}, true);
    ASSERT_NE(entry, nullptr);
    auto result = runtime->field(*entry, 0, value, Admit, 65536);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  }
};

Snapshot lookupReplies() {
  auto input = emptyCache();
  auto &config = *input.service_manager;
  config.binder_proxies = {{7, true}, {8, true}};
  config.context_object.emplace(7);
  using Query = ServiceState::LookupResult;
  config.lookup_results = {{Query::Operation::Get, 7, "alpha", 8},
                           {Query::Operation::Check, 7, "alpha", 8},
                           {Query::Operation::Get, 7, "missing", {}},
                           {Query::Operation::Check, 7, std::nullopt, {}}};
  return input;
}

TEST(AndroidServiceManagerTest, ExplicitRepliesRetainAliasesWithoutCaching) {
  State s(lookupReplies());
  s.step = 25000;
  const auto key = s.heap->literal("alpha")->Identity;
  const auto first = s.lookup("getService", key);
  ASSERT_EQ(first.stop, ExecutionStop::Returned) << first.diagnostic;
  ASSERT_NE(first.value, 0u);
  for (const auto *method : {"getService", "checkService"}) {
    auto again = s.lookup(method, s.heap->literal("alpha")->Identity);
    ASSERT_EQ(again.stop, ExecutionStop::Returned) << again.diagnostic;
    EXPECT_EQ(again.value, first.value);
  }
  auto absent = s.lookup("getService", s.heap->literal("missing")->Identity);
  ASSERT_EQ(absent.stop, ExecutionStop::Returned) << absent.diagnostic;
  EXPECT_EQ(absent.value, 0u);
  auto null_name = s.lookup("checkService", 0);
  ASSERT_EQ(null_name.stop, ExecutionStop::Returned) << null_name.diagnostic;
  EXPECT_EQ(null_name.value, 0u);
  EXPECT_EQ(s.field("sGetServiceAccumulatedCallCount", "I"), 3u);
  EXPECT_EQ(s.field("sGetServiceAccumulatedUs", "I"), 75u);
  EXPECT_EQ(s.clocks, 10u); // Initialization and three timed lookups.
  EXPECT_EQ(s.uid_reads, 3u);
  EXPECT_EQ(s.uptime_reads, 3u);
  const auto logger = s.heap->get(
      s.field("sStatLogger", "Lcom/android/internal/util/StatLogger;"));
  const auto count = s.heap->get(logger->Fields.at(s.program.findField(
      {"Lcom/android/internal/util/StatLogger;", "mCountStats", "[I"}, false)));
  EXPECT_EQ(llvm::support::endian::read32le(count->Bytes.data()), 3u);
  const auto state = s.runtime->snapshot().framework.service_manager;
  ASSERT_TRUE(state);
  EXPECT_TRUE(state->entries.empty());
  ASSERT_EQ(state->binder_proxies.size(), 2u);
  for (const auto &proxy : state->binder_proxies)
    EXPECT_EQ(proxy.warn_on_blocking, false);
}

TEST(AndroidServiceManagerTest, ReplyMatchingRequiresMethodManagerAndJavaName) {
  for (unsigned mode = 0; mode != 4; ++mode) {
    auto input = lookupReplies();
    input.service_manager->lookup_results.resize(1);
    auto &reply = input.service_manager->lookup_results[0];
    if (mode == 1)
      reply.operation = ServiceState::LookupResult::Operation::Check;
    if (mode == 2)
      reply.manager = 8;
    if (mode == 3)
      reply.name = std::nullopt;
    if (!mode)
      reply.name = std::string("a\0b", 3);
    State s(input);
    const auto key = s.heap->literal(std::string("a\0b", 3))->Identity;
    auto result = s.lookup("getService", key);
    EXPECT_EQ(result.stop,
              mode ? ExecutionStop::Unsupported : ExecutionStop::Returned)
        << result.diagnostic;
    if (mode) {
      EXPECT_NE(result.diagnostic.find("transaction result is not supplied"),
                std::string::npos);
      EXPECT_EQ(s.uid_reads, 0u);
      EXPECT_TRUE(
          s.runtime->snapshot().framework.service_manager->entries.empty());
    }
  }
}

TEST(AndroidServiceManagerTest, SharedUidSelectsCoreThresholdAndLogBoundary) {
  for (uint64_t uid : {0u, 9999u, 10000u, 100999u, 110000u, UINT32_MAX}) {
    State s(lookupReplies());
    s.uid = uid;
    s.step =
        20000000; // 20 ms: above the core threshold, below the app threshold.
    auto result = s.lookup("getService", s.heap->literal("alpha")->Identity);
    const bool core = uid == 0 || uid == 9999 || uid == 100999;
    EXPECT_EQ(result.stop,
              core ? ExecutionStop::Unsupported : ExecutionStop::Returned)
        << result.diagnostic;
    if (core)
      EXPECT_EQ(result.diagnostic,
                "Android event log write service is not supplied");
    EXPECT_EQ(s.uid_reads, 1u);
    EXPECT_EQ(s.uptime_reads, 1u);
  }
}

TEST(AndroidServiceManagerTest, UidAndUptimeFailuresKeepEarlierStatistics) {
  for (unsigned mode = 0; mode != 3; ++mode) {
    auto input = lookupReplies();
    State s(input);
    if (mode == 0)
      s.services.process_uid = {};
    if (mode == 1)
      s.uid_stop = ExecutionStop::Timeout;
    if (mode == 2)
      s.services.uptime_milliseconds = {};
    s.runtime = std::make_unique<Runtime>(s.program, s.heap, input, s.services);
    auto result = s.lookup("getService", s.heap->literal("alpha")->Identity);
    EXPECT_EQ(result.stop,
              mode == 1 ? ExecutionStop::Timeout : ExecutionStop::Unsupported);
    const auto state = s.runtime->snapshot();
    EXPECT_TRUE(state.terminal);
    const auto logger =
        s.heap->get(state.framework.service_manager->stat_logger);
    auto counts = s.heap->get(logger->Fields.at(s.program.findField(
        {"Lcom/android/internal/util/StatLogger;", "mCountStats", "[I"},
        false)));
    EXPECT_EQ(llvm::support::endian::read32le(counts->Bytes.data()), 1u);
    EXPECT_EQ(s.clocks, 4u);
    EXPECT_EQ(s.uid_reads, mode ? 1u : 0u);
    EXPECT_EQ(s.uptime_reads, 0u);
  }
}

TEST(AndroidServiceManagerTest, JavaIntWrappingAndStatsLogBoundaryAreRetained) {
  for (bool logging : {false, true}) {
    State s(lookupReplies());
    ASSERT_EQ(s.runtime->initialize(Manager, Admit, 65536).stop,
              ExecutionStop::Returned);
    auto count = s.program.findField(
        {Manager, "sGetServiceAccumulatedCallCount", "I"}, true);
    ASSERT_NE(count, nullptr);
    ASSERT_EQ(
        s.runtime->field(*count, 0, logging ? 199u : UINT32_MAX, Admit, 65536)
            .stop,
        ExecutionStop::Returned);
    s.uptime = logging ? 5000 : 2000;
    if (!logging)
      s.step = (uint64_t(UINT32_MAX) + 2) * 1000;
    auto result = s.lookup("getService", s.heap->literal("alpha")->Identity);
    EXPECT_EQ(result.stop,
              logging ? ExecutionStop::Unsupported : ExecutionStop::Returned)
        << result.diagnostic;
    if (logging)
      EXPECT_EQ(result.diagnostic,
                "Android event log write service is not supplied");
    else {
      EXPECT_EQ(s.field("sGetServiceAccumulatedCallCount", "I"), 0u);
      EXPECT_EQ(s.field("sGetServiceAccumulatedUs", "I"), 1u);
    }
  }
}

TEST(AndroidServiceManagerTest,
     EventLogsCommitInOrderEvenWhenLiblogReturnsAnError) {
  for (uint32_t returned : {0u, 21u, uint32_t(-11), uint32_t(INT32_MIN)}) {
    State s(lookupReplies(), true);
    s.log_result = returned;
    s.step = 20000000;
    s.uptime = 5000;
    s.slow_ms = 0;
    s.set("sGetServiceAccumulatedCallCount", "I", 199);
    s.set("sGetServiceAccumulatedUs", "I", 500);
    auto result = s.lookup("getService", s.heap->literal("alpha")->Identity);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    ASSERT_EQ(s.logs.size(), 2u);
    EXPECT_EQ(s.logs[0], (std::vector<uint8_t>{
                             0x71, 0x82, 3, 0, 3, 2,   0,   20,  0,   0,  0,
                             2,    5,    0, 0, 0, 'a', 'l', 'p', 'h', 'a'}));
    EXPECT_EQ(s.logs[1],
              (std::vector<uint8_t>{0x70, 0x82, 3, 0, 3, 3, 0,    200,  0, 0, 0,
                                    0,    20,   0, 0, 0, 0, 0x88, 0x13, 0, 0}));
    EXPECT_EQ(s.field("sLastSlowLogUptime", "J"), 5000u);
    EXPECT_EQ(s.field("sLastSlowLogActualTime", "J"), 20000u);
    EXPECT_EQ(s.field("sLastStatsLogUptime", "J"), 5000u);
    EXPECT_EQ(s.field("sGetServiceAccumulatedCallCount", "I"), 0u);
    EXPECT_EQ(s.field("sGetServiceAccumulatedUs", "I"), 0u);
    for (const auto &proxy :
         s.runtime->snapshot().framework.service_manager->binder_proxies)
      EXPECT_EQ(proxy.warn_on_blocking, false);
  }
}

TEST(AndroidServiceManagerTest, SlowLogUsesStrictTimeOrGreaterDuration) {
  State s(lookupReplies(), true);
  s.step = 20000000;
  s.slow_ms = 20;
  const auto name = s.heap->literal("alpha")->Identity;
  for (uint64_t uptime : {2000u, 7000u, 7001u, 7001u}) {
    s.uptime = uptime;
    auto result = s.lookup("getService", name);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(s.logs.size(), uptime > 7000 ? 2u : 1u);
  }
  s.step = 21000000;
  ASSERT_EQ(s.lookup("getService", name).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.logs.size(), 3u);
  EXPECT_EQ(s.field("sLastSlowLogActualTime", "J"), 21000u);
  // Java long addition wraps before the signed comparison.
  s.set("sLastSlowLogUptime", "J", INT64_MAX);
  ASSERT_EQ(s.lookup("getService", name).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.logs.size(), 4u);
}

TEST(AndroidServiceManagerTest,
     StatsLogUsesInclusiveTimeAndJavaSignedArithmetic) {
  State s(lookupReplies(), true);
  const auto name = s.heap->literal("alpha")->Identity;
  s.uptime = 4999;
  s.set("sGetServiceAccumulatedCallCount", "I", 199);
  ASSERT_EQ(s.lookup("getService", name).stop, ExecutionStop::Returned);
  EXPECT_TRUE(s.logs.empty());
  s.uptime = (uint64_t(1) << 32) + 5000;
  s.set("sGetServiceAccumulatedUs", "I", uint32_t(-1500));
  ASSERT_EQ(s.lookup("getService", name).stop, ExecutionStop::Returned);
  ASSERT_EQ(s.logs.size(), 1u);
  EXPECT_EQ(s.logs[0], (std::vector<uint8_t>{
                           0x70, 0x82, 3,    0,    3,    3, 0,    201,  0, 0, 0,
                           0,    0xff, 0xff, 0xff, 0xff, 0, 0x88, 0x13, 0, 0}));
  EXPECT_EQ(s.field("sLastStatsLogUptime", "J"), s.uptime);
  EXPECT_EQ(s.field("sGetServiceAccumulatedCallCount", "I"), 0u);
  EXPECT_EQ(s.field("sGetServiceAccumulatedUs", "I"), 0u);
  s.uptime += 5000;
  s.set("sGetServiceAccumulatedCallCount", "I", UINT32_MAX);
  ASSERT_EQ(s.lookup("getService", name).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.logs.size(), 1u);
}

TEST(AndroidServiceManagerTest, SlowLogKeepsNullAndARTStringBytesDistinct) {
  const std::optional<std::string> names[] = {
      std::nullopt, "", std::string("a\0b", 3), "\xf0\x9f\x98\x80\xed\xa0\x80"};
  const std::vector<uint8_t> bytes[] = {
      {'N', 'U', 'L', 'L'},
      {},
      {'a', 0xc0, 0x80, 'b'},
      {0xf0, 0x9f, 0x98, 0x80, 0xed, 0xa0, 0x80}};
  for (unsigned i = 0; i != 4; ++i) {
    auto input = lookupReplies();
    input.service_manager->lookup_results = {
        {ServiceState::LookupResult::Operation::Get, 7, names[i], {}}};
    State s(input, true);
    s.uptime = 5001;
    s.slow_ms = 0;
    auto result = s.lookup("getService",
                           names[i] ? s.heap->literal(*names[i])->Identity : 0);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    ASSERT_EQ(s.logs.size(), 1u);
    std::vector<uint8_t> expected{
        0x71, 0x82, 3, 0, 3, 2, 0, 0, 0, 0, 0, 2, uint8_t(bytes[i].size()),
        0,    0,    0};
    expected.insert(expected.end(), bytes[i].begin(), bytes[i].end());
    EXPECT_EQ(s.logs[0], expected);
  }
}

TEST(AndroidServiceManagerTest,
     EventLogPayloadCapacityAndResourceStopsAreExplicit) {
  for (unsigned size : {4052u, 4053u}) {
    auto input = lookupReplies();
    input.service_manager->lookup_results[0].name = std::string(size, 'x');
    State s(input, true);
    s.uptime = 5001;
    s.slow_ms = 0;
    auto result = s.lookup("getService",
                           s.heap->literal(std::string(size, 'x'))->Identity);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    ASSERT_EQ(s.logs.size(), 1u);
    EXPECT_EQ(s.logs[0].size(), 4068u);
    EXPECT_EQ(llvm::support::endian::read32le(s.logs[0].data() + 12), 4052u);
    EXPECT_TRUE(std::all_of(s.logs[0].begin() + 16, s.logs[0].end(),
                            [](uint8_t byte) { return byte == 'x'; }));
  }
  for (ExecutionStop stop :
       {ExecutionStop::Timeout, ExecutionStop::InstructionLimit,
        ExecutionStop::Unsupported, ExecutionStop::HeapLimit}) {
    for (unsigned index : {1u, 2u}) {
      State s(lookupReplies(), true);
      s.step = 20000000;
      s.uptime = 5000;
      s.stop_log = index;
      s.log_stop = stop;
      s.slow_ms = 0;
      s.set("sGetServiceAccumulatedCallCount", "I", 199);
      auto result = s.lookup("getService", s.heap->literal("alpha")->Identity);
      EXPECT_EQ(result.stop, stop);
      EXPECT_EQ(s.logs.size(), index);
      auto state = s.runtime->snapshot();
      EXPECT_TRUE(state.terminal);
      ASSERT_EQ(state.framework.service_manager->binder_proxies.size(), 2u);
      // Context allowBlocking precedes lookup; result allowBlocking follows
      // logs.
      EXPECT_EQ(
          state.framework.service_manager->binder_proxies[0].warn_on_blocking,
          false);
      EXPECT_EQ(
          state.framework.service_manager->binder_proxies[1].warn_on_blocking,
          true);
    }
  }
}

TEST(AndroidServiceManagerTest, NegativeDurationsKeepSignedLogFields) {
  State s(lookupReplies(), true);
  s.slow_ms = -2;
  s.step = ((uint64_t(1) << 32) - 1500) * 1000;
  s.uptime = 6000;
  s.set("sGetServiceAccumulatedCallCount", "I", 199);
  auto result = s.lookup("getService", s.heap->literal("alpha")->Identity);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  ASSERT_EQ(s.logs.size(), 2u);
  EXPECT_EQ(llvm::support::endian::read32le(s.logs[0].data() + 7), UINT32_MAX);
  EXPECT_EQ(llvm::support::endian::read32le(s.logs[1].data() + 12), UINT32_MAX);
  EXPECT_EQ(s.field("sLastSlowLogActualTime", "J"), uint64_t(-1500));
  EXPECT_EQ(s.field("sGetServiceAccumulatedUs", "I"), 0u);
}

TEST(AndroidServiceManagerTest, LogEncodingHonorsWorkBudgetAndResultWidth) {
  auto input = lookupReplies();
  input.service_manager->lookup_results[0].name = std::string(4052, 'x');
  State s(input, true);
  s.slow_ms = 0;
  s.uptime = 5001;
  ASSERT_EQ(s.runtime->initialize(Manager, Admit, 65536).stop,
            ExecutionStop::Returned);
  const uint64_t args[] = {s.heap->literal(std::string(4052, 'x'))->Identity};
  const auto *method = libraryMethod(
      {Manager, "getService", {"Ljava/lang/String;"}, "Landroid/os/IBinder;"},
      true);
  unsigned work = 0;
  auto result = s.runtime->library(*method, 0, args, [&] {
    return ++work <= 100 ? ExecutionStop::Returned
                         : ExecutionStop::InstructionLimit;
  });
  EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
  EXPECT_TRUE(s.logs.empty());
  EXPECT_TRUE(s.runtime->snapshot().terminal);

  State invalid(lookupReplies(), true);
  invalid.slow_ms = 0;
  invalid.uptime = 5001;
  invalid.log_result = uint64_t(1) << 32;
  result =
      invalid.lookup("getService", invalid.heap->literal("alpha")->Identity);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("Java int carrier"), std::string::npos);
  EXPECT_EQ(invalid.logs.size(), 1u);
}

TEST(AndroidServiceManagerTest, LogStopsRetainOnlyEarlierFieldAssignments) {
  for (unsigned index : {1u, 2u}) {
    State s(lookupReplies(), true);
    s.slow_ms = 0;
    s.step = 20000000;
    s.uptime = 5000;
    s.stop_log = index;
    s.log_stop = ExecutionStop::Timeout;
    // Inspect the owning framework directly after the stop; Runtime's
    // independent terminal guard is tested above and must not be bypassed by
    // retrying an interrupted invocation.
    AndroidFramework framework(lookupReplies(), s.program, s.services);
    ASSERT_EQ(framework
                  .initializeServiceManager(*s.heap, s.program, Admit,
                                            [](std::string_view) {
                                              return ExecutionResult{
                                                  ExecutionStop::Returned};
                                            })
                  .stop,
              ExecutionStop::Returned);
    auto field = [&](const char *name, const char *type,
                     std::optional<uint64_t> write = {}) {
      return framework.field(*s.heap,
                             *s.program.findField({Manager, name, type}, true),
                             0, write, Admit);
    };
    ASSERT_EQ(field("sGetServiceAccumulatedCallCount", "I", 199).stop,
              ExecutionStop::Returned);
    ASSERT_EQ(field("sGetServiceAccumulatedUs", "I", 123).stop,
              ExecutionStop::Returned);
    const uint64_t args[] = {s.heap->literal("alpha")->Identity};
    const auto *method = libraryMethod(
        {Manager, "getService", {"Ljava/lang/String;"}, "Landroid/os/IBinder;"},
        true);
    auto result =
        framework.execute(*s.heap, s.program, *method, 0, args, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Timeout);
    EXPECT_EQ(field("sLastSlowLogUptime", "J").value, index == 2 ? 5000u : 0u);
    EXPECT_EQ(field("sLastSlowLogActualTime", "J").value,
              index == 2 ? 20000u : 0u);
    EXPECT_EQ(field("sLastStatsLogUptime", "J").value, 0u);
    EXPECT_EQ(field("sGetServiceAccumulatedCallCount", "I").value, 200u);
    EXPECT_EQ(field("sGetServiceAccumulatedUs", "I").value, 20123u);
  }
}

TEST(AndroidServiceManagerTest, TypedRepliesRejectAmbiguityAndDanglingKeys) {
  for (unsigned mode = 0; mode != 7; ++mode) {
    auto input = lookupReplies();
    auto &queries = input.service_manager->lookup_results;
    auto &query = queries[0];
    if (mode == 0)
      query.manager = 0;
    if (mode == 1)
      query.binder = 9;
    if (mode == 2)
      query.operation = static_cast<ServiceState::LookupResult::Operation>(2);
    if (mode == 3)
      query.name = std::string(4097, 'x');
    if (mode == 4)
      query.name = std::string("\xff", 1);
    if (mode == 5)
      queries.push_back(query);
    if (mode == 6)
      queries.resize(257);
    EXPECT_THROW(validateAndroidServiceManager(input), Error);
  }
}

TEST(AndroidServiceManagerTest, CacheAndBinderMethodsHaveSharedDeclarations) {
  for (const auto &method : {MethodRef{"Landroid/os/ServiceManager;",
                                       "getService",
                                       {"Ljava/lang/String;"},
                                       "Landroid/os/IBinder;"},
                             MethodRef{"Landroid/os/ServiceManager;",
                                       "checkService",
                                       {"Ljava/lang/String;"},
                                       "Landroid/os/IBinder;"},
                             MethodRef{"Landroid/os/ServiceManager;",
                                       "initServiceCache",
                                       {"Ljava/util/Map;"},
                                       "V"},
                             MethodRef{"Landroid/os/Binder;",
                                       "allowBlocking",
                                       {"Landroid/os/IBinder;"},
                                       "Landroid/os/IBinder;"}})
    EXPECT_NE(libraryMethod(method, true), nullptr) << method.identity();
}

TEST(AndroidServiceManagerTest,
     InitializerReusesPropertiesClockAndArrayStateOnce) {
  State s;
  EXPECT_EQ(s.field("GET_SERVICE_SLOW_THRESHOLD_US_CORE", "J"), 10000u);
  EXPECT_EQ(s.field("GET_SERVICE_SLOW_THRESHOLD_US_NON_CORE", "J"), 50000u);
  EXPECT_EQ(s.field("GET_SERVICE_LOG_EVERY_CALLS_CORE", "I"), 100u);
  EXPECT_EQ(s.field("GET_SERVICE_LOG_EVERY_CALLS_NON_CORE", "I"), 200u);
  EXPECT_EQ(s.properties, 4u);
  EXPECT_EQ(s.clocks, 1u);
  EXPECT_EQ(s.field("sGetServiceAccumulatedCallCount", "I"), 0u);
  const auto logger = s.heap->get(
      s.field("sStatLogger", "Lcom/android/internal/util/StatLogger;"));
  ASSERT_TRUE(logger);
  const auto *labels =
      s.program.findField({"Lcom/android/internal/util/StatLogger;", "mLabels",
                           "[Ljava/lang/String;"},
                          false);
  ASSERT_NE(labels, nullptr);
  auto array = s.heap->get(logger->Fields.at(labels));
  ASSERT_TRUE(array);
  ASSERT_EQ(array->Length, 1u);
  EXPECT_EQ(array->Elements[0].lock()->Bytes, javaStringBytes("getService()"));
  EXPECT_EQ(s.runtime->initialize(Manager, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.properties, 4u);
  EXPECT_EQ(s.clocks, 1u);
  ASSERT_TRUE(s.runtime->snapshot().framework.service_manager);
  EXPECT_TRUE(s.runtime->snapshot().framework.service_manager->initialized);
  for (const auto &row : s.runtime->snapshot().classes)
    if (row.descriptor == "Ljava/util/HashMap;" ||
        row.descriptor == "Lcom/android/internal/util/StatLogger;" ||
        row.descriptor == "Landroid/os/SystemProperties;")
      EXPECT_EQ(row.initialization,
                JavaClassState::Initialization::Initialized);
}

TEST(AndroidServiceManagerTest,
     ThresholdMultipliesIntBeforeSignedLongWidening) {
  State s;
  s.services.property_integer = [](std::string_view key, int32_t) {
    return ExecutionResult{ExecutionStop::Returned,
                           key.ends_with("slow_call_core_ms")
                               ? uint32_t(INT32_MAX)
                               : uint32_t(-3)};
  };
  s.runtime =
      std::make_unique<Runtime>(s.program, s.heap, emptyCache(), s.services);
  EXPECT_EQ(std::bit_cast<int64_t>(
                s.field("GET_SERVICE_SLOW_THRESHOLD_US_CORE", "J")),
            -1000);
  EXPECT_EQ(std::bit_cast<int64_t>(
                s.field("GET_SERVICE_SLOW_THRESHOLD_US_NON_CORE", "J")),
            -3000);
  EXPECT_EQ(s.field("GET_SERVICE_LOG_EVERY_CALLS_CORE", "I"), uint32_t(-3));
}

TEST(AndroidServiceManagerTest,
     UnknownCacheAndUnknownProxyAreSeparateBoundaries) {
  for (bool known_cache : {false, true}) {
    auto input = emptyCache();
    input.service_manager->manager_proxy.reset();
    if (!known_cache)
      input.service_manager->cache.reset();
    State s(input);
    auto result = s.lookup("getService", s.heap->literal("example")->Identity);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_NE(
        result.diagnostic.find(known_cache ? "proxy state" : "cache contents"),
        std::string::npos);
    EXPECT_EQ(s.clocks, known_cache ? 2u : 1u);
    auto snapshot = s.runtime->snapshot();
    ASSERT_TRUE(snapshot.framework.service_manager);
    EXPECT_TRUE(snapshot.framework.service_manager->initialized);
    EXPECT_EQ(snapshot.framework.service_manager->cache_known, known_cache);
    EXPECT_FALSE(snapshot.framework.service_manager->manager);
    EXPECT_TRUE(snapshot.terminal);
  }
}

TEST(AndroidServiceManagerTest,
     CacheAliasesNullKeysAndBlockingStateShareObjects) {
  auto input = emptyCache();
  auto &config = *input.service_manager;
  config.binder_proxies = {{7, true}};
  config.cache = std::vector<ServiceState::CacheEntry>{
      {"alpha", 7}, {"beta", 7}, {std::nullopt, 7}, {"null", std::nullopt}};
  config.context_object.emplace(std::nullopt);
  State s(input);
  const auto first = s.lookup("getService", s.heap->literal("alpha")->Identity);
  ASSERT_EQ(first.stop, ExecutionStop::Returned) << first.diagnostic;
  EXPECT_NE(first.value, 0u);
  EXPECT_EQ(s.lookup("checkService", s.heap->literal("beta")->Identity).value,
            first.value);
  EXPECT_EQ(s.lookup("getService", 0).value, first.value);
  EXPECT_EQ(s.clocks, 1u);
  const auto *warning = s.program.findField(
      {"Landroid/os/BinderProxy;", "mWarnOnBlocking", "Z"}, false);
  ASSERT_NE(warning, nullptr);
  auto initial_warning =
      s.runtime->field(*warning, first.value, {}, Admit, 65536);
  ASSERT_EQ(initial_warning.stop, ExecutionStop::Returned)
      << initial_warning.diagnostic;
  EXPECT_EQ(initial_warning.value, 1u);
  auto allow = s.call({"Landroid/os/Binder;",
                       "allowBlocking",
                       {"Landroid/os/IBinder;"},
                       "Landroid/os/IBinder;"},
                      true, 0, {first.value});
  EXPECT_EQ(allow.stop, ExecutionStop::Returned);
  EXPECT_EQ(allow.value, first.value);
  EXPECT_EQ(s.runtime->field(*warning, first.value, {}, Admit, 65536).value,
            0u);
  EXPECT_EQ(s.call({"Landroid/os/IBinder;",
                    "queryLocalInterface",
                    {"Ljava/lang/String;"},
                    "Landroid/os/IInterface;"},
                   false, first.value, {0})
                .value,
            0u);
  const auto before = s.runtime->snapshot().framework.service_manager->entries;
  auto null = s.lookup("getService", s.heap->literal("null")->Identity);
  EXPECT_EQ(null.stop, ExecutionStop::Thrown);
  EXPECT_EQ(null.exception, "java/lang/NullPointerException");
  EXPECT_EQ(s.clocks, 2u);
  EXPECT_FALSE(s.runtime->snapshot().terminal);
  EXPECT_EQ(s.runtime->snapshot().framework.service_manager->entries, before);
}

TEST(AndroidServiceManagerTest,
     CacheHitsNeedNoManagerContextWarningOrAdditionalClockObservation) {
  for (const auto warning : {std::optional<bool>{}, std::optional<bool>{false},
                             std::optional<bool>{true}}) {
    Snapshot input;
    auto &config = input.service_manager.emplace();
    config.binder_proxies = {{7, warning}};
    config.cache = std::vector<ServiceState::CacheEntry>{{"example", 7}};
    State s(input);
    s.services.elapsed_realtime_nanoseconds = [&] {
      return ++s.clocks == 1
                 ? ExecutionResult{ExecutionStop::Returned, 50987654321}
                 : ExecutionResult{ExecutionStop::Unsupported,
                                   0,
                                   {},
                                   "unexpected cache-hit clock read"};
    };
    s.runtime = std::make_unique<Runtime>(s.program, s.heap, input, s.services);
    const auto key = s.heap->literal("example")->Identity;
    const auto get = s.lookup("getService", key);
    ASSERT_EQ(get.stop, ExecutionStop::Returned) << get.diagnostic;
    ASSERT_NE(get.value, 0u);
    const auto check = s.lookup("checkService", key);
    ASSERT_EQ(check.stop, ExecutionStop::Returned) << check.diagnostic;
    EXPECT_EQ(check.value, get.value);
    EXPECT_EQ(s.clocks, 1u);
    EXPECT_EQ(s.field("sGetServiceAccumulatedCallCount", "I"), 0u);
    const auto snapshot = s.runtime->snapshot();
    EXPECT_FALSE(snapshot.terminal);
    ASSERT_TRUE(snapshot.framework.service_manager);
    const auto &state = *snapshot.framework.service_manager;
    EXPECT_TRUE(state.initialized);
    EXPECT_TRUE(state.cache_known);
    EXPECT_FALSE(state.manager);
    ASSERT_EQ(state.entries.size(), 1u);
    EXPECT_EQ(state.entries[0].binder, get.value);
    ASSERT_EQ(state.binder_proxies.size(), 1u);
    EXPECT_EQ(state.binder_proxies[0].warn_on_blocking, warning);
  }
}

TEST(AndroidServiceManagerTest,
     NullAndAbsentCacheEntriesRequireManagerStateBeforeContext) {
  for (const char *method : {"getService", "checkService"}) {
    SCOPED_TRACE(method);
    for (bool null_entry : {false, true}) {
      SCOPED_TRACE(null_entry);
      Snapshot input;
      auto &config = input.service_manager.emplace();
      config.cache.emplace();
      if (null_entry)
        config.cache->push_back({"example", std::nullopt});
      config.binder_proxies = {{7, true}};
      config.context_object.emplace(7);
      State s(input);
      const auto result =
          s.lookup(method, s.heap->literal("example")->Identity);
      EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
      EXPECT_EQ(result.diagnostic,
                "Android ServiceManager proxy state is not supplied");
      EXPECT_EQ(s.clocks, std::string_view(method) == "getService" ? 2u : 1u);
      const auto snapshot = s.runtime->snapshot();
      EXPECT_TRUE(snapshot.terminal);
      ASSERT_TRUE(snapshot.framework.service_manager);
      const auto &state = *snapshot.framework.service_manager;
      EXPECT_TRUE(state.initialized);
      EXPECT_TRUE(state.cache_known);
      EXPECT_FALSE(state.manager);
      EXPECT_TRUE(state.binder_proxies.empty());
      ASSERT_EQ(state.entries.size(), null_entry ? 1u : 0u);
      if (null_entry)
        EXPECT_EQ(state.entries[0].binder, 0u);
    }
  }
}

TEST(AndroidServiceManagerTest,
     InitCacheCopiesMapAndChecksSizeBeforeNullArgument) {
  State s;
  auto map = s.call({"Ljava/util/HashMap;", "<init>", {}, "V"}, false).value;
  auto install = [&](uint64_t source) {
    return s.call({Manager, "initServiceCache", {"Ljava/util/Map;"}, "V"}, true,
                  0, {source});
  };
  EXPECT_EQ(install(map).stop, ExecutionStop::Returned);
  EXPECT_EQ(install(map).stop,
            ExecutionStop::Returned); // Empty installation is repeatable.
  auto nil = install(0);
  EXPECT_EQ(nil.stop, ExecutionStop::Thrown);
  EXPECT_EQ(nil.exception, "java/lang/NullPointerException");
  auto key = s.heap->literal("bad-value")->Identity;
  s.call({"Ljava/util/HashMap;",
          "put",
          {"Ljava/lang/Object;", "Ljava/lang/Object;"},
          "Ljava/lang/Object;"},
         false, map, {key, key});
  EXPECT_EQ(install(map).stop, ExecutionStop::Returned);
  s.call({"Ljava/util/HashMap;", "clear", {}, "V"}, false, map);
  auto lookup = s.lookup("getService", key);
  EXPECT_EQ(lookup.stop, ExecutionStop::Thrown);
  EXPECT_EQ(lookup.exception, "java/lang/ClassCastException");
  auto again = install(0);
  ASSERT_EQ(again.stop, ExecutionStop::Thrown);
  EXPECT_EQ(again.exception, "java/lang/IllegalStateException");
  const auto exception = s.heap->get(again.throwable);
  ASSERT_TRUE(exception);
  EXPECT_EQ(s.heap->get(exception->Message)->Bytes,
            javaStringBytes("setServiceCache may only be called once"));
  EXPECT_EQ(s.runtime->snapshot().framework.service_manager->entries.size(),
            1u);
}

TEST(AndroidServiceManagerTest, MissRetainsContextProxyAndStopsBeforeRPC) {
  for (bool existing : {false, true}) {
    auto input = emptyCache();
    auto &config = *input.service_manager;
    config.binder_proxies = {{7, true}};
    config.context_object.emplace(7);
    if (existing)
      config.manager_proxy.emplace(7);
    State s(input);
    auto result = s.lookup("checkService", s.heap->literal("absent")->Identity);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_NE(result.diagnostic.find("Binder transaction"), std::string::npos);
    const auto snapshot = s.runtime->snapshot().framework.service_manager;
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(snapshot->manager);
    ASSERT_EQ(snapshot->binder_proxies.size(), 1u);
    EXPECT_EQ(snapshot->binder_proxies[0].warn_on_blocking, existing);
    const auto manager = s.heap->get(*snapshot->manager);
    ASSERT_TRUE(manager);
    auto field = s.program.findField(
        {"Landroid/os/ServiceManagerProxy;", "mRemote", "Landroid/os/IBinder;"},
        false);
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(manager->Fields.at(field), snapshot->binder_proxies[0].identity);
    EXPECT_TRUE(snapshot->entries.empty());
    EXPECT_EQ(s.clocks, 1u); // checkService never starts StatLogger timing.
  }
}

TEST(AndroidServiceManagerTest,
     InitializerJavaFailureIsErroneousAndBudgetStopIsTerminal) {
  for (auto reason : {ExecutionStop::Thrown, ExecutionStop::Timeout}) {
    State s;
    unsigned reads = 0;
    s.services.property_integer = [&](std::string_view, int32_t) {
      ++reads;
      return ExecutionResult{reason, 0,
                             reason == ExecutionStop::Thrown
                                 ? "java/lang/IllegalArgumentException"
                                 : "",
                             "property stop"};
    };
    s.runtime =
        std::make_unique<Runtime>(s.program, s.heap, emptyCache(), s.services);
    auto result = s.runtime->initialize(Manager, Admit, 65536);
    EXPECT_EQ(result.stop, reason);
    if (reason == ExecutionStop::Thrown)
      EXPECT_EQ(result.exception, "java/lang/ExceptionInInitializerError");
    EXPECT_EQ(s.clocks, 0u);
    auto again = s.lookup("getService", 0);
    EXPECT_EQ(again.stop, reason);
    if (reason == ExecutionStop::Thrown)
      EXPECT_EQ(again.exception, "java/lang/NoClassDefFoundError");
    else
      EXPECT_TRUE(s.runtime->snapshot().terminal);
    EXPECT_EQ(reads, 1u);
    ASSERT_TRUE(s.runtime->snapshot().framework.service_manager);
    EXPECT_FALSE(s.runtime->snapshot().framework.service_manager->initialized);
  }
}

TEST(AndroidServiceManagerTest,
     TypedAdmissionRejectsDanglingAndDuplicateState) {
  for (unsigned mode = 0; mode < 9; ++mode) {
    auto input = emptyCache();
    auto &state = *input.service_manager;
    state.binder_proxies = {{7, {}}};
    if (mode == 0)
      state.binder_proxies.push_back({7, true});
    if (mode == 1)
      state.binder_proxies[0].key = 0;
    if (mode == 2)
      state.cache->push_back({"service", 8});
    if (mode == 3)
      state.context_object.emplace(8);
    if (mode == 4)
      state.manager_proxy.emplace(8);
    if (mode == 5)
      state.cache = std::vector<ServiceState::CacheEntry>{
          {"same", 7}, {"same", std::nullopt}};
    if (mode == 6)
      state.cache = std::vector<ServiceState::CacheEntry>(257);
    if (mode == 7)
      state.binder_proxies.resize(257);
    if (mode == 8)
      state.cache->push_back({std::string(4097, 'x'), 7});
    EXPECT_THROW(validateAndroidServiceManager(input), Error);
    Program program({}, std::map<std::string, std::string>{});
    EXPECT_THROW(Runtime runtime(program, {}, input), Error);
  }
}
TEST(AndroidServiceManagerTest,
     DexStaticCallUsesTheSameInitializerCacheAndReplies) {
  Class caller;
  caller.name = "Ltest/ServiceCaller;";
  caller.superclass = "Ljava/lang/Object;";
  caller.access = {"public"};
  Method method;
  method.reference = {
      caller.name, "lookup", {"Ljava/lang/String;"}, "Landroid/os/IBinder;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction invoke;
  invoke.pc = 0;
  invoke.opcode = "invoke-static";
  invoke.registers = {0};
  invoke.reference = MethodRef{
      Manager, "getService", {"Ljava/lang/String;"}, "Landroid/os/IBinder;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {invoke, move, ret};
  caller.methods.push_back(method);
  Program program({caller}, std::map<std::string, std::string>{});
  for (bool replies : {false, true}) {
    auto input = replies ? lookupReplies() : emptyCache();
    if (!replies) {
      input.service_manager->binder_proxies = {{7, {}}};
      input.service_manager->cache =
          std::vector<ServiceState::CacheEntry>{{"alpha", 7}};
    }
    State services;
    auto heap = std::make_shared<Heap>();
    Runtime runtime(program, heap, input, services.services);
    const uint64_t args[] = {heap->literal("alpha")->Identity};
    auto result = runtime.execute(*program.findMethod(method.reference, true),
                                  args, Admit, 65536);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(heap->get(result.value)->ClassName, "android/os/BinderProxy");
    EXPECT_EQ(services.properties, 4u);
    EXPECT_EQ(services.clocks, replies ? 4u : 1u);
    ASSERT_TRUE(runtime.snapshot().framework.service_manager);
    EXPECT_TRUE(runtime.snapshot().framework.service_manager->initialized);
    const auto *library = libraryMethod(
        {Manager, "getService", {"Ljava/lang/String;"}, "Landroid/os/IBinder;"},
        true);
    ASSERT_NE(library, nullptr);
    EXPECT_EQ(runtime.library(*library, 0, args, Admit).value, result.value);
    EXPECT_EQ(services.properties, 4u);
  }
}

TEST(AndroidServiceManagerTest,
     UnknownBlockingFlagCanBeWrittenWithoutInferringADefault) {
  auto input = emptyCache();
  input.service_manager->binder_proxies = {{7, {}}};
  input.service_manager->cache =
      std::vector<ServiceState::CacheEntry>{{"alpha", 7}};
  for (bool allow : {false, true}) {
    State s(input);
    auto binder = s.lookup("getService", s.heap->literal("alpha")->Identity);
    ASSERT_EQ(binder.stop, ExecutionStop::Returned);
    const auto *warning = s.program.findField(
        {"Landroid/os/BinderProxy;", "mWarnOnBlocking", "Z"}, false);
    ASSERT_NE(warning, nullptr);
    if (allow)
      EXPECT_EQ(s.call({"Landroid/os/Binder;",
                        "allowBlocking",
                        {"Landroid/os/IBinder;"},
                        "Landroid/os/IBinder;"},
                       true, 0, {binder.value})
                    .stop,
                ExecutionStop::Returned);
    auto result = s.runtime->field(*warning, binder.value, {}, Admit, 65536);
    EXPECT_EQ(result.stop,
              allow ? ExecutionStop::Returned : ExecutionStop::Unsupported);
    if (allow)
      EXPECT_EQ(result.value, 0u);
  }
}
TEST(AndroidServiceManagerTest,
     VolatileProjectionDoesNotAdmitSuppliedVolatileFields) {
  Class type;
  type.name = "Ltest/Volatile;";
  type.superclass = "Ljava/lang/Object;";
  type.access = {"public"};
  Field field;
  field.reference = {type.name, "warning", "Z"};
  field.access = {"public", "volatile"};
  type.fields.push_back(field);
  Program program({type}, std::map<std::string, std::string>{});
  EXPECT_TRUE(program.unsupportedClass(type.name));
  EXPECT_FALSE(program.unsupportedClass("Landroid/os/BinderProxy;"));
}

TEST(AndroidServiceManagerTest, WorkAndHeapLimitsNeverCompleteInitialization) {
  for (bool heap_limit : {false, true}) {
    State s;
    if (heap_limit)
      s.heap = std::make_shared<Heap>(128, 1);
    s.runtime =
        std::make_unique<Runtime>(s.program, s.heap, emptyCache(), s.services);
    unsigned ticks = 0;
    auto admit = [&] {
      return heap_limit || ++ticks < 6 ? ExecutionStop::Returned
                                       : ExecutionStop::InstructionLimit;
    };
    const auto expected =
        heap_limit ? ExecutionStop::HeapLimit : ExecutionStop::InstructionLimit;
    auto result = s.runtime->initialize(Manager, admit, 65536);
    EXPECT_EQ(result.stop, expected);
    auto again = s.runtime->initialize(Manager, Admit, 65536);
    EXPECT_EQ(again.stop, expected);
    EXPECT_TRUE(s.runtime->snapshot().terminal);
    EXPECT_EQ(s.properties, 0u);
    EXPECT_EQ(s.clocks, 0u);
    for (const auto &row : s.runtime->snapshot().classes)
      if (row.descriptor == Manager)
        EXPECT_EQ(row.initialization,
                  JavaClassState::Initialization::Initializing);
  }
}
} // namespace
} // namespace neverd::mobile::dalvik
