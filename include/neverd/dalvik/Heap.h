//===- Heap.h - Shared workload Java object storage -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include "neverd/dalvik/Limits.h"
#include "neverd/dalvik/Model.h"

#include <memory>
#include <optional>
#include <span>

namespace neverd::mobile::dalvik {
// Identities are workload-local tokens, never host or guest addresses. The
// owner retains bounded objects until teardown; weak edges admit cycles without
// leaking host ownership. JNI references and DEX registers use this same heap.
struct Object {
  enum Kind { Class, String, PrimitiveArray, ObjectArray, Throwable, Instance };
  Kind Type;
  uint32_t Identity = 0;
  std::string ClassName;
  std::vector<uint8_t> Bytes;
  std::vector<std::weak_ptr<Object>> Elements;
  std::map<const Field *, uint64_t> Fields;
  uint64_t Length = 0;
  unsigned ElementSize = 0;
  bool Initialized = true;
  uint32_t Message =
      0; // Throwable detailMessage identity (no host stack trace).
  struct MapEntry {
    uint32_t Key, Value;
  };
  // Known HashMap contents. Raw AllocObject and an existing process map with
  // unsupplied contents remain unknown. Entries retain workload identities,
  // permitting null values and cycles.
  std::optional<std::vector<MapEntry>> MapEntries;
};
class Heap {
public:
  explicit Heap(
      uint64_t byte_limit = ::neverd::limits::dalvik::kHeapByteLimit,
      uint32_t object_limit = ::neverd::limits::dalvik::kHeapObjectLimit);
  // Returns null on the finite model budget; this is not a claim about a real
  // Java VM's allocation failure or collection policy. extra accounts for maps.
  std::shared_ptr<Object> allocate(Object::Kind kind, std::string_view name,
                                   uint64_t bytes, uint64_t extra = 0);
  // Callers validate the canonical array descriptor and nonnegative Java
  // length. Allocates zero primitive bytes or null reference elements.
  std::shared_ptr<Object> allocateArray(std::string_view type, uint32_t length);
  std::shared_ptr<Object> get(uint64_t identity) const;
  // Resize retained storage only after admitting its growth to the heap budget.
  bool resizeBytes(Object &object, uint64_t size);
  bool replaceBytes(Object &object, std::vector<uint8_t> bytes);
  bool replaceMapEntries(Object &object, std::vector<Object::MapEntry> entries);
  std::shared_ptr<Object> classObject(std::string_view name);
  uint32_t classObjectIdentity(std::string_view name) const;
  std::shared_ptr<Object> stringConstant(std::string_view key,
                                         std::vector<uint8_t> bytes);
  std::shared_ptr<Object> intern(std::shared_ptr<Object> string);
  std::shared_ptr<Object> literal(std::string_view utf8);

private:
  uint64_t limit, used = 0;
  uint32_t count_limit;
  std::vector<std::shared_ptr<Object>> objects;
  std::map<std::string, std::shared_ptr<Object>> classes, constants;
  std::map<std::vector<uint8_t>, std::shared_ptr<Object>> interned;
};
bool referenceType(std::string_view type);
std::string objectDescriptor(const Object &object);
// The finite platform hierarchy and arrays. Program owns supplied declarations
// and extends assignability with their explicit superclass/interface edges.
bool platformClass(std::string_view name);
std::string_view javaParent(std::string_view name);
// Canonical UTF-8/WTF-8 from the shared DEX reader -> exact UTF-16LE units.
std::vector<uint8_t> javaStringBytes(std::string_view text);
// ART JNI UTF-8: NUL is C0 80, paired surrogates use four bytes, isolated
// surrogates retain three bytes. Input is the heap's validated UTF-16LE
// storage.
std::vector<uint8_t> artStringUTF8(std::span<const uint8_t> utf16);
bool referenceAssignable(std::string_view source, std::string_view destination);
} // namespace neverd::mobile::dalvik
