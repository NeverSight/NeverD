//===- MobileKotlinMetadata.cpp - Bounded synthetic generic scope witness ===//
#include "MobileDalvikSignature.h"

#include <algorithm>

namespace neverd::mobile::dalvik {
namespace {
// Independent reader for the wire contracts in JetBrains/kotlin v2.1.0:
// core/metadata/src/metadata.proto and
// core/metadata.jvm/src/jvm_metadata.proto. Only modern byte-encoded metadata
// and unconstrained method formals are used. This is a declaration witness, not
// a general Kotlin source reconstruction.
struct Unproven {};
constexpr size_t MetadataLimit = 1 << 20;
struct WireValue {
  unsigned number = 0, wire = 0;
  uint64_t integer = 0;
  std::string_view bytes;
};
using Message = std::vector<WireValue>;

uint64_t varint(std::string_view &input, Budget &budget) {
  uint64_t result = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    budget.tick();
    if (input.empty())
      throw Unproven{};
    const auto byte = uint8_t(input.front());
    input.remove_prefix(1);
    if (shift == 63 && byte > 1)
      throw Unproven{};
    result |= uint64_t(byte & 127) << shift;
    if (!(byte & 128))
      return result;
  }
  throw Unproven{};
}
std::string_view take(std::string_view &input, size_t length) {
  if (length > input.size())
    throw Unproven{};
  const auto result = input.substr(0, length);
  input.remove_prefix(length);
  return result;
}
Message message(std::string_view input, Budget &budget) {
  Message result;
  if (input.size() > MetadataLimit)
    throw Unproven{};
  while (!input.empty()) {
    if (result.size() >= 32768)
      throw Unproven{};
    auto tag = varint(input, budget);
    if (tag < 8 || tag >> 3 >= uint64_t(1) << 29)
      throw Unproven{};
    WireValue value{unsigned(tag >> 3), unsigned(tag & 7)};
    if (value.wire == 0)
      value.integer = varint(input, budget);
    else if (value.wire == 2) {
      const auto length = varint(input, budget);
      if (length > MetadataLimit)
        throw Unproven{};
      value.bytes = take(input, size_t(length));
    } else if (value.wire == 1 || value.wire == 5)
      value.bytes = take(input, value.wire == 1 ? 8 : 4);
    else
      throw Unproven{};
    result.push_back(value);
  }
  return result;
}
const WireValue *one(const Message &input, unsigned number, unsigned wire) {
  const WireValue *result = nullptr;
  for (const auto &value : input)
    if (value.number == number) {
      if (result || value.wire != wire)
        throw Unproven{};
      result = &value;
    }
  return result;
}
bool present(const Message &input, unsigned number) {
  return std::any_of(input.begin(), input.end(),
                     [&](const auto &value) { return value.number == number; });
}
uint64_t integer(const Message &input, unsigned number, uint64_t fallback) {
  const auto *value = one(input, number, 0);
  return value ? value->integer : fallback;
}
std::vector<Message> children(const Message &input, unsigned number,
                              Budget &budget) {
  std::vector<Message> result;
  for (const auto &value : input)
    if (value.number == number) {
      if (value.wire != 2)
        throw Unproven{};
      result.push_back(message(value.bytes, budget));
    }
  return result;
}
const AnnotationValue &element(const AnnotationUse &metadata,
                               const std::string &name, unsigned kind) {
  auto found = metadata.elements.find(name);
  if (found == metadata.elements.end() || found->second.kind != kind)
    throw Unproven{};
  return found->second;
}
int64_t scalar(const AnnotationValue &value) {
  const auto *number = std::get_if<int64_t>(&value.value);
  if (value.kind != 0x04 || !number)
    throw Unproven{};
  return *number;
}
std::vector<std::string> strings(const AnnotationUse &metadata,
                                 const std::string &name, Budget &budget) {
  std::vector<std::string> result;
  size_t bytes = 0;
  for (const auto &value : element(metadata, name, 0x1c).array) {
    budget.tick();
    const auto *text = std::get_if<std::string>(&value.value);
    if (value.kind != 0x17 || !text || text->size() > MetadataLimit - bytes ||
        result.size() >= 32768)
      throw Unproven{};
    bytes += text->size();
    result.push_back(*text);
  }
  return result;
}
std::string decode(const std::vector<std::string> &parts, Budget &budget) {
  if (parts.empty() || parts[0].empty() || parts[0][0] != 0)
    throw Unproven{};
  std::string result;
  for (size_t p = 0; p < parts.size(); ++p)
    for (size_t i = p == 0 ? 1 : 0; i < parts[p].size(); ++i) {
      budget.tick();
      // Kotlin's zero marker stores one byte per UTF-16 code unit. The model
      // holds UTF-8, so only ASCII and U+0080..U+00FF are valid in this mode.
      auto c = uint8_t(parts[p][i]);
      if (c >= 128) {
        if ((c != 0xc2 && c != 0xc3) || ++i == parts[p].size())
          throw Unproven{};
        auto low = uint8_t(parts[p][i]);
        if ((low & 0xc0) != 0x80)
          throw Unproven{};
        c = uint8_t(((c & 3) << 6) | (low & 63));
      }
      result.push_back(char(c));
    }
  return result;
}

class Names {
  std::vector<std::string> strings;
  std::vector<Message> groups;
  std::vector<size_t> records;

public:
  Names(std::vector<std::string> strings, const Message &table, Budget &budget)
      : strings(std::move(strings)) {
    // Local class IDs cannot be substituted for globally named JVM types.
    if (present(table, 5))
      throw Unproven{};
    groups = children(table, 1, budget);
    for (size_t group = 0; group < groups.size(); ++group) {
      const auto &record = groups[group];
      const auto count = integer(record, 1, 1);
      if (!count || count > this->strings.size() - records.size())
        throw Unproven{};
      for (size_t i = 0; i < count; ++i) {
        budget.tick();
        records.push_back(group);
      }
    }
    if (records.size() != this->strings.size())
      throw Unproven{};
  }
  std::string get(uint64_t index) const {
    if (index >= records.size())
      throw Unproven{};
    const auto &record = groups[records[index]];
    if (present(record, 4) || present(record, 5))
      throw Unproven{};
    std::string result;
    if (auto *literal = one(record, 6, 2))
      result = literal->bytes;
    else if (auto *predefined = one(record, 2, 0)) {
      // Only these classes enter the supported, context-checked erasure subset.
      if (predefined->integer == 0)
        result = "kotlin/Any";
      else if (predefined->integer == 2)
        result = "kotlin/Unit";
      else if (predefined->integer == 5)
        result = "kotlin/Byte";
      else if (predefined->integer == 6)
        result = "kotlin/Double";
      else if (predefined->integer == 7)
        result = "kotlin/Float";
      else if (predefined->integer == 8)
        result = "kotlin/Int";
      else if (predefined->integer == 9)
        result = "kotlin/Long";
      else if (predefined->integer == 10)
        result = "kotlin/Short";
      else if (predefined->integer == 11)
        result = "kotlin/Boolean";
      else if (predefined->integer == 12)
        result = "kotlin/Char";
      else if (predefined->integer == 14)
        result = "kotlin/String";
      else
        throw Unproven{};
    } else
      result = strings[index];
    const auto operation = integer(record, 3, 0);
    if (operation == 2) {
      if (result.size() < 3 || result.front() != 'L' || result.back() != ';')
        throw Unproven{};
      result = result.substr(1, result.size() - 2);
    } else if (operation > 2)
      throw Unproven{};
    if (operation)
      std::replace(result.begin(), result.end(), '$', '.');
    return result;
  }
};

std::string classDescriptor(std::string name) {
  if (name == "kotlin/Any")
    return "Ljava/lang/Object;";
  if (name == "kotlin/String")
    return "Ljava/lang/String;";
  // Ordinary coroutine reference classes retain their JVM internal names;
  // mapped Kotlin builtins outside the supported subset remain unproven.
  if (name.empty() ||
      (name.starts_with("kotlin/") &&
       !name.starts_with("kotlin/coroutines/")) ||
      name.find_first_of(";[<>:\0", 0, 6) != name.npos)
    throw Unproven{};
  std::replace(name.begin(), name.end(), '.', '$');
  return "L" + name + ";";
}

class FunctionScope {
  const Names &names;
  Budget &budget;
  std::vector<Message> types;
  std::optional<size_t> first_nullable;
  std::map<uint64_t, std::string> formals;
  bool requires_jvm_descriptor = false;

