//===- AndroidEventLogTests.cpp - API 28 event encoding regressions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent event bytes, truncation boundaries and shared runtime effects.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
struct EventState {
  Program program{{}, std::map<std::string, std::string>{}};
  std::shared_ptr<Heap> heap = std::make_shared<Heap>();
  std::vector<std::vector<uint8_t>> writes;
  ExecutionResult returned{ExecutionStop::Returned, uint32_t(-11)};
  RuntimeServices services;
  std::unique_ptr<Runtime> runtime;
  explicit EventState(bool supplied = true) {
    if (supplied)
      services.event_log_write = [&](std::span<const uint8_t> bytes) {
        writes.emplace_back(bytes.begin(), bytes.end());
        return returned;
      };
    runtime = std::make_unique<Runtime>(program, heap, std::nullopt, services);
  }
  ExecutionResult write(const char *type, uint64_t value,
                        const AdmitInstruction &admit = Admit) {
    const auto *method = libraryMethod(
        {"Landroid/util/EventLog;", "writeEvent", {"I", type}, "I"}, true);
    EXPECT_NE(method, nullptr);
    const uint64_t args[] = {0x12345678, value};
    return method ? runtime->library(*method, 0, args, admit)
                  : ExecutionResult{ExecutionStop::Invalid};
  }
  uint64_t box(const char *name, const char *type, uint64_t value) {
    const auto *method = libraryMethod(
        {"L" + std::string(name) + ";", "<init>", {type}, "V"}, false);
    EXPECT_NE(method, nullptr);
    if (!method)
      return 0;
    const uint64_t args[] = {value};
    auto result = runtime->library(*method, 0, args, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  }
  uint64_t array(std::initializer_list<uint64_t> values) {
    auto result = heap->allocateArray("[Ljava/lang/Object;", values.size());
    unsigned index = 0;
    for (uint64_t value : values)
      result->Elements[index++] = heap->get(value);
    return result->Identity;
  }
};

TEST(AndroidEventLogTest, ScalarOverloadsPreserveSignedAndFloatingBits) {
  const struct {
    const char *type;
    uint64_t value;
    std::vector<uint8_t> bytes;
  } cases[] = {{"I", 0xffffffff, {0, 255, 255, 255, 255}},
               {"J", 0x8000000000000001, {1, 1, 0, 0, 0, 0, 0, 0, 128}},
               {"F", 0x80000000, {4, 0, 0, 0, 128}},
               {"F", 0x7fc12345, {4, 0x45, 0x23, 0xc1, 0x7f}},
               {"F", 0x7f800000, {4, 0, 0, 0x80, 0x7f}}};
  for (const auto &c : cases) {
    EventState s;
    auto result = s.write(c.type, c.value);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, uint32_t(-11));
    std::vector<uint8_t> expected{0x78, 0x56, 0x34, 0x12};
    expected.insert(expected.end(), c.bytes.begin(), c.bytes.end());
    EXPECT_EQ(s.writes, (std::vector<std::vector<uint8_t>>{expected}));
  }
}

TEST(AndroidEventLogTest, StringsRetainArtEncodingAndDistinctNullForms) {
  EventState s;
  auto string = s.heap->allocate(Object::String, "java/lang/String", 12);
  string->Bytes = {0, 0, 0x3d, 0xd8, 0, 0xde, 0, 0xd8, 0, 0xdc, 0xff, 0xdc};
  ASSERT_EQ(s.write("Ljava/lang/String;", string->Identity).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.writes.back(), (std::vector<uint8_t>{
                                 0x78, 0x56, 0x34, 0x12, 2,    13,   0,    0,
                                 0,    0xc0, 0x80, 0xf0, 0x9f, 0x98, 0x80, 0xf0,
                                 0x90, 0x80, 0x80, 0xed, 0xb3, 0xbf}));
  ASSERT_EQ(s.write("Ljava/lang/String;", 0).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.writes.back(),
            (std::vector<uint8_t>{0x78, 0x56, 0x34, 0x12, 2, 4, 0, 0, 0, 'N',
                                  'U', 'L', 'L'}));
  ASSERT_EQ(s.write("[Ljava/lang/Object;", 0).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.writes.back(),
            (std::vector<uint8_t>{0x78, 0x56, 0x34, 0x12, 2, 6, 0, 0, 0, '[',
                                  'N', 'U', 'L', 'L', ']'}));
  ASSERT_EQ(s.write("[Ljava/lang/Object;", s.array({})).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.writes.back(), (std::vector<uint8_t>{0x78, 0x56, 0x34, 0x12}));
  ASSERT_EQ(s.write("[Ljava/lang/Object;", s.array({0})).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.writes.back(), s.writes[1]);
}

