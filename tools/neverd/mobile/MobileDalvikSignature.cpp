//===- MobileDalvikSignature.cpp - Bounded generic grammar and scope checks
// -===//
#include "MobileDalvikSignature.h"

#include <algorithm>
#include <functional>

namespace neverd::mobile::dalvik {
namespace {
[[noreturn]] void invalid(std::string_view context, std::string message) {
  throw Error("Unsupported generic declaration " + std::string(context) + ": " +
              message);
}

// Java SE 8 declaration contracts only: each is an interface with exactly
// one Object-bounded formal. This is not a method/API or hierarchy catalog.
// A declaration supplied by the input always takes precedence.
std::optional<std::string_view> platformFormal(std::string_view name) {
  if (name == "Ljava/util/List;")
    return "E";
  if (name == "Ljava/lang/Comparable;")
    return "T";
  return std::nullopt;
}

enum class BoundDeclarationKind { Class, Interface };

// Java SE 8 declaration-kind facts for supported external formal bounds.
// This table does not establish generic arity, assignability or member APIs.
// In particular, CharSequence and Serializable are not generic declarations.
std::optional<BoundDeclarationKind> platformBoundKind(std::string_view name) {
  if (name == "Ljava/lang/Object;" || name == "Ljava/lang/Number;" ||
      name == "Ljava/lang/String;" || name == "Ljava/lang/Exception;" ||
      name == "Ljava/lang/Throwable;")
    return BoundDeclarationKind::Class;
  if (name == "Ljava/lang/CharSequence;" || name == "Ljava/io/Serializable;" ||
      name == "Ljava/lang/Comparable;" || name == "Ljava/util/List;")
    return BoundDeclarationKind::Interface;
  return std::nullopt;
}

class Parser {
  std::string_view text;
  Budget &budget;
  size_t position = 0;
  GenericSignature result;