  Message type(const Message &owner, unsigned direct, unsigned indexed) {
    const auto *inline_type = one(owner, direct, 2);
    const auto *index = one(owner, indexed, 0);
    if (bool(inline_type) == bool(index))
      throw Unproven{};
    if (inline_type)
      return message(inline_type->bytes, budget);
    if (index->integer >= types.size())
      throw Unproven{};
    auto result = types[index->integer];
    if (first_nullable && index->integer >= *first_nullable) {
      const auto *nullable = one(result, 3, 0);
      if (nullable)
        for (auto &value : result)
          if (value.number == 3)
            value.integer = 1;
      if (!nullable)
        result.push_back({3, 0, 1, {}});
    }
    return result;
  }
  std::string erasure(const Message &value, bool return_position = false) {
    const auto flags = integer(value, 1, 0);
    if (flags > 1)
      throw Unproven{};
    for (unsigned unsupported : {4u, 5u, 8u, 10u, 11u, 12u, 13u, 14u})
      if (present(value, unsupported))
        throw Unproven{};
    const auto *cls = one(value, 6, 0), *formal = one(value, 7, 0),
               *named_formal = one(value, 9, 0);
    if (unsigned(bool(cls)) + unsigned(bool(formal)) +
            unsigned(bool(named_formal)) !=
        1)
      throw Unproven{};
    if (formal || named_formal) {
      const bool declared =
          formal
              ? formals.contains(formal->integer)
              : std::any_of(
                    formals.begin(), formals.end(), [&](const auto &entry) {
                      return entry.second == names.get(named_formal->integer);
                    });
      if (flags || !declared || present(value, 2))
        throw Unproven{};
      return "Ljava/lang/Object;";
    }
    const auto name = names.get(cls->integer);
    if (name.starts_with("kotlin/Function")) {
      auto suffix = std::string_view(name).substr(15);
      if (suffix.empty() || suffix.size() > 2)
        throw Unproven{};
      unsigned arity = 0;
      for (unsigned char c : suffix) {
        if (c < '0' || c > '9')
          throw Unproven{};
        arity = arity * 10 + c - '0';
      }
      if (arity > 22 || std::to_string(arity) != suffix)
        throw Unproven{};
      if (flags) {
        // The classifier already describes the runtime FunctionN. A suspend
        // callback must additionally carry its Continuation/Object ABI tail,
        // and the complete explicit JVM method descriptor must corroborate it.
        const auto arguments = children(value, 2, budget);
        if (!arity || arguments.size() != arity + 1)
          throw Unproven{};
        auto plainClass = [&](const Message &type, std::string_view expected) {
          if (integer(type, 1, 0) != 0)
            throw Unproven{};
          for (unsigned unsupported :
               {4u, 5u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u})
            if (present(type, unsupported))
              throw Unproven{};
          const auto *cls = one(type, 6, 0);
          return cls && names.get(cls->integer) == expected;
        };
        const auto continuation = type(arguments[arity - 1], 2, 3);
        const auto result = type(arguments[arity], 2, 3);
        if (!plainClass(continuation, "kotlin/coroutines/Continuation") ||
            integer(continuation, 3, 0) != 0 ||
            children(continuation, 2, budget).size() != 1 ||
            !plainClass(result, "kotlin/Any") || integer(result, 3, 0) == 0 ||
            present(result, 2))
          throw Unproven{};
        requires_jvm_descriptor = true;
      }
      return "Lkotlin/jvm/functions/Function" + std::to_string(arity) + ";";
    }
    if (flags)
      throw Unproven{};
    struct Primitive {
      std::string_view name, boxed;
      char descriptor;
    };
    static constexpr Primitive primitives[] = {
        {"kotlin/Boolean", "Boolean", 'Z'}, {"kotlin/Char", "Character", 'C'},
        {"kotlin/Byte", "Byte", 'B'},       {"kotlin/Short", "Short", 'S'},
        {"kotlin/Int", "Integer", 'I'},     {"kotlin/Long", "Long", 'J'},
        {"kotlin/Float", "Float", 'F'},     {"kotlin/Double", "Double", 'D'}};
    for (const auto &primitive : primitives)
      if (name == primitive.name) {
        if (present(value, 2))
          throw Unproven{};
        return integer(value, 3, 0) == 0
                   ? std::string(1, primitive.descriptor)
                   : "Ljava/lang/" + std::string(primitive.boxed) + ";";
      }
    if (name == "kotlin/Unit") {
      // Unit maps to void only for an ordinary non-null return. It is not a
      // reference descriptor for parameters, receivers or formal bounds.
      if (!return_position || integer(value, 3, 0) != 0 || present(value, 2))
        throw Unproven{};
      return "V";
    }
    return classDescriptor(name);
  }

public:
  FunctionScope(const Names &names, const Message &container,
                const Message &function, Budget &budget)
      : names(names), budget(budget) {
    const auto *table = one(function, 30, 2);
    if (!table)
      table = one(container, 30, 2);
    if (table) {
      const auto contents = message(table->bytes, budget);
      types = children(contents, 1, budget);
      if (const auto *nullable = one(contents, 2, 0)) {
        if (nullable->integer != UINT64_MAX) {
          if (nullable->integer > types.size())
            throw Unproven{};
          first_nullable = size_t(nullable->integer);
        }
      }
    }
  }
  std::vector<std::string> read(const Message &function, const MethodRef &ref,
                                bool &matched) {
    // Context receivers, suspend/value-class ABI adaptations and varargs need
    // independent descriptor proofs; do not infer those from an ordinary type.
    if (present(function, 10) || present(function, 11) ||
        integer(function, 9, integer(function, 1, 6)) >= 4096)
      throw Unproven{};
    auto name = names.get(integer(function, 2, UINT64_MAX));
    std::optional<Message> jvm;
    if (const auto *extension = one(function, 100, 2)) {
      jvm = message(extension->bytes, budget);
      if (const auto *encoded_name = one(*jvm, 1, 0))
        name = names.get(encoded_name->integer);
    }
    if (name != ref.name)
      throw Unproven{};
    std::vector<std::string> result;
    std::set<std::string> seen;
    const auto declarations = children(function, 4, budget);
    for (const auto &formal : declarations) {
      budget.tick();
      auto id = integer(formal, 1, UINT64_MAX);
      auto name = names.get(integer(formal, 2, UINT64_MAX));
      if (id > 65535 || name.empty() ||
          !std::all_of(name.begin(), name.end(),
                       [](unsigned char c) {
                         return (c >= 'a' && c <= 'z') ||
                                (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '_' || c == '$';
                       }) ||
          !seen.insert(name).second || !formals.emplace(id, name).second ||
          result.size() >= 256)
        throw Unproven{};
      result.push_back(std::move(name));
    }
    if (result.empty())
      throw Unproven{};
    std::vector<std::string> parameters;
    if (present(function, 5) || present(function, 8))
      parameters.push_back(erasure(type(function, 5, 8)));
    for (const auto &parameter : children(function, 6, budget)) {
      if (present(parameter, 4) || present(parameter, 6))
        throw Unproven{};
      parameters.push_back(erasure(type(parameter, 3, 5)));
    }
    if (parameters != ref.parameters ||
        erasure(type(function, 3, 7), true) != ref.returns)
      throw Unproven{};
    matched = true;
    if (requires_jvm_descriptor && (!jvm || !one(*jvm, 2, 0)))
      throw Unproven{};
    if (jvm) {
      if (const auto *encoded_desc = one(*jvm, 2, 0)) {
        std::string descriptor = "(";
        for (const auto &parameter : parameters)
          descriptor += parameter;
        descriptor += ")" + ref.returns;
        if (names.get(encoded_desc->integer) != descriptor)
          throw Unproven{};
      }
    }
    for (const auto &formal : declarations) {
      if (integer(formal, 3, 0) != 0 || integer(formal, 4, 2) != 2 ||
          present(formal, 6))
        throw Unproven{};
      const auto bounds = children(formal, 5, budget);
      if (bounds.size() > 1 ||
          (!bounds.empty() && (present(bounds[0], 2) ||
                               erasure(bounds[0]) != "Ljava/lang/Object;" ||
                               !one(bounds[0], 6, 0))))
        throw Unproven{};
    }
    return result;
  }
};
} // namespace

std::optional<std::vector<std::string>>
kotlinUnconstrainedMethodFormals(const Class &owner, const Method &method,
                                 Budget &budget) {
  if (owner.external || method.reference.owner != owner.name ||
      method.generic_signature || !has(method.access, "static") ||
      !has(method.access, "synthetic"))
    return {};
  try {
    const AnnotationUse *metadata = nullptr;
    for (const auto &annotation : owner.annotations)
      if (annotation.type == "Lkotlin/Metadata;") {
        if (metadata || annotation.visibility != 1)
          throw Unproven{};
        metadata = &annotation;
      }
    if (!metadata)
      return {};
    const auto kind = scalar(element(*metadata, "k", 0x04));
    const auto &version = element(*metadata, "mv", 0x1c).array;
    if ((kind != 1 && kind != 2 && kind != 5) || version.size() != 3)
      return {};
    if (kind == 5) {
      const auto &extra = element(*metadata, "xs", 0x17);
      const auto *facade = std::get_if<std::string>(&extra.value);
      if (!facade || facade->empty() || facade->size() > 65535)
        return {};
      bool initial = true;
      for (unsigned char c : *facade) {
        budget.tick();
        if (c == '/') {
          if (initial)
            return {};
          initial = true;
        } else {
          const bool letter = (c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
          if (!letter && (initial || c < '0' || c > '9'))
            return {};
          initial = false;
        }
      }
      if (owner.name.size() < 3 || owner.name.front() != 'L' ||
          owner.name.back() != ';')
        return {};
      auto package = [](std::string_view name) {
        const auto end = name.find_last_of('/');
        return name.substr(0, end == name.npos ? 0 : end + 1);
      };
      if (initial || "L" + *facade + ";" == owner.name ||
          package(*facade) != package(std::string_view(owner.name)
                                          .substr(1, owner.name.size() - 2)))
        return {};
    }
    const auto major = scalar(version[0]), minor = scalar(version[1]);
    if (!((major == 1 && minor >= 4 && minor <= 9) ||
          (major == 2 && minor >= 0 && minor <= 1)) ||
        scalar(version[2]) < 0)
      return {};
    auto bytes = decode(strings(*metadata, "d1", budget), budget);
    std::string_view remaining = bytes;
    const auto length = varint(remaining, budget);
    if (length > MetadataLimit)
      return {};
    Names names(strings(*metadata, "d2", budget),
                message(take(remaining, size_t(length)), budget), budget);
    const auto container = message(remaining, budget);
    if (kind == 1 && (present(container, 5) ||
                      classDescriptor(names.get(
                          integer(container, 3, UINT64_MAX))) != owner.name))
      return {};
    std::optional<std::vector<std::string>> result;
    for (const auto &function :
         children(container, kind == 1 ? 9 : 3, budget)) {
      std::optional<std::vector<std::string>> candidate;
      bool matched = false;
      try {
        candidate = FunctionScope(names, container, function, budget)
                        .read(function, method.reference, matched);
      } catch (const Unproven &) {
        if (matched)
          return {};
        continue;
      }
      if (result)
        return {}; // An exact method must have one declaration, not two.
      result = std::move(candidate);
    }
    return result;
  } catch (const Unproven &) {
    return {};
  }
}
} // namespace neverd::mobile::dalvik