TEST(AndroidEventLogTest, MixedBoxedArrayUsesRetainedFields) {
  EventState s;
  const auto integer = s.box("java/lang/Integer", "I", 0xffffffff);
  const auto wide = s.box("java/lang/Long", "J", 0x8877665544332211);
  const auto floating = s.box("java/lang/Float", "F", 0x7fc12345);
  auto result =
      s.write("[Ljava/lang/Object;", s.array({integer, wide, floating, 0}));
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(s.writes.back(),
            (std::vector<uint8_t>{0x78, 0x56, 0x34, 0x12, 3,    4,    0,
                                  255,  255,  255,  255,  1,    0x11, 0x22,
                                  0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 4,
                                  0x45, 0x23, 0xc1, 0x7f, 2,    4,    0,
                                  0,    0,    'N',  'U',  'L',  'L'}));
  const auto *field =
      s.program.findField({"Ljava/lang/Float;", "value", "F"}, false);
  ASSERT_NE(field, nullptr);
  result = s.runtime->field(*field, floating, {}, Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 0x7fc12345u);
  result = s.runtime->field(*field, floating, 0, Admit, 65536);
  EXPECT_EQ(result.stop, ExecutionStop::Invalid);
  EXPECT_EQ(s.heap->get(floating)->Fields.at(field), 0x7fc12345u);
}

TEST(AndroidEventLogTest, ArrayLimitSkipsUnvisitedInvalidItems) {
  EventState s;
  auto array = s.heap->allocateArray("[Ljava/lang/Object;", 256);
  const auto boxed = s.box("java/lang/Integer", "I", 42);
  std::fill_n(array->Elements.begin(), 255, s.heap->get(boxed));
  array->Elements[255] =
      s.heap->allocate(Object::Instance, "java/lang/Object", 0);
  auto result = s.write("[Ljava/lang/Object;", array->Identity);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  std::vector<uint8_t> expected{0x78, 0x56, 0x34, 0x12, 3, 255};
  for (unsigned i = 0; i < 255; ++i)
    expected.insert(expected.end(), {0, 42, 0, 0, 0});
  EXPECT_EQ(s.writes.back(), expected);
  array->Elements[254] = array;
  result = s.write("[Ljava/lang/Object;", array->Identity);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/IllegalArgumentException");
  EXPECT_EQ(s.writes.size(), 1u);
}

TEST(AndroidEventLogTest, InvalidArrayItemsThrowBeforeWritingAnyPrefix) {
  for (const char *name :
       {"java/lang/Object", "java/lang/Double", "java/lang/Boolean"}) {
    EventState s;
    const auto invalid = s.heap->allocate(Object::Instance, name, 0);
    auto result =
        s.write("[Ljava/lang/Object;", s.array({0, invalid->Identity}));
    EXPECT_EQ(result.stop, ExecutionStop::Thrown);
    EXPECT_EQ(result.exception, "java/lang/IllegalArgumentException");
    EXPECT_EQ(result.diagnostic, "Invalid payload item type");
    EXPECT_TRUE(s.writes.empty());
  }
  EventState s;
  const auto unknown =
      s.heap->allocate(Object::Instance, "java/lang/Integer", 0);
  auto result = s.write("[Ljava/lang/Object;", s.array({unknown->Identity}));
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_TRUE(s.writes.empty());
}

TEST(AndroidEventLogTest, SingletonTruncationRetainsApi28ZeroTail) {
  for (unsigned size : {0u, 4056u, 4057u, 4058u, 4059u, 4064u, 9000u}) {
    for (bool array : {false, true}) {
      SCOPED_TRACE(size);
      EventState s;
      auto string = s.heap->literal(std::string(size, 'x'));
      auto result =
          s.write(array ? "[Ljava/lang/Object;" : "Ljava/lang/String;",
                  array ? s.array({string->Identity}) : string->Identity);
      ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
      EXPECT_EQ(result.value, uint32_t(-11));
      const unsigned kept = std::min(size, 4057u);
      std::vector<uint8_t> expected{
          0x78, 0x56, 0x34, 0x12, 2, uint8_t(kept), uint8_t(kept >> 8), 0, 0};
      expected.insert(expected.end(), kept, 'x');
      expected.resize(std::min(size + 9, 4068u), 0);
      EXPECT_EQ(s.writes.back(), expected);
    }
  }
}