  [[noreturn]] void fail(std::string message) const {
    throw Error("Invalid JVM generic signature at byte " +
                std::to_string(position) + ": " + message);
  }
  char peek() const { return position < text.size() ? text[position] : 0; }
  char take() {
    budget.tick();
    if (position == text.size())
      fail("unexpected end");
    return text[position++];
  }
  void expect(char value) {
    if (take() != value)
      fail("expected '" + std::string(1, value) + "'");
  }
  std::string identifier() {
    size_t start = position;
    while (peek() &&
           std::string_view(".;[/<>:").find(peek()) == std::string_view::npos)
      take();
    if (start == position)
      fail("empty identifier");
    return std::string(text.substr(start, position - start));
  }
  GenericTypeId add(GenericType type) {
    budget.tick();
    if (result.types.size() >= UINT32_MAX)
      fail("too many type nodes");
    result.types.push_back(std::move(type));
    return GenericTypeId(result.types.size() - 1);
  }
  std::vector<GenericTypeArgument> arguments(unsigned depth) {
    std::vector<GenericTypeArgument> values;
    if (peek() != '<')
      return values;
    take();
    while (peek() != '>') {
      GenericTypeArgument value;
      if (peek() == '*') {
        take();
        value.variance = GenericTypeArgument::Variance::Any;
      } else {
        if (peek() == '+' || peek() == '-')
          value.variance = take() == '+'
                               ? GenericTypeArgument::Variance::Extends
                               : GenericTypeArgument::Variance::Super;
        value.type = type(true, false, depth + 1);
      }
      values.push_back(std::move(value));
    }
    take();
    if (values.empty())
      fail("empty type argument list");
    return values;
  }
  GenericTypeId type(bool reference, bool allow_void, unsigned depth = 0) {
    if (depth > 64)
      fail("type nesting exceeds limit");
    GenericType value;
    value.offset = position;
    char code = take();
    if (code == 'L') {
      value.kind = GenericType::Kind::Class;
      auto name = identifier();
      while (peek() == '/') {
        take();
        if (!value.package.empty())
          value.package += '/';
        value.package += name;
        name = identifier();
      }
      value.segments.push_back({std::move(name), arguments(depth)});
      while (peek() == '.') {
        take();
        value.segments.push_back({identifier(), arguments(depth)});
      }
      expect(';');
    } else if (code == 'T') {
      value.kind = GenericType::Kind::TypeVariable;
      value.variable_name = identifier();
      expect(';');
    } else if (code == '[') {
      unsigned dimensions = 1;
      while (peek() == '[') {
        take();
        if (++dimensions > 255)
          fail("array dimensions exceed limit");
      }
      auto element = type(false, false, depth + 1);
      for (unsigned i = 0; i < dimensions; ++i) {
        GenericType array;
        array.kind = GenericType::Kind::Array;
        array.offset = value.offset + dimensions - i - 1;
        array.element = element;
        element = add(std::move(array));
      }
      return element;
    } else if (!reference && code == 'V' && allow_void) {
      value.kind = GenericType::Kind::Void;
    } else if (!reference && std::string_view("BCDFIJSZ").find(code) !=
                                 std::string_view::npos) {
      value.kind = GenericType::Kind::Primitive;
      value.primitive = code;
    } else {
      fail("invalid type tag");
    }
    return add(std::move(value));
  }
  void formals() {
    if (peek() != '<')
      return;
    take();
    std::set<std::string> names;
    while (peek() != '>') {
      GenericTypeParameter formal;
      formal.name = identifier();
      if (!names.insert(formal.name).second)
        fail("duplicate type parameter");
      expect(':');
      if (peek() == 'L' || peek() == 'T' || peek() == '[')
        formal.class_bound = type(true, false);
      while (peek() == ':') {
        take();
        formal.interface_bounds.push_back(type(true, false));
      }
      result.type_parameters.push_back(std::move(formal));
    }
    take();
    if (result.type_parameters.empty())
      fail("empty type parameter list");
  }

public:
  Parser(GenericSignatureKind kind, std::string_view text, Budget &budget)
      : text(text), budget(budget) {
    result.kind = kind;
  }
  GenericSignature parse() {
    budget.check();
    if (text.empty() || text.size() > budget.limits.max_bytes)
      fail("empty or oversized signature");
    if (result.kind == GenericSignatureKind::Field) {
      result.field_type = type(true, false);
    } else {
      formals();
      if (result.kind == GenericSignatureKind::Class) {
        result.superclass = type(true, false);
        if (result.types[*result.superclass].kind != GenericType::Kind::Class)
          fail("superclass is not a class type");
        while (peek()) {
          auto id = type(true, false);
          if (result.types[id].kind != GenericType::Kind::Class)
            fail("interface is not a class type");
          result.interfaces.push_back(id);
        }
      } else {
        expect('(');
        while (peek() != ')')
          result.parameters.push_back(type(false, false));
        take();
        result.result = type(false, true);
        while (peek() == '^') {
          take();
          auto id = type(true, false);
          auto kind = result.types[id].kind;
          if (kind != GenericType::Kind::Class &&
              kind != GenericType::Kind::TypeVariable)
            fail("throws type is not a class or type variable");
          result.throws_types.push_back(id);
        }
      }
    }
    if (position != text.size())
      fail("trailing input");
    return std::move(result);
  }
};

struct Binding {
  GenericSignature *signature;
  size_t index;
  std::string owner;
};
using Scope = std::map<std::string, Binding>;
struct TypeUse {
  GenericSignature *signature;
  GenericTypeId id;
  const Scope *scope;
};
using Substitutions = std::map<std::pair<std::string, std::string>, TypeUse>;

class Validator {
  const ClassMap &classes;
  Budget &budget;
  GenericSignaturePlans plans;
  std::map<std::string, Scope> class_scopes;
  std::set<std::string> pending_classes;
  std::set<std::pair<std::string, size_t>> pending_formals;

