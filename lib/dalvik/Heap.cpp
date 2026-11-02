//===- Heap.cpp - Bounded Java object identity and array types ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/dalvik/Heap.h"

#include <cassert>

namespace neverd::mobile::dalvik {
Heap::Heap(uint64_t byte_limit, uint32_t object_limit)
    : limit(byte_limit), count_limit(object_limit) {}
std::shared_ptr<Object> Heap::allocate(Object::Kind kind, std::string_view name,
                                       uint64_t bytes, uint64_t extra) {
  const uint64_t overhead = sizeof(Object) + name.size() + 64;
  if (objects.size() >= count_limit || bytes > limit || extra > limit - bytes ||
      overhead > limit - bytes - extra ||
      used > limit - bytes - extra - overhead)
    return {};
  auto object = std::make_shared<Object>();
  object->Type = kind;
  object->ClassName = name;
  object->Identity = uint32_t(objects.size()) + 1;
  object->Bytes.resize(bytes);
  objects.push_back(object);
  used += bytes + extra + overhead;
  return object;
}
std::shared_ptr<Object> Heap::allocateArray(std::string_view type,
                                            uint32_t length) {
  assert(type.starts_with('[') && length <= INT32_MAX);
  const auto component = type.substr(1);
  const bool refs = referenceType(component);
  assert(refs || (component.size() == 1 &&
                  std::string_view("ZBCSIJFD").find(component[0]) !=
                      std::string_view::npos));
  const unsigned width = refs ? sizeof(std::weak_ptr<Object>)
                         : component == "J" || component == "D" ? 8
                         : component == "I" || component == "F" ? 4
                         : component == "C" || component == "S" ? 2
                                                                : 1;
  const uint64_t bytes = uint64_t(length) * width;
  auto object = allocate(refs ? Object::ObjectArray : Object::PrimitiveArray,
                         type, refs ? 0 : bytes, refs ? bytes : 0);
  if (object) {
    object->Length = length;
    object->ElementSize = width;
    if (refs)
      object->Elements.resize(length);
  }
  return object;
}
std::shared_ptr<Object> Heap::get(uint64_t id) const {
  return id && id <= objects.size() ? objects[id - 1] : nullptr;
}
bool Heap::replaceBytes(Object &object, std::vector<uint8_t> bytes) {
  const uint64_t growth = bytes.size() > object.Bytes.size()
                              ? bytes.size() - object.Bytes.size()
                              : 0;
  if (growth > limit - used)
    return false;
  used += growth; // Conservative retained capacity; no model GC/reclamation.
  object.Bytes = std::move(bytes);
  object.Length = object.Bytes.size() / 2;
  return true;
}
bool Heap::resizeBytes(Object &object, uint64_t size) {
  const uint64_t growth =
      size > object.Bytes.size() ? size - object.Bytes.size() : 0;
  if (growth > limit - used)
    return false;
  used += growth; // Same conservative retained-capacity policy as replacement.
  object.Bytes.resize(size);
  object.Length = size / 2;
  return true;
}
bool Heap::replaceMapEntries(Object &object,
                             std::vector<Object::MapEntry> entries) {
  const size_t previous = object.MapEntries ? object.MapEntries->size() : 0;
  const uint64_t growth =
      entries.size() > previous
          ? (entries.size() - previous) * sizeof(Object::MapEntry)
          : 0;
  if (growth > limit - used)
    return false;
  used += growth; // No model reclamation, including after remove/clear.
  object.MapEntries = std::move(entries);
  return true;
}
std::shared_ptr<Object> Heap::classObject(std::string_view name) {
  auto key = std::string(name);
  if (auto it = classes.find(key); it != classes.end())
    return it->second;
  auto value = allocate(Object::Class, name, 0, name.size() + 64);
  if (value)
    classes.emplace(std::move(key), value);
  return value;
}
uint32_t Heap::classObjectIdentity(std::string_view name) const {
  auto it = classes.find(std::string(name));
  return it == classes.end() ? 0 : it->second->Identity;
}
std::shared_ptr<Object> Heap::stringConstant(std::string_view key,
                                             std::vector<uint8_t> bytes) {
  if (auto it = constants.find(std::string(key)); it != constants.end())
    return it->second;
  auto value = allocate(Object::String, "java/lang/String", bytes.size(),
                        key.size() + 64);
  if (value) {
    value->Bytes = std::move(bytes);
    value->Length = value->Bytes.size() / 2;
    constants.emplace(std::string(key), value);
  }
  return value;
}
std::shared_ptr<Object> Heap::intern(std::shared_ptr<Object> value) {
  if (auto it = interned.find(value->Bytes); it != interned.end())
    return it->second;
  const uint64_t extra = value->Bytes.size() + 64;
  if (extra > limit - used)
    return {};
  used += extra;
  interned.emplace(value->Bytes, value);
  return value;
}
std::shared_ptr<Object> Heap::literal(std::string_view text) {
  auto bytes = javaStringBytes(text);
  if (auto it = interned.find(bytes); it != interned.end())
    return it->second;
  auto value = allocate(Object::String, "java/lang/String", bytes.size());
  if (!value)
    return {};
  value->Bytes = std::move(bytes);
  value->Length = value->Bytes.size() / 2;
  return intern(value);
}
std::vector<uint8_t> javaStringBytes(std::string_view text) {
  std::vector<uint8_t> bytes;
  auto unit = [&](uint16_t v) {
    bytes.push_back(v);
    bytes.push_back(v >> 8);
  };
  for (size_t i = 0; i < text.size();) {
    uint32_t c = uint8_t(text[i++]);
    unsigned count = c < 0x80                 ? 0
                     : c >= 0xc2 && c <= 0xdf ? 1
                     : c >= 0xe0 && c <= 0xef ? 2
                     : c >= 0xf0 && c <= 0xf4 ? 3
                                              : 4;
    if (count == 4)
      throw Error("invalid DEX string UTF-8");
    c &= count ? (1U << (6 - count)) - 1 : 0x7f;
    for (unsigned j = 0; j < count; ++j) {
      if (i == text.size() || (uint8_t(text[i]) & 0xc0) != 0x80)
        throw Error("invalid DEX string continuation");
      c = (c << 6) | (uint8_t(text[i++]) & 63);
    }
    if ((count == 1 && c < 0x80) || (count == 2 && c < 0x800) ||
        (count == 3 && (c < 0x10000 || c > 0x10ffff)))
      throw Error("noncanonical DEX string UTF-8");
    if (c > 0xffff) {
      c -= 0x10000;
      unit(0xd800 | (c >> 10));
      c = 0xdc00 | (c & 1023);
    }
    unit(c);
  }
  return bytes;
}
std::vector<uint8_t> artStringUTF8(std::span<const uint8_t> UTF16) {
  std::vector<uint8_t> Out;
  for (size_t I = 0; I < UTF16.size(); I += 2) {
    const uint16_t Value = uint16_t(UTF16[I] | uint16_t(UTF16[I + 1]) << 8);
    // ART's JNI encoding uses four bytes for a complete surrogate pair;
    // isolated surrogate units still retain their three-byte representation.
    if (Value >= 0xd800 && Value < 0xdc00 && I + 2 < UTF16.size()) {
      const uint16_t Next =
          uint16_t(UTF16[I + 2] | uint16_t(UTF16[I + 3]) << 8);
      if (Next >= 0xdc00 && Next <= 0xdfff) {
        const uint32_t Scalar =
            0x10000 + ((Value - 0xd800) << 10) + (Next - 0xdc00);
        Out.push_back(0xf0 | (Scalar >> 18));
        Out.push_back(0x80 | ((Scalar >> 12) & 63));
        Out.push_back(0x80 | ((Scalar >> 6) & 63));
        Out.push_back(0x80 | (Scalar & 63));
        I += 2;
        continue;
      }
    }
    if (Value && Value < 0x80) {
      Out.push_back(Value);
    } else if (Value < 0x800) {
      Out.push_back(0xc0 | (Value >> 6));
      Out.push_back(0x80 | (Value & 63));
    } else {
      Out.push_back(0xe0 | (Value >> 12));
      Out.push_back(0x80 | ((Value >> 6) & 63));
      Out.push_back(0x80 | (Value & 63));
    }
  }
  return Out;
}
bool platformClass(std::string_view name) {
  constexpr std::string_view names[] = {
      "java/lang/Object",
      "java/lang/Class",
      "java/lang/System",
      "java/lang/String",
      "java/lang/Cloneable",
      "java/io/Serializable",
      "java/io/IOException",
      "java/lang/Throwable",
      "java/lang/Exception",
      "java/lang/RuntimeException",
      "java/lang/Error",
      "java/lang/ArithmeticException",
      "java/lang/NullPointerException",
      "java/lang/IllegalArgumentException",
      "java/lang/IllegalStateException",
      "java/lang/IndexOutOfBoundsException",
      "java/lang/ArrayIndexOutOfBoundsException",
      "java/lang/StringIndexOutOfBoundsException",
      "java/lang/NegativeArraySizeException",
      "java/lang/ArrayStoreException",
      "java/lang/ClassCastException",
      "java/lang/OutOfMemoryError",
      "java/lang/LinkageError",
      "java/lang/ExceptionInInitializerError",
      "java/lang/NoClassDefFoundError",
      "java/lang/UnsatisfiedLinkError",
      "java/lang/NoSuchMethodError",
      "java/lang/IncompatibleClassChangeError",
      "java/lang/ClassNotFoundException",
      "java/lang/ReflectiveOperationException"};
  for (auto n : names)
    if (name == n)
      return true;
  return false;
}
bool referenceType(std::string_view type) {
  return type.starts_with('L') || type.starts_with('[');
}
std::string objectDescriptor(const Object &object) {
  if (object.Type == Object::Class)
    return "Ljava/lang/Class;";
  return object.ClassName.starts_with('[') ? object.ClassName
                                           : "L" + object.ClassName + ";";
}
std::string_view javaParent(std::string_view name) {
  if (name == "java/lang/Object")
    return {};
  if (name == "java/lang/ClassNotFoundException")
    return "java/lang/ReflectiveOperationException";
  if (name == "java/io/IOException" || name == "java/lang/RuntimeException" ||
      name == "java/lang/ReflectiveOperationException")
    return "java/lang/Exception";
  if (name == "java/lang/Exception" || name == "java/lang/Error")
    return "java/lang/Throwable";
  if (name == "java/lang/OutOfMemoryError" || name == "java/lang/LinkageError")
    return "java/lang/Error";
  if (name == "java/lang/NoSuchMethodError")
    return "java/lang/IncompatibleClassChangeError";
  if (name == "java/lang/ExceptionInInitializerError" ||
      name == "java/lang/UnsatisfiedLinkError" ||
      name == "java/lang/NoClassDefFoundError" ||
      name == "java/lang/IncompatibleClassChangeError")
    return "java/lang/LinkageError";
  if (name == "java/lang/ArrayIndexOutOfBoundsException" ||
      name == "java/lang/StringIndexOutOfBoundsException")
    return "java/lang/IndexOutOfBoundsException";
  if (platformClass(name) && name.ends_with("Exception"))
    return "java/lang/RuntimeException";
  return "java/lang/Object";
}
bool referenceAssignable(std::string_view source,
                         std::string_view destination) {
  if (source == destination)
    return true;
  if (!referenceType(source) || !referenceType(destination))
    return false;
  if (source.starts_with('[')) {
    if (destination == "Ljava/lang/Object;" ||
        destination == "Ljava/lang/Cloneable;" ||
        destination == "Ljava/io/Serializable;")
      return true;
    return destination.starts_with('[') && referenceType(source.substr(1)) &&
           referenceType(destination.substr(1)) &&
           referenceAssignable(source.substr(1), destination.substr(1));
  }
  if (destination.starts_with('['))
    return false;
  source = source.substr(1, source.size() - 2);
  destination = destination.substr(1, destination.size() - 2);
  for (; !source.empty(); source = javaParent(source)) {
    if (source == destination ||
        (source == "java/lang/String" &&
         destination == "java/lang/CharSequence") ||
        (destination == "java/io/Serializable" &&
         (source == "java/lang/String" || source == "java/lang/Class" ||
          source == "java/lang/Throwable")))
      return true;
  }
  return false;
}
} // namespace neverd::mobile::dalvik
