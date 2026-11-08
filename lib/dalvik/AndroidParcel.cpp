//===- AndroidParcel.cpp - Shared Java Parcel data and lifecycle ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 owned and borrowed Parcels over bounded storage, shared by DEX/JNI.
//===----------------------------------------------------------------------===//

#include "AndroidFramework.h"

#include "neverd/dalvik/Limits.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace neverd::mobile::dalvik {
namespace {
// API 28 libbinder adds PENALTY_GATHER to the wire policy, not local state.
constexpr uint32_t kStrictModePenaltyGather = 0x00400000;
struct Stop {
  ExecutionResult result;
};
ExecutionResult unsupported(std::string text) {
  return {ExecutionStop::Unsupported, 0, {}, std::move(text)};
}
} // namespace

ExecutionResult AndroidFramework::parcelStatus(uint64_t receiver) const {
  const auto entry = parcels.find(receiver);
  if (entry == parcels.end())
    return unsupported("Android Parcel owned native state is not supplied");
  if (entry->second.recycled)
    return unsupported("Android Parcel access after recycle is unsupported");
  return {};
}

ExecutionResult AndroidFramework::readParcelBuffer(Heap &heap,
                                                   uint64_t receiver) {
  const auto &state = parcelState(receiver);
  if (!state.view)
    return {};
  std::span<const uint8_t> bytes;
  auto result = state.view(bytes);
  if (result.stop != ExecutionStop::Returned)
    return result;
  auto &storage = parcelStorage(heap, receiver).Bytes;
  if (bytes.size() != storage.size())
    return unsupported("Android Parcel Binder buffer size changed");
  std::copy(bytes.begin(), bytes.end(), storage.begin());
  return {};
}

AndroidFramework::ParcelState &
AndroidFramework::parcelState(uint64_t receiver) {
  auto &wrapper = parcels.at(receiver);
  return wrapper.native ? *wrapper.native : wrapper;
}

Object &AndroidFramework::parcelStorage(Heap &heap, uint64_t receiver) {
  auto &state = parcelState(receiver);
  return state.storage ? *state.storage : *heap.get(receiver);
}

ExecutionResult
AndroidFramework::wrapNativeParcel(Heap &heap,
                                   std::shared_ptr<ParcelState> state,
                                   const AdmitInstruction &admit) {
  auto stop = admit();
  if (stop != ExecutionStop::Returned)
    return {stop};
  for (auto &slot : native_parcel_pool)
    if (slot) {
      const auto id = std::exchange(slot, 0);
      auto &wrapper = parcels.at(id);
      wrapper.recycled = false;
      wrapper.native = std::move(state);
      return {ExecutionStop::Returned, id};
    }
  auto object = heap.allocate(
      Object::Instance, "android/os/Parcel", 0,
      ::neverd::limits::dalvik::kFrameworkFieldStateOverheadBytes);
  if (!object)
    return {ExecutionStop::HeapLimit,
            0,
            {},
            "Android native Parcel wrapper storage budget exhausted"};
  parcels[object->Identity].native = std::move(state);
  return {ExecutionStop::Returned, object->Identity};
}

