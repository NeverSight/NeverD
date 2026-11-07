//===- AndroidBinderTests.cpp - Ordered Binder transaction entry ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 wrapper effects and native validation without fabricated transport.
//===----------------------------------------------------------------------===//

#include "../../lib/dalvik/AndroidFramework.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neverd::mobile::dalvik {
namespace {
constexpr auto Binder = "Landroid/os/Binder;";
constexpr auto Proxy = "Landroid/os/BinderProxy;";
constexpr auto Parcel = "Landroid/os/Parcel;";
constexpr auto Transaction = "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z";
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };

AndroidApplicationSnapshot input(std::optional<bool> warning) {
  AndroidApplicationSnapshot value;
  auto &manager = value.service_manager.emplace();
  manager.binder_proxies = {{7, warning}};
  manager.cache.emplace().push_back({"alpha", 7});
  return value;
}
RuntimeServices services() {
  RuntimeServices value;
  value.property_integer = [](std::string_view, int32_t fallback) {
    return ExecutionResult{ExecutionStop::Returned, uint32_t(fallback)};
  };
  value.elapsed_realtime_nanoseconds = [] {
    return ExecutionResult{ExecutionStop::Returned, 1000000000};
  };
  return value;
}
struct State {
  Program program{{}, std::map<std::string, std::string>{}};
  std::shared_ptr<Heap> heap = std::make_shared<Heap>();
  AndroidFramework framework;
  uint64_t proxy = 0;
  explicit State(std::optional<bool> warning = false)
      : framework(input(warning), program, services()) {
    auto result = framework.initializeServiceManager(
        *heap, program, Admit,
        [](std::string_view) { return ExecutionResult{}; });
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    proxy = call("Landroid/os/ServiceManager;", "getService",
                 "(Ljava/lang/String;)Landroid/os/IBinder;", 0,
                 {heap->literal("alpha")->Identity}, true)
                .value;
    EXPECT_NE(proxy, 0u);
  }
  ExecutionResult call(const char *owner, const char *name,
                       const char *signature, uint64_t receiver,
                       std::initializer_list<uint64_t> args = {},
                       bool is_static = false,
                       const AdmitInstruction &admit = Admit) {
    auto [parameters, result] = prototype(signature);
    const auto *entry =
        libraryMethod({owner, name, parameters, result}, is_static);
    EXPECT_NE(entry, nullptr);
    return entry ? executeLibrary(*heap, *entry, receiver, args, admit, false,
                                  &program, &framework)
                 : ExecutionResult{ExecutionStop::Invalid};
  }
  uint64_t obtain() {
    auto result = call(Parcel, "obtain", "()Landroid/os/Parcel;", 0, {}, true);
    EXPECT_EQ(result.stop, ExecutionStop::Returned);
    return result.value;
  }
  ExecutionResult transact(uint64_t data, uint64_t reply = 0,
                           uint32_t flags = 0, bool native = false,
                           const AdmitInstruction &admit = Admit) {
    return call(Proxy, native ? "transactNative" : "transact", Transaction,
                proxy, {0xffffffff, data, reply, flags}, false, admit);
  }
  ExecutionResult trace(const char *name,
                        const AdmitInstruction &admit = Admit) {
    return call(Binder, name,
                std::string_view(name) == "isTracingEnabled" ? "()Z" : "()V", 0,
                {}, true, admit);
  }
  ExecutionResult warning() {
    return framework.field(
        *heap, *program.findField({Proxy, "mWarnOnBlocking", "Z"}, false),
        proxy, {}, Admit);
  }
};
void expectStop(const ExecutionResult &result, std::string_view diagnostic) {
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported) << result.diagnostic;
  EXPECT_NE(result.diagnostic.find(diagnostic), std::string::npos)
      << result.diagnostic;
}
void expectNull(const ExecutionResult &result) {
  EXPECT_EQ(result.stop, ExecutionStop::Thrown) << result.diagnostic;
  EXPECT_EQ(result.exception, "java/lang/NullPointerException");
}

