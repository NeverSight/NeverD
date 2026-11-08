//===- AndroidBinderDispatchTests.cpp - Local and incoming Java Binder ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordered Parcel cursors, virtual Java bodies and retained native upcalls.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <map>

namespace neverd::mobile::dalvik {
namespace {
constexpr const char *Binder = "Landroid/os/Binder;";
constexpr const char *Parcel = "Landroid/os/Parcel;";
constexpr const char *Stub = "Ldispatch/Stub;";
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
const MethodRef OnTransact{
    Binder, "onTransact", {"I", Parcel, Parcel, "I"}, "Z"};

Instruction op(unsigned pc, const char *opcode, std::vector<unsigned> regs) {
  Instruction value;
  value.pc = pc;
  value.opcode = opcode;
  value.registers = std::move(regs);
  return value;
}
Class stub(bool native = false, bool throwing = false) {
  Class type;
  type.name = Stub;
  type.superclass = Binder;
  type.access = {"public"};
  Method method;
  method.reference = OnTransact;
  method.reference.owner = Stub;
  method.access = {"protected"};
  if (native)
    method.access.insert("native");
  else {
    method.registers = 7;
    auto read = op(0, "invoke-virtual", {4});
    read.reference = MethodRef{Parcel, "readInt", {}, "I"};
    auto write = op(6, "invoke-virtual", {5, 0});
    write.reference = MethodRef{Parcel, "writeInt", {"I"}, "V"};
    auto value = op(9, "const/4", {0});
    value.literal = throwing ? 0 : 1;
    method.instructions = {read,
                           op(3, "move-result", {0}),
                           op(4, "add-int", {0, 0, 3}),
                           op(5, "add-int", {0, 0, 6}),
                           write,
                           value,
                           op(10, throwing ? "throw" : "return", {0})};
  }
  type.methods.push_back(std::move(method));
  return type;
}
struct State {
  Program program;
  std::shared_ptr<Heap> heap = std::make_shared<Heap>();
  Runtime runtime;
  uint64_t binder, data, reply;
  explicit State(Class type = stub(), RuntimeServices services = {})
      : program({std::move(type)}, std::map<std::string, std::string>{}),
        runtime(program, heap, {}, std::move(services)) {
    binder = runtime.allocate(Stub, true, Admit, 65536).value;
    data = call({Parcel, "obtain", {}, Parcel}, 0, {}, true).value;
    reply = call({Parcel, "obtain", {}, Parcel}, 0, {}, true).value;
    EXPECT_NE(binder, 0u);
    EXPECT_NE(data, 0u);
    EXPECT_NE(reply, 0u);
  }
  ExecutionResult call(const MethodRef &reference, uint64_t receiver,
                       std::initializer_list<uint64_t> args = {},
                       bool is_static = false, uint64_t owner = 11) {
    auto method = libraryMethod(reference, is_static);
    EXPECT_NE(method, nullptr);
    return runtime.library(*method, receiver, args, Admit, false, owner, 65536);
  }
  ExecutionResult transact(uint64_t source, uint64_t destination,
                           uint64_t owner = 11) {
    auto ref = OnTransact;
    ref.name = "transact";
    return call(ref, binder, {3, source, destination, 0xffffffff}, false,
                owner);
  }
  void write(uint64_t object, uint32_t value) {
    EXPECT_EQ(call({Parcel, "writeInt", {"I"}, "V"}, object, {value}).stop,
              ExecutionStop::Returned);
  }
  uint64_t position(uint64_t object) {
    return call({Parcel, "dataPosition", {}, "I"}, object).value;
  }
};

TEST(AndroidBinderDispatchTest, LocalJavaBodyRewindsInputAndOnlyNormalReply) {
  for (bool alias : {false, true}) {
    State s;
    s.write(s.data, 40);
    s.write(s.reply, 90);
    auto result = s.transact(s.data, alias ? s.data : s.reply, 0);
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    EXPECT_EQ(result.value, 1u);
    EXPECT_EQ(s.position(s.data), alias ? 0u : 4u);
    EXPECT_EQ(s.position(alias ? s.data : s.reply), 0u);
    EXPECT_EQ(s.heap->get(alias ? s.data : s.reply)->Bytes,
              (std::vector<uint8_t>{alias ? uint8_t(40) : uint8_t(90), 0, 0, 0,
                                    42, 0, 0, 0}));
  }
  State throwing(stub(false, true));
  throwing.write(throwing.data, 40);
  auto result = throwing.transact(throwing.data, throwing.reply);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/NullPointerException");
  EXPECT_EQ(throwing.position(throwing.data), 4u);
  EXPECT_EQ(throwing.position(throwing.reply), 4u);
  EXPECT_EQ(throwing.heap->get(throwing.reply)->Bytes,
            (std::vector<uint8_t>{42, 0, 0, 0}));
}

TEST(AndroidBinderDispatchTest, LocalCallsKeepNullsAndEntryFailureOrder) {
  State s;
  s.write(s.data, 12);
  auto result = s.transact(0, s.reply);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(result.exception, "java/lang/NullPointerException");
  EXPECT_EQ(s.position(s.reply), 0u);
  result = s.transact(s.data, 0);
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
  EXPECT_EQ(s.position(s.data), 4u);
  EXPECT_EQ(s.call({Parcel, "recycle", {}, "V"}, s.data).stop,
            ExecutionStop::Returned);
  result = s.transact(s.data, s.reply);
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("after recycle"), std::string::npos);
}

TEST(AndroidBinderDispatchTest, NativeOverrideResumesBeforeReplyRewind) {
  State s(stub(true));
  s.write(s.data, 17);
  s.write(s.reply, 28);
  auto pending = s.transact(s.data, s.reply);
  ASSERT_EQ(pending.stop, ExecutionStop::Suspended) << pending.diagnostic;
  ASSERT_TRUE(pending.external);
  EXPECT_EQ(pending.external->kind, ExternalCall::Native);
  EXPECT_EQ(pending.external->method->reference.owner, Stub);
  EXPECT_EQ(pending.external->arguments,
            (std::vector<uint64_t>{s.binder, 3, s.data, s.reply, 0xffffffff}));
  EXPECT_EQ(s.position(s.data), 0u);
  EXPECT_EQ(s.position(s.reply), 4u);
  s.write(s.reply, 29);
  auto result =
      s.runtime.resume(pending.continuation, 11, {ExecutionStop::Returned, 1});
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 1u);
  EXPECT_EQ(s.position(s.reply), 0u);
  EXPECT_EQ(s.runtime.resume(pending.continuation, 11, {}).stop,
            ExecutionStop::Invalid);
}

