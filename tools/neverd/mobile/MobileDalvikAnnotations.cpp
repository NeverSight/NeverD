//===- MobileDalvikAnnotations.cpp - Linked annotation value contracts ---===//
#include "MobileDalvik.h"

#include <algorithm>
#include <functional>

namespace neverd::mobile::dalvik {
namespace {
[[noreturn]] void invalid(const std::string &site, const std::string &reason) {
  throw Error("Android annotation on " + site + ": " + reason);
}

class AnnotationVerifier {
  const ClassMap &classes;
  Budget &budget;
  std::set<std::string> checked_dependencies, checking_dependencies;

  const Class &definition(const std::string &type, const std::string &site) {
    auto found = classes.find(type);
    if (found == classes.end() || !has(found->second.access, "annotation"))
      invalid(site, "unresolved annotation definition: " + type);
    if (found->second.external && !checked_dependencies.contains(type)) {
      if (!checking_dependencies.insert(type).second)
        invalid(site, "cyclic dependency annotation defaults: " + type);
      const auto &cls = found->second;
      if (cls.superclass != "Ljava/lang/Object;" ||
          cls.interfaces !=
              std::vector<std::string>{"Ljava/lang/annotation/Annotation;"})
        invalid(site, "invalid dependency annotation inheritance: " + type);
      declaration(cls);
      checking_dependencies.erase(type);
      checked_dependencies.insert(type);
    }
    return found->second;
  }

  bool elementType(const std::string &type, const std::string &site) {
    if (type.starts_with('['))
      return !type.substr(1).starts_with('[') &&
             elementType(type.substr(1), site);
    if (type.size() == 1)
      return std::string_view("ZBCSIJFD").find(type[0]) !=
             std::string_view::npos;
    if (type == "Ljava/lang/String;" || type == "Ljava/lang/Class;")
      return true;
    auto found = classes.find(type);
    if (found == classes.end())
      invalid(site, "unresolved annotation element type: " + type +
                        "; supply dependency declarations with --classpath");
    return has(found->second.access, "annotation") ||
           has(found->second.access, "enum");
  }

  void elements(const std::string &type,
                const std::map<std::string, AnnotationValue> &values,
                const std::string &site, unsigned depth) {
    if (depth > 64)
      invalid(site, "annotation nesting exceeds limit");
    const auto &annotation = definition(type, site);
    std::map<std::string, const Method *> declarations;
    for (const auto &method : annotation.methods) {
      budget.tick();
      if (method.reference.name == "<clinit>")
        continue;
      if (!declarations.emplace(method.reference.name, &method).second)
        invalid(site, "overloaded annotation element: " + type);
    }
    for (const auto &[name, v] : values) {
      budget.tick();
      auto found = declarations.find(name);
      if (found == declarations.end())
        invalid(site, "unknown annotation element " + type + "->" + name);
      value(v, found->second->reference.returns,
            site + " " + type + "->" + name, depth + 1);
    }
    for (const auto &[name, method] : declarations) {
      budget.tick();
      if (!values.contains(name) && !method->annotation_default)
        invalid(site,
                "missing required annotation element " + type + "->" + name);
    }
  }

public:
  AnnotationVerifier(const ClassMap &classes, Budget &budget)
      : classes(classes), budget(budget) {}

  void marker(const MarkerAnnotation &use, const std::string &site) {
    elements(use.type, {}, site, 0);
  }

