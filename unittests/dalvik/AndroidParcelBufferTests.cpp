//===- AndroidParcelBufferTests.cpp - Borrowed Parcel ownership -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent capacity, live-buffer and Binder reference lifetime controls.
//===----------------------------------------------------------------------===//
#include "../../lib/dalvik/AndroidFramework.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
constexpr auto Parcel = "Landroid/os/Parcel;";
const AdmitInstruction Admit = [] { return ExecutionStop::Returned; };
struct BufferState {
  Program program{{}, std::map<std::string, std::string>{}};
  Heap heap;
  std::vector<std::string> events;
  std::set<uint64_t> identities;
  uint64_t next = 0x4000;
  AndroidFramework framework;
  RuntimeServices services() {
    RuntimeServices result;
    result.binder_identity = [this](BinderIdentityOperation op, uint64_t id) {
      if (op == BinderIdentityOperation::Allocate) {
        id = next;
        next += 0x1000;
        identities.insert(id);
      } else if (!identities.contains(id))
        return ExecutionResult{ExecutionStop::Invalid};
      else if (op == BinderIdentityOperation::Release)
        identities.erase(id);
      return ExecutionResult{ExecutionStop::Returned, id};
    };
    result.retain_binder_handle = [this](uint32_t fd, uint32_t handle) {
      EXPECT_EQ(fd, 3u);
      events.push_back("acquire " + std::to_string(handle));
      return ExecutionResult{};
    };
    result.release_binder_handle = [this](uint32_t fd, uint32_t handle) {
      EXPECT_EQ(fd, 3u);
      events.push_back("release " + std::to_string(handle));
      return ExecutionResult{};
    };
    return result;
  }
  explicit BufferState(
      uint64_t limit = ::neverd::limits::dalvik::kHeapByteLimit)
      : heap(limit), framework({}, program, services()) {}
  ExecutionResult call(const char *name, const char *signature, uint64_t id,
                       std::initializer_list<uint64_t> args = {},
                       const AdmitInstruction &admit = Admit) {
    auto [parameters, result] = prototype(signature);
    const auto *entry = libraryMethod({Parcel, name, parameters, result},
                                      std::string_view(name) == "obtain");
    EXPECT_NE(entry, nullptr);
    return entry ? executeLibrary(heap, *entry, id, args, admit, false,
                                  &program, &framework)
                 : ExecutionResult{ExecutionStop::Invalid};
  }
  uint64_t obtain() {
    auto result = call("obtain", "()Landroid/os/Parcel;", 0);
    EXPECT_EQ(result.stop, ExecutionStop::Returned);
    return result.value;
  }
  void seek(uint64_t id, uint32_t position) {
    ASSERT_EQ(call("setDataPosition", "(I)V", id, {position}).stop,
              ExecutionStop::Returned);
  }
  void install(uint64_t id, BinderReply reply) {
    ExternalCall call;
    call.kind = ExternalCall::BinderTransaction;
    call.binder.emplace().reply = id;
    auto result = framework.completeBinder(heap, call, std::move(reply));
    ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  }
  BinderReply borrowed(std::vector<uint8_t> &arena,
                       std::vector<uint64_t> objects = {}) {
    BinderReply reply;
    reply.bytes = arena;
    reply.fd = 3;
    reply.objects = std::move(objects);
    reply.view = [&arena](std::span<const uint8_t> &view) {
      view = arena;
      return ExecutionResult{};
    };
    reply.release = [this] { events.push_back("free"); };
    return reply;
  }
};

TEST(AndroidParcelBufferTest, GrowthCopiesLiveBytesAndReleasesTheArenaOnce) {
  BufferState s;
  const auto id = s.obtain();
  std::vector<uint8_t> arena{1, 2, 3, 4};
  s.install(id, s.borrowed(arena));
  arena[0] = 9;
  s.seek(id, 4);
  auto result = s.call("writeInt", "(I)V", id, {0x88776655});
  ASSERT_EQ(result.stop, ExecutionStop::Returned) << result.diagnostic;
  EXPECT_EQ(s.heap.get(id)->Bytes,
            (std::vector<uint8_t>{9, 2, 3, 4, 0x55, 0x66, 0x77, 0x88}));
  EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 12u);
  EXPECT_EQ(s.call("dataSize", "()I", id).value, 8u);
  EXPECT_EQ(s.events, (std::vector<std::string>{"free"}));
  arena.assign(4, 0);
  s.seek(id, 0);
  EXPECT_EQ(s.call("readInt", "()I", id).value, 0x04030209u);
  ASSERT_EQ(s.call("recycle", "()V", id).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.events.size(), 1u);
}

