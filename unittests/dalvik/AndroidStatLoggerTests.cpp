//===- AndroidStatLoggerTests.cpp - Shared statistics initialization ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 constructor state, array identities and process-clock boundaries.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"

#include "llvm/Support/Endian.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
constexpr auto Logger = "Lcom/android/internal/util/StatLogger;";
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
using Properties = std::map<std::string, std::string>;

TEST(AndroidStatLoggerTest, ConstructorRetainsLabelsAndIndependentZeroArrays) {
  Program program({}, Properties{});
  auto heap = std::make_shared<Heap>();
  RuntimeServices services;
  unsigned reads = 0;
  services.elapsed_realtime_nanoseconds = [&] {
    ++reads;
    return ExecutionResult{ExecutionStop::Returned, 50987654321};
  };
  Runtime runtime(program, heap, {}, services);
  const auto *constructor =
      libraryMethod({Logger, "<init>", {"[Ljava/lang/String;"}, "V"}, false);
  ASSERT_NE(constructor, nullptr);
  auto labels = heap->allocateArray("[Ljava/lang/String;", 2);
  labels->Elements[0] = heap->literal("event");
  const uint64_t args[] = {labels->Identity};
  std::set<uint64_t> identities;
  for (unsigned round = 0; round != 2; ++round) {
    auto result = runtime.library(*constructor, 0, args, Admit);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    auto logger = heap->get(result.value);
    ASSERT_TRUE(logger);
    auto field = [&](const char *name, const char *type) {
      const auto *entry = program.findField({Logger, name, type}, false);
      EXPECT_NE(entry, nullptr);
      return entry && logger->Fields.contains(entry) ? logger->Fields.at(entry)
                                                     : uint64_t(-1);
    };
    EXPECT_EQ(field("SIZE", "I"), 2u);
    EXPECT_EQ(field("mNextTickTime", "J"), 51987u);
    EXPECT_EQ(field("mLabels", "[Ljava/lang/String;"), labels->Identity);
    auto lock = heap->get(field("mLock", "Ljava/lang/Object;"));
    ASSERT_TRUE(lock);
    EXPECT_EQ(lock->ClassName, "java/lang/Object");
    EXPECT_TRUE(identities.insert(lock->Identity).second);
    for (auto [name, type] : {std::pair{"mCountStats", "[I"},
                              {"mDurationStats", "[J"},
                              {"mCallsPerSecond", "[I"},
                              {"mMaxCallsPerSecond", "[I"},
                              {"mDurationPerSecond", "[J"},
                              {"mMaxDurationPerSecond", "[J"},
                              {"mMaxDurationStats", "[J"}}) {
      auto array = heap->get(field(name, type));
      ASSERT_TRUE(array);
      EXPECT_EQ(array->ClassName, type);
      EXPECT_EQ(array->Type, Object::PrimitiveArray);
      EXPECT_EQ(array->Length, 2u);
      EXPECT_EQ(array->ElementSize, type[1] == 'I' ? 4u : 8u);
      EXPECT_EQ(array->Bytes, std::vector<uint8_t>(2 * array->ElementSize, 0));
      EXPECT_TRUE(identities.insert(array->Identity).second);
    }
  }
  EXPECT_EQ(reads, 2u);
}

TEST(AndroidStatLoggerTest,
     GetTimeUsesElapsedMicrosecondsWithoutInstanceState) {
  Program program({}, Properties{});
  auto heap = std::make_shared<Heap>();
  RuntimeServices services;
  uint64_t nanoseconds = 0;
  services.elapsed_realtime_nanoseconds = [&] {
    return ExecutionResult{ExecutionStop::Returned, nanoseconds};
  };
  Runtime runtime(program, heap, {}, services);
  const auto *method = libraryMethod({Logger, "getTime", {}, "J"}, false);
  ASSERT_NE(method, nullptr);
  auto raw = runtime.allocate(Logger, true, Admit, 65536);
  ASSERT_EQ(raw.stop, ExecutionStop::Returned) << raw.diagnostic;
  for (uint64_t value : {0ULL, 999ULL, 1000ULL, 98765432101ULL,
                         static_cast<unsigned long long>(INT64_MAX)}) {
    nanoseconds = value;
    auto result = runtime.library(*method, raw.value, {}, Admit);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, value / 1000);
  }
}

