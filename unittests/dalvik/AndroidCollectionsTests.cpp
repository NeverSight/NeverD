//===- AndroidCollectionsTests.cpp - Shared Android value services --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded HashMap values and read-only properties shared by JNI and DEX.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/dalvik/AndroidProperties.h"
#include "neverd/dalvik/Library.h"

#include <array>

namespace neverd::mobile::dalvik {
namespace {
using Properties = std::map<std::string, std::string>;
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };

const LibraryMethod &mapMethod(std::string_view name) {
  for (const auto &method : libraryMethods())
    if (method.Class == std::string_view("java/util/HashMap") &&
        method.Name == name)
      return method;
  std::abort();
}
struct MapState {
  Program program{{}, Properties{}};
  std::shared_ptr<Heap> heap = std::make_shared<Heap>();
  Runtime runtime{program, heap};
  uint64_t call(std::string_view name, uint64_t receiver = 0,
                std::initializer_list<uint64_t> args = {}) {
    auto result = runtime.library(mapMethod(name), receiver, args, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  }
};

TEST(AndroidHashMapTest, DefaultConstructorOwnsIndependentState) {
  Program program({}, Properties{});
  auto heap = std::make_shared<Heap>();
  Runtime runtime(program, heap);
  const auto *constructor =
      libraryMethod({"Ljava/util/HashMap;", "<init>", {}, "V"}, false);
  ASSERT_NE(constructor, nullptr);
  const auto *size =
      libraryMethod({"Ljava/util/HashMap;", "size", {}, "I"}, false);
  ASSERT_NE(size, nullptr);
  auto first = runtime.library(*constructor, 0, {}, Admit);
  auto second = runtime.library(*constructor, 0, {}, Admit);
  ASSERT_EQ(first.stop, ExecutionStop::Returned) << first.diagnostic;
  ASSERT_EQ(second.stop, ExecutionStop::Returned) << second.diagnostic;
  EXPECT_NE(first.value, second.value);
  auto result = runtime.library(*size, first.value, {}, Admit);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 0u);
}

TEST(AndroidHashMapTest, PutAllCopiesOverlapsNullsAndSelfWithoutAliasing) {
  MapState state;
  const auto source = state.call("<init>"), target = state.call("<init>");
  const auto key = state.heap->literal("key")->Identity;
  const auto other = state.heap->literal("other")->Identity;
  state.call("put", source, {key, source});
  state.call("put", source, {0, 0});
  state.call("put", target, {key, target});
  state.call("put", target, {other, source});
  state.call("putAll", target, {source});
  state.call("putAll", target, {target});
  EXPECT_EQ(state.call("size", target), 3u);
  EXPECT_EQ(state.call("get", target, {key}), source);
  EXPECT_EQ(state.call("containsKey", target, {0}), 1u);
  state.call("clear", source);
  EXPECT_EQ(state.call("get", target, {key}), source);
  EXPECT_EQ(state.call("size", target), 3u);
  const uint64_t args[] = {0};
  auto result = state.runtime.library(mapMethod("putAll"), target, args, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/NullPointerException");
}

TEST(AndroidPropertyTest, CatalogueRequiresExplicitRuntimeService) {
  Program program({}, Properties{});
  auto heap = std::make_shared<Heap>();
  Runtime runtime(program, heap);
  const auto *getter = libraryMethod({"Landroid/os/SystemProperties;",
                                      "getInt",
                                      {"Ljava/lang/String;", "I"},
                                      "I"},
                                     true);
  ASSERT_NE(getter, nullptr);
  const uint64_t args[] = {heap->literal("debug.example")->Identity, 41};
  auto result = runtime.library(*getter, 0, args, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("service is not supplied"),
            std::string::npos);
}

TEST(AndroidHashMapTest, StringValueKeysNullsCollisionsAndCycles) {
  MapState state;
  const auto first = state.call("<init>"), second = state.call("<init>");
  const auto key = state.heap->literal("Aa")->Identity;
  auto equal = state.heap->allocate(Object::String, "java/lang/String", 0);
  ASSERT_TRUE(state.heap->replaceBytes(*equal, javaStringBytes("Aa")));
  const auto collision = state.heap->literal("BB")->Identity;
  EXPECT_EQ(state.call("isEmpty", first), 1u);
  EXPECT_EQ(state.call("put", first, {key, first}), 0u);
  EXPECT_EQ(state.call("put", first, {collision, second}), 0u);
  EXPECT_EQ(state.call("put", first, {equal->Identity, 0}), first);
  EXPECT_EQ(state.call("size", first), 2u);
  EXPECT_EQ(state.call("get", first, {key}), 0u);
  EXPECT_EQ(state.call("containsKey", first, {equal->Identity}), 1u);
  EXPECT_EQ(state.call("containsKey", second, {key}), 0u);
  EXPECT_EQ(state.call("get", first, {collision}), second);
  EXPECT_EQ(state.call("put", first, {0, second}), 0u);
  EXPECT_EQ(state.call("get", first, {0}), second);
  EXPECT_EQ(state.call("size", first), 3u);
  EXPECT_EQ(state.call("remove", first, {0}), second);
  EXPECT_EQ(state.call("remove", first, {0}), 0u);
  EXPECT_EQ(state.call("remove", first, {equal->Identity}), 0u);
  EXPECT_EQ(state.call("containsKey", first, {key}), 0u);
  EXPECT_EQ(state.call("size", first), 1u);
  state.call("clear", first);
  EXPECT_EQ(state.call("isEmpty", first), 1u);
  EXPECT_EQ(state.call("get", first, {collision}), 0u);
  EXPECT_EQ(state.call("put", first, {key, second}), 0u);
  EXPECT_EQ(state.call("size", first), 1u);
  EXPECT_EQ(state.call("size", second), 0u);
  for (const auto *base : {"Ljava/util/AbstractMap;", "Ljava/util/Map;",
                           "Ljava/lang/Cloneable;", "Ljava/io/Serializable;"})
    EXPECT_TRUE(state.program.assignable("Ljava/util/HashMap;", base));
  EXPECT_FALSE(
      state.program.findClass("Ljava/util/HashMap;")->access.count("final"));
}

TEST(AndroidHashMapTest, UnknownKeysAndUnconstructedObjectsCannotSucceed) {
  for (const auto *name : {"get", "put", "containsKey", "remove"}) {
    MapState state;
    const auto map = state.call("<init>");
    const uint64_t args[] = {map, map}; // Requires custom hashCode/equals.
    auto result = state.runtime.library(
        mapMethod(name), map,
        std::span(args, std::string_view(name) == "put" ? 2 : 1), Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_TRUE(state.heap->get(map)->MapEntries->empty());
    EXPECT_EQ(state.runtime.library(mapMethod("size"), map, {}, Admit).stop,
              ExecutionStop::Unsupported);
  }
  for (bool initialized : {false, true}) {
    MapState state;
    auto raw = state.heap->allocate(Object::Instance, "java/util/HashMap", 0);
    raw->Initialized = initialized;
    auto result =
        state.runtime.library(mapMethod("size"), raw->Identity, {}, Admit);
    EXPECT_EQ(result.stop, initialized ? ExecutionStop::Unsupported
                                       : ExecutionStop::Invalid);
  }
  MapState state;
  const auto map = state.call("<init>");
  EXPECT_EQ(state.runtime.library(mapMethod("<init>"), map, {}, Admit).stop,
            ExecutionStop::Invalid);
}

TEST(AndroidHashMapTest, KeysPreserveNulAndIsolatedSurrogateUnits) {
  MapState state;
  const auto map = state.call("<init>");
  const std::vector<std::vector<uint8_t>> keys = {
      {}, {0, 0}, {0, 0xd8}, {0xfd, 0xff}, {0x3d, 0xd8, 0, 0xde}};
  std::vector<uint32_t> values;
  for (const auto &bytes : keys) {
    auto key = state.heap->allocate(Object::String, "java/lang/String", 0);
    ASSERT_TRUE(state.heap->replaceBytes(*key, bytes));
    values.push_back(key->Identity);
    EXPECT_EQ(state.call("put", map, {key->Identity, key->Identity}), 0u);
  }
  EXPECT_EQ(state.call("size", map), keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    auto equal = state.heap->allocate(Object::String, "java/lang/String", 0);
    ASSERT_TRUE(state.heap->replaceBytes(*equal, keys[i]));
    EXPECT_EQ(state.call("get", map, {equal->Identity}), values[i]);
  }
}

TEST(AndroidHashMapTest, SubclassesStayUnsupportedAndNullReceiverThrows) {
  Class subclass;
  subclass.name = "Ltest/CustomMap;";
  subclass.superclass = "Ljava/util/HashMap;";
  subclass.access = {"public"};
  Program program({subclass}, Properties{});
  auto heap = std::make_shared<Heap>();
  auto custom = heap->allocate(Object::Instance, "test/CustomMap", 0);
  Runtime runtime(program, heap);
  auto result = runtime.library(mapMethod("size"), 0, {}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/NullPointerException");
  result = runtime.library(mapMethod("size"), custom->Identity, {}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("subclass behavior"), std::string::npos);
}

TEST(AndroidHashMapTest, EqualityDoesNotFallBackToObjectIdentity) {
  for (bool nonvirtual : {false, true}) {
    MapState state;
    const auto map = state.call("<init>");
    const auto *equals = libraryMethod(
        {"Ljava/lang/Object;", "equals", {"Ljava/lang/Object;"}, "Z"}, false);
    ASSERT_NE(equals, nullptr);
    const uint64_t args[] = {map};
    auto result = state.runtime.library(*equals, map, args, Admit, nonvirtual);
    EXPECT_EQ(result.stop, nonvirtual ? ExecutionStop::Returned
                                      : ExecutionStop::Unsupported);
    if (nonvirtual)
      EXPECT_EQ(result.value, 1u);
  }
}

TEST(AndroidHashMapTest, WorkAndHeapStopsDoNotPublishMutation) {
  for (const auto *name : {"put", "remove", "clear"}) {
    MapState state;
    const auto map = state.call("<init>");
    const auto key = state.heap->literal("key")->Identity;
    state.call("put", map, {key, map});
    const uint64_t args[] = {key, 0};
    unsigned ticks = 0;
    const unsigned count = std::string_view(name) == "put"      ? 2
                           : std::string_view(name) == "remove" ? 1
                                                                : 0;
    auto result = state.runtime.library(
        mapMethod(name), map, std::span(args, count), [&] {
          return ++ticks == 2 ? ExecutionStop::InstructionLimit
                              : ExecutionStop::Returned;
        });
    EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
    ASSERT_EQ(state.heap->get(map)->MapEntries->size(), 1u);
    EXPECT_EQ(state.heap->get(map)->MapEntries->front().Value, map);
  }
  // Exactly enough for the constructed empty map, but no first entry.
  auto heap = std::make_shared<Heap>(
      sizeof(Object) + std::string_view("java/util/HashMap").size() + 64);
  Program program({}, Properties{});
  Runtime runtime(program, heap);
  auto map = runtime.library(mapMethod("<init>"), 0, {}, Admit);
  ASSERT_EQ(map.stop, ExecutionStop::Returned);
  const uint64_t args[] = {0, map.value};
  auto result = runtime.library(mapMethod("put"), map.value, args, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::HeapLimit);
  EXPECT_TRUE(heap->get(map.value)->MapEntries->empty());
}

Instruction instruction(uint32_t pc, const char *opcode,
                        std::initializer_list<unsigned> registers) {
  Instruction result;
  result.pc = pc;
  result.opcode = opcode;
  result.registers = registers;
  return result;
}
TEST(AndroidHashMapTest, DexConstructionAndReadsShareLibraryMutation) {
  Class caller;
  caller.name = "Ltest/MapCaller;";
  caller.superclass = "Ljava/lang/Object;";
  caller.access = {"public"};
  Method make;
  make.reference = {caller.name, "make", {}, "Ljava/util/HashMap;"};
  make.access = {"public", "static"};
  make.registers = 1;
  auto allocation = instruction(0, "new-instance", {0});
  allocation.reference = std::string("Ljava/util/HashMap;");
  auto init = instruction(2, "invoke-direct", {0});
  init.reference = MethodRef{"Ljava/util/HashMap;", "<init>", {}, "V"};
  make.instructions = {allocation, init, instruction(5, "return-object", {0})};
  make.code_end = 6;
  Method read;
  read.reference = {caller.name,
                    "read",
                    {"Ljava/util/HashMap;", "Ljava/lang/String;"},
                    "Ljava/lang/Object;"};
  read.access = {"public", "static"};
  read.registers = 2;
  auto get = instruction(0, "invoke-interface", {0, 1});
  get.reference = MethodRef{
      "Ljava/util/Map;", "get", {"Ljava/lang/Object;"}, "Ljava/lang/Object;"};
  read.instructions = {get, instruction(3, "move-result-object", {0}),
                       instruction(4, "return-object", {0})};
  read.code_end = 5;
  caller.methods = {make, read};
  Program program({caller}, Properties{});
  auto heap = std::make_shared<Heap>();
  Runtime runtime(program, heap);
  auto map =
      runtime.execute(*program.findStatic(make.reference), {}, Admit, 65536);
  ASSERT_EQ(map.stop, ExecutionStop::Returned) << map.diagnostic;
  const auto key = heap->literal("key")->Identity;
  const uint64_t put_args[] = {key, map.value}, get_args[] = {map.value, key};
  auto result = runtime.library(mapMethod("put"), map.value, put_args, Admit);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  result = runtime.execute(*program.findStatic(read.reference), get_args, Admit,
                           65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, map.value);
}

TEST(AndroidPropertyTest, IntegerParsingUsesApi28PrefixAndSignedBounds) {
  for (const auto &[text, expected] :
       std::array<std::pair<const char *, int32_t>, 9>{
           {{"28", 28},
            {"028", 28},
            {"0x1c", 28},
            {"0X7FFFFFFF", INT32_MAX},
            {"\t +28", 28},
            {"-2147483648", INT32_MIN},
            {"2147483647", INT32_MAX},
            {"-0", 0},
            {"\n\v\r\f 12", 12}}})
    EXPECT_EQ(androidPropertyInt(text, 41), expected) << text;
  for (const auto *text : {"", " ", "+", "-", "0x", "0x80000000", "2147483648",
                           "-2147483649", "1 ", " 0x1c", "+0x1c", "-0x1c", "1z",
                           "--1", "+-1", "999999999999999999999999999"})
    EXPECT_EQ(androidPropertyInt(text, -41), -41) << text;
}

TEST(AndroidPropertyTest, KeyEncodingDefaultBitsNullAndDexShareOneService) {
  Class caller;
  caller.name = "Ltest/PropertyCaller;";
  caller.superclass = "Ljava/lang/Object;";
  caller.access = {"public"};
  Method method;
  method.reference = {caller.name, "get", {"Ljava/lang/String;", "I"}, "I"};
  method.access = {"public", "static"};
  method.registers = 2;
  auto invoke = instruction(0, "invoke-static", {0, 1});
  MethodRef getter{"Landroid/os/SystemProperties;",
                   "getInt",
                   {"Ljava/lang/String;", "I"},
                   "I"};
  invoke.reference = getter;
  method.instructions = {invoke, instruction(3, "move-result", {0}),
                         instruction(4, "return", {0})};
  method.code_end = 5;
  caller.methods = {method};
  Program program({caller}, Properties{});
  auto heap = std::make_shared<Heap>();
  auto key = heap->allocate(Object::String, "java/lang/String", 0);
  // NUL, isolated high surrogate, then a complete supplementary pair.
  ASSERT_TRUE(
      heap->replaceBytes(*key, {0, 0, 0, 0xd8, 'a', 0, 0x3d, 0xd8, 0, 0xde}));
  unsigned calls = 0;
  RuntimeServices services;
  services.property_integer = [&](std::string_view name, int32_t fallback) {
    ++calls;
    EXPECT_EQ(name, std::string("\xc0\x80\xed\xa0\x80"
                                "a\xf0\x9f\x98\x80"));
    EXPECT_EQ(fallback, INT32_MIN);
    return ExecutionResult{ExecutionStop::Returned, uint32_t(-7)};
  };
  Runtime runtime(program, heap, {}, services);
  const uint64_t args[] = {key->Identity, uint32_t(INT32_MIN)};
  const auto *library = libraryMethod(getter, true);
  ASSERT_NE(library, nullptr);
  auto result = runtime.library(*library, 0, args, Admit);
  ASSERT_EQ(result.stop, ExecutionStop::Returned);
  EXPECT_EQ(result.value, uint32_t(-7));
  result = runtime.execute(*program.findStatic(method.reference), args, Admit,
                           65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(uint32_t(result.value), uint32_t(-7));
  const uint64_t null_args[] = {0, 0};
  result = runtime.library(*library, 0, null_args, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/NullPointerException");
  EXPECT_EQ(calls, 2u);
  EXPECT_FALSE(program.findClass("Landroid/os/SystemProperties;")
                   ->access.count("final"));
}

TEST(AndroidPropertyTest, ResourceStopsAreTerminalAndPreserveServiceResult) {
  Program program({}, Properties{});
  auto heap = std::make_shared<Heap>();
  const auto *getter = libraryMethod({"Landroid/os/SystemProperties;",
                                      "getInt",
                                      {"Ljava/lang/String;", "I"},
                                      "I"},
                                     true);
  ASSERT_NE(getter, nullptr);
  const uint64_t args[] = {heap->literal("abc")->Identity, 0};
  for (const auto stop :
       {ExecutionStop::InstructionLimit, ExecutionStop::Timeout,
        ExecutionStop::Invalid, ExecutionStop::Unsupported}) {
    unsigned reads = 0;
    RuntimeServices services;
    services.property_integer = [&](std::string_view, int32_t) {
      ++reads;
      return ExecutionResult{stop, 0, {}, "property owner stopped"};
    };
    Runtime runtime(program, heap, {}, services);
    for (unsigned repeat = 0; repeat != 2; ++repeat) {
      auto result = runtime.library(*getter, 0, args, Admit);
      EXPECT_EQ(result.stop, stop);
      EXPECT_EQ(result.diagnostic, "property owner stopped");
    }
    EXPECT_EQ(reads, 1u);
    Runtime fresh(program, heap, {}, services);
    unsigned ticks = 0;
    auto result = fresh.library(*getter, 0, args, [&] {
      return ++ticks == 2 ? ExecutionStop::InstructionLimit
                          : ExecutionStop::Returned;
    });
    EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
    EXPECT_EQ(reads, 1u);
  }
}
} // namespace
} // namespace neverd::mobile::dalvik