  void value(const AnnotationValue &v, const std::string &type,
             const std::string &site, unsigned depth = 0) {
    budget.tick();
    if (depth > 64)
      invalid(site, "annotation nesting exceeds limit");
    if (type.starts_with('[')) {
      if (v.kind != 0x1c || type.substr(1).starts_with('[') ||
          !std::holds_alternative<std::monostate>(v.value) ||
          !v.annotation_type.empty() || !v.elements.empty())
        invalid(site, "annotation array value disagrees with " + type);
      for (const auto &entry : v.array)
        value(entry, type.substr(1), site, depth + 1);
      return;
    }
    if (!v.array.empty() ||
        (v.kind != 0x1d &&
         (!v.elements.empty() || !v.annotation_type.empty())) ||
        (v.kind == 0x1d && !std::holds_alternative<std::monostate>(v.value)))
      invalid(site, "annotation value has conflicting payloads");
    static const std::map<std::string, unsigned> scalars{
        {"B", 0},
        {"S", 2},
        {"C", 3},
        {"I", 4},
        {"J", 6},
        {"F", 0x10},
        {"D", 0x11},
        {"Z", 0x1f},
        {"Ljava/lang/String;", 0x17},
        {"Ljava/lang/Class;", 0x18}};
    if (auto found = scalars.find(type); found != scalars.end()) {
      if (v.kind != found->second)
        invalid(site, "annotation scalar kind disagrees with " + type);
      bool valid = false;
      if (type == "Ljava/lang/String;")
        valid = std::holds_alternative<std::string>(v.value);
      else if (type == "Ljava/lang/Class;") {
        if (auto text = std::get_if<std::string>(&v.value)) {
          descriptor(*text, true);
          valid = true;
        }
      } else if (type == "Z")
        valid = std::holds_alternative<bool>(v.value);
      else if (type == "F" || type == "D") {
        if (auto bits = std::get_if<FloatBits>(&v.value))
          valid = bits->wide == (type == "D") &&
                  (bits->wide || bits->bits <= UINT32_MAX);
      } else if (auto number = std::get_if<int64_t>(&v.value)) {
        valid = type == "J" ||
                (type == "I" && *number >= INT32_MIN && *number <= INT32_MAX) ||
                (type == "S" && *number >= INT16_MIN && *number <= INT16_MAX) ||
                (type == "B" && *number >= INT8_MIN && *number <= INT8_MAX) ||
                (type == "C" && *number >= 0 && *number <= UINT16_MAX);
      }
      if (!valid)
        invalid(site, "invalid annotation scalar payload for " + type);
      return;
    }
    if (v.kind == 0x1d && v.annotation_type == type) {
      elements(type, v.elements, site, depth + 1);
      return;
    }
    if (v.kind == 0x1b && std::holds_alternative<std::string>(v.value)) {
      const auto ref = fieldRef(std::get<std::string>(v.value));
      auto owner = classes.find(type);
      if (ref.owner == type && ref.type == type && owner != classes.end() &&
          has(owner->second.access, "enum")) {
        for (const auto &field : owner->second.fields) {
          budget.tick();
          if (field.reference == ref && has(field.access, "enum") &&
              has(field.access, "static") && has(field.access, "final"))
            return;
        }
      }
    }
    invalid(site, "annotation value disagrees with " + type);
  }

  void declaration(const Class &cls) {
    bool annotation = has(cls.access, "annotation");
    for (const auto &method : cls.methods) {
      budget.tick();
      const auto &ref = method.reference;
      if (!annotation) {
        if (method.annotation_default)
          invalid(ref.identity(), "default requires an annotation declaration");
        continue;
      }
      if (ref.name == "<clinit>" && !method.annotation_default)
        continue;
      if (!ref.parameters.empty() ||
          !elementType(ref.returns, ref.identity()) ||
          !has(method.access, "abstract") || !has(method.access, "public") ||
          has(method.access, "static") || method.declared_throws ||
          !method.instructions.empty() ||
          Access{"hashCode", "toString", "annotationType", "getClass", "notify",
                 "notifyAll", "wait", "clone", "finalize"}
              .contains(ref.name))
        invalid(ref.identity(), "invalid annotation element declaration");
      if (method.annotation_default)
        value(*method.annotation_default, ref.returns, ref.identity());
    }
  }

