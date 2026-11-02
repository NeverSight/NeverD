//===- AndroidMessageQueueTests.cpp - Shared queue removal semantics ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent message ordering, callback identity and refusal boundaries.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"
#include "neverd/dalvik/Limits.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {

const AdmitInstruction AdmitMessage = [] { return ExecutionStop::Returned; };

Class queueAction() {
  Class Action;
  Action.name = "Lqueue/Action;";
  Action.superclass = "Ljava/lang/Object;";
  Action.interfaces = {"Ljava/lang/Runnable;"};
  Action.access = {"public"};
  Field Called;
  Called.reference = {Action.name, "called", "I"};
  Called.access = {"public", "static"};
  Action.fields.push_back(Called);
  Method Run;
  Run.reference = {Action.name, "run", {}, "V"};
  Run.access = {"public"};
  Run.registers = 2;
  Instruction Value;
  Value.pc = 0;
  Value.opcode = "const/4";
  Value.registers = {0};
  Value.literal = int64_t(1);
  Instruction Store;
  Store.pc = 1;
  Store.opcode = "sput";
  Store.registers = {0};
  Store.reference = Called.reference;
  Instruction Return;
  Return.pc = 3;
  Return.opcode = "return-void";
  Run.instructions = {Value, Store, Return};
  Run.code_end = 4;
  Action.methods.push_back(Run);
  Method Private;
  Private.reference = {Action.name,
                       "privateNext",
                       {"Landroid/os/MessageQueue;"},
                       "Landroid/os/Message;"};
  Private.access = {"public", "static"};
  Private.registers = 2;
  Instruction Invoke;
  Invoke.pc = 0;
  Invoke.opcode = "invoke-virtual";
  Invoke.registers = {1};
  Invoke.reference = MethodRef{
      "Landroid/os/MessageQueue;", "next", {}, "Landroid/os/Message;"};
  Instruction Move;
  Move.pc = 3;
  Move.opcode = "move-result-object";
  Move.registers = {0};
  Return.pc = 4;
  Return.opcode = "return-object";
  Return.registers = {0};
  Private.instructions = {Invoke, Move, Return};
  Private.code_end = 5;
  Action.methods.push_back(Private);
  return Action;
}

const LibraryMethod &queueOperation(LibraryOperation Op) {
  for (const auto &Method : libraryMethods())
    if (Method.Op == Op)
      return Method;
  std::abort();
}

class AndroidMessageQueueTest : public testing::Test {
protected:
  Program Declarations{{queueAction()}, std::map<std::string, std::string>{}};
  std::shared_ptr<Heap> Objects;
  std::unique_ptr<Runtime> State;
  uint64_t Now = 100, Looper = 0, Queue = 0, Handler = 0;
  unsigned ClockReads = 0;
  ExecutionStop ClockStop = ExecutionStop::Returned;

  uint64_t call(LibraryOperation Op, uint64_t Receiver = 0,
                std::initializer_list<uint64_t> Arguments = {}) {
    auto Result =
        State->library(queueOperation(Op), Receiver, Arguments, AdmitMessage);
    EXPECT_EQ(Result.stop, ExecutionStop::Returned) << Result.diagnostic;
    return Result.value;
  }

  void start(std::optional<bool> NativeEmpty = true, bool QueueKnown = true,
             bool ClockKnown = true) {
    Objects = std::make_shared<Heap>();
    AndroidApplicationSnapshot Input;
    Input.main_looper_prepared = true;
    Input.initial_message_pool_empty = true;
    if (QueueKnown)
      Input.initial_main_queue
          .emplace(AndroidApplicationSnapshot::EmptyMessageQueue{})
          .native_callbacks_empty = NativeEmpty;
    RuntimeServices Services;
    if (ClockKnown)
      Services.uptime_milliseconds = [&] {
        ++ClockReads;
        return ExecutionResult{ClockStop, Now};
      };
    State = std::make_unique<Runtime>(Declarations, Objects, Input, Services);
    Looper = call(MainLooper);
    Queue = call(LooperQueue, Looper);
    const auto *Construct = libraryMethod(
        {"Landroid/os/Handler;", "<init>", {"Landroid/os/Looper;"}, "V"},
        false);
    auto Result = State->library(*Construct, 0, std::array<uint64_t, 1>{Looper},
                                 AdmitMessage);
    ASSERT_EQ(Result.stop, ExecutionStop::Returned) << Result.diagnostic;
    Handler = Result.value;
  }

