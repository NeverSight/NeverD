//===- AndroidEventLog.cpp - API 28 event payload encoding ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// EventLogHelper overloads and liblog truncation over shared Java values.
/// Source contracts: docs/android-log-reader.md.
//===----------------------------------------------------------------------===//
#include "AndroidEventLog.h"

#include "AndroidFramework.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
constexpr unsigned DataBytes =
    ::neverd::limits::dalvik::kAndroidEventLogPayloadBytes - 4;
}

void AndroidEventLog::scalar(uint8_t type, uint64_t value) {
  const unsigned bytes = type == 1 ? 8 : 4;
  if (failed())
    return;
  if (position + 1 + bytes > DataBytes) {
    error = -5; // Android EIO; retained even after the transport write.
    return;
  }
  ++count;
  storage[position++] = type;
  for (unsigned i = 0; i < bytes; ++i)
    storage[position++] = uint8_t(value >> (8 * i));
}

void AndroidEventLog::string(std::span<const uint8_t> bytes) {
  if (failed())
    return;
  unsigned length = std::min<size_t>(bytes.size(), DataBytes);
  const unsigned needed = 5 + length;
  if (position + needed > DataBytes) {
    if (position + 5 >= DataBytes) {
      error = -5;
      return;
    }
    length = DataBytes - position - 5;
  }
  ++count;
  storage[position] = 2;
  llvm::support::endian::write32le(storage.data() + position + 1, length);
  std::copy_n(bytes.begin(), length, storage.begin() + position + 5);
  // API 28 advances by the original size, including after string truncation.
  position += needed;
}

ExecutionResult AndroidEventLog::string(const Object *object,
                                        const AdmitInstruction &admit) {
  if (!object) {
    const uint8_t text[] = {'N', 'U', 'L', 'L'};
    string(text);
  } else {
    for (size_t unit = 0; unit < object->Bytes.size(); unit += 2) {
      const auto stop = admit();
      if (stop != ExecutionStop::Returned)
        return {stop, 0, {}, "Android event string work budget exhausted"};
    }
    string(artStringUTF8(object->Bytes));
  }
  return {ExecutionStop::Returned};
}

ExecutionResult AndroidEventLog::write(const RuntimeServices &services,
                                       const AdmitInstruction &admit) {
  if (!services.event_log_write)
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android event log write service is not supplied"};
  const auto stop = admit();
  if (stop != ExecutionStop::Returned)
    return {stop, 0, {}, "Android event write work budget exhausted"};
  storage[1] = uint8_t(count);
  const unsigned offset = count <= 1 ? 2 : 0;
  const unsigned length = std::min(position - offset, DataBytes);
  std::vector<uint8_t> payload(4 + length);
  llvm::support::endian::write32le(payload.data(), tag);
  std::copy_n(storage.begin() + offset, length, payload.begin() + 4);
  auto result = services.event_log_write(payload);
  if (result.stop != ExecutionStop::Returned)
    return result;
  if (result.value > UINT32_MAX)
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android event log result exceeds its Java int carrier"};
  if (error)
    result.value = uint32_t(error);
  return result;
}

ExecutionResult AndroidFramework::eventLog(Heap &heap, const Program &program,
                                           const LibraryMethod &method,
                                           std::span<const uint64_t> arguments,
                                           const AdmitInstruction &admit) {
  AndroidEventLog log{uint32_t(arguments[0])};
  const auto parameters = prototype(method.Signature).first;
  const auto &type = parameters[1];
  if (type == "I" || type == "J" || type == "F") {
    log.scalar(type == "I" ? 0 : type == "J" ? 1 : 4, arguments[1]);
  } else if (type == "Ljava/lang/String;") {
    auto result = log.string(heap.get(arguments[1]).get(), admit);
    if (result.stop != ExecutionStop::Returned)
      return result;
  } else if (!arguments[1]) {
    const uint8_t text[] = {'[', 'N', 'U', 'L', 'L', ']'};
    log.string(text);
  } else {
    const auto array = heap.get(arguments[1]);
    // EventLogHelper only visits the first 255 elements, stopping on the
    // first liblog encoding error. Nested arrays are not recursively encoded.
    for (size_t i = 0; i < array->Elements.size() && i < 255 && !log.failed();
         ++i) {
      const auto stop = admit();
      if (stop != ExecutionStop::Returned)
        return {stop, 0, {}, "Android event array work budget exhausted"};
      const auto item = array->Elements[i].lock();
      if (!item || item->Type == Object::String) {
        auto result = log.string(item.get(), admit);
        if (result.stop != ExecutionStop::Returned)
          return result;
        continue;
      }
      const char *valueType = item->ClassName == "java/lang/Integer" ? "I"
                              : item->ClassName == "java/lang/Long"  ? "J"
                              : item->ClassName == "java/lang/Float" ? "F"
                                                                     : nullptr;
      if (!valueType)
        return {ExecutionStop::Thrown, 0, "java/lang/IllegalArgumentException",
                "Invalid payload item type"};
      const auto *field = program.findField(
          {objectDescriptor(*item), "value", valueType}, false);
      if (!field || !item->Fields.contains(field))
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Android event boxed value field is not supplied"};
      log.scalar(*valueType == 'I'   ? 0
                 : *valueType == 'J' ? 1
                                     : 4,
                 item->Fields.at(field));
    }
  }
  return log.write(services, admit);
}
} // namespace neverd::mobile::dalvik
