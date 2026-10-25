//===- AndroidFrameworkTests.cpp - Application and loader state ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"
#include "neverd/dalvik/Limits.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
using Phase = AndroidApplicationSnapshot::Phase;
using Properties = std::map<std::string, std::string>;
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
const LibraryMethod &operation(LibraryOperation op) {
  for (const auto &method : libraryMethods())
    if (method.Op == op)
      return method;
  std::abort();
}
class AndroidFrameworkTest : public testing::Test {
protected:
  Program program{{}, Properties{}};
  std::shared_ptr<Heap> heap = std::make_shared<Heap>();
  Runtime runtime{program, heap,
                  AndroidApplicationSnapshot{Phase::Application, "test.app"}};
  uint64_t call(LibraryOperation op, uint64_t receiver = 0,
                std::initializer_list<uint64_t> args = {}) {
    auto result = runtime.library(operation(op), receiver, args, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  }
};
using Instrumentation = AndroidApplicationSnapshot::Instrumentation;
using LookupFailure = AndroidApplicationSnapshot::ClassLookupFailure;
using Loader = LookupFailure::Loader;
const LibraryMethod &handlerConstructor() {
  return *libraryMethod(
      {"Landroid/os/Handler;", "<init>", {"Landroid/os/Looper;"}, "V"}, false);
}
AndroidApplicationSnapshot messageInput() {
  AndroidApplicationSnapshot input;
  input.main_looper_prepared = true;
  input.initial_main_queue.emplace(
      AndroidApplicationSnapshot::EmptyMessageQueue{});
  input.initial_message_pool_empty = true;
  return input;
}
uint64_t makeHandler(Runtime &state) {
  auto looper = state.library(operation(MainLooper), 0, {}, Admit);
  EXPECT_EQ(looper.stop, ExecutionStop::Returned) << looper.diagnostic;
  auto handler = state.library(handlerConstructor(), 0,
                               std::array<uint64_t, 1>{looper.value}, Admit);
  EXPECT_EQ(handler.stop, ExecutionStop::Returned) << handler.diagnostic;
  return handler.value;
}
const Field *messageField(const Program &program, const char *name,
                          const char *type) {
  return program.findField({"Landroid/os/Message;", name, type}, false);
}
TEST_F(AndroidFrameworkTest, HandlerPostInputsKeepUnknownAndNullBoundaries) {
  for (unsigned mode = 0; mode != 5; ++mode) {
    auto input = messageInput();
    RuntimeServices services;
    unsigned reads = 0;
    if (mode != 2)
      services.uptime_milliseconds = [&] {
        ++reads;
        return ExecutionResult{ExecutionStop::Returned, 123};
      };
    if (mode == 0)
      input.initial_message_pool_empty.reset();
    if (mode == 1)
      input.initial_message_pool_empty = false;
    if (mode == 3)
      input.initial_main_queue.reset();
    Runtime state(program, heap, input, std::move(services));
    auto handler =
        mode == 4
            ? state.allocate("Landroid/os/Handler;", true, Admit, 65536).value
            : makeHandler(state);
    auto result = state.library(operation(HandlerPost), handler,
                                std::array<uint64_t, 1>{0}, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    const char *diagnostics[] = {
        "Android Message pool state is not supplied",
        "Android nonempty Message pool is unmodeled",
        "Android uptime service is not supplied",
        "Android Handler queue state is not supplied",
        "Android Handler null-queue warning requires logging"};
    EXPECT_EQ(result.diagnostic, diagnostics[mode]);
    EXPECT_EQ(reads, mode >= 3 ? 1u : 0u);
    EXPECT_TRUE(state.snapshot().terminal);
    auto snapshot = state.snapshot().framework;
    if (snapshot.main_message_queue)
      EXPECT_TRUE(snapshot.main_message_queue->messages.empty());
  }
  auto bad = messageInput();
  bad.main_looper_prepared = false;
  EXPECT_THROW(Runtime(program, heap, bad), Error);
}
TEST_F(AndroidFrameworkTest, HandlerPostKeepsStableOrderAndMessageFields) {
  uint64_t now = 1200;
  Runtime state(program, heap, messageInput(), {[&] {
                  return ExecutionResult{ExecutionStop::Returned, now};
                }});
  auto first = makeHandler(state), second = makeHandler(state);
  std::vector<uint32_t> identities;
  for (auto [time, target] : {std::pair{1200u, first},
                              {1200u, second},
                              {900u, first},
                              {0u, second}}) {
    now = time;
    auto result = state.library(operation(HandlerPost), target,
                                std::array<uint64_t, 1>{0}, Admit);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, 1u);
    auto messages = state.snapshot().framework.main_message_queue->messages;
    auto it = std::find_if(messages.begin(), messages.end(), [&](auto row) {
      return std::find(identities.begin(), identities.end(), row.identity) ==
             identities.end();
    });
    ASSERT_NE(it, messages.end());
    identities.push_back(it->identity);
  }
  const auto queue = *state.snapshot().framework.main_message_queue;
  ASSERT_EQ(queue.messages.size(), 4u);
  EXPECT_TRUE(queue.complete);
  EXPECT_FALSE(queue.blocked);
  const unsigned order[] = {3, 2, 0, 1};
  const int64_t times[] = {0, 900, 1200, 1200};
  for (unsigned i = 0; i != 4; ++i) {
    const auto &row = queue.messages[i];
    EXPECT_EQ(row.identity, identities[order[i]]);
    EXPECT_EQ(row.when, times[i]);
    EXPECT_EQ(row.flags, 1u);
    EXPECT_EQ(row.callback, 0u);
    EXPECT_EQ(row.next, i == 3 ? 0u : identities[order[i + 1]]);
    auto object = heap->get(row.identity);
    ASSERT_EQ(object->Fields.size(), 12u);
    for (const auto &[field, value] : object->Fields) {
      const auto &name = field->reference.name;
      if (name == "sendingUid")
        EXPECT_EQ(value, UINT32_MAX);
      else if (name != "flags" && name != "when" && name != "target" &&
               name != "next")
        EXPECT_EQ(value, 0u) << name;
    }
  }
  EXPECT_EQ(queue.messages[0].target, second);
  EXPECT_EQ(queue.messages[1].target, first);
  EXPECT_EQ(queue.messages[2].target, first);
  EXPECT_EQ(queue.messages[3].target, second);
}
TEST_F(AndroidFrameworkTest, HandlerPostBlockedQueueRetainsEnqueueBeforeWake) {
  auto input = messageInput();
  input.initial_main_queue->blocked = true;
  Runtime state(program, heap, input,
                {[] { return ExecutionResult{ExecutionStop::Returned, 77}; }});
  const auto handler = makeHandler(state);
  auto result = state.library(operation(HandlerPost), handler,
                              std::array<uint64_t, 1>{0}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_EQ(result.diagnostic, "Android MessageQueue.nativeWake is unmodeled");
  const auto snapshot = state.snapshot();
  EXPECT_TRUE(snapshot.terminal);
  const auto &queue = *snapshot.framework.main_message_queue;
  ASSERT_EQ(queue.messages.size(), 1u);
  EXPECT_TRUE(queue.complete);
  EXPECT_TRUE(queue.blocked);
  EXPECT_EQ(queue.messages[0].target, handler);
  EXPECT_EQ(queue.messages[0].when, 77);
  EXPECT_EQ(queue.messages[0].flags, 1u);
}
TEST_F(AndroidFrameworkTest,
       HandlerPostAdmitsFinalMethodAndRefusesOverrideBypass) {
  for (bool final : {false, true}) {
    Class type;
    type.name = "Ltest/HandlerSubclass;";
    type.superclass = "Landroid/os/Handler;";
    type.access = {"public"};
    Method override;
    override.reference =
        final ? MethodRef{type.name, "post", {"Ljava/lang/Runnable;"}, "Z"}
              : MethodRef{type.name,
                          "sendMessageAtTime",
                          {"Landroid/os/Message;", "J"},
                          "Z"};
    override.access = {"public", "native"};
    type.methods.push_back(override);
    Program declarations({type}, Properties{});
    EXPECT_EQ(declarations.unsupportedClass(type.name).has_value(), final);
    if (final)
      continue;
    Runtime state(declarations, heap, messageInput(),
                  {[] { return ExecutionResult{ExecutionStop::Returned, 1}; }});
    auto looper = state.library(operation(MainLooper), 0, {}, Admit).value;
    auto handler = state.allocate(type.name, false, Admit, 65536);
    ASSERT_EQ(handler.stop, ExecutionStop::Returned);
    auto made = state.library(handlerConstructor(), handler.value,
                              std::array<uint64_t, 1>{looper}, Admit);
    ASSERT_EQ(made.stop, ExecutionStop::Returned);
    heap->get(handler.value)->Initialized = true;
    auto result = state.library(operation(HandlerPost), handler.value,
                                std::array<uint64_t, 1>{0}, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_EQ(result.diagnostic,
              "Android Handler.sendMessageAtTime override requires execution");
    EXPECT_TRUE(
        state.snapshot().framework.main_message_queue->messages.empty());
  }
}
TEST_F(AndroidFrameworkTest, HandlerPostDexRetainsRunnableWithoutExecutingIt) {
  Class action;
  action.name = "Ltest/PostedAction;";
  action.superclass = "Ljava/lang/Object;";
  action.interfaces = {"Ljava/lang/Runnable;"};
  action.access = {"public"};
  Field called;
  called.reference = {action.name, "called", "I"};
  called.access = {"public", "static"};
  action.fields.push_back(called);
  Method run;
  run.reference = {action.name, "run", {}, "V"};
  run.access = {"public"};
  run.registers = 2;
  Instruction constant;
  constant.pc = 0;
  constant.opcode = "const/4";
  constant.registers = {0};
  constant.literal = int64_t(1);
  Instruction store;
  store.pc = 1;
  store.opcode = "sput";
  store.registers = {0};
  store.reference = called.reference;
  Instruction ret;
  ret.pc = 3;
  ret.opcode = "return-void";
  run.instructions = {constant, store, ret};
  run.code_end = 4;
  action.methods.push_back(run);
  Method post;
  post.reference = {action.name,
                    "postIt",
                    {"Landroid/os/Handler;", "Ljava/lang/Runnable;"},
                    "Z"};
  post.access = {"public", "static"};
  post.registers = 3;
  Instruction invoke;
  invoke.pc = 0;
  invoke.opcode = "invoke-virtual";
  invoke.registers = {1, 2};
  invoke.reference =
      MethodRef{"Landroid/os/Handler;", "post", {"Ljava/lang/Runnable;"}, "Z"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result";
  move.registers = {0};
  ret.pc = 4;
  ret.opcode = "return";
  ret.registers = {0};
  post.instructions = {invoke, move, ret};
  post.code_end = 5;
  action.methods.push_back(post);
  Program declarations({action}, Properties{});
  Runtime state(declarations, heap, messageInput(),
                {[] { return ExecutionResult{ExecutionStop::Returned, 555}; }});
  const auto callback = state.allocate(action.name, true, Admit, 65536).value;
  const auto handler = makeHandler(state);
  uint64_t args[] = {handler, callback};
  auto result = state.execute(*declarations.findStatic(post.reference), args,
                              Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 1u);
  const auto *flag = declarations.findStaticField(called.reference);
  EXPECT_EQ(state.field(*flag, 0, {}, Admit, 65536).value, 0u);
  const auto queue = *state.snapshot().framework.main_message_queue;
  ASSERT_EQ(queue.messages.size(), 1u);
  EXPECT_EQ(queue.messages[0].callback, callback);
  EXPECT_EQ(queue.messages[0].target, handler);
  auto direct = state.execute(*declarations.findMethod(run.reference, false),
                              {&callback, 1}, Admit, 65536);
  EXPECT_EQ(direct.stop, ExecutionStop::Returned) << direct.diagnostic;
  EXPECT_EQ(state.field(*flag, 0, {}, Admit, 65536).value, 1u);
  EXPECT_EQ(state.snapshot().framework.main_message_queue->messages,
            queue.messages);
}
TEST_F(AndroidFrameworkTest,
       HandlerPostUsesMutableHeapLinksAndBoundsSnapshots) {
  Runtime state(program, heap, messageInput(),
                {[] { return ExecutionResult{ExecutionStop::Returned, 5}; }});
  auto handler = makeHandler(state);
  ASSERT_EQ(state
                .library(operation(HandlerPost), handler,
                         std::array<uint64_t, 1>{0}, Admit)
                .stop,
            ExecutionStop::Returned);
  auto id = state.snapshot().framework.main_message_queue->messages[0].identity;
  auto *next = messageField(program, "next", "Landroid/os/Message;");
  ASSERT_EQ(state.field(*next, id, id, Admit, 65536).stop,
            ExecutionStop::Returned);
  auto snapshot = state.snapshot();
  EXPECT_FALSE(snapshot.framework.main_message_queue->complete);
  EXPECT_EQ(snapshot.framework.main_message_queue->messages[0].next, id);
  auto result = state.library(operation(HandlerPost), handler,
                              std::array<uint64_t, 1>{0}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Invalid);
  EXPECT_EQ(result.diagnostic, "Android Message queue contains a cycle");
  EXPECT_TRUE(state.snapshot().terminal);
}
TEST_F(AndroidFrameworkTest, HandlerPostPreservesLimitsAndClockFailures) {
  for (auto stop : {ExecutionStop::InstructionLimit, ExecutionStop::Timeout,
                    ExecutionStop::HeapLimit}) {
    Runtime state(program, heap, messageInput(),
                  {[=] { return ExecutionResult{stop, 0, {}, "clock stop"}; }});
    auto handler = makeHandler(state);
    auto result = state.library(operation(HandlerPost), handler,
                                std::array<uint64_t, 1>{0}, Admit);
    EXPECT_EQ(result.stop, stop);
    EXPECT_TRUE(
        state.snapshot().framework.main_message_queue->messages.empty());
    EXPECT_EQ(state
                  .library(operation(HandlerPost), handler,
                           std::array<uint64_t, 1>{0}, Admit)
                  .stop,
              stop);
  }
  Runtime limited(program, heap, messageInput(),
                  {[] { return ExecutionResult{ExecutionStop::Returned, 4}; }});
  auto handler = makeHandler(limited);
  unsigned ticks = 0;
  auto result = limited.library(
      operation(HandlerPost), handler, std::array<uint64_t, 1>{0}, [&] {
        return ++ticks < 4 ? ExecutionStop::Returned
                           : ExecutionStop::InstructionLimit;
      });
  EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
  EXPECT_TRUE(
      limited.snapshot().framework.main_message_queue->messages.empty());
}
TEST_F(AndroidFrameworkTest, HandlerPostQueueCapacityIsNotSuccessOrDispatch) {
  Runtime state(program, heap, messageInput(),
                {[] { return ExecutionResult{ExecutionStop::Returned, 3}; }});
  auto handler = makeHandler(state);
  for (unsigned i = 0; i < 1024; ++i) {
    auto result = state.library(operation(HandlerPost), handler,
                                std::array<uint64_t, 1>{0}, Admit);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << i << result.diagnostic;
  }
  auto result = state.library(operation(HandlerPost), handler,
                              std::array<uint64_t, 1>{0}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::HeapLimit);
  EXPECT_EQ(result.diagnostic, "Android Message queue limit exhausted");
  EXPECT_EQ(state.snapshot().framework.main_message_queue->messages.size(),
            1024u);
}
TEST_F(AndroidFrameworkTest,
       HandlerPostRejectsWrongReferencesAndExhaustedHeap) {
  {
    Runtime state(program, heap, messageInput());
    auto result = state.library(operation(HandlerPost), 0,
                                std::array<uint64_t, 1>{0}, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Thrown);
    EXPECT_EQ(result.exception, "java/lang/NullPointerException");
    EXPECT_FALSE(state.snapshot().terminal);
  }
  {
    Runtime state(program, heap, messageInput());
    auto handler = makeHandler(state);
    auto text = heap->literal("not a Runnable");
    auto result = state.library(operation(HandlerPost), handler,
                                std::array<uint64_t, 1>{text->Identity}, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Invalid);
    EXPECT_TRUE(
        state.snapshot().framework.main_message_queue->messages.empty());
  }
  {
    auto small = std::make_shared<Heap>(4096, 3);
    Runtime state(program, small, messageInput(),
                  {[] { return ExecutionResult{ExecutionStop::Returned, 1}; }});
    auto handler = makeHandler(state);
    auto result = state.library(operation(HandlerPost), handler,
                                std::array<uint64_t, 1>{0}, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::HeapLimit);
    EXPECT_TRUE(
        state.snapshot().framework.main_message_queue->messages.empty());
  }
}
TEST_F(AndroidFrameworkTest, MainLooperStatesDoNotInferApplicationReadiness) {
  for (auto phase : {Phase::Unattached, Phase::Attached, Phase::Application}) {
    AndroidApplicationSnapshot input{phase};
    Runtime unknown(program, heap, input);
    auto missing = unknown.library(operation(MainLooper), 0, {}, Admit);
    EXPECT_EQ(missing.stop, ExecutionStop::Unsupported);
    EXPECT_EQ(missing.diagnostic, "Android main Looper state is not supplied");
    input.main_looper_prepared = false;
    Runtime unprepared(program, heap, input);
    auto absent = unprepared.library(operation(MainLooper), 0, {}, Admit);
    EXPECT_EQ(absent.stop, ExecutionStop::Returned);
    EXPECT_EQ(absent.value, 0u);
    input.main_looper_prepared = true;
    Runtime prepared(program, heap, input);
    auto looper = prepared.library(operation(MainLooper), 0, {}, Admit);
    ASSERT_EQ(looper.stop, ExecutionStop::Returned);
    ASSERT_NE(looper.value, 0u);
    EXPECT_EQ(heap->get(looper.value)->ClassName, "android/os/Looper");
    EXPECT_EQ(prepared.library(operation(MainLooper), 0, {}, Admit).value,
              looper.value);
    Runtime other(program, heap, input);
    EXPECT_NE(other.library(operation(MainLooper), 0, {}, Admit).value,
              looper.value);
  }
}
TEST_F(AndroidFrameworkTest, MainLooperHandlerConstructionRetainsExactFields) {
  AndroidApplicationSnapshot input;
  input.main_looper_prepared = true;
  Runtime state(program, heap, input);
  const auto looper = state.library(operation(MainLooper), 0, {}, Admit).value;
  const uint64_t args[] = {looper};
  auto first = state.library(handlerConstructor(), 0, args, Admit);
  auto second = state.library(handlerConstructor(), 0, args, Admit);
  ASSERT_EQ(first.stop, ExecutionStop::Returned) << first.diagnostic;
  ASSERT_EQ(second.stop, ExecutionStop::Returned);
  EXPECT_NE(first.value, second.value);
  const auto *queue = program.findField(
      {"Landroid/os/Looper;", "mQueue", "Landroid/os/MessageQueue;"}, false);
  const auto queue_id = heap->get(looper)->Fields.at(queue);
  ASSERT_NE(queue_id, 0u);
  EXPECT_EQ(heap->get(queue_id)->ClassName, "android/os/MessageQueue");
  for (const auto id : {first.value, second.value}) {
    EXPECT_TRUE(heap->get(id)->Initialized);
    for (const auto &[name, type, expected] :
         {std::tuple{"mLooper", "Landroid/os/Looper;", looper},
          {"mQueue", "Landroid/os/MessageQueue;", queue_id},
          {"mCallback", "Landroid/os/Handler$Callback;", uint64_t(0)},
          {"mAsynchronous", "Z", uint64_t(0)}}) {
      const auto *field =
          program.findField({"Landroid/os/Handler;", name, type}, false);
      ASSERT_NE(field, nullptr);
      EXPECT_EQ(heap->get(id)->Fields.at(field), expected);
    }
  }
}
TEST_F(AndroidFrameworkTest, MainLooperHandlerDistinguishesNullZeroAndUnknown) {
  Runtime zero(program, heap);
  auto blank = zero.allocate("Landroid/os/Looper;", true, Admit, 65536);
  ASSERT_EQ(blank.stop, ExecutionStop::Returned);
  const uint64_t args[] = {blank.value};
  auto handler = zero.library(handlerConstructor(), 0, args, Admit);
  ASSERT_EQ(handler.stop, ExecutionStop::Returned) << handler.diagnostic;
  const auto *looper_field = program.findField(
      {"Landroid/os/Handler;", "mLooper", "Landroid/os/Looper;"}, false);
  EXPECT_EQ(zero.field(*looper_field, handler.value, {}, Admit, 65536).value,
            blank.value);
  const auto *queue = program.findField(
      {"Landroid/os/Handler;", "mQueue", "Landroid/os/MessageQueue;"}, false);
  EXPECT_EQ(heap->get(handler.value)->Fields.at(queue), 0u);
  auto blank_handler =
      zero.allocate("Landroid/os/Handler;", true, Admit, 65536);
  ASSERT_EQ(blank_handler.stop, ExecutionStop::Returned);
  EXPECT_EQ(
      zero.field(*looper_field, blank_handler.value, {}, Admit, 65536).value,
      0u);
  const uint64_t null_args[] = {0};
  auto null = zero.library(handlerConstructor(), 0, null_args, Admit);
  EXPECT_EQ(null.stop, ExecutionStop::Thrown);
  EXPECT_EQ(null.exception, "java/lang/NullPointerException");
  Runtime foreign(program, heap);
  auto opaque = heap->allocate(Object::Instance, "android/os/Looper", 0);
  ASSERT_TRUE(opaque);
  const uint64_t unknown_args[] = {opaque->Identity};
  auto missing = foreign.library(handlerConstructor(), 0, unknown_args, Admit);
  EXPECT_EQ(missing.stop, ExecutionStop::Unsupported);
  EXPECT_EQ(missing.diagnostic, "Android Looper queue field is not supplied");
}
TEST_F(AndroidFrameworkTest, MainLooperPreservesTerminalResourceLimits) {
  for (auto stop : {ExecutionStop::InstructionLimit, ExecutionStop::Timeout}) {
    AndroidApplicationSnapshot input;
    input.main_looper_prepared = true;
    Runtime state(program, heap, input);
    auto result =
        state.library(operation(MainLooper), 0, {}, [=] { return stop; });
    EXPECT_EQ(result.stop, stop);
    EXPECT_EQ(state.library(operation(MainLooper), 0, {}, Admit).stop, stop);
    EXPECT_TRUE(state.snapshot().terminal);
  }
  AndroidApplicationSnapshot input;
  input.main_looper_prepared = true;
  auto limited = std::make_shared<Heap>(65536, 1);
  Runtime state(program, limited, input);
  EXPECT_EQ(state.library(operation(MainLooper), 0, {}, Admit).stop,
            ExecutionStop::HeapLimit);
  EXPECT_EQ(state.library(operation(MainLooper), 0, {}, Admit).stop,
            ExecutionStop::HeapLimit);
  EXPECT_TRUE(state.snapshot().terminal);
}
TEST_F(AndroidFrameworkTest, MainLooperDexConstructorUsesTheSharedOwner) {
  Class caller;
  caller.name = "Ltest/HandlerCaller;";
  caller.superclass = "Ljava/lang/Object;";
  caller.access = {"public"};
  Method method;
  method.reference = {caller.name, "make", {}, "Landroid/os/Handler;"};
  method.access = {"public", "static"};
  method.registers = 3;
  Instruction allocation;
  allocation.pc = 0;
  allocation.opcode = "new-instance";
  allocation.registers = {0};
  allocation.reference = std::string("Landroid/os/Handler;");
  Instruction main;
  main.pc = 2;
  main.opcode = "invoke-static";
  main.reference = MethodRef{
      "Landroid/os/Looper;", "getMainLooper", {}, "Landroid/os/Looper;"};
  Instruction move;
  move.pc = 5;
  move.opcode = "move-result-object";
  move.registers = {1};
  Instruction construct;
  construct.pc = 6;
  construct.opcode = "invoke-direct";
  construct.registers = {0, 1};
  construct.reference =
      MethodRef{"Landroid/os/Handler;", "<init>", {"Landroid/os/Looper;"}, "V"};
  Instruction ret;
  ret.pc = 9;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {allocation, main, move, construct, ret};
  method.code_end = 10;
  caller.methods.push_back(method);
  Program declarations({caller}, Properties{});
  for (bool prepared : {false, true}) {
    AndroidApplicationSnapshot input;
    input.main_looper_prepared = prepared;
    Runtime state(declarations, heap, input);
    auto execution = state.execute(*declarations.findStatic(method.reference),
                                   {}, Admit, 65536);
    if (prepared) {
      ASSERT_EQ(execution.stop, ExecutionStop::Returned)
          << execution.diagnostic;
      const auto *field = declarations.findField(
          {"Landroid/os/Handler;", "mLooper", "Landroid/os/Looper;"}, false);
      EXPECT_EQ(state.field(*field, execution.value, {}, Admit, 65536).value,
                state.library(operation(MainLooper), 0, {}, Admit).value);
    } else {
      EXPECT_EQ(execution.stop, ExecutionStop::Thrown);
      EXPECT_EQ(execution.exception, "java/lang/NullPointerException");
    }
  }
}
TEST_F(AndroidFrameworkTest, MainLooperDeclarationsKeepPlatformAccess) {
  const auto *handler = program.findClass("Landroid/os/Handler;");
  ASSERT_NE(handler, nullptr);
  EXPECT_TRUE(has(handler->access, "public"));
  EXPECT_FALSE(has(handler->access, "final"));
  for (const auto &field : handler->fields) {
    EXPECT_TRUE(has(field.access, "final"));
    EXPECT_FALSE(has(field.access, "public"));
  }
  for (const auto *name :
       {"Landroid/os/Looper;", "Landroid/os/MessageQueue;"}) {
    const auto *type = program.findClass(name);
    ASSERT_NE(type, nullptr);
    EXPECT_TRUE(has(type->access, "public"));
    EXPECT_TRUE(has(type->access, "final"));
  }
  const auto *runnable = program.findClass("Ljava/lang/Runnable;");
  ASSERT_NE(runnable, nullptr);
  EXPECT_TRUE(has(runnable->access, "interface"));
  ASSERT_EQ(runnable->methods.size(), 1u);
  EXPECT_TRUE(has(runnable->methods[0].access, "abstract"));
  EXPECT_TRUE(has(runnable->methods[0].access, "public"));
  EXPECT_EQ(runnable->methods[0].reference.signature(), "()V");
  const auto *post = libraryMethod(
      {"Landroid/os/Handler;", "post", {"Ljava/lang/Runnable;"}, "Z"}, false);
  ASSERT_NE(post, nullptr);
  EXPECT_TRUE(post->Final);
}
TEST_F(AndroidFrameworkTest, LocaleHierarchyPreservesFinalAndMarkerInterfaces) {
  const auto *locale = program.findClass("Ljava/util/Locale;");
  ASSERT_NE(locale, nullptr);
  EXPECT_TRUE(has(locale->access, "final"));
  EXPECT_EQ(locale->superclass, "Ljava/lang/Object;");
  EXPECT_EQ(locale->interfaces,
            (std::vector<std::string>{"Ljava/lang/Cloneable;",
                                      "Ljava/io/Serializable;"}));
  const auto *cloneable = program.findClass("Ljava/lang/Cloneable;");
  ASSERT_NE(cloneable, nullptr);
  EXPECT_TRUE(has(cloneable->access, "interface"));
  EXPECT_TRUE(locale->methods.empty());
  EXPECT_TRUE(locale->fields.empty());
  Class derived;
  derived.name = "Ltest/DerivedLocale;";
  derived.superclass = locale->name;
  Program invalid({derived}, Properties{});
  EXPECT_TRUE(invalid.unsupportedClass(derived.name).has_value());
  Class replacement;
  replacement.name = locale->name;
  EXPECT_THROW(Program({replacement}, Properties{}), Error);
}
TEST_F(AndroidFrameworkTest,
       DefaultLocaleRetainsIdentityAndInternedComponents) {
  for (auto phase : {Phase::Unattached, Phase::Attached, Phase::Application}) {
    AndroidApplicationSnapshot input{phase};
    input.default_locale = AndroidApplicationSnapshot::Locale{"iw", "US"};
    Runtime state(program, heap, input);
    auto locale = state.library(operation(LocaleDefault), 0, {}, Admit);
    ASSERT_EQ(locale.stop, ExecutionStop::Returned);
    ASSERT_NE(locale.value, 0u);
    EXPECT_EQ(heap->get(locale.value)->ClassName, "java/util/Locale");
    EXPECT_EQ(state.library(operation(LocaleDefault), 0, {}, Admit).value,
              locale.value);
    for (auto [op, text] :
         {std::pair{LocaleLanguage, "iw"}, {LocaleCountry, "US"}}) {
      auto value = state.library(operation(op), locale.value, {}, Admit);
      ASSERT_EQ(value.stop, ExecutionStop::Returned) << value.diagnostic;
      EXPECT_EQ(value.value, heap->literal(text)->Identity);
      EXPECT_EQ(state.library(operation(op), locale.value, {}, Admit).value,
                value.value);
    }
    Runtime other(program, heap, input);
    EXPECT_NE(other.library(operation(LocaleDefault), 0, {}, Admit).value,
              locale.value);
  }
  AndroidApplicationSnapshot input;
  input.default_locale = AndroidApplicationSnapshot::Locale{"", ""};
  Runtime state(program, heap, input);
  auto locale = state.library(operation(LocaleDefault), 0, {}, Admit).value;
  for (auto op : {LocaleLanguage, LocaleCountry}) {
    auto value = state.library(operation(op), locale, {}, Admit);
    ASSERT_EQ(value.stop, ExecutionStop::Returned);
    EXPECT_EQ(value.value, heap->literal("")->Identity);
  }
}
TEST_F(AndroidFrameworkTest, DefaultLocaleRequiresKnownOwnedState) {
  for (auto op : {LocaleDefault, LocaleLanguage, LocaleCountry}) {
    AndroidApplicationSnapshot input;
    if (op != LocaleDefault)
      input.default_locale.emplace();
    Runtime state(program, heap, input);
    auto locale =
        op == LocaleDefault
            ? 0
            : state.library(operation(LocaleDefault), 0, {}, Admit).value;
    auto result = state.library(operation(op), locale, {}, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_NE(result.diagnostic.find("is not supplied"), std::string::npos);
  }
  AndroidApplicationSnapshot input;
  input.default_locale = AndroidApplicationSnapshot::Locale{"en", "US"};
  Runtime foreign(program, heap, input);
  foreign.library(operation(LocaleDefault), 0, {}, Admit);
  auto object = heap->allocate(Object::Instance, "java/util/Locale", 0);
  ASSERT_TRUE(object);
  auto refused =
      foreign.library(operation(LocaleLanguage), object->Identity, {}, Admit);
  EXPECT_EQ(refused.stop, ExecutionStop::Unsupported);
  EXPECT_NE(refused.diagnostic.find("unmodeled Android Locale instance"),
            std::string::npos);
  Runtime null_receiver(program, heap, input);
  auto thrown = null_receiver.library(operation(LocaleCountry), 0, {}, Admit);
  EXPECT_EQ(thrown.stop, ExecutionStop::Thrown);
  EXPECT_EQ(thrown.exception, "java/lang/NullPointerException");
}
TEST_F(AndroidFrameworkTest, DefaultLocaleValidatesAlreadyNormalizedState) {
  for (const auto &text : std::vector<std::string>{
           "EN", "he", "yi", "id", "\xff", std::string(1025, 'a')}) {
    AndroidApplicationSnapshot input;
    input.default_locale.emplace().language = text;
    EXPECT_THROW(Runtime(program, heap, input), Error);
  }
  AndroidApplicationSnapshot input;
  input.default_locale.emplace().country = "us";
  EXPECT_THROW(Runtime(program, heap, input), Error);
  for (const auto *text : {"", "US", "419", "X_TEST"}) {
    input.default_locale->country = text;
    EXPECT_NO_THROW(Runtime(program, heap, input));
  }
  EXPECT_EQ(
      libraryMethod(
          {"Ljava/util/Locale;", "setDefault", {"Ljava/util/Locale;"}, "V"},
          true),
      nullptr);
}
TEST_F(AndroidFrameworkTest, DefaultLocaleHonorsTerminalResourceBudgets) {
  AndroidApplicationSnapshot input;
  input.default_locale = AndroidApplicationSnapshot::Locale{"en", "US"};
  Runtime stopped(program, heap, input);
  auto limited = stopped.library(operation(LocaleDefault), 0, {}, [] {
    return ExecutionStop::InstructionLimit;
  });
  EXPECT_EQ(limited.stop, ExecutionStop::InstructionLimit);
  EXPECT_EQ(stopped.library(operation(LocaleDefault), 0, {}, Admit).stop,
            ExecutionStop::InstructionLimit);
  for (bool getter : {false, true}) {
    auto small = std::make_shared<Heap>(1024, 128);
    Runtime state(program, small, input);
    auto locale =
        getter ? state.library(operation(LocaleDefault), 0, {}, Admit).value
               : 0;
    while (small->allocate(Object::Instance, "java/lang/Object", 0)) {
    }
    auto result = state.library(
        operation(getter ? LocaleLanguage : LocaleDefault), locale, {}, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::HeapLimit);
    EXPECT_EQ(state.library(operation(LocaleDefault), 0, {}, Admit).stop,
              ExecutionStop::HeapLimit);
  }
}
TEST_F(AndroidFrameworkTest, DefaultLocaleDexAndLibraryShareTheSameValue) {
  Class reader;
  reader.name = "Ltest/LocaleReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {reader.name, "language", {}, "Ljava/lang/String;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction current;
  current.opcode = "invoke-static";
  current.reference =
      MethodRef{"Ljava/util/Locale;", "getDefault", {}, "Ljava/util/Locale;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction language;
  language.pc = 4;
  language.opcode = "invoke-virtual";
  language.registers = {0};
  language.reference =
      MethodRef{"Ljava/util/Locale;", "getLanguage", {}, "Ljava/lang/String;"};
  Instruction ret;
  ret.pc = 8;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {current, move, language, move, ret};
  method.instructions[3].pc = 7;
  reader.methods = {method};
  Program code({reader}, Properties{});
  AndroidApplicationSnapshot input;
  input.default_locale = AndroidApplicationSnapshot::Locale{"en", "US"};
  Runtime state(code, heap, input);
  auto locale = state.library(operation(LocaleDefault), 0, {}, Admit).value;
  auto direct = state.library(operation(LocaleLanguage), locale, {}, Admit);
  auto dex =
      state.execute(*code.findStatic(method.reference), {}, Admit, 65536);
  ASSERT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
  EXPECT_EQ(dex.value, direct.value);
  EXPECT_EQ(dex.value, heap->literal("en")->Identity);
}
using LoaderPaths = AndroidApplicationSnapshot::ClassLoaderPaths;
uint64_t applicationLoader(Runtime &state) {
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
  EXPECT_EQ(app.stop, ExecutionStop::Returned);
  auto loader =
      state.library(operation(ContextClassLoader), app.value, {}, Admit);
  EXPECT_EQ(loader.stop, ExecutionStop::Returned);
  return loader.value;
}
TEST_F(AndroidFrameworkTest, ClassLoaderPathsKeepOrderAndProduceFreshStrings) {
  AndroidApplicationSnapshot input{Phase::Application};
  input.application_loader_paths = LoaderPaths{
      {"/one.apk", "relative\"two.apk"}, {"/app/lib", ""}, {"/system/lib64"}};
  input.system_loader_paths = LoaderPaths{};
  Runtime state(program, heap, input);
  const auto app = applicationLoader(state);
  const auto system =
      state.library(operation(SystemClassLoader), 0, {}, Admit).value;
  for (auto [receiver, expected] :
       {std::pair{app,
                  "dalvik.system.PathClassLoader[DexPathList[[zip file "
                  "\"/one.apk\", zip file \"relative\"two.apk\"],"
                  "nativeLibraryDirectories=[/app/lib, , /system/lib64]]]"},
        std::pair{system, "dalvik.system.PathClassLoader[DexPathList[[],"
                          "nativeLibraryDirectories=[]]]"}}) {
    auto first =
        state.library(operation(ClassLoaderString), receiver, {}, Admit);
    auto second =
        state.library(operation(ClassLoaderString), receiver, {}, Admit);
    ASSERT_EQ(first.stop, ExecutionStop::Returned) << first.diagnostic;
    ASSERT_EQ(second.stop, ExecutionStop::Returned) << second.diagnostic;
    ASSERT_NE(first.value, second.value);
    EXPECT_EQ(heap->get(first.value)->Bytes, javaStringBytes(expected));
    EXPECT_EQ(heap->get(second.value)->Bytes, heap->get(first.value)->Bytes);
  }
}
TEST_F(AndroidFrameworkTest, ClassLoaderPathsRequireTheExactOwnedLoader) {
  for (bool app_missing : {false, true}) {
    AndroidApplicationSnapshot input{Phase::Application};
    (app_missing ? input.system_loader_paths : input.application_loader_paths)
        .emplace();
    Runtime state(program, heap, input);
    auto app = applicationLoader(state);
    auto system =
        state.library(operation(SystemClassLoader), 0, {}, Admit).value;
    auto result = state.library(operation(ClassLoaderString),
                                app_missing ? app : system, {}, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_NE(result.diagnostic.find(app_missing ? "application loader paths"
                                                 : "system loader paths"),
              std::string::npos);
  }
  AndroidApplicationSnapshot input{Phase::Application};
  input.application_loader_paths.emplace();
  Runtime foreign(program, heap, input);
  applicationLoader(foreign);
  auto object =
      heap->allocate(Object::Instance, "dalvik/system/PathClassLoader", 0);
  ASSERT_TRUE(object);
  EXPECT_EQ(
      foreign.library(operation(ClassLoaderString), object->Identity, {}, Admit)
          .stop,
      ExecutionStop::Unsupported);
  Runtime null_receiver(program, heap, input);
  auto result =
      null_receiver.library(operation(ClassLoaderString), 0, {}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/NullPointerException");
}
TEST_F(AndroidFrameworkTest, ClassLoaderPathsValidateLifecycleAndBoundedState) {
  AndroidApplicationSnapshot input;
  input.system_loader_paths.emplace();
  EXPECT_NO_THROW(Runtime(program, heap, input));
  input.application_loader_paths.emplace();
  EXPECT_THROW(Runtime(program, heap, input), Error);
  input.phase = Phase::Attached;
  EXPECT_THROW(Runtime(program, heap, input), Error);
  input.application_loader_established = true;
  EXPECT_NO_THROW(Runtime(program, heap, input));
  for (auto paths :
       {LoaderPaths{{std::string(4097, 'a')}, {}, {}},
        LoaderPaths{std::vector<std::string>(65, ""), {}, {}},
        LoaderPaths{
            {}, std::vector<std::string>(5, std::string(4096, 'a')), {}},
        LoaderPaths{{}, {}, {"\xff"}}}) {
    input.system_loader_paths = std::move(paths);
    EXPECT_THROW(Runtime(program, heap, input), Error);
  }
}
TEST_F(AndroidFrameworkTest, ClassLoaderPathsHonorTerminalResourceLimits) {
  for (bool memory : {false, true}) {
    auto store = std::make_shared<Heap>(1024, 128);
    AndroidApplicationSnapshot input;
    input.system_loader_paths = LoaderPaths{{"/one.apk"}, {}, {}};
    Runtime state(program, store, input);
    auto loader =
        state.library(operation(SystemClassLoader), 0, {}, Admit).value;
    if (memory)
      while (store->allocate(Object::Instance, "java/lang/Object", 0)) {
      }
    const AdmitInstruction limited = [] {
      return ExecutionStop::InstructionLimit;
    };
    auto result = state.library(operation(ClassLoaderString), loader, {},
                                memory ? Admit : limited);
    const auto expected =
        memory ? ExecutionStop::HeapLimit : ExecutionStop::InstructionLimit;
    EXPECT_EQ(result.stop, expected);
    EXPECT_EQ(
        state.library(operation(ClassLoaderString), loader, {}, Admit).stop,
        expected);
  }
}
TEST_F(AndroidFrameworkTest, ClassLoaderPathsDexUsesTheSharedOwner) {
  Class reader;
  reader.name = "Ltest/LoaderStringReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {reader.name,
                      "render",
                      {"Ldalvik/system/PathClassLoader;"},
                      "Ljava/lang/String;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction invoke;
  invoke.opcode = "invoke-virtual";
  invoke.registers = {0};
  invoke.reference = MethodRef{
      "Ldalvik/system/PathClassLoader;", "toString", {}, "Ljava/lang/String;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {invoke, move, ret};
  reader.methods = {method};
  Program code({reader}, Properties{});
  AndroidApplicationSnapshot input;
  input.system_loader_paths = LoaderPaths{{"/one.apk"}, {}, {"/system/lib"}};
  Runtime state(code, heap, input);
  auto loader = state.library(operation(SystemClassLoader), 0, {}, Admit).value;
  auto direct = state.library(operation(ClassLoaderString), loader, {}, Admit);
  const uint64_t args[]{loader};
  auto dex =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  ASSERT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
  ASSERT_EQ(direct.stop, ExecutionStop::Returned) << direct.diagnostic;
  EXPECT_NE(dex.value, direct.value);
  EXPECT_EQ(heap->get(dex.value)->Bytes, heap->get(direct.value)->Bytes);
}
TEST_F(AndroidFrameworkTest, ExplicitLookupFailuresAreScopedAndRetainMessages) {
  for (auto selected : {Loader::Boot, Loader::System, Loader::Application}) {
    for (auto message :
         {std::optional<std::string>{}, std::optional<std::string>{""},
          std::optional<std::string>{"declared \xc3\xa9"}}) {
      AndroidApplicationSnapshot snapshot{Phase::Application};
      snapshot.class_lookup_failures = {{selected, "test.Absent", message}};
      Runtime state(program, heap, snapshot);
      auto app =
          state.library(operation(CurrentApplication), 0, {}, Admit).value;
      auto sys =
          state.library(operation(SystemClassLoader), 0, {}, Admit).value;
      auto boot = state.library(operation(LoaderParent), sys, {}, Admit).value;
      auto loader =
          selected == Loader::Boot ? boot
          : selected == Loader::System
              ? sys
              : state.library(operation(ContextClassLoader), app, {}, Admit)
                    .value;
      const uint64_t args[]{heap->literal("test.Absent")->Identity};
      uint64_t previous = 0;
      for (unsigned i = 0; i != 2; ++i) {
        auto result = state.library(operation(LoadClass), loader, args, Admit);
        ASSERT_EQ(result.stop, ExecutionStop::Thrown) << result.diagnostic;
        EXPECT_EQ(result.exception, "java/lang/ClassNotFoundException");
        ASSERT_NE(result.throwable, 0u);
        EXPECT_NE(result.throwable, previous);
        previous = result.throwable;
        auto value = state.library(operation(ThrowableMessage),
                                   result.throwable, {}, Admit);
        ASSERT_EQ(value.stop, ExecutionStop::Returned);
        if (message) {
          ASSERT_NE(value.value, 0u);
          EXPECT_EQ(heap->get(value.value)->Bytes, javaStringBytes(*message));
        } else {
          EXPECT_EQ(value.value, 0u);
        }
      }
      const uint64_t known[]{heap->literal("java.lang.String")->Identity};
      EXPECT_EQ(state.library(operation(LoadClass), loader, known, Admit).stop,
                ExecutionStop::Returned);
      auto other = loader == boot ? sys : boot;
      EXPECT_EQ(state.library(operation(LoadClass), other, args, Admit).stop,
                ExecutionStop::Unsupported);
    }
  }
}
TEST_F(AndroidFrameworkTest,
       ExactQueryLookupFailuresPreserveWhitespaceAndLoaderScope) {
  for (auto selected : {Loader::Boot, Loader::System, Loader::Application}) {
    for (const char *query : {"test.Absent ", " test.Absent", "test. Absent",
                              "java.lang.String "}) {
      for (auto message :
           {std::optional<std::string>{}, std::optional<std::string>{""},
            std::optional<std::string>{"declared missing"}}) {
        AndroidApplicationSnapshot snapshot{Phase::Application};
        snapshot.class_lookup_failures = {{selected, {}, message, query}};
        Runtime state(program, heap, snapshot);
        auto sys =
            state.library(operation(SystemClassLoader), 0, {}, Admit).value;
        auto boot =
            state.library(operation(LoaderParent), sys, {}, Admit).value;
        auto app =
            state.library(operation(CurrentApplication), 0, {}, Admit).value;
        auto loader =
            selected == Loader::Boot ? boot
            : selected == Loader::System
                ? sys
                : state.library(operation(ContextClassLoader), app, {}, Admit)
                      .value;
        const uint64_t args[]{heap->literal(query)->Identity};
        uint64_t previous = 0;
        for (unsigned i = 0; i != 2; ++i) {
          auto result =
              state.library(operation(LoadClass), loader, args, Admit);
          ASSERT_EQ(result.stop, ExecutionStop::Thrown) << result.diagnostic;
          EXPECT_EQ(result.exception, "java/lang/ClassNotFoundException");
          ASSERT_NE(result.throwable, 0u);
          EXPECT_NE(result.throwable, previous);
          previous = result.throwable;
          auto value = state.library(operation(ThrowableMessage),
                                     result.throwable, {}, Admit);
          ASSERT_EQ(value.stop, ExecutionStop::Returned);
          if (message) {
            ASSERT_NE(value.value, 0u);
            EXPECT_EQ(heap->get(value.value)->Bytes, javaStringBytes(*message));
          } else {
            EXPECT_EQ(value.value, 0u);
          }
        }
        const uint64_t known[]{heap->literal("java.lang.String")->Identity};
        EXPECT_EQ(
            state.library(operation(LoadClass), loader, known, Admit).stop,
            ExecutionStop::Returned);
        auto other = loader == boot ? sys : boot;
        EXPECT_EQ(state.library(operation(LoadClass), other, args, Admit).stop,
                  ExecutionStop::Unsupported);
      }
    }
  }
  for (const char *unlisted : {"test.Absent", "test.Absent  ", "test.Other"}) {
    AndroidApplicationSnapshot snapshot{Phase::Attached};
    snapshot.class_lookup_failures = {{Loader::System, {}, {}, "test.Absent "}};
    Runtime state(program, heap, snapshot);
    auto loader =
        state.library(operation(SystemClassLoader), 0, {}, Admit).value;
    const uint64_t args[]{heap->literal(unlisted)->Identity};
    EXPECT_EQ(state.library(operation(LoadClass), loader, args, Admit).stop,
              ExecutionStop::Unsupported);
  }
}
TEST_F(AndroidFrameworkTest, ExactQueryLookupFailuresRejectAmbiguousInputs) {
  AndroidApplicationSnapshot snapshot{Phase::Application};
  for (const auto &query :
       {std::string{}, std::string("[I"), std::string("test/Absent"),
        std::string("a\0b", 3), std::string("a\tb"), std::string("a\nb"),
        std::string("a\x7f"), std::string("a\xc3\xa9"),
        std::string(::neverd::limits::dalvik::kMaxAndroidClassNameBytes + 1,
                    'x')}) {
    snapshot.class_lookup_failures = {{Loader::Boot, {}, {}, query}};
    EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  }
  snapshot.class_lookup_failures = {
      {Loader::Boot, "test.Absent", {}, "test.Absent "}};
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  snapshot.class_lookup_failures = {{Loader::Boot, "test.Absent", {}},
                                    {Loader::Boot, {}, {}, "test.Absent"}};
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  snapshot.class_lookup_failures[0] = snapshot.class_lookup_failures[1];
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  for (auto loader : {Loader::Boot, Loader::System, Loader::Application}) {
    snapshot.class_lookup_failures = {{loader, {}, {}, "java.lang.String"}};
    EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  }
  snapshot.class_lookup_failures = {
      {Loader::Boot,
       {},
       {},
       std::string(::neverd::limits::dalvik::kMaxAndroidClassNameBytes, 'x')}};
  EXPECT_NO_THROW(Runtime(program, heap, snapshot));
  snapshot.class_lookup_failures = {
      {Loader::Application, {}, {}, "test.Absent "}};
  snapshot.phase = Phase::Attached;
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  snapshot.application_loader_established = true;
  EXPECT_NO_THROW(Runtime(program, heap, snapshot));
  Class type;
  type.name = "Ltest/Visible;";
  type.superclass = "Ljava/lang/Object;";
  Program code({type}, Properties{});
  snapshot.class_lookup_failures = {
      {Loader::Application, {}, {}, "test.Visible"}};
  EXPECT_THROW(Runtime(code, heap, snapshot), Error);
  snapshot.class_lookup_failures[0].loader = Loader::Boot;
  EXPECT_NO_THROW(Runtime(code, heap, snapshot));
}
TEST_F(AndroidFrameworkTest,
       LookupFailuresRejectInvalidAndVisibleDeclarations) {
  AndroidApplicationSnapshot snapshot{Phase::Application};
  snapshot.class_lookup_failures = {LookupFailure{}};
  snapshot.class_lookup_failures[0].binary_name = "test.Absent";
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  for (auto name : {"", ".a", "a.", "a..b", "a/b", "[I", "La;", "a.1B",
                    "a.\xc3\xa9", "a ", " a", "a. b"}) {
    snapshot.class_lookup_failures = {{Loader::Boot, name, {}}};
    EXPECT_THROW(Runtime(program, heap, snapshot), Error) << name;
  }
  for (auto loader : {Loader::Boot, Loader::System, Loader::Application}) {
    snapshot.class_lookup_failures = {{loader, "java.lang.String", {}}};
    EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  }
  snapshot.class_lookup_failures = {{Loader(9), "test.Absent", {}}};
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  snapshot.class_lookup_failures = {{Loader::Boot, "test.Absent", {}}};
  snapshot.class_lookup_failures.push_back(snapshot.class_lookup_failures[0]);
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  snapshot.class_lookup_failures = {
      {Loader::Boot, "test.Absent",
       std::string(::neverd::limits::dalvik::kMaxAndroidLookupMessageBytes + 1,
                   'x')}};
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  snapshot.class_lookup_failures = {
      {Loader::Boot,
       std::string(::neverd::limits::dalvik::kMaxAndroidClassNameBytes + 1,
                   'x'),
       {}}};
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  snapshot.class_lookup_failures.assign(
      ::neverd::limits::dalvik::kMaxAndroidApplicationClassLookupFailures + 1,
      {Loader::Boot, "test.Absent", {}});
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  snapshot.class_lookup_failures = {{Loader::Application, "test.Absent", {}}};
  snapshot.phase = Phase::Attached;
  EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  Class type;
  type.name = "Ltest/Visible;";
  type.superclass = "Ljava/lang/Object;";
  Program code({type}, Properties{});
  snapshot.phase = Phase::Application;
  snapshot.class_lookup_failures = {{Loader::Application, "test.Visible", {}}};
  EXPECT_THROW(Runtime(code, heap, snapshot), Error);
  // A supplied app declaration need not be visible to the boot loader.
  snapshot.class_lookup_failures[0].loader = Loader::Boot;
  EXPECT_NO_THROW(Runtime(code, heap, snapshot));
}
TEST_F(AndroidFrameworkTest, LookupFailuresRespectHeapAndInstructionLimits) {
  for (bool exact : {false, true}) {
    for (bool exhaustion : {false, true}) {
      auto storage = std::make_shared<Heap>(1024, exhaustion ? 4 : 128);
      AndroidApplicationSnapshot snapshot{Phase::Attached};
      snapshot.class_lookup_failures = {{Loader::System, "test.Absent", {}}};
      const char *query = exact ? "test.Absent " : "test.Absent";
      if (exact)
        snapshot.class_lookup_failures = {{Loader::System, {}, {}, query}};
      Runtime state(program, storage, snapshot);
      auto loader =
          state.library(operation(SystemClassLoader), 0, {}, Admit).value;
      const uint64_t args[]{storage->literal(query)->Identity};
      if (exhaustion) {
        while (storage->allocate(Object::Instance, "java/lang/Object", 0)) {
        }
      }
      unsigned ticks = 0;
      auto result = state.library(operation(LoadClass), loader, args, [&] {
        return !exhaustion && ++ticks > 3 ? ExecutionStop::InstructionLimit
                                          : ExecutionStop::Returned;
      });
      EXPECT_EQ(result.stop, exhaustion ? ExecutionStop::HeapLimit
                                        : ExecutionStop::InstructionLimit);
      EXPECT_EQ(state.library(operation(LoadClass), loader, args, Admit).stop,
                result.stop);
    }
  }
}
TEST_F(AndroidFrameworkTest, DexCatchObservesTheSameLookupThrowableAndMessage) {
  Class reader;
  reader.name = "Ltest/LoaderReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {reader.name,
                      "lookup",
                      {"Ljava/lang/ClassLoader;", "Ljava/lang/String;"},
                      "Ljava/lang/Throwable;"};
  method.access = {"public", "static"};
  method.registers = 3;
  Instruction invoke;
  invoke.opcode = "invoke-virtual";
  invoke.registers = {1, 2};
  invoke.reference = MethodRef{"Ljava/lang/ClassLoader;",
                               "loadClass",
                               {"Ljava/lang/String;"},
                               "Ljava/lang/Class;"};
  Instruction zero;
  zero.pc = 3;
  zero.opcode = "const/4";
  zero.registers = {0};
  zero.literal = int64_t(0);
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  Instruction caught;
  caught.pc = 5;
  caught.opcode = "move-exception";
  caught.registers = {0};
  method.instructions = {invoke, zero, ret, caught, ret};
  method.instructions.back().pc = 6;
  method.tries = {{0, 3, {{"Ljava/lang/ClassNotFoundException;", 5}}}};
  reader.methods = {method};
  Program code({reader}, Properties{});
  for (bool exact : {false, true}) {
    AndroidApplicationSnapshot snapshot{Phase::Attached};
    snapshot.class_lookup_failures = {
        {Loader::System, "test.Absent", "explicit result"}};
    const char *query = exact ? "test.Absent " : "test.Absent";
    if (exact)
      snapshot.class_lookup_failures = {
          {Loader::System, {}, "explicit result", query}};
    Runtime state(code, heap, snapshot);
    auto loader =
        state.library(operation(SystemClassLoader), 0, {}, Admit).value;
    const uint64_t args[]{loader, heap->literal(query)->Identity};
    auto result =
        state.execute(*code.findStatic(method.reference), args, Admit, 65536);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    ASSERT_NE(result.value, 0u);
    EXPECT_EQ(heap->get(result.value)->ClassName,
              "java/lang/ClassNotFoundException");
    auto message =
        state.library(operation(ThrowableMessage), result.value, {}, Admit);
    ASSERT_EQ(message.stop, ExecutionStop::Returned);
    ASSERT_NE(message.value, 0u);
    EXPECT_EQ(heap->get(message.value)->Bytes,
              javaStringBytes("explicit result"));
    const uint64_t missing[]{loader, heap->literal("test.Other")->Identity};
    EXPECT_EQ(
        state.execute(*code.findStatic(method.reference), missing, Admit, 65536)
            .stop,
        ExecutionStop::Unsupported);
  }
}
const FieldRef InstrumentationField{"Landroid/app/ActivityThread;",
                                    "mInstrumentation",
                                    "Landroid/app/Instrumentation;"};
TEST_F(AndroidFrameworkTest,
       InstrumentationStateIsIndependentOfApplicationPhase) {
  const auto *field = program.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_FALSE(has(field->access, "public"));
  EXPECT_EQ(program.findField(InstrumentationField, true), nullptr);
  for (auto phase : {Phase::Attached, Phase::Application}) {
    Runtime unknown(program, heap, AndroidApplicationSnapshot{phase});
    auto thread =
        unknown.library(operation(CurrentActivityThread), 0, {}, Admit);
    auto missing = unknown.field(*field, thread.value, {}, Admit, 65536);
    EXPECT_EQ(missing.stop, ExecutionStop::Unsupported);
    EXPECT_NE(missing.diagnostic.find("instrumentation state"),
              std::string::npos);
    for (auto input :
         {Instrumentation::Uninitialized, Instrumentation::Platform}) {
      AndroidApplicationSnapshot snapshot{phase};
      snapshot.instrumentation = input;
      Runtime state(program, heap, snapshot);
      auto thread =
          state.library(operation(CurrentActivityThread), 0, {}, Admit);
      auto value = state.field(*field, thread.value, {}, Admit, 65536);
      ASSERT_EQ(value.stop, ExecutionStop::Returned) << value.diagnostic;
      EXPECT_EQ(value.value != 0, input == Instrumentation::Platform);
      auto method = state.library(operation(ThreadInstrumentation),
                                  thread.value, {}, Admit);
      EXPECT_EQ(method.stop, ExecutionStop::Returned) << method.diagnostic;
      EXPECT_EQ(method.value, value.value);
      if (value.value)
        EXPECT_EQ(heap->get(value.value)->ClassName,
                  "android/app/Instrumentation");
      auto cleared = state.field(*field, thread.value, 0, Admit, 65536);
      EXPECT_EQ(cleared.stop, ExecutionStop::Returned);
      EXPECT_EQ(state
                    .library(operation(ThreadInstrumentation), thread.value, {},
                             Admit)
                    .value,
                0u);
      EXPECT_EQ(
          state.field(*field, thread.value, value.value, Admit, 65536).stop,
          ExecutionStop::Returned);
      EXPECT_EQ(state
                    .library(operation(ThreadInstrumentation), thread.value, {},
                             Admit)
                    .value,
                value.value);
    }
  }
}
TEST_F(AndroidFrameworkTest,
       ExplicitFieldWritesDoNotNeedAnInventedInitialValue) {
  auto thread = call(CurrentActivityThread);
  const auto *field = program.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(runtime.field(*field, thread, 0, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(call(ThreadInstrumentation, thread), 0u);
}
TEST_F(AndroidFrameworkTest,
       InstrumentationRejectsForeignReceiversAndWrongValues) {
  const auto *field = program.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  for (const char *type : {"android/app/ActivityThread", "java/lang/Object"}) {
    Runtime state(program, heap, AndroidApplicationSnapshot{Phase::Attached});
    state.library(operation(CurrentActivityThread), 0, {}, Admit);
    auto foreign = heap->allocate(Object::Instance, type, 0);
    auto result = state.field(*field, foreign->Identity, 0, Admit, 65536);
    EXPECT_EQ(result.stop,
              std::string_view(type) == "android/app/ActivityThread"
                  ? ExecutionStop::Unsupported
                  : ExecutionStop::Invalid);
  }
  auto thread = call(CurrentActivityThread);
  auto wrong = heap->allocate(Object::Instance, "java/lang/Object", 0);
  EXPECT_EQ(runtime.field(*field, thread, wrong->Identity, Admit, 65536).stop,
            ExecutionStop::Invalid);
}
TEST_F(AndroidFrameworkTest,
       InstrumentationHonorsBudgetAndRejectsMalformedSnapshots) {
  for (auto input :
       {Instrumentation::Uninitialized, Instrumentation::Platform}) {
    AndroidApplicationSnapshot snapshot{Phase::Unattached};
    snapshot.instrumentation = input;
    EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  }
  AndroidApplicationSnapshot invalid{Phase::Attached};
  invalid.instrumentation = Instrumentation(9);
  EXPECT_THROW(Runtime(program, heap, invalid), Error);
  AndroidApplicationSnapshot snapshot{Phase::Attached};
  snapshot.instrumentation = Instrumentation::Platform;
  Runtime state(program, heap, snapshot);
  auto thread =
      state.library(operation(CurrentActivityThread), 0, {}, Admit).value;
  const auto *field = program.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  auto result = state.field(
      *field, thread, {}, [] { return ExecutionStop::Timeout; }, 65536);
  EXPECT_EQ(result.stop, ExecutionStop::Timeout);
  EXPECT_EQ(state.field(*field, thread, {}, Admit, 65536).stop, result.stop);
}
TEST_F(AndroidFrameworkTest,
       DexGetterAndNativeFieldShareMutableSubclassIdentity) {
  Class reader;
  reader.name = "Ltest/InstrumentationReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {reader.name,
                      "read",
                      {"Landroid/app/ActivityThread;"},
                      "Landroid/app/Instrumentation;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction get;
  get.opcode = "invoke-virtual";
  get.registers = {0};
  get.reference = MethodRef{"Landroid/app/ActivityThread;",
                            "getInstrumentation",
                            {},
                            "Landroid/app/Instrumentation;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {get, move, ret};
  reader.methods.push_back(method);
  Class custom;
  custom.name = "Ltest/CustomInstrumentation;";
  custom.superclass = "Landroid/app/Instrumentation;";
  custom.access = {"public"};
  Program code({reader, custom}, Properties{});
  Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Attached});
  auto thread =
      state.library(operation(CurrentActivityThread), 0, {}, Admit).value;
  auto replacement = state.allocate(custom.name, true, Admit, 65536);
  ASSERT_EQ(replacement.stop, ExecutionStop::Returned)
      << replacement.diagnostic;
  const auto *field = code.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(state.field(*field, thread, replacement.value, Admit, 65536).stop,
            ExecutionStop::Returned);
  const uint64_t args[]{thread};
  auto dex =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  EXPECT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
  EXPECT_EQ(dex.value, replacement.value);
  EXPECT_EQ(state.field(*field, thread, {}, Admit, 65536).value, dex.value);
}

TEST_F(AndroidFrameworkTest,
       FrameworkFieldOwnershipDoesNotTrustSourceMetadata) {
  Class ordinary;
  ordinary.name = "Ltest/Ordinary;";
  ordinary.superclass = "Ljava/lang/Object;";
  ordinary.access = {"public"};
  ordinary.source_id = "android-api28-framework";
  Field field;
  field.reference = {ordinary.name, "value", "I"};
  field.access = {"public"};
  ordinary.fields.push_back(field);
  Program code({ordinary}, Properties{});
  const auto *owned = code.findField(field.reference, false);
  ASSERT_NE(owned, nullptr);
  EXPECT_FALSE(code.androidFrameworkField(*owned));
  const auto *platform = code.findField(InstrumentationField, false);
  ASSERT_NE(platform, nullptr);
  EXPECT_TRUE(code.androidFrameworkField(*platform));
  Field copy = *platform;
  EXPECT_FALSE(code.androidFrameworkField(copy));
  Runtime state(code, heap);
  auto object = state.allocate(ordinary.name, true, Admit, 65536);
  ASSERT_EQ(object.stop, ExecutionStop::Returned);
  EXPECT_EQ(state.field(*owned, object.value, 23, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(state.field(*owned, object.value, {}, Admit, 65536).value, 23u);
}
TEST_F(AndroidFrameworkTest, InstrumentationAllocationHonorsSharedHeapLimit) {
  auto bounded = std::make_shared<Heap>(65536, 1);
  AndroidApplicationSnapshot snapshot{Phase::Attached};
  snapshot.instrumentation = Instrumentation::Platform;
  Runtime state(program, bounded, snapshot);
  auto thread = state.library(operation(CurrentActivityThread), 0, {}, Admit);
  ASSERT_EQ(thread.stop, ExecutionStop::Returned);
  const auto *field = program.findField(InstrumentationField, false);
  ASSERT_NE(field, nullptr);
  auto result = state.field(*field, thread.value, {}, Admit, 65536);
  EXPECT_EQ(result.stop, ExecutionStop::HeapLimit);
  EXPECT_EQ(
      state.library(operation(ThreadInstrumentation), thread.value, {}, Admit)
          .stop,
      ExecutionStop::HeapLimit);
}

TEST_F(AndroidFrameworkTest, FilesDirectoryRetainsFileAndStringIdentities) {
  for (const char *input :
       {"/data/user/0/example/files", "//data///files/", "/"}) {
    Runtime state(program, heap,
                  AndroidApplicationSnapshot{Phase::Application, {}, input});
    auto invoke = [&](LibraryOperation op, uint64_t receiver = 0) {
      auto r = state.library(operation(op), receiver, {}, Admit);
      EXPECT_EQ(r.stop, ExecutionStop::Returned) << r.diagnostic;
      return r.value;
    };
    auto app = invoke(CurrentApplication);
    auto base = invoke(BaseContext, app);
    auto file = invoke(FilesDirectory, app);
    EXPECT_EQ(invoke(FilesDirectory, base), file);
    EXPECT_EQ(heap->get(file)->ClassName, "java/io/File");
    EXPECT_TRUE(program.assignable("Ljava/io/File;", "Ljava/io/Serializable;"));
    EXPECT_TRUE(program.assignable("Ljava/io/File;", "Ljava/lang/Comparable;"));
    auto path = invoke(FilePath, file);
    EXPECT_EQ(invoke(FilePath, file), path);
    EXPECT_EQ(invoke(FileIsAbsolute, file), 1u);
    std::string expected = input;
    if (expected.starts_with("//"))
      expected = "/data/files";
    EXPECT_EQ(heap->get(path)->Bytes, javaStringBytes(expected));
    const auto name = invoke(FileName, file);
    EXPECT_EQ(heap->get(name)->Bytes,
              javaStringBytes(expected == "/" ? "" : "files"));
    const auto parent = invoke(FileParent, file);
    if (expected == "/")
      EXPECT_EQ(parent, 0u);
    else
      EXPECT_EQ(heap->get(parent)->Bytes,
                javaStringBytes(expected.substr(0, expected.rfind('/'))));
  }
}
TEST_F(AndroidFrameworkTest, ContentResolverBelongsToTheAttachedContext) {
  auto app = call(CurrentApplication);
  auto resolver = call(ContextContentResolver, app);
  auto base = call(BaseContext, app);
  EXPECT_NE(resolver, 0u);
  EXPECT_NE(resolver, app);
  EXPECT_NE(resolver, base);
  EXPECT_EQ(call(ContextContentResolver, app), resolver);
  EXPECT_EQ(call(ContextContentResolver, base), resolver);
  EXPECT_EQ(heap->get(resolver)->ClassName,
            "android/app/ContextImpl$ApplicationContentResolver");
  EXPECT_EQ(
      program.parent("Landroid/app/ContextImpl$ApplicationContentResolver;"),
      "Landroid/content/ContentResolver;");
  EXPECT_TRUE(
      program.assignable("Landroid/app/ContextImpl$ApplicationContentResolver;",
                         "Landroid/content/ContentResolver;"));
  // A separate workload owns a separate resolver, even with shared storage.
  Runtime other(program, heap, AndroidApplicationSnapshot{Phase::Application});
  auto other_app =
      other.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto other_resolver =
      other.library(operation(ContextContentResolver), other_app, {}, Admit);
  EXPECT_EQ(other_resolver.stop, ExecutionStop::Returned);
  EXPECT_NE(other_resolver.value, resolver);
}
TEST_F(AndroidFrameworkTest, PackageManagerUsesExplicitAvailabilityAndContext) {
  for (bool available : {false, true}) {
    AndroidApplicationSnapshot snapshot{Phase::Application};
    snapshot.package_manager_available = available;
    Runtime state(program, heap, snapshot);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto base = state.library(operation(BaseContext), app, {}, Admit).value;
    auto first =
        state.library(operation(ContextPackageManager), app, {}, Admit);
    ASSERT_EQ(first.stop, ExecutionStop::Returned) << first.diagnostic;
    EXPECT_EQ(first.value == 0, !available);
    for (uint64_t receiver : {app, base}) {
      auto again =
          state.library(operation(ContextPackageManager), receiver, {}, Admit);
      EXPECT_EQ(again.stop, ExecutionStop::Returned);
      EXPECT_EQ(again.value, first.value);
    }
    if (!available)
      continue;
    EXPECT_NE(first.value, app);
    EXPECT_NE(first.value, base);
    EXPECT_EQ(heap->get(first.value)->ClassName,
              "android/app/ApplicationPackageManager");
    EXPECT_EQ(program.parent("Landroid/app/ApplicationPackageManager;"),
              "Landroid/content/pm/PackageManager;");
    EXPECT_TRUE(program.assignable("Landroid/app/ApplicationPackageManager;",
                                   "Landroid/content/pm/PackageManager;"));
    Runtime other(program, heap, snapshot);
    auto other_app =
        other.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto other_manager =
        other.library(operation(ContextPackageManager), other_app, {}, Admit);
    ASSERT_EQ(other_manager.stop, ExecutionStop::Returned);
    EXPECT_NE(other_manager.value, first.value);
  }
}
TEST_F(AndroidFrameworkTest, PackageManagerRequiresKnownAvailability) {
  auto app = call(CurrentApplication);
  auto unknown =
      runtime.library(operation(ContextPackageManager), app, {}, Admit);
  EXPECT_EQ(unknown.stop, ExecutionStop::Unsupported);
  EXPECT_NE(unknown.diagnostic.find("availability is not supplied"),
            std::string::npos);
}
TEST_F(AndroidFrameworkTest, PackageManagerRequiresItsOwnContextAndLifecycle) {
  for (auto phase :
       {std::optional<Phase>{}, std::optional{Phase::Unattached},
        std::optional{Phase::Attached}, std::optional{Phase::Application}}) {
    std::optional<AndroidApplicationSnapshot> snapshot;
    if (phase) {
      snapshot.emplace();
      snapshot->phase = *phase;
      snapshot->package_manager_available = true;
    }
    Runtime state(program, heap, snapshot);
    auto foreign =
        heap->allocate(Object::Instance, "android/app/Application", 0);
    auto result = state.library(operation(ContextPackageManager),
                                foreign->Identity, {}, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  }
  auto null = runtime.library(operation(ContextPackageManager), 0, {}, Admit);
  EXPECT_EQ(null.stop, ExecutionStop::Thrown);
  EXPECT_EQ(null.exception, "java/lang/NullPointerException");
  auto wrong = heap->allocate(Object::Instance, "java/lang/Object", 0);
  EXPECT_EQ(
      runtime
          .library(operation(ContextPackageManager), wrong->Identity, {}, Admit)
          .stop,
      ExecutionStop::Invalid);
}
TEST_F(AndroidFrameworkTest, PackageManagerHonorsHeapAndWorkLimits) {
  for (bool available : {false, true}) {
    AndroidApplicationSnapshot snapshot{Phase::Application};
    snapshot.package_manager_available = available;
    auto bounded = std::make_shared<Heap>(65536, 1);
    Runtime state(program, bounded, snapshot);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
    ASSERT_EQ(app.stop, ExecutionStop::Returned);
    auto result =
        state.library(operation(ContextPackageManager), app.value, {}, Admit);
    EXPECT_EQ(result.stop,
              available ? ExecutionStop::HeapLimit : ExecutionStop::Returned);
    if (!available)
      EXPECT_EQ(result.value, 0u);
    else
      EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
                ExecutionStop::HeapLimit);
  }
  for (unsigned budget : {0u, 1u}) {
    AndroidApplicationSnapshot snapshot{Phase::Application};
    snapshot.package_manager_available = true;
    Runtime limited(program, heap, snapshot);
    auto app = limited.library(operation(CurrentApplication), 0, {}, Admit);
    ASSERT_EQ(app.stop, ExecutionStop::Returned);
    unsigned work = 0;
    auto result =
        limited.library(operation(ContextPackageManager), app.value, {}, [&] {
          return work++ < budget ? ExecutionStop::Returned
                                 : ExecutionStop::InstructionLimit;
        });
    EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
    EXPECT_EQ(
        limited.library(operation(ContextPackageManager), app.value, {}, Admit)
            .stop,
        ExecutionStop::InstructionLimit);
  }
}
const FieldRef SourceDirectoryField{"Landroid/content/pm/ApplicationInfo;",
                                    "sourceDir", "Ljava/lang/String;"};
const FieldRef NativeDirectoryField{"Landroid/content/pm/ApplicationInfo;",
                                    "nativeLibraryDir", "Ljava/lang/String;"};
const FieldRef NonLocalizedLabelField{"Landroid/content/pm/PackageItemInfo;",
                                      "nonLocalizedLabel",
                                      "Ljava/lang/CharSequence;"};
AndroidApplicationSnapshot labelSnapshot() {
  AndroidApplicationSnapshot input{Phase::Application};
  input.package_manager_available = true;
  input.package_item_force_safe_labels = false;
  input.application_info.emplace();
  return input;
}
TEST_F(AndroidFrameworkTest,
       ApplicationLabelPreservesCharSequenceIdentityAndWhitespace) {
  const auto *field = program.findField(NonLocalizedLabelField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_TRUE(program.androidFrameworkField(*field));
  EXPECT_TRUE(has(field->access, "public"));
  EXPECT_FALSE(has(field->access, "final"));
  FieldRef inherited = NonLocalizedLabelField;
  inherited.owner = "Landroid/content/pm/ApplicationInfo;";
  EXPECT_EQ(program.findField(inherited, false), field);
  for (const auto &text : {std::string{}, std::string{" \tlabel\n"},
                           std::string{"\xc3\xa9"}, std::string{"a\0b", 3}}) {
    auto input = labelSnapshot();
    input.application_info->non_localized_label =
        std::optional<std::string>{text};
    Runtime state(program, heap, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto manager =
        state.library(operation(ContextPackageManager), app, {}, Admit).value;
    const uint64_t args[]{
        state.library(operation(ContextApplicationInfo), app, {}, Admit).value};
    auto label =
        state.library(operation(PackageApplicationLabel), manager, args, Admit);
    ASSERT_EQ(label.stop, ExecutionStop::Returned) << label.diagnostic;
    ASSERT_NE(label.value, 0u);
    EXPECT_EQ(heap->get(label.value)->Bytes, javaStringBytes(text));
    EXPECT_EQ(state.field(*field, args[0], {}, Admit, 65536).value,
              label.value);
    EXPECT_EQ(
        state.library(operation(PackageApplicationLabel), manager, args, Admit)
            .value,
        label.value);
    const auto *constructor = libraryMethod(
        MethodRef{"Ljava/lang/StringBuilder;", "<init>", {}, "V"}, false);
    ASSERT_NE(constructor, nullptr);
    auto constructed = state.library(*constructor, 0, {}, Admit);
    ASSERT_EQ(constructed.stop, ExecutionStop::Returned)
        << constructed.diagnostic;
    auto builder = heap->get(constructed.value);
    ASSERT_NE(builder, nullptr);
    ASSERT_EQ(
        state.field(*field, args[0], builder->Identity, Admit, 65536).stop,
        ExecutionStop::Returned);
    const uint64_t character[]{'!'};
    ASSERT_EQ(state
                  .library(operation(BuilderAppendChar), builder->Identity,
                           character, Admit)
                  .stop,
              ExecutionStop::Returned);
    auto mutable_label =
        state.library(operation(PackageApplicationLabel), manager, args, Admit);
    EXPECT_EQ(mutable_label.stop, ExecutionStop::Returned);
    EXPECT_EQ(mutable_label.value, builder->Identity);
    EXPECT_EQ(heap->get(mutable_label.value)->Bytes, javaStringBytes("!"));
  }
}
TEST_F(AndroidFrameworkTest,
       PackageResourcePathRetainsContextValueAfterInfoWrites) {
  for (const auto &text :
       {std::string{}, std::string{"relative//base.apk"},
        std::string{"/\xc3\xa9.apk"}, std::string{"a\0b", 3}}) {
    AndroidApplicationSnapshot input{Phase::Application};
    input.package_resource_path = std::optional<std::string>{text};
    Runtime state(program, heap, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto base = state.library(operation(BaseContext), app, {}, Admit).value;
    auto path = state.library(operation(PackageResourcePath), app, {}, Admit);
    ASSERT_EQ(path.stop, ExecutionStop::Returned) << path.diagnostic;
    ASSERT_NE(path.value, 0u);
    EXPECT_EQ(heap->get(path.value)->Bytes, javaStringBytes(text));
    EXPECT_EQ(
        state.library(operation(PackageResourcePath), base, {}, Admit).value,
        path.value);
    auto info =
        state.library(operation(ContextApplicationInfo), base, {}, Admit).value;
    const auto *field = program.findField(SourceDirectoryField, false);
    ASSERT_NE(field, nullptr);
    auto changed = heap->literal("/replacement.apk")->Identity;
    ASSERT_EQ(state.field(*field, info, changed, Admit, 65536).stop,
              ExecutionStop::Returned);
    EXPECT_EQ(
        state.library(operation(PackageResourcePath), app, {}, Admit).value,
        path.value);
    EXPECT_EQ(state.field(*field, info, {}, Admit, 65536).value, changed);
  }
}
TEST_F(AndroidFrameworkTest,
       PackageResourcePathKeepsUnknownNullAndForeignDistinct) {
  AndroidApplicationSnapshot input{Phase::Application};
  input.application_info =
      AndroidApplicationSnapshot::ApplicationInfo{"/known.apk"};
  Runtime unknown(program, heap, input);
  auto app = unknown.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto result = unknown.library(operation(PackageResourcePath), app, {}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_EQ(result.diagnostic, "Android package resource path is not supplied");
  input.package_resource_path = std::optional<std::string>{};
  Runtime state(program, heap, input);
  app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto base = state.library(operation(BaseContext), app, {}, Admit).value;
  for (auto receiver : {app, base}) {
    auto value =
        state.library(operation(PackageResourcePath), receiver, {}, Admit);
    EXPECT_EQ(value.stop, ExecutionStop::Returned) << value.diagnostic;
    EXPECT_EQ(value.value, 0u);
  }
  auto foreign = heap->allocate(Object::Instance, "android/app/ContextImpl", 0);
  ASSERT_NE(foreign, nullptr);
  EXPECT_EQ(
      state
          .library(operation(PackageResourcePath), foreign->Identity, {}, Admit)
          .stop,
      ExecutionStop::Unsupported);
  Runtime null_state(program, heap, input);
  auto null = null_state.library(operation(PackageResourcePath), 0, {}, Admit);
  EXPECT_EQ(null.stop, ExecutionStop::Thrown);
  EXPECT_EQ(null.exception, "java/lang/NullPointerException");
}
TEST_F(AndroidFrameworkTest,
       PackageResourcePathValidatesSnapshotAndTerminalBudgets) {
  for (auto phase : {Phase::Unattached, Phase::Attached}) {
    AndroidApplicationSnapshot input{phase};
    input.package_resource_path = std::optional<std::string>{};
    EXPECT_THROW((Runtime(program, heap, input)), Error);
  }
  for (const auto &text : {std::string(4097, 'x'), std::string{"\xff"}}) {
    AndroidApplicationSnapshot input{Phase::Application};
    input.package_resource_path = std::optional<std::string>{text};
    EXPECT_THROW((Runtime(program, heap, input)), Error);
  }
  AndroidApplicationSnapshot valid{Phase::Application};
  valid.package_resource_path =
      std::optional<std::string>{std::string(4096, 'x')};
  EXPECT_NO_THROW((Runtime(program, heap, valid)));
  for (unsigned mode = 0; mode != 5; ++mode) {
    auto bounded =
        std::make_shared<Heap>(mode == 4 ? 4096 : 65536, mode == 3 ? 1 : 128);
    auto input = valid;
    if (mode != 4)
      input.package_resource_path = std::optional<std::string>{"path"};
    Runtime state(program, bounded, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    ASSERT_NE(app, 0u);
    unsigned ticks = 0;
    auto result = state.library(operation(PackageResourcePath), app, {}, [&] {
      return mode < 3 && ++ticks > mode ? ExecutionStop::InstructionLimit
                                        : ExecutionStop::Returned;
    });
    auto expected =
        mode < 3 ? ExecutionStop::InstructionLimit : ExecutionStop::HeapLimit;
    EXPECT_EQ(result.stop, expected) << mode << ": " << result.diagnostic;
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              expected);
  }
}
TEST_F(AndroidFrameworkTest,
       PackageResourcePathDexAndLibraryCallsShareIdentity) {
  Class reader;
  reader.name = "Ltest/ResourcePathReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {
      reader.name, "read", {"Landroid/content/Context;"}, "Ljava/lang/String;"};
  method.access = {"public", "static"};
  method.registers = 2;
  Instruction invoke;
  invoke.opcode = "invoke-virtual";
  invoke.registers = {1};
  invoke.reference = MethodRef{"Landroid/content/Context;",
                               "getPackageResourcePath",
                               {},
                               "Ljava/lang/String;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {invoke, move, ret};
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  AndroidApplicationSnapshot input{Phase::Application};
  input.package_resource_path = std::optional<std::string>{"/resource.apk"};
  Runtime state(code, heap, input);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto base = state.library(operation(BaseContext), app, {}, Admit).value;
  auto expected = state.library(operation(PackageResourcePath), app, {}, Admit);
  ASSERT_EQ(expected.stop, ExecutionStop::Returned);
  const uint64_t args[]{base};
  auto result =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, expected.value);
}
TEST_F(AndroidFrameworkTest,
       ApplicationLabelRequiresExplicitStateAndOwnedObjects) {
  for (unsigned mode = 0; mode < 8; ++mode) {
    auto input = labelSnapshot();
    if (mode != 2)
      input.application_info->non_localized_label =
          mode == 3 ? std::optional<std::string>{}
                    : std::optional<std::string>{"label"};
    if (mode == 0)
      input.package_item_force_safe_labels.reset();
    if (mode == 1)
      input.package_item_force_safe_labels = true;
    Runtime state(program, heap, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto manager =
        state.library(operation(ContextPackageManager), app, {}, Admit).value;
    uint64_t args[]{
        state.library(operation(ContextApplicationInfo), app, {}, Admit).value};
    if (mode == 4)
      args[0] = 0;
    if (mode == 5)
      args[0] = heap->allocate(Object::Instance,
                               "android/content/pm/ApplicationInfo", 0)
                    ->Identity;
    if (mode == 6)
      manager = heap->allocate(Object::Instance,
                               "android/app/ApplicationPackageManager", 0)
                    ->Identity;
    if (mode == 7)
      args[0] = app;
    auto result =
        state.library(operation(PackageApplicationLabel), manager, args, Admit);
    EXPECT_EQ(result.stop, mode == 4   ? ExecutionStop::Thrown
                           : mode == 7 ? ExecutionStop::Invalid
                                       : ExecutionStop::Unsupported)
        << mode;
    if (mode == 4)
      EXPECT_EQ(result.exception, "java/lang/NullPointerException");
  }
}
TEST_F(AndroidFrameworkTest,
       ApplicationLabelQueryFieldsDoNotAliasContextOrOtherResults) {
  auto input = labelSnapshot();
  input.application_info->non_localized_label =
      std::optional<std::string>{"context"};
  AndroidApplicationSnapshot::ApplicationInfo info;
  info.non_localized_label = std::optional<std::string>{"query"};
  input.application_info_queries = {{"test.app", 0, info}};
  Runtime state(program, heap, input);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto manager =
      state.library(operation(ContextPackageManager), app, {}, Admit).value;
  auto context =
      state.library(operation(ContextApplicationInfo), app, {}, Admit).value;
  const uint64_t query_args[]{heap->literal("test.app")->Identity, 0};
  auto first = state.library(operation(PackageApplicationInfo), manager,
                             query_args, Admit);
  auto second = state.library(operation(PackageApplicationInfo), manager,
                              query_args, Admit);
  ASSERT_EQ(first.stop, ExecutionStop::Returned);
  ASSERT_EQ(second.stop, ExecutionStop::Returned);
  const auto *field = program.findField(NonLocalizedLabelField, false);
  auto changed = heap->literal("changed")->Identity;
  EXPECT_EQ(state.field(*field, first.value, changed, Admit, 65536).value,
            changed);
  for (auto [object, expected] : {std::pair{first.value, "changed"},
                                  {second.value, "query"},
                                  {context, "context"}}) {
    const uint64_t args[]{object};
    auto result =
        state.library(operation(PackageApplicationLabel), manager, args, Admit);
    ASSERT_EQ(result.stop, ExecutionStop::Returned);
    EXPECT_EQ(heap->get(result.value)->Bytes, javaStringBytes(expected));
  }
}
TEST_F(AndroidFrameworkTest,
       ApplicationLabelValidatesInputAndHonorsTerminalBudgets) {
  for (bool query : {false, true}) {
    for (const auto &bad : {std::string(4097, 'x'), std::string{"\xff"}}) {
      auto input = labelSnapshot();
      if (query) {
        AndroidApplicationSnapshot::ApplicationInfo info;
        info.non_localized_label = std::optional<std::string>{bad};
        input.application_info_queries = {{"test.app", 0, info}};
      } else {
        input.application_info->non_localized_label =
            std::optional<std::string>{bad};
      }
      EXPECT_THROW((Runtime(program, heap, input)), Error);
    }
  }
  for (unsigned mode = 0; mode < 5; ++mode) {
    auto input = labelSnapshot();
    input.application_info->non_localized_label = std::optional<std::string>{
        mode == 4 ? std::string(4096, 'x') : "label"};
    auto bounded =
        std::make_shared<Heap>(mode == 4 ? 4096 : 65536, mode == 3 ? 3 : 128);
    Runtime state(program, bounded, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto manager =
        state.library(operation(ContextPackageManager), app, {}, Admit).value;
    auto info =
        state.library(operation(ContextApplicationInfo), app, {}, Admit);
    ASSERT_EQ(info.stop, ExecutionStop::Returned);
    const uint64_t args[]{info.value};
    unsigned ticks = 0;
    auto result =
        state.library(operation(PackageApplicationLabel), manager, args, [&] {
          return mode < 3 && ++ticks > mode ? ExecutionStop::InstructionLimit
                                            : ExecutionStop::Returned;
        });
    const auto expected =
        mode < 3 ? ExecutionStop::InstructionLimit : ExecutionStop::HeapLimit;
    EXPECT_EQ(result.stop, expected) << mode;
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              expected);
  }
}
TEST_F(AndroidFrameworkTest,
       ApplicationLabelSharesDexFieldWritesWithLibraryCalls) {
  Class reader;
  reader.name = "Ltest/LabelReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {reader.name,
                      "replace",
                      {"Landroid/content/pm/PackageManager;",
                       "Landroid/content/pm/ApplicationInfo;",
                       "Ljava/lang/CharSequence;"},
                      "Ljava/lang/CharSequence;"};
  method.access = {"public", "static"};
  method.registers = 4;
  Instruction put;
  put.opcode = "iput-object";
  put.registers = {3, 2};
  put.reference = NonLocalizedLabelField;
  Instruction get;
  get.pc = 2;
  get.opcode = "invoke-virtual";
  get.registers = {1, 2};
  get.reference = MethodRef{"Landroid/content/pm/PackageManager;",
                            "getApplicationLabel",
                            {"Landroid/content/pm/ApplicationInfo;"},
                            "Ljava/lang/CharSequence;"};
  Instruction move;
  move.pc = 5;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction ret;
  ret.pc = 6;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {put, get, move, ret};
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  Runtime state(code, heap, labelSnapshot());
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto manager =
      state.library(operation(ContextPackageManager), app, {}, Admit).value;
  auto info =
      state.library(operation(ContextApplicationInfo), app, {}, Admit).value;
  auto replacement = heap->literal("  from DEX  ")->Identity;
  const uint64_t args[]{manager, info, replacement};
  auto result =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, replacement);
  EXPECT_EQ(state
                .field(*code.findField(NonLocalizedLabelField, false), info, {},
                       Admit, 65536)
                .value,
            replacement);
}
TEST_F(AndroidFrameworkTest,
       PackageApplicationInfoResultsHaveIndependentFields) {
  AndroidApplicationSnapshot input{Phase::Application};
  input.package_manager_available = true;
  input.application_info =
      AndroidApplicationSnapshot::ApplicationInfo{"/context"};
  input.application_info_queries = {
      {"test.app", 0, {"/query", std::optional<std::string>{"/query/lib"}}},
      {"test.app", -1, {std::nullopt, std::optional<std::string>{}}},
      {"other.\xc3\xa9", 0, {"/unicode"}}};
  Runtime state(program, heap, input);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto context_info =
      state.library(operation(ContextApplicationInfo), app, {}, Admit).value;
  auto manager =
      state.library(operation(ContextPackageManager), app, {}, Admit).value;
  uint64_t args[]{heap->literal("test.app")->Identity, 0};
  auto first =
      state.library(operation(PackageApplicationInfo), manager, args, Admit);
  auto second =
      state.library(operation(PackageApplicationInfo), manager, args, Admit);
  ASSERT_EQ(first.stop, ExecutionStop::Returned);
  ASSERT_EQ(second.stop, ExecutionStop::Returned);
  EXPECT_NE(first.value, second.value);
  EXPECT_NE(first.value, context_info);
  EXPECT_EQ(heap->get(first.value)->ClassName,
            "android/content/pm/ApplicationInfo");
  const auto *source = program.findField(SourceDirectoryField, false);
  const auto *native = program.findField(NativeDirectoryField, false);
  auto read = [&](uint64_t object, const Field *field) {
    auto result = state.field(*field, object, {}, Admit, 65536);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  };
  auto changed = heap->literal("changed")->Identity;
  EXPECT_EQ(state.field(*source, first.value, changed, Admit, 65536).value,
            changed);
  EXPECT_EQ(state.field(*native, first.value, 0, Admit, 65536).value, 0u);
  EXPECT_EQ(read(first.value, source), changed);
  EXPECT_EQ(read(first.value, native), 0u);
  EXPECT_EQ(heap->get(read(second.value, source))->Bytes,
            javaStringBytes("/query"));
  EXPECT_EQ(heap->get(read(second.value, native))->Bytes,
            javaStringBytes("/query/lib"));
  EXPECT_EQ(heap->get(read(context_info, source))->Bytes,
            javaStringBytes("/context"));
  args[1] = UINT64_MAX; // Java int arguments use their low 32 bits.
  auto nulls =
      state.library(operation(PackageApplicationInfo), manager, args, Admit);
  ASSERT_EQ(nulls.stop, ExecutionStop::Returned);
  EXPECT_EQ(read(nulls.value, source), 0u);
  EXPECT_EQ(read(nulls.value, native), 0u);
  args[0] = heap->literal("other.\xc3\xa9")->Identity;
  args[1] = 0;
  auto unicode =
      state.library(operation(PackageApplicationInfo), manager, args, Admit);
  ASSERT_EQ(unicode.stop, ExecutionStop::Returned);
  EXPECT_EQ(heap->get(read(unicode.value, source))->Bytes,
            javaStringBytes("/unicode"));
  EXPECT_EQ(state.field(*native, unicode.value, {}, Admit, 65536).stop,
            ExecutionStop::Unsupported);
}
TEST_F(AndroidFrameworkTest,
       PackageApplicationInfoRejectsUnsuppliedQueriesAndOwners) {
  for (unsigned mode = 0; mode < 8; ++mode) {
    AndroidApplicationSnapshot input{Phase::Application};
    input.package_manager_available = true;
    if (mode != 0)
      input.application_info_queries = {{"test.app", 0, {"/query"}}};
    Runtime state(program, heap, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto manager =
        state.library(operation(ContextPackageManager), app, {}, Admit).value;
    uint64_t args[]{
        heap->literal(mode == 1 ? "other.app" : "test.app")->Identity,
        mode == 2 ? 1u : 0u};
    if (mode == 3)
      args[0] = 0;
    if (mode == 4)
      manager = heap->allocate(Object::Instance,
                               "android/app/ApplicationPackageManager", 0)
                    ->Identity;
    if (mode == 5)
      manager = 0;
    if (mode == 6)
      args[0] = app;
    if (mode == 7)
      args[0] = heap->literal(std::string(1025, 'x'))->Identity;
    auto result =
        state.library(operation(PackageApplicationInfo), manager, args, Admit);
    EXPECT_EQ(result.stop, mode == 5   ? ExecutionStop::Thrown
                           : mode == 6 ? ExecutionStop::Invalid
                                       : ExecutionStop::Unsupported)
        << mode;
    if (mode == 5)
      EXPECT_EQ(result.exception, "java/lang/NullPointerException");
  }
}
TEST_F(AndroidFrameworkTest, PackageApplicationInfoValidatesTypedResponses) {
  AndroidApplicationSnapshot good{Phase::Application};
  good.package_manager_available = true;
  good.application_info_queries = {{"test.app", INT32_MIN, {"/query"}}};
  EXPECT_NO_THROW((Runtime(program, heap, good)));
  for (unsigned mode = 0; mode < 10; ++mode) {
    auto bad = good;
    auto &query = bad.application_info_queries[0];
    switch (mode) {
    case 0:
      bad.phase = Phase::Attached;
      break;
    case 1:
      bad.package_manager_available.reset();
      break;
    case 2:
      bad.package_manager_available = false;
      break;
    case 3:
      query.package_name.clear();
      break;
    case 4:
      query.package_name = std::string("a\0b", 3);
      break;
    case 5:
      query.package_name = std::string(1025, 'x');
      break;
    case 6:
      query.info.source_dir = std::string(4097, 'x');
      break;
    case 7:
      query.info.native_library_dir = std::string(4097, 'x');
      break;
    case 8:
      bad.application_info_queries.push_back(query);
      break;
    case 9:
      query.package_name = "\xff";
      break;
    }
    EXPECT_THROW((Runtime(program, heap, bad)), Error) << mode;
  }
  for (unsigned i = 1; i < 64; ++i)
    good.application_info_queries.push_back(
        {"test." + std::to_string(i), 0, {}});
  EXPECT_NO_THROW((Runtime(program, heap, good)));
  good.application_info_queries.push_back({"test.extra", 0, {}});
  EXPECT_THROW((Runtime(program, heap, good)), Error);
}
TEST_F(AndroidFrameworkTest, PackageApplicationInfoHonorsTerminalBudgets) {
  for (unsigned mode = 0; mode < 4; ++mode) {
    AndroidApplicationSnapshot input{Phase::Application};
    input.package_manager_available = true;
    input.application_info_queries = {{"test.app", 0, {"/query"}}};
    auto bounded = std::make_shared<Heap>(65536, 16);
    Runtime state(program, bounded, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto manager =
        state.library(operation(ContextPackageManager), app, {}, Admit).value;
    const uint64_t args[]{bounded->literal("test.app")->Identity, 0};
    if (mode == 0)
      while (bounded->allocate(Object::Instance, "java/lang/Object", 0)) {
      }
    unsigned ticks = 0;
    auto result =
        state.library(operation(PackageApplicationInfo), manager, args, [&] {
          return mode != 0 && ++ticks > (mode == 1   ? 0u
                                         : mode == 2 ? 4u
                                                     : 10u)
                     ? ExecutionStop::InstructionLimit
                     : ExecutionStop::Returned;
        });
    const auto expected =
        mode == 0 ? ExecutionStop::HeapLimit : ExecutionStop::InstructionLimit;
    EXPECT_EQ(result.stop, expected) << mode;
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              expected);
  }
  AndroidApplicationSnapshot input{Phase::Application};
  input.package_manager_available = true;
  input.application_info_queries = {{"test.app", 0, {"/query"}}};
  auto byte_limited = std::make_shared<Heap>(4096, 4096);
  Runtime state(program, byte_limited, input);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto manager =
      state.library(operation(ContextPackageManager), app, {}, Admit).value;
  const uint64_t args[]{byte_limited->literal("test.app")->Identity, 0};
  ExecutionResult result;
  unsigned count = 0;
  do {
    result =
        state.library(operation(PackageApplicationInfo), manager, args, Admit);
    ++count;
  } while (result.stop == ExecutionStop::Returned && count < 64);
  EXPECT_GT(count, 1u);
  EXPECT_LT(count, 64u);
  EXPECT_EQ(result.stop, ExecutionStop::HeapLimit);
  EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
            ExecutionStop::HeapLimit);
}
TEST_F(AndroidFrameworkTest, PackageQueriesAdmitOneResultAllocation) {
  AndroidApplicationSnapshot input{Phase::Application};
  input.package_manager_available = true;
  input.application_info_queries = {{"test.app", 0, {"/query"}}};
  input.package_info_queries = {{"test.app", 0, {}}};
  unsigned query_work = 0;
  for (auto op : {PackageInfoQuery, PackageApplicationInfo}) {
    SCOPED_TRACE(op);
    for (bool complete : {true, false}) {
      SCOPED_TRACE(complete);
      auto storage = std::make_shared<Heap>();
      Runtime state(program, storage, input);
      auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
      ASSERT_EQ(app.stop, ExecutionStop::Returned);
      auto manager =
          state.library(operation(ContextPackageManager), app.value, {}, Admit);
      ASSERT_EQ(manager.stop, ExecutionStop::Returned);
      auto name = storage->literal("test.app");
      ASSERT_TRUE(name);
      auto before = storage->allocate(Object::Instance, "java/lang/Object", 0);
      ASSERT_TRUE(before);
      const uint64_t args[]{name->Identity, 0};
      unsigned work = 0;
      auto result = state.library(operation(op), manager.value, args, [&] {
        ++work;
        return query_work && work > query_work - unsigned(!complete)
                   ? ExecutionStop::InstructionLimit
                   : ExecutionStop::Returned;
      });
      if (!query_work) {
        ASSERT_EQ(result.stop, ExecutionStop::Returned);
        query_work = work;
      }
      // The same name and query row require one fresh result in either API.
      EXPECT_EQ(work, query_work);
      if (complete) {
        ASSERT_EQ(result.stop, ExecutionStop::Returned);
        EXPECT_EQ(result.value, before->Identity + 1u);
        EXPECT_EQ(storage->get(result.value)->ClassName,
                  op == PackageInfoQuery
                      ? "android/content/pm/PackageInfo"
                      : "android/content/pm/ApplicationInfo");
      } else {
        EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
        EXPECT_TRUE(state.snapshot().terminal);
      }
      auto after = storage->allocate(Object::Instance, "java/lang/Object", 0);
      ASSERT_TRUE(after);
      EXPECT_EQ(after->Identity, before->Identity + (complete ? 2u : 1u));
    }
  }
}
TEST_F(AndroidFrameworkTest,
       PackageApplicationInfoDexAndLibraryShareFieldOwnership) {
  Class reader;
  reader.name = "Ltest/PackageInfoReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {
      reader.name,
      "query",
      {"Landroid/content/pm/PackageManager;", "Ljava/lang/String;", "I"},
      "Landroid/content/pm/ApplicationInfo;"};
  method.access = {"public", "static"};
  method.registers = 4;
  Instruction get;
  get.opcode = "invoke-virtual";
  get.registers = {1, 2, 3};
  get.reference = MethodRef{"Landroid/content/pm/PackageManager;",
                            "getApplicationInfo",
                            {"Ljava/lang/String;", "I"},
                            "Landroid/content/pm/ApplicationInfo;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction put;
  put.pc = 4;
  put.opcode = "iput-object";
  put.registers = {2, 0};
  put.reference = SourceDirectoryField;
  Instruction ret;
  ret.pc = 6;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {get, move, put, ret};
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  AndroidApplicationSnapshot input{Phase::Application};
  input.package_manager_available = true;
  input.application_info_queries = {{"test.app", 0, {"/query"}}};
  Runtime state(code, heap, input);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto manager =
      state.library(operation(ContextPackageManager), app, {}, Admit).value;
  const uint64_t args[]{manager, heap->literal("test.app")->Identity, 0};
  auto result =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  const auto *field = code.findField(SourceDirectoryField, false);
  EXPECT_EQ(state.field(*field, result.value, {}, Admit, 65536).value, args[1]);
  auto next = state.library(operation(PackageApplicationInfo), manager,
                            std::span(args).subspan(1), Admit);
  ASSERT_EQ(next.stop, ExecutionStop::Returned);
  EXPECT_NE(next.value, result.value);
  auto value = state.field(*field, next.value, {}, Admit, 65536);
  ASSERT_EQ(value.stop, ExecutionStop::Returned);
  EXPECT_EQ(heap->get(value.value)->Bytes, javaStringBytes("/query"));
}
TEST_F(AndroidFrameworkTest,
       ApplicationInfoNativeDirectoryRetainsIndependentField) {
  const auto *field = program.findField(NativeDirectoryField, false);
  const auto *source = program.findField(SourceDirectoryField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_TRUE(program.androidFrameworkField(*field));
  EXPECT_TRUE(has(field->access, "public"));
  EXPECT_FALSE(has(field->access, "final"));
  EXPECT_EQ(program.findField(NativeDirectoryField, true), nullptr);
  AndroidApplicationSnapshot input{Phase::Application};
  input.application_info =
      AndroidApplicationSnapshot::ApplicationInfo{"/source.apk"};
  for (auto value :
       {std::optional<std::string>{}, std::optional<std::string>{""},
        std::optional<std::string>{"/lib/\xc3\xa9"}}) {
    input.application_info->native_library_dir.emplace(value);
    Runtime state(program, heap, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto info =
        state.library(operation(ContextApplicationInfo), app, {}, Admit).value;
    auto source_value = state.field(*source, info, {}, Admit, 65536);
    ASSERT_EQ(source_value.stop, ExecutionStop::Returned);
    auto native = state.field(*field, info, {}, Admit, 65536);
    ASSERT_EQ(native.stop, ExecutionStop::Returned) << native.diagnostic;
    if (value) {
      ASSERT_NE(native.value, 0u);
      EXPECT_EQ(heap->get(native.value)->Bytes, javaStringBytes(*value));
    } else {
      EXPECT_EQ(native.value, 0u);
    }
    EXPECT_EQ(state.field(*field, info, {}, Admit, 65536).value, native.value);
    EXPECT_EQ(state.field(*field, info, source_value.value, Admit, 65536).value,
              source_value.value);
    EXPECT_EQ(state.field(*field, info, 0, Admit, 65536).value, 0u);
    EXPECT_EQ(state.field(*source, info, {}, Admit, 65536).value,
              source_value.value);
  }
}
TEST_F(AndroidFrameworkTest,
       ApplicationInfoNativeDirectoryDoesNotInferMissingState) {
  const auto *field = program.findField(NativeDirectoryField, false);
  AndroidApplicationSnapshot input{Phase::Application};
  input.application_info =
      AndroidApplicationSnapshot::ApplicationInfo{"/source.apk"};
  Runtime state(program, heap, input);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto info =
      state.library(operation(ContextApplicationInfo), app, {}, Admit).value;
  auto result = state.field(*field, info, {}, Admit, 65536);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("nativeLibraryDir is not supplied"),
            std::string::npos);
  for (auto value : {std::string(4097, 'x'), std::string("\xff")}) {
    input.application_info->native_library_dir.emplace(value);
    EXPECT_THROW(Runtime(program, heap, input), Error);
  }
}
TEST_F(AndroidFrameworkTest,
       ApplicationInfoRetainsNullableMutableSourceDirectory) {
  const auto *field = program.findField(SourceDirectoryField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_TRUE(has(field->access, "public"));
  EXPECT_FALSE(has(field->access, "final"));
  EXPECT_TRUE(program.androidFrameworkField(*field));
  Field copy = *field;
  EXPECT_FALSE(program.androidFrameworkField(copy));
  EXPECT_EQ(program.findField(SourceDirectoryField, true), nullptr);
  EXPECT_TRUE(program.assignable("Landroid/content/pm/ApplicationInfo;",
                                 "Landroid/content/pm/PackageItemInfo;"));
  EXPECT_TRUE(program.assignable("Landroid/content/pm/ApplicationInfo;",
                                 "Landroid/os/Parcelable;"));
  for (auto text :
       {std::optional<std::string>{}, std::optional<std::string>{""},
        std::optional<std::string>{"/app/base.apk"},
        std::optional<std::string>{std::string("a\0\xc3\xa9", 4)}}) {
    AndroidApplicationSnapshot input{Phase::Application};
    input.application_info = AndroidApplicationSnapshot::ApplicationInfo{text};
    Runtime state(program, heap, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto base = state.library(operation(BaseContext), app, {}, Admit).value;
    auto info =
        state.library(operation(ContextApplicationInfo), app, {}, Admit);
    ASSERT_EQ(info.stop, ExecutionStop::Returned) << info.diagnostic;
    EXPECT_NE(info.value, 0u);
    EXPECT_EQ(
        state.library(operation(ContextApplicationInfo), base, {}, Admit).value,
        info.value);
    auto value = state.field(*field, info.value, {}, Admit, 65536);
    ASSERT_EQ(value.stop, ExecutionStop::Returned) << value.diagnostic;
    if (text) {
      ASSERT_NE(value.value, 0u);
      EXPECT_EQ(heap->get(value.value)->Bytes, javaStringBytes(*text));
      EXPECT_NE(value.value, heap->literal(*text)->Identity);
    } else {
      EXPECT_EQ(value.value, 0u);
    }
    EXPECT_EQ(state.field(*field, info.value, {}, Admit, 65536).value,
              value.value);
    auto replacement = heap->literal("replacement")->Identity;
    EXPECT_EQ(state.field(*field, info.value, replacement, Admit, 65536).value,
              replacement);
    EXPECT_EQ(state.field(*field, info.value, {}, Admit, 65536).value,
              replacement);
    EXPECT_EQ(state.field(*field, info.value, 0, Admit, 65536).value, 0u);
    EXPECT_EQ(state.field(*field, info.value, {}, Admit, 65536).value, 0u);
  }
}
TEST_F(AndroidFrameworkTest, ApplicationInfoRejectsUnknownAndForeignState) {
  const auto *field = program.findField(SourceDirectoryField, false);
  auto app = call(CurrentApplication);
  auto info = call(ContextApplicationInfo, app);
  auto absent = runtime.field(*field, info, {}, Admit, 65536);
  EXPECT_EQ(absent.stop, ExecutionStop::Unsupported);
  EXPECT_NE(absent.diagnostic.find("sourceDir is not supplied"),
            std::string::npos);
  // A guest write establishes a value without inventing an initial read.
  Runtime writable(program, heap,
                   AndroidApplicationSnapshot{Phase::Application});
  app = writable.library(operation(CurrentApplication), 0, {}, Admit).value;
  info =
      writable.library(operation(ContextApplicationInfo), app, {}, Admit).value;
  EXPECT_EQ(writable.field(*field, info, 0, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(writable.field(*field, info, {}, Admit, 65536).value, 0u);
  for (auto type : {"android/content/pm/ApplicationInfo", "java/lang/Object"}) {
    Runtime state(program, heap,
                  AndroidApplicationSnapshot{Phase::Application});
    auto foreign = heap->allocate(Object::Instance, type, 0);
    EXPECT_EQ(state.field(*field, foreign->Identity, 0, Admit, 65536).stop,
              std::string_view(type) == "java/lang/Object"
                  ? ExecutionStop::Invalid
                  : ExecutionStop::Unsupported);
  }
  auto wrong = heap->allocate(Object::Instance, "java/lang/Object", 0);
  EXPECT_EQ(writable.field(*field, info, wrong->Identity, Admit, 65536).stop,
            ExecutionStop::Invalid);
  Runtime nulls(program, heap, AndroidApplicationSnapshot{Phase::Application});
  EXPECT_EQ(nulls.field(*field, 0, {}, Admit, 65536).exception,
            "java/lang/NullPointerException");
  EXPECT_EQ(
      nulls.library(operation(ContextApplicationInfo), 0, {}, Admit).exception,
      "java/lang/NullPointerException");
  for (auto phase : {Phase::Unattached, Phase::Attached, Phase::Application}) {
    Runtime state(program, heap, AndroidApplicationSnapshot{phase});
    auto foreign =
        heap->allocate(Object::Instance, "android/app/Application", 0);
    EXPECT_EQ(state
                  .library(operation(ContextApplicationInfo), foreign->Identity,
                           {}, Admit)
                  .stop,
              ExecutionStop::Unsupported);
  }
}
TEST_F(AndroidFrameworkTest,
       ApplicationInfoHonorsTypedInputAndTerminalBudgets) {
  AndroidApplicationSnapshot input{Phase::Application};
  input.application_info =
      AndroidApplicationSnapshot::ApplicationInfo{"/app.apk"};
  for (auto phase : {Phase::Unattached, Phase::Attached}) {
    auto invalid = input;
    invalid.phase = phase;
    EXPECT_THROW(Runtime(program, heap, invalid), Error);
  }
  for (auto text : {std::string(4097, 'x'), std::string("\xff")}) {
    auto invalid = input;
    invalid.application_info->source_dir = text;
    EXPECT_THROW(Runtime(program, heap, invalid), Error);
  }
  const auto *field = program.findField(SourceDirectoryField, false);
  for (unsigned limit : {1u, 2u}) {
    auto storage = std::make_shared<Heap>(65536, limit);
    Runtime state(program, storage, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto info =
        state.library(operation(ContextApplicationInfo), app, {}, Admit);
    auto stopped =
        limit == 1 ? info : state.field(*field, info.value, {}, Admit, 65536);
    EXPECT_EQ(stopped.stop, ExecutionStop::HeapLimit);
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              ExecutionStop::HeapLimit);
  }
  for (bool allocate_info : {false, true}) {
    SCOPED_TRACE(allocate_info);
    Runtime state(program, heap, input);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto info =
        allocate_info
            ? state.library(operation(ContextApplicationInfo), app, {}, Admit)
                  .value
            : 0;
    unsigned work = 0;
    auto budget = [&] {
      return work++ < 1 ? ExecutionStop::Returned
                        : ExecutionStop::InstructionLimit;
    };
    auto result =
        allocate_info
            ? state.field(*field, info, {}, budget, 65536)
            : state.library(operation(ContextApplicationInfo), app, {}, budget);
    EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              ExecutionStop::InstructionLimit);
  }
}
TEST_F(AndroidFrameworkTest, ApplicationInfoSharesDexFieldWritesAndReads) {
  for (const auto &selected : {SourceDirectoryField, NativeDirectoryField}) {
    Class reader;
    reader.name = "Ltest/InfoReader;";
    reader.superclass = "Ljava/lang/Object;";
    reader.access = {"public"};
    Method method;
    method.reference = {reader.name,
                        "replace",
                        {"Landroid/content/Context;", "Ljava/lang/String;"},
                        "Ljava/lang/String;"};
    method.access = {"public", "static"};
    method.registers = 3;
    Instruction get;
    get.opcode = "invoke-virtual";
    get.registers = {1};
    get.reference = MethodRef{"Landroid/content/Context;",
                              "getApplicationInfo",
                              {},
                              "Landroid/content/pm/ApplicationInfo;"};
    Instruction move;
    move.pc = 3;
    move.opcode = "move-result-object";
    move.registers = {0};
    Instruction put;
    put.pc = 4;
    put.opcode = "iput-object";
    put.registers = {2, 0};
    put.reference = selected;
    Instruction read;
    read.pc = 6;
    read.opcode = "iget-object";
    read.registers = {0, 0};
    read.reference = selected;
    Instruction ret;
    ret.pc = 8;
    ret.opcode = "return-object";
    ret.registers = {0};
    method.instructions = {get, move, put, read, ret};
    reader.methods.push_back(method);
    Program code({reader}, Properties{});
    Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Application});
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto base = state.library(operation(BaseContext), app, {}, Admit).value;
    auto info =
        state.library(operation(ContextApplicationInfo), app, {}, Admit).value;
    const auto *field = code.findField(selected, false);
    for (auto context : {app, base}) {
      const uint64_t args[]{context, heap->literal("dex-value")->Identity};
      auto result =
          state.execute(*code.findStatic(method.reference), args, Admit, 65536);
      ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
      EXPECT_EQ(result.value, args[1]);
      EXPECT_EQ(state.field(*field, info, {}, Admit, 65536).value, args[1]);
      EXPECT_EQ(state.field(*field, info, 0, Admit, 65536).value, 0u);
    }
  }
}
TEST_F(AndroidFrameworkTest, SecureCacheDistinguishesUnknownNullEmptyAndValue) {
  using Cache = AndroidApplicationSnapshot::SecureSettingsCache;
  for (auto input :
       {std::optional<Cache>{}, std::optional{Cache{}},
        std::optional{Cache{""}},
        std::optional{Cache{std::string("A\0", 2) + "\xc3\xa9"}}}) {
    AndroidApplicationSnapshot snapshot{Phase::Application};
    snapshot.secure_settings_cache = input;
    Runtime state(program, heap, snapshot);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto resolver =
        state.library(operation(ContextContentResolver), app, {}, Admit).value;
    const uint64_t args[]{resolver, heap->literal("android_id")->Identity};
    auto result = state.library(operation(SecureSettingString), 0, args, Admit);
    if (!input) {
      EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
      continue;
    }
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(
        state.library(operation(SecureSettingString), 0, args, Admit).value,
        result.value);
    if (!input->android_id) {
      EXPECT_EQ(result.value, 0u);
    } else {
      ASSERT_NE(result.value, 0u);
      EXPECT_EQ(heap->get(result.value)->Bytes,
                javaStringBytes(*input->android_id));
    }
  }
}
TEST_F(AndroidFrameworkTest, SecureCacheRequiresOwnResolverAndExactKey) {
  AndroidApplicationSnapshot snapshot{Phase::Application};
  snapshot.secure_settings_cache =
      AndroidApplicationSnapshot::SecureSettingsCache{"fixture"};
  for (unsigned mode = 0; mode != 4; ++mode) {
    Runtime state(program, heap, snapshot);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto resolver =
        state.library(operation(ContextContentResolver), app, {}, Admit).value;
    uint64_t args[]{resolver, heap->literal("android_id")->Identity};
    if (mode == 0)
      args[0] = 0;
    if (mode == 1)
      args[0] = heap->allocate(
                        Object::Instance,
                        "android/app/ContextImpl$ApplicationContentResolver", 0)
                    ->Identity;
    if (mode == 2)
      args[1] = heap->literal("unmodeled_key")->Identity;
    if (mode == 3)
      args[1] = 0;
    auto result = state.library(operation(SecureSettingString), 0, args, Admit);
    EXPECT_EQ(result.stop,
              mode == 0 ? ExecutionStop::Thrown : ExecutionStop::Unsupported);
    if (mode == 0) {
      EXPECT_EQ(result.exception, "java/lang/NullPointerException");
      EXPECT_FALSE(state.snapshot().terminal);
    }
  }
}
TEST_F(AndroidFrameworkTest, SecureCacheValidatesTypedStateAndBudgets) {
  using Cache = AndroidApplicationSnapshot::SecureSettingsCache;
  AndroidApplicationSnapshot snapshot{Phase::Application};
  snapshot.secure_settings_cache = Cache{"value"};
  for (auto phase : {Phase::Unattached, Phase::Attached}) {
    snapshot.phase = phase;
    EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  }
  snapshot.phase = Phase::Application;
  for (auto bad : {std::string(4097, 'x'), std::string("\xc0\x80")}) {
    snapshot.secure_settings_cache = Cache{bad};
    EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  }
  snapshot.secure_settings_cache = Cache{"value"};
  for (unsigned mode = 0; mode != 3; ++mode) {
    auto storage = std::make_shared<Heap>(65536, mode == 0 ? 3 : 4096);
    Runtime state(program, storage, snapshot);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto resolver =
        state.library(operation(ContextContentResolver), app, {}, Admit).value;
    const uint64_t args[]{resolver, storage->literal("android_id")->Identity};
    unsigned remaining = mode == 1 ? 0 : 11;
    auto result = state.library(operation(SecureSettingString), 0, args, [&] {
      if (mode == 0)
        return ExecutionStop::Returned;
      if (!remaining)
        return ExecutionStop::InstructionLimit;
      --remaining;
      return ExecutionStop::Returned;
    });
    EXPECT_EQ(result.stop, mode == 0 ? ExecutionStop::HeapLimit
                                     : ExecutionStop::InstructionLimit);
    EXPECT_EQ(
        state.library(operation(SecureSettingString), 0, args, Admit).stop,
        result.stop);
  }
}
TEST_F(AndroidFrameworkTest, SettingsIntUsesGlobalCacheAndJavaDefaultRules) {
  constexpr uint32_t fallback = uint32_t(-23);
  struct Case {
    std::optional<std::string> text;
    uint32_t expected;
  };
  const Case cases[]{{std::nullopt, fallback},
                     {"0", 0},
                     {"1", 1},
                     {"-0", 0},
                     {"+7", 7},
                     {"00042", 42},
                     {"2147483647", 2147483647u},
                     {"-2147483648", 2147483648u},
                     {"2147483648", fallback},
                     {"-2147483649", fallback},
                     {"99999999999999999999", fallback},
                     {"", fallback},
                     {"+", fallback},
                     {"-", fallback},
                     {"0x10", fallback},
                     {" 1", fallback},
                     {"1 ", fallback},
                     {"1.0", fallback},
                     {std::string("1\0", 2), fallback}};
  for (const auto &test : cases) {
    AndroidApplicationSnapshot snapshot{Phase::Application};
    snapshot.secure_settings_cache =
        AndroidApplicationSnapshot::SecureSettingsCache{"99"};
    snapshot.global_settings_cache =
        AndroidApplicationSnapshot::GlobalSettingsCache{test.text};
    Runtime state(program, heap, snapshot);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto resolver =
        state.library(operation(ContextContentResolver), app, {}, Admit).value;
    const uint64_t args[]{resolver, heap->literal("adb_enabled")->Identity,
                          fallback};
    auto result =
        state.library(operation(SecureSettingIntDefault), 0, args, Admit);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, test.expected) << test.text.value_or("<null>");
    auto text = state.library(operation(SecureSettingString), 0,
                              std::span(args).first<2>(), Admit);
    ASSERT_EQ(text.stop, ExecutionStop::Returned);
    if (test.text) {
      ASSERT_NE(text.value, 0u);
      EXPECT_EQ(heap->get(text.value)->Bytes, javaStringBytes(*test.text));
      EXPECT_EQ(state
                    .library(operation(SecureSettingString), 0,
                             std::span(args).first<2>(), Admit)
                    .value,
                text.value);
    } else {
      EXPECT_EQ(text.value, 0u);
    }
    const uint64_t secure[]{resolver, heap->literal("android_id")->Identity, 0};
    EXPECT_EQ(
        state.library(operation(SecureSettingIntDefault), 0, secure, Admit)
            .value,
        99u);
  }
}
TEST_F(AndroidFrameworkTest, SettingsIntPreservesUnknownAndUnicodeBoundaries) {
  for (auto value :
       {std::optional<std::string>{}, std::optional<std::string>{"\xd9\xa1"},
        std::optional<std::string>{"\xef\xbc\x91"}}) {
    AndroidApplicationSnapshot snapshot{Phase::Application};
    if (value)
      snapshot.global_settings_cache =
          AndroidApplicationSnapshot::GlobalSettingsCache{value};
    Runtime state(program, heap, snapshot);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto resolver =
        state.library(operation(ContextContentResolver), app, {}, Admit).value;
    const uint64_t args[]{resolver, heap->literal("adb_enabled")->Identity, 17};
    auto result =
        state.library(operation(SecureSettingIntDefault), 0, args, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_NE(
        result.diagnostic.find(value ? "non-ASCII" : "cache is not supplied"),
        std::string::npos);
  }
  AndroidApplicationSnapshot snapshot{Phase::Application};
  snapshot.global_settings_cache =
      AndroidApplicationSnapshot::GlobalSettingsCache{"1"};
  Runtime state(program, heap, snapshot);
  const uint64_t args[]{0, heap->literal("adb_enabled")->Identity, 17};
  auto result =
      state.library(operation(SecureSettingIntDefault), 0, args, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/NullPointerException");
  EXPECT_FALSE(state.snapshot().terminal);
  for (auto phase : {Phase::Unattached, Phase::Attached}) {
    snapshot.phase = phase;
    EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  }
  snapshot.phase = Phase::Application;
  for (auto bad : {std::string(4097, 'x'), std::string("\xc0\x80")}) {
    snapshot.global_settings_cache =
        AndroidApplicationSnapshot::GlobalSettingsCache{bad};
    EXPECT_THROW(Runtime(program, heap, snapshot), Error);
  }
}
TEST_F(AndroidFrameworkTest, SettingsIntSharesDexAndLibraryState) {
  Class reader;
  reader.name = "Ltest/IntegerSettingReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {
      reader.name,
      "read",
      {"Landroid/content/ContentResolver;", "Ljava/lang/String;", "I"},
      "I"};
  method.access = {"public", "static"};
  method.registers = 3;
  Instruction get;
  get.opcode = "invoke-static";
  get.registers = {0, 1, 2};
  get.reference = MethodRef{"Landroid/provider/Settings$Secure;", "getInt",
                            method.reference.parameters, "I"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result";
  move.registers = {0};
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return";
  ret.registers = {0};
  method.instructions = {get, move, ret};
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  AndroidApplicationSnapshot snapshot{Phase::Application};
  snapshot.global_settings_cache =
      AndroidApplicationSnapshot::GlobalSettingsCache{"-2147483648"};
  Runtime state(code, heap, snapshot);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto resolver =
      state.library(operation(ContextContentResolver), app, {}, Admit).value;
  const uint64_t args[]{resolver, heap->literal("adb_enabled")->Identity, 0};
  auto dex =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  ASSERT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
  EXPECT_EQ(dex.value, 2147483648u);
  EXPECT_EQ(
      state.library(operation(SecureSettingIntDefault), 0, args, Admit).value,
      dex.value);
}
TEST_F(AndroidFrameworkTest, SettingsIntRetainsTerminalHeapAndWorkLimits) {
  for (bool exhaust_heap : {false, true}) {
    auto storage = std::make_shared<Heap>(65536, exhaust_heap ? 3 : 4096);
    AndroidApplicationSnapshot snapshot{Phase::Application};
    snapshot.global_settings_cache =
        AndroidApplicationSnapshot::GlobalSettingsCache{std::string(128, '0')};
    Runtime state(program, storage, snapshot);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
    auto resolver =
        state.library(operation(ContextContentResolver), app, {}, Admit).value;
    const uint64_t args[]{resolver, storage->literal("adb_enabled")->Identity,
                          17};
    unsigned work = 0;
    auto result =
        state.library(operation(SecureSettingIntDefault), 0, args, [&] {
          return !exhaust_heap && ++work > 20 ? ExecutionStop::InstructionLimit
                                              : ExecutionStop::Returned;
        });
    EXPECT_EQ(result.stop, exhaust_heap ? ExecutionStop::HeapLimit
                                        : ExecutionStop::InstructionLimit);
    EXPECT_EQ(
        state.library(operation(SecureSettingIntDefault), 0, args, Admit).stop,
        result.stop);
  }
}
TEST_F(AndroidFrameworkTest, DexReadsTheSameRetainedSecureCacheString) {
  Class reader;
  reader.name = "Ltest/SecureReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {
      reader.name,
      "read",
      {"Landroid/content/ContentResolver;", "Ljava/lang/String;"},
      "Ljava/lang/String;"};
  method.access = {"public", "static"};
  method.registers = 2;
  Instruction get;
  get.opcode = "invoke-static";
  get.registers = {0, 1};
  get.reference =
      MethodRef{"Landroid/provider/Settings$Secure;", "getString",
                method.reference.parameters, method.reference.returns};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {get, move, ret};
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  AndroidApplicationSnapshot snapshot{Phase::Application};
  snapshot.secure_settings_cache =
      AndroidApplicationSnapshot::SecureSettingsCache{"fixture"};
  Runtime state(code, heap, snapshot);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto resolver =
      state.library(operation(ContextContentResolver), app, {}, Admit).value;
  const uint64_t args[]{resolver, heap->literal("android_id")->Identity};
  auto result =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(heap->get(result.value)->Bytes, javaStringBytes("fixture"));
  EXPECT_EQ(state.library(operation(SecureSettingString), 0, args, Admit).value,
            result.value);
}
TEST_F(AndroidFrameworkTest,
       SecureKeyConstantsShareDexAndRuntimeLiteralStorage) {
  for (auto [name, text] : {std::pair{"ADB_ENABLED", "adb_enabled"},
                            {"ANDROID_ID", "android_id"}}) {
    const FieldRef key{"Landroid/provider/Settings$Secure;", name,
                       "Ljava/lang/String;"};
    Class reader;
    reader.name = "Ltest/ConstantReader;";
    reader.superclass = "Ljava/lang/Object;";
    reader.access = {"public"};
    Method method;
    method.reference = {reader.name, "read", {}, "Ljava/lang/String;"};
    method.access = {"public", "static"};
    method.registers = 1;
    Instruction get;
    get.opcode = "sget-object";
    get.registers = {0};
    get.reference = key;
    Instruction ret;
    ret.pc = 2;
    ret.opcode = "return-object";
    ret.registers = {0};
    method.instructions = {get, ret};
    reader.methods.push_back(method);
    Program code({reader}, Properties{});
    // Constants do not depend on a supplied application or provider snapshot.
    Runtime state(code, heap);
    const auto *field = code.findStaticField(key);
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(code.findField(key, false), nullptr);
    EXPECT_FALSE(code.androidFrameworkField(*field));
    auto dex =
        state.execute(*code.findStatic(method.reference), {}, Admit, 65536);
    ASSERT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
    EXPECT_EQ(dex.value, heap->literal(text)->Identity);
    auto direct = state.field(*field, 0, {}, Admit, 65536);
    ASSERT_EQ(direct.stop, ExecutionStop::Returned) << direct.diagnostic;
    EXPECT_EQ(direct.value, dex.value);
    auto write = state.field(*field, 0, heap->literal("replacement")->Identity,
                             Admit, 65536);
    EXPECT_EQ(write.stop, ExecutionStop::Invalid);
    EXPECT_NE(write.diagnostic.find("final field write"), std::string::npos);
  }
}
TEST_F(AndroidFrameworkTest, ContentResolverRequiresItsOwnContextAndLifecycle) {
  for (auto phase :
       {std::optional<Phase>{}, std::optional{Phase::Unattached},
        std::optional{Phase::Attached}, std::optional{Phase::Application}}) {
    Runtime state(program, heap,
                  phase ? std::optional{AndroidApplicationSnapshot{*phase}}
                        : std::nullopt);
    auto foreign =
        heap->allocate(Object::Instance, "android/app/Application", 0);
    auto result = state.library(operation(ContextContentResolver),
                                foreign->Identity, {}, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              ExecutionStop::Unsupported);
  }
  auto null = runtime.library(operation(ContextContentResolver), 0, {}, Admit);
  EXPECT_EQ(null.stop, ExecutionStop::Thrown);
  EXPECT_EQ(null.exception, "java/lang/NullPointerException");
  auto wrong = heap->allocate(Object::Instance, "java/lang/Object", 0);
  EXPECT_EQ(runtime
                .library(operation(ContextContentResolver), wrong->Identity, {},
                         Admit)
                .stop,
            ExecutionStop::Invalid);
}
TEST_F(AndroidFrameworkTest, ContentResolverHonorsHeapAndWorkLimits) {
  auto bounded = std::make_shared<Heap>(65536, 1);
  Runtime state(program, bounded,
                AndroidApplicationSnapshot{Phase::Application});
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
  ASSERT_EQ(app.stop, ExecutionStop::Returned);
  EXPECT_EQ(
      state.library(operation(ContextContentResolver), app.value, {}, Admit)
          .stop,
      ExecutionStop::HeapLimit);
  EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
            ExecutionStop::HeapLimit);
  for (unsigned budget : {0u, 1u}) {
    Runtime limited(program, heap,
                    AndroidApplicationSnapshot{Phase::Application});
    auto instance =
        limited.library(operation(CurrentApplication), 0, {}, Admit);
    ASSERT_EQ(instance.stop, ExecutionStop::Returned);
    unsigned work = 0;
    auto result = limited.library(
        operation(ContextContentResolver), instance.value, {}, [&] {
          return work++ < budget ? ExecutionStop::Returned
                                 : ExecutionStop::InstructionLimit;
        });
    EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
    EXPECT_EQ(limited
                  .library(operation(ContextContentResolver), instance.value,
                           {}, Admit)
                  .stop,
              ExecutionStop::InstructionLimit);
  }
}
TEST_F(AndroidFrameworkTest, DexAndLibraryShareTheContextResolver) {
  Class reader;
  reader.name = "Ltest/ResolverReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {reader.name,
                      "read",
                      {"Landroid/content/Context;"},
                      "Landroid/content/ContentResolver;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction get;
  get.opcode = "invoke-virtual";
  get.registers = {0};
  get.reference = MethodRef{"Landroid/content/Context;",
                            "getContentResolver",
                            {},
                            "Landroid/content/ContentResolver;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {get, move, ret};
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Application});
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto base = state.library(operation(BaseContext), app, {}, Admit).value;
  auto resolver =
      state.library(operation(ContextContentResolver), app, {}, Admit);
  ASSERT_EQ(resolver.stop, ExecutionStop::Returned);
  for (uint64_t receiver : {app, base}) {
    const uint64_t args[]{receiver};
    auto result =
        state.execute(*code.findStatic(method.reference), args, Admit, 65536);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, resolver.value);
  }
}
TEST_F(AndroidFrameworkTest, DexAndLibraryShareThePackageManager) {
  Class reader;
  reader.name = "Ltest/PackageManagerReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {reader.name,
                      "read",
                      {"Landroid/content/Context;"},
                      "Landroid/content/pm/PackageManager;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction get;
  get.opcode = "invoke-virtual";
  get.registers = {0};
  get.reference = MethodRef{"Landroid/content/Context;",
                            "getPackageManager",
                            {},
                            "Landroid/content/pm/PackageManager;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {get, move, ret};
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  AndroidApplicationSnapshot snapshot{Phase::Application};
  snapshot.package_manager_available = true;
  Runtime state(code, heap, snapshot);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto base = state.library(operation(BaseContext), app, {}, Admit).value;
  auto manager =
      state.library(operation(ContextPackageManager), app, {}, Admit);
  ASSERT_EQ(manager.stop, ExecutionStop::Returned);
  for (uint64_t receiver : {app, base}) {
    const uint64_t args[]{receiver};
    auto result =
        state.execute(*code.findStatic(method.reference), args, Admit, 65536);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, manager.value);
  }
}
TEST_F(AndroidFrameworkTest,
       FilesDirectoryDoesNotManufactureMissingStateOrForeignFiles) {
  auto app = call(CurrentApplication);
  auto missing = runtime.library(operation(FilesDirectory), app, {}, Admit);
  EXPECT_EQ(missing.stop, ExecutionStop::Unsupported);
  EXPECT_NE(missing.diagnostic.find("files directory"), std::string::npos);
  Runtime state(program, heap,
                AndroidApplicationSnapshot{Phase::Application, {}, "/files"});
  auto foreign = heap->allocate(Object::Instance, "java/io/File", 0);
  auto invalid =
      state.library(operation(FilePath), foreign->Identity, {}, Admit);
  EXPECT_EQ(invalid.stop, ExecutionStop::Unsupported);
  Runtime nullable(
      program, heap,
      AndroidApplicationSnapshot{Phase::Application, {}, "/files"});
  EXPECT_EQ(nullable.library(operation(FilePath), 0, {}, Admit).stop,
            ExecutionStop::Thrown);
}
TEST_F(AndroidFrameworkTest, FilesDirectoryValidatesPhaseTextAndBudget) {
  for (const char *path : {"", "relative", "\xff"}) {
    EXPECT_THROW(
        Runtime(program, heap,
                AndroidApplicationSnapshot{Phase::Application, {}, path}),
        Error);
  }
  EXPECT_THROW(
      Runtime(program, heap,
              AndroidApplicationSnapshot{Phase::Attached, {}, "/files"}),
      Error);
  Runtime state(program, heap,
                AndroidApplicationSnapshot{Phase::Application, {}, "/files"});
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  unsigned steps = 0;
  auto r = state.library(operation(FilesDirectory), app, {}, [&] {
    return ++steps <= 2 ? ExecutionStop::Returned
                        : ExecutionStop::InstructionLimit;
  });
  EXPECT_EQ(r.stop, ExecutionStop::InstructionLimit);
  EXPECT_EQ(state.library(operation(FilesDirectory), app, {}, Admit).stop,
            r.stop);
}

TEST_F(AndroidFrameworkTest, PhaseIsExplicitAndAnUnknownStopIsTerminal) {
  Runtime unknown(program, heap);
  auto result = unknown.library(operation(CurrentActivityThread), 0, {}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("snapshot"), std::string::npos);
  EXPECT_EQ(
      unknown.library(operation(SystemClassLoader), 0, {}, Admit).diagnostic,
      result.diagnostic);
  for (auto phase : {Phase::Unattached, Phase::Attached, Phase::Application}) {
    Runtime state(program, heap, AndroidApplicationSnapshot{phase, {}});
    auto thread = state.library(operation(CurrentActivityThread), 0, {}, Admit);
    auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
    ASSERT_EQ(thread.stop, ExecutionStop::Returned);
    ASSERT_EQ(app.stop, ExecutionStop::Returned);
    EXPECT_EQ(thread.value != 0, phase != Phase::Unattached);
    EXPECT_EQ(app.value != 0, phase == Phase::Application);
    if (thread.value) {
      auto instance =
          state.library(operation(ThreadApplication), thread.value, {}, Admit);
      EXPECT_EQ(instance.stop, ExecutionStop::Returned);
      EXPECT_EQ(instance.value, app.value);
    }
  }
}
TEST_F(AndroidFrameworkTest,
       ApplicationAndBaseContextRetainDistinctIdentities) {
  auto thread = call(CurrentActivityThread);
  auto app = call(ThreadApplication, thread);
  auto base = call(BaseContext, app);
  ASSERT_NE(thread, 0u);
  ASSERT_NE(app, 0u);
  ASSERT_NE(base, 0u);
  EXPECT_NE(thread, app);
  EXPECT_NE(app, base);
  EXPECT_EQ(heap->get(app)->ClassName, "android/app/Application");
  EXPECT_EQ(heap->get(base)->ClassName, "android/app/ContextImpl");
  EXPECT_EQ(call(CurrentActivityThread), thread);
  EXPECT_EQ(call(CurrentApplication), app);
  EXPECT_EQ(call(ApplicationContext, app), app);
  EXPECT_EQ(call(ApplicationContext, base), app);
  EXPECT_EQ(call(BaseContext, app), base);
  EXPECT_EQ(call(ContextClassLoader, app), call(ContextClassLoader, base));
  auto package = call(PackageName, app);
  EXPECT_EQ(call(PackageName, base), package);
  EXPECT_EQ(heap->get(package)->Length, 8u);
  EXPECT_EQ(heap->get(package)->Bytes, javaStringBytes("test.app"));
}
TEST_F(AndroidFrameworkTest, FrameworkHierarchyPreservesActualParentEdges) {
  EXPECT_EQ(program.parent("Landroid/app/ActivityThread;"),
            "Landroid/app/ClientTransactionHandler;");
  EXPECT_EQ(program.parent("Landroid/app/Application;"),
            "Landroid/content/ContextWrapper;");
  EXPECT_TRUE(program.assignable("Landroid/app/Application;",
                                 "Landroid/content/ComponentCallbacks;"));
  EXPECT_TRUE(program.assignable("Landroid/app/Application;",
                                 "Landroid/content/Context;"));
  EXPECT_FALSE(program.assignable("Landroid/app/ContextImpl;",
                                  "Landroid/app/Application;"));
  Class impossible;
  impossible.name = "Ltest/BadThread;";
  impossible.superclass = "Landroid/app/ActivityThread;";
  Program invalid({impossible}, Properties{});
  EXPECT_EQ(invalid.unsupportedClass(impossible.name),
            "DEX invalid superclass");
}
TEST_F(AndroidFrameworkTest, Api28HasDistinctBootSystemAndApplicationLoaders) {
  auto app = call(CurrentApplication);
  auto app_loader = call(ContextClassLoader, app);
  auto system_loader = call(SystemClassLoader);
  auto app_class = call(GetClass, app);
  auto boot = call(ClassLoaderOf, app_class);
  EXPECT_NE(app_loader, system_loader);
  EXPECT_NE(app_loader, boot);
  EXPECT_NE(system_loader, boot);
  EXPECT_EQ(heap->get(app_loader)->ClassName, "dalvik/system/PathClassLoader");
  EXPECT_EQ(heap->get(boot)->ClassName, "java/lang/BootClassLoader");
  EXPECT_EQ(call(LoaderParent, app_loader), boot);
  EXPECT_EQ(call(LoaderParent, system_loader), boot);
  EXPECT_EQ(call(LoaderParent, boot), 0u);
  EXPECT_EQ(call(ClassLoaderOf, call(GetClass, app_loader)), boot);
  for (auto loader : {app_loader, system_loader, boot}) {
    auto name = heap->literal("android.app.Application");
    EXPECT_EQ(call(LoadClass, loader, {name->Identity}), app_class);
  }
  EXPECT_EQ(call(ClassLoaderOf, heap->classObject("[I")->Identity), boot);
  EXPECT_EQ(
      call(ClassLoaderOf, heap->classObject("[[Ljava/lang/String;")->Identity),
      boot);
}
TEST_F(AndroidFrameworkTest, DexAndLibraryCallsShareTheApplicationIdentity) {
  Class reader;
  reader.name = "Ltest/Reader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {
      reader.name, "application", {}, "Landroid/app/Application;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction thread;
  thread.opcode = "invoke-static";
  thread.reference = MethodRef{"Landroid/app/ActivityThread;",
                               "currentActivityThread",
                               {},
                               "Landroid/app/ActivityThread;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction get;
  get.pc = 4;
  get.opcode = "invoke-virtual";
  get.registers = {0};
  get.reference = MethodRef{"Landroid/app/ActivityThread;",
                            "getApplication",
                            {},
                            "Landroid/app/Application;"};
  Instruction ret;
  ret.pc = 8;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {thread, move, get, move, ret};
  method.instructions[3].pc = 7;
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Application, {}});
  auto direct = state.library(operation(CurrentApplication), 0, {}, Admit);
  for (unsigned i = 0; i != 2; ++i) {
    auto dex =
        state.execute(*code.findStatic(method.reference), {}, Admit, 65536);
    EXPECT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
    EXPECT_EQ(dex.value, direct.value);
  }
}
TEST_F(AndroidFrameworkTest, DexAndNativeFileAccessShareTheSameObjects) {
  Class reader;
  reader.name = "Ltest/FileReader;";
  reader.superclass = "Ljava/lang/Object;";
  reader.access = {"public"};
  Method method;
  method.reference = {
      reader.name, "path", {"Landroid/content/Context;"}, "Ljava/lang/String;"};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction get;
  get.opcode = "invoke-virtual";
  get.registers = {0};
  get.reference = MethodRef{
      "Landroid/content/Context;", "getFilesDir", {}, "Ljava/io/File;"};
  Instruction move;
  move.pc = 3;
  move.opcode = "move-result-object";
  move.registers = {0};
  Instruction path = get;
  path.pc = 4;
  path.reference =
      MethodRef{"Ljava/io/File;", "getAbsolutePath", {}, "Ljava/lang/String;"};
  Instruction move2 = move;
  move2.pc = 7;
  Instruction ret;
  ret.pc = 8;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {get, move, path, move2, ret};
  reader.methods.push_back(method);
  Program code({reader}, Properties{});
  ASSERT_TRUE(code.knownReference("Ljava/io/File;"));
  ASSERT_FALSE(code.unsupportedClass("Ljava/io/File;"));
  Runtime state(
      code, heap,
      AndroidApplicationSnapshot{Phase::Application, {}, "/data/files"});
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto file = state.library(operation(FilesDirectory), app, {}, Admit).value;
  auto direct = state.library(operation(FilePath), file, {}, Admit);
  const uint64_t args[]{app};
  auto dex =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  EXPECT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
  EXPECT_EQ(dex.value, direct.value);
}
TEST_F(AndroidFrameworkTest, LoadClassDoesNotRunSuppliedClassInitialization) {
  Class type;
  type.name = "Ltest/Deferred;";
  type.superclass = "Ljava/lang/Object;";
  type.access = {"public"};
  Method init;
  init.reference = {type.name, "<clinit>", {}, "V"};
  init.access = {"static", "constructor"};
  init.registers = 1;
  Instruction zero;
  zero.opcode = "const/4";
  zero.registers = {0};
  zero.literal = int64_t(0);
  Instruction raise;
  raise.pc = 1;
  raise.opcode = "throw";
  raise.registers = {0};
  init.instructions = {zero, raise};
  type.methods.push_back(init);
  Program code({type}, Properties{});
  Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Application, {}});
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
  auto loader =
      state.library(operation(ContextClassLoader), app.value, {}, Admit);
  auto text = heap->literal("test.Deferred");
  const uint64_t args[]{text->Identity};
  auto loaded = state.library(operation(LoadClass), loader.value, args, Admit);
  EXPECT_EQ(loaded.stop, ExecutionStop::Returned) << loaded.diagnostic;
  EXPECT_EQ(loaded.value, heap->classObject("test/Deferred")->Identity);
  auto owner = state.library(operation(ClassLoaderOf), loaded.value, {}, Admit);
  EXPECT_EQ(owner.value, loader.value);
  auto initialized = state.initialize(type.name, Admit, 65536);
  EXPECT_EQ(initialized.stop, ExecutionStop::Thrown);
  EXPECT_EQ(initialized.exception, "java/lang/ExceptionInInitializerError");
  auto loaded_again =
      state.library(operation(LoadClass), loader.value, args, Admit);
  EXPECT_EQ(loaded_again.stop, ExecutionStop::Returned);
  EXPECT_EQ(loaded_again.value, loaded.value);
  auto again = state.initialize(type.name, Admit, 65536);
  EXPECT_EQ(again.exception, "java/lang/NoClassDefFoundError");
  EXPECT_EQ(again.stop, ExecutionStop::Thrown);
  auto array = heap->classObject("[[Ltest/Deferred;");
  EXPECT_EQ(
      state.library(operation(ClassLoaderOf), array->Identity, {}, Admit).value,
      loader.value);
}
TEST_F(AndroidFrameworkTest, UnknownPackageAndForeignInstancesAreNotInvented) {
  Runtime no_package(program, heap,
                     AndroidApplicationSnapshot{Phase::Application, {}});
  auto app = no_package.library(operation(CurrentApplication), 0, {}, Admit);
  auto result =
      no_package.library(operation(PackageName), app.value, {}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("package"), std::string::npos);
  auto foreign =
      heap->allocate(Object::Instance, "android/app/ActivityThread", 0);
  auto denied = runtime.library(operation(ThreadApplication), foreign->Identity,
                                {}, Admit);
  EXPECT_EQ(denied.stop, ExecutionStop::Unsupported);
}
TEST_F(AndroidFrameworkTest,
       NoImplicitMissingClassesOrSystemLoaderAppNamespace) {
  for (const char *text : {"test.Missing", "java.lang.NotAClass", "[I",
                           "java/lang/String", "test.\xc3\xa9"}) {
    SCOPED_TRACE(text);
    Runtime state(program, heap,
                  AndroidApplicationSnapshot{Phase::Application, {}});
    auto loader = state.library(operation(SystemClassLoader), 0, {}, Admit);
    const uint64_t args[]{heap->literal(text)->Identity};
    auto loaded =
        state.library(operation(LoadClass), loader.value, args, Admit);
    EXPECT_EQ(loaded.stop, ExecutionStop::Unsupported) << loaded.diagnostic;
    EXPECT_TRUE(loaded.exception.empty());
  }
  Class supplied;
  supplied.name = "Ltest/Known;";
  supplied.superclass = "Ljava/lang/Object;";
  supplied.access = {"public"};
  Program code({supplied}, Properties{});
  Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Application, {}});
  auto loader = state.library(operation(SystemClassLoader), 0, {}, Admit);
  const uint64_t args[]{heap->literal("test.Known")->Identity};
  auto loaded = state.library(operation(LoadClass), loader.value, args, Admit);
  EXPECT_EQ(loaded.stop, ExecutionStop::Unsupported);
}
TEST_F(AndroidFrameworkTest, NullAndWrongTypeKeepCommonLibraryValidation) {
  auto loader = call(SystemClassLoader);
  const uint64_t null[]{0};
  auto result = runtime.library(operation(LoadClass), loader, null, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/NullPointerException");
  EXPECT_NE(result.throwable, 0u);
  EXPECT_EQ(call(SystemClassLoader), loader);
  auto wrong = runtime.library(operation(ThreadApplication), loader, {}, Admit);
  EXPECT_EQ(wrong.stop, ExecutionStop::Invalid);
  EXPECT_NE(wrong.diagnostic.find("receiver type"), std::string::npos);
}
TEST_F(AndroidFrameworkTest, WorkAndHeapBudgetsPoisonPartialState) {
  for (auto limit : {0, 1}) {
    Runtime state(program, heap,
                  AndroidApplicationSnapshot{Phase::Application, {}});
    unsigned used = 0;
    auto limited = [&] {
      return used++ < unsigned(limit) ? ExecutionStop::Returned
                                      : ExecutionStop::InstructionLimit;
    };
    auto result = state.library(operation(CurrentApplication), 0, {}, limited);
    EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
    EXPECT_EQ(
        state.library(operation(CurrentApplication), 0, {}, Admit).diagnostic,
        result.diagnostic);
  }
  Runtime bounded(program, std::make_shared<Heap>(64, 1),
                  AndroidApplicationSnapshot{Phase::Application, {}});
  auto result = bounded.library(operation(CurrentApplication), 0, {}, Admit);
  EXPECT_EQ(result.stop, ExecutionStop::HeapLimit);
  EXPECT_EQ(bounded.library(operation(CurrentApplication), 0, {}, Admit).stop,
            ExecutionStop::HeapLimit);
}
TEST_F(AndroidFrameworkTest,
       InvalidSnapshotsAndPlatformReplacementAreRejected) {
  Program ordinary({});
  EXPECT_FALSE(ordinary.containsClass("Landroid/app/ActivityThread;"));
  EXPECT_THROW(
      Runtime(ordinary, heap, AndroidApplicationSnapshot{Phase::Attached, {}}),
      Error);
  EXPECT_THROW(
      Runtime(program, heap, AndroidApplicationSnapshot{Phase(99), {}}), Error);
  for (auto phase : {Phase::Unattached, Phase::Attached})
    EXPECT_THROW(
        Runtime(program, heap, AndroidApplicationSnapshot{phase, "app"}),
        Error);
  for (const std::string name : {std::string(), std::string(1025, 'x'),
                                 std::string("a\0b", 3), std::string("\xff")})
    EXPECT_THROW(Runtime(program, heap,
                         AndroidApplicationSnapshot{Phase::Application, name}),
                 Error);
  Class replacement;
  replacement.name = "Landroid/app/Application;";
  EXPECT_THROW(Program({replacement}, Properties{}), Error);
}
const FieldRef PackageInfoNameField{"Landroid/content/pm/PackageInfo;",
                                    "packageName", "Ljava/lang/String;"};
const FieldRef PackageInfoVersionField{"Landroid/content/pm/PackageInfo;",
                                       "versionName", "Ljava/lang/String;"};
const FieldRef PackageInfoCodeField{"Landroid/content/pm/PackageInfo;",
                                    "versionCode", "I"};
const FieldRef PackageApplicationInfoField{
    "Landroid/content/pm/PackageInfo;", "applicationInfo",
    "Landroid/content/pm/ApplicationInfo;"};
AndroidApplicationSnapshot packageInfoSnapshot() {
  AndroidApplicationSnapshot input{Phase::Application};
  input.package_manager_available = true;
  AndroidApplicationSnapshot::PackageInfo info;
  info.package_name = std::optional<std::string>{"result.app"};
  info.version_name = std::optional<std::string>{"fixture-version"};
  info.version_code = 7;
  input.package_info_queries = {{"test.app", 0, info}};
  return input;
}
uint64_t packageInfoManager(Runtime &state) {
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
  EXPECT_EQ(app.stop, ExecutionStop::Returned);
  auto manager =
      state.library(operation(ContextPackageManager), app.value, {}, Admit);
  EXPECT_EQ(manager.stop, ExecutionStop::Returned);
  return manager.value;
}
ExecutionResult queryPackageInfo(Runtime &state, Heap &store, uint64_t manager,
                                 std::string_view name = "test.app",
                                 uint64_t flags = 0) {
  auto text = store.literal(name);
  EXPECT_TRUE(text);
  const uint64_t args[]{text ? text->Identity : 0, flags};
  return state.library(operation(PackageInfoQuery), manager, args, Admit);
}
TEST_F(AndroidFrameworkTest, PackageInfoQueriesKeepResultsIndependent) {
  auto input = packageInfoSnapshot();
  auto alternate = input.package_info_queries[0].info;
  alternate.version_code = -1;
  input.package_info_queries.push_back({"test.app", -1, alternate});
  input.package_info_queries.push_back({"other.\xc3\xa9", 0, alternate});
  Runtime state(program, heap, input);
  auto manager = packageInfoManager(state);
  auto first = queryPackageInfo(state, *heap, manager);
  auto second = queryPackageInfo(state, *heap, manager);
  ASSERT_EQ(first.stop, ExecutionStop::Returned) << first.diagnostic;
  ASSERT_EQ(second.stop, ExecutionStop::Returned);
  EXPECT_NE(first.value, second.value);
  EXPECT_EQ(heap->get(first.value)->ClassName,
            "android/content/pm/PackageInfo");
  EXPECT_TRUE(program.assignable("Landroid/content/pm/PackageInfo;",
                                 "Landroid/os/Parcelable;"));
  const auto *version = program.findField(PackageInfoVersionField, false);
  ASSERT_NE(version, nullptr);
  auto original = state.field(*version, first.value, {}, Admit, 65536);
  ASSERT_EQ(original.stop, ExecutionStop::Returned);
  EXPECT_EQ(heap->get(original.value)->Bytes,
            javaStringBytes("fixture-version"));
  auto changed = heap->literal("changed")->Identity;
  EXPECT_EQ(state.field(*version, first.value, changed, Admit, 65536).value,
            changed);
  auto unchanged = state.field(*version, second.value, {}, Admit, 65536);
  ASSERT_EQ(unchanged.stop, ExecutionStop::Returned);
  EXPECT_EQ(heap->get(unchanged.value)->Bytes,
            javaStringBytes("fixture-version"));
  const auto *code = program.findField(PackageInfoCodeField, false);
  for (auto result :
       {queryPackageInfo(state, *heap, manager, "test.app",
                         0x12345678ffffffffULL),
        queryPackageInfo(state, *heap, manager, "other.\xc3\xa9")}) {
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(state.field(*code, result.value, {}, Admit, 65536).value,
              UINT32_MAX);
  }
  auto name = state.field(*program.findField(PackageInfoNameField, false),
                          first.value, {}, Admit, 65536);
  ASSERT_EQ(name.stop, ExecutionStop::Returned);
  EXPECT_EQ(heap->get(name.value)->Bytes, javaStringBytes("result.app"));
}
TEST_F(AndroidFrameworkTest, PackageInfoQueriesKeepUnknownInputsExplicit) {
  for (unsigned mode = 0; mode < 6; ++mode) {
    auto input = packageInfoSnapshot();
    if (mode == 0)
      input.package_info_queries.clear();
    Runtime state(program, heap, input);
    auto manager = packageInfoManager(state);
    auto text = heap->literal(mode == 1   ? "other.app"
                              : mode == 5 ? std::string(1025, 'x')
                                          : "test.app");
    uint64_t args[]{mode == 3 ? 0 : text->Identity, mode == 2 ? 1u : 0u};
    if (mode == 4)
      manager = heap->allocate(Object::Instance,
                               "android/app/ApplicationPackageManager", 0)
                    ->Identity;
    auto result =
        state.library(operation(PackageInfoQuery), manager, args, Admit);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported) << mode;
    EXPECT_TRUE(result.exception.empty());
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              ExecutionStop::Unsupported);
  }
}
TEST_F(AndroidFrameworkTest, PackageInfoFieldsDistinguishUnknownNullAndWrites) {
  for (const auto &ref :
       {PackageInfoNameField, PackageInfoVersionField, PackageInfoCodeField}) {
    auto input = packageInfoSnapshot();
    input.package_info_queries[0].info = {};
    Runtime state(program, heap, input);
    auto result = queryPackageInfo(state, *heap, packageInfoManager(state));
    ASSERT_EQ(result.stop, ExecutionStop::Returned);
    auto unknown = state.field(*program.findField(ref, false), result.value, {},
                               Admit, 65536);
    EXPECT_EQ(unknown.stop, ExecutionStop::Unsupported);
    EXPECT_NE(unknown.diagnostic.find("is not supplied"), std::string::npos);
  }
  auto input = packageInfoSnapshot();
  auto &info = input.package_info_queries[0].info;
  info.package_name = std::optional<std::string>{};
  info.version_name = std::optional<std::string>{};
  info.version_code = 0;
  Runtime known(program, heap, input);
  auto result = queryPackageInfo(known, *heap, packageInfoManager(known));
  ASSERT_EQ(result.stop, ExecutionStop::Returned);
  for (const auto &ref :
       {PackageInfoNameField, PackageInfoVersionField, PackageInfoCodeField}) {
    auto value = known.field(*program.findField(ref, false), result.value, {},
                             Admit, 65536);
    EXPECT_EQ(value.stop, ExecutionStop::Returned);
    EXPECT_EQ(value.value, 0u);
  }
  auto foreign =
      heap->allocate(Object::Instance, "android/content/pm/PackageInfo", 0);
  EXPECT_EQ(known
                .field(*program.findField(PackageInfoCodeField, false),
                       foreign->Identity, {}, Admit, 65536)
                .stop,
            ExecutionStop::Unsupported);
  info = {};
  Runtime written(program, heap, input);
  auto object = queryPackageInfo(written, *heap, packageInfoManager(written));
  ASSERT_EQ(object.stop, ExecutionStop::Returned);
  const auto *field = program.findField(PackageInfoVersionField, false);
  auto text = heap->literal("written")->Identity;
  EXPECT_EQ(written.field(*field, object.value, text, Admit, 65536).value,
            text);
  EXPECT_EQ(written.field(*field, object.value, {}, Admit, 65536).value, text);
  auto wrong = heap->allocate(Object::Instance, "java/lang/Object", 0);
  EXPECT_EQ(
      written.field(*field, object.value, wrong->Identity, Admit, 65536).stop,
      ExecutionStop::Invalid);
}
TEST_F(AndroidFrameworkTest, PackageInfoPrimitiveFieldsShareDexNarrowing) {
  Class owner;
  owner.name = "Ltest/PackageVersion;";
  owner.superclass = "Ljava/lang/Object;";
  owner.access = {"public"};
  Field ordinary;
  ordinary.reference = {owner.name, "ordinary", "I"};
  ordinary.access = {"public", "static"};
  owner.fields.push_back(ordinary);
  Method method;
  method.reference = {
      owner.name, "replace", {"Landroid/content/pm/PackageInfo;", "I"}, "I"};
  method.access = {"public", "static"};
  method.registers = 2;
  Instruction put;
  put.opcode = "iput";
  put.registers = {1, 0};
  put.reference = PackageInfoCodeField;
  Instruction get;
  get.pc = 2;
  get.opcode = "iget";
  get.registers = {1, 0};
  get.reference = PackageInfoCodeField;
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return";
  ret.registers = {1};
  method.instructions = {put, get, ret};
  owner.methods.push_back(method);
  Program code({owner}, Properties{});
  Runtime state(code, heap, packageInfoSnapshot());
  auto object = queryPackageInfo(state, *heap, packageInfoManager(state));
  ASSERT_EQ(object.stop, ExecutionStop::Returned);
  const auto *framework = code.findField(PackageInfoCodeField, false);
  const auto *plain = code.findField(ordinary.reference, true);
  for (uint64_t bits : std::vector<uint64_t>{
           0, 0xffffffff, 0x123456789abcdef0ULL, UINT64_MAX}) {
    auto a = state.field(*framework, object.value, bits, Admit, 65536);
    auto b = state.field(*plain, 0, bits, Admit, 65536);
    ASSERT_EQ(a.stop, ExecutionStop::Returned);
    ASSERT_EQ(b.stop, ExecutionStop::Returned);
    EXPECT_EQ(a.value, uint32_t(bits));
    EXPECT_EQ(a.value, b.value);
    EXPECT_EQ(state.field(*framework, object.value, {}, Admit, 65536).value,
              b.value);
  }
  const uint64_t args[]{object.value, 0xfedcba98};
  auto result =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 0xfedcba98u);
  EXPECT_EQ(state.field(*framework, object.value, {}, Admit, 65536).value,
            result.value);
}
TEST_F(AndroidFrameworkTest, PackageInfoQueriesValidateKeysAndFieldBounds) {
  for (auto phase : {Phase::Unattached, Phase::Attached}) {
    auto input = packageInfoSnapshot();
    input.phase = phase;
    EXPECT_THROW((Runtime(program, heap, input)), Error);
  }
  for (auto available : {std::optional<bool>{}, std::optional<bool>{false}}) {
    auto input = packageInfoSnapshot();
    input.package_manager_available = available;
    EXPECT_THROW((Runtime(program, heap, input)), Error);
  }
  auto duplicate = packageInfoSnapshot();
  duplicate.package_info_queries.push_back(
      duplicate.package_info_queries.front());
  EXPECT_THROW((Runtime(program, heap, duplicate)), Error);
  for (const auto &text : {std::string(), std::string(1025, 'x'),
                           std::string("a\0b", 3), std::string("\xff")}) {
    auto input = packageInfoSnapshot();
    input.package_info_queries[0].package_name = text;
    EXPECT_THROW((Runtime(program, heap, input)), Error);
  }
  for (bool version : {false, true}) {
    for (const auto &text : {std::string(4097, 'x'), std::string("\xff")}) {
      auto input = packageInfoSnapshot();
      auto &info = input.package_info_queries[0].info;
      (version ? info.version_name : info.package_name) =
          std::optional<std::string>{text};
      EXPECT_THROW((Runtime(program, heap, input)), Error);
    }
  }
  auto input = packageInfoSnapshot();
  for (int32_t flags = 1; flags < 64; ++flags)
    input.package_info_queries.push_back({"test.app", flags, {}});
  EXPECT_NO_THROW((Runtime(program, heap, input)));
  input.package_info_queries.push_back({"test.app", 64, {}});
  EXPECT_THROW((Runtime(program, heap, input)), Error);
}
TEST_F(AndroidFrameworkTest, PackageInfoApplicationKeepsUnknownNullAndWrites) {
  const auto *field = program.findField(PackageApplicationInfoField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(program.findField(PackageApplicationInfoField, true), nullptr);
  auto input = packageInfoSnapshot();
  Runtime unknown(program, heap, input);
  auto package = queryPackageInfo(unknown, *heap, packageInfoManager(unknown));
  ASSERT_EQ(package.stop, ExecutionStop::Returned);
  auto absent = unknown.field(*field, package.value, {}, Admit, 65536);
  EXPECT_EQ(absent.stop, ExecutionStop::Unsupported);
  EXPECT_NE(absent.diagnostic.find("applicationInfo is not supplied"),
            std::string::npos);

  input.package_info_queries[0].info.application_info.emplace();
  Runtime known(program, heap, input);
  auto null_package = queryPackageInfo(known, *heap, packageInfoManager(known));
  ASSERT_EQ(null_package.stop, ExecutionStop::Returned);
  auto null = known.field(*field, null_package.value, {}, Admit, 65536);
  ASSERT_EQ(null.stop, ExecutionStop::Returned);
  EXPECT_EQ(null.value, 0u);

  input.package_info_queries[0].info.application_info.reset();
  input.application_info =
      AndroidApplicationSnapshot::ApplicationInfo{std::string("context.apk")};
  Runtime written(program, heap, input);
  auto object = queryPackageInfo(written, *heap, packageInfoManager(written));
  auto app = written.library(operation(CurrentApplication), 0, {}, Admit);
  auto context =
      written.library(operation(ContextApplicationInfo), app.value, {}, Admit);
  ASSERT_EQ(context.stop, ExecutionStop::Returned);
  ASSERT_EQ(object.stop, ExecutionStop::Returned);
  EXPECT_EQ(
      written.field(*field, object.value, context.value, Admit, 65536).value,
      context.value);
  EXPECT_EQ(written.field(*field, object.value, {}, Admit, 65536).value,
            context.value);
  EXPECT_EQ(written.field(*field, object.value, 0, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(written.field(*field, object.value, {}, Admit, 65536).value, 0u);
  auto wrong = heap->literal("wrong reference");
  EXPECT_EQ(
      written.field(*field, object.value, wrong->Identity, Admit, 65536).stop,
      ExecutionStop::Invalid);
}
TEST_F(AndroidFrameworkTest, PackageInfoApplicationObjectsAreIndependent) {
  auto input = packageInfoSnapshot();
  AndroidApplicationSnapshot::ApplicationInfo nested;
  nested.source_dir = "parcel.apk";
  nested.native_library_dir = std::optional<std::string>{"parcel/lib"};
  nested.non_localized_label = std::optional<std::string>{"parcel label"};
  nested.target_sdk_version = 28;
  input.package_info_queries[0].info.application_info = nested;
  input.application_info =
      AndroidApplicationSnapshot::ApplicationInfo{std::string("context.apk")};
  input.application_info_queries = {
      {"test.app", 0, {std::string("application-query.apk")}}};
  input.package_item_force_safe_labels = false;
  Runtime state(program, heap, input);
  auto manager = packageInfoManager(state);
  auto first = queryPackageInfo(state, *heap, manager);
  auto second = queryPackageInfo(state, *heap, manager);
  const auto *field = program.findField(PackageApplicationInfoField, false);
  auto child = state.field(*field, first.value, {}, Admit, 65536);
  auto separate = state.field(*field, second.value, {}, Admit, 65536);
  ASSERT_EQ(child.stop, ExecutionStop::Returned);
  ASSERT_EQ(separate.stop, ExecutionStop::Returned);
  EXPECT_NE(child.value, separate.value);
  EXPECT_EQ(heap->get(child.value)->ClassName,
            "android/content/pm/ApplicationInfo");
  EXPECT_EQ(state.field(*field, first.value, {}, Admit, 65536).value,
            child.value);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
  auto context =
      state.library(operation(ContextApplicationInfo), app.value, {}, Admit);
  const uint64_t query_args[]{heap->literal("test.app")->Identity, 0};
  auto query = state.library(operation(PackageApplicationInfo), manager,
                             query_args, Admit);
  ASSERT_EQ(query.stop, ExecutionStop::Returned);
  EXPECT_NE(child.value, context.value);
  EXPECT_NE(child.value, query.value);
  auto text = [&](uint64_t object, const FieldRef &ref) {
    auto result =
        state.field(*program.findField(ref, false), object, {}, Admit, 65536);
    EXPECT_EQ(result.stop, ExecutionStop::Returned);
    auto value = heap->get(result.value);
    EXPECT_TRUE(value);
    return value ? value->Bytes : std::vector<uint8_t>{};
  };
  EXPECT_EQ(text(child.value, SourceDirectoryField),
            javaStringBytes("parcel.apk"));
  EXPECT_EQ(text(child.value, NativeDirectoryField),
            javaStringBytes("parcel/lib"));
  EXPECT_EQ(text(child.value, NonLocalizedLabelField),
            javaStringBytes("parcel label"));
  const FieldRef target{"Landroid/content/pm/ApplicationInfo;",
                        "targetSdkVersion", "I"};
  EXPECT_EQ(state
                .field(*program.findField(target, false), child.value, {},
                       Admit, 65536)
                .value,
            28u);
  const uint64_t label_args[]{child.value};
  auto label = state.library(operation(PackageApplicationLabel), manager,
                             label_args, Admit);
  ASSERT_EQ(label.stop, ExecutionStop::Returned);
  EXPECT_EQ(heap->get(label.value)->Bytes, javaStringBytes("parcel label"));
  auto changed = heap->literal("changed.apk")->Identity;
  EXPECT_EQ(state
                .field(*program.findField(SourceDirectoryField, false),
                       child.value, changed, Admit, 65536)
                .stop,
            ExecutionStop::Returned);
  EXPECT_EQ(text(separate.value, SourceDirectoryField),
            javaStringBytes("parcel.apk"));
  EXPECT_EQ(text(context.value, SourceDirectoryField),
            javaStringBytes("context.apk"));
  EXPECT_EQ(text(query.value, SourceDirectoryField),
            javaStringBytes("application-query.apk"));
  EXPECT_EQ(state.field(*field, first.value, context.value, Admit, 65536).value,
            context.value);
  EXPECT_EQ(text(child.value, SourceDirectoryField),
            javaStringBytes("changed.apk"));
}
TEST_F(AndroidFrameworkTest, PackageInfoApplicationDexSharesReferenceWrites) {
  Class owner;
  owner.name = "Ltest/PackageApplication;";
  owner.superclass = "Ljava/lang/Object;";
  owner.access = {"public"};
  Method method;
  method.reference = {owner.name,
                      "replace",
                      {"Landroid/content/pm/PackageInfo;",
                       "Landroid/content/pm/ApplicationInfo;"},
                      "Landroid/content/pm/ApplicationInfo;"};
  method.access = {"public", "static"};
  method.registers = 2;
  Instruction put;
  put.opcode = "iput-object";
  put.registers = {1, 0};
  put.reference = PackageApplicationInfoField;
  Instruction get;
  get.pc = 2;
  get.opcode = "iget-object";
  get.registers = {1, 0};
  get.reference = PackageApplicationInfoField;
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {1};
  method.instructions = {put, get, ret};
  owner.methods.push_back(method);
  Program code({owner}, Properties{});
  Runtime state(code, heap, packageInfoSnapshot());
  auto object = queryPackageInfo(state, *heap, packageInfoManager(state));
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
  auto context =
      state.library(operation(ContextApplicationInfo), app.value, {}, Admit);
  ASSERT_EQ(context.stop, ExecutionStop::Returned);
  const auto *field = code.findField(PackageApplicationInfoField, false);
  for (uint64_t value : {context.value, uint64_t(0)}) {
    const uint64_t args[]{object.value, value};
    auto result =
        state.execute(*code.findStatic(method.reference), args, Admit, 65536);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, value);
    EXPECT_EQ(state.field(*field, object.value, {}, Admit, 65536).value, value);
  }
}
TEST_F(AndroidFrameworkTest, PackageInfoApplicationValidatesNestedFields) {
  for (const auto &text : {std::string(4097, 'x'), std::string("\xff")}) {
    for (unsigned field = 0; field != 3; ++field) {
      auto input = packageInfoSnapshot();
      AndroidApplicationSnapshot::ApplicationInfo info;
      if (field == 0)
        info.source_dir = text;
      else if (field == 1)
        info.native_library_dir = std::optional<std::string>{text};
      else
        info.non_localized_label = std::optional<std::string>{text};
      input.package_info_queries[0].info.application_info = info;
      EXPECT_THROW((Runtime(program, heap, input)), Error);
    }
  }
}
TEST_F(AndroidFrameworkTest, PackageInfoApplicationHonorsTerminalBudgets) {
  for (bool work : {false, true}) {
    auto input = packageInfoSnapshot();
    input.package_info_queries[0].info.application_info =
        AndroidApplicationSnapshot::ApplicationInfo{};
    auto bounded = std::make_shared<Heap>(65536, 16);
    Runtime state(program, bounded, input);
    auto object = queryPackageInfo(state, *bounded, packageInfoManager(state));
    ASSERT_EQ(object.stop, ExecutionStop::Returned);
    if (!work)
      while (bounded->allocate(Object::Instance, "java/lang/Object", 0)) {
      }
    auto result = state.field(
        *program.findField(PackageApplicationInfoField, false), object.value,
        {},
        [&] {
          return work ? ExecutionStop::InstructionLimit
                      : ExecutionStop::Returned;
        },
        65536);
    auto expected =
        work ? ExecutionStop::InstructionLimit : ExecutionStop::HeapLimit;
    EXPECT_EQ(result.stop, expected);
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              expected);
  }
}
TEST_F(AndroidFrameworkTest, PackageInfoQueriesHonorTerminalBudgets) {
  for (unsigned mode = 0; mode < 5; ++mode) {
    auto input = packageInfoSnapshot();
    if (mode == 4)
      input.package_info_queries[0].info.version_name =
          std::optional<std::string>{std::string(4096, 'x')};
    auto bounded = std::make_shared<Heap>(mode == 4 ? 4096 : 65536, 16);
    Runtime state(program, bounded, input);
    auto manager = packageInfoManager(state);
    auto text = bounded->literal("test.app");
    ASSERT_TRUE(text);
    if (mode == 3)
      while (bounded->allocate(Object::Instance, "java/lang/Object", 0)) {
      }
    const uint64_t args[]{text->Identity, 0};
    unsigned ticks = 0;
    auto result =
        state.library(operation(PackageInfoQuery), manager, args, [&] {
          return mode < 3 && ++ticks > (mode == 0   ? 0u
                                        : mode == 1 ? 4u
                                                    : 10u)
                     ? ExecutionStop::InstructionLimit
                     : ExecutionStop::Returned;
        });
    if (mode == 4) {
      ASSERT_EQ(result.stop, ExecutionStop::Returned);
      result = state.field(*program.findField(PackageInfoVersionField, false),
                           result.value, {}, Admit, 65536);
    }
    const auto expected =
        mode < 3 ? ExecutionStop::InstructionLimit : ExecutionStop::HeapLimit;
    EXPECT_EQ(result.stop, expected) << mode;
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              expected);
  }
}
const FieldRef TargetSdkField{"Landroid/content/pm/ApplicationInfo;",
                              "targetSdkVersion", "I"};
uint64_t targetSdkInfo(Runtime &state) {
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit);
  EXPECT_EQ(app.stop, ExecutionStop::Returned);
  auto info =
      state.library(operation(ContextApplicationInfo), app.value, {}, Admit);
  EXPECT_EQ(info.stop, ExecutionStop::Returned);
  return info.value;
}
TEST_F(AndroidFrameworkTest, TargetSdkVersionIsIndependentOfPlatformVersion) {
  Program code({}, Properties{{"ro.build.version.sdk", "35"}});
  const auto *field = code.findField(TargetSdkField, false);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(code.findField(TargetSdkField, true), nullptr);
  for (bool supplied_info : {false, true}) {
    AndroidApplicationSnapshot input{Phase::Application};
    if (supplied_info)
      input.application_info.emplace();
    Runtime state(code, heap, input);
    auto result = state.field(*field, targetSdkInfo(state), {}, Admit, 65536);
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_NE(result.diagnostic.find("targetSdkVersion is not supplied"),
              std::string::npos);
  }
  for (int32_t value : {INT32_MIN, 0, 28, INT32_MAX}) {
    AndroidApplicationSnapshot input{Phase::Application};
    input.application_info.emplace().target_sdk_version = value;
    Runtime state(code, heap, input);
    auto result = state.field(*field, targetSdkInfo(state), {}, Admit, 65536);
    ASSERT_EQ(result.stop, ExecutionStop::Returned);
    EXPECT_EQ(result.value, uint32_t(value));
  }
}
TEST_F(AndroidFrameworkTest, TargetSdkVersionMutationsRemainWithTheirOwner) {
  AndroidApplicationSnapshot input{Phase::Application};
  input.package_manager_available = true;
  input.application_info.emplace().target_sdk_version = 28;
  AndroidApplicationSnapshot::ApplicationInfo query;
  query.target_sdk_version = 29;
  input.application_info_queries = {{"test.app", 0, query}};
  Runtime state(program, heap, input);
  const auto *field = program.findField(TargetSdkField, false);
  auto context = targetSdkInfo(state);
  auto app = state.library(operation(CurrentApplication), 0, {}, Admit).value;
  auto base = state.library(operation(BaseContext), app, {}, Admit).value;
  EXPECT_EQ(
      state.library(operation(ContextApplicationInfo), base, {}, Admit).value,
      context);
  auto manager = packageInfoManager(state);
  const uint64_t args[]{heap->literal("test.app")->Identity, 0};
  auto first =
      state.library(operation(PackageApplicationInfo), manager, args, Admit);
  auto second =
      state.library(operation(PackageApplicationInfo), manager, args, Admit);
  ASSERT_EQ(first.stop, ExecutionStop::Returned);
  ASSERT_EQ(second.stop, ExecutionStop::Returned);
  EXPECT_NE(first.value, second.value);
  EXPECT_EQ(state.field(*field, context, {}, Admit, 65536).value, 28u);
  EXPECT_EQ(state.field(*field, first.value, {}, Admit, 65536).value, 29u);
  EXPECT_EQ(state.field(*field, first.value, 31, Admit, 65536).value, 31u);
  EXPECT_EQ(state.field(*field, second.value, {}, Admit, 65536).value, 29u);
  EXPECT_EQ(state.field(*field, context, UINT64_MAX, Admit, 65536).value,
            UINT32_MAX);
  EXPECT_EQ(state.field(*field, first.value, {}, Admit, 65536).value, 31u);
  auto foreign =
      heap->allocate(Object::Instance, "android/content/pm/ApplicationInfo", 0);
  EXPECT_EQ(state.field(*field, foreign->Identity, {}, Admit, 65536).stop,
            ExecutionStop::Unsupported);
  input.application_info.reset();
  Runtime written(program, heap, input);
  auto object = targetSdkInfo(written);
  EXPECT_EQ(written.field(*field, object, 0, Admit, 65536).stop,
            ExecutionStop::Returned);
  auto zero = written.field(*field, object, {}, Admit, 65536);
  EXPECT_EQ(zero.stop, ExecutionStop::Returned);
  EXPECT_EQ(zero.value, 0u);
}
TEST_F(AndroidFrameworkTest, TargetSdkVersionDexWritesUseSharedFieldState) {
  Class owner;
  owner.name = "Ltest/TargetSdk;";
  owner.superclass = "Ljava/lang/Object;";
  owner.access = {"public"};
  Method method;
  method.reference = {owner.name,
                      "replace",
                      {"Landroid/content/pm/ApplicationInfo;", "I"},
                      "I"};
  method.access = {"public", "static"};
  method.registers = 2;
  Instruction put;
  put.opcode = "iput";
  put.registers = {1, 0};
  put.reference = TargetSdkField;
  Instruction get = put;
  get.pc = 2;
  get.opcode = "iget";
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return";
  ret.registers = {1};
  method.instructions = {put, get, ret};
  owner.methods.push_back(method);
  Program code({owner}, Properties{});
  Runtime state(code, heap, AndroidApplicationSnapshot{Phase::Application});
  auto info = targetSdkInfo(state);
  const uint64_t args[]{info, UINT32_MAX};
  auto result =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, UINT32_MAX);
  EXPECT_EQ(
      state
          .field(*code.findField(TargetSdkField, false), info, {}, Admit, 65536)
          .value,
      UINT32_MAX);
}
TEST_F(AndroidFrameworkTest, TargetSdkVersionHonorsTerminalWorkLimits) {
  for (bool write : {false, true}) {
    AndroidApplicationSnapshot input{Phase::Application};
    input.application_info.emplace().target_sdk_version = 28;
    Runtime state(program, heap, input);
    auto info = targetSdkInfo(state);
    auto result = state.field(
        *program.findField(TargetSdkField, false), info,
        write ? std::optional<uint64_t>{29} : std::nullopt,
        [] { return ExecutionStop::InstructionLimit; }, 65536);
    EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              ExecutionStop::InstructionLimit);
  }
}
const FieldRef SplitSourcesField{"Landroid/content/pm/ApplicationInfo;",
                                 "splitSourceDirs", "[Ljava/lang/String;"};
using SplitSources =
    AndroidApplicationSnapshot::ApplicationInfo::SplitSourceDirectories;
class AndroidSplitSourcesTest : public AndroidFrameworkTest,
                                public testing::WithParamInterface<bool> {
protected:
  FieldRef selectedField() const {
    return {"Landroid/content/pm/ApplicationInfo;",
            GetParam() ? "splitPublicSourceDirs" : "splitSourceDirs",
            "[Ljava/lang/String;"};
  }
  auto &
  suppliedSources(AndroidApplicationSnapshot::ApplicationInfo &info) const {
    return GetParam() ? info.split_public_source_dirs : info.split_source_dirs;
  }
};
TEST_P(AndroidSplitSourcesTest,
       SplitSourceDirsKeepsUnknownNullAndEmptyDistinct) {
  const auto *field = program.findField(selectedField(), false);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(program.findField(selectedField(), true), nullptr);
  for (unsigned mode = 0; mode != 4; ++mode) {
    AndroidApplicationSnapshot input{Phase::Application};
    if (mode)
      input.application_info.emplace();
    if (mode) {
      auto &other = GetParam()
                        ? input.application_info->split_source_dirs
                        : input.application_info->split_public_source_dirs;
      other = SplitSources{"other.apk"};
    }
    if (mode >= 2)
      suppliedSources(*input.application_info).emplace();
    if (mode == 3)
      suppliedSources(*input.application_info)->emplace();
    Runtime state(program, heap, input);
    auto info = targetSdkInfo(state);
    auto value = state.field(*field, info, {}, Admit, 65536);
    EXPECT_EQ(value.stop,
              mode < 2 ? ExecutionStop::Unsupported : ExecutionStop::Returned);
    if (mode == 2)
      EXPECT_EQ(value.value, 0u);
    if (mode == 3) {
      auto array = heap->get(value.value);
      ASSERT_TRUE(array);
      EXPECT_EQ(array->Type, Object::ObjectArray);
      EXPECT_EQ(array->ClassName, "[Ljava/lang/String;");
      EXPECT_EQ(array->Length, 0u);
      EXPECT_TRUE(array->Elements.empty());
      EXPECT_EQ(state.field(*field, info, {}, Admit, 65536).value, value.value);
    }
  }
  AndroidApplicationSnapshot input{Phase::Application};
  Runtime written(program, heap, input);
  auto info = targetSdkInfo(written);
  auto array = heap->allocate(Object::ObjectArray, "[Ljava/lang/String;", 0);
  ASSERT_TRUE(array);
  EXPECT_EQ(written.field(*field, info, array->Identity, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(written.field(*field, info, {}, Admit, 65536).value,
            array->Identity);
  EXPECT_EQ(written.field(*field, info, 0, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(written.field(*field, info, {}, Admit, 65536).value, 0u);
  EXPECT_EQ(
      written
          .field(*field, info, heap->literal("wrong")->Identity, Admit, 65536)
          .stop,
      ExecutionStop::Invalid);
}
TEST_P(AndroidSplitSourcesTest,
       SplitSourceDirsPreservesArraysAcrossOwnersAndDex) {
  Class owner;
  owner.name = "Ltest/SplitSources;";
  owner.superclass = "Ljava/lang/Object;";
  owner.access = {"public"};
  Method method;
  method.reference = {
      owner.name,
      "setElement",
      {"Landroid/content/pm/ApplicationInfo;", "Ljava/lang/String;", "I"},
      "[Ljava/lang/String;"};
  method.access = {"public", "static"};
  method.registers = 4;
  Instruction get;
  get.opcode = "iget-object";
  get.registers = {0, 1};
  get.reference = selectedField();
  Instruction put;
  put.pc = 2;
  put.opcode = "aput-object";
  put.registers = {2, 0, 3};
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  method.instructions = {get, put, ret};
  owner.methods.push_back(method);
  Program code({owner}, Properties{});
  auto input = packageInfoSnapshot();
  AndroidApplicationSnapshot::ApplicationInfo supplied;
  supplied.source_dir = "base.apk";
  suppliedSources(supplied) = SplitSources{"first.apk", std::nullopt, "",
                                           "\xe7\x8c\xab.apk", "first.apk"};
  input.application_info = supplied;
  input.application_info_queries = {{"test.app", 0, supplied}};
  input.package_info_queries[0].info.application_info = supplied;
  Runtime state(code, heap, input);
  const auto *field = code.findField(selectedField(), false);
  auto context = targetSdkInfo(state);
  auto manager = packageInfoManager(state);
  const uint64_t query_args[]{heap->literal("test.app")->Identity, 0};
  auto query = state.library(operation(PackageApplicationInfo), manager,
                             query_args, Admit);
  auto parcel = queryPackageInfo(state, *heap, manager);
  auto nested = state.field(*code.findField(PackageApplicationInfoField, false),
                            parcel.value, {}, Admit, 65536);
  ASSERT_EQ(query.stop, ExecutionStop::Returned);
  ASSERT_EQ(nested.stop, ExecutionStop::Returned);
  std::vector<uint64_t> arrays;
  for (auto info : {context, query.value, nested.value}) {
    auto result = state.field(*field, info, {}, Admit, 65536);
    ASSERT_EQ(result.stop, ExecutionStop::Returned);
    auto array = heap->get(result.value);
    ASSERT_TRUE(array);
    ASSERT_EQ(array->Length, 5u);
    ASSERT_EQ(array->Elements.size(), 5u);
    EXPECT_EQ(array->ElementSize, sizeof(std::weak_ptr<Object>));
    EXPECT_TRUE(array->Elements[1].expired());
    for (size_t i : {0u, 2u, 3u, 4u}) {
      auto text = array->Elements[i].lock();
      ASSERT_TRUE(text);
      EXPECT_EQ(text->Bytes,
                javaStringBytes(*(**suppliedSources(supplied))[i]));
    }
    EXPECT_EQ(state.field(*field, info, {}, Admit, 65536).value, result.value);
    arrays.push_back(result.value);
  }
  EXPECT_NE(arrays[0], arrays[1]);
  EXPECT_NE(arrays[0], arrays[2]);
  EXPECT_NE(arrays[1], arrays[2]);
  auto changed = heap->literal("changed");
  const uint64_t args[]{nested.value, changed->Identity, 1};
  auto dex =
      state.execute(*code.findStatic(method.reference), args, Admit, 65536);
  ASSERT_EQ(dex.stop, ExecutionStop::Returned) << dex.diagnostic;
  EXPECT_EQ(dex.value, arrays[2]);
  EXPECT_EQ(heap->get(arrays[2])->Elements[1].lock(), changed);
  EXPECT_TRUE(heap->get(arrays[0])->Elements[1].expired());
  EXPECT_TRUE(heap->get(arrays[1])->Elements[1].expired());
  EXPECT_EQ(state.field(*field, nested.value, arrays[0], Admit, 65536).value,
            arrays[0]);
  EXPECT_EQ(state.field(*field, nested.value, {}, Admit, 65536).value,
            arrays[0]);
  EXPECT_EQ(heap->get(arrays[2])->Elements[1].lock(), changed);
}
TEST_P(AndroidSplitSourcesTest,
       SplitSourceDirsValidatesTypedBoundsAndEncoding) {
  for (unsigned origin = 0; origin != 3; ++origin) {
    auto input = packageInfoSnapshot();
    input.application_info.emplace();
    input.application_info_queries = {{"test.app", 0, {}}};
    input.package_info_queries[0].info.application_info =
        AndroidApplicationSnapshot::ApplicationInfo{};
    auto &info = origin == 0 ? *input.application_info
                 : origin == 1
                     ? input.application_info_queries[0].info
                     : **input.package_info_queries[0].info.application_info;
    suppliedSources(info) = SplitSources(
        ::neverd::limits::dalvik::kMaxAndroidApplicationSplitSources);
    EXPECT_NO_THROW((Runtime(program, heap, input)));
    (**suppliedSources(info)).emplace_back();
    EXPECT_THROW((Runtime(program, heap, input)), Error);
    suppliedSources(info) = SplitSources{std::string(
        ::neverd::limits::dalvik::kMaxAndroidApplicationInfoFieldBytes, 'x')};
    EXPECT_NO_THROW((Runtime(program, heap, input)));
    for (const auto &bad :
         {std::string(
              ::neverd::limits::dalvik::kMaxAndroidApplicationInfoFieldBytes +
                  1,
              'x'),
          std::string("\xff")}) {
      suppliedSources(info) = SplitSources{bad};
      EXPECT_THROW((Runtime(program, heap, input)), Error);
    }
  }
}
TEST_P(AndroidSplitSourcesTest,
       SplitSourceDirsHasTerminalWorkAndStorageLimits) {
  for (unsigned mode = 0; mode != 4; ++mode) {
    auto bounded = std::make_shared<Heap>(65536, 32);
    AndroidApplicationSnapshot input{Phase::Application};
    suppliedSources(input.application_info.emplace()) =
        SplitSources{"first", std::nullopt, "second"};
    Runtime state(program, bounded, input);
    auto info = targetSdkInfo(state);
    if (mode == 3)
      while (bounded->allocate(Object::Instance, "java/lang/Object", 0)) {
      }
    unsigned admitted = 0;
    auto value = state.field(
        *program.findField(selectedField(), false), info, {},
        [&] {
          return mode != 3 && admitted++ == (mode == 2 ? 9 : mode * 3)
                     ? ExecutionStop::InstructionLimit
                     : ExecutionStop::Returned;
        },
        65536);
    auto expected =
        mode == 3 ? ExecutionStop::HeapLimit : ExecutionStop::InstructionLimit;
    EXPECT_EQ(value.stop, expected);
    EXPECT_EQ(state.library(operation(CurrentApplication), 0, {}, Admit).stop,
              expected);
  }
}
INSTANTIATE_TEST_SUITE_P(BothFields, AndroidSplitSourcesTest, testing::Bool());

TEST_F(AndroidFrameworkTest, SplitSourceDirsDoNotAliasPublicSourcesImplicitly) {
  AndroidApplicationSnapshot input{Phase::Application};
  auto &supplied = input.application_info.emplace();
  supplied.split_source_dirs = SplitSources{"same.apk", std::nullopt};
  supplied.split_public_source_dirs = supplied.split_source_dirs;
  Runtime state(program, heap, input);
  auto owner = targetSdkInfo(state);
  const auto *source = program.findField(SplitSourcesField, false);
  const auto *public_source =
      program.findField({"Landroid/content/pm/ApplicationInfo;",
                         "splitPublicSourceDirs", "[Ljava/lang/String;"},
                        false);
  ASSERT_TRUE(source && public_source);
  auto a = state.field(*source, owner, {}, Admit, 65536);
  auto b = state.field(*public_source, owner, {}, Admit, 65536);
  ASSERT_EQ(a.stop, ExecutionStop::Returned);
  ASSERT_EQ(b.stop, ExecutionStop::Returned);
  EXPECT_NE(a.value, b.value);
  EXPECT_EQ(heap->get(a.value)->Elements[0].lock()->Bytes,
            heap->get(b.value)->Elements[0].lock()->Bytes);
  auto null = state.field(*source, owner, 0, Admit, 65536);
  ASSERT_EQ(null.stop, ExecutionStop::Returned);
  EXPECT_EQ(state.field(*public_source, owner, {}, Admit, 65536).value,
            b.value);
  // A guest reference write may deliberately alias the two fields.
  ASSERT_EQ(state.field(*source, owner, b.value, Admit, 65536).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(state.field(*source, owner, {}, Admit, 65536).value, b.value);
}

} // namespace
} // namespace neverd::mobile::dalvik
