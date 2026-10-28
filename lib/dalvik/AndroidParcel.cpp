//===- AndroidParcel.cpp - Shared Java Parcel data and lifecycle ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 owned Parcels over bounded heap bytes, shared by DEX and JNI.
//===----------------------------------------------------------------------===//

#include "AndroidFramework.h"

#include "neverd/dalvik/Limits.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

ExecutionResult AndroidFramework::parcel(Heap &heap,
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
    auto entry = parcels.find(receiver);
    if (entry == parcels.end())
      return unsupported("Android Parcel owned native state is not supplied");
    auto &state = entry->second;
    if (state.recycled)
      return unsupported("Android Parcel access after recycle is unsupported");
    auto object = heap.get(receiver); // Library validated the receiver.
    auto &bytes = object->Bytes;
    auto &position = state.position;
    if (name == "recycle") {
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
    if (name == "dataAvail")
      return returned(bytes.size() > position ? bytes.size() - position : 0);
    if (name == "setDataPosition") {
      const uint32_t next = arguments[0];
      if (next > INT32_MAX)
        return unsupported(
            "Android Parcel negative position requires native abort");
      position = next;
      return returned();
    }
    // No allocator bytes are invented for a cursor beyond initialized data.
    auto write = [&](std::span<const uint8_t> input, size_t zeros = 0) {
      if (position > bytes.size())
        throw Stop{
            unsupported("Android Parcel write crosses an uninitialized gap")};
      const uint64_t end = uint64_t(position) + input.size() + zeros;
      if (end > INT32_MAX)
        throw Stop{
            unsupported("Android Parcel data exceeds the native size range")};
      work(input.size() + zeros);
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
      if (uint64_t(position) + size > bytes.size())
        return 0; // API 28 short scalar reads do not advance the cursor.
      const auto *data = bytes.data() + position;
      position += size;
      return size == 4 ? llvm::support::endian::read32le(data)
                       : llvm::support::endian::read64le(data);
    };
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
          uint64_t(position) + padded > bytes.size())
        return returned();
      work(raw);
      const auto start = position;
      position += padded;
      auto string =
          heap.allocate(Object::String, "java/lang/String", length * 2);
      if (!string)
        return storage();
      // API 28 reads the declared UTF-16 units without checking the terminator.
      std::copy_n(bytes.begin() + start, length * 2, string->Bytes.begin());
      string->Length = length;
      return returned(string->Identity);
    }
    if (name == "marshall") {
      if (position > bytes.size())
        return unsupported(
            "Android Parcel marshall requires uninitialized bytes");
      work(bytes.size());
      auto array = heap.allocateArray("[B", bytes.size());
      if (!array)
        return storage();
      std::copy(bytes.begin(), bytes.end(), array->Bytes.begin());
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
      work(length);
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
