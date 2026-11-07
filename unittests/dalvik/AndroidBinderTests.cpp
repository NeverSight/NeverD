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
} // namespace
} // namespace neverd::mobile::dalvik
