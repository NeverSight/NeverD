//===- MobileJavaClass.cpp - Dependency classfile declaration reader -----===//
#include "MobileDalvik.h"

#include <bit>
#include <functional>

namespace neverd::mobile::dalvik {
namespace {
[[noreturn]] void bad(const std::string &message) {
  throw Error("Invalid dependency classfile: " + message);
}

class Cursor {
  std::string_view data;
  size_t offset = 0;

public:
  explicit Cursor(std::string_view bytes) : data(bytes) {}
  size_t remaining() const { return data.size() - offset; }
  std::string_view take(size_t count) {
    if (count > remaining())
      bad("truncated structure");
    auto result = data.substr(offset, count);
    offset += count;
    return result;
  }
  uint64_t number(unsigned count) {
    uint64_t result = 0;
    for (unsigned char byte : take(count))
      result = (result << 8) | byte;
    return result;
  }
  unsigned u1() { return number(1); }
  unsigned u2() { return number(2); }
  uint32_t u4() { return number(4); }
  void end() const {
    if (remaining())
      bad("trailing structure data");
  }
};

std::string utf8(std::string_view bytes, Budget &budget) {
  Cursor input(bytes);
  std::vector<uint16_t> units;
  while (input.remaining()) {
    budget.tick();
    unsigned first = input.u1(), unit = first;
    auto next = [&]() {
      auto value = input.u1();
      if ((value & 0xc0) != 0x80)
        bad("invalid modified UTF-8 continuation");
      return value & 63;
    };
    if (first >= 0xc0 && first <= 0xdf) {
      unit = ((first & 31) << 6) | next();
      if (unit && unit < 128)
        bad("overlong modified UTF-8");
    } else if (first >= 0xe0 && first <= 0xef) {
      unit = ((first & 15) << 12) | (next() << 6);
      unit |= next();
      if (unit < 2048)
        bad("overlong modified UTF-8");
    } else if (!first || first > 127) {
      bad("invalid modified UTF-8 leading byte");
    }
    units.push_back(unit);
  }
  std::string result;
  for (size_t i = 0; i < units.size(); ++i) {
    uint32_t cp = units[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < units.size() &&
        units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff)
      cp = 0x10000 + ((cp - 0xd800) << 10) + (units[++i] - 0xdc00);
    // Isolated surrogate units retain WTF-8, as in the DEX reader.
    if (cp < 128)
      result += char(cp);
    else if (cp < 2048) {
      result += char(0xc0 | (cp >> 6));
      result += char(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
      result += char(0xe0 | (cp >> 12));
      result += char(0x80 | ((cp >> 6) & 63));
      result += char(0x80 | (cp & 63));
    } else {
      result += char(0xf0 | (cp >> 18));
      result += char(0x80 | ((cp >> 12) & 63));
      result += char(0x80 | ((cp >> 6) & 63));
      result += char(0x80 | (cp & 63));
    }
  }
  return result;
}

struct Constant {
  unsigned tag = 0, first = 0, second = 0;
  uint64_t bits = 0;
  std::string text;
};

class Reader {
  Cursor input;
  Budget &budget;
  std::vector<Constant> pool;
  Class result;
  unsigned major = 0;

  Access classAccess(unsigned flags) const {
    // Before classfile version 50, interface implies abstract in both the
    // class header and InnerClasses metadata. Normalize that reader-owned
    // role before the shared model validates it; preserve all other flags.
    if (major < 50 && (flags & 0x200))
      flags |= 0x400;
    return accessFlags(flags);
  }

  const Constant &constant(unsigned index, unsigned tag) const {
    if (!index || index >= pool.size() || pool[index].tag != tag)
      bad("constant pool index or kind mismatch");
    return pool[index];
  }
  std::string text(unsigned index) const { return constant(index, 1).text; }
  std::string classType(unsigned index) const {
    auto name = text(constant(index, 7).first);
    if (name.empty() || name.starts_with('['))
      bad("declaration requires a non-array class");
    return descriptor("L" + name + ";");
  }
  void constants() {
    auto count = input.u2();
    if (!count)
      bad("empty constant pool");
    pool.resize(count);
    for (unsigned i = 1; i < count; ++i) {
      budget.tick();
      auto &entry = pool[i];
      entry.tag = input.u1();
      switch (entry.tag) {
      case 1:
        entry.text = utf8(input.take(input.u2()), budget);
        break;
      case 3:
      case 4:
        entry.bits = input.u4();
        break;
      case 5:
      case 6:
        entry.bits = input.number(8);
        if (++i >= count)
          bad("wide constant has no reserved slot");
        break;
      case 7:
      case 8:
      case 16:
      case 19:
      case 20:
        entry.first = input.u2();
        break;
      case 9:
      case 10:
      case 11:
      case 12:
      case 17:
      case 18:
        entry.first = input.u2();
        entry.second = input.u2();
        break;
      case 15:
        entry.first = input.u1();
        entry.second = input.u2();
        break;
      default:
        bad("unsupported constant pool tag");
      }
      if (((entry.tag == 15 || entry.tag == 16 || entry.tag == 18) &&
           major < 51) ||
          ((entry.tag == 19 || entry.tag == 20) && major < 53) ||
          (entry.tag == 17 && major < 55))
        bad("constant pool tag disagrees with version");
    }
    for (const auto &entry : pool) {
      budget.tick();
      switch (entry.tag) {
      case 7:
      case 8:
      case 16:
      case 19:
      case 20:
        constant(entry.first, 1);
        break;
      case 9:
      case 10:
      case 11:
        constant(entry.first, 7);
        constant(entry.second, 12);
        break;
      case 12:
        constant(entry.first, 1);
        constant(entry.second, 1);
        break;
      case 17:
      case 18:
        constant(entry.second, 12);
        break;
      case 15:
        if (!entry.first || entry.first > 9 || !entry.second ||
            entry.second >= pool.size())
          bad("invalid method handle");
        if (entry.first <= 4)
          constant(entry.second, 9);
        else if (entry.first == 9)
          constant(entry.second, 11);
        else if (pool[entry.second].tag != 10 &&
                 !(major >= 52 && (entry.first == 6 || entry.first == 7) &&
                   pool[entry.second].tag == 11))
          bad("method handle reference kind mismatch");
        break;
      }
    }
  }
  void attributes(Cursor &reader,
                  const std::function<bool(const std::string &, Cursor &)> &f) {
    auto count = reader.u2();
    std::set<std::string> seen;
    for (unsigned i = 0; i < count; ++i) {
      budget.tick();
      auto name = text(reader.u2());
      Cursor body(reader.take(reader.u4()));
      if (f(name, body)) {
        if (!seen.insert(name).second)
          bad("duplicate declaration attribute: " + name);
        body.end();
      }
      // Unused dependency attributes, including Code, are opaque. This
      // reader establishes declarations, not executable JVM semantics.
    }
  }
  AnnotationValue value(Cursor &reader, unsigned depth) {
    budget.tick();
    if (depth > 64)
      bad("annotation nesting exceeds limit");
    AnnotationValue v;
    auto tag = reader.u1();
    const std::map<unsigned, unsigned> integers{
        {'B', 0}, {'C', 3}, {'S', 2}, {'I', 4}, {'Z', 0x1f}};
    if (auto found = integers.find(tag); found != integers.end()) {
      v.kind = found->second;
      auto n = int64_t(
          std::bit_cast<int32_t>(uint32_t(constant(reader.u2(), 3).bits)));
      if (tag == 'Z') {
        if (n != 0 && n != 1)
          bad("invalid annotation boolean");
        v.value = bool(n);
      } else {
        if ((tag == 'B' && (n < INT8_MIN || n > INT8_MAX)) ||
            (tag == 'S' && (n < INT16_MIN || n > INT16_MAX)) ||
            (tag == 'C' && (n < 0 || n > UINT16_MAX)))
          bad("annotation integer exceeds its element type");
        v.value = n;
      }
    } else if (tag == 'J') {
      v.kind = 6;
      v.value = std::bit_cast<int64_t>(constant(reader.u2(), 5).bits);
    } else if (tag == 'F' || tag == 'D') {
      v.kind = tag == 'F' ? 0x10 : 0x11;
      v.value =
          FloatBits{constant(reader.u2(), tag == 'F' ? 4 : 6).bits, tag == 'D'};
    } else if (tag == 's' || tag == 'c') {
      v.kind = tag == 's' ? 0x17 : 0x18;
      auto t = text(reader.u2());
      v.value = tag == 's' ? t : descriptor(t, true);
    } else if (tag == 'e') {
      v.kind = 0x1b;
      auto type = descriptor(text(reader.u2()));
      auto name = text(reader.u2());
      if (!type.starts_with('L') || name.empty())
        bad("invalid enum element");
      v.value = type + "->" + name + ":" + type;
    } else if (tag == '[') {
      v.kind = 0x1c;
      auto count = reader.u2();
      for (unsigned i = 0; i < count; ++i)
        v.array.push_back(value(reader, depth + 1));
    } else if (tag == '@') {
      v.kind = 0x1d;
      auto use = annotation(reader, depth + 1);
      v.annotation_type = std::move(use.type);
      v.elements = std::move(use.elements);
    } else {
      bad("invalid annotation value tag");
    }
    return v;
  }
  AnnotationUse annotation(Cursor &reader, unsigned depth) {
    if (depth > 64)
      bad("annotation nesting exceeds limit");
    AnnotationUse use;
    use.type = descriptor(text(reader.u2()));
    if (!use.type.starts_with('L'))
      bad("annotation type is not a class");
    auto count = reader.u2();
    for (unsigned i = 0; i < count; ++i) {
      budget.tick();
      auto name = text(reader.u2());
      auto v = value(reader, depth + 1);
      if (name.empty() || !use.elements.emplace(name, std::move(v)).second)
        bad("invalid or duplicate annotation element");
    }
    return use;
  }
  void annotationPolicy(Cursor &reader) {
    auto count = reader.u2();
    std::set<std::string> seen;
    for (unsigned i = 0; i < count; ++i) {
      auto use = annotation(reader, 0);
      if (!seen.insert(use.type).second)
        bad("duplicate declaration annotation");
      auto enumName = [](const AnnotationValue &v, const std::string &owner) {
        if (v.kind != 0x1b || !std::holds_alternative<std::string>(v.value))
          bad("annotation policy requires an enum constant");
        auto ref = fieldRef(std::get<std::string>(v.value));
        if (ref.owner != owner || ref.type != owner)
          bad("annotation policy enum kind mismatch");
        return ref.name;
      };
      if (use.type == "Ljava/lang/annotation/Retention;" ||
          use.type == "Ljava/lang/annotation/Target;") {
        if (use.elements.size() != 1 || !use.elements.contains("value"))
          bad("invalid annotation policy elements");
        const auto &v = use.elements.at("value");
        if (use.type == "Ljava/lang/annotation/Retention;") {
          result.annotation_metadata.retention =
              enumName(v, "Ljava/lang/annotation/RetentionPolicy;");
          if (!Access{"SOURCE", "CLASS", "RUNTIME"}.contains(
                  *result.annotation_metadata.retention))
            bad("invalid annotation retention policy");
        } else {
          if (v.kind != 0x1c)
            bad("annotation target requires an array");
          std::vector<std::string> targets;
          std::set<std::string> seen_targets;
          for (const auto &entry : v.array) {
            auto target = enumName(entry, "Ljava/lang/annotation/ElementType;");
            if (!annotationTargetJavaRelease(target) ||
                !seen_targets.insert(target).second)
              bad("invalid annotation target policy");
            targets.push_back(std::move(target));
          }
          result.annotation_metadata.targets = std::move(targets);
        }
      } else if (use.type == "Ljava/lang/annotation/Documented;" ||
                 use.type == "Ljava/lang/annotation/Inherited;") {
        if (!use.elements.empty())
          bad("invalid marker policy elements");
        if (use.type == "Ljava/lang/annotation/Documented;")
          result.annotation_metadata.documented = true;
        else
          result.annotation_metadata.inherited = true;
      }
    }
  }

public:
  Reader(std::string_view bytes, std::string_view source, Budget &budget)
      : input(bytes), budget(budget) {
    budget.tick(bytes.size() / 8 + 1);
    result.source_id = std::string(source);
    result.external = true;
  }
  Class read() {
    if (input.u4() != 0xcafebabe)
      bad("invalid magic");
    auto minor = input.u2();
    major = input.u2();
    if (major < 45 || major > 61 || (major == 45 ? minor > 3 : minor != 0))
      bad("unsupported version; dependency declarations require Java 1.1–17");
    constants();
    auto flags = input.u2();
    if (flags & ~unsigned(0x7631))
      bad("invalid class access flags");
    result.access = classAccess(flags & ~unsigned(0x20)); // ACC_SUPER
    result.name = classType(input.u2());
    auto super = input.u2();
    if (super)
      result.superclass = classType(super);
    auto interface_count = input.u2();
    for (unsigned i = 0; i < interface_count; ++i)
      result.interfaces.push_back(classType(input.u2()));
    auto field_count = input.u2();
    for (unsigned i = 0; i < field_count; ++i) {
      budget.tick();
      Field field;
      field.access = accessFlags(input.u2());
      field.reference = {result.name, text(input.u2()), ""};
      field.reference.type = descriptor(text(input.u2()));
      attributes(input, [&](const std::string &name, Cursor &body) {
        if (name == "Signature")
          field.generic_signature = text(body.u2());
        else if (name == "Synthetic")
          field.access.insert("synthetic");
        else
          return false;
        return true;
      });
      result.fields.push_back(std::move(field));
    }
    auto method_count = input.u2();
    for (unsigned i = 0; i < method_count; ++i) {
      budget.tick();
      Method method;
      auto access = input.u2();
      method.access = accessFlags(access & ~unsigned(0xc0));
      if (access & 0x40)
        method.access.insert("bridge");
      if (access & 0x80)
        method.access.insert("varargs");
      auto name = text(input.u2());
      auto signature = prototype(text(input.u2()));
      method.reference = {result.name, name, std::move(signature.first),
                          std::move(signature.second)};
      bool has_code = false;
      attributes(input, [&](const std::string &attribute, Cursor &body) {
        if (attribute == "Signature")
          method.generic_signature = text(body.u2());
        else if (attribute == "Exceptions") {
          std::vector<std::string> types;
          auto count = body.u2();
          for (unsigned j = 0; j < count; ++j)
            types.push_back(classType(body.u2()));
          method.declared_throws = std::move(types);
        } else if (attribute == "AnnotationDefault")
          method.annotation_default = value(body, 0);
        else if (attribute == "Synthetic")
          method.access.insert("synthetic");
        else if (attribute == "Code") {
          has_code = true;
          body.take(body.remaining());
        } else
          return false;
        return true;
      });
      if (has_code ==
          (has(method.access, "abstract") || has(method.access, "native")))
        bad("method Code attribute disagrees with access flags");
      result.methods.push_back(std::move(method));
    }
    attributes(input, [&](const std::string &name, Cursor &body) {
      if (name == "Signature")
        result.generic_signature = text(body.u2());
      else if (name == "Synthetic")
        result.access.insert("synthetic");
      else if (name == "RuntimeVisibleAnnotations")
        annotationPolicy(body);
      else if (name == "EnclosingMethod") {
        auto owner = classType(body.u2());
        auto method = body.u2();
        if (method) {
          const auto &ref = constant(method, 12);
          auto signature = prototype(text(ref.second));
          result.enclosing_method =
              MethodRef{owner, text(ref.first), std::move(signature.first),
                        std::move(signature.second)};
        } else
          result.enclosing = std::move(owner);
      } else if (name == "InnerClasses") {
        auto count = body.u2();
        bool self = false;
        for (unsigned i = 0; i < count; ++i) {
          budget.tick();
          auto inner = classType(body.u2());
          auto outer_index = body.u2();
          std::optional<std::string> outer;
          if (outer_index)
            outer = classType(outer_index);
          auto name_index = body.u2();
          std::optional<std::string> inner_name;
          if (name_index)
            inner_name = text(name_index);
          auto access = classAccess(body.u2());
          if (inner == result.name) {
            if (self)
              bad("duplicate self InnerClasses entry");
            self = true;
            result.inner_class_present = true;
            result.inner_name = inner_name;
            result.inner_access = access;
            if (outer)
              result.enclosing = outer;
          }
          if (outer == result.name && inner_name)
            if (!result.declared_member_types.emplace(*inner_name, inner)
                     .second)
              bad("duplicate member source name");
        }
      } else
        return false;
      return true;
    });
    input.end();
    return std::move(result);
  }
};
} // namespace

Class parseClassDeclaration(std::string_view bytes, std::string_view input_id,
                            Budget &budget) {
  try {
    return Reader(bytes, input_id, budget).read();
  } catch (const Error &error) {
    throw Error(std::string(input_id) + ": " + error.what());
  }
}
} // namespace neverd::mobile::dalvik