TEST(AndroidBinderTest, DeclarationsMatchTheTransactionAndTracingContracts) {
  State s;
  for (const auto *owner : {Proxy, "Landroid/os/IBinder;"}) {
    auto [args, result] = prototype(Transaction);
    auto method = libraryMethod({owner, "transact", args, result}, false);
    ASSERT_NE(method, nullptr);
    EXPECT_TRUE(method->Public);
    EXPECT_FALSE(method->Final);
    EXPECT_FALSE(libraryMethod({owner, "transact", args, result}, true));
  }
  const auto *field =
      s.program.findField({Binder, "sTracingEnabled", "Z"}, true);
  ASSERT_NE(field, nullptr);
  EXPECT_TRUE(field->access.contains("private"));
  EXPECT_TRUE(field->access.contains("volatile"));
  auto local = s.heap->allocate(Object::Instance, "android/os/Binder", 0);
  expectStop(s.call("Landroid/os/IBinder;", "transact", Transaction,
                    local->Identity, {1, 0, 0, 0}),
             "local Binder onTransact");
}

TEST(AndroidBinderTest, TracingMethodsAndFieldShareProcessStateAndAdmission) {
  State s, independent;
  EXPECT_EQ(s.trace("isTracingEnabled").value, 0u);
  const AdmitInstruction deny = [] { return ExecutionStop::InstructionLimit; };
  EXPECT_EQ(s.trace("enableTracing", deny).stop,
            ExecutionStop::InstructionLimit);
  EXPECT_EQ(s.trace("isTracingEnabled").value, 0u);
  EXPECT_EQ(s.trace("enableTracing").stop, ExecutionStop::Returned);
  EXPECT_EQ(s.trace("isTracingEnabled").value, 1u);
  EXPECT_EQ(independent.trace("isTracingEnabled").value, 0u);
  const auto *field =
      s.program.findField({Binder, "sTracingEnabled", "Z"}, true);
  EXPECT_EQ(s.framework.field(*s.heap, *field, 0, {}, Admit).value, 1u);
  EXPECT_EQ(s.trace("disableTracing").stop, ExecutionStop::Returned);
  EXPECT_EQ(s.framework.field(*s.heap, *field, 0, {}, Admit).value, 0u);
  EXPECT_EQ(s.framework.field(*s.heap, *field, 0, 1, Admit).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.trace("isTracingEnabled").value, 1u);
}

TEST(AndroidBinderTest, WarningClearsBeforeLoggingAndBeforeNativeValidation) {
  State s(true);
  expectStop(s.transact(0), "blocking warning requires Log.w");
  EXPECT_EQ(s.warning().value, 0u);
  expectNull(s.transact(0));
  State denied(true);
  const AdmitInstruction deny = [] { return ExecutionStop::InstructionLimit; };
  EXPECT_EQ(denied.transact(0, 0, 0, false, deny).stop,
            ExecutionStop::InstructionLimit);
  EXPECT_EQ(denied.warning().value, 1u);
  State unknown(std::nullopt);
  expectStop(unknown.transact(0), "warning state is not supplied");
}

TEST(AndroidBinderTest, OneWayAndNativeEntrySkipTheBlockingWarning) {
  for (std::optional<bool> warning : {std::optional<bool>{}, {true}, {false}}) {
    State s(warning);
    for (uint32_t flags : {1u, 0xffffffffu})
      expectNull(s.transact(0, 0, flags));
    expectNull(s.transact(0, 0, 0, true));
    auto state = s.warning();
    if (warning)
      EXPECT_EQ(state.value, *warning);
    else
      EXPECT_EQ(state.stop, ExecutionStop::Unsupported);
  }
}

TEST(AndroidBinderTest, WarningThenTracingPrecedeNativeNullDataChecks) {
  State s(true);
  s.trace("enableTracing");
  expectStop(s.transact(0), "blocking warning requires Log.w");
  expectStop(s.transact(0), "transaction tracing requires");
  expectStop(s.transact(0, 0, 1), "transaction tracing requires");
  expectNull(s.transact(0, 0, 0, true));
  s.trace("disableTracing");
  expectNull(s.transact(0));
}

TEST(AndroidBinderTest, NativeValidationOrdersNullDataOwnedParcelsAndBinding) {
  State s;
  const auto data = s.obtain(), reply = s.obtain();
  const auto raw =
      s.heap->allocate(Object::Instance, "android/os/Parcel", 0)->Identity;
  expectNull(s.transact(0, raw));
  expectStop(s.transact(raw, reply), "owned native state");
  s.call(Parcel, "recycle", "()V", data);
  expectStop(s.transact(data, raw), "after recycle");
  EXPECT_EQ(s.obtain(), data);
  expectStop(s.transact(data, raw), "owned native state");
  s.call(Parcel, "recycle", "()V", reply);
  expectStop(s.transact(data, reply), "after recycle");
  EXPECT_EQ(s.obtain(), reply);
  expectStop(s.transact(data, reply), "explicit native Binder binding");
  expectStop(s.transact(data), "explicit native Binder binding");
  s.proxy =
      s.heap->allocate(Object::Instance, "android/os/BinderProxy", 0)->Identity;
  expectNull(s.transact(0, raw, 0, true));
  expectStop(s.transact(data, reply, 0, true), "native object is not supplied");
}

