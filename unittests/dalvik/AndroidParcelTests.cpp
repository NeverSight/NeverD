//===- AndroidParcelTests.cpp - Owned API 28 Parcel data ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent wire bytes, lifecycle identities and DEX/library equivalence.
//===----------------------------------------------------------------------===//

#include "../../lib/dalvik/AndroidFramework.h"
#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
constexpr auto Parcel = "Landroid/os/Parcel;";
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
struct State {
  Program program{{}, std::map<std::string, std::string>{}};
  std::shared_ptr<Heap> heap;
  AndroidFramework framework;
  explicit State(uint64_t bytes = ::neverd::limits::dalvik::kHeapByteLimit,
                 uint32_t objects = ::neverd::limits::dalvik::kHeapObjectLimit,
                 RuntimeServices services = {})
      : heap(std::make_shared<Heap>(bytes, objects)),
        framework({}, program, std::move(services)) {}
  explicit State(RuntimeServices services)
      : State(::neverd::limits::dalvik::kHeapByteLimit,
              ::neverd::limits::dalvik::kHeapObjectLimit, std::move(services)) {
  }
  ExecutionResult binder(const char *name, const char *signature,
                         std::initializer_list<uint64_t> args = {},
                         const AdmitInstruction &admit = Admit) {
    auto [parameters, result] = prototype(signature);
    const auto *entry =
        libraryMethod({"Landroid/os/Binder;", name, parameters, result}, true);
    EXPECT_NE(entry, nullptr);
    return entry ? executeLibrary(*heap, *entry, 0, args, admit, false,
                                  &program, &framework)
                 : ExecutionResult{ExecutionStop::Invalid};
  }
  ExecutionResult call(const char *name, const char *signature, uint64_t object,
                       std::initializer_list<uint64_t> args = {},
                       const AdmitInstruction &admit = Admit) {
    auto [parameters, result] = prototype(signature);
    const auto *entry = libraryMethod({Parcel, name, parameters, result},
                                      std::string_view(name) == "obtain");
    EXPECT_NE(entry, nullptr);
    return entry ? executeLibrary(*heap, *entry, object, args, admit, false,
                                  &program, &framework)
                 : ExecutionResult{ExecutionStop::Invalid};
  }
  uint64_t obtain() {
    const auto result = call("obtain", "()Landroid/os/Parcel;", 0);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  }
  uint64_t query(uint64_t object, const char *name) {
    const auto result = call(name, "()I", object);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  }
  void seek(uint64_t object, uint32_t position) {
    EXPECT_EQ(call("setDataPosition", "(I)V", object, {position}).stop,
              ExecutionStop::Returned);
  }
  uint64_t bytes(std::initializer_list<uint8_t> values) {
    auto array = heap->allocateArray("[B", values.size());
    EXPECT_NE(array, nullptr);
    if (!array)
      return 0;
    std::copy(values.begin(), values.end(), array->Bytes.begin());
    return array->Identity;
  }
  void load(uint64_t object, std::initializer_list<uint8_t> values) {
    EXPECT_EQ(
        call("unmarshall", "([BII)V", object, {bytes(values), 0, values.size()})
            .stop,
        ExecutionStop::Returned);
    seek(object, 0);
  }
};

TEST(AndroidParcelTest, DeclarationsArePublicAndFinalWithoutNativePointers) {
  State s;
  const auto *type = s.program.findClass(Parcel);
  ASSERT_NE(type, nullptr);
  EXPECT_TRUE(type->access.contains("public"));
  EXPECT_TRUE(type->access.contains("final"));
  EXPECT_TRUE(type->fields.empty());
  for (const auto &entry : libraryMethods()) {
    if (std::string_view(entry.Class) != "android/os/Parcel")
      continue;
    EXPECT_TRUE(entry.Public);
    EXPECT_EQ(entry.Final, !entry.Static);
  }
  EXPECT_FALSE(libraryMethod(
      {Parcel, "readStrongBinder", {}, "Landroid/os/IBinder;"}, false));
}