  AndroidMessageQueueState queue() const {
    return *State->snapshot().framework.main_message_queue;
  }

  const Field *field(const char *Name, const char *Type) const {
    return Declarations.findField({"Landroid/os/Message;", Name, Type}, false);
  }

  uint64_t post(uint64_t Callback = 0) {
    std::set<uint64_t> Before;
    for (const auto &Message : queue().messages)
      Before.insert(Message.identity);
    EXPECT_EQ(call(HandlerPost, Handler, {Callback}), 1u);
    for (const auto &Message : queue().messages)
      if (!Before.contains(Message.identity))
        return Message.identity;
    ADD_FAILURE() << "post did not retain a new message";
    return 0;
  }

  ExecutionResult next(const AdmitInstruction &Admit = AdmitMessage) {
    return State->library(queueOperation(MessageQueueNext), Queue, {}, Admit);
  }
};

TEST_F(AndroidMessageQueueTest, DueRemovalPreservesCallbackAndEveryOtherField) {
  start();
  auto Callback =
      State->allocate("Lqueue/Action;", true, AdmitMessage, 65536).value;
  const auto First = post(Callback), Second = post(), Third = post();
  auto Original = Objects->get(First)->Fields;
  Original[field("what", "I")] = 73;
  Original[field("arg1", "I")] = UINT32_MAX;
  Objects->get(First)->Fields = Original;
  auto Result = next();
  ASSERT_EQ(Result.stop, ExecutionStop::Returned) << Result.diagnostic;
  EXPECT_EQ(Result.value, First);
  Original[field("next", "Landroid/os/Message;")] = 0;
  EXPECT_EQ(Objects->get(First)->Fields, Original);
  EXPECT_EQ(call(MessageTarget, First), Handler);
  EXPECT_EQ(call(MessageCallback, First), Callback);
  ASSERT_EQ(queue().messages.size(), 2u);
  EXPECT_EQ(queue().messages[0].identity, Second);
  EXPECT_EQ(queue().messages[1].identity, Third);
  EXPECT_FALSE(queue().blocked);
  const auto *Called =
      Declarations.findStaticField({"Lqueue/Action;", "called", "I"});
  EXPECT_EQ(State->field(*Called, 0, {}, AdmitMessage, 65536).value, 0u);
  const auto *Run =
      Declarations.findMethod({"Lqueue/Action;", "run", {}, "V"}, false);
  auto Executed = State->execute(*Run, {&Callback, 1}, AdmitMessage, 65536);
  ASSERT_EQ(Executed.stop, ExecutionStop::Returned) << Executed.diagnostic;
  EXPECT_EQ(State->field(*Called, 0, {}, AdmitMessage, 65536).value, 1u);
  EXPECT_EQ(next().value, Second);
  EXPECT_EQ(next().value, Third);
  EXPECT_TRUE(queue().messages.empty());
  EXPECT_EQ(next().stop, ExecutionStop::Unsupported);
}

TEST_F(AndroidMessageQueueTest,
       BarrierSelectsFirstAsyncAndPreservesOtherLinks) {
  start();
  const auto Barrier = post(), Synchronous = post(), Async = post(),
             Tail = post();
  Objects->get(Barrier)->Fields.at(field("target", "Landroid/os/Handler;")) = 0;
  Objects->get(Async)->Fields.at(field("flags", "I")) = 2;
  auto Result = next();
  ASSERT_EQ(Result.stop, ExecutionStop::Returned) << Result.diagnostic;
  EXPECT_EQ(Result.value, Async);
  EXPECT_EQ(Objects->get(Async)->Fields.at(field("flags", "I")), 3u);
  EXPECT_EQ(
      Objects->get(Async)->Fields.at(field("next", "Landroid/os/Message;")),
      0u);
  const auto After = queue();
  ASSERT_EQ(After.messages.size(), 3u);
  EXPECT_EQ(After.messages[0].identity, Barrier);
  EXPECT_EQ(After.messages[0].next, Synchronous);
  EXPECT_EQ(After.messages[1].identity, Synchronous);
  EXPECT_EQ(After.messages[1].next, Tail);
  EXPECT_EQ(After.messages[2].identity, Tail);
  EXPECT_EQ(next().stop, ExecutionStop::Unsupported);
  EXPECT_EQ(queue(), After);
}