  std::set<std::string> uses(const std::vector<AnnotationUse> &annotations,
                             const Class &owner, const std::string &target,
                             const std::string &site,
                             bool enum_constant = false) {
    std::set<std::string> seen;
    std::set<std::string> report_only;
    for (const auto &use : annotations) {
      budget.tick();
      if (!seen.insert(use.type).second || use.visibility > 1)
        invalid(site,
                "duplicate or invalid annotation visibility: " + use.type);
      const auto &cls = definition(use.type, site);
      const auto &policy = cls.annotation_metadata;
      auto retention = policy.retention.value_or("CLASS");
      if (retention == "SOURCE" ||
          use.visibility != (retention == "RUNTIME" ? 1u : 0u))
        invalid(site,
                "annotation visibility disagrees with retention: " + use.type);
      if (policy.targets) {
        const auto &targets = *policy.targets;
        auto permits = [&](const std::string &t) {
          return std::find(targets.begin(), targets.end(), t) != targets.end();
        };
        if (!permits(target) &&
            !(target == "ANNOTATION_TYPE" && permits("TYPE")) &&
            !((target == "TYPE" || target == "ANNOTATION_TYPE" ||
               target == "FIELD" || target == "PARAMETER" ||
               target == "METHOD") &&
              permits("TYPE_USE"))) {
          if (!enum_constant)
            invalid(site,
                    "annotation target excludes declaration: " + use.type);
          report_only.insert(use.type);
        }
      }
      if (target == "CLASS_INITIALIZER")
        invalid(site, "annotation cannot annotate a class initializer");
      // Class-level marker applications additionally use the established
      // lexical-access validation in validateSourceScopes. For all other
      // applications require an accessible declaration in the same package
      // or a public declaration, including its enclosing classes.
      auto package = [](const std::string &name) {
        return name.substr(0, name.rfind('/'));
      };
      const Class *scope = &cls;
      std::set<std::string> scopes;
      while (scope) {
        budget.tick();
        if (!scopes.insert(scope->name).second)
          invalid(site, "cyclic annotation ownership");
        const auto &flags =
            scope->enclosing ? scope->inner_access : scope->access;
        if (owner.name != scope->name &&
            (has(flags, "private") ||
             (package(owner.name) != package(scope->name) &&
              !has(flags, "public"))))
          invalid(site, "annotation definition is not accessible: " + use.type);
        if (!scope->enclosing)
          break;
        auto parent = classes.find(*scope->enclosing);
        if (parent == classes.end())
          invalid(site, "unresolved annotation owner");
        scope = &parent->second;
      }
      elements(use.type, use.elements, site, 0);
    }
    return report_only;
  }
};
} // namespace

AnnotationFieldInitializers
planAnnotationFieldInitializers(const ClassMap &classes, Budget &budget) {
  AnnotationFieldInitializers result;
  auto field = [&](const FieldRef &reference) -> const Field * {
    auto owner = classes.find(reference.owner);
    if (owner != classes.end())
      for (const auto &candidate : owner->second.fields) {
        budget.tick();
        if (candidate.reference == reference)
          return &candidate;
      }
    return nullptr;
  };
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external || !has(cls.access, "annotation"))
      continue;
    for (const auto &method : cls.methods) {
      if (method.reference.name != "<clinit>")
        continue;
      auto fail = [&] {
        invalid(method.reference.identity(),
                "annotation initializer requires one proven static field copy");
      };
      const auto &code = method.instructions;
      if (!has(method.access, "static") ||
          !method.reference.parameters.empty() ||
          method.reference.returns != "V" || !method.tries.empty() ||
          code.size() != 3 || method.generic_signature ||
          method.declared_throws || method.annotation_default ||
          method.deprecated || method.suppress_lint ||
          !method.annotations.empty() || !method.parameter_annotations.empty())
        fail();
      const auto *source = std::get_if<FieldRef>(&code[0].reference);
      const auto *target = std::get_if<FieldRef>(&code[1].reference);
      if (!source || !target || target->owner != name ||
          source->type != target->type || code[0].registers.size() != 1 ||
          code[0].registers != code[1].registers ||
          code[0].registers[0] >= method.registers ||
          code[2].opcode != "return-void" || !code[2].registers.empty() ||
          !std::holds_alternative<std::monostate>(code[2].reference) ||
          code[0].pc != 0 || code[0].pc >= code[1].pc ||
          code[1].pc >= code[2].pc || code[2].pc >= method.code_end)
        fail();
      for (const auto &op : code)
        if (op.target || !op.targets.empty() || !op.keys.empty() ||
            !op.data.empty() ||
            !std::holds_alternative<std::monostate>(op.literal))
          fail();
      const auto *source_field = field(*source);
      const auto *target_field = field(*target);
      if (!source_field || !target_field ||
          !has(source_field->access, "static") ||
          !has(target_field->access, "static") ||
          !has(target_field->access, "final") ||
          source_field->generic_signature || target_field->generic_signature ||
          !std::holds_alternative<std::monostate>(target_field->value))
        fail();
      const auto &type = source->type;
      auto suffix = type.starts_with('L') || type.starts_with('[') ? "-object"
                    : width(type) == 2                             ? "-wide"
                    : type == "Z"                                  ? "-boolean"
                    : type == "B"                                  ? "-byte"
                    : type == "C"                                  ? "-char"
                    : type == "S"                                  ? "-short"
                                                                   : "";
      if (code[0].opcode != std::string("sget") + suffix ||
          code[1].opcode != std::string("sput") + suffix ||
          (width(type) == 2 && code[0].registers[0] + 1 >= method.registers))
        fail();
      if (!result
               .emplace(method.reference,
                        AnnotationFieldInitializer{*source, *target})
               .second)
        fail();
    }
  }
  return result;
}