  GenericSignature parse(GenericSignatureKind kind, const std::string &raw,
                         const std::string &context) {
    try {
      return parseGenericSignature(kind, raw, budget);
    } catch (const Error &error) {
      invalid(context, error.what());
    }
  }
  void introduce(GenericSignature &signature, Scope &scope,
                 const std::string &owner) {
    for (size_t i = 0; i < signature.type_parameters.size(); ++i) {
      const auto &name = signature.type_parameters[i].name;
      budget.tick(1 + (owner.size() + name.size()) / 16);
      scope[name] = {&signature, i, owner};
    }
    for (const auto &[name, binding] : scope) {
      budget.tick(1 + name.size() / 16);
      signature.visible_type_variables.insert(name);
    }
  }
  Scope copyScope(const Scope &scope) {
    for (const auto &[name, binding] : scope)
      budget.tick(1 + (name.size() + binding.owner.size()) / 16);
    return scope;
  }
  std::string formal(const Binding &binding, const Scope &scope,
                     const std::string &context, unsigned depth) {
    auto &parameter = binding.signature->type_parameters[binding.index];
    if (!parameter.erasure.empty())
      return parameter.erasure;
    auto key = std::pair{binding.owner, binding.index};
    if (!pending_formals.insert(key).second)
      invalid(context, "cyclic type-variable erasure");
    auto bound = parameter.class_bound;
    if (!bound && !parameter.interface_bounds.empty())
      bound = parameter.interface_bounds.front();
    if (!bound)
      invalid(context, "type parameter has no representable bound");
    parameter.erasure =
        erase(*binding.signature, *bound, scope, context, depth + 1);
    pending_formals.erase(key);
    return parameter.erasure;
  }
  std::string erase(GenericSignature &signature, GenericTypeId id,
                    const Scope &scope, const std::string &context,
                    unsigned depth = 0) {
    budget.tick();
    if (depth > 512)
      invalid(context, "erasure dependency depth exceeds limit");
    auto &node = signature.types.at(id);
    if (!node.erasure.empty()) {
      budget.tick(1 + node.erasure.size() / 16);
      return node.erasure;
    }
    switch (node.kind) {
    case GenericType::Kind::Primitive:
      node.erasure = node.primitive;
      break;
    case GenericType::Kind::Void:
      node.erasure = "V";
      break;
    case GenericType::Kind::Array:
      node.erasure =
          "[" + erase(signature, *node.element, scope, context, depth + 1);
      break;
    case GenericType::Kind::Class: {
      std::string name = "L";
      if (!node.package.empty())
        name += node.package + '/';
      for (size_t i = 0; i < node.segments.size(); ++i) {
        if (i)
          name += '$';
        name += node.segments[i].name;
      }
      node.erasure = descriptor(name + ';');
      break;
    }
    case GenericType::Kind::TypeVariable: {
      auto found = scope.find(node.variable_name);
      if (found == scope.end())
        invalid(context, "unbound type variable " + node.variable_name);
      node.variable_owner = found->second.owner;
      // An outer class formal's bounds use its declaring environment, even
      // when a method formal shadows a name mentioned inside those bounds.
      const Scope *owner_scope = &scope;
      if (classes.contains(found->second.owner))
        owner_scope = &classScope(found->second.owner);
      node.erasure = formal(found->second, *owner_scope, context, depth + 1);
      break;
    }
    }
    descriptor(node.erasure, true);
    budget.tick(1 + node.erasure.size() / 16);
    return node.erasure;
  }
  TypeUse substitute(TypeUse use, const Substitutions &substitutions,
                     const std::string &context) {
    erase(*use.signature, use.id, *use.scope, context);
    const auto &node = use.signature->types[use.id];
    if (node.kind == GenericType::Kind::TypeVariable) {
      auto found =
          substitutions.find({*node.variable_owner, node.variable_name});
      if (found != substitutions.end())
        return found->second;
    }
    return use;
  }
  bool sameType(TypeUse actual, TypeUse expected,
                const Substitutions &substitutions, const std::string &context,
                unsigned depth) {
    budget.tick();
    if (depth > 64)
      invalid(context, "generic bound comparison exceeds depth limit");
    expected = substitute(expected, substitutions, context);
    erase(*actual.signature, actual.id, *actual.scope, context);
    const auto &a = actual.signature->types[actual.id];
    const auto &e = expected.signature->types[expected.id];
    if (a.kind != e.kind)
      return false;
    if (a.kind == GenericType::Kind::TypeVariable)
      return a.variable_owner == e.variable_owner &&
             a.variable_name == e.variable_name;
    if (a.kind == GenericType::Kind::Array)
      return sameType({actual.signature, *a.element, actual.scope},
                      {expected.signature, *e.element, expected.scope},
                      substitutions, context, depth + 1);
    if (a.erasure != e.erasure || a.segments.size() != e.segments.size())
      return false;
    for (size_t i = 0; i < a.segments.size(); ++i) {
      const auto &aa = a.segments[i].arguments;
      const auto &ea = e.segments[i].arguments;
      if (aa.size() != ea.size())
        return false;
      for (size_t j = 0; j < aa.size(); ++j) {
        if (aa[j].variance != ea[j].variance ||
            bool(aa[j].type) != bool(ea[j].type))
          return false;
        if (aa[j].type &&
            !sameType({actual.signature, *aa[j].type, actual.scope},
                      {expected.signature, *ea[j].type, expected.scope},
                      substitutions, context, depth + 1))
          return false;
      }
    }
    return true;
  }
  bool satisfies(TypeUse actual, TypeUse expected,
                 const Substitutions &substitutions, const std::string &context,
                 unsigned depth = 0) {
    budget.tick();
    if (depth > 64)
      invalid(context, "generic bound proof exceeds depth limit");
    expected = substitute(expected, substitutions, context);
    if (sameType(actual, expected, {}, context, depth))
      return true;
    erase(*actual.signature, actual.id, *actual.scope, context);
    erase(*expected.signature, expected.id, *expected.scope, context);
    const auto &a = actual.signature->types[actual.id];
    const auto &e = expected.signature->types[expected.id];
    if (e.erasure == "Ljava/lang/Object;" && e.kind == GenericType::Kind::Class)
      return a.kind != GenericType::Kind::Primitive &&
             a.kind != GenericType::Kind::Void;
    if (a.kind == GenericType::Kind::TypeVariable) {
      const Scope *scope = actual.scope;
      if (classes.contains(*a.variable_owner))
        scope = &classScope(*a.variable_owner);
      auto binding = scope->find(a.variable_name);
      if (binding == scope->end() || binding->second.owner != *a.variable_owner)
        return false;
      const auto &formal =
          binding->second.signature->type_parameters[binding->second.index];
      auto prove = [&](GenericTypeId bound) {
        return satisfies({binding->second.signature, bound, scope}, expected,
                         substitutions, context, depth + 1);
      };
      if (formal.class_bound && prove(*formal.class_bound))
        return true;
      return std::any_of(formal.interface_bounds.begin(),
                         formal.interface_bounds.end(), prove);
    }
    if (e.kind != GenericType::Kind::Class)
      return false;
    bool parameterized = std::any_of(
        e.segments.begin(), e.segments.end(),
        [](const auto &segment) { return !segment.arguments.empty(); });
    // Matching instantiated bounds may be proved structurally. Substitution
    // along a different generic superclass path needs a separate proof.
    if (parameterized)
      return sameType(actual, expected, substitutions, context, depth);
    if (a.kind == GenericType::Kind::Array)
      return e.erasure == "Ljava/lang/Cloneable;" ||
             e.erasure == "Ljava/io/Serializable;";
    if (a.kind != GenericType::Kind::Class)
      return false;
    std::vector<std::string> pending{a.erasure};
    std::set<std::string> seen;
    while (!pending.empty()) {
      budget.tick();
      auto name = std::move(pending.back());
      pending.pop_back();
      if (name == e.erasure)
        return true;
      if (!seen.insert(name).second)
        continue;
      if (seen.size() > 1024)
        invalid(context, "generic bound ancestry exceeds limit");
      auto found = classes.find(name);
      if (found == classes.end())
        continue;
      if (found->second.superclass)
        pending.push_back(*found->second.superclass);
      pending.insert(pending.end(), found->second.interfaces.begin(),
                     found->second.interfaces.end());
    }
    return false;
  }
  void arguments(GenericSignature &signature, const GenericType &node,
                 const Scope &scope, const std::string &context) {
    auto declaration = plans.classes.find(node.erasure);
    size_t expected = declaration == plans.classes.end()
                          ? 0
                          : declaration->second.type_parameters.size();
    const auto &arguments = node.segments.front().arguments;
    if (arguments.size() != expected)
      invalid(context, "type argument count disagrees with declaration");
    auto &target = declaration->second;
    const auto &target_scope = classScope(node.erasure);
    Substitutions substitutions;
    for (size_t i = 0; i < arguments.size(); ++i) {
      budget.tick();
      if (arguments[i].type)
        substitutions.emplace(
            std::pair{node.erasure, target.type_parameters[i].name},
            TypeUse{&signature, *arguments[i].type, &scope});
    }
    for (size_t i = 0; i < arguments.size(); ++i) {
      if (!arguments[i].type)
        continue;
      const auto &parameter = target.type_parameters[i];
      auto check = [&](GenericTypeId bound) {
        if (!satisfies({&signature, *arguments[i].type, &scope},
                       {&target, bound, &target_scope}, substitutions, context))
          invalid(context, "type argument bound is not proven for " +
                               node.erasure + " parameter " + parameter.name);
      };
      if (parameter.class_bound)
        check(*parameter.class_bound);
      for (auto bound : parameter.interface_bounds)
        check(bound);
    }
  }
  void resolve(GenericSignature &signature, const Scope &scope,
               const std::string &context) {
    for (GenericTypeId i = 0; i < signature.types.size(); ++i)
      erase(signature, i, scope, context);
    for (size_t i = 0; i < signature.type_parameters.size(); ++i) {
      auto &parameter = signature.type_parameters[i];
      formal(scope.at(parameter.name), scope, context, 0);
      std::set<std::string> bounds;
      auto check = [&](GenericTypeId id, bool is_interface) {
        const auto &node = signature.types[id];
        if (node.kind != GenericType::Kind::Class &&
            (!parameter.interface_bounds.empty() || is_interface ||
             node.kind != GenericType::Kind::TypeVariable))
          invalid(context, "bound cannot be expressed as a Java type bound");
        if (!bounds.insert(node.erasure).second)
          invalid(context, "duplicate bound erasure");
        if (node.kind == GenericType::Kind::Class) {
          auto known = classes.find(node.erasure);
          std::optional<BoundDeclarationKind> kind;
          if (known != classes.end())
            kind = has(known->second.access, "interface")
                       ? BoundDeclarationKind::Interface
                       : BoundDeclarationKind::Class;
          else
            kind = platformBoundKind(node.erasure);
          if (!kind)
            invalid(context,
                    "formal-bound declaration kind is not proven for " +
                        node.erasure);
          if (is_interface && *kind != BoundDeclarationKind::Interface)
            invalid(context, "interface bound names a known class");
          if (!is_interface && *kind != BoundDeclarationKind::Class)
            invalid(context, "class-bound position names a known interface");
        }
      };
      if (parameter.class_bound)
        check(*parameter.class_bound, false);
      for (auto id : parameter.interface_bounds)
        check(id, true);
    }
    for (const auto &node : signature.types) {
      budget.tick();
      if (node.kind != GenericType::Kind::Class)
        continue;
      if (node.segments.size() > 1 &&
          std::any_of(node.segments.begin(), node.segments.end(),
                      [](const auto &s) { return !s.arguments.empty(); }))
        invalid(context, "parameterized inner owner binding is not proven");
      auto known = classes.find(node.erasure);
      if (known != classes.end() && known->second.enclosing_method)
        invalid(context, "generic reference to a method-local class is not "
                         "supported");
      if (node.segments.size() == 1 &&
          !node.segments.front().arguments.empty()) {
        if (known != classes.end()) {
          arguments(signature, node, scope, context);
        } else if (platformFormal(node.erasure)) {
          if (node.segments.front().arguments.size() != 1)
            invalid(context, "platform type argument count mismatch");
        } else {
          invalid(context, "external generic declaration is not proven for " +
                               node.erasure);
        }
      }
    }
  }
  const Scope &classScope(const std::string &name) {
    auto found = class_scopes.find(name);
    if (found != class_scopes.end())
      return found->second;
    if (pending_classes.size() >= 64 || !pending_classes.insert(name).second)
      invalid(name, "enclosing generic scope is cyclic or too deep");
    const auto &cls = classes.at(name);
    Scope scope;
    if (cls.enclosing && !has(cls.inner_access, "static")) {
      if (!classes.contains(*cls.enclosing))
        invalid(name, "enclosing generic declaration is missing");
      const auto &outer_scope = classScope(*cls.enclosing);
      scope = copyScope(outer_scope);
    }
    auto declaration = plans.classes.find(name);
    if (declaration != plans.classes.end())
      introduce(declaration->second, scope, name);
    auto &stored = class_scopes.emplace(name, std::move(scope)).first->second;
    pending_classes.erase(name);
    return stored;
  }
  void throwable(const std::string &type, const std::string &context) {
    descriptor(type);
    if (!type.starts_with('L'))
      invalid(context, "throws entry is not a class");
    std::set<std::string> seen;
    std::string current = type;
    while (true) {
      budget.tick();
      if (current == "Ljava/lang/Throwable;" ||
          current == "Ljava/lang/Exception;" ||
          current == "Ljava/lang/RuntimeException;" ||
          current == "Ljava/lang/Error;")
        return;
      if (seen.size() >= 64 || !seen.insert(current).second)
        invalid(context, "throws hierarchy is cyclic or too deep");
      auto found = classes.find(current);
      if (found == classes.end() || !found->second.superclass ||
          has(found->second.access, "interface"))
        invalid(context, "throws hierarchy is not proven for " + type);
      current = *found->second.superclass;
    }
  }

public:
  Validator(const ClassMap &classes, Budget &budget)
      : classes(classes), budget(budget) {}
  GenericSignaturePlans validate() {
    // Parse every declaration before binding any formal, including forward
    // references to another class's formal count.
    for (const auto &[name, cls] : classes) {
      budget.tick();
      if (name != cls.name)
        invalid(name, "class map identity mismatch");
      if (cls.generic_signature)
        plans.classes.emplace(name, parse(GenericSignatureKind::Class,
                                          *cls.generic_signature,
                                          name + " from " + cls.source_id));
      for (const auto &field : cls.fields) {
        budget.tick();
        if (field.reference.owner != name)
          invalid(name, "field owner mismatch");
        if (field.generic_signature &&
            !plans.fields
                 .emplace(field.reference,
                          parse(GenericSignatureKind::Field,
                                *field.generic_signature,
                                name + "->" + field.reference.name))
                 .second)
          invalid(name, "duplicate generic field identity");
      }
      for (const auto &method : cls.methods) {
        budget.tick();
        const auto &ref = method.reference;
        if (ref.owner != name)
          invalid(ref.identity(), "method owner mismatch");
        if (method.generic_signature &&
            !plans.methods
                 .emplace(ref, parse(GenericSignatureKind::Method,
                                     *method.generic_signature, ref.identity()))
                 .second)
          invalid(ref.identity(), "duplicate generic method identity");
        if (method.declared_throws &&
            !plans.declared_throws.emplace(ref, *method.declared_throws).second)
          invalid(ref.identity(), "duplicate throws method identity");
      }
    }
    for (const auto &[name, cls] : classes) {
      const auto &scope = classScope(name);
      auto class_plan = plans.classes.find(name);
      if (class_plan != plans.classes.end()) {
        if (cls.enclosing_method)
          invalid(name, "generic method-local class scope is not supported");
        auto &signature = class_plan->second;
        resolve(signature, scope, name);
        if (!cls.superclass ||
            signature.types[*signature.superclass].erasure != *cls.superclass)
          invalid(name, "superclass erasure mismatch");
        if (!classes.contains(*cls.superclass) &&
            platformFormal(*cls.superclass))
          invalid(name, "superclass names a known platform interface");
        std::vector<std::string> interfaces;
        for (auto id : signature.interfaces)
          interfaces.push_back(signature.types[id].erasure);
        if (interfaces != cls.interfaces)
          invalid(name, "interface erasure mismatch");
      }
      for (const auto &field : cls.fields) {
        auto found = plans.fields.find(field.reference);
        if (found == plans.fields.end())
          continue;
        auto &signature = found->second;
        bool is_static = has(field.access, "static");
        Scope environment = is_static ? Scope{} : copyScope(scope);
        introduce(signature, environment, name);
        resolve(signature, environment, name + "->" + field.reference.name);
        if (signature.types[*signature.field_type].erasure !=
            field.reference.type)
          invalid(name + "->" + field.reference.name, "field erasure mismatch");
      }
      for (const auto &method : cls.methods) {
        const auto &ref = method.reference;
        auto context = ref.identity();
        if (method.declared_throws) {
          if (ref.name == "<clinit>")
            invalid(context, "class initializer cannot declare throws");
          std::set<std::string> seen;
          for (const auto &type : *method.declared_throws) {
            throwable(type, context);
            if (!seen.insert(type).second)
              invalid(context, "duplicate throws entry");
          }
        }
        auto found = plans.methods.find(ref);
        if (found == plans.methods.end())
          continue;
        if (ref.name == "<clinit>" || cls.enclosing_method)
          invalid(context, "generic initializer or method-local scope is not "
                           "supported");
        auto &signature = found->second;
        bool is_static = has(method.access, "static");
        Scope environment = is_static ? Scope{} : copyScope(scope);
        introduce(signature, environment, context);
        resolve(signature, environment, context);
        std::vector<std::string> parameters;
        for (auto id : signature.parameters)
          parameters.push_back(signature.types[id].erasure);
        if (parameters != ref.parameters)
          invalid(context, ref.name == "<init>"
                               ? "constructor implicit parameter mapping is "
                                 "not proven"
                               : "parameter erasure mismatch");
        if (signature.types[*signature.result].erasure != ref.returns)
          invalid(context, "return erasure mismatch");
        if (!signature.throws_types.empty()) {
          std::vector<std::string> throws_types;
          for (auto id : signature.throws_types) {
            auto type = signature.types[id].erasure;
            throwable(type, context);
            throws_types.push_back(std::move(type));
          }
          if (!method.declared_throws ||
              throws_types != *method.declared_throws)
            invalid(context, "generic throws erasure disagrees with Throws "
                             "annotation");
        }
      }
    }
    return std::move(plans);
  }
};
} // namespace

GenericSignature parseGenericSignature(GenericSignatureKind kind,
                                       std::string_view signature,
                                       Budget &budget) {
  return Parser(kind, signature, budget).parse();
}
GenericSignaturePlans validateGenericSignatures(const ClassMap &classes,
                                                Budget &budget) {
  return Validator(classes, budget).validate();
}
} // namespace neverd::mobile::dalvik