TEST(AndroidParcelBufferTest,
     CapacityDoesNotExposeUninitializedAllocationBytes) {
  BufferState s;
  const auto id = s.obtain();
  ASSERT_EQ(s.call("writeInt", "(I)V", id, {1}).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 6u);
  ASSERT_EQ(s.call("writeInt", "(I)V", id, {2}).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 12u);
  ASSERT_EQ(s.call("setDataCapacity", "(I)V", id, {29}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 29u);
  EXPECT_EQ(s.call("dataSize", "()I", id).value, 8u);
  EXPECT_EQ(s.call("readInt", "()I", id).value, 0u);
  EXPECT_EQ(s.call("dataPosition", "()I", id).value, 8u);
  s.seek(id, 9);
  EXPECT_EQ(s.call("writeInt", "(I)V", id, {42}).stop,
            ExecutionStop::Unsupported);
  EXPECT_EQ(s.heap.get(id)->Bytes.size(), 8u);
  EXPECT_EQ(s.call("setDataCapacity", "(I)V", id, {UINT32_MAX}).exception,
            "java/lang/IllegalArgumentException");
  EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 29u);
}

TEST(AndroidParcelBufferTest, CapacityNoOpDoesNotReadOrReleaseBorrowedStorage) {
  BufferState s;
  const auto id = s.obtain();
  std::vector<uint8_t> arena{1, 2, 3, 4};
  auto reply = s.borrowed(arena);
  reply.view = [](std::span<const uint8_t> &) {
    return ExecutionResult{ExecutionStop::Unsupported, 0, {}, "unmapped"};
  };
  s.install(id, std::move(reply));
  for (auto capacity : {0, 2, 4})
    EXPECT_EQ(s.call("setDataCapacity", "(I)V", id, {uint64_t(capacity)}).stop,
              ExecutionStop::Returned);
  EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 4u);
  EXPECT_TRUE(s.events.empty());
  auto result = s.call("setDataCapacity", "(I)V", id, {5});
  EXPECT_EQ(result.diagnostic, "unmapped");
  EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 4u);
  EXPECT_TRUE(s.events.empty());
}