TEST(AndroidBinderDispatchTest,
     BaseDispatchUsesVirtualDescriptorAndSharedFields) {
  auto type = stub();
  type.methods.clear();
  State s(type);
  auto result = s.transact(0, 0);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 0u);
  auto descriptor = s.heap->literal("example")->Identity;
  ASSERT_EQ(s.call({Binder,
                    "attachInterface",
                    {"Landroid/os/IInterface;", "Ljava/lang/String;"},
                    "V"},
                   s.binder, {0, descriptor})
                .stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.call({Binder, "getInterfaceDescriptor", {}, "Ljava/lang/String;"},
                   s.binder)
                .value,
            descriptor);
  auto ref = OnTransact;
  ref.name = "transact";
  result = s.call(ref, s.binder, {0x5f4e5446, 0, s.reply, 0});
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 1u);
  EXPECT_EQ(s.position(s.reply), 0u);
  EXPECT_EQ(s.heap->get(s.reply)->Bytes,
            (std::vector<uint8_t>{7,   0, 0,   0, 'e', 0, 'x', 0, 'a', 0,
                                  'm', 0, 'p', 0, 'l', 0, 'e', 0, 0,   0}));
  result = s.call(ref, s.binder, {0x5f4e5446, 0, 0, 0});
  EXPECT_EQ(result.stop, ExecutionStop::Thrown);
}
RuntimeServices incomingServices(bool clock = true) {
  RuntimeServices services;
  services.binder_identity =
      [next = uint64_t(0x4000)](BinderIdentityOperation operation,
                                uint64_t) mutable {
        if (operation == BinderIdentityOperation::Allocate) {
          const auto value = next;
          next += 0x1000;
          return ExecutionResult{ExecutionStop::Returned, value};
        }
        return ExecutionResult{};
      };
  services.binder_thread_policy = [] { return ExecutionResult{}; };
  if (clock)
    services.realtime_milliseconds = [] {
      return ExecutionResult{ExecutionStop::Returned, 123000};
    };
  return services;
}
TEST(AndroidBinderDispatchTest,
     ProtectedSuperCallUsesVirtualDescriptorOverride) {
  auto type = stub();
  auto &method = type.methods.front();
  method.registers = 5;
  auto super = op(0, "invoke-super", {0, 1, 2, 3, 4});
  super.reference = OnTransact;
  method.instructions = {super, op(3, "move-result", {0}),
                         op(4, "return", {0})};
  Method descriptor;
  descriptor.reference = {
      Stub, "getInterfaceDescriptor", {}, "Ljava/lang/String;"};
  descriptor.access = {"public", "native"};
  type.methods.push_back(std::move(descriptor));
  State s(std::move(type));
  auto ref = OnTransact;
  ref.name = "transact";
  auto result = s.call(ref, s.binder, {0x5f4e5446, s.data, s.reply, 0});
  ASSERT_EQ(result.stop, ExecutionStop::Suspended) << result.diagnostic;
  ASSERT_EQ(result.external->kind, ExternalCall::Native);
  EXPECT_EQ(result.external->method->reference.name, "getInterfaceDescriptor");
  auto text = s.heap->literal("iface");
  result = s.runtime.resume(result.continuation, 11,
                            {ExecutionStop::Returned, text->Identity});
  EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 1u);
  EXPECT_EQ(s.position(s.reply), 0u);
  auto read = s.call({Parcel, "readString", {}, "Ljava/lang/String;"}, s.reply);
  ASSERT_EQ(read.stop, ExecutionStop::Returned);
  ASSERT_TRUE(s.heap->get(read.value));
  EXPECT_EQ(s.heap->get(read.value)->Bytes, text->Bytes);
}
BinderIncoming exportBinder(State &s) {
  s.heap->get(s.binder)->Initialized = false;
  auto constructed = s.call({Binder, "<init>", {}, "V"}, s.binder);
  EXPECT_EQ(constructed.stop, ExecutionStop::Returned)
      << constructed.diagnostic;
  s.heap->get(s.binder)->Initialized = true;
  auto written =
      s.call({Parcel, "writeStrongBinder", {"Landroid/os/IBinder;"}, "V"},
             s.data, {s.binder});
  EXPECT_EQ(written.stop, ExecutionStop::Returned) << written.diagnostic;
  BinderIncoming incoming;
  const auto &bytes = s.heap->get(s.data)->Bytes;
  if (bytes.size() == 24) {
    incoming.pointer = llvm::support::endian::read64le(bytes.data() + 8);
    incoming.cookie = llvm::support::endian::read64le(bytes.data() + 16);
  }
  incoming.code = 7;
  incoming.flags = 16;
  incoming.uid = 10001;
  incoming.data.bytes = std::vector<uint8_t>{19, 0, 0, 0};
  incoming.data.fd = 3;
  return incoming;
}

