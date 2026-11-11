//===- MobileDalvikAnonymous.cpp - Anonymous allocation/source ownership --===//
#include "MobileDalvik.h"

#include <algorithm>

namespace neverd::mobile::dalvik {
namespace {
[[noreturn]] void invalid(const Class &cls, const std::string &reason) {
  throw Error("Android anonymous class " + cls.name + " from " + cls.source_id +
              ": " + reason);
}
bool refers(const std::string &type, const std::string &name) {
  auto leaf = type.find_first_not_of('[');
  return leaf != std::string::npos && type.substr(leaf) == name;
}
} // namespace

AnonymousClassPlan planAnonymousClasses(const ClassMap &classes,
                                        Budget &budget) {
  AnonymousClassPlan result;
  const auto members = planNamedAnonymousMembers(classes, budget);
  std::set<std::string> constructorless;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (!isAnonymous(cls) || isAnonymousLocal(cls) ||
        isEnumConstantClass(cls, classes))
      continue;
    if (isNamedAnonymousMember(cls, members)) {
      auto parent = classes.find(*cls.enclosing);
      if (parent == classes.end() || parent->second.external ||
          (isAnonymous(parent->second) && !isAnonymousLocal(parent->second) &&
           !isNamedAnonymousMember(parent->second, members)) ||
          has(cls.access, "interface") || has(cls.access, "abstract") ||
          has(cls.access, "enum"))
        invalid(cls,
                "captured anonymous member requires a named class enclosure");
      unsigned constructors = 0;
      for (const auto &method : cls.methods) {
        budget.tick();
        if (has(method.access, "native") || has(method.access, "abstract") ||
            method.instructions.empty())
          invalid(
              cls,
              "captured anonymous member requires original concrete bodies");
        constructors += method.reference.name == "<init>";
      }
      if (!constructors && !isConstructorlessAnonymousMember(cls))
        invalid(cls,
                "captured anonymous member requires an original constructor");
      if (isConstructorlessAnonymousMember(cls))
        constructorless.insert(name);
      continue;
    }
    if (!cls.enclosing || cls.enclosing_method ||
        !classes.contains(*cls.enclosing) ||
        classes.at(*cls.enclosing).external ||
        isAnonymous(classes.at(*cls.enclosing)))
      invalid(cls, "anonymous class source context is unsupported; expected a "
                   "known class initializer owner");
    if (cls.superclass != "Ljava/lang/Object;" || cls.interfaces.size() != 1 ||
        !classes.contains(cls.interfaces[0]) ||
        !has(classes.at(cls.interfaces[0]).access, "interface"))
      invalid(cls,
              "requires one proven interface and direct Object inheritance");
    if (!cls.fields.empty() || !cls.annotations.empty() ||
        !cls.marker_annotations.empty() || cls.deprecated || cls.suppress_lint)
      invalid(
          cls,
          "capture fields or declaration annotations require a source mapping");
    for (const auto *flags : {&cls.access, &cls.inner_access})
      for (const auto &flag : *flags)
        if (flag != "synthetic" && flag != "final" && flag != "static")
          invalid(cls, "unsupported anonymous declaration modifier");
    unsigned constructors = 0;
    for (const auto &method : cls.methods) {
      budget.tick();
      const auto &ref = method.reference;
      if (ref.name != "<init>") {
        if (ref.name == "<clinit>" || has(method.access, "static") ||
            !has(method.access, "public") || has(method.access, "native") ||
            has(method.access, "abstract") || method.instructions.empty())
          invalid(cls, "requires concrete public instance methods");
        continue;
      }
      ++constructors;
      const auto &code = method.instructions;
      if (!ref.parameters.empty() || ref.returns != "V" ||
          method.registers != 1 || !method.tries.empty() || code.size() != 2 ||
          code[0].opcode != "invoke-direct" ||
          code[0].registers != std::vector<unsigned>{0} ||
          code[0].reference !=
              Reference(MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}) ||
          code[1].opcode != "return-void" || !code[1].registers.empty() ||
          code[0].pc >= code[1].pc || code[1].pc >= method.code_end ||
          method.generic_signature || method.declared_throws ||
          method.deprecated || method.suppress_lint ||
          !method.annotations.empty() || !method.parameter_annotations.empty())
        invalid(cls,
                "constructor requires a verified empty Object initialization");
    }
    if (constructors != 1)
      invalid(cls, "requires exactly one original constructor");
    result.emplace(name, AnonymousClassSite{});
  }
  if (!constructorless.empty())
    for (const auto &[name, cls] : classes) {
      if (cls.external)
        continue;
      for (const auto &method : cls.methods)
        for (const auto &op : method.instructions) {
          budget.tick();
          if (const auto *type = std::get_if<std::string>(&op.reference);
              type && op.opcode == "new-instance" &&
              constructorless.contains(*type))
            invalid(classes.at(*type),
                    "constructorless anonymous type cannot be allocated");
          if (const auto *ref = std::get_if<MethodRef>(&op.reference);
              ref && ref->name == "<init>" &&
              constructorless.contains(ref->owner))
            invalid(
                classes.at(ref->owner),
                "constructorless anonymous type has no original constructor");
        }
    }
  if (result.empty())
    return result;
  std::map<std::string, unsigned> allocations, initializers;
  // All descriptor uses are checked before a source unit is emitted. The
  // anonymous type has no Java name: only its unique allocation and own
  // receiver calls may mention that identity.
  for (const auto &[owner, cls] : classes) {
    if (cls.external)
      continue;
    auto declaration = [&](const std::string &type) {
      auto leaf = type.find_first_not_of('[');
      if (leaf != std::string::npos && result.contains(type.substr(leaf)))
        invalid(classes.at(type.substr(leaf)),
                "anonymous identity escapes through a declaration");
    };
    if (cls.superclass)
      declaration(*cls.superclass);
    for (const auto &type : cls.interfaces)
      declaration(type);
    if (cls.enclosing && result.contains(*cls.enclosing))
      invalid(classes.at(*cls.enclosing),
              "nested anonymous declarations require a lexical mapping");
    for (const auto &field : cls.fields)
      declaration(field.reference.type);
    for (const auto &method : cls.methods) {
      budget.tick();
      declaration(method.reference.returns);
      for (const auto &type : method.reference.parameters)
        declaration(type);
      for (const auto &region : method.tries)
        for (const auto &handler : region.handlers)
          if (handler.type)
            declaration(*handler.type);
      for (size_t i = 0; i < method.instructions.size(); ++i) {
        budget.tick();
        const auto &op = method.instructions[i];
        if (const auto *type = std::get_if<std::string>(&op.reference)) {
          auto leaf = type->find_first_not_of('[');
          if (leaf == std::string::npos || !result.contains(type->substr(leaf)))
            continue;
          const auto &anonymous = classes.at(type->substr(leaf));
          if (*type != anonymous.name || op.opcode != "new-instance" ||
              method.reference.name != "<clinit>" ||
              method.reference.owner != anonymous.enclosing ||
              !has(method.access, "static") || op.registers.size() != 1)
            invalid(
                anonymous,
                "anonymous allocation must be in its exact class initializer");
          if (++allocations[anonymous.name] != 1)
            invalid(anonymous, "multiple allocation sites cannot share one "
                               "anonymous source identity");
          if (i + 1 == method.instructions.size())
            invalid(anonymous, "allocation has no adjacent constructor");
          const auto &next = method.instructions[i + 1];
          if (next.opcode != "invoke-direct" ||
              next.reference !=
                  Reference(MethodRef{anonymous.name, "<init>", {}, "V"}) ||
              next.registers != op.registers)
            invalid(anonymous, "allocation has no exact adjacent constructor");
          result.at(anonymous.name) = {method.reference, op.pc};
        } else if (const auto *field = std::get_if<FieldRef>(&op.reference)) {
          declaration(field->owner);
          declaration(field->type);
        } else if (const auto *ref = std::get_if<MethodRef>(&op.reference)) {
          declaration(ref->returns);
          for (const auto &type : ref->parameters)
            declaration(type);
          if (!result.contains(ref->owner))
            continue;
          const auto &anonymous = classes.at(ref->owner);
          if (ref->name == "<init>") {
            if (method.reference.name != "<clinit>" ||
                method.reference.owner != anonymous.enclosing || !i ||
                method.instructions[i - 1].opcode != "new-instance" ||
                method.instructions[i - 1].reference !=
                    Reference(anonymous.name))
              invalid(anonymous,
                      "constructor is not tied to the unique allocation");
            ++initializers[anonymous.name];
          } else if (owner != anonymous.name) {
            invalid(
                anonymous,
                "external invocation requires its declared interface identity");
          }
        }
      }
    }
  }
  for (const auto &[name, site] : result)
    if (allocations[name] != 1 || initializers[name] != 1)
      invalid(classes.at(name),
              "unique allocation/constructor boundary is not proven");
  return result;
}
} // namespace neverd::mobile::dalvik