TEST(AndroidEventLogTest, TruncationCanSplitAnEncodedCharacter) {
  EventState s;
  auto text = s.heap->literal(std::string(4056, 'x') + "\xc3\xa9");
  auto result = s.write("Ljava/lang/String;", text->Identity);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  ASSERT_EQ(s.writes.back().size(), 4067u);
  EXPECT_EQ(s.writes.back()[5], 0xd9);
  EXPECT_EQ(s.writes.back()[6], 0x0f);
  EXPECT_EQ(s.writes.back()[4065], 0xc3);
  EXPECT_EQ(s.writes.back()[4066], 0);
}

TEST(AndroidEventLogTest, FullPrefixWritesButEncodingErrorWinsOverTransport) {
  for (int32_t transport : {21, -11}) {
    EventState s;
    s.returned.value = uint32_t(transport);
    const auto first = s.box("java/lang/Integer", "I", 42);
    auto string = s.heap->literal(std::string(6000, 'x'));
    const auto bad = s.heap->allocate(Object::Instance, "java/lang/Object", 0);
    auto result =
        s.write("[Ljava/lang/Object;", s.array({first, string->Identity}));
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, uint32_t(transport));
    ASSERT_EQ(s.writes.back().size(), 4068u);
    EXPECT_EQ(std::vector<uint8_t>(s.writes.back().begin(),
                                   s.writes.back().begin() + 16),
              (std::vector<uint8_t>{0x78, 0x56, 0x34, 0x12, 3, 2, 0, 42, 0, 0,
                                    0, 2, 0xd4, 0x0f, 0, 0}));
    EXPECT_TRUE(std::all_of(s.writes.back().begin() + 16, s.writes.back().end(),
                            [](uint8_t byte) { return byte == 'x'; }));
    result = s.write("[Ljava/lang/Object;",
                     s.array({first, string->Identity, first, bad->Identity}));
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, uint32_t(-5));
    EXPECT_EQ(s.writes[0], s.writes[1]);
    // A truncated string alone sets no error. The next invalid item is visited.
    result = s.write("[Ljava/lang/Object;",
                     s.array({first, string->Identity, bad->Identity}));
    EXPECT_EQ(result.stop, ExecutionStop::Thrown);
    EXPECT_EQ(s.writes.size(), 2u);
  }
}

TEST(AndroidEventLogTest, ExactEmptyStringHeaderFitsButNonemptyStringFails) {
  for (bool empty : {true, false}) {
    EventState s;
    auto first = s.heap->literal(std::string(4052, 'x'));
    auto last = s.heap->literal(empty ? "" : "a");
    auto result = s.write("[Ljava/lang/Object;",
                          s.array({first->Identity, last->Identity}));
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, empty ? uint32_t(-11) : uint32_t(-5));
    ASSERT_EQ(s.writes.back().size(), empty ? 4068u : 4061u);
    EXPECT_EQ(s.writes.back()[4], empty ? 3 : 2);
    if (empty)
      EXPECT_EQ(std::vector<uint8_t>(s.writes.back().end() - 5,
                                     s.writes.back().end()),
                (std::vector<uint8_t>{2, 0, 0, 0, 0}));
  }
}

TEST(AndroidEventLogTest, WorkAndServiceStopsNeverInventSuccessfulWrites) {
  for (unsigned limit = 0; limit < 9; ++limit) {
    unsigned budget = limit;
    EventState s;
    const auto text = s.heap->literal("abc")->Identity;
    auto result = s.write("[Ljava/lang/Object;", s.array({text, 0}), [&] {
      if (!budget)
        return ExecutionStop::InstructionLimit;
      --budget;
      return ExecutionStop::Returned;
    });
    EXPECT_EQ(result.stop, limit < 7 ? ExecutionStop::InstructionLimit
                                     : ExecutionStop::Returned);
    EXPECT_EQ(s.writes.size(), limit < 7 ? 0u : 1u);
  }
  EventState missing(false);
  EXPECT_EQ(missing.write("I", 1).stop, ExecutionStop::Unsupported);
  for (auto stop : {ExecutionStop::Timeout, ExecutionStop::InstructionLimit,
                    ExecutionStop::HeapLimit, ExecutionStop::Unsupported}) {
    EventState s;
    s.returned.stop = stop;
    EXPECT_EQ(s.write("I", 1).stop, stop);
    EXPECT_TRUE(s.runtime->snapshot().terminal);
  }
  EventState invalid;
  invalid.returned.value = uint64_t(1) << 32;
  EXPECT_EQ(invalid.write("I", 1).stop, ExecutionStop::Unsupported);
}