ExecutionResult AndroidFramework::parcel(Heap &heap, const Program &program,
                                         const LibraryMethod &method,
                                         uint64_t receiver,
                                         std::span<const uint64_t> arguments,
                                         const AdmitInstruction &admit) {
  const std::string_view name = method.Name;
  auto returned = [](uint64_t value = 0) {
    return ExecutionResult{ExecutionStop::Returned, value};
  };
  auto storage = []() -> ExecutionResult {
    throw Stop{{ExecutionStop::HeapLimit,
                0,
                {},
                "Android Parcel storage budget exhausted"}};
  };
  auto work = [&](size_t bytes) {
    for (size_t i = 0; i < bytes; i += 2) {
      const auto stop = admit();
      if (stop != ExecutionStop::Returned)
        throw Stop{{stop, 0, {}, "Android Parcel work budget exhausted"}};
    }
  };
  try {
    if (name == "obtain") {
      for (auto &slot : parcel_pool)
        if (slot) {
          const auto id = std::exchange(slot, 0);
          parcels.at(id).recycled = false;
          return returned(id);
        }
      auto object = heap.allocate(
          Object::Instance, "android/os/Parcel", 0,
          ::neverd::limits::dalvik::kFrameworkFieldStateOverheadBytes);
      if (!object)
        return storage();
      parcels.emplace(object->Identity, ParcelState{});
      return returned(object->Identity);
    }
    auto status = parcelStatus(receiver);
    if (status.stop != ExecutionStop::Returned)
      return status;
    auto &wrapper = parcels.at(receiver);
    if (name == "recycle" && wrapper.native) {
      wrapper.native.reset();
      wrapper.recycled = true;
      for (auto &slot : native_parcel_pool)
        if (!slot) {
          slot = receiver;
          break;
        }
      return returned();
    }
    auto &state = parcelState(receiver);
    auto *object = &parcelStorage(heap, receiver);
    auto &bytes = object->Bytes;
    auto &position = state.position;
    if (name == "recycle") {
      auto released = releaseParcelObjects(state, bytes);
      if (released.stop != ExecutionStop::Returned)
        return released;
      if (state.release)
        state.release();
      state.release = {};
      state.view = {};
      state.write = {};
      state.reallocate_objects = {};
      state.capacity = 0;
      state.error = 0;
      state.objects.clear();
      state.owns_objects = true;
      state.objects_sorted = false;
      state.object_hint = 0;
      state.fd.reset();
      heap.resizeBytes(*object, 0);
      position = 0;
      state.recycled = true;
      for (auto &slot : parcel_pool)
        if (!slot) {
          slot = receiver;
          break;
        }
      return returned();
    }
    if (name == "dataPosition")
      return returned(position);
    if (name == "dataSize")
      return returned(std::max<size_t>(bytes.size(), position));
    if (name == "dataCapacity")
      return returned(state.capacity);
    if (name == "dataAvail")
      return returned(bytes.size() > position ? bytes.size() - position : 0);
    if (name == "setDataPosition") {
      const uint32_t next = arguments[0];
      if (next > INT32_MAX)
        return unsupported(
            "Android Parcel negative position requires native abort");
      position = next;
      state.object_hint = 0;
      state.objects_sorted = false;
      return returned();
    }
    auto capacity = [&](uint64_t desired) {
      if (desired > INT32_MAX)
        throw Stop{
            {ExecutionStop::Thrown, 0, "java/lang/IllegalArgumentException"}};
      if (desired <= state.capacity)
        return;
      // Only initialized data is copied; reserved allocator bytes never become
      // observable. A borrowed buffer must retain its references before free.
      if (state.release) {
        work(bytes.size());
        auto read = readParcelBuffer(heap, receiver);
        if (read.stop != ExecutionStop::Returned)
          throw Stop{std::move(read)};
      }
      if (!heap.reserveBytes(*object, desired))
        storage();
      if (state.release) {
        auto acquired = acquireParcelObjects(state, bytes);
        if (acquired.stop != ExecutionStop::Returned)
          throw Stop{std::move(acquired)};
        state.release();
        state.release = {};
        state.view = {};
        state.write = {};
        state.reallocate_objects = {};
        state.object_hint = 0;
        state.objects_sorted = false;
      }
      state.capacity = desired;
    };
    if (name == "setDataCapacity") {
      capacity(uint32_t(arguments[0]));
      return returned();
    }
    if (name == "marshall" && !state.objects.empty()) {
      const auto exception =
          heap.allocate(Object::Throwable, "java/lang/RuntimeException", 0);
      if (!exception)
        return storage();
      auto message = stringValue(
          heap, "Tried to marshall a Parcel that contained Binder objects.",
          admit);
      if (message.stop != ExecutionStop::Returned)
        return message;
      exception->Message = message.value;
      ExecutionResult result{ExecutionStop::Thrown, 0,
                             "java/lang/RuntimeException"};
      result.throwable = exception->Identity;
      return result;
    }
    std::span<const uint8_t> readable = bytes;
    if (name != "unmarshall" && !name.starts_with("write") && state.view) {
      auto result = state.view(readable);
      if (result.stop != ExecutionStop::Returned)
        return result;
      if (readable.size() != bytes.size())
        return unsupported("Android Parcel Binder buffer size changed");
    }
    auto protectedRange = [&](uint64_t start, uint64_t size) {
      // validateReadData sorts the actual table lazily; transport and native
      // release subsequently see that same order (including duplicate offsets).
      if (!state.objects_sorted && state.objects.size() > 1) {
        if (!std::is_sorted(state.objects.begin(), state.objects.end())) {
          std::sort(state.objects.begin(), state.objects.end());
          state.object_hint = 0;
        }
        state.objects_sorted = true;
      }
      auto next = state.object_hint;
      while (next < state.objects.size() &&
             state.objects[next] < start + size) {
        if (start < state.objects[next] + 24)
          return true;
        ++next;
      }
      state.object_hint = next;
      return false;
    };
    // No allocator bytes are invented for a cursor beyond initialized data.
    auto write = [&](std::span<const uint8_t> input, size_t zeros = 0) {
      if (position > bytes.size())
        throw Stop{
            unsupported("Android Parcel write crosses an uninitialized gap")};
      const uint64_t end = uint64_t(position) + input.size() + zeros;
      if (end > INT32_MAX)
        throw Stop{
            unsupported("Android Parcel data exceeds the native size range")};
      // Native writes preserve offsets and the old strong acquisitions. A
      // later release rereads whatever object bytes now occupy those slots.
      work(input.size() + zeros);
      if (end > state.capacity)
        capacity(((uint64_t(bytes.size()) + input.size() + zeros) * 3) / 2);
      if (state.release) {
        if (!state.write)
          throw Stop{unsupported(
              "Android Parcel borrowed write service is not supplied")};
        // In-capacity writes do not detach mOwner. The native memory owner
        // enforces the actual mapping permissions and allocation identity.
        auto written = state.write(position, input);
        if (written.stop != ExecutionStop::Returned)
          throw Stop{std::move(written)};
        if (zeros) {
          const std::array<uint8_t, 4> padding{};
          written = state.write(position + input.size(),
                                std::span(padding).first(zeros));
          if (written.stop != ExecutionStop::Returned)
            throw Stop{std::move(written)};
        }
      }
      if (end > bytes.size() && !heap.resizeBytes(*object, end))
        storage();
      std::copy(input.begin(), input.end(), bytes.begin() + position);
      std::fill(bytes.begin() + position + input.size(), bytes.begin() + end,
                0);
      position = end;
    };
    auto integer = [&](uint64_t value, size_t size) {
      std::array<uint8_t, 8> encoded;
      llvm::support::endian::write64le(encoded.data(), value);
      write(std::span(encoded).first(size));
    };
    auto readInteger = [&](size_t size) -> uint64_t {
      if (uint64_t(position) + size > readable.size())
        return 0; // API 28 short scalar reads do not advance the cursor.
      const auto *data = readable.data() + position;
      const bool denied = protectedRange(position, size);
      position += size;
      if (denied)
        return 0; // validateReadData fails after readAligned advances.
      return size == 4 ? llvm::support::endian::read32le(data)
                       : llvm::support::endian::read64le(data);
    };
    if (name == "readStrongBinder") {
      if (uint64_t(position) + 24 > readable.size())
        return returned();
      const auto start = position;
      position +=
          24; // readObject consumes a complete record before validation.
      const auto *data = readable.data() + start;
      const auto type = llvm::support::endian::read32le(data);
      const auto pointer = llvm::support::endian::read64le(data + 8);
      const auto cookie = llvm::support::endian::read64le(data + 16);
      if (pointer || cookie) {
        // readObject searches in table order using its hint; unlike scalar
        // reads, it does not sort a table produced by non-append writes.
        if (state.objects.empty())
          return returned();
        auto index = std::min(state.object_hint, state.objects.size() - 1);
        while (index + 1 < state.objects.size() && state.objects[index] < start)
          ++index;
        while (index && state.objects[index] > start)
          --index;
        if (state.objects[index] != start)
          return returned();
        state.object_hint = index + 1;
      }
      if (type == 0x73622a85) {
        if (!cookie)
          return returned();
        auto node = local_binders.find(cookie);
        if (node != local_binders.end() && node->second.alive()) {
          auto valid = services.binder_identity(
              BinderIdentityOperation::Validate, node->second.pointer);
          return valid.stop == ExecutionStop::Returned
                     ? returned(node->second.object)
                     : valid;
        }
        return unsupported("Android local Binder object requires a native Java "
                           "object association");
      }
      if (type != 0x73682a85)
        return returned(); // Native no-argument readStrongBinder ignores
                           // BAD_TYPE.
      if (!state.fd)
        return unsupported(
            "Android Parcel Binder handle has no native connection");
      return binderHandle(heap, *state.fd, uint32_t(pointer), admit);
    }
    if (name == "writeStrongBinder") {
      std::array<uint8_t, 24> encoded{};
      uint32_t type = 0x73622a85;
      uint64_t pointer = 0, cookie = 0;
      std::optional<uint32_t> fd;
      if (const auto binder = heap.get(arguments[0])) {
        if (program.assignable(objectDescriptor(*binder),
                               "Landroid/os/Binder;")) {
          auto local = localBinder(heap, program, arguments[0]);
          if (local.stop != ExecutionStop::Returned)
            return local;
          cookie = local.value;
          pointer = local_binders.find(cookie)->second.pointer;
        } else if (binder->ClassName == "android/os/BinderProxy") {
          auto binding = binder_bindings.find(arguments[0]);
          if (binding == binder_bindings.end())
            return unsupported(
                "Android Parcel BinderProxy has no native binding");
          fd = binding->second.fd;
          if (state.fd && state.fd != fd)
            return unsupported(
                "Android Parcel Binder objects require one native connection");
          type = 0x73682a85;
          pointer = binding->second.handle;
        } else
          return unsupported("Android custom IBinder conversion requires "
                             "native warning logging");
      }
      if (state.release &&
          uint64_t(position) + encoded.size() <= state.capacity) {
        if (!state.reallocate_objects)
          return unsupported(
              "Android Parcel borrowed object-table allocator is not supplied");
        // ipcSetDataReference makes mObjectsCapacity equal mObjectsSize, even
        // for an empty table. JNI converts the Java Binder before writeObject
        // attempts realloc; the allocator failure precedes data/metadata
        // writes.
        const auto allocation = ((state.objects.size() + 2) * 3 / 2) * 8;
        return state.reallocate_objects(allocation);
      }
      if (!state.owns_objects && !state.release && !state.objects.empty())
        return unsupported(
            "Android Parcel object ownership service is not supplied");
      llvm::support::endian::write32le(encoded.data(), type);
      // API 28's cold IPCThreadState allows background scheduling: nice 19
      // and ACCEPTS_FDS. This is a policy word, not an object ownership flag.
      llvm::support::endian::write32le(encoded.data() + 4, 0x113);
      llvm::support::endian::write64le(encoded.data() + 8, pointer);
      llvm::support::endian::write64le(encoded.data() + 16, cookie);
      const auto offset = position;
      write(encoded);
      if (pointer) {
        state.objects.push_back(offset);
        state.owns_objects = true;
      }
      if (cookie)
        ++local_binders.find(cookie)->second.references;
      if (fd) {
        state.fd = fd;
        if (pointer) {
          ++binder_parcel_handles[std::make_pair(*fd, uint32_t(pointer))];
        }
      }
      return returned();
    }
    if (name == "writeNoException") {
      // Supported StrictMode paths retain the initial null violation list.
      integer(0, 4);
      return returned();
    }
    if (name == "writeInt" || name == "writeLong") {
      integer(arguments[0], name == "writeInt" ? 4 : 8);
      return returned();
    }
    if (name == "readInt" || name == "readLong")
      return returned(readInteger(name == "readInt" ? 4 : 8));
    if (name == "writeString" || name == "writeInterfaceToken") {
      const auto string = heap.get(arguments[0]);
      if (name == "writeInterfaceToken") {
        if (!string)
          return unsupported("Android Parcel interface token requires a "
                             "nonnull native String");
        auto policy = binderThreadPolicy();
        if (policy.stop != ExecutionStop::Returned)
          return policy;
        integer(uint32_t(policy.value) | kStrictModePenaltyGather, 4);
      }
      integer(string ? string->Length : UINT32_MAX, 4);
      if (string) {
        const auto size = string->Bytes.size();
        // UTF-16 terminator followed by zero padding to a four-byte boundary.
        write(string->Bytes, ((size + 2 + 3) & ~size_t(3)) - size);
      }
      return returned();
    }
    if (name == "readString") {
      const uint32_t length = readInteger(4);
      const uint64_t raw = (uint64_t(length) + 1) * 2;
      const uint64_t padded = (raw + 3) & ~uint64_t(3);
      if (length >= INT32_MAX || raw > INT32_MAX ||
          uint64_t(position) + padded > readable.size())
        return returned();
      work(raw);
      const auto start = position;
      position += padded;
      if (protectedRange(start, padded))
        return returned();
      auto string =
          heap.allocate(Object::String, "java/lang/String", length * 2);
      if (!string)
        return storage();
      // API 28 reads the declared UTF-16 units without checking the terminator.
      std::copy_n(readable.begin() + start, length * 2, string->Bytes.begin());
      string->Length = length;
      return returned(string->Identity);
    }
    if (name == "marshall") {
      if (position > readable.size())
        return unsupported(
            "Android Parcel marshall requires uninitialized bytes");
      work(readable.size());
      auto array = heap.allocateArray("[B", readable.size());
      if (!array)
        return storage();
      std::copy(readable.begin(), readable.end(), array->Bytes.begin());
      return returned(array->Identity);
    }
    if (name == "unmarshall") {
      const uint32_t offset = arguments[1], length = arguments[2];
      if (length > INT32_MAX)
        return returned(); // Native API 28 returns before touching the array.
      const auto array = heap.get(arguments[0]);
      if (!array || offset > array->Bytes.size() ||
          length > array->Bytes.size() - offset)
        return unsupported(
            "Android Parcel unmarshall requires a valid native array range");
      const uint64_t padded = (uint64_t(length) + 3) & ~uint64_t(3);
      // JNI uses setDataSize, which preserves entries whose offsets remain
      // in range. Overwriting those records is not an ownership reset.
      if (std::any_of(state.objects.begin(), state.objects.end(),
                      [&](uint64_t object) { return object < length; }))
        return unsupported("Android Parcel unmarshall overwrites retained "
                           "Binder object metadata");
      work(length);
      if (state.release && length) {
        work(std::min<size_t>(bytes.size(), length));
        auto read = readParcelBuffer(heap, receiver);
        if (read.stop != ExecutionStop::Returned)
          return read;
      }
      uint64_t nextCapacity =
          state.release ? length : std::max(state.capacity, length);
      if (padded > nextCapacity)
        nextCapacity = (uint64_t(length) + padded) * 3 / 2;
      if (nextCapacity > INT32_MAX)
        return unsupported("Android Parcel data exceeds the native size range");
      if (!heap.reserveBytes(*object, nextCapacity))
        return storage();
      auto released = releaseParcelObjects(state, bytes, false);
      if (released.stop != ExecutionStop::Returned)
        return released;
      if (state.release)
        state.release();
      state.release = {};
      state.view = {};
      state.write = {};
      state.reallocate_objects = {};
      state.error = 0;
      state.objects.clear();
      state.owns_objects = true;
      state.objects_sorted = false;
      state.object_hint = 0;
      state.fd.reset();
      state.capacity = nextCapacity;
      if (!heap.resizeBytes(*object, padded))
        return storage();
      std::copy_n(array->Bytes.begin() + offset, length, bytes.begin());
      std::fill(bytes.begin() + length, bytes.end(), 0);
      position = padded;
      return returned();
    }
    return unsupported("Android Parcel method is not modeled");
  } catch (Stop &stop) {
    return std::move(stop.result);
  }
}
} // namespace neverd::mobile::dalvik