TEST(AndroidStatLoggerTest, MetadataKeepsBootOwnershipAndFieldAccess) {
  Program program({}, Properties{});
  const auto *type = program.findClass(Logger);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(type->superclass, "Ljava/lang/Object;");
  EXPECT_EQ(type->access, (Access{"public"}));
  ASSERT_EQ(type->fields.size(), 12u);
  for (const auto &field : type->fields) {
    EXPECT_TRUE(field.access.contains("private"));
    EXPECT_EQ(field.access.contains("final"),
              field.reference.name != "mNextTickTime");
    EXPECT_EQ(field.access.contains("static"), field.reference.name == "TAG");
  }
  const auto *tag =
      program.findField({Logger, "TAG", "Ljava/lang/String;"}, true);
  ASSERT_NE(tag, nullptr);
  EXPECT_EQ(std::get<std::string>(tag->value), "StatLogger");
  auto heap = std::make_shared<Heap>();
  Runtime runtime(program, heap, AndroidApplicationSnapshot{});
  const auto *loader = libraryMethod(
      {"Ljava/lang/Class;", "getClassLoader", {}, "Ljava/lang/ClassLoader;"},
      false);
  ASSERT_NE(loader, nullptr);
  for (const auto *name : {"com/android/internal/util/StatLogger",
                           "[Lcom/android/internal/util/StatLogger;"}) {
    auto result =
        runtime.library(*loader, heap->classObject(name)->Identity, {}, Admit);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    ASSERT_TRUE(heap->get(result.value));
    EXPECT_EQ(heap->get(result.value)->ClassName, "java/lang/BootClassLoader");
  }
  Class replacement;
  replacement.name = Logger;
  EXPECT_THROW((Program({replacement}, Properties{})), Error);
  EXPECT_THROW((Program({replacement})), Error);
  EXPECT_TRUE(
      libraryMethod({Logger, "logDurationStat", {"I", "J"}, "J"}, false));
}

struct Statistics {
  Program program{{}, Properties{}};
  std::shared_ptr<Heap> heap = std::make_shared<Heap>();
  uint64_t now = 1000000000;
  unsigned reads = 0, fail_read = 0;
  RuntimeServices services;
  std::unique_ptr<Runtime> runtime;
  uint64_t logger = 0;
  Statistics() {
    services.elapsed_realtime_nanoseconds = [&] {
      return ExecutionResult{++reads == fail_read ? ExecutionStop::Timeout
                                                  : ExecutionStop::Returned,
                             now};
    };
    runtime = std::make_unique<Runtime>(program, heap, std::nullopt, services);
    const uint64_t args[] = {
        heap->allocateArray("[Ljava/lang/String;", 2)->Identity};
    const auto *method =
        libraryMethod({Logger, "<init>", {"[Ljava/lang/String;"}, "V"}, false);
    auto result = runtime->library(*method, 0, args, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    logger = result.value;
  }
  uint64_t &field(const char *name, const char *type) {
    return heap->get(logger)->Fields.at(
        program.findField({Logger, name, type}, false));
  }
  uint64_t element(const char *name, bool wide, unsigned index,
                   std::optional<uint64_t> write = {}) {
    auto array = heap->get(field(name, wide ? "[J" : "[I"));
    auto *bytes = array->Bytes.data() + index * (wide ? 8 : 4);
    if (write) {
      if (wide)
        llvm::support::endian::write64le(bytes, *write);
      else
        llvm::support::endian::write32le(bytes, uint32_t(*write));
    }
    return wide ? llvm::support::endian::read64le(bytes)
                : llvm::support::endian::read32le(bytes);
  }
  ExecutionResult record(int32_t event, uint64_t start,
                         const AdmitInstruction &admit = Admit) {
    const uint64_t args[] = {uint32_t(event), start};
    return runtime->library(
        *libraryMethod({Logger, "logDurationStat", {"I", "J"}, "J"}, false),
        logger, args, admit);
  }
};

TEST(AndroidStatLoggerTest, DurationCountersTickStrictlyAndKeepOtherEvents) {
  Statistics s;
  for (auto [time, event, start, duration] :
       {std::tuple{1500000000ULL, 0, 1499950ULL, 50ULL},
        std::tuple{2000000000ULL, 1, 1999900ULL, 100ULL},
        std::tuple{2001000000ULL, 0, 2000800ULL, 200ULL}}) {
    s.now = time;
    auto result = s.record(event, start);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, duration);
    EXPECT_EQ(s.field("mNextTickTime", "J"), time > 2000000000 ? 3001u : 2000u);
  }
  EXPECT_EQ(s.element("mCountStats", false, 0), 2u);
  EXPECT_EQ(s.element("mDurationStats", true, 0), 250u);
  EXPECT_EQ(s.element("mMaxDurationStats", true, 0), 200u);
  EXPECT_EQ(s.element("mMaxCallsPerSecond", false, 0), 1u);
  EXPECT_EQ(s.element("mMaxDurationPerSecond", true, 0), 50u);
  EXPECT_EQ(s.element("mCallsPerSecond", false, 0), 1u);
  EXPECT_EQ(s.element("mDurationPerSecond", true, 0), 200u);
  EXPECT_EQ(s.element("mCallsPerSecond", false, 1), 1u);
  EXPECT_EQ(s.element("mDurationPerSecond", true, 1), 100u);
  EXPECT_EQ(s.element("mMaxDurationPerSecond", true, 1), 0u);
  s.element("mCountStats", false, 0, UINT32_MAX);
  s.element("mDurationStats", true, 0, UINT64_MAX - 99);
  ASSERT_EQ(s.record(0, 2000800).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.element("mCountStats", false, 0), 0u);
  EXPECT_EQ(s.element("mDurationStats", true, 0), 100u);
  auto negative = s.record(0, 2001005);
  ASSERT_EQ(negative.stop, ExecutionStop::Returned);
  EXPECT_EQ(negative.value, uint64_t(-5));
  EXPECT_EQ(s.element("mDurationStats", true, 0), 95u);
  EXPECT_EQ(s.element("mMaxDurationStats", true, 0), 200u);
}