TEST(AndroidBinderTest, EntryPreservesBytesPositionsAliasesAndSignedArguments) {
  State s;
  const auto data = s.obtain(), reply = s.obtain();
  s.call(Parcel, "writeInt", "(I)V", data, {0x88776655});
  s.call(Parcel, "writeInt", "(I)V", reply, {0x11223344});
  s.call(Parcel, "setDataPosition", "(I)V", data, {1});
  const auto data_bytes = s.heap->get(data)->Bytes;
  const auto reply_bytes = s.heap->get(reply)->Bytes;
  for (auto target : {reply, data, uint64_t(0)})
    expectStop(s.transact(data, target, 0xffffffff),
               "explicit native Binder binding");
  EXPECT_EQ(s.heap->get(data)->Bytes, data_bytes);
  EXPECT_EQ(s.heap->get(reply)->Bytes, reply_bytes);
  EXPECT_EQ(s.call(Parcel, "dataPosition", "()I", data).value, 1u);
  EXPECT_EQ(s.call(Parcel, "dataPosition", "()I", reply).value, 4u);
}

TEST(AndroidBinderTest,
     DexInterfaceDispatchCatchesNullDataButNotMissingTransport) {
  Class caller;
  caller.name = "Ltest/Transaction;";
  caller.superclass = "Ljava/lang/Object;";
  Method method;
  method.reference = {caller.name,
                      "run",
                      {"Landroid/os/IBinder;", "I", Parcel, Parcel, "I"},
                      "Ljava/lang/Throwable;"};
  method.access = {"public", "static"};
  method.registers = 5;
  Instruction invoke;
  invoke.pc = 0;
  invoke.opcode = "invoke-interface";
  invoke.registers = {0, 1, 2, 3, 4};
  invoke.reference = MethodRef{
      "Landroid/os/IBinder;", "transact", {"I", Parcel, Parcel, "I"}, "Z"};
  Instruction zero;
  zero.pc = 3;
  zero.opcode = "const/4";
  zero.registers = {0};
  zero.literal = 0;
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return-object";
  ret.registers = {0};
  Instruction caught;
  caught.pc = 5;
  caught.opcode = "move-exception";
  caught.registers = {0};
  method.instructions = {invoke, zero, ret, caught};
  ret.pc = 6;
  method.instructions.push_back(ret);
  method.tries = {{0, 3, {{"Ljava/lang/NullPointerException;", 5}}}};
  caller.methods.push_back(method);
  Program program({caller}, std::map<std::string, std::string>{});
  auto heap = std::make_shared<Heap>();
  Runtime runtime(program, heap);
  const auto proxy =
      heap->allocate(Object::Instance, "android/os/BinderProxy", 0)->Identity;
  std::array<uint64_t, 5> args{proxy, 0xffffffff, 0, 0, 1};
  const auto *body = program.findStatic(method.reference);
  ASSERT_NE(body, nullptr);
  auto result = runtime.execute(*body, args, Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  const auto exception = heap->get(result.value);
  ASSERT_NE(exception, nullptr);
  EXPECT_EQ(exception->ClassName, "java/lang/NullPointerException");
  EXPECT_EQ(exception->Message, 0u);
  const auto *obtain = libraryMethod({Parcel, "obtain", {}, Parcel}, true);
  args[2] = runtime.library(*obtain, 0, {}, Admit).value;
  expectStop(runtime.execute(*body, args, Admit, 65536),
             "native object is not supplied");
}

struct BoundRuntime {
  Program program{{}, std::map<std::string, std::string>{}};
  std::shared_ptr<Heap> heap = std::make_shared<Heap>();
  Runtime runtime;
  uint64_t proxy = 0, data = 0, reply = 0;
  static AndroidApplicationSnapshot application() {
    auto app = input(false);
    app.service_manager->binder_proxies[0].native_binding =
        AndroidApplicationSnapshot::ServiceManager::BinderProxy::NativeBinding{
            3, 0, true};
    return app;
  }
  explicit BoundRuntime(RuntimeServices inputServices = services())
      : runtime(program, heap, application(), std::move(inputServices)) {
    proxy = call("Landroid/os/ServiceManager;", "getService",
                 "(Ljava/lang/String;)Landroid/os/IBinder;", 0,
                 {heap->literal("alpha")->Identity}, true)
                .value;
    data = call(Parcel, "obtain", "()Landroid/os/Parcel;", 0, {}, true).value;
    reply = call(Parcel, "obtain", "()Landroid/os/Parcel;", 0, {}, true).value;
    EXPECT_NE(proxy, 0u);
    EXPECT_NE(data, 0u);
    EXPECT_NE(reply, 0u);
  }
  ExecutionResult call(const char *owner, const char *name,
                       const char *signature, uint64_t receiver,
                       std::initializer_list<uint64_t> values = {},
                       bool is_static = false) {
    auto [params, result] = prototype(signature);
    const auto *method =
        libraryMethod({owner, name, params, result}, is_static);
    return runtime.library(*method, receiver, values, Admit, false, 11, 65536);
  }
  ExecutionResult transact(uint64_t source = 0,
                           uint64_t destination = UINT64_MAX) {
    return call(Proxy, "transact", Transaction, proxy,
                {19, source ? source : data,
                 destination == UINT64_MAX ? reply : destination, 0});
  }
  ExecutionResult complete(const ExecutionResult &request, BinderReply reply) {
    ExecutionResult result;
    result.binder_reply = std::move(reply);
    return runtime.resume(request.continuation, 11, std::move(result));
  }
};

TEST(AndroidBinderTest,
     NativeBindingAuthenticatesResumeAndRetainsReplyStorage) {
  BoundRuntime s;
  auto request = s.transact();
  ASSERT_EQ(request.stop, ExecutionStop::Suspended) << request.diagnostic;
  ASSERT_TRUE(request.external);
  EXPECT_EQ(request.external->kind, ExternalCall::BinderTransaction);
  ASSERT_TRUE(request.external->binder);
  EXPECT_EQ(*request.external->binder,
            (BinderTransactionRequest{s.proxy, s.data, s.reply, 19, 0, 3, 0}));
  EXPECT_EQ(s.runtime.resume(request.continuation, 12, {}).stop,
            ExecutionStop::Invalid);
  EXPECT_EQ(s.runtime.resume(request.continuation, 11, {}).stop,
            ExecutionStop::Invalid);
  unsigned releases = 0;
  std::vector<uint8_t> arena{0x78, 0x56, 0x34, 0x12};
  BinderReply native;
  native.bytes = arena;
  native.view = [&](std::span<const uint8_t> &view) {
    view = arena;
    return ExecutionResult{};
  };
  native.release = [&] { ++releases; };
  auto result = s.complete(request, std::move(native));
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 1u);
  EXPECT_EQ(releases, 0u);
  EXPECT_EQ(s.call(Parcel, "dataPosition", "()I", s.reply).value, 0u);
  // Reads observe the shared native buffer, not the bridge's old copy.
  arena[0] = 0xab;
  EXPECT_EQ(s.call(Parcel, "readInt", "()I", s.reply).value, 0x123456abu);
  EXPECT_EQ(s.call(Parcel, "recycle", "()V", s.reply).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(releases, 1u);
  EXPECT_EQ(s.runtime.resume(request.continuation, 11, {}).stop,
            ExecutionStop::Invalid);
}

TEST(AndroidBinderTest,
     StatusReplyPreservesBytesCursorAndPropagatesParcelError) {
  BoundRuntime s;
  s.call(Parcel, "writeInt", "(I)V", s.reply, {0x11223344});
  auto request = s.transact();
  auto result = s.complete(request, {-74});
  ASSERT_EQ(result.stop, ExecutionStop::Returned);
  EXPECT_EQ(result.value, 0u);
  EXPECT_EQ(s.call(Parcel, "dataPosition", "()I", s.reply).value, 4u);
  EXPECT_EQ(s.heap->get(s.reply)->Bytes,
            (std::vector<uint8_t>{0x44, 0x33, 0x22, 0x11}));
  result = s.transact(s.reply, s.data);
  EXPECT_EQ(result.stop, ExecutionStop::Returned);
  EXPECT_EQ(result.value, 0u);
  EXPECT_FALSE(result.external);
}

TEST(AndroidBinderTest,
     DeadReplyCachesBpBinderLivenessAndCheckedExceptionHierarchy) {
  BoundRuntime s;
  auto request = s.transact();
  auto result = s.complete(request, {-32});
  ASSERT_EQ(result.stop, ExecutionStop::Thrown) << result.diagnostic;
  EXPECT_EQ(result.exception, "android/os/DeadObjectException");
  EXPECT_TRUE(s.program.assignable("Landroid/os/DeadObjectException;",
                                   "Landroid/os/RemoteException;"));
  EXPECT_FALSE(s.program.assignable("Landroid/os/DeadObjectException;",
                                    "Ljava/lang/RuntimeException;"));
  auto next = s.transact();
  EXPECT_EQ(next.stop, ExecutionStop::Thrown);
  EXPECT_FALSE(next.external);
  auto snapshot = s.runtime.snapshot();
  ASSERT_TRUE(snapshot.framework.service_manager);
  ASSERT_TRUE(
      snapshot.framework.service_manager->binder_proxies[0].native_binding);
  EXPECT_FALSE(snapshot.framework.service_manager->binder_proxies[0]
                   .native_binding->alive);
}

TEST(AndroidBinderTest, NullReplyReleasesTemporaryAndRetiredArenaStopsReads) {
  BoundRuntime s;
  unsigned released = 0;
  BinderReply reply;
  reply.bytes = std::vector<uint8_t>{1, 2, 3, 4};
  reply.release = [&] { ++released; };
  ASSERT_EQ(s.complete(s.transact(0, 0), std::move(reply)).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(released, 1u);
  reply.bytes = std::vector<uint8_t>{1, 2, 3, 4};
  reply.view = [](std::span<const uint8_t> &) {
    return ExecutionResult{
        ExecutionStop::Unsupported,
        0,
        {},
        "Android Parcel Binder buffer mapping is no longer live"};
  };
  reply.release = [&] { ++released; };
  ASSERT_EQ(s.complete(s.transact(), std::move(reply)).stop,
            ExecutionStop::Returned);
  expectStop(s.call(Parcel, "readInt", "()I", s.reply),
             "mapping is no longer live");
  EXPECT_EQ(released, 1u);
}
TEST(AndroidBinderTest, DexCallerCatchesNativeReplyAndCachedDeadObject) {
  Class caller;
  caller.name = "Ltest/BoundTransaction;";
  caller.superclass = "Ljava/lang/Object;";
  Method method;
  method.reference = {caller.name,
                      "run",
                      {"Landroid/os/IBinder;", "I", Parcel, Parcel, "I"},
                      "I"};
  method.access = {"public", "static"};
  method.registers = 5;
  Instruction invoke;
  invoke.pc = 0;
  invoke.opcode = "invoke-interface";
  invoke.registers = {0, 1, 2, 3, 4};
  invoke.reference = MethodRef{
      "Landroid/os/IBinder;", "transact", {"I", Parcel, Parcel, "I"}, "Z"};
  Instruction constant;
  constant.pc = 3;
  constant.opcode = "const/4";
  constant.registers = {0};
  constant.literal = 0;
  Instruction ret;
  ret.pc = 4;
  ret.opcode = "return";
  ret.registers = {0};
  Instruction caught;
  caught.pc = 5;
  caught.opcode = "move-exception";
  caught.registers = {0};
  method.instructions = {invoke, constant, ret, caught};
  constant.pc = 6;
  constant.literal = 7;
  ret.pc = 7;
  method.instructions.push_back(constant);
  method.instructions.push_back(ret);
  method.tries = {{0, 3, {{"Landroid/os/RemoteException;", 5}}}};
  caller.methods.push_back(method);
  Program program({caller}, std::map<std::string, std::string>{});
  auto heap = std::make_shared<Heap>();
  Runtime runtime(program, heap, BoundRuntime::application(), services());
  const auto *lookup = libraryMethod({"Landroid/os/ServiceManager;",
                                      "getService",
                                      {"Ljava/lang/String;"},
                                      "Landroid/os/IBinder;"},
                                     true);
  std::array<uint64_t, 1> name{heap->literal("alpha")->Identity};
  auto proxy = runtime.library(*lookup, 0, name, Admit).value;
  const auto *obtain = libraryMethod({Parcel, "obtain", {}, Parcel}, true);
  auto data = runtime.library(*obtain, 0, {}, Admit).value;
  std::array<uint64_t, 5> args{proxy, 23, data, 0, 0};
  const auto *body = program.findStatic(method.reference);
  auto request = runtime.execute(*body, args, Admit, 65536, 11);
  ASSERT_EQ(request.stop, ExecutionStop::Suspended) << request.diagnostic;
  ExecutionResult completion;
  completion.binder_reply = BinderReply{-32};
  auto result = runtime.resume(request.continuation, 11, std::move(completion));
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 7u);
  result = runtime.execute(*body, args, Admit, 65536, 11);
  EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 7u);
}