TEST(AndroidEventLogTest, ScalarCapacityIsAtomicForEachEncodedWidth) {
  for (bool wide : {false, true}) {
    for (unsigned length : {4048u, 4049u, 4052u, 4053u}) {
      EventState s;
      auto string = s.heap->literal(std::string(length, 'x'));
      const auto number = s.box(wide ? "java/lang/Long" : "java/lang/Integer",
                                wide ? "J" : "I", 42);
      auto result =
          s.write("[Ljava/lang/Object;", s.array({string->Identity, number}));
      ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
      const bool fits = length <= (wide ? 4048u : 4052u);
      EXPECT_EQ(result.value, fits ? uint32_t(-11) : uint32_t(-5));
      std::vector<uint8_t> expected{0x78, 0x56, 0x34, 0x12};
      if (fits)
        expected.insert(expected.end(), {3, 2});
      expected.insert(expected.end(),
                      {2, uint8_t(length), uint8_t(length >> 8), 0, 0});
      expected.insert(expected.end(), length, 'x');
      if (fits) {
        expected.insert(expected.end(), {uint8_t(wide ? 1 : 0), 42, 0, 0, 0});
        if (wide)
          expected.insert(expected.end(), {0, 0, 0, 0});
      }
      EXPECT_EQ(s.writes.back(), expected);
    }
  }
}

TEST(AndroidEventLogTest, DexInvocationsUseTheSameOverloadsAndFloatBits) {
  for (const char *type :
       {"I", "J", "F", "Ljava/lang/String;", "[Ljava/lang/Object;"}) {
    EventState s;
    uint64_t value = std::string_view(type) == "J"   ? 0x8877665544332211
                     : std::string_view(type) == "F" ? 0x7fc12345
                                                     : 42;
    if (referenceType(type)) {
      value = s.heap->literal("data")->Identity;
      if (type[0] == '[')
        value = s.array({value, 0});
    }
    Class caller;
    caller.name = "Ltest/EventCaller;";
    caller.superclass = "Ljava/lang/Object;";
    caller.access = {"public"};
    Method method;
    // An I argument supplies the exact float constant register bits. No
    // floating arithmetic or general floating method body is introduced.
    method.reference = {caller.name,
                        "run",
                        {"I", type == std::string_view("F") ? "I" : type},
                        "I"};
    method.access = {"public", "static"};
    method.registers = type == std::string_view("J") ? 3 : 2;
    Instruction invoke;
    invoke.pc = 0;
    invoke.opcode = "invoke-static";
    invoke.registers = {0, 1};
    if (method.registers == 3)
      invoke.registers.push_back(2);
    const MethodRef target{
        "Landroid/util/EventLog;", "writeEvent", {"I", type}, "I"};
    invoke.reference = target;
    Instruction move, ret;
    move.pc = 3;
    move.opcode = "move-result";
    move.registers = {0};
    ret.pc = 4;
    ret.opcode = "return";
    ret.registers = {0};
    method.instructions = {invoke, move, ret};
    method.code_end = 5;
    caller.methods = {method};
    Program program({caller}, std::map<std::string, std::string>{});
    Runtime runtime(program, s.heap, {}, s.services);
    const uint64_t args[] = {0x12345678, value};
    auto result = runtime.execute(*program.findStatic(method.reference), args,
                                  Admit, 65536);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, uint32_t(-11));
    result = runtime.library(*libraryMethod(target, true), 0, args, Admit);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    ASSERT_EQ(s.writes.size(), 2u);
    EXPECT_EQ(s.writes[0], s.writes[1]);
  }
}

TEST(AndroidEventLogTest, BoxConstructorsKeepFreshIdentityAndHeapLimits) {
  for (auto [name, type] : {std::pair{"java/lang/Integer", "I"},
                            {"java/lang/Long", "J"},
                            {"java/lang/Float", "F"}}) {
    EventState s;
    const auto first = s.box(name, type, 42), second = s.box(name, type, 42);
    EXPECT_NE(first, second);
    const auto *constructor = libraryMethod(
        {"L" + std::string(name) + ";", "<init>", {type}, "V"}, false);
    auto heap = std::make_shared<Heap>(0);
    Runtime limited(s.program, heap);
    const uint64_t args[] = {42};
    EXPECT_EQ(limited.library(*constructor, 0, args, Admit).stop,
              ExecutionStop::HeapLimit);
    const auto *equals = libraryMethod(
        {"Ljava/lang/Object;", "equals", {"Ljava/lang/Object;"}, "Z"}, false);
    const uint64_t equalArgs[] = {second};
    auto result = s.runtime->library(*equals, first, equalArgs, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  }
}
} // namespace
} // namespace neverd::mobile::dalvik