TEST(AndroidStatLoggerTest, ArrayAndClockFailuresPreserveOrderedUpdates) {
  for (unsigned mode = 0; mode != 4; ++mode) {
    Statistics s;
    if (mode == 0)
      s.field("mLock", "Ljava/lang/Object;") = 0;
    if (mode == 1)
      s.field("mDurationStats", "[J") = 0;
    if (mode == 2)
      s.field("mDurationStats", "[J") =
          s.heap->allocateArray("[J", 0)->Identity;
    if (mode == 3)
      s.fail_read = 3; // Constructor, duration, then the per-second clock.
    auto result = s.record(0, 999975);
    EXPECT_EQ(result.stop,
              mode == 3 ? ExecutionStop::Timeout : ExecutionStop::Thrown);
    if (mode != 3)
      EXPECT_EQ(result.exception,
                mode == 2 ? "java/lang/ArrayIndexOutOfBoundsException"
                          : "java/lang/NullPointerException");
    EXPECT_EQ(s.reads, mode == 0 ? 1u : mode == 3 ? 3u : 2u);
    EXPECT_EQ(s.element("mCountStats", false, 0), mode ? 1u : 0u);
    EXPECT_EQ(s.element("mCallsPerSecond", false, 0), 0u);
    if (mode == 3) {
      EXPECT_EQ(s.element("mDurationStats", true, 0), 25u);
      EXPECT_EQ(s.element("mMaxDurationStats", true, 0), 25u);
    }
  }
}

TEST(AndroidStatLoggerTest, InvalidEventsAndWorkStopsRemainExplicit) {
  for (int32_t event : {-1, 2}) {
    Statistics s;
    auto result = s.record(event, 0);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_NE(result.diagnostic.find("invalid-event logging"),
              std::string::npos);
    EXPECT_EQ(s.reads, 2u);
    EXPECT_EQ(s.element("mCountStats", false, 0), 0u);
  }
  Statistics s;
  auto result = s.record(0, 0, [] { return ExecutionStop::InstructionLimit; });
  EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
  EXPECT_EQ(s.reads, 1u);
  EXPECT_EQ(s.element("mCountStats", false, 0), 0u);
  EXPECT_TRUE(s.runtime->snapshot().terminal);
}