TEST(AndroidBinderDispatchTest, IncomingJavaBodyRetainsBufferUntilReplyAck) {
  State s(stub(), incomingServices());
  auto incoming = exportBinder(s);
  unsigned releases = 0;
  incoming.data.release = [&] { ++releases; };
  auto result = s.runtime.dispatchBinder(std::move(incoming), Admit, 65536, 11);
  ASSERT_EQ(result.stop, ExecutionStop::Suspended) << result.diagnostic;
  ASSERT_TRUE(result.external);
  EXPECT_EQ(result.external->kind, ExternalCall::BinderSendReply);
  ASSERT_TRUE(result.external->response);
  EXPECT_EQ(result.external->response->status, 0);
  EXPECT_EQ(result.external->response->bytes,
            (std::vector<uint8_t>{42, 0, 0, 0}));
  EXPECT_EQ(releases, 0u);
  EXPECT_EQ(s.runtime.resume(result.continuation, 11, {}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(releases, 1u);
  auto calls = s.runtime.snapshot().framework.binder_calls;
  ASSERT_TRUE(calls);
  EXPECT_EQ(calls->start_milliseconds, 123000u);
  ASSERT_EQ(calls->entries.size(), 1u);
  EXPECT_EQ(calls->entries[0].uid, 10001u);
  EXPECT_EQ(calls->entries[0].count, 1u);
  EXPECT_EQ(s.runtime.resume(result.continuation, 11, {}).stop,
            ExecutionStop::Invalid);
}

TEST(AndroidBinderDispatchTest, IncomingNativeBodyRecyclesSeparateWrapperPool) {
  State s(stub(true), incomingServices());
  auto original = exportBinder(s);
  uint64_t last_data = 0, last_reply = 0;
  for (unsigned i = 0; i != 3; ++i) {
    unsigned releases = 0;
    auto incoming = original;
    incoming.data.release = [&] { ++releases; };
    auto result =
        s.runtime.dispatchBinder(std::move(incoming), Admit, 65536, 11);
    ASSERT_EQ(result.stop, ExecutionStop::Suspended) << result.diagnostic;
    ASSERT_EQ(result.external->kind, ExternalCall::Native);
    const auto data = result.external->arguments[2],
               reply = result.external->arguments[3];
    if (i) {
      EXPECT_EQ(data, last_reply);
      EXPECT_EQ(reply, last_data);
    }
    EXPECT_NE(data, s.data);
    EXPECT_NE(reply, s.reply);
    EXPECT_EQ(s.call({Parcel, "readInt", {}, "I"}, data).value, 19u);
    s.write(reply, 42 + i);
    result =
        s.runtime.resume(result.continuation, 11, {ExecutionStop::Returned, 1});
    ASSERT_EQ(result.stop, ExecutionStop::Suspended) << result.diagnostic;
    EXPECT_EQ(result.external->response->bytes.front(), 42 + i);
    EXPECT_EQ(releases, 0u);
    EXPECT_EQ(s.runtime.resume(result.continuation, 11, {}).stop,
              ExecutionStop::Returned);
    EXPECT_EQ(releases, 1u);
    last_data = data;
    last_reply = reply;
  }
  EXPECT_EQ(s.call({Parcel, "readInt", {}, "I"}, last_data).stop,
            ExecutionStop::Unsupported);
}

TEST(AndroidBinderDispatchTest, IncomingPingAndFalseUseNativeStatusReplies) {
  auto base = stub();
  base.methods.clear();
  State s(std::move(base), incomingServices(false));
  auto incoming = exportBinder(s);
  incoming.code = 0x5f504e47;
  auto ping = s.runtime.dispatchBinder(incoming, Admit, 65536, 11);
  ASSERT_EQ(ping.stop, ExecutionStop::Suspended) << ping.diagnostic;
  EXPECT_EQ(ping.external->response->bytes, std::vector<uint8_t>(4));
  EXPECT_EQ(s.runtime.resume(ping.continuation, 11, {}).stop,
            ExecutionStop::Returned);
  incoming.code = 7;
  auto missing = s.runtime.dispatchBinder(incoming, Admit, 65536, 11);
  EXPECT_EQ(missing.stop, ExecutionStop::Unsupported);
  EXPECT_NE(missing.diagnostic.find("realtime clock"), std::string::npos);

  base = stub();
  base.methods.clear();
  State normal(std::move(base), incomingServices());
  auto unknown =
      normal.runtime.dispatchBinder(exportBinder(normal), Admit, 65536, 11);
  ASSERT_EQ(unknown.stop, ExecutionStop::Suspended) << unknown.diagnostic;
  EXPECT_EQ(unknown.external->response->status, -74);
}

TEST(AndroidBinderDispatchTest,
     IncomingStopsKeepClockExceptionAndStackBoundaries) {
  State throwing(stub(false, true), incomingServices());
  auto error =
      throwing.runtime.dispatchBinder(exportBinder(throwing), Admit, 65536, 11);
  ASSERT_EQ(error.stop, ExecutionStop::Suspended) << error.diagnostic;
  ASSERT_EQ(error.external->kind, ExternalCall::BinderSendReply);
  EXPECT_EQ(error.external->response->status, 0);
  EXPECT_EQ(error.external->response->bytes,
            (std::vector<uint8_t>{252, 255, 255, 255, 255, 255, 255, 255, 0, 0,
                                  0, 0}));
  auto calls = throwing.runtime.snapshot().framework.binder_calls;
  ASSERT_TRUE(calls);
  ASSERT_EQ(calls->entries.size(), 1u);
  EXPECT_EQ(calls->entries[0].count, 1u);
  EXPECT_EQ(throwing.runtime.resume(error.continuation, 11, {}).stop,
            ExecutionStop::Returned);
  State limited(stub(), incomingServices());
  auto exhausted =
      limited.runtime.dispatchBinder(exportBinder(limited), Admit, 0, 11);
  EXPECT_EQ(exhausted.stop, ExecutionStop::StackLimit);
  State work(stub(), incomingServices());
  auto request = exportBinder(work);
  exhausted = work.runtime.dispatchBinder(
      std::move(request), [] { return ExecutionStop::InstructionLimit; }, 65536,
      11);
  EXPECT_EQ(exhausted.stop, ExecutionStop::InstructionLimit);
}
TEST(AndroidParcelExceptionTest, LiteralHeadersKeepTailAndNullableMessages) {
  const char *types[] = {"java/lang/SecurityException",
                         "android/os/BadParcelableException",
                         "java/lang/IllegalArgumentException",
                         "java/lang/NullPointerException",
                         "java/lang/IllegalStateException",
                         "android/os/NetworkOnMainThreadException",
                         "java/lang/UnsupportedOperationException"};
  for (unsigned i = 0; i != 7; ++i)
    for (bool message : {false, true}) {
      SCOPED_TRACE(types[i]);
      SCOPED_TRACE(message);
      State s;
      for (unsigned n = 0; n != 8; ++n)
        s.write(s.reply, 0x11223344);
      ASSERT_EQ(
          s.call({Parcel, "setDataPosition", {"I"}, "V"}, s.reply, {4}).stop,
          ExecutionStop::Returned);
      auto error = s.heap->allocate(Object::Throwable, types[i], 0);
      if (message) {
        auto text = s.heap->allocate(Object::String, "java/lang/String", 2);
        ASSERT_TRUE(s.heap->replaceBytes(*text, {0xe9, 0}));
        error->Message = text->Identity;
      }
      auto result =
          s.call({Parcel, "writeException", {"Ljava/lang/Exception;"}, "V"},
                 s.reply, {error->Identity}, false, 0);
      ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
      std::vector<uint8_t> expected;
      for (unsigned n = 0; n != 8; ++n)
        expected.insert(expected.end(), {0x44, 0x33, 0x22, 0x11});
      expected[4] = uint8_t(255 - i);
      expected[5] = expected[6] = expected[7] = 255;
      const std::vector<uint8_t> payload =
          message ? std::vector<uint8_t>{1, 0, 0, 0, 0xe9, 0, 0, 0, 0, 0, 0, 0}
                  : std::vector<uint8_t>{255, 255, 255, 255, 0, 0, 0, 0};
      std::copy(payload.begin(), payload.end(), expected.begin() + 8);
      EXPECT_EQ(s.heap->get(s.reply)->Bytes, expected);
      EXPECT_EQ(s.position(s.reply), message ? 20u : 16u);
      ASSERT_EQ(s.call({Parcel, "writeNoException", {}, "V"}, s.reply).stop,
                ExecutionStop::Returned);
      EXPECT_EQ(s.position(s.reply), message ? 24u : 20u);
      EXPECT_EQ(llvm::support::endian::read32le(
                    s.heap->get(s.reply)->Bytes.data() + (message ? 20 : 16)),
                0u);
    }
}

TEST(AndroidParcelExceptionTest, UnknownRuntimeRethrowsAfterZeroTag) {
  for (const char *type :
       {"java/lang/RuntimeException", "java/lang/ArithmeticException"}) {
    State s;
    auto error = s.heap->allocate(Object::Throwable, type, 0);
    auto result =
        s.call({Parcel, "writeException", {"Ljava/lang/Exception;"}, "V"},
               s.reply, {error->Identity});
    EXPECT_EQ(result.stop, ExecutionStop::Thrown);
    EXPECT_EQ(result.throwable, error->Identity);
    EXPECT_EQ(s.heap->get(s.reply)->Bytes, (std::vector<uint8_t>{0, 0, 0, 0}));
  }
  for (bool null : {false, true}) {
    State s;
    auto error =
        s.heap->allocate(Object::Throwable, "android/os/RemoteException", 0);
    auto result =
        s.call({Parcel, "writeException", {"Ljava/lang/Exception;"}, "V"},
               s.reply, {null ? 0 : error->Identity});
    EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
    EXPECT_NE(result.diagnostic.find("Throwable-cause"), std::string::npos);
    EXPECT_EQ(s.heap->get(s.reply)->Bytes, (std::vector<uint8_t>{0, 0, 0, 0}));
  }
}

TEST(AndroidParcelExceptionTest,
     VirtualMessageSuspendsAfterTagAndKeepsIdentity) {
  for (bool throws : {false, true}) {
    Class type;
    type.name = "Lexception/Custom;";
    type.superclass = "Ljava/lang/IllegalArgumentException;";
    type.access = {"public"};
    Method message;
    message.reference = {type.name, "getMessage", {}, "Ljava/lang/String;"};
    message.access = {"public", "native"};
    type.methods.push_back(std::move(message));
    Program program({std::move(type)}, std::map<std::string, std::string>{});
    auto heap = std::make_shared<Heap>();
    Runtime runtime(program, heap);
    auto parcel =
        runtime
            .library(*libraryMethod({Parcel, "obtain", {}, Parcel}, true), 0,
                     {}, Admit)
            .value;
    auto error = heap->allocate(Object::Throwable, "exception/Custom", 0);
    const uint64_t args[] = {error->Identity};
    auto pending = runtime.library(
        *libraryMethod(
            {Parcel, "writeException", {"Ljava/lang/Exception;"}, "V"}, false),
        parcel, args, Admit, false, 17, 65536);
    ASSERT_EQ(pending.stop, ExecutionStop::Suspended) << pending.diagnostic;
    ASSERT_EQ(pending.external->kind, ExternalCall::Native);
    EXPECT_EQ(pending.external->arguments,
              (std::vector<uint64_t>{error->Identity}));
    EXPECT_EQ(heap->get(parcel)->Bytes,
              (std::vector<uint8_t>{253, 255, 255, 255}));
    auto text = heap->allocate(Object::String, "java/lang/String", 2);
    ASSERT_TRUE(heap->replaceBytes(*text, {'x', 0}));
    auto completed = runtime.resume(
        pending.continuation, 17,
        throws ? ExecutionResult{ExecutionStop::Thrown, 0,
                                 "java/lang/IllegalStateException"}
               : ExecutionResult{ExecutionStop::Returned, text->Identity});
    EXPECT_EQ(completed.stop,
              throws ? ExecutionStop::Thrown : ExecutionStop::Returned);
    if (throws) {
      EXPECT_NE(completed.throwable, 0u);
      EXPECT_EQ(heap->get(parcel)->Bytes.size(), 4u);
    } else
      EXPECT_EQ(heap->get(parcel)->Bytes,
                (std::vector<uint8_t>{253, 255, 255, 255, 1, 0, 0, 0, 'x', 0, 0,
                                      0, 0, 0, 0, 0}));
    EXPECT_EQ(runtime.resume(pending.continuation, 17, {}).stop,
              ExecutionStop::Invalid);
  }
}

TEST(AndroidBinderDispatchTest, UncaughtExceptionsDoNotCompleteTheCall) {
  for (const char *type :
       {"java/lang/ArithmeticException", "java/lang/Error"}) {
    State s(stub(true), incomingServices());
    auto pending = s.runtime.dispatchBinder(exportBinder(s), Admit, 65536, 11);
    ASSERT_EQ(pending.stop, ExecutionStop::Suspended) << pending.diagnostic;
    ASSERT_EQ(pending.external->kind, ExternalCall::Native);
    auto stopped = s.runtime.resume(pending.continuation, 11,
                                    {ExecutionStop::Thrown, 0, type});
    EXPECT_EQ(stopped.stop, ExecutionStop::Unsupported);
    EXPECT_NE(stopped.diagnostic.find("uncaught exception logging"),
              std::string::npos);
    auto calls = s.runtime.snapshot().framework.binder_calls;
    ASSERT_TRUE(calls);
    EXPECT_TRUE(calls->entries.empty());
  }
}

TEST(AndroidBinderDispatchTest, DeferredStrongReleaseChangesIncomingLiveness) {
  State s(stub(), incomingServices());
  auto incoming = exportBinder(s);
  incoming.code = 0x5f504e47;
  auto reference = [&](BinderReferenceOperation operation) {
    return s.runtime.binderReference(operation, incoming.pointer,
                                     incoming.cookie);
  };
  for (auto operation : {BinderReferenceOperation::AcquireWeak,
                         BinderReferenceOperation::AcquireStrong})
    ASSERT_EQ(reference(operation).stop, ExecutionStop::Returned);
  ASSERT_EQ(s.call({Parcel, "recycle", {}, "V"}, s.data).stop,
            ExecutionStop::Returned);
  ASSERT_EQ(reference(BinderReferenceOperation::DeferStrongRelease).stop,
            ExecutionStop::Returned);
  auto ping = s.runtime.dispatchBinder(incoming, Admit, 65536, 11);
  ASSERT_EQ(ping.stop, ExecutionStop::Suspended);
  EXPECT_EQ(ping.external->response->status, 0);
  ASSERT_EQ(s.runtime.resume(ping.continuation, 11, {}).stop,
            ExecutionStop::Returned);
  ASSERT_EQ(reference(BinderReferenceOperation::DeferWeakRelease).stop,
            ExecutionStop::Returned);
  ASSERT_EQ(reference(BinderReferenceOperation::ReleaseDeferredWeak).stop,
            ExecutionStop::Returned);
  ASSERT_EQ(reference(BinderReferenceOperation::ReleaseDeferredStrong).stop,
            ExecutionStop::Returned);
  auto dead = s.runtime.dispatchBinder(incoming, Admit, 65536, 11);
  ASSERT_EQ(dead.stop, ExecutionStop::Suspended);
  EXPECT_EQ(dead.external->response->status, -74);
  ASSERT_EQ(s.runtime.resume(dead.continuation, 11, {}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(reference(BinderReferenceOperation::ReleaseDeferredStrong).stop,
            ExecutionStop::Unsupported);
}
} // namespace
} // namespace neverd::mobile::dalvik