TEST(AndroidParcelTest, PoolUsesSixSlotsAndClearsDataBeforeIdentityReuse) {
  State s;
  std::array<uint64_t, 7> objects;
  for (auto &id : objects) {
    id = s.obtain();
    EXPECT_EQ(s.query(id, "dataSize"), 0u);
    EXPECT_EQ(s.call("writeInt", "(I)V", id, {123}).stop,
              ExecutionStop::Returned);
  }
  for (auto id : objects)
    EXPECT_EQ(s.call("recycle", "()V", id).stop, ExecutionStop::Returned);
  for (unsigned i = 0; i != 6; ++i) {
    const auto id = s.obtain();
    EXPECT_EQ(id, objects[i]);
    EXPECT_EQ(s.query(id, "dataSize"), 0u);
    EXPECT_EQ(s.query(id, "dataPosition"), 0u);
    EXPECT_EQ(s.query(id, "dataAvail"), 0u);
  }
  const auto extra = s.obtain();
  for (auto id : objects)
    EXPECT_NE(extra, id);
  // A released middle slot is selected before allocating any new identity.
  EXPECT_EQ(s.call("recycle", "()V", objects[3]).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.obtain(), objects[3]);
}

TEST(AndroidParcelTest, RawObjectsAndRecycledAccessDoNotInventNativeState) {
  State s;
  const auto raw = s.heap->allocate(Object::Instance, "android/os/Parcel", 0);
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(s.call("dataSize", "()I", raw->Identity).stop,
            ExecutionStop::Unsupported);
  const auto id = s.obtain();
  EXPECT_EQ(s.call("recycle", "()V", id).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.call("recycle", "()V", id).stop, ExecutionStop::Unsupported);
  EXPECT_EQ(s.call("readInt", "()I", id).stop, ExecutionStop::Unsupported);
  EXPECT_EQ(s.obtain(), id);
  EXPECT_EQ(s.call("readInt", "()I", id).stop, ExecutionStop::Returned);
  Runtime independent(s.program, s.heap);
  const auto *entry = libraryMethod({Parcel, "dataSize", {}, "I"}, false);
  EXPECT_EQ(independent.library(*entry, id, {}, Admit).stop,
            ExecutionStop::Unsupported);
}

