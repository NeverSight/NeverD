//===- AndroidBinderObjectTests.cpp - Local Binder ownership tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent LP64 bytes, holder weak promotion and native reference effects.
//===----------------------------------------------------------------------===//
#include "../../lib/dalvik/AndroidFramework.h"
#include "gtest/gtest.h"

#include "neverd/dalvik/Library.h"

#include "llvm/Support/Endian.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
constexpr auto Binder = "Landroid/os/Binder;";
constexpr auto Parcel = "Landroid/os/Parcel;";
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
struct LocalState {
  Program program{{}, std::map<std::string, std::string>{}};
  Heap heap;
  uint64_t next = 0x4000;
  std::set<uint64_t> native;
  AndroidFramework framework;
  RuntimeServices services() {
    RuntimeServices s;
    s.binder_identity = [this](BinderIdentityOperation op, uint64_t address) {
      if (op == BinderIdentityOperation::Allocate) {
        const auto result = next;
        next += 0x1000;
        native.insert(result);
        return ExecutionResult{ExecutionStop::Returned, result};
      }
      if (!native.contains(address))
        return ExecutionResult{
            ExecutionStop::Unsupported, 0, {}, "identity is not live"};
      if (op == BinderIdentityOperation::Release)
        native.erase(address);
      return ExecutionResult{};
    };
    return s;
  }
  LocalState() : framework({}, program, services()) {}
  ExecutionResult call(const char *owner, const char *name,
                       const char *signature, uint64_t receiver = 0,
                       std::initializer_list<uint64_t> args = {},
                       bool is_static = false) {
    auto [parameters, result] = prototype(signature);
    auto *entry = libraryMethod({owner, name, parameters, result}, is_static);
    EXPECT_NE(entry, nullptr);
    return entry ? executeLibrary(heap, *entry, receiver, args, Admit, false,
                                  &program, &framework)
                 : ExecutionResult{ExecutionStop::Invalid};
  }
  uint64_t binder() {
    auto result = call(Binder, "<init>", "()V");
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  }
  uint64_t parcel() {
    return call(Parcel, "obtain", "()Landroid/os/Parcel;", 0, {}, true).value;
  }
  ExecutionResult write(uint64_t parcel, uint64_t object) {
    return call(Parcel, "writeStrongBinder", "(Landroid/os/IBinder;)V", parcel,
                {object});
  }
  void seek(uint64_t parcel, uint32_t position = 0) {
    EXPECT_EQ(call(Parcel, "setDataPosition", "(I)V", parcel, {position}).stop,
              ExecutionStop::Returned);
  }
  void recycle(uint64_t parcel) {
    EXPECT_EQ(call(Parcel, "recycle", "()V", parcel).stop,
              ExecutionStop::Returned);
  }
  uint64_t read(uint64_t parcel) {
    auto result =
        call(Parcel, "readStrongBinder", "()Landroid/os/IBinder;", parcel);
    EXPECT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
    return result.value;
  }
  std::vector<uint8_t> flat(uint64_t pointer, uint64_t cookie) {
    std::vector<uint8_t> result{0x85, 0x2a, 0x62, 0x73, 0x13, 1, 0, 0};
    result.resize(24);
    llvm::support::endian::write64le(result.data() + 8, pointer);
    llvm::support::endian::write64le(result.data() + 16, cookie);
    return result;
  }
};

TEST(AndroidLocalBinderTest, NullHasExactWireBytesAndNoObjectMetadata) {
  LocalState s;
  const auto parcel = s.parcel();
  ASSERT_EQ(s.write(parcel, 0).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.heap.get(parcel)->Bytes, s.flat(0, 0));
  EXPECT_TRUE(s.native.empty());
  auto bytes = s.call(Parcel, "marshall", "()[B", parcel);
  ASSERT_EQ(bytes.stop, ExecutionStop::Returned);
  EXPECT_EQ(s.heap.get(bytes.value)->Bytes, s.flat(0, 0));
  s.seek(parcel);
  EXPECT_EQ(s.read(parcel), 0u);
  EXPECT_EQ(s.call(Parcel, "dataPosition", "()I", parcel).value, 24u);
}