TEST_F(AndroidMessageQueueTest, FutureAsyncDoesNotSkipToLaterDueMessage) {
  start();
  const auto Barrier = post(), Future = post(), Later = post();
  Objects->get(Barrier)->Fields.at(field("target", "Landroid/os/Handler;")) = 0;
  Objects->get(Future)->Fields.at(field("flags", "I")) = 3;
  Objects->get(Future)->Fields.at(field("when", "J")) = 101;
  Objects->get(Later)->Fields.at(field("flags", "I")) = 3;
  const auto Before = queue();
  EXPECT_EQ(next().stop, ExecutionStop::Unsupported);
  EXPECT_EQ(queue(), Before);
}

TEST_F(AndroidMessageQueueTest,
       UnknownNativeCallbacksDoNotInventPollCompletion) {
  for (const auto Empty : {std::optional<bool>{}, std::optional<bool>{false}}) {
    start(Empty);
    const auto Message = post();
    const auto Before = queue();
    const auto Fields = Objects->get(Message)->Fields;
    const auto Reads = ClockReads;
    EXPECT_EQ(next().stop, ExecutionStop::Unsupported);
    EXPECT_EQ(ClockReads, Reads);
    EXPECT_EQ(queue(), Before);
    EXPECT_EQ(Objects->get(Message)->Fields, Fields);
  }
}

TEST_F(AndroidMessageQueueTest, UnknownQueueIsNotAnEmptyQueue) {
  start(true, false);
  EXPECT_FALSE(State->snapshot().framework.main_message_queue);
  EXPECT_EQ(next().stop, ExecutionStop::Unsupported);
  EXPECT_FALSE(State->snapshot().framework.main_message_queue);
}

TEST_F(AndroidMessageQueueTest, DelayedAndEmptyWaitsNeverReturnNull) {
  for (const bool Empty : {false, true}) {
    start();
    if (!Empty) {
      Now = 101;
      post();
      Now = 100;
    }
    const auto Before = queue();
    EXPECT_EQ(next().stop, ExecutionStop::Unsupported);
    EXPECT_EQ(queue(), Before);
  }
}

TEST_F(AndroidMessageQueueTest, ClockErrorsAndOverflowKeepTheRetainedQueue) {
  for (const auto Stop :
       {ExecutionStop::Timeout, ExecutionStop::InstructionLimit,
        ExecutionStop::HeapLimit, ExecutionStop::Returned}) {
    ClockStop = ExecutionStop::Returned;
    Now = 100;
    start();
    const auto Message = post();
    const auto Before = queue();
    const auto Fields = Objects->get(Message)->Fields;
    ClockStop = Stop;
    if (Stop == ExecutionStop::Returned)
      Now = uint64_t(1) << 63;
    EXPECT_EQ(next().stop, Stop == ExecutionStop::Returned
                               ? ExecutionStop::Unsupported
                               : Stop);
    EXPECT_EQ(queue(), Before);
    EXPECT_EQ(Objects->get(Message)->Fields, Fields);
  }
}

TEST_F(AndroidMessageQueueTest, MissingClockAndForeignQueueRemainUnknown) {
  start(true, true, false);
  EXPECT_EQ(next().stop, ExecutionStop::Unsupported);
  start();
  post();
  const auto Before = queue();
  const auto Foreign =
      State->allocate("Landroid/os/MessageQueue;", true, AdmitMessage, 65536);
  ASSERT_EQ(Foreign.stop, ExecutionStop::Returned);
  auto Result = State->library(queueOperation(MessageQueueNext), Foreign.value,
                               {}, AdmitMessage);
  EXPECT_EQ(Result.stop, ExecutionStop::Unsupported);
  EXPECT_EQ(queue(), Before);
}