BinderReply binderObject(uint32_t type, uint64_t pointer, uint64_t cookie = 0,
                         bool metadata = true) {
  BinderReply reply;
  reply.bytes.emplace(24);
  llvm::support::endian::write32le(reply.bytes->data(), type);
  llvm::support::endian::write64le(reply.bytes->data() + 8, pointer);
  llvm::support::endian::write64le(reply.bytes->data() + 16, cookie);
  if (metadata)
    reply.objects = {0};
  reply.fd = 3;
  return reply;
}
TEST(AndroidBinderTest,
     ReadStrongBinderSharesNativeProxyIdentityAndRetainsOnce) {
  unsigned retains = 0, releases = 0;
  auto svc = services();
  svc.retain_binder_handle = [&](uint32_t fd, uint32_t handle) {
    EXPECT_EQ(fd, 3u);
    EXPECT_EQ(handle, 9u);
    ++retains;
    return ExecutionResult{};
  };
  BoundRuntime s(std::move(svc));
  auto reply = binderObject(0x73682a85, 9);
  auto bytes = *reply.bytes;
  reply.bytes->insert(reply.bytes->end(), bytes.begin(), bytes.end());
  reply.objects.push_back(24);
  reply.release = [&] { ++releases; };
  ASSERT_EQ(s.complete(s.transact(), std::move(reply)).stop,
            ExecutionStop::Returned);
  auto first =
      s.call(Parcel, "readStrongBinder", "()Landroid/os/IBinder;", s.reply);
  ASSERT_EQ(first.stop, ExecutionStop::Returned) << first.diagnostic;
  ASSERT_NE(first.value, 0u);
  EXPECT_EQ(s.heap->get(first.value)->ClassName, "android/os/BinderProxy");
  auto second =
      s.call(Parcel, "readStrongBinder", "()Landroid/os/IBinder;", s.reply);
  EXPECT_EQ(second.value, first.value);
  EXPECT_EQ(retains, 1u);
  auto outgoing = s.transact(s.reply, s.data);
  ASSERT_EQ(outgoing.stop, ExecutionStop::Suspended) << outgoing.diagnostic;
  EXPECT_EQ(outgoing.external->binder->objects, (std::vector<uint64_t>{0, 24}));
  ASSERT_EQ(s.complete(outgoing, {-74}).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.call(Parcel, "recycle", "()V", s.reply).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(releases, 1u);
  const auto state = s.runtime.snapshot();
  const auto &proxies = state.framework.service_manager->binder_proxies;
  ASSERT_EQ(proxies.size(), 2u);
  EXPECT_FALSE(proxies[1].key);
  EXPECT_EQ(proxies[1].identity, first.value);
  EXPECT_FALSE(*proxies[1].warn_on_blocking);
  EXPECT_EQ(proxies[1].native_binding->handle, 9u);
  auto next =
      s.call(Proxy, "transact", Transaction, first.value, {21, s.data, 0, 0});
  // The status-only reply set data.errorCheck before any further transport.
  EXPECT_EQ(next.stop, ExecutionStop::Returned);
}
TEST(AndroidBinderTest, BinderObjectsProtectScalarReadsAndCannotBeMarshalled) {
  BoundRuntime s;
  auto reply = binderObject(0x73682a85, 9);
  reply.bytes->resize(28);
  llvm::support::endian::write32le(reply.bytes->data() + 24, 0x12345678);
  ASSERT_EQ(s.complete(s.transact(), std::move(reply)).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.call(Parcel, "readInt", "()I", s.reply).value, 0u);
  EXPECT_EQ(s.call(Parcel, "readLong", "()J", s.reply).value, 0u);
  EXPECT_EQ(s.call(Parcel, "dataPosition", "()I", s.reply).value, 12u);
  s.call(Parcel, "setDataPosition", "(I)V", s.reply, {20});
  EXPECT_EQ(s.call(Parcel, "readInt", "()I", s.reply).value, 0u);
  EXPECT_EQ(s.call(Parcel, "readInt", "()I", s.reply).value, 0x12345678u);
  s.call(Parcel, "setDataPosition", "(I)V", s.reply, {0});
  EXPECT_EQ(s.call(Parcel, "readString", "()Ljava/lang/String;", s.reply).value,
            0u);
  EXPECT_EQ(s.call(Parcel, "dataPosition", "()I", s.reply).value, 8u);
  auto result = s.call(Parcel, "marshall", "()[B", s.reply);
  ASSERT_EQ(result.stop, ExecutionStop::Thrown) << result.diagnostic;
  EXPECT_EQ(result.exception, "java/lang/RuntimeException");
  auto error = s.heap->get(result.throwable);
  ASSERT_TRUE(error);
  EXPECT_EQ(
      s.heap->get(error->Message)->Bytes,
      s.heap
          ->literal("Tried to marshall a Parcel that contained Binder objects.")
          ->Bytes);
  // Unmarshalling bytes clears object metadata and does not create a proxy.
  auto array = s.heap->allocateArray("[B", 24);
  array->Bytes = *binderObject(0x73682a85, 9).bytes;
  ASSERT_EQ(
      s.call(Parcel, "unmarshall", "([BII)V", s.reply, {array->Identity, 0, 24})
          .stop,
      ExecutionStop::Returned);
  s.call(Parcel, "setDataPosition", "(I)V", s.reply, {0});
  EXPECT_EQ(
      s.call(Parcel, "readStrongBinder", "()Landroid/os/IBinder;", s.reply)
          .value,
      0u);
  EXPECT_EQ(s.call(Parcel, "marshall", "()[B", s.reply).stop,
            ExecutionStop::Returned);
}
TEST(AndroidBinderTest,
     ReadStrongBinderHonorsNullShortAndObjectTableValidation) {
  for (unsigned mode = 0; mode != 5; ++mode) {
    SCOPED_TRACE(mode);
    BoundRuntime s;
    auto reply =
        binderObject(mode == 3   ? 0x77682a85
                     : mode == 4 ? 0x73682a85
                                 : 0x73622a85,
                     mode == 0 ? 0 : 42, mode == 2 ? 99 : 0, mode >= 2);
    if (mode == 1)
      reply.bytes->resize(23);
    ASSERT_EQ(s.complete(s.transact(), std::move(reply)).stop,
              ExecutionStop::Returned);
    auto result =
        s.call(Parcel, "readStrongBinder", "()Landroid/os/IBinder;", s.reply);
    if (mode == 2)
      expectStop(result, "native Java object association");
    else if (mode == 4)
      expectStop(result, "ownership service is not supplied");
    else {
      EXPECT_EQ(result.stop, ExecutionStop::Returned);
      EXPECT_EQ(result.value, 0u);
    }
    if (result.stop == ExecutionStop::Returned)
      EXPECT_EQ(s.call(Parcel, "dataPosition", "()I", s.reply).value,
                mode == 1 ? 0u : 24u);
    else {
      const auto stopped = s.call(Parcel, "dataPosition", "()I", s.reply);
      EXPECT_EQ(stopped.stop, result.stop);
      EXPECT_EQ(stopped.diagnostic, result.diagnostic);
    } // Runtime retains its original terminal result.
  }
  BoundRuntime s;
  ASSERT_EQ(
      s.complete(s.transact(), binderObject(0x73682a85, 0, 0, false)).stop,
      ExecutionStop::Returned);
  EXPECT_EQ(
      s.call(Parcel, "readStrongBinder", "()Landroid/os/IBinder;", s.reply)
          .value,
      s.proxy);
}
TEST(AndroidBinderTest,
     NativeEndpointDeclarationsCannotAssignTwoJavaIdentities) {
  auto app = BoundRuntime::application();
  app.service_manager->binder_proxies.push_back(
      app.service_manager->binder_proxies.front());
  app.service_manager->binder_proxies.back().key = 8;
  Program program({}, std::map<std::string, std::string>{});
  EXPECT_THROW((Runtime(program, std::make_shared<Heap>(), app, services())),
               Error);
}

} // namespace
} // namespace neverd::mobile::dalvik