TEST(AndroidStatLoggerTest, NullLabelsPreserveLockClockAndExceptionOrder) {
  for (unsigned mode = 0; mode != 5; ++mode) {
    Program program({}, Properties{});
    auto heap = std::make_shared<Heap>();
    unsigned reads = 0;
    RuntimeServices services;
    if (mode)
      services.elapsed_realtime_nanoseconds = [&] {
        ++reads;
        return ExecutionResult{mode == 2   ? ExecutionStop::Timeout
                               : mode == 3 ? ExecutionStop::Invalid
                                           : ExecutionStop::Returned,
                               mode == 4 ? uint64_t(-1) : 1234567890,
                               {},
                               "clock boundary"};
      };
    Runtime runtime(program, heap, {}, services);
    auto raw = runtime.allocate(Logger, false, Admit, 65536);
    ASSERT_EQ(raw.stop, ExecutionStop::Returned) << raw.diagnostic;
    const auto *constructor =
        libraryMethod({Logger, "<init>", {"[Ljava/lang/String;"}, "V"}, false);
    ASSERT_NE(constructor, nullptr);
    const uint64_t args[] = {0};
    auto result = runtime.library(*constructor, raw.value, args, Admit);
    const auto expected = mode == 1   ? ExecutionStop::Thrown
                          : mode == 2 ? ExecutionStop::Timeout
                          : mode == 3 ? ExecutionStop::Invalid
                                      : ExecutionStop::Unsupported;
    EXPECT_EQ(result.stop, expected) << result.diagnostic;
    EXPECT_EQ(reads, mode ? 1u : 0u);
    auto logger = heap->get(raw.value);
    EXPECT_FALSE(logger->Initialized);
    for (const auto &field : program.findClass(Logger)->fields) {
      if (field.access.contains("static"))
        continue;
      const auto value = logger->Fields.at(&field);
      if (field.reference.name == "mLock") {
        ASSERT_TRUE(heap->get(value));
        EXPECT_EQ(heap->get(value)->ClassName, "java/lang/Object");
      } else {
        EXPECT_EQ(value, field.reference.name == "mNextTickTime" && mode == 1
                             ? 2234u
                             : 0u);
      }
    }
    if (mode == 1) {
      EXPECT_EQ(result.exception, "java/lang/NullPointerException");
      EXPECT_FALSE(runtime.snapshot().terminal);
    } else {
      EXPECT_TRUE(runtime.snapshot().terminal);
      EXPECT_EQ(runtime.library(*constructor, raw.value, args, Admit).stop,
                expected);
      EXPECT_EQ(reads, mode ? 1u : 0u);
    }
  }
}

TEST(AndroidStatLoggerTest,
     WorkStopsAndHeapExhaustionCannotCompleteConstructor) {
  const auto *constructor =
      libraryMethod({Logger, "<init>", {"[Ljava/lang/String;"}, "V"}, false);
  ASSERT_NE(constructor, nullptr);
  RuntimeServices services;
  services.elapsed_realtime_nanoseconds = [] {
    return ExecutionResult{ExecutionStop::Returned, INT64_MAX};
  };
  for (unsigned limit = 1; limit <= 11; ++limit) {
    Program program({}, Properties{});
    auto heap = std::make_shared<Heap>();
    Runtime runtime(program, heap, {}, services);
    const uint64_t args[] = {
        heap->allocateArray("[Ljava/lang/String;", 0)->Identity};
    auto raw = runtime.allocate(Logger, false, Admit, 65536);
    ASSERT_EQ(raw.stop, ExecutionStop::Returned);
    unsigned ticks = 0;
    auto result = runtime.library(*constructor, raw.value, args, [&] {
      return ++ticks == limit ? ExecutionStop::InstructionLimit
                              : ExecutionStop::Returned;
    });
    EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit) << limit;
    EXPECT_EQ(ticks, limit);
    EXPECT_TRUE(runtime.snapshot().terminal);
    EXPECT_FALSE(heap->get(raw.value)->Initialized);
    const auto *labels =
        program.findField({Logger, "mLabels", "[Ljava/lang/String;"}, false);
    EXPECT_EQ(heap->get(raw.value)->Fields.at(labels), 0u);
  }
  unsigned failures = 0, successes = 0;
  for (unsigned objects = 1; objects <= 14; ++objects) {
    Program program({}, Properties{});
    auto heap = std::make_shared<Heap>(1 << 20, objects);
    Runtime runtime(program, heap, {}, services);
    const uint64_t args[] = {
        heap->allocateArray("[Ljava/lang/String;", 0)->Identity};
    auto result = runtime.library(*constructor, 0, args, Admit);
    if (result.stop == ExecutionStop::HeapLimit) {
      ++failures;
      EXPECT_TRUE(runtime.snapshot().terminal);
    } else {
      ++successes;
      ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
      const auto *tick =
          program.findField({Logger, "mNextTickTime", "J"}, false);
      EXPECT_EQ(heap->get(result.value)->Fields.at(tick),
                uint64_t(INT64_MAX) / 1000000 + 1000);
    }
  }
  EXPECT_GT(failures, 0u);
  EXPECT_GT(successes, 0u);
}