TEST(AndroidParcelTest, ScalarWireBytesAndUnalignedOverwriteRetainTheSuffix) {
  State s;
  const auto id = s.obtain();
  EXPECT_EQ(s.call("writeInt", "(I)V", id, {0xfffffff9}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.call("writeLong", "(J)V", id, {0x8877665544332211}).stop,
            ExecutionStop::Returned);
  const std::vector<uint8_t> expected{0xf9, 0xff, 0xff, 0xff, 0x11, 0x22,
                                      0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  EXPECT_EQ(s.heap->get(id)->Bytes, expected);
  EXPECT_EQ(s.query(id, "dataPosition"), 12u);
  s.seek(id, 0);
  EXPECT_EQ(s.query(id, "dataAvail"), 12u);
  EXPECT_EQ(s.call("readInt", "()I", id).value, 0xfffffff9u);
  EXPECT_EQ(s.call("readLong", "()J", id).value, 0x8877665544332211u);
  EXPECT_EQ(s.call("readLong", "()J", id).value, 0u);
  EXPECT_EQ(s.query(id, "dataPosition"), 12u);
  s.seek(id, 1);
  EXPECT_EQ(s.call("writeInt", "(I)V", id, {0x12345678}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.heap->get(id)->Bytes,
            (std::vector<uint8_t>{0xf9, 0x78, 0x56, 0x34, 0x12, 0x22, 0x33,
                                  0x44, 0x55, 0x66, 0x77, 0x88}));
  EXPECT_EQ(s.query(id, "dataSize"), 12u);
  EXPECT_EQ(s.query(id, "dataAvail"), 7u);
}

TEST(AndroidParcelTest, StringsKeepNullEmptyNulSurrogatesAndPaddingDistinct) {
  State s;
  const auto id = s.obtain();
  auto text = s.heap->allocate(Object::String, "java/lang/String", 10);
  text->Bytes = {'A', 0, 0, 0, 0x3d, 0xd8, 0, 0xde, 0, 0xd8};
  text->Length = 5;
  const uint64_t empty = s.heap->literal("")->Identity;
  for (auto value : {uint64_t(0), empty, uint64_t(text->Identity)})
    EXPECT_EQ(s.call("writeString", "(Ljava/lang/String;)V", id, {value}).stop,
              ExecutionStop::Returned);
  const std::vector<uint8_t> expected{
      0xff, 0xff, 0xff, 0xff, 0, 0, 0,    0,    0, 0,    0, 0,    5, 0,
      0,    0,    'A',  0,    0, 0, 0x3d, 0xd8, 0, 0xde, 0, 0xd8, 0, 0};
  EXPECT_EQ(s.heap->get(id)->Bytes, expected);
  s.seek(id, 0);
  EXPECT_EQ(s.call("readString", "()Ljava/lang/String;", id).value, 0u);
  auto zero = s.call("readString", "()Ljava/lang/String;", id);
  ASSERT_NE(zero.value, 0u);
  EXPECT_TRUE(s.heap->get(zero.value)->Bytes.empty());
  auto result = s.call("readString", "()Ljava/lang/String;", id);
  ASSERT_NE(result.value, 0u);
  EXPECT_NE(result.value, text->Identity);
  EXPECT_EQ(s.heap->get(result.value)->Bytes, text->Bytes);
  EXPECT_EQ(s.query(id, "dataAvail"), 0u);
  // Two code units require both a terminator and two padding bytes.
  s.seek(id, 0);
  EXPECT_EQ(s.call("writeString", "(Ljava/lang/String;)V", id,
                   {s.heap->literal("ab")->Identity})
                .stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.query(id, "dataPosition"), 12u);
  EXPECT_EQ(s.heap->get(id)->Bytes[10], 0u);
  EXPECT_EQ(s.heap->get(id)->Bytes[11], 0u);
}

TEST(AndroidParcelTest, MarshallAndUnmarshallCopyArraysAndPadTheImportedSize) {
  State s;
  const auto id = s.obtain();
  const auto array = s.bytes({0xaa, 1, 2, 3, 4, 5, 0xbb});
  EXPECT_EQ(s.call("unmarshall", "([BII)V", id, {array, 1, 5}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.query(id, "dataSize"), 8u);
  EXPECT_EQ(s.query(id, "dataPosition"), 8u);
  EXPECT_EQ(s.heap->get(id)->Bytes,
            (std::vector<uint8_t>{1, 2, 3, 4, 5, 0, 0, 0}));
  auto copy = s.call("marshall", "()[B", id);
  ASSERT_EQ(copy.stop, ExecutionStop::Returned);
  ASSERT_NE(copy.value, 0u);
  EXPECT_EQ(s.heap->get(copy.value)->ElementSize, 1u);
  EXPECT_EQ(s.heap->get(copy.value)->Length, 8u);
  s.heap->get(array)->Bytes[1] = 99;
  s.heap->get(copy.value)->Bytes[0] = 77;
  EXPECT_EQ(s.heap->get(id)->Bytes.front(), 1u);
  s.load(id, {});
  EXPECT_EQ(s.query(id, "dataSize"), 0u);
  EXPECT_NE(s.call("marshall", "()[B", id).value, 0u);
}

TEST(AndroidParcelTest, ShortAndMalformedReadsFollowAPI28CursorEffects) {
  State s;
  const auto id = s.obtain();
  for (uint32_t length :
       {0xffffffffu, 0xfffffffeu, 0x7fffffffu, 0x40000000u, 10u}) {
    s.load(id, {uint8_t(length), uint8_t(length >> 8), uint8_t(length >> 16),
                uint8_t(length >> 24), 'x', 0, 0, 0});
    EXPECT_EQ(s.call("readString", "()Ljava/lang/String;", id).value, 0u);
    EXPECT_EQ(s.query(id, "dataPosition"), 4u);
  }
  // API 28 accepts a nonzero UTF-16 terminator and still consumes padding.
  s.load(id, {1, 0, 0, 0, 'x', 0, 'z', 0});
  const auto value = s.call("readString", "()Ljava/lang/String;", id);
  ASSERT_NE(value.value, 0u);
  EXPECT_EQ(s.heap->get(value.value)->Bytes, (std::vector<uint8_t>{'x', 0}));
  EXPECT_EQ(s.query(id, "dataPosition"), 8u);
  s.seek(id, 5);
  EXPECT_EQ(s.call("readInt", "()I", id).value, 0u);
  EXPECT_EQ(s.call("readString", "()Ljava/lang/String;", id).value, 0u);
  EXPECT_EQ(s.query(id, "dataPosition"), 5u);
}

TEST(AndroidParcelTest, NativeUndefinedRangesStayExplicitWithoutChangingBytes) {
  State s;
  const auto id = s.obtain();
  s.load(id, {1, 2, 3, 4});
  const auto array = s.bytes({8, 9});
  EXPECT_EQ(s.call("unmarshall", "([BII)V", id, {0, 0, 0xffffffff}).stop,
            ExecutionStop::Returned);
  for (auto args : {std::array<uint64_t, 3>{0, 0, 1},
                    {array, 0xffffffff, 1},
                    {array, 1, 2},
                    {array, 0, 0x7fffffff}})
    EXPECT_EQ(
        s.call("unmarshall", "([BII)V", id, {args[0], args[1], args[2]}).stop,
        ExecutionStop::Unsupported);
  EXPECT_EQ(s.call("setDataPosition", "(I)V", id, {0xffffffff}).stop,
            ExecutionStop::Unsupported);
  EXPECT_EQ(s.query(id, "dataPosition"), 0u);
  s.seek(id, INT32_MAX);
  EXPECT_EQ(s.query(id, "dataSize"), uint64_t(INT32_MAX));
  EXPECT_EQ(s.query(id, "dataAvail"), 0u);
  EXPECT_EQ(s.call("readLong", "()J", id).value, 0u);
  EXPECT_EQ(s.call("marshall", "()[B", id).stop, ExecutionStop::Unsupported);
  EXPECT_EQ(s.call("writeInt", "(I)V", id, {7}).stop,
            ExecutionStop::Unsupported);
  EXPECT_EQ(s.heap->get(id)->Bytes, (std::vector<uint8_t>{1, 2, 3, 4}));
  s.seek(id, 0);
  EXPECT_EQ(s.call("readInt", "()I", id).value, 0x04030201u);
}

TEST(AndroidParcelTest, InterfaceTokenStopsBeforeInventingBinderThreadPolicy) {
  State s;
  const auto data = s.obtain(), reply = s.obtain();
  EXPECT_NE(data, reply);
  auto result =
      s.call("writeInterfaceToken", "(Ljava/lang/String;)V", data,
             {s.heap->literal("android.os.IServiceManager")->Identity});
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("StrictMode"), std::string::npos);
  EXPECT_EQ(s.query(data, "dataSize"), 0u);
  EXPECT_EQ(s.query(reply, "dataSize"), 0u);
}

RuntimeServices policyServices(uint32_t &policy) {
  RuntimeServices services;
  services.binder_thread_policy = [&policy] {
    return ExecutionResult{ExecutionStop::Returned, policy};
  };
  services.set_binder_thread_policy = [&policy](uint32_t value) {
    policy = value;
    return ExecutionResult{};
  };
  return services;
}

TEST(AndroidParcelTest, BinderPolicyUsesServicesAndAllJavaIntBits) {
  uint32_t policy = 0x81234567;
  State s(policyServices(policy));
  const auto *get = libraryMethod(
      {"Landroid/os/Binder;", "getThreadStrictModePolicy", {}, "I"}, true);
  ASSERT_NE(get, nullptr);
  EXPECT_TRUE(get->Final && get->Public);
  EXPECT_EQ(s.binder("getThreadStrictModePolicy", "()I").value, policy);
  for (uint64_t value :
       {0ULL, 0x80000000ULL, 0xffffffffULL, 0xfeedface12345678ULL}) {
    EXPECT_EQ(s.binder("setThreadStrictModePolicy", "(I)V", {value}).stop,
              ExecutionStop::Returned);
    EXPECT_EQ(policy, uint32_t(value));
    EXPECT_EQ(s.binder("getThreadStrictModePolicy", "()I").value,
              uint64_t(uint32_t(value)));
  }
  EXPECT_EQ(s.binder("setThreadStrictModePolicy", "(I)V", {0},
                     [] { return ExecutionStop::InstructionLimit; })
                .stop,
            ExecutionStop::InstructionLimit);
  EXPECT_EQ(policy, 0x12345678u);
  State missing;
  EXPECT_EQ(missing.binder("getThreadStrictModePolicy", "()I").stop,
            ExecutionStop::Unsupported);
  EXPECT_EQ(missing.binder("setThreadStrictModePolicy", "(I)V", {0}).stop,
            ExecutionStop::Unsupported);
}

TEST(AndroidParcelTest, InterfaceTokenUsesCurrentPolicyAndExactUTF16WireBytes) {
  uint32_t policy = 0;
  State s(policyServices(policy));
  auto string = s.heap->allocate(Object::String, "java/lang/String", 6);
  ASSERT_NE(string, nullptr);
  string->Length = 3;
  string->Bytes = {0, 0, 0, 0xd8, 'X', 0};
  const auto id = s.obtain();
  struct Case {
    uint32_t policy;
    std::array<uint8_t, 4> wire;
  };
  for (auto test :
       {Case{0, {0, 0, 0x40, 0}}, Case{0x80000001, {1, 0, 0x40, 0x80}},
        Case{0x00400000, {0, 0, 0x40, 0}},
        Case{0xffffffff, {0xff, 0xff, 0xff, 0xff}}}) {
    policy = test.policy;
    s.seek(id, 0);
    auto result = s.call("writeInterfaceToken", "(Ljava/lang/String;)V", id,
                         {string->Identity});
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    std::vector<uint8_t> expected(test.wire.begin(), test.wire.end());
    const uint8_t tail[] = {3, 0, 0, 0, 0, 0, 0, 0xd8, 'X', 0, 0, 0};
    expected.insert(expected.end(), std::begin(tail), std::end(tail));
    EXPECT_EQ(s.heap->get(id)->Bytes, expected);
    EXPECT_EQ(s.query(id, "dataPosition"), 16u);
    EXPECT_EQ(policy, test.policy);
  }
  const auto empty = s.obtain();
  policy = 7;
  ASSERT_EQ(s.call("writeInterfaceToken", "(Ljava/lang/String;)V", empty,
                   {s.heap->literal("")->Identity})
                .stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.heap->get(empty)->Bytes,
            (std::vector<uint8_t>{7, 0, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
}

TEST(AndroidParcelTest, InterfaceTokenRefusesNullAndPropagatesServiceStops) {
  unsigned reads = 0;
  RuntimeServices services;
  services.binder_thread_policy = [&] {
    ++reads;
    return ExecutionResult{ExecutionStop::Timeout, 0, {}, "policy stop"};
  };
  State s(std::move(services));
  const auto id = s.obtain();
  auto null = s.call("writeInterfaceToken", "(Ljava/lang/String;)V", id, {0});
  EXPECT_EQ(null.stop, ExecutionStop::Unsupported);
  EXPECT_NE(null.diagnostic.find("nonnull"), std::string::npos);
  EXPECT_EQ(reads, 0u);
  auto stopped = s.call("writeInterfaceToken", "(Ljava/lang/String;)V", id,
                        {s.heap->literal("x")->Identity});
  EXPECT_EQ(stopped.stop, ExecutionStop::Timeout);
  EXPECT_EQ(stopped.diagnostic, "policy stop");
  EXPECT_EQ(reads, 1u);
  EXPECT_EQ(s.query(id, "dataPosition"), 0u);
  EXPECT_TRUE(s.heap->get(id)->Bytes.empty());
}

TEST(AndroidParcelTest, InterfaceTokenWorkStopsPreserveCompletedHeaderWrites) {
  uint32_t policy = 0x80000001;
  for (unsigned budget : {1u, 3u, 5u}) {
    State s(policyServices(policy));
    const auto id = s.obtain();
    unsigned remaining = budget;
    auto result = s.call("writeInterfaceToken", "(Ljava/lang/String;)V", id,
                         {s.heap->literal("abc")->Identity}, [&] {
                           if (!remaining)
                             return ExecutionStop::InstructionLimit;
                           --remaining;
                           return ExecutionStop::Returned;
                         });
    EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
    std::vector<uint8_t> expected = {1, 0, 0x40, 0x80, 3, 0, 0, 0};
    expected.resize((budget - 1) * 2);
    EXPECT_EQ(s.heap->get(id)->Bytes, expected);
    EXPECT_EQ(s.query(id, "dataPosition"), expected.size());
    EXPECT_EQ(policy, 0x80000001u);
  }
}

TEST(AndroidParcelTest, DexBinderAndTokenCallsReuseRuntimeServices) {
  constexpr auto Binder = "Landroid/os/Binder;";
  Class caller;
  caller.name = "Ltest/BinderCaller;";
  caller.superclass = "Ljava/lang/Object;";
  caller.access = {"public"};
  Method method;
  method.reference = {
      caller.name, "write", {"I", Parcel, "Ljava/lang/String;"}, "I"};
  method.access = {"public", "static"};
  method.registers = 3;
  Instruction set;
  set.pc = 0;
  set.opcode = "invoke-static";
  set.registers = {0};
  set.reference = MethodRef{Binder, "setThreadStrictModePolicy", {"I"}, "V"};
  Instruction token;
  token.pc = 3;
  token.opcode = "invoke-virtual";
  token.registers = {1, 2};
  token.reference =
      MethodRef{Parcel, "writeInterfaceToken", {"Ljava/lang/String;"}, "V"};
  Instruction get;
  get.pc = 6;
  get.opcode = "invoke-static";
  get.reference = MethodRef{Binder, "getThreadStrictModePolicy", {}, "I"};
  Instruction move;
  move.pc = 9;
  move.opcode = "move-result";
  move.registers = {0};
  Instruction ret;
  ret.pc = 10;
  ret.opcode = "return";
  ret.registers = {0};
  method.instructions = {set, token, get, move, ret};
  caller.methods.push_back(method);
  Program program({caller}, std::map<std::string, std::string>{});
  auto heap = std::make_shared<Heap>();
  uint32_t policy = 17;
  Runtime runtime(program, heap, {}, policyServices(policy));
  const auto *obtain = libraryMethod({Parcel, "obtain", {}, Parcel}, true);
  ASSERT_NE(obtain, nullptr);
  auto data = runtime.library(*obtain, 0, {}, Admit);
  ASSERT_EQ(data.stop, ExecutionStop::Returned);
  const uint64_t args[] = {0x80000003, data.value,
                           heap->literal("x")->Identity};
  const auto *body = program.findMethod(method.reference, true);
  ASSERT_NE(body, nullptr);
  auto result = runtime.execute(*body, args, Admit, 65536);
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(result.value, 0x80000003u);
  EXPECT_EQ(policy, 0x80000003u);
  EXPECT_EQ(heap->get(data.value)->Bytes,
            (std::vector<uint8_t>{3, 0, 0x40, 0x80, 1, 0, 0, 0, 'x', 0, 0, 0}));
}

TEST(AndroidParcelTest, WorkStopsKeepTheAlreadyWrittenStringLength) {
  State s;
  const auto id = s.obtain();
  const auto text = s.heap->literal("abc")->Identity;
  unsigned remaining = 3; // Library admission, then the int32 length.
  auto result = s.call("writeString", "(Ljava/lang/String;)V", id, {text},
                       [&] { return ExecutionStop::InstructionLimit; });
  // First prove an entry stop cannot mutate the Parcel.
  EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
  EXPECT_TRUE(s.heap->get(id)->Bytes.empty());
  remaining = 3;
  result = s.call("writeString", "(Ljava/lang/String;)V", id, {text}, [&] {
    if (!remaining)
      return ExecutionStop::InstructionLimit;
    --remaining;
    return ExecutionStop::Returned;
  });
  EXPECT_EQ(result.stop, ExecutionStop::InstructionLimit);
  EXPECT_EQ(s.heap->get(id)->Bytes, (std::vector<uint8_t>{3, 0, 0, 0}));
  EXPECT_EQ(s.query(id, "dataPosition"), 4u);
}

TEST(AndroidParcelTest, HeapLimitsBoundOwnedStorageAndReadAllocation) {
  const uint64_t overhead =
      sizeof(Object) + std::string_view("android/os/Parcel").size() + 64 +
      ::neverd::limits::dalvik::kFrameworkFieldStateOverheadBytes;
  State small(overhead + 4);
  const auto id = small.obtain();
  EXPECT_EQ(small.call("writeLong", "(J)V", id, {9}).stop,
            ExecutionStop::HeapLimit);
  EXPECT_EQ(small.query(id, "dataPosition"), 0u);
  EXPECT_EQ(small.call("writeInt", "(I)V", id, {5}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(small.call("marshall", "()[B", id).stop, ExecutionStop::HeapLimit);
  State objects(::neverd::limits::dalvik::kHeapByteLimit, 2);
  const auto p = objects.obtain();
  EXPECT_EQ(objects
                .call("writeString", "(Ljava/lang/String;)V", p,
                      {objects.heap->literal("x")->Identity})
                .stop,
            ExecutionStop::Returned);
  objects.seek(p, 0);
  EXPECT_EQ(objects.call("readString", "()Ljava/lang/String;", p).stop,
            ExecutionStop::HeapLimit);
  EXPECT_EQ(objects.query(p, "dataPosition"), 8u);
}

TEST(AndroidParcelTest, DexCallsAndLibraryCallsShareTheSameParcelIdentity) {
  Class caller;
  caller.name = "Ltest/ParcelCaller;";
  caller.superclass = "Ljava/lang/Object;";
  caller.access = {"public"};
  Method method;
  method.reference = {caller.name, "make", {}, Parcel};
  method.access = {"public", "static"};
  method.registers = 1;
  Instruction invoke;
  invoke.pc = 0;
  invoke.opcode = "invoke-static";
  invoke.reference = MethodRef{Parcel, "obtain", {}, Parcel};
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
  auto heap = std::make_shared<Heap>();
  Runtime runtime(program, heap);
  const auto *body = program.findMethod(method.reference, true);
  auto first = runtime.execute(*body, {}, Admit, 65536);
  ASSERT_EQ(first.stop, ExecutionStop::Returned) << first.diagnostic;
  auto recycle = libraryMethod({Parcel, "recycle", {}, "V"}, false);
  ASSERT_NE(recycle, nullptr);
  ASSERT_EQ(runtime.library(*recycle, first.value, {}, Admit).stop,
            ExecutionStop::Returned);
  auto second = runtime.execute(*body, {}, Admit, 65536);
  EXPECT_EQ(second.stop, ExecutionStop::Returned) << second.diagnostic;
  EXPECT_EQ(second.value, first.value);
}

TEST(AndroidParcelTest,
     RuntimeTerminalBoundaryKeepsPriorDataAndCannotBeRetried) {
  Program program({}, std::map<std::string, std::string>{});
  auto heap = std::make_shared<Heap>();
  Runtime runtime(program, heap);
  const auto *obtain = libraryMethod({Parcel, "obtain", {}, Parcel}, true);
  const auto *write = libraryMethod({Parcel, "writeInt", {"I"}, "V"}, false);
  const auto *token = libraryMethod(
      {Parcel, "writeInterfaceToken", {"Ljava/lang/String;"}, "V"}, false);
  ASSERT_NE(obtain, nullptr);
  ASSERT_NE(write, nullptr);
  ASSERT_NE(token, nullptr);
  const auto object = runtime.library(*obtain, 0, {}, Admit);
  ASSERT_EQ(object.stop, ExecutionStop::Returned);
  const uint64_t args[] = {0x12345678};
  ASSERT_EQ(runtime.library(*write, object.value, args, Admit).stop,
            ExecutionStop::Returned);
  const uint64_t name[] = {
      heap->literal("android.os.IServiceManager")->Identity};
  const auto stopped = runtime.library(*token, object.value, name, Admit);
  ASSERT_EQ(stopped.stop, ExecutionStop::Unsupported);
  EXPECT_TRUE(runtime.snapshot().terminal);
  const auto retry = runtime.library(*write, object.value, args, Admit);
  EXPECT_EQ(retry.stop, stopped.stop);
  EXPECT_EQ(retry.diagnostic, stopped.diagnostic);
  EXPECT_EQ(heap->get(object.value)->Bytes,
            (std::vector<uint8_t>{0x78, 0x56, 0x34, 0x12}));
}
} // namespace
} // namespace neverd::mobile::dalvik