TEST(AndroidLocalBinderTest,
     OwnedParcelsKeepIdentityUntilTheLastStrongReference) {
  LocalState s;
  const auto binder = s.binder(), first = s.parcel(), second = s.parcel();
  auto *holder = s.program.findField({Binder, "mObject", "J"}, false);
  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(s.framework.field(s.heap, *holder, binder, {}, Admit).value,
            0x4000u);
  ASSERT_EQ(s.write(first, binder).stop, ExecutionStop::Returned);
  ASSERT_EQ(s.write(first, binder).stop, ExecutionStop::Returned);
  ASSERT_EQ(s.write(second, binder).stop, ExecutionStop::Returned);
  auto twice = s.flat(0x5000, 0x5008);
  const auto once = twice;
  twice.insert(twice.end(), once.begin(), once.end());
  EXPECT_EQ(s.heap.get(first)->Bytes, twice);
  EXPECT_EQ(s.heap.get(second)->Bytes, once);
  s.seek(first);
  EXPECT_EQ(s.read(first), binder);
  EXPECT_EQ(s.read(first), binder);
  EXPECT_EQ(s.call(Parcel, "marshall", "()[B", first).exception,
            "java/lang/RuntimeException");
  s.recycle(first);
  s.seek(second);
  EXPECT_EQ(s.read(second), binder);
  s.recycle(second);
  ASSERT_TRUE(s.native.contains(0x5000)); // Holder still owns its weak cache.
  const auto third = s.parcel();
  ASSERT_EQ(s.write(third, binder).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.heap.get(third)->Bytes, s.flat(0x6000, 0x6008));
  EXPECT_FALSE(s.native.contains(0x5000));
  EXPECT_EQ(s.native.size(), 2u); // Holder plus the new native Binder.
}

TEST(AndroidLocalBinderTest, SynchronousDriverReleasesRemainDeferred) {
  LocalState s;
  const auto binder = s.binder(), parcel = s.parcel();
  ASSERT_EQ(s.write(parcel, binder).stop, ExecutionStop::Returned);
  for (auto op : {BinderReferenceOperation::AcquireWeak,
                  BinderReferenceOperation::AcquireStrong,
                  BinderReferenceOperation::DeferStrongRelease,
                  BinderReferenceOperation::DeferWeakRelease})
    ASSERT_EQ(s.framework.binderReference(op, 0x5000, 0x5008).stop,
              ExecutionStop::Returned);
  s.recycle(parcel);
  const auto next = s.parcel();
  ASSERT_EQ(s.write(next, binder).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.heap.get(next)->Bytes, s.flat(0x5000, 0x5008));
  EXPECT_EQ(s.framework
                .binderReference(BinderReferenceOperation::DeferStrongRelease,
                                 0x5000, 0x5008)
                .stop,
            ExecutionStop::Unsupported);
  EXPECT_EQ(s.framework
                .binderReference(BinderReferenceOperation::AcquireWeak, 0x9990,
                                 0x5008)
                .stop,
            ExecutionStop::Unsupported);
}

TEST(AndroidLocalBinderTest, EmptyUnmarshallingDropsOwnedLocalReferences) {
  LocalState s;
  const auto binder = s.binder(), parcel = s.parcel();
  ASSERT_EQ(s.write(parcel, binder).stop, ExecutionStop::Returned);
  const auto original = s.heap.get(parcel)->Bytes;
  auto array = s.heap.allocateArray("[B", original.size());
  ASSERT_NE(array, nullptr);
  array->Bytes = original;
  EXPECT_EQ(s.call(Parcel, "unmarshall", "([BII)V", parcel,
                   {array->Identity, 0, original.size()})
                .stop,
            ExecutionStop::Unsupported);
  ASSERT_EQ(
      s.call(Parcel, "unmarshall", "([BII)V", parcel, {array->Identity, 0, 0})
          .stop,
      ExecutionStop::Returned);
  ASSERT_EQ(s.call(Parcel, "unmarshall", "([BII)V", parcel,
                   {array->Identity, 0, original.size()})
                .stop,
            ExecutionStop::Returned);
  s.seek(parcel);
  EXPECT_EQ(s.read(parcel), 0u); // Copied bytes cannot retain native ownership.
  s.seek(parcel);
  ASSERT_EQ(s.write(parcel, binder).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.heap.get(parcel)->Bytes, s.flat(0x6000, 0x6008));
  EXPECT_FALSE(s.native.contains(0x5000));
}

TEST(AndroidLocalBinderTest, ConstructorRequiresNativeServiceAndWorkAdmission) {
  LocalState s;
  AndroidFramework missing({}, s.program, {});
  const auto *constructor = libraryMethod({Binder, "<init>", {}, "V"}, false);
  ASSERT_NE(constructor, nullptr);
  EXPECT_EQ(executeLibrary(s.heap, *constructor, 0, {}, Admit, false,
                           &s.program, &missing)
                .stop,
            ExecutionStop::Unsupported);
  for (auto stop : {ExecutionStop::InstructionLimit, ExecutionStop::Timeout}) {
    EXPECT_EQ(executeLibrary(
                  s.heap, *constructor, 0, {}, [=] { return stop; }, false,
                  &s.program, &s.framework)
                  .stop,
              stop);
    EXPECT_TRUE(s.native.empty());
  }
}