TEST(AndroidStatLoggerTest, DexConstructionAndVirtualTimeShareTheSameObjects) {
  Class caller;
  caller.name = "Ltest/LoggerCaller;";
  caller.superclass = "Ljava/lang/Object;";
  caller.access = {"public"};
  auto insn = [](uint32_t pc, const char *opcode, std::vector<unsigned> regs) {
    Instruction result;
    result.pc = pc;
    result.opcode = opcode;
    result.registers = std::move(regs);
    return result;
  };
  Method make;
  make.reference = {caller.name, "make", {"I"}, Logger};
  make.access = {"public", "static"};
  make.registers = 3;
  auto array = insn(0, "new-array", {1, 2});
  array.reference = std::string("[Ljava/lang/String;");
  auto allocation = insn(2, "new-instance", {0});
  allocation.reference = std::string(Logger);
  auto init = insn(4, "invoke-direct", {0, 1});
  init.reference = MethodRef{Logger, "<init>", {"[Ljava/lang/String;"}, "V"};
  make.instructions = {array, allocation, init, insn(7, "return-object", {0})};
  make.code_end = 8;
  Method time;
  time.reference = {caller.name, "time", {Logger}, "J"};
  time.access = {"public", "static"};
  time.registers = 3;
  auto invoke = insn(0, "invoke-virtual", {2});
  invoke.reference = MethodRef{Logger, "getTime", {}, "J"};
  time.instructions = {invoke, insn(3, "move-result-wide", {0}),
                       insn(4, "return-wide", {0})};
  time.code_end = 5;
  caller.methods = {make, time};
  Program program({caller}, Properties{});
  auto heap = std::make_shared<Heap>();
  RuntimeServices services;
  services.elapsed_realtime_nanoseconds = [] {
    return ExecutionResult{ExecutionStop::Returned, 1234567890};
  };
  Runtime runtime(program, heap, {}, services);
  const uint64_t length[] = {3};
  auto result = runtime.execute(*program.findStatic(make.reference), length,
                                Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  auto logger = heap->get(result.value);
  ASSERT_TRUE(logger);
  EXPECT_TRUE(logger->Initialized);
  const auto *field =
      program.findField({Logger, "mLabels", "[Ljava/lang/String;"}, false);
  auto labels = runtime.field(*field, logger->Identity, {}, Admit, 65536);
  ASSERT_EQ(labels.stop, ExecutionStop::Returned) << labels.diagnostic;
  EXPECT_EQ(heap->get(labels.value)->Length, 3u);
  const uint64_t args[] = {logger->Identity};
  auto clock =
      runtime.execute(*program.findStatic(time.reference), args, Admit, 65536);
  ASSERT_EQ(clock.stop, ExecutionStop::Returned) << clock.diagnostic;
  EXPECT_EQ(clock.value, 1234567u);
}

TEST(AndroidArrayTest, SharedAllocationKeepsWidthsZerosAndExactStorageBudgets) {
  for (auto [type, width] :
       {std::pair{"[Z", 1u},
        {"[B", 1u},
        {"[C", 2u},
        {"[S", 2u},
        {"[I", 4u},
        {"[J", 8u},
        {"[F", 4u},
        {"[D", 8u},
        {"[[I", unsigned(sizeof(std::weak_ptr<Object>))},
        {"[Ljava/lang/String;", unsigned(sizeof(std::weak_ptr<Object>))}}) {
    for (unsigned length : {0u, 3u}) {
      const uint64_t bytes = sizeof(Object) + std::string_view(type).size() +
                             64 + uint64_t(width) * length;
      Heap exact(bytes), short_heap(bytes - 1);
      auto array = exact.allocateArray(type, length);
      ASSERT_TRUE(array);
      EXPECT_EQ(array->Length, length);
      EXPECT_EQ(array->ElementSize, width);
      if (type[1] == '[' || type[1] == 'L') {
        EXPECT_TRUE(array->Bytes.empty());
        EXPECT_EQ(array->Elements.size(), length);
        for (const auto &element : array->Elements)
          EXPECT_TRUE(element.expired());
      } else {
        EXPECT_EQ(array->Bytes, std::vector<uint8_t>(length * width, 0));
        EXPECT_TRUE(array->Elements.empty());
      }
      EXPECT_FALSE(short_heap.allocateArray(type, length));
      EXPECT_FALSE(exact.allocateArray(type, 0));
    }
  }
}
} // namespace
} // namespace neverd::mobile::dalvik
