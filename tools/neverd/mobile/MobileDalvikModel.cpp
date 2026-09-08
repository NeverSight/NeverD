//===- MobileDalvikModel.cpp - Shared Dalvik declaration validation
//--------===//
#include "MobileDalvik.h"

#include <algorithm>
#include <array>
#include <limits>

namespace neverd::mobile::dalvik {
bool has(const Access &access, std::string_view flag) {
  return access.contains(std::string(flag));
}

unsigned width(std::string_view type) {
  return type == "J" || type == "D" ? 2 : 1;
}

std::string descriptor(std::string_view value, bool allow_void) {
  if (value.empty() || value.size() > 65535)
    throw Error("invalid Dalvik type descriptor");
  size_t depth = 0;
  while (depth < value.size() && value[depth] == '[')
    ++depth;
  auto leaf = value.substr(depth);
  if (depth > 255 || leaf.empty())
    throw Error("invalid Dalvik type descriptor");
  if (leaf.size() == 1) {
    if (std::string_view("ZBCSIJFD").find(leaf[0]) != std::string_view::npos ||
        (leaf == "V" && !depth && allow_void))
      return std::string(value);
    throw Error("invalid Dalvik primitive descriptor");
  }
  if (leaf.front() != 'L' || leaf.back() != ';' || leaf.size() < 3)
    throw Error("invalid Dalvik class descriptor");
  auto body = leaf.substr(1, leaf.size() - 2);
  if (body.front() == '/' || body.back() == '/' ||
      body.find("//") != std::string_view::npos)
    throw Error("empty Dalvik package or class name");
  for (size_t pos = 0; pos < body.size();) {
    uint32_t c = static_cast<unsigned char>(body[pos++]);
    unsigned following = 0;
    if (c >= 0xc2 && c <= 0xdf) {
      following = 1;
      c &= 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
      following = 2;
      c &= 0x0f;
    } else if (c >= 0xf0 && c <= 0xf4) {
      following = 3;
      c &= 0x07;
    } else if (c >= 0x80) {
      throw Error("invalid encoding in Dalvik class descriptor");
    }
    if (following > body.size() - pos)
      throw Error("truncated encoding in Dalvik class descriptor");
    for (unsigned i = 0; i < following; ++i) {
      auto next = static_cast<unsigned char>(body[pos++]);
      if ((next & 0xc0) != 0x80)
        throw Error("invalid encoding in Dalvik class descriptor");
      c = (c << 6) | (next & 63);
    }
    // Preserve WTF-8 surrogate code units while rejecting malformed encodings.
    if ((following == 1 && c < 0x80) || (following == 2 && c < 0x800) ||
        (following == 3 && c < 0x10000) || c > 0x10ffff)
      throw Error("invalid encoding in Dalvik class descriptor");
    bool unicode_space =
        c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
        c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
    if (c <= 0x20 || c == 0x7f || unicode_space || c == ';' || c == '[' ||
        c == '.')
      throw Error("invalid character in Dalvik class descriptor");
  }
  return std::string(value);
}

std::pair<std::vector<std::string>, std::string>
prototype(std::string_view value) {
  if (value.empty() || value.front() != '(')
    throw Error("invalid Dalvik method prototype");
  auto close = value.find(')');
  if (close == std::string_view::npos)
    throw Error("invalid Dalvik method prototype");
  std::vector<std::string> parameters;
  size_t pos = 1;
  while (pos < close) {
    size_t start = pos;
    while (pos < close && value[pos] == '[')
      ++pos;
    if (pos == close)
      throw Error("invalid Dalvik parameter descriptor");
    if (value[pos] == 'L') {
      auto end = value.find(';', pos);
      if (end == std::string_view::npos || end >= close)
        throw Error("invalid Dalvik parameter descriptor");
      pos = end + 1;
    } else {
      ++pos;
    }
    parameters.push_back(descriptor(value.substr(start, pos - start)));
  }
  return {std::move(parameters), descriptor(value.substr(close + 1), true)};
}

std::string MethodRef::signature() const {
  std::string result = "(";
  for (const auto &p : parameters)
    result += p;
  return result + ")" + returns;
}
std::string MethodRef::identity() const {
  return owner + "->" + name + signature();
}
unsigned Method::incomingWords() const {
  uint64_t result = !has(access, "static");
  for (const auto &p : reference.parameters)
    result += width(p);
  if (result > 65535)
    throw Error("Dalvik incoming register frame exceeds its word limit");
  return static_cast<unsigned>(result);
}

MethodRef methodRef(std::string_view value) {
  auto arrow = value.find("->");
  auto open = value.find('(', arrow == std::string_view::npos ? 0 : arrow + 2);
  if (arrow == std::string_view::npos || open == std::string_view::npos ||
      open <= arrow + 2)
    throw Error("invalid Dalvik method reference");
  auto [parameters, returns] = prototype(value.substr(open));
  return {descriptor(value.substr(0, arrow)),
          std::string(value.substr(arrow + 2, open - arrow - 2)),
          std::move(parameters), std::move(returns)};
}

FieldRef fieldRef(std::string_view value) {
  auto arrow = value.find("->");
  auto colon = value.find(':', arrow == std::string_view::npos ? 0 : arrow + 2);
  if (arrow == std::string_view::npos || colon == std::string_view::npos ||
      colon <= arrow + 2)
    throw Error("invalid Dalvik field reference");
  return {descriptor(value.substr(0, arrow)),
          std::string(value.substr(arrow + 2, colon - arrow - 2)),
          descriptor(value.substr(colon + 1))};
}

Access accessFlags(uint32_t value) {
  static const std::pair<uint32_t, const char *> flags[] = {
      {1, "public"},
      {2, "private"},
      {4, "protected"},
      {8, "static"},
      {0x10, "final"},
      {0x20, "synchronized"},
      {0x40, "volatile"},
      {0x80, "transient"},
      {0x100, "native"},
      {0x200, "interface"},
      {0x400, "abstract"},
      {0x800, "strictfp"},
      {0x1000, "synthetic"},
      {0x2000, "annotation"},
      {0x4000, "enum"},
      {0x10000, "constructor"},
      {0x20000, "declared-synchronized"}};
  Access result;
  for (const auto &[bit, name] : flags) {
    if (value & bit)
      result.emplace(name);
    value &= ~bit;
  }
  if (value)
    throw Error("unknown Dalvik declaration access flags");
  return result;
}

namespace {
const Access visibility = {"public", "private", "protected"};
const Access classFlags = {"public",     "private",   "protected", "static",
                           "final",      "interface", "abstract",  "synthetic",
                           "annotation", "enum"};
const Access fieldFlags = {"public",    "private",   "protected",
                           "static",    "final",     "volatile",
                           "transient", "synthetic", "enum"};
const Access methodFlags = {"public",      "private",
                            "protected",   "static",
                            "final",       "synchronized",
                            "bridge",      "varargs",
                            "native",      "abstract",
                            "strictfp",    "synthetic",
                            "constructor", "declared-synchronized"};
bool any(const Access &flags, std::initializer_list<const char *> names) {
  return std::any_of(names.begin(), names.end(),
                     [&](auto name) { return has(flags, name); });
}
void checkFlags(const Access &flags, const Access &allowed,
                const std::string &id) {
  unsigned visible = 0;
  for (const auto &flag : flags) {
    if (!allowed.contains(flag))
      throw Error("invalid declaration access flags: " + id);
    visible += visibility.contains(flag);
  }
  if (visible > 1)
    throw Error("conflicting declaration visibility: " + id);
}
void checkClass(const Class &cls, Budget &budget) {
  checkFlags(cls.access, classFlags, cls.name);
  if (!descriptor(cls.name).starts_with('L'))
    throw Error("class declaration requires a class descriptor");
  if (!cls.superclass) {
    if (cls.name != "Ljava/lang/Object;")
      throw Error("class has no superclass");
  } else if (!descriptor(*cls.superclass).starts_with('L')) {
    throw Error("invalid superclass descriptor");
  }
  Access interfaces;
  for (const auto &interface : cls.interfaces) {
    budget.tick();
    if (!descriptor(interface).starts_with('L') ||
        !interfaces.insert(interface).second)
      throw Error("invalid or duplicate declared interface");
  }
  std::vector<const Access *> declarations = {&cls.access};
  if (cls.enclosing) {
    checkFlags(cls.inner_access, classFlags, cls.name);
    declarations.push_back(&cls.inner_access);
  } else if (any(cls.access, {"private", "protected", "static"})) {
    throw Error("top-level class has nested-only access flags");
  }
  for (const auto *flags : declarations) {
    if (has(*flags, "final") && any(*flags, {"abstract", "interface"}))
      throw Error("class cannot be both final and abstract/interface");
    if (has(*flags, "interface") && !has(*flags, "abstract"))
      throw Error("interface must be abstract");
    if (has(*flags, "annotation") && !has(*flags, "interface"))
      throw Error("annotation must be an interface");
  }
  bool interface = has(cls.access, "interface");
  if (interface && cls.superclass != "Ljava/lang/Object;")
    throw Error("interface has a non-Object superclass");
  for (const auto &field : cls.fields) {
    budget.tick();
    checkFlags(field.access, fieldFlags, field.reference.name);
    if (field.reference.owner != cls.name)
      throw Error("field owner disagrees with its declaration");
    descriptor(field.reference.type);
    if (has(field.access, "final") && has(field.access, "volatile"))
      throw Error("field cannot be both final and volatile");
    if (interface &&
        (!has(field.access, "public") || !has(field.access, "static") ||
         !has(field.access, "final") ||
         any(field.access, {"volatile", "transient"})))
      throw Error("invalid interface field declaration");
  }
  for (const auto &method : cls.methods) {
    budget.tick();
    const auto &ref = method.reference;
    const auto &flags = method.access;
    checkFlags(flags, methodFlags, ref.identity());
    if (ref.owner != cls.name)
      throw Error("method owner disagrees with its declaration");
    descriptor(ref.returns, true);
    for (const auto &parameter : ref.parameters) {
      budget.tick();
      descriptor(parameter);
    }
    if (has(flags, "abstract")) {
      if (!has(cls.access, "abstract"))
        throw Error("concrete class declares an abstract method");
      if (any(flags, {"static", "private", "final", "native", "synchronized",
                      "strictfp", "constructor", "declared-synchronized"}))
        throw Error("incompatible abstract method flags");
    }
    if (has(flags, "native") && has(flags, "strictfp"))
      throw Error("native method cannot be strictfp");
    if (interface && any(flags, {"protected", "final", "synchronized", "native",
                                 "declared-synchronized"}))
      throw Error("invalid interface method flags");
    if (ref.name == "<init>") {
      checkFlags(flags,
                 {"public", "private", "protected", "constructor", "synthetic",
                  "varargs"},
                 ref.identity());
      if (ref.returns != "V" || interface)
        throw Error("invalid instance initializer declaration");
    } else if (ref.name == "<clinit>") {
      checkFlags(flags, {"static", "constructor", "synthetic", "strictfp"},
                 ref.identity());
      if (!ref.parameters.empty() || ref.returns != "V" ||
          !has(flags, "static"))
        throw Error("invalid class initializer declaration");
    } else if (ref.name.starts_with('<') || has(flags, "constructor")) {
      throw Error("invalid constructor identity");
    }
  }
}
} // namespace

ClassMap linkClasses(std::vector<Class> classes, Budget &budget) {
  ClassMap result;
  for (auto &cls : classes) {
    budget.tick();
    checkClass(cls, budget);
    std::set<MethodRef> methods;
    std::set<FieldRef> fields;
    for (const auto &method : cls.methods) {
      budget.tick();
      if (!methods.insert(method.reference).second)
        throw Error("duplicate method definition");
      bool no_code =
          has(method.access, "abstract") || has(method.access, "native");
      if (no_code == !method.instructions.empty())
        throw Error("method body/access mismatch");
      if (!no_code && (method.registers < method.incomingWords() ||
                       method.registers > 65535))
        throw Error("invalid register frame");
    }
    for (const auto &field : cls.fields)
      if (!fields.insert(field.reference).second)
        throw Error("duplicate field definition");
    auto key = cls.name;
    if (!result.emplace(std::move(key), std::move(cls)).second)
      throw Error("duplicate Android class definition");
  }
  std::map<std::string, std::vector<std::string>> parents;
  for (const auto &[name, cls] : result) {
    budget.tick();
    if (cls.superclass && result.contains(*cls.superclass)) {
      const auto &parent = result.at(*cls.superclass);
      if (any(parent.access, {"interface", "final"}))
        throw Error("invalid interface or final superclass");
      parents[name].push_back(parent.name);
    }
    for (const auto &interface : cls.interfaces) {
      if (result.contains(interface)) {
        if (!has(result.at(interface).access, "interface"))
          throw Error("implemented type is not an interface");
        parents[name].push_back(interface);
      }
    }
  }
  std::map<std::string, unsigned> states;
  for (const auto &[root, cls] : result) {
    std::vector<std::pair<std::string, bool>> pending = {{root, false}};
    while (!pending.empty()) {
      budget.tick();
      auto [name, finish] = std::move(pending.back());
      pending.pop_back();
      if (finish) {
        states[name] = 2;
      } else if (states[name] != 2) {
        if (states[name] == 1)
          throw Error("cyclic class/interface inheritance");
        states[name] = 1;
        pending.emplace_back(name, true);
        for (const auto &parent : parents[name])
          pending.emplace_back(parent, false);
      }
    }
  }
  return result;
}
} // namespace neverd::mobile::dalvik