TEST(AndroidParcelBufferTest, GrowthAdmitsStorageAndWorkBeforeReleasingOwner) {
  for (auto stop : {ExecutionStop::InstructionLimit, ExecutionStop::Timeout}) {
    BufferState s;
    const auto id = s.obtain();
    std::vector<uint8_t> arena{1, 2, 3, 4};
    s.install(id, s.borrowed(arena));
    unsigned admitted = 0;
    EXPECT_EQ(
        s.call("setDataCapacity", "(I)V", id, {64},
               [&] { return ++admitted <= 2 ? ExecutionStop::Returned : stop; })
            .stop,
        stop);
    EXPECT_TRUE(s.events.empty());
    EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 4u);
    EXPECT_EQ(s.heap.get(id)->Bytes, arena);
  }
  BufferState s(4096);
  const auto id = s.obtain();
  std::vector<uint8_t> arena{1, 2, 3, 4};
  s.install(id, s.borrowed(arena));
  EXPECT_EQ(s.call("setDataCapacity", "(I)V", id, {4096}).stop,
            ExecutionStop::HeapLimit);
  EXPECT_TRUE(s.events.empty());
  EXPECT_EQ(s.heap.get(id)->Bytes, arena);
}
TEST(AndroidParcelBufferTest, InCapacityWritesUseTheLiveNativeMemoryOwner) {
  for (bool writable : {false, true}) {
    BufferState s;
    const auto id = s.obtain();
    std::vector<uint8_t> arena{1, 2, 3, 4, 5, 6, 7, 8};
    auto reply = s.borrowed(arena);
    reply.write = [&](uint64_t offset, std::span<const uint8_t> data) {
      s.events.push_back("write");
      if (!writable)
        return ExecutionResult{
            ExecutionStop::Invalid, 0, {}, "read-only memory"};
      std::copy(data.begin(), data.end(), arena.begin() + offset);
      return ExecutionResult{};
    };
    s.install(id, std::move(reply));
    s.seek(id, 1);
    const auto result = s.call("writeInt", "(I)V", id, {0x88776655});
    EXPECT_EQ(result.stop,
              writable ? ExecutionStop::Returned : ExecutionStop::Invalid);
    EXPECT_EQ(s.events, (std::vector<std::string>{"write"}));
    EXPECT_EQ(s.call("dataPosition", "()I", id).value, writable ? 5u : 1u);
    EXPECT_EQ(arena, writable ? (std::vector<uint8_t>{1, 0x55, 0x66, 0x77, 0x88,
                                                      6, 7, 8})
                              : (std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
    s.call("recycle", "()V", id);
    EXPECT_EQ(s.events, (std::vector<std::string>{"write", "free"}));
  }
}

std::vector<uint8_t> remoteObjects() {
  // Three LP64 strong handles: two occurrences share one native proxy.
  std::vector<uint8_t> bytes(72);
  const uint32_t handles[] = {9, 10, 9};
  for (unsigned i = 0; i != 3; ++i) {
    const uint8_t header[] = {0x85, 0x2a, 0x68, 0x73, 0x7f, 1, 0, 0};
    std::copy(std::begin(header), std::end(header), bytes.begin() + i * 24);
    llvm::support::endian::write64le(bytes.data() + i * 24 + 8, handles[i]);
  }
  return bytes;
}

TEST(AndroidParcelBufferTest, BorrowedObjectReallocationPrecedesBufferAccess) {
  for (unsigned count : {0, 1, 3}) {
    SCOPED_TRACE(count);
    BufferState s;
    const auto id = s.obtain();
    auto arena = remoteObjects();
    std::vector<uint64_t> offsets;
    for (unsigned i = 0; i != count; ++i)
      offsets.push_back(i * 24);
    auto reply = s.borrowed(arena, offsets);
    reply.view = [](std::span<const uint8_t> &) {
      ADD_FAILURE() << "realloc must precede reading borrowed storage";
      return ExecutionResult{ExecutionStop::Invalid};
    };
    reply.write = [](uint64_t, std::span<const uint8_t>) {
      ADD_FAILURE() << "rejected realloc must precede writing borrowed storage";
      return ExecutionResult{ExecutionStop::Invalid};
    };
    reply.reallocate_objects = [&](uint64_t size) {
      EXPECT_EQ(size, count == 0 ? 24u : count == 1 ? 32u : 56u);
      s.events.push_back("realloc");
      return ExecutionResult{ExecutionStop::Invalid, 0, {}, "allocator fault"};
    };
    s.install(id, std::move(reply));
    auto result =
        s.call("writeStrongBinder", "(Landroid/os/IBinder;)V", id, {0});
    EXPECT_EQ(result.stop, ExecutionStop::Invalid);
    EXPECT_EQ(result.diagnostic, "allocator fault");
    EXPECT_EQ(s.events, (std::vector<std::string>{"realloc"}));
    EXPECT_EQ(s.call("dataPosition", "()I", id).value, 0u);
    EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 72u);
    EXPECT_EQ(s.heap.get(id)->Bytes, arena);
    ASSERT_EQ(s.call("recycle", "()V", id).stop, ExecutionStop::Returned);
    EXPECT_EQ(s.events, (std::vector<std::string>{"realloc", "free"}));
  }
}

TEST(AndroidParcelBufferTest,
     JavaBinderConversionPrecedesBorrowedReallocation) {
  BufferState s;
  const auto *ctor =
      libraryMethod({"Landroid/os/Binder;", "<init>", {}, "V"}, false);
  ASSERT_NE(ctor, nullptr);
  auto created = executeLibrary(s.heap, *ctor, 0, {}, Admit, false, &s.program,
                                &s.framework);
  ASSERT_EQ(created.stop, ExecutionStop::Returned);
  EXPECT_EQ(s.identities.size(), 1u); // Holder; conversion creates JavaBBinder.
  const auto id = s.obtain();
  std::vector<uint8_t> arena(24);
  auto reply = s.borrowed(arena);
  reply.reallocate_objects = [&](uint64_t size) {
    EXPECT_EQ(size, 24u);
    EXPECT_EQ(s.identities.size(), 2u);
    s.events.push_back("realloc");
    return ExecutionResult{ExecutionStop::Invalid};
  };
  s.install(id, std::move(reply));
  auto unconstructed =
      s.heap.allocate(Object::Instance, "android/os/Binder", 0);
  auto result = s.call("writeStrongBinder", "(Landroid/os/IBinder;)V", id,
                       {unconstructed->Identity});
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("original native holder"),
            std::string::npos);
  EXPECT_TRUE(s.events.empty());
  result = s.call("writeStrongBinder", "(Landroid/os/IBinder;)V", id,
                  {created.value});
  EXPECT_EQ(result.stop, ExecutionStop::Invalid);
  EXPECT_EQ(s.events, (std::vector<std::string>{"realloc"}));
  EXPECT_EQ(s.heap.get(id)->Bytes, arena);
  EXPECT_EQ(s.call("dataPosition", "()I", id).value, 0u);
}

