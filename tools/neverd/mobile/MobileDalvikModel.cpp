//===- MobileDalvikModel.cpp - Shared Dalvik declaration validation
//--------===//
#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"

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
[[noreturn]] void scopeError(const Class &cls, const std::string &message) {
  std::string context =
      "Android class " + cls.name + " from " +
      (cls.source_id.empty() ? "<unspecified>" : cls.source_id);
  if (cls.enclosing_method)
    context += " enclosing " + cls.enclosing_method->identity();
  else if (cls.enclosing)
    context += " enclosing " + *cls.enclosing;
  throw Error(context + ": " + message);
}
bool scalarType(const std::string &type, bool allow_void = false) {
  return type.size() == 1 && (std::string_view("ZBCSIJFD").find(type[0]) !=
                                  std::string_view::npos ||
                              (allow_void && type == "V"));
}
void scalarSignature(const Class &cls, const MethodRef &ref, Budget &budget) {
  if (!scalarType(ref.returns, true))
    scopeError(cls, "method-local projection requires a scalar return: " +
                        ref.identity());
  for (const auto &parameter : ref.parameters) {
    budget.tick();
    if (!scalarType(parameter))
      scopeError(cls, "method-local projection requires scalar parameters: " +
                          ref.identity());
  }
}
void checkSourceScope(const Class &cls, Budget &budget) {
  if (cls.enclosing && cls.enclosing_method)
    scopeError(cls, "conflicting class and method enclosing contexts");
  if (!cls.inner_class_present) {
    if (cls.enclosing || cls.enclosing_method || cls.inner_name ||
        !cls.inner_access.empty())
      scopeError(cls, "enclosing metadata requires an InnerClass annotation");
    return;
  }
  if (!cls.enclosing && !cls.enclosing_method)
    scopeError(cls, "InnerClass annotation has no enclosing context");
  if (!cls.inner_name)
    scopeError(cls, "anonymous class source context is unsupported");
  if (cls.inner_name->empty())
    scopeError(cls, "inner class source name is empty");
  if (!cls.enclosing_method)
    return;

  const auto &enclosing = *cls.enclosing_method;
  if (!enclosing.owner.starts_with('L') || enclosing.owner == cls.name ||
      enclosing.name.empty() || enclosing.name.starts_with('<'))
    scopeError(cls, "method-local class needs an ordinary enclosing method");
  // A hand-constructed model must have the same typed identity as a reader.
  bool valid_reference = false;
  try {
    valid_reference = methodRef(enclosing.identity()) == enclosing;
  } catch (const Error &) {
  }
  if (!valid_reference)
    scopeError(cls, "invalid typed EnclosingMethod reference");
  for (const auto *flags : {&cls.access, &cls.inner_access})
    for (const auto &flag : *flags) {
      budget.tick();
      if (flag != "final" && flag != "synthetic")
        scopeError(cls, "Java 8 local class permits only final/synthetic "
                        "access flags");
    }
  if (has(cls.access, "final") != has(cls.inner_access, "final"))
    scopeError(cls,
               "local class final flag disagrees with InnerClass metadata");
  if (cls.superclass != "Ljava/lang/Object;" || !cls.interfaces.empty())
    scopeError(cls, "method-local projection requires direct Object "
                    "inheritance without interfaces");
  if (!cls.fields.empty())
    scopeError(cls, "method-local fields or capture storage are unsupported");
  unsigned constructors = 0;
  for (const auto &method : cls.methods) {
    budget.tick();
    const auto &ref = method.reference;
    if (ref.owner != cls.name)
      scopeError(cls, "local method owner disagrees with its declaration: " +
                          ref.identity());
    if (ref.name == "<clinit>")
      scopeError(cls, "method-local static initializer is unsupported");
    if (any(method.access, {"static", "native", "abstract"}) ||
        method.instructions.empty())
      scopeError(cls, "method-local methods require real instance bodies: " +
                          ref.identity());
    if (ref.name == "<init>") {
      ++constructors;
      if (!ref.parameters.empty() || ref.returns != "V")
        scopeError(cls, "method-local constructor must be no-argument; "
                        "captured constructor arguments are unsupported");
    } else {
      if (ref.name.empty() || ref.name.starts_with('<'))
        scopeError(cls,
                   "invalid ordinary local method identity: " + ref.identity());
      scalarSignature(cls, ref, budget);
    }
  }
  if (constructors != 1)
    scopeError(cls, "method-local class requires exactly one real "
                    "no-argument constructor");
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
  if (cls.enclosing || cls.enclosing_method) {
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

void linkLocalScopes(const ClassMap &classes, Budget &budget) {
  std::map<std::string, const Class *> locals;
  std::map<MethodRef, std::pair<const Method *, const Class *>> definitions;
  std::map<MethodRef, std::set<std::string>> names;
  std::set<std::string> owners;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (!cls.enclosing_method)
      continue;
    const auto &ref = *cls.enclosing_method;
    locals.emplace(name, &cls);
    definitions.emplace(ref, std::pair{nullptr, &cls});
    owners.insert(ref.owner);
    if (!names[ref].insert(*cls.inner_name).second)
      scopeError(cls, "duplicate local source name in one enclosing method");
  }
  if (locals.empty())
    return;
  // Resolve each needed owner's method table once, including cross-DEX owners.
  for (const auto &owner : owners) {
    budget.tick();
    const auto found = classes.find(owner);
    if (found == classes.end())
      continue;
    for (const auto &method : found->second.methods) {
      budget.tick();
      if (auto entry = definitions.find(method.reference);
          entry != definitions.end()) {
        if (entry->second.first)
          scopeError(*entry->second.second,
                     "exact enclosing method definition is duplicated");
        entry->second.first = &method;
      }
    }
  }
  for (const auto &[name, cls] : locals) {
    budget.tick();
    const auto &ref = *cls->enclosing_method;
    const auto *method = definitions.at(ref).first;
    if (!method)
      scopeError(*cls, "exact enclosing method definition is unavailable");
    if (classes.at(ref.owner).enclosing_method)
      scopeError(*cls, "nested method-local contexts are unsupported");
    if (!has(method->access, "static") ||
        any(method->access, {"native", "abstract"}) ||
        method->instructions.empty())
      scopeError(*cls, "enclosing method must be ordinary static with a body");
    scalarSignature(*cls, ref, budget);
  }
  auto localType = [&](const std::string &type) -> const Class * {
    budget.tick();
    auto start = type.find_first_not_of('[');
    if (start == std::string::npos)
      return nullptr;
    auto found = locals.find(type.substr(start));
    return found == locals.end() ? nullptr : found->second;
  };
  auto declarationType = [&](const std::string &type,
                             const std::string &where) {
    if (const auto *local = localType(type))
      scopeError(*local, "local type is unavailable in a declaration or "
                         "handler: " +
                             where);
  };
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.enclosing) {
      if (const auto found = locals.find(*cls.enclosing); found != locals.end())
        scopeError(*found->second,
                   "method-local nested descendants are unsupported: " + name);
    }
    if (cls.superclass)
      declarationType(*cls.superclass, name + " superclass");
    for (const auto &interface : cls.interfaces)
      declarationType(interface, name + " interface");
    for (const auto &field : cls.fields)
      declarationType(field.reference.type, name + "->" + field.reference.name);
    for (const auto &method : cls.methods) {
      budget.tick();
      const auto identity = method.reference.identity();
      declarationType(method.reference.returns, identity);
      for (const auto &type : method.reference.parameters)
        declarationType(type, identity);
      for (const auto &region : method.tries)
        for (const auto &handler : region.handlers)
          if (handler.type)
            declarationType(*handler.type, identity + " exception handler");
      auto instructionType = [&](const std::string &type) {
        if (const auto *local = localType(type))
          if (cls.name != local->name &&
              (cls.name != local->enclosing_method->owner ||
               method.reference != *local->enclosing_method))
            scopeError(*local, "local type reference is outside its exact "
                               "method scope: " +
                                   identity);
      };
      for (const auto &instruction : method.instructions) {
        budget.tick();
        if (const auto *type =
                std::get_if<std::string>(&instruction.reference)) {
          instructionType(*type);
        } else if (const auto *field =
                       std::get_if<FieldRef>(&instruction.reference)) {
          instructionType(field->owner);
          instructionType(field->type);
        } else if (const auto *ref =
                       std::get_if<MethodRef>(&instruction.reference)) {
          instructionType(ref->owner);
          instructionType(ref->returns);
          for (const auto &type : ref->parameters)
            instructionType(type);
        }
      }
    }
  }
}
} // namespace

void validateSourceScopes(const ClassMap &classes, Budget &budget) {
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (name != cls.name)
      scopeError(cls, "class map key disagrees with its descriptor");
    checkSourceScope(cls, budget);
  }
  linkLocalScopes(classes, budget);
}

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
  validateSourceScopes(result, budget);
  validateGenericSignatures(result, budget);
  return result;
}
} // namespace neverd::mobile::dalvik