TEST(AndroidLocalBinderTest, HolderAndObjectOverwriteFailuresKeepParcelBytes) {
  LocalState s;
  const auto binder = s.binder(), parcel = s.parcel();
  ASSERT_EQ(s.write(parcel, binder).stop, ExecutionStop::Returned);
  const auto expected = s.heap.get(parcel)->Bytes;
  s.seek(parcel);
  EXPECT_EQ(s.call(Parcel, "writeInt", "(I)V", parcel, {42}).stop,
            ExecutionStop::Unsupported);
  EXPECT_EQ(s.heap.get(parcel)->Bytes, expected);
  EXPECT_EQ(s.write(parcel, binder).stop, ExecutionStop::Unsupported);
  EXPECT_EQ(s.heap.get(parcel)->Bytes, expected);
  const auto *holder = s.program.findField({Binder, "mObject", "J"}, false);
  ASSERT_EQ(s.framework.field(s.heap, *holder, binder, 0, Admit).stop,
            ExecutionStop::Returned);
  s.seek(parcel, 24);
  EXPECT_EQ(s.write(parcel, binder).stop, ExecutionStop::Unsupported);
  EXPECT_EQ(s.heap.get(parcel)->Bytes, expected);
  EXPECT_EQ(s.call(Parcel, "dataPosition", "()I", parcel).value, 24u);
}

TEST(AndroidLocalBinderTest, DexSubclassConstructorUsesTheSharedAssociation) {
  Class child;
  child.name = "Ltest/LocalBinder;";
  child.superclass = Binder;
  child.access = {"public"};
  Method constructor;
  constructor.reference = {child.name, "<init>", {}, "V"};
  constructor.access = {"public", "constructor"};
  constructor.registers = 1;
  Instruction base;
  base.pc = 0;
  base.opcode = "invoke-direct";
  base.registers = {0};
  base.reference = MethodRef{Binder, "<init>", {}, "V"};
  Instruction done;
  done.pc = 3;
  done.opcode = "return-void";
  constructor.instructions = {base, done};
  constructor.code_end = 4;
  child.methods.push_back(constructor);
  Method make;
  make.reference = {child.name, "make", {}, child.name};
  make.access = {"public", "static"};
  make.registers = 1;
  Instruction allocate;
  allocate.pc = 0;
  allocate.opcode = "new-instance";
  allocate.registers = {0};
  allocate.reference = child.name;
  Instruction initialize = base;
  initialize.pc = 2;
  initialize.reference = constructor.reference;
  Instruction result;
  result.pc = 5;
  result.opcode = "return-object";
  result.registers = {0};
  make.instructions = {allocate, initialize, result};
  make.code_end = 6;
  child.methods.push_back(make);
  Program program({child}, std::map<std::string, std::string>{});
  auto heap = std::make_shared<Heap>();
  LocalState native;
  Runtime runtime(program, heap, {}, native.services());
  const auto value =
      runtime.execute(*program.findStatic(make.reference), {}, Admit, 65536);
  ASSERT_EQ(value.stop, ExecutionStop::Returned) << value.diagnostic;
  ASSERT_TRUE(heap->get(value.value)->Initialized);
  const auto *field = program.findField({Binder, "mObject", "J"}, false);
  EXPECT_EQ(runtime.field(*field, value.value, {}, Admit, 65536).value,
            0x4000u);
  auto call = [&](const char *name, const char *signature, uint64_t receiver,
                  std::initializer_list<uint64_t> arguments,
                  bool is_static = false) {
    auto [parameters, returns] = prototype(signature);
    return runtime.library(
        *libraryMethod({Parcel, name, parameters, returns}, is_static),
        receiver, arguments, Admit);
  };
  const auto parcel =
      call("obtain", "()Landroid/os/Parcel;", 0, {}, true).value;
  ASSERT_EQ(call("writeStrongBinder", "(Landroid/os/IBinder;)V", parcel,
                 {value.value})
                .stop,
            ExecutionStop::Returned);
  ASSERT_EQ(call("setDataPosition", "(I)V", parcel, {0}).stop,
            ExecutionStop::Returned);
  const auto read =
      call("readStrongBinder", "()Landroid/os/IBinder;", parcel, {});
  EXPECT_EQ(read.stop, ExecutionStop::Returned) << read.diagnostic;
  EXPECT_EQ(read.value, value.value);
}
} // namespace
} // namespace neverd::mobile::dalvik