TEST_F(AndroidMessageQueueTest, MalformedLinksAndFieldsDoNotPartiallyUnlink) {
  for (unsigned Mode = 0; Mode != 6; ++Mode) {
    start();
    const auto First = post(), Second = post();
    auto Item = Objects->get(Second);
    switch (Mode) {
    case 0:
      Item->Fields.at(field("next", "Landroid/os/Message;")) = First;
      break;
    case 1:
      Item->Fields.at(field("next", "Landroid/os/Message;")) = UINT64_MAX;
      break;
    case 2:
      Item->Initialized = false;
      break;
    case 3:
      Item->Fields.erase(field("when", "J"));
      break;
    case 4:
      Item->Fields.at(field("target", "Landroid/os/Handler;")) = Looper;
      break;
    case 5:
      Item->Fields.at(field("flags", "I")) = uint64_t(1) << 32;
      break;
    }
    const auto Before = queue();
    const auto FirstFields = Objects->get(First)->Fields;
    const auto SecondFields = Item->Fields;
    EXPECT_EQ(next().stop,
              Mode == 3 ? ExecutionStop::Unsupported : ExecutionStop::Invalid)
        << Mode;
    EXPECT_EQ(queue(), Before) << Mode;
    EXPECT_EQ(Objects->get(First)->Fields, FirstFields) << Mode;
    EXPECT_EQ(Item->Fields, SecondFields) << Mode;
  }
}

TEST_F(AndroidMessageQueueTest, WorkStopsBeforeCommitKeepAllFieldsAndLinks) {
  unsigned Stops = 0, Returns = 0;
  for (unsigned Limit = 1; Limit != 12; ++Limit) {
    start();
    const auto First = post(), Second = post();
    const auto Before = queue();
    const auto FirstFields = Objects->get(First)->Fields;
    const auto SecondFields = Objects->get(Second)->Fields;
    unsigned Ticks = 0;
    auto Result = next([&] {
      return ++Ticks < Limit ? ExecutionStop::Returned
                             : ExecutionStop::InstructionLimit;
    });
    if (Result.stop == ExecutionStop::Returned) {
      ++Returns;
      EXPECT_EQ(Result.value, First);
      ASSERT_EQ(queue().messages.size(), 1u);
      EXPECT_EQ(queue().messages[0].identity, Second);
    } else {
      ++Stops;
      EXPECT_EQ(Result.stop, ExecutionStop::InstructionLimit);
      EXPECT_EQ(queue(), Before);
      EXPECT_EQ(Objects->get(First)->Fields, FirstFields);
      EXPECT_EQ(Objects->get(Second)->Fields, SecondFields);
    }
  }
  EXPECT_GT(Stops, 0u);
  EXPECT_GT(Returns, 0u);
}

TEST_F(AndroidMessageQueueTest, RemovingAtCapacityAllowsOneNewMessage) {
  start();
  for (unsigned I = 0; I != 1024; ++I)
    post();
  const auto First = queue().messages.front().identity;
  ASSERT_EQ(next().value, First);
  EXPECT_EQ(queue().messages.size(), 1023u);
  EXPECT_NE(post(), First);
  EXPECT_EQ(queue().messages.size(), 1024u);
  EXPECT_TRUE(queue().complete);
}

TEST_F(AndroidMessageQueueTest, OrdinaryReferenceGettersPreserveKnownNull) {
  start();
  const auto OtherLooper =
      State->allocate("Landroid/os/Looper;", true, AdmitMessage, 65536).value;
  const auto Message =
      State->allocate("Landroid/os/Message;", true, AdmitMessage, 65536).value;
  EXPECT_EQ(call(LooperQueue, OtherLooper), 0u);
  EXPECT_EQ(call(MessageCallback, Message), 0u);
  EXPECT_EQ(call(MessageTarget, Message), 0u);
  EXPECT_EQ(call(LooperQueue, Looper), Queue);
}

TEST_F(AndroidMessageQueueTest,
       NonpublicNextIsNotAnOrdinaryApplicationDexCall) {
  start();
  post();
  const auto Before = queue();
  const auto *Method = Declarations.findStatic({"Lqueue/Action;",
                                                "privateNext",
                                                {"Landroid/os/MessageQueue;"},
                                                "Landroid/os/Message;"});
  ASSERT_NE(Method, nullptr);
  auto Result = State->execute(*Method, {&Queue, 1}, AdmitMessage, 65536);
  EXPECT_EQ(Result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(Result.diagnostic.find("nonpublic library method access"),
            std::string::npos);
  EXPECT_EQ(queue(), Before);
  EXPECT_FALSE(queueOperation(MessageQueueNext).Public);
  EXPECT_TRUE(queueOperation(LooperQueue).Public);
}

} // namespace
} // namespace neverd::mobile::dalvik