TEST(AndroidParcelBufferTest, BorrowedObjectWritesRequireAnAllocatorService) {
  BufferState s;
  const auto id = s.obtain();
  std::vector<uint8_t> arena(24);
  s.install(id, s.borrowed(arena));
  auto result = s.call("writeStrongBinder", "(Landroid/os/IBinder;)V", id, {0});
  EXPECT_EQ(result.stop, ExecutionStop::Unsupported);
  EXPECT_NE(result.diagnostic.find("allocator is not supplied"),
            std::string::npos);
  EXPECT_TRUE(s.events.empty());
  EXPECT_EQ(s.call("dataPosition", "()I", id).value, 0u);
  ASSERT_EQ(s.call("setDataCapacity", "(I)V", id, {48}).stop,
            ExecutionStop::Returned);
  ASSERT_EQ(
      s.call("writeStrongBinder", "(Landroid/os/IBinder;)V", id, {0}).stop,
      ExecutionStop::Returned);
  EXPECT_EQ(s.events, (std::vector<std::string>{"free"}));
}

TEST(AndroidParcelBufferTest, NativeOnlyProxyReferencesAreSharedAcrossCopies) {
  BufferState s;
  const auto first = s.obtain(), second = s.obtain();
  auto arena = remoteObjects();
  s.install(first, s.borrowed(arena, {0, 24, 48}));
  s.install(second, s.borrowed(arena, {0, 24, 48}));
  ASSERT_EQ(s.call("setDataCapacity", "(I)V", first, {100}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.events,
            (std::vector<std::string>{"acquire 9", "acquire 10", "free"}));
  ASSERT_EQ(s.call("setDataCapacity", "(I)V", second, {100}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.events, (std::vector<std::string>{"acquire 9", "acquire 10",
                                                "free", "free"}));
  ASSERT_EQ(s.call("recycle", "()V", first).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.events.size(), 4u);
  ASSERT_EQ(s.call("recycle", "()V", second).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.events,
            (std::vector<std::string>{"acquire 9", "acquire 10", "free", "free",
                                      "release 10", "release 9"}));
}

TEST(AndroidParcelBufferTest,
     JavaProxyPromotionReusesTheNativeParcelReference) {
  BufferState s;
  const auto id = s.obtain();
  auto arena = remoteObjects();
  s.install(id, s.borrowed(arena, {0, 24, 48}));
  ASSERT_EQ(s.call("setDataCapacity", "(I)V", id, {100}).stop,
            ExecutionStop::Returned);
  const auto first = s.call("readStrongBinder", "()Landroid/os/IBinder;", id);
  ASSERT_EQ(first.stop, ExecutionStop::Returned) << first.diagnostic;
  ASSERT_NE(first.value, 0u);
  EXPECT_EQ(s.heap.get(first.value)->ClassName, "android/os/BinderProxy");
  EXPECT_EQ(s.events.size(), 3u); // No second native acquisition for handle 9.
  ASSERT_EQ(s.call("recycle", "()V", id).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.events, (std::vector<std::string>{"acquire 9", "acquire 10",
                                                "free", "release 10"}));
}

TEST(AndroidParcelBufferTest,
     CopyRetainsLocalAssociationAfterOriginalParcelRetires) {
  BufferState s;
  const auto *ctor =
      libraryMethod({"Landroid/os/Binder;", "<init>", {}, "V"}, false);
  ASSERT_NE(ctor, nullptr);
  auto created = executeLibrary(s.heap, *ctor, 0, {}, Admit, false, &s.program,
                                &s.framework);
  ASSERT_EQ(created.stop, ExecutionStop::Returned);
  const auto original = s.obtain(), copy = s.obtain(), later = s.obtain();
  ASSERT_EQ(s.call("writeStrongBinder", "(Landroid/os/IBinder;)V", original,
                   {created.value})
                .stop,
            ExecutionStop::Returned);
  auto arena = s.heap.get(original)->Bytes;
  s.install(copy, s.borrowed(arena, {0}));
  ASSERT_EQ(s.call("setDataCapacity", "(I)V", copy, {40}).stop,
            ExecutionStop::Returned);
  ASSERT_EQ(s.call("recycle", "()V", original).stop, ExecutionStop::Returned);
  EXPECT_EQ(s.call("readStrongBinder", "()Landroid/os/IBinder;", copy).value,
            created.value);
  ASSERT_EQ(s.call("writeStrongBinder", "(Landroid/os/IBinder;)V", later,
                   {created.value})
                .stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.heap.get(later)->Bytes, arena);
  s.call("recycle", "()V", later);
  s.call("recycle", "()V", copy);
  const auto newest = s.obtain();
  ASSERT_EQ(s.call("writeStrongBinder", "(Landroid/os/IBinder;)V", newest,
                   {created.value})
                .stop,
            ExecutionStop::Returned);
  EXPECT_NE(s.heap.get(newest)->Bytes, arena);
}

TEST(AndroidParcelBufferTest,
     UnmarshallingUsesSetDataSizeCapacityAndKeepsItsBounds) {
  BufferState s;
  const auto id = s.obtain();
  auto array = s.heap.allocateArray("[B", 3);
  array->Bytes = {9, 8, 7};
  ASSERT_EQ(s.call("unmarshall", "([BII)V", id, {array->Identity, 0, 3}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 10u);
  EXPECT_EQ(s.heap.get(id)->Bytes, (std::vector<uint8_t>{9, 8, 7, 0}));
  ASSERT_EQ(s.call("setDataCapacity", "(I)V", id, {40}).stop,
            ExecutionStop::Returned);
  ASSERT_EQ(s.call("unmarshall", "([BII)V", id, {array->Identity, 0, 0}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 40u);
  EXPECT_EQ(s.call("dataSize", "()I", id).value, 0u);
  std::vector<uint8_t> arena{1, 2, 3, 4};
  s.install(id, s.borrowed(arena));
  ASSERT_EQ(s.call("unmarshall", "([BII)V", id, {array->Identity, 0, 3}).stop,
            ExecutionStop::Returned);
  EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 10u);
  EXPECT_EQ(s.events, (std::vector<std::string>{"free"}));
  EXPECT_EQ(s.heap.get(id)->Bytes, (std::vector<uint8_t>{9, 8, 7, 0}));
}

TEST(AndroidParcelBufferTest, UnsupportedObjectsDoNotReleaseTheBorrowedArena) {
  for (auto type : {0x77682a85u, 0x66642a85u, 0x73622a85u, 0x73682a85u}) {
    SCOPED_TRACE(type);
    BufferState s;
    const auto id = s.obtain();
    std::vector<uint8_t> arena(24);
    llvm::support::endian::write32le(arena.data(), type);
    // Foreign local cookie; the other records include an uncached handle zero.
    if (type == 0x73622a85u) {
      llvm::support::endian::write64le(arena.data() + 8, 0x1000);
      llvm::support::endian::write64le(arena.data() + 16, 0x1008);
    }
    s.install(id, s.borrowed(arena, {0}));
    EXPECT_EQ(s.call("setDataCapacity", "(I)V", id, {25}).stop,
              ExecutionStop::Unsupported);
    EXPECT_EQ(s.call("dataCapacity", "()I", id).value, 24u);
    EXPECT_TRUE(s.events.empty());
    EXPECT_EQ(s.heap.get(id)->Bytes, arena);
  }
}

TEST(AndroidParcelBufferTest, ReservedHeapStorageIsChargedOnceAndBoundsGrowth) {
  Heap heap(4096);
  auto object = heap.allocate(Object::Instance, "test/Buffer", 0);
  ASSERT_NE(object, nullptr);
  ASSERT_TRUE(heap.reserveBytes(*object, 3000));
  EXPECT_TRUE(object->Bytes.empty());
  ASSERT_TRUE(heap.resizeBytes(*object, 2999));
  ASSERT_TRUE(heap.resizeBytes(*object, 0));
  ASSERT_TRUE(heap.resizeBytes(*object, 3000));
  EXPECT_FALSE(heap.reserveBytes(*object, 4096));
  EXPECT_EQ(object->Bytes.size(), 3000u);
  EXPECT_EQ(object->ByteReserve, 3000u);
}

} // namespace
} // namespace neverd::mobile::dalvik