AnnotationProjections validateAnnotations(const ClassMap &classes,
                                          Budget &budget,
                                          const EnumPlans *enums) {
  const auto owned_enums =
      enums ? EnumPlans{} : planEnumClasses(classes, budget);
  const auto &enum_plans = enums ? *enums : owned_enums;
  AnnotationProjections projections;
  AnnotationVerifier verifier(classes, budget);
  for (const auto &[name, cls] : classes)
    if (!cls.external)
      verifier.declaration(cls);
  for (const auto &[name, cls] : classes) {
    if (cls.external)
      continue;
    std::set<std::string> annotation_types;
    for (const auto &marker : cls.marker_annotations) {
      annotation_types.insert(marker.type);
      verifier.marker(marker, name);
    }
    for (const auto &annotation : cls.annotations)
      if (!annotation_types.insert(annotation.type).second)
        invalid(name, "duplicate annotation application: " + annotation.type);
    verifier.uses(cls.annotations, cls,
                  has(cls.access, "annotation") ? "ANNOTATION_TYPE" : "TYPE",
                  name);
    for (const auto &field : cls.fields) {
      auto report_only = verifier.uses(
          field.annotations, cls, "FIELD", name + "->" + field.reference.name,
          enum_plans.classes.contains(name) && has(field.access, "enum"));
      if (!report_only.empty())
        projections.emplace(field.reference, std::move(report_only));
    }
    for (const auto &method : cls.methods) {
      const auto &ref = method.reference;
      auto site = ref.identity();
      if (method.parameter_metadata) {
        if (method.parameter_metadata->size() != ref.parameters.size())
          invalid(site, "parameter metadata count mismatch");
        for (const auto &parameter : *method.parameter_metadata)
          if (parameter.access & ~uint32_t(0x9010))
            invalid(site, "invalid parameter metadata access flags");
      }
      verifier.uses(method.annotations, cls,
                    ref.name == "<init>"     ? "CONSTRUCTOR"
                    : ref.name == "<clinit>" ? "CLASS_INITIALIZER"
                                             : "METHOD",
                    site);
      if (!method.parameter_annotations.empty() &&
          method.parameter_annotations.size() != ref.parameters.size())
        invalid(site, "parameter annotation count mismatch");
      for (size_t i = 0; i < method.parameter_annotations.size(); ++i)
        verifier.uses(method.parameter_annotations[i], cls, "PARAMETER",
                      site + " parameter " + std::to_string(i));
    }
  }
  return projections;
}
} // namespace neverd::mobile::dalvik
