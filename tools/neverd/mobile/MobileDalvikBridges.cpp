//===- MobileDalvikBridges.cpp - Proven generic inheritance bridge mapping
//--===//
#include "MobileDalvikSignature.h"

#include <algorithm>
#include <functional>
#include <limits>

namespace neverd::mobile::dalvik {
namespace {
using SourceMethodNames =
    std::map<MethodRef, GenericInheritancePlan::SourceMethodName>;

// These implicit Object slots need their own complete inheritance proof.
const std::set<std::string> &objectSourceMethodNames() {
  static const std::set<std::string> names{"getClass",  "hashCode", "equals",
                                           "clone",     "toString", "notify",
                                           "notifyAll", "wait",     "finalize"};
  return names;
}

bool sameSourceMethodName(const MethodRef &left, const MethodRef &right,
                          const SourceMethodNames *names) {
  auto spelling = [&](const MethodRef &reference) -> std::string_view {
    if (names)
      if (const auto found = names->find(reference); found != names->end())
        return found->second.name;
    return reference.name;
  };
  return spelling(left) == spelling(right);
}

[[noreturn]] void unsupported(const std::string &context,
                              const std::string &message) {
  throw Error("generic inheritance/bridge " + context + ": " + message);
}

struct SubstitutedType {
  std::string descriptor;
  // Equality key, not serialized JVM metadata: scoped open variables include
  // their common-validator-resolved declaration scope.
  std::string signature;
  bool parameterized = false;
  bool open = false;
  std::string class_name;
  std::vector<
      std::vector<std::pair<GenericTypeArgument::Variance, std::string>>>
      class_arguments;
};

std::string originalClassType(const Class &cls, size_t offset, Budget &budget) {
  if (!cls.generic_signature || offset >= cls.generic_signature->size() ||
      cls.generic_signature->at(offset) != 'L')
    unsupported(cls.name, "original interface Signature offset is invalid");
  const auto &text = *cls.generic_signature;
  size_t depth = 0;
  for (size_t i = offset; i < text.size(); ++i) {
    budget.tick();
    if (text[i] == '<')
      ++depth;
    else if (text[i] == '>') {
      if (!depth)
        unsupported(cls.name, "original interface Signature is unbalanced");
      --depth;
    } else if (text[i] == ';' && !depth)
      return text.substr(offset, i - offset + 1);
  }
  unsupported(cls.name, "original interface Signature is unterminated");
}

SubstitutedType
substitute(const GenericSignature &signature, GenericTypeId id,
           const std::map<std::string, SubstitutedType> &bindings,
           const std::string &context, Budget &budget, unsigned depth = 0,
           bool scoped_formals = false) {
  budget.tick();
  if (depth > 64)
    unsupported(context, "substitution nesting exceeds limit");
  const auto &node = signature.types.at(id);
  if (node.kind == GenericType::Kind::Primitive) {
    const auto text = std::string(1, node.primitive);
    return {text, text};
  }
  if (node.kind == GenericType::Kind::Void)
    return {"V", "V"};
  if (node.kind == GenericType::Kind::Array) {
    auto element = substitute(signature, *node.element, bindings, context,
                              budget, depth + 1, scoped_formals);
    element.descriptor = "[" + element.descriptor;
    element.signature = "[" + element.signature;
    if (!element.class_name.empty())
      element.class_name = "[" + element.class_name;
    return element;
  }
  if (node.kind == GenericType::Kind::TypeVariable) {
    auto found = bindings.find(node.variable_name);
    if (found != bindings.end())
      return found->second;
    // Lexical availability, declaration identity, bounds and runtime erasure
    // belong to the common validator. Keep that exact resolved binding when a
    // local override consumes its class or enclosing declaration's formal.
    if (scoped_formals && node.variable_owner && !node.erasure.empty() &&
        signature.visible_type_variables.contains(node.variable_name))
      return {node.erasure,
              "T[" + *node.variable_owner + "]" + node.variable_name + ";",
              false, true};
    unsupported(context, "open type-variable substitution is not proven");
  }
  std::string runtime = "L" + node.package;
  if (!node.package.empty())
    runtime += '/';
  std::string source = runtime;
  bool parameterized = false, open = false;
  SubstitutedType result;
  result.class_name = runtime;
  for (size_t i = 0; i < node.segments.size(); ++i) {
    const auto &segment = node.segments[i];
    if (i) {
      runtime += '$';
      source += '.';
      result.class_name += '.';
    }
    runtime += segment.name;
    source += segment.name;
    result.class_name += segment.name;
    auto &argument_types = result.class_arguments.emplace_back();
    if (!segment.arguments.empty()) {
      parameterized = true;
      source += '<';
      for (const auto &argument : segment.arguments) {
        budget.tick();
        const auto variance =
            canonicalGenericArgumentVariance(signature, argument, budget);
        if (variance == GenericTypeArgument::Variance::Any) {
          if (argument.variance == GenericTypeArgument::Variance::Any &&
              argument.type)
            unsupported(context, "unbounded member argument has a type");
          source += '*';
          argument_types.emplace_back(variance, "");
          continue;
        }
        if (!argument.type)
          unsupported(context, "member argument has no type");
        auto child = substitute(signature, *argument.type, bindings, context,
                                budget, depth + 1, scoped_formals);
        // A closed binding can become literal Object only after substitution.
        // Scoped formals with Object erasure keep their distinct T[...] key.
        const auto resolved_variance = canonicalGenericArgumentVariance(
            variance, child.signature == "Ljava/lang/Object;", budget);
        if (resolved_variance == GenericTypeArgument::Variance::Any) {
          source += '*';
          argument_types.emplace_back(resolved_variance, "");
          continue;
        }
        if (resolved_variance == GenericTypeArgument::Variance::Extends)
          source += '+';
        else if (resolved_variance == GenericTypeArgument::Variance::Super)
          source += '-';
        source += child.signature;
        open |= child.open;
        argument_types.emplace_back(resolved_variance,
                                    std::move(child.signature));
      }
      source += '>';
    }
  }
  result.descriptor = descriptor(runtime + ";");
  result.signature = source + ";";
  result.parameterized = parameterized;
  result.open = open;
  result.class_name += ';';
  return result;
}

bool sameOrContainedType(const SubstitutedType &actual,
                         const SubstitutedType &expected, Budget &budget) {
  if (actual.signature == expected.signature)
    return true;
  if (actual.descriptor != expected.descriptor || expected.class_name.empty() ||
      actual.class_name != expected.class_name ||
      actual.class_arguments.size() != expected.class_arguments.size())
    return false;
  for (size_t segment = 0; segment < expected.class_arguments.size();
       ++segment) {
    const auto &wanted = expected.class_arguments[segment];
    const auto &provided = actual.class_arguments[segment];
    if (wanted.size() != provided.size())
      return false;
    for (size_t i = 0; i < wanted.size(); ++i) {
      budget.tick();
      if (wanted[i].first == GenericTypeArgument::Variance::Any)
        continue;
      if (wanted[i] == provided[i])
        continue;
      if (wanted[i].second != provided[i].second ||
          provided[i].first != GenericTypeArgument::Variance::Invariant ||
          (wanted[i].first != GenericTypeArgument::Variance::Extends &&
           wanted[i].first != GenericTypeArgument::Variance::Super))
        return false;
    }
  }
  return true;
}

bool closedParameterizedRawType(const SubstitutedType &actual,
                                const SubstitutedType &expected) {
  // A validated closed parameterization retains this exact raw class.
  // Consumers use this only for result covariance or the separately bounded
  // whole erased subsignature; typed parameters still retain every argument.
  return actual.parameterized && !actual.open && !expected.parameterized &&
         !expected.open && expected.signature == expected.descriptor &&
         actual.descriptor == expected.descriptor &&
         actual.descriptor.starts_with('L') &&
         actual.class_name == actual.descriptor &&
         actual.class_arguments.size() == 1;
}

bool sameOrContainedReturnType(const SubstitutedType &actual,
                               const SubstitutedType &expected,
                               Budget &budget) {
  return sameOrContainedType(actual, expected, budget) ||
         closedParameterizedRawType(actual, expected);
}

const GenericSignature *
covariantBridgeContract(const MethodRef &reference,
                        const GenericSignaturePlans &signatures,
                        Budget &budget) {
  const GenericSignature *contract = nullptr;
  std::vector<std::string> identity;
  auto proof = signatures.covariant_bridge_contracts.lower_bound(
      {reference, MethodRef{}});
  for (; proof != signatures.covariant_bridge_contracts.end() &&
         proof->first.first == reference;
       ++proof) {
    budget.tick();
    const auto &candidate = proof->second.contract;
    std::vector<std::string> shape;
    for (const auto id : candidate.parameters)
      shape.push_back(
          substitute(candidate, id, {}, reference.owner, budget, 0, true)
              .signature);
    shape.push_back(substitute(candidate, *candidate.result, {},
                               reference.owner, budget, 0, true)
                        .signature);
    if (contract && shape != identity)
      unsupported(reference.identity(),
                  "original covariant bridge has conflicting full contracts");
    contract = &candidate;
    identity = std::move(shape);
  }
  return contract;
}

GenericTypeId methodFormalBound(const GenericSignature &signature,
                                const GenericTypeParameter &formal,
                                const std::string &context, Budget &budget,
                                const ClassMap *nominal_classes = nullptr) {
  budget.tick();
  const bool interface_bound =
      !formal.class_bound && formal.interface_bounds.size() == 1;
  if ((!formal.class_bound && !interface_bound) ||
      (formal.class_bound && !formal.interface_bounds.empty()))
    unsupported(context,
                "inherited method formals require one exact validated bound");
  const auto id =
      interface_bound ? formal.interface_bounds[0] : *formal.class_bound;
  const auto &bound = signature.types.at(id);
  if (!interface_bound && bound.kind == GenericType::Kind::TypeVariable &&
      bound.erasure == formal.erasure && bound.variable_owner &&
      signature.visible_type_variables.contains(bound.variable_name))
    // The common resolver owns availability, bound cycles and exact declaring
    // scope. Keep this original node for the complete scoped alpha comparison;
    // its erasure alone cannot replace a class or another method formal.
    return id;
  if (bound.kind != GenericType::Kind::Class || bound.erasure != formal.erasure)
    unsupported(context, "inherited method formals require a class or "
                         "single interface bound");
  if (formal.erasure != "Ljava/lang/Object;" || interface_bound) {
    const auto known = nominal_classes ? nominal_classes->find(formal.erasure)
                                       : ClassMap::const_iterator{};
    if (!nominal_classes || known == nominal_classes->end() ||
        has(known->second.access, "interface") != interface_bound)
      unsupported(context, "inherited method formals require known "
                           "class or interface bounds with exact roles");
  }
  // The common validator already proved each argument, variable scope and
  // bound. Preserve parameterized class bounds, including Enum<E>, for the
  // same alpha-equivalent full-bound comparison used by interface bounds.
  return id;
}

std::map<std::string, SubstitutedType> methodFormalBindings(
    const GenericSignature &signature, const std::string &context,
    Budget &budget, const ClassMap *nominal_classes = nullptr,
    const std::map<std::string, SubstitutedType> *class_bindings = nullptr) {
  std::map<std::string, SubstitutedType> result;
  std::map<std::string, size_t> indices;
  std::vector<GenericTypeId> bounds;
  for (size_t i = 0; i < signature.type_parameters.size(); ++i) {
    budget.tick();
    const auto &formal = signature.type_parameters[i];
    bounds.push_back(
        methodFormalBound(signature, formal, context, budget, nominal_classes));
    indices.emplace(formal.name, i);
    result.emplace(formal.name, SubstitutedType{formal.erasure,
                                                "M[" + std::to_string(i) + "];",
                                                false, true});
  }
  if (!class_bindings)
    return result;
  auto scoped = *class_bindings;
  for (const auto &[name, alpha] : result)
    scoped.insert_or_assign(name, alpha);
  std::vector<unsigned> states(bounds.size());
  std::function<void(size_t, unsigned)> resolve = [&](size_t i,
                                                      unsigned depth) {
    budget.tick();
    if (depth > 64 || states[i] == 1)
      unsupported(context,
                  "specialized method bound graph is cyclic or exceeds limit");
    if (states[i] == 2)
      return;
    states[i] = 1;
    const auto &bound = signature.types.at(bounds[i]);
    if (bound.kind == GenericType::Kind::TypeVariable)
      if (const auto dependency = indices.find(bound.variable_name);
          dependency != indices.end())
        resolve(dependency->second, depth + 1);
    // A method variable keeps its own alpha identity after class substitution,
    // but its source erasure follows the complete substituted upper bound.
    // Original runtime descriptors remain on MethodRef and its bridge proof.
    const auto &formal = signature.type_parameters[i];
    auto &alpha = result.at(formal.name);
    alpha.descriptor =
        substitute(signature, bounds[i], scoped, context, budget, 0, true)
            .descriptor;
    scoped.insert_or_assign(formal.name, alpha);
    states[i] = 2;
  };
  for (size_t i = 0; i < bounds.size(); ++i)
    resolve(i, 0);
  return result;
}

const Method *findMethod(const Class &cls, const MethodRef &reference) {
  for (const auto &method : cls.methods)
    if (method.reference == reference)
      return &method;
  return nullptr;
}

std::optional<PrimitiveBridgeConversion>
primitiveUnitConversion(const std::string &primitive) {
  if (primitive == "B")
    return PrimitiveBridgeConversion{
        "Ljava/lang/Byte;",
        "Ljava/lang/Number;",
        {"Ljava/lang/Number;", "byteValue", {}, "B"}};
  if (primitive == "S")
    return PrimitiveBridgeConversion{
        "Ljava/lang/Short;",
        "Ljava/lang/Number;",
        {"Ljava/lang/Number;", "shortValue", {}, "S"}};
  if (primitive == "I")
    return PrimitiveBridgeConversion{
        "Ljava/lang/Integer;",
        "Ljava/lang/Number;",
        {"Ljava/lang/Number;", "intValue", {}, "I"}};
  if (primitive == "C")
    return PrimitiveBridgeConversion{
        "Ljava/lang/Character;",
        "Ljava/lang/Character;",
        {"Ljava/lang/Character;", "charValue", {}, "C"}};
  if (primitive == "Z")
    return PrimitiveBridgeConversion{
        "Ljava/lang/Boolean;",
        "Ljava/lang/Boolean;",
        {"Ljava/lang/Boolean;", "booleanValue", {}, "Z"}};
  if (primitive == "J")
    return PrimitiveBridgeConversion{
        "Ljava/lang/Long;",
        "Ljava/lang/Number;",
        {"Ljava/lang/Number;", "longValue", {}, "J"}};
  if (primitive == "F")
    return PrimitiveBridgeConversion{
        "Ljava/lang/Float;",
        "Ljava/lang/Number;",
        {"Ljava/lang/Number;", "floatValue", {}, "F"}};
  if (primitive == "D")
    return PrimitiveBridgeConversion{
        "Ljava/lang/Double;",
        "Ljava/lang/Number;",
        {"Ljava/lang/Number;", "doubleValue", {}, "D"}};
  return {};
}

bool knownPrimitiveBridgeConversion(const PrimitiveBridgeConversion &conversion,
                                    const ClassMap &classes) {
  const auto boxed = classes.find(conversion.boxed);
  const auto checked = classes.find(conversion.checked);
  if (boxed == classes.end() || checked == classes.end() ||
      !has(boxed->second.access, "public") ||
      !has(boxed->second.access, "final") ||
      !has(checked->second.access, "public") ||
      has(checked->second.access, "interface") ||
      boxed->second.superclass != (conversion.boxed == conversion.checked
                                       ? "Ljava/lang/Object;"
                                       : conversion.checked))
    return false;
  const auto *unbox = findMethod(checked->second, conversion.unbox);
  return unbox && has(unbox->access, "public") &&
         !has(unbox->access, "static") && !unbox->generic_signature &&
         (!unbox->declared_throws || unbox->declared_throws->empty());
}

bool inheritedObjectContract(const Class &cls, const Method &member,
                             const MethodRef &specialized,
                             const ClassMap &classes, Budget &budget) {
  budget.tick();
  // The only caller has already composed every known class edge to Object.
  if (has(cls.access, "interface") || member.generic_signature ||
      (member.declared_throws && !member.declared_throws->empty()) ||
      specialized.parameters != member.reference.parameters ||
      specialized.returns != member.reference.returns ||
      std::any_of(
          cls.methods.begin(), cls.methods.end(), [&](const Method &candidate) {
            return candidate.reference.name == member.reference.name &&
                   candidate.reference.parameters == specialized.parameters;
          }))
    return false;
  const auto found = classes.find("Ljava/lang/Object;");
  if (found == classes.end())
    return false;
  const auto &object = found->second;
  if (!object.external || !has(object.access, "public") ||
      has(object.access, "interface") || has(object.access, "abstract") ||
      object.superclass || !object.interfaces.empty() || object.enclosing ||
      object.enclosing_method || object.generic_signature)
    return false;
  auto reference = member.reference;
  reference.owner = object.name;
  const auto *implementation = findMethod(object, reference);
  return implementation && has(implementation->access, "public") &&
         !has(implementation->access, "static") &&
         !has(implementation->access, "abstract") &&
         !has(implementation->access, "final") &&
         !implementation->generic_signature &&
         (!implementation->declared_throws ||
          implementation->declared_throws->empty());
}

// One original bridge selects a direct interface default on its original self.
// This receipt owns inheritance admission and the emitter's lexical dispatch.
std::optional<MethodRef> plainInterfaceSuperForwarder(
    const Class &cls, const Method &bridge, const ClassMap &classes,
    const GenericSignaturePlans &signatures, Budget &budget) {
  budget.tick();
  const auto scalar = [](const std::string &type) {
    return type.size() == 1 &&
           std::string_view("ZBCSIJFD").find(type[0]) != std::string_view::npos;
  };
  const bool getter = bridge.reference.parameters.empty() &&
                      bridge.reference.returns.starts_with('L');
  const bool void_forwarder =
      bridge.reference.parameters.empty() && bridge.reference.returns == "V";
  const bool scalar_forwarder = bridge.reference.parameters.size() == 1 &&
                                scalar(bridge.reference.parameters[0]) &&
                                scalar(bridge.reference.returns);
  const auto fixed_nominal = [&](const std::string &type) {
    const auto known = classes.find(type);
    return type.starts_with('L') && known != classes.end() &&
           has(known->second.access, "public") &&
           !has(known->second.access, "annotation") &&
           independentSourceScope(known->second) &&
           !known->second.generic_signature;
  };
  const bool nominal_forwarder =
      bridge.reference.parameters.size() == 1 &&
      fixed_nominal(bridge.reference.parameters[0]) &&
      fixed_nominal(bridge.reference.returns);
  const auto fixed_slot = [&](const std::string &type) {
    return scalar(type) || fixed_nominal(type);
  };
  const auto &parameters = bridge.reference.parameters;
  const bool mixed_forwarder =
      std::any_of(parameters.begin(), parameters.end(), scalar) &&
      std::any_of(parameters.begin(), parameters.end(), fixed_nominal) &&
      std::all_of(parameters.begin(), parameters.end(), fixed_slot) &&
      fixed_slot(bridge.reference.returns);
  const bool reference_result = bridge.reference.returns.starts_with('L');
  if (cls.external || has(cls.access, "interface") ||
      !independentSourceScope(cls) || cls.generic_signature ||
      (!mixed_forwarder && !void_forwarder &&
       cls.superclass != "Ljava/lang/Object;") ||
      !cls.superclass || cls.interfaces.empty() ||
      (getter && cls.interfaces.size() != 1) ||
      (bridge.access != Access{"public", "bridge"} &&
       bridge.access != Access{"public", "bridge", "synthetic"}) ||
      bridge.reference.owner != cls.name ||
      (!getter && !scalar_forwarder && !nominal_forwarder && !mixed_forwarder &&
       !void_forwarder) ||
      objectSourceMethodNames().contains(bridge.reference.name) ||
      bridge.generic_signature ||
      signatures.methods.contains(bridge.reference) ||
      bridge.annotation_default || !bridge.tries.empty() ||
      (bridge.declared_throws && !bridge.declared_throws->empty()) ||
      bridge.registers < bridge.incomingWords() ||
      (!void_forwarder && bridge.registers == bridge.incomingWords()) ||
      bridge.registers > 65535 ||
      bridge.instructions.size() != (void_forwarder ? 2u : 3u))
    return {};
  const auto &call = bridge.instructions[0];
  const auto &move = bridge.instructions[1], &ret = bridge.instructions.back();
  const auto *reference = std::get_if<MethodRef>(&call.reference);
  const unsigned self = bridge.registers - bridge.incomingWords();
  const bool wide = !void_forwarder && width(bridge.reference.returns) == 2;
  std::vector<unsigned> incoming;
  for (unsigned reg = self; reg < bridge.registers; ++reg)
    incoming.push_back(reg);
  if ((call.opcode != "invoke-super" && call.opcode != "invoke-super/range") ||
      !reference ||
      std::find(cls.interfaces.begin(), cls.interfaces.end(),
                reference->owner) == cls.interfaces.end() ||
      reference->name != bridge.reference.name ||
      reference->parameters != bridge.reference.parameters ||
      reference->returns != bridge.reference.returns ||
      call.registers != incoming)
    return {};
  if (void_forwarder) {
    if (ret.opcode != "return-void" || !ret.registers.empty())
      return {};
  } else if (move.opcode != (reference_result ? "move-result-object"
                             : wide           ? "move-result-wide"
                                              : "move-result") ||
             move.registers.size() != 1 || move.registers[0] >= self ||
             (wide && self - move.registers[0] < 2) ||
             ret.opcode != (reference_result ? "return-object"
                            : wide           ? "return-wide"
                                             : "return") ||
             ret.registers != move.registers)
    return {};
  for (size_t i = 0; i < bridge.instructions.size(); ++i) {
    budget.tick();
    const auto &op = bridge.instructions[i];
    if ((i && op.pc <= bridge.instructions[i - 1].pc) ||
        op.pc >= bridge.code_end || op.target || !op.targets.empty() ||
        !op.keys.empty() || !op.data.empty() || op.element_width ||
        !std::holds_alternative<std::monostate>(op.literal) ||
        (i && !std::holds_alternative<std::monostate>(op.reference)))
      return {};
  }
  const auto provider = classes.find(reference->owner);
  if (provider == classes.end() || !has(provider->second.access, "public") ||
      !has(provider->second.access, "interface") ||
      !independentSourceScope(provider->second) ||
      provider->second.generic_signature)
    return {};
  if (getter) {
    const auto *target = findMethod(provider->second, *reference);
    if (!target || target->access != Access{"public"} ||
        target->generic_signature || signatures.methods.contains(*reference) ||
        (target->declared_throws && !target->declared_throws->empty()) ||
        (!provider->second.external && target->instructions.empty()))
      return {};
    return *reference;
  }
  // Scalar and fixed nominal slots may qualify an inherited default. Check the
  // plain nominal DAG, including other direct interfaces, before resolving
  // the unique most-specific declaration under that exact qualifier.
  std::map<std::string, unsigned> states;
  std::function<bool(const std::string &, unsigned)> known =
      [&](const std::string &name, unsigned depth) {
        budget.tick();
        if (depth > 64 || states.size() > 128)
          return false;
        const auto found = classes.find(name);
        if (found == classes.end())
          return false;
        auto &state = states[name];
        if (state)
          return state == 2;
        state = 1;
        const auto &owner = found->second;
        if (!has(owner.access, "public") || !has(owner.access, "interface") ||
            !independentSourceScope(owner) || owner.generic_signature ||
            owner.superclass != "Ljava/lang/Object;")
          return false;
        for (const auto &parent : owner.interfaces)
          if (!known(parent, depth + 1))
            return false;
        state = 2;
        return true;
      };
  for (const auto &direct : cls.interfaces)
    if (!known(direct, 0))
      return {};
  // A class ancestor may share unrelated interfaces with the qualifier. Its
  // complete header chain must neither inherit that qualifier nor contribute
  // a same-name contract whose source selection has not been established.
  std::set<std::string> base_classes;
  std::vector<std::string> base_interfaces;
  for (auto base = cls.superclass; base != "Ljava/lang/Object;";) {
    budget.tick();
    if (!base || !base_classes.insert(*base).second || base_classes.size() > 64)
      return {};
    const auto found = classes.find(*base);
    if (found == classes.end())
      return {};
    const auto &owner = found->second;
    if (!has(owner.access, "public") || has(owner.access, "interface") ||
        has(owner.access, "annotation") || !independentSourceScope(owner) ||
        owner.generic_signature || !owner.superclass)
      return {};
    for (const auto &candidate : owner.methods) {
      budget.tick();
      if (candidate.reference.name == reference->name)
        return {};
    }
    for (const auto &parent : owner.interfaces) {
      if (!known(parent, 0))
        return {};
      base_interfaces.push_back(parent);
    }
    base = owner.superclass;
  }
  const auto reaches = [&](const std::string &child,
                           const std::string &parent) {
    std::vector<std::string> pending{child};
    std::set<std::string> seen;
    for (size_t at = 0; at < pending.size(); ++at) {
      budget.tick();
      const auto &name = pending[at];
      if (!seen.insert(name).second)
        continue;
      if (name == parent)
        return true;
      const auto &edges = classes.at(name).interfaces;
      pending.insert(pending.end(), edges.begin(), edges.end());
    }
    return false;
  };
  for (const auto &other : cls.interfaces)
    if (other != reference->owner && reaches(other, reference->owner))
      return {}; // Java cannot qualify a redundant, less-specific interface.
  for (const auto &base : base_interfaces) {
    if (reaches(base, reference->owner))
      return {};
    for (const auto &[name, state] : states)
      if (reaches(base, name))
        for (const auto &candidate : classes.at(name).methods) {
          budget.tick();
          if (candidate.reference.name == reference->name)
            return {};
        }
  }
  std::vector<const Method *> declarations;
  for (const auto &[name, state] : states) {
    if (!reaches(reference->owner, name))
      continue;
    const auto &owner = classes.at(name);
    for (const auto &candidate : owner.methods) {
      budget.tick();
      const auto &slot = candidate.reference;
      if (slot.name != reference->name)
        continue;
      // The nominal slot retains the exact original descriptor. A different
      // reference overload needs its own complete source selection proof.
      const bool same_nominal_slot = nominal_forwarder &&
                                     slot.parameters == reference->parameters &&
                                     slot.returns == reference->returns;
      const bool scalar_slot =
          scalar_forwarder && slot.parameters.size() == 1 &&
          scalar(slot.parameters.front()) && scalar(slot.returns);
      const bool mixed_slot = mixed_forwarder && fixed_slot(slot.returns) &&
                              std::all_of(slot.parameters.begin(),
                                          slot.parameters.end(), fixed_slot);
      const bool void_slot =
          void_forwarder && slot.parameters.empty() && slot.returns == "V";
      if ((!same_nominal_slot && !scalar_slot && !mixed_slot && !void_slot) ||
          (candidate.access != Access{"public"} &&
           candidate.access != Access{"public", "abstract"}) ||
          candidate.generic_signature || signatures.methods.contains(slot) ||
          (candidate.declared_throws && !candidate.declared_throws->empty()) ||
          (!owner.external && !has(candidate.access, "abstract") &&
           candidate.instructions.empty()))
        return {};
      // Java selects complete fixed overloads from the exact incoming
      // parameter types. Only the matching parameter descriptor participates
      // in most-specific default selection; a return-only collision is still
      // unrepresentable and must not acquire a forwarding receipt.
      if (slot.parameters != reference->parameters)
        continue;
      if (slot.returns != reference->returns)
        return {};
      declarations.push_back(&candidate);
    }
  }
  const Method *selected = nullptr;
  for (const auto *candidate : declarations) {
    if (std::any_of(
            declarations.begin(), declarations.end(), [&](const Method *other) {
              return other != candidate && reaches(other->reference.owner,
                                                   candidate->reference.owner);
            }))
      continue;
    if (selected || has(candidate->access, "abstract"))
      return {};
    selected = candidate;
  }
  return selected ? std::optional<MethodRef>{*reference} : std::nullopt;
}

bool capturedPlainIntegerScope(const Class &cls,
                               const GenericSignaturePlans &signatures,
                               const std::vector<SubstitutedType> *parameters,
                               const SubstitutedType *result) {
  return parameters && parameters->empty() && result &&
         result->signature == "I" &&
         signatures.captured_inner_classes.contains(cls.name);
}

std::optional<MethodRef> plainPrimitiveForwardingBridge(
    const Class &cls, const Method &bridge, const Method &contract,
    const GenericSignaturePlans &signatures, Budget &budget,
    const std::vector<SubstitutedType> *parameters = nullptr,
    const SubstitutedType *result = nullptr) {
  budget.tick();
  const bool scalar_getter =
      !contract.generic_signature && contract.reference.parameters.empty() &&
      contract.reference.returns.size() == 1 &&
      std::string_view("ZBCSIJFD").find(contract.reference.returns.front()) !=
          std::string_view::npos &&
      has(bridge.access, "final");
  const bool indexed_character =
      !contract.generic_signature &&
      contract.reference.parameters == std::vector<std::string>{"I"} &&
      contract.reference.returns == "C" && has(bridge.access, "final") &&
      parameters && parameters->size() == 1 &&
      parameters->front().signature == "I" && result &&
      result->signature == "C";
  bool boolean_forwarder =
      parameters && result && result->signature == "Z" &&
      contract.reference.returns == "Z" && parameters->size() == 1 &&
      bridge.reference.parameters.size() == 1 &&
      (has(cls.access, "final") || has(bridge.access, "final")) &&
      parameters->front().signature == bridge.reference.parameters.front();
  if (const auto full = signatures.methods.find(contract.reference);
      boolean_forwarder && full != signatures.methods.end())
    boolean_forwarder = full->second.type_parameters.empty() &&
                        full->second.throws_types.empty() &&
                        full->second.source_projections.empty();
  if ((!independentSourceScope(cls) &&
       !(scalar_getter && contract.reference.returns == "I" &&
         capturedPlainIntegerScope(cls, signatures, parameters, result)) &&
       !(boolean_forwarder && !contract.generic_signature &&
         signatures.captured_inner_classes.contains(cls.name))) ||
      (!scalar_getter && !indexed_character && !boolean_forwarder) ||
      bridge.reference.parameters != contract.reference.parameters ||
      bridge.reference.returns != contract.reference.returns ||
      (bridge.access != Access{"public", "final", "bridge"} &&
       bridge.access != Access{"public", "final", "bridge", "synthetic"} &&
       !(boolean_forwarder &&
         bridge.access == Access{"public", "bridge", "synthetic"})) ||
      bridge.generic_signature ||
      signatures.methods.contains(bridge.reference) ||
      bridge.annotation_default || !bridge.tries.empty() ||
      (bridge.declared_throws && !bridge.declared_throws->empty()) ||
      (contract.declared_throws && !contract.declared_throws->empty()) ||
      bridge.registers <= bridge.incomingWords() || bridge.registers > 65535 ||
      bridge.instructions.size() != 3)
    return {};
  // Indexed character and boolean forwarding consume complete substituted
  // parameters. Scalar getters retain their exact primitive descriptor profile.
  const auto &invoke = bridge.instructions[0];
  const auto &move = bridge.instructions[1];
  const auto &ret = bridge.instructions[2];
  const auto *reference = std::get_if<MethodRef>(&invoke.reference);
  const unsigned self = bridge.registers - bridge.incomingWords();
  std::vector<unsigned> arguments;
  for (unsigned i = 0; i < bridge.incomingWords(); ++i)
    arguments.push_back(self + i);
  if ((invoke.opcode != "invoke-virtual" &&
       invoke.opcode != "invoke-virtual/range") ||
      !reference || reference->owner != cls.name ||
      reference->name == bridge.reference.name ||
      reference->parameters != bridge.reference.parameters ||
      reference->returns != bridge.reference.returns ||
      invoke.registers != arguments ||
      move.opcode != (width(bridge.reference.returns) == 2 ? "move-result-wide"
                                                           : "move-result") ||
      move.registers.size() != 1 || move.registers[0] >= self ||
      width(bridge.reference.returns) > self - move.registers[0] ||
      ret.opcode !=
          (width(bridge.reference.returns) == 2 ? "return-wide" : "return") ||
      ret.registers != move.registers)
    return {};
  uint32_t previous = 0;
  for (size_t i = 0; i < bridge.instructions.size(); ++i) {
    budget.tick();
    const auto &op = bridge.instructions[i];
    if ((i && op.pc <= previous) || op.pc >= bridge.code_end || op.target ||
        !op.targets.empty() || !op.keys.empty() || !op.data.empty() ||
        op.element_width ||
        !std::holds_alternative<std::monostate>(op.literal) ||
        (i && !std::holds_alternative<std::monostate>(op.reference)))
      return {};
    previous = op.pc;
  }
  const auto *target = findMethod(cls, *reference);
  if (!target || !has(target->access, "public") ||
      has(target->access, "static") ||
      (has(target->access, "abstract") && !has(cls.access, "abstract")) ||
      (has(target->access, "bridge") &&
       !signatures.superclass_scalar_getters.contains(target->reference)) ||
      target->generic_signature ||
      signatures.methods.contains(target->reference) ||
      (target->declared_throws && !target->declared_throws->empty()))
    return {};
  return target->reference;
}

// Share the original call/move/return proof across both getter profiles.
std::optional<MethodRef>
referenceGetterCall(const Class &cls, const Method &bridge, Budget &budget) {
  const bool indexed =
      bridge.reference.parameters == std::vector<std::string>{"I"};
  const unsigned incoming = indexed ? 2 : 1;
  if ((!bridge.reference.parameters.empty() && !indexed) ||
      !bridge.reference.returns.starts_with('L') || !bridge.tries.empty() ||
      bridge.registers <= incoming || bridge.registers > 65535 ||
      bridge.instructions.size() != 3)
    return {};
  const auto &invoke = bridge.instructions[0];
  const auto &move = bridge.instructions[1], &ret = bridge.instructions[2];
  const auto *ref = std::get_if<MethodRef>(&invoke.reference);
  const unsigned self = bridge.registers - incoming;
  const std::vector<unsigned> arguments =
      indexed ? std::vector<unsigned>{self, self + 1}
              : std::vector<unsigned>{self};
  if ((invoke.opcode != "invoke-virtual" &&
       invoke.opcode != "invoke-virtual/range") ||
      !ref || ref->owner != cls.name || ref->name == bridge.reference.name ||
      ref->parameters != bridge.reference.parameters ||
      ref->returns != bridge.reference.returns ||
      invoke.registers != arguments || move.opcode != "move-result-object" ||
      move.registers.size() != 1 || move.registers[0] >= self ||
      ret.opcode != "return-object" || ret.registers != move.registers)
    return {};
  for (size_t i = 0; i < bridge.instructions.size(); ++i) {
    budget.tick();
    const auto &op = bridge.instructions[i];
    if ((i && op.pc <= bridge.instructions[i - 1].pc) ||
        op.pc >= bridge.code_end || op.target || !op.targets.empty() ||
        !op.keys.empty() || !op.data.empty() || op.element_width ||
        !std::holds_alternative<std::monostate>(op.literal) ||
        (i && !std::holds_alternative<std::monostate>(op.reference)))
      return {};
  }
  return *ref;
}

std::optional<MethodRef> genericReferenceGetterBridge(
    const Class &cls, const Method &bridge, const SubstitutedType &contract,
    const GenericSignaturePlans &signatures, Budget &budget) {
  budget.tick();
  if (const auto closed =
          signatures.closed_reference_getter_bridges.find(bridge.reference);
      closed != signatures.closed_reference_getter_bridges.end()) {
    const auto &full = closed->second.signature;
    if (bridge.reference.owner == cls.name && full.result &&
        bridge.reference.parameters.empty() &&
        bridge.reference.returns == contract.descriptor &&
        substitute(full, *full.result, {}, cls.name, budget, 0, true)
                .signature == contract.signature)
      return closed->second.target;
    return {};
  }
  const auto plan = signatures.methods.find(bridge.reference);
  const bool indexed =
      bridge.reference.parameters == std::vector<std::string>{"I"};
  const unsigned incoming = indexed ? 2 : 1;
  if (!independentSourceScope(cls) ||
      (!bridge.reference.parameters.empty() && !indexed) ||
      !bridge.reference.returns.starts_with('L') ||
      bridge.reference.returns != contract.descriptor ||
      (bridge.access != Access{"public", "final", "bridge"} &&
       bridge.access != Access{"public", "final", "bridge", "synthetic"}) ||
      !bridge.generic_signature || plan == signatures.methods.end() ||
      !plan->second.type_parameters.empty() ||
      !plan->second.throws_types.empty() ||
      !plan->second.source_projections.empty() || bridge.annotation_default ||
      !bridge.tries.empty() ||
      (bridge.declared_throws && !bridge.declared_throws->empty()) ||
      bridge.registers <= incoming || bridge.registers > 65535 ||
      bridge.instructions.size() != 3)
    return {};
  if (plan->second.parameters.size() != bridge.reference.parameters.size() ||
      (indexed && substitute(plan->second, plan->second.parameters[0], {},
                             cls.name, budget, 0, true)
                          .signature != "I"))
    return {};
  const auto actual = substitute(plan->second, *plan->second.result, {},
                                 cls.name, budget, 0, true);
  if (actual.signature != contract.signature)
    return {};
  const auto reference = referenceGetterCall(cls, bridge, budget);
  if (!reference)
    return {};
  const auto *ref = &*reference;
  const auto *target = findMethod(cls, *ref);
  const auto target_plan = signatures.methods.find(*ref);
  const bool super_getter = signatures.superclass_getter_results.contains(*ref);
  if (!target || !has(target->access, "public") ||
      has(target->access, "static") ||
      (has(target->access, "abstract") && !has(cls.access, "abstract")) ||
      (has(target->access, "bridge") && !super_getter) ||
      (target->declared_throws && !target->declared_throws->empty()) ||
      !target->generic_signature || target_plan == signatures.methods.end() ||
      !target_plan->second.type_parameters.empty() ||
      !target_plan->second.throws_types.empty() ||
      (!target_plan->second.source_projections.empty() && !super_getter) ||
      target_plan->second.parameters.size() !=
          bridge.reference.parameters.size() ||
      (indexed &&
       substitute(target_plan->second, target_plan->second.parameters[0], {},
                  cls.name, budget, 0, true)
               .signature != "I") ||
      substitute(target_plan->second, *target_plan->second.result, {}, cls.name,
                 budget, 0, true)
              .signature != actual.signature)
    return {};
  return *ref;
}

std::optional<MethodRef> guardedReferenceForwardingBridge(
    const Class &cls, const Method &bridge, const Method &contract,
    const ClassMap &classes, const GenericSignaturePlans &signatures,
    Budget &budget, const SubstitutedType *full_result = nullptr) {
  budget.tick();
  const auto &ops = bridge.instructions;
  const bool reference_result = full_result &&
                                full_result->descriptor.starts_with('L') &&
                                full_result->descriptor != "Ljava/lang/Object;";
  const bool erased_reference =
      reference_result && bridge.reference.returns == "Ljava/lang/Object;";
  const auto metadata = signatures.methods.find(bridge.reference);
  if ((!independentSourceScope(cls) &&
       (reference_result ||
        (!signatures.captured_inner_classes.contains(cls.name) &&
         !signatures.closed_anonymous_classes.contains(cls.name)))) ||
      (!reference_result && contract.generic_signature) ||
      contract.reference.parameters !=
          std::vector<std::string>{"Ljava/lang/Object;"} ||
      (!reference_result && contract.reference.returns != "Z" &&
       contract.reference.returns != "I") ||
      bridge.reference.parameters != contract.reference.parameters ||
      (reference_result
           ? (!erased_reference &&
              bridge.reference.returns != full_result->descriptor)
           : bridge.reference.returns != contract.reference.returns) ||
      (bridge.access != Access{"public", "final", "bridge"} &&
       bridge.access != Access{"public", "final", "bridge", "synthetic"}) ||
      (!reference_result &&
       (bridge.generic_signature || metadata != signatures.methods.end())) ||
      bridge.annotation_default || !bridge.tries.empty() ||
      (bridge.declared_throws && !bridge.declared_throws->empty()) ||
      (contract.declared_throws && !contract.declared_throws->empty()) ||
      bridge.registers < 3 || bridge.registers > 65535 ||
      (ops.size() < 8 || ops.size() > 13))
    return {};
  if (reference_result) {
    if (erased_reference) {
      if (bridge.generic_signature || metadata != signatures.methods.end())
        return {};
    } else {
      if (!bridge.generic_signature || metadata == signatures.methods.end())
        return {};
      const auto &shape = metadata->second;
      if (!shape.result || shape.parameters.size() != 1 ||
          !shape.type_parameters.empty() || !shape.throws_types.empty() ||
          !shape.source_projections.empty() ||
          substitute(shape, shape.parameters[0], {}, cls.name, budget, 0, true)
                  .signature != "Ljava/lang/Object;" ||
          substitute(shape, *shape.result, {}, cls.name, budget, 0, true)
                  .signature != full_result->signature)
        return {};
    }
  }
  const auto self = bridge.registers - 2, argument = bridge.registers - 1;
  size_t guard_at = 0;
  if (ops[0].opcode == "if-nez") {
    // The original null arm supplies true; the nonnull arm supplies its exact
    // instanceof result. Both join the same checked forwarding decision.
    const auto *literal = std::get_if<int64_t>(&ops[1].literal);
    if (reference_result || ops.size() < 11 ||
        ops[0].registers != std::vector<unsigned>{argument} ||
        ops[0].target != ops[3].pc ||
        (ops[1].opcode != "const/4" && ops[1].opcode != "const/16" &&
         ops[1].opcode != "const") ||
        !literal || *literal != 1 || ops[1].registers.size() != 1 ||
        (ops[2].opcode != "goto" && ops[2].opcode != "goto/16" &&
         ops[2].opcode != "goto/32") ||
        !ops[2].registers.empty() || ops[2].target != ops[4].pc ||
        ops[3].opcode != "instance-of" || ops[3].registers.size() != 2 ||
        ops[3].registers[0] != ops[1].registers[0])
      return {};
    guard_at = 3;
  }
  const auto &guard = ops[guard_at];
  const auto *type = std::get_if<std::string>(&guard.reference);
  size_t branch = guard_at + 1;
  unsigned condition = 0;
  if (guard.opcode == "instance-of") {
    if (guard.registers.size() != 2 || guard.registers[0] >= self ||
        guard.registers[1] != argument || !type || !type->starts_with('L') ||
        !classes.contains(*type))
      return {};
    condition = guard.registers[0];
  } else {
    const auto *predicate = std::get_if<MethodRef>(&ops[0].reference);
    if ((ops[0].opcode != "invoke-static" &&
         ops[0].opcode != "invoke-static/range") ||
        !predicate ||
        predicate->parameters !=
            std::vector<std::string>{"Ljava/lang/Object;"} ||
        predicate->returns != "Z" ||
        ops[0].registers != std::vector<unsigned>{argument} ||
        ops[1].opcode != "move-result" || ops[1].registers.size() != 1 ||
        ops[1].registers[0] >= self)
      return {};
    const auto owner = classes.find(predicate->owner);
    const auto *declaration = owner == classes.end()
                                  ? nullptr
                                  : findMethod(owner->second, *predicate);
    if (!declaration || !has(declaration->access, "public") ||
        !has(declaration->access, "static") ||
        has(declaration->access, "abstract") ||
        declaration->generic_signature ||
        signatures.methods.contains(*predicate) ||
        (declaration->declared_throws &&
         !declaration->declared_throws->empty()))
      return {};
    condition = ops[1].registers[0];
    branch = 2;
  }
  if ((ops[branch].opcode != "if-nez" && ops[branch].opcode != "if-eqz") ||
      ops[branch].registers != std::vector<unsigned>{condition} ||
      !ops[branch].target)
    return {};
  const auto label = std::find_if(ops.begin(), ops.end(), [&](const auto &op) {
    budget.tick();
    return op.pc == *ops[branch].target;
  });
  if (label == ops.end())
    return {};
  const size_t jump = label - ops.begin();
  size_t positive = ops[branch].opcode == "if-nez" ? jump : branch + 1;
  const size_t negative = ops[branch].opcode == "if-nez" ? branch + 1 : jump;
  if (positive <= branch || negative <= branch || negative + 1 >= ops.size())
    return {};
  const auto &zero = ops[negative], &false_return = ops[negative + 1];
  const auto *literal = std::get_if<int64_t>(&zero.literal);
  if ((zero.opcode != "const/4" && zero.opcode != "const/16" &&
       zero.opcode != "const") ||
      !literal ||
      ((bridge.reference.returns == "Z" || reference_result)
           ? *literal != 0
           : *literal < std::numeric_limits<int32_t>::min() ||
                 *literal > std::numeric_limits<int32_t>::max()) ||
      zero.registers.size() != 1 || zero.registers[0] >= self ||
      false_return.opcode != (reference_result ? "return-object" : "return") ||
      false_return.registers != zero.registers)
    return {};
  std::set<size_t> used{negative, negative + 1};
  for (size_t i = 0; i <= branch; ++i)
    used.insert(i);
  unsigned checked = argument;
  if (positive < ops.size() && (ops[positive].opcode == "move-object" ||
                                ops[positive].opcode == "move-object/from16" ||
                                ops[positive].opcode == "move-object/16")) {
    const auto &move = ops[positive];
    if (move.registers.size() != 2 || move.registers[0] >= self ||
        move.registers[1] != argument)
      return {};
    checked = move.registers[0];
    used.insert(positive++);
  }
  if (positive + 3 >= ops.size())
    return {};
  const auto &cast = ops[positive], &invoke = ops[positive + 1];
  const auto &result = ops[positive + 2], &ret = ops[positive + 3];
  const auto *cast_type = std::get_if<std::string>(&cast.reference);
  const auto *target = std::get_if<MethodRef>(&invoke.reference);
  if (cast.opcode != "check-cast" || !cast_type ||
      (type && *cast_type != *type) || !cast_type->starts_with('L') ||
      !classes.contains(*cast_type) ||
      cast.registers != std::vector<unsigned>{checked} ||
      (invoke.opcode != "invoke-virtual" &&
       invoke.opcode != "invoke-virtual/range") ||
      !target || target->owner != cls.name ||
      target->name != bridge.reference.name ||
      target->parameters != std::vector<std::string>{*cast_type} ||
      target->returns != (reference_result ? full_result->descriptor
                                           : bridge.reference.returns) ||
      invoke.registers != std::vector<unsigned>{self, checked} ||
      result.opcode !=
          (reference_result ? "move-result-object" : "move-result") ||
      result.registers.size() != 1 || result.registers[0] >= self ||
      ret.opcode != (reference_result ? "return-object" : "return") ||
      ret.registers != result.registers)
    return {};
  for (size_t i = positive; i < positive + 4; ++i)
    used.insert(i);
  if (used.size() != ops.size())
    return {};
  uint32_t previous = 0;
  for (size_t i = 0; i < ops.size(); ++i) {
    budget.tick();
    const auto &op = ops[i];
    if ((i && op.pc <= previous) || op.pc >= bridge.code_end ||
        (i != branch && !(guard_at && (i == 0 || i == 2)) && op.target) ||
        !op.targets.empty() || !op.keys.empty() || !op.data.empty() ||
        op.element_width ||
        (i != negative && !(guard_at && i == 1) &&
         !std::holds_alternative<std::monostate>(op.literal)) ||
        (i != guard_at && i != positive && i != positive + 1 &&
         !std::holds_alternative<std::monostate>(op.reference)))
      return {};
    previous = op.pc;
  }
  const auto *implementation = findMethod(cls, *target);
  if (!implementation || !has(implementation->access, "public") ||
      has(implementation->access, "static") ||
      has(implementation->access, "abstract") ||
      (has(implementation->access, "bridge") &&
       !signatures.superclass_scalar_adapters.contains(
           implementation->reference) &&
       !signatures.superclass_reference_adapters.contains(
           implementation->reference)) ||
      (implementation->declared_throws &&
       !implementation->declared_throws->empty()))
    return {};
  if (reference_result) {
    const auto target_plan = signatures.methods.find(*target);
    if (target_plan == signatures.methods.end() ||
        !target_plan->second.result ||
        !target_plan->second.source_projections.empty() ||
        substitute(target_plan->second, *target_plan->second.result, {},
                   cls.name, budget, 0, true)
                .signature != full_result->signature)
      return {};
  }
  if (const auto plan = signatures.methods.find(*target);
      plan != signatures.methods.end() &&
      (!plan->second.type_parameters.empty() ||
       !plan->second.throws_types.empty()))
    return {};
  return *target;
}

bool objectCapturedScopeProjection(const GenericSignature &plan) {
  return !plan.source_projections.empty() &&
         std::all_of(plan.source_projections.begin(),
                     plan.source_projections.end(), [](const auto &change) {
                       return (change.kind == "static-inner-type-variable" ||
                               change.kind ==
                                   "anonymous-member-type-variable") &&
                              change.source_type == "Ljava/lang/Object;";
                     });
}

bool capturedSiblingClassShape(const Class &cls, const GenericSignature &plan,
                               const GenericSignaturePlans &signatures) {
  if (!cls.enclosing || cls.enclosing_method ||
      !signatures.captured_inner_classes.contains(cls.name) ||
      !plan.type_parameters.empty())
    return false;
  return std::all_of(
      plan.source_projections.begin(), plan.source_projections.end(),
      [&](const auto &change) {
        if (change.kind == "static-inner-type-variable")
          return change.source_type == "Ljava/lang/Object;" &&
                 change.original_type.starts_with(*cls.enclosing + "::");
        return change.kind == "static-inner-owner-arguments" &&
               cls.superclass && change.original_type == *cls.superclass &&
               change.source_type == *cls.superclass &&
               signatures.captured_sibling_superclasses.contains(
                   {cls.name, *cls.superclass});
      });
}

bool onlyContainedConsumerProjections(const GenericSignature &signature) {
  // The common validator has checked every ancestor path and the safe input
  // containment direction. An inherited provider uses that same source shape;
  // complete substituted parameters and results are still compared below.
  return !signature.source_projections.empty() &&
         std::all_of(
             signature.source_projections.begin(),
             signature.source_projections.end(), [](const auto &change) {
               return change.kind == "inherited-contained-consumer-parameter";
             });
}

bool usableInheritedClassShape(const GenericSignature &plan,
                               bool allow_erased_inner_scope,
                               bool proven_raw_marker = false) {
  // The common marker receipt proves that the source superclass formal
  // affects no ancestor field/member contract. Other already validated
  // consumer projections cannot alter those plain runtime contracts; full
  // parameter/result and interface-path comparisons still run below.
  if (proven_raw_marker)
    return true;
  return onlyBoundPositionProjections(plan) ||
         (allow_erased_inner_scope && objectCapturedScopeProjection(plan));
}

const GenericSignature *
inheritanceClassSignature(const std::string &name,
                          const GenericSignaturePlans &signatures) {
  // A default guard needs raw source syntax, but member/default proofs must
  // continue to consume its complete original contract on every surface.
  const auto found = signatures.classes.find(name);
  if (signatures.guarded_default_raw_owners.contains(name) ||
      signatures.boxed_superclass_owners.contains(name) ||
      (found != signatures.classes.end() &&
       std::any_of(found->second.source_projections.begin(),
                   found->second.source_projections.end(), [](const auto &p) {
                     return p.kind == "guarded-default-shared-interface" ||
                            p.kind ==
                                "checked-inherited-value-carrier-interface";
                   })))
    return &signatures.inheritance_contracts.at(name);
  return found == signatures.classes.end() ? nullptr : &found->second;
}

bool knownCapturedSiblingClassEdge(const Class &current, const Class &ancestor,
                                   const GenericSignaturePlans &signatures) {
  const auto *source = inheritanceClassSignature(current.name, signatures);
  const auto *parent = inheritanceClassSignature(ancestor.name, signatures);
  return source && parent && current.enclosing == ancestor.enclosing &&
         signatures.captured_sibling_superclasses.contains(
             {current.name, ancestor.name}) &&
         capturedSiblingClassShape(current, *source, signatures) &&
         capturedSiblingClassShape(ancestor, *parent, signatures);
}

std::optional<std::map<std::string, SubstitutedType>>
composeClassEdge(const Class &current, const Class &ancestor,
                 const std::map<std::string, SubstitutedType> &bindings,
                 const GenericSignaturePlans &signatures,
                 const std::string &context, Budget &budget,
                 bool allow_erased_inner_scope = false) {
  std::map<std::string, SubstitutedType> composed;
  const auto *source_shape =
      inheritanceClassSignature(current.name, signatures);
  if (!source_shape)
    return composed;
  const auto &source = *source_shape;
  if (!usableInheritedClassShape(
          source, allow_erased_inner_scope,
          signatures.raw_marker_superclasses.contains(current.name)) &&
      !capturedSiblingClassShape(current, source, signatures))
    return {};
  if (!source.superclass)
    return {};
  const auto &edge = source.types.at(*source.superclass);
  if (edge.erasure != ancestor.name)
    return {};
  if (edge.segments.size() != 1) {
    // The common scope validator compared complete original outer arguments
    // before erasure. No class-owned formal or remaining argument may enter
    // this captured sibling edge, so its projected binding is exactly empty.
    if (edge.segments.size() != 2 ||
        !knownCapturedSiblingClassEdge(current, ancestor, signatures) ||
        std::any_of(
            edge.segments.begin(), edge.segments.end(),
            [](const auto &segment) { return !segment.arguments.empty(); }))
      return {};
    return composed;
  }
  const auto &arguments = edge.segments.front().arguments;
  if (arguments.empty())
    return composed;
  const auto *formals = inheritanceClassSignature(ancestor.name, signatures);
  if (!formals ||
      !usableInheritedClassShape(
          *formals, allow_erased_inner_scope,
          signatures.raw_marker_superclasses.contains(ancestor.name)) ||
      arguments.size() != formals->type_parameters.size())
    return {};
  for (size_t i = 0; i < arguments.size(); ++i) {
    budget.tick();
    if (arguments[i].variance != GenericTypeArgument::Variance::Invariant ||
        !arguments[i].type)
      return {};
    composed.emplace(formals->type_parameters[i].name,
                     substitute(source, *arguments[i].type, bindings, context,
                                budget, 0, true));
  }
  return composed;
}

bool inheritedFinalBridgeUses(const Method &bridge, const Method &provider,
                              const GenericSignaturePlans &signatures,
                              Budget &budget) {
  if (bridge.reference.returns == provider.reference.returns ||
      bridge.access != Access{"public", "bridge", "synthetic"} ||
      provider.access != Access{"public", "final"})
    return false;
  auto proof = signatures.inherited_final_covariant_bridges.lower_bound(
      {bridge.reference, MethodRef{}});
  for (; proof != signatures.inherited_final_covariant_bridges.end() &&
         proof->first.first == bridge.reference;
       ++proof) {
    budget.tick();
    if (proof->second.target == provider.reference)
      return true;
  }
  return false;
}

bool provenClassFormalBoundReturn(const MethodRef &provided,
                                  const MethodRef &required,
                                  std::string_view expected_result,
                                  size_t method_formal_count,
                                  const GenericSignaturePlans &signatures) {
  return !method_formal_count && expected_result == required.returns &&
         provided.returns == required.returns &&
         signatures.class_formal_bound_returns.contains({provided, required});
}

bool provenSelfCarrierReturn(
    const MethodRef &provided, const MethodRef &required,
    const std::map<std::string, SubstitutedType> &bindings,
    const SubstitutedType &result, size_t method_formal_count,
    const GenericSignaturePlans &signatures, const std::string &context,
    Budget &budget) {
  const auto proof =
      signatures.self_carrier_return_contracts.find({provided, required});
  if (method_formal_count || !provided.parameters.empty() ||
      !required.parameters.empty() ||
      proof == signatures.self_carrier_return_contracts.end() ||
      !signatures.self_carrier_return_sources.contains(provided))
    return false;
  const auto &full = proof->second;
  return full.result && full.parameters.empty() &&
         full.type_parameters.empty() && full.throws_types.empty() &&
         full.source_projections.empty() &&
         substitute(full, *full.result, bindings, context, budget, 0, true)
                 .signature == result.signature;
}

bool originalRawParameterSubsignature(
    const Method &provided, const Method &required,
    const std::vector<SubstitutedType> &parameters,
    const SubstitutedType &result, size_t method_formal_count,
    bool checked_original_bridge = false) {
  if (provided.generic_signature || method_formal_count || result.open ||
      result.parameterized || result.signature != result.descriptor ||
      result.descriptor != provided.reference.returns ||
      parameters.size() != provided.reference.parameters.size() ||
      (provided.declared_throws && !provided.declared_throws->empty()) ||
      (required.declared_throws && !required.declared_throws->empty()))
    return false;
  bool parameterized = false;
  for (size_t i = 0; i < parameters.size(); ++i) {
    const auto &type = parameters[i];
    const auto &erased = provided.reference.parameters[i];
    if (type.descriptor != erased)
      return false;
    if (type.parameterized) {
      const bool scoped_nominal_erasure =
          type.open && erased.starts_with('L') && type.class_name == erased &&
          type.class_arguments.size() == 1;
      if (!scoped_nominal_erasure &&
          !closedParameterizedRawType(type, SubstitutedType{erased, erased}))
        return false;
      parameterized = true;
    } else if (type.open && checked_original_bridge &&
               type.signature.starts_with("T["))
      parameterized = true;
    else if (type.signature != erased)
      return false;
  }
  // JLS 8.4.2 permits a Signature-free raw declaration to implement the
  // complete erased signature, including nominal parameter arguments whose
  // class or enclosing variables were resolved by the common validator. The
  // nominal runtime parameter stays unchanged, so it needs no new bridge.
  // A direct own-class formal still requires the checked original bridge and
  // common scoped raw class receipt. Partial typed contracts and generic
  // results remain outside this relation.
  return parameterized;
}

bool publicErasedProtectedTarget(
    const Method &bridge, const Method &target, const ClassMap &classes,
    Budget &budget, const GenericSignaturePlans *signatures = nullptr);

const Method *
completeSourceBridgeProvider(const Class &owner, const Method &bridge,
                             const Method &required, const ClassMap &classes,
                             const GenericSignaturePlans &signatures,
                             Budget &budget) {
  if (owner.external ||
      bridge.access != Access{"public", "bridge", "synthetic"} ||
      bridge.generic_signature || bridge.reference.owner != owner.name)
    return nullptr;
  const Method *typed = nullptr;
  for (const auto &op : bridge.instructions) {
    budget.tick();
    const auto *call = std::get_if<MethodRef>(&op.reference);
    if ((op.opcode != "invoke-virtual" &&
         op.opcode != "invoke-virtual/range") ||
        !call || call->owner != owner.name ||
        call->name != bridge.reference.name)
      continue;
    const auto *provider = findMethod(owner, *call);
    const auto plan = signatures.methods.find(*call);
    if (!provider || !provider->generic_signature ||
        (!has(provider->access, "public") &&
         !publicErasedProtectedTarget(bridge, *provider, classes, budget,
                                      &signatures)) ||
        has(provider->access, "static") || has(provider->access, "abstract") ||
        has(provider->access, "native") || has(provider->access, "bridge") ||
        provider->instructions.empty() || plan == signatures.methods.end() ||
        !plan->second.type_parameters.empty() ||
        !plan->second.source_projections.empty())
      continue;
    if (typed && typed != provider)
      unsupported(owner.name,
                  "inherited source bridge has competing typed providers");
    typed = provider;
  }
  if (typed)
    verifyGenericBridge(bridge, *typed, budget, nullptr, nullptr, &classes,
                        &signatures, &required);
  return typed;
}

bool sameCompleteMethodContract(const Method &provided, const Method &required,
                                const GenericSignaturePlans &signatures,
                                const std::string &context, Budget &budget,
                                const ClassMap &classes,
                                bool class_contract = false);

bool sourceContractImplementation(const Class &cls, const Method &method,
                                  const Method &required,
                                  const ClassMap &classes,
                                  const GenericSignaturePlans &signatures,
                                  Budget &budget);

bool inheritedClassContract(
    const Class &cls, const Method &member, const MethodRef &specialized,
    const std::vector<SubstitutedType> &parameters,
    const SubstitutedType &result,
    const std::vector<std::pair<std::string, std::string>> &method_bounds,
    const ClassMap &classes, const GenericSignaturePlans &signatures,
    Budget &budget, bool class_contract = false,
    const SourceMethodNames *method_names = nullptr) {
  const size_t method_formal_count = method_bounds.size();
  if (has(cls.access, "interface") ||
      std::any_of(
          cls.methods.begin(), cls.methods.end(), [&](const Method &candidate) {
            return sameSourceMethodName(candidate.reference, member.reference,
                                        method_names) &&
                   (candidate.reference.parameters ==
                        member.reference.parameters ||
                    candidate.reference.parameters == specialized.parameters);
          }))
    return false;
  const Class *current = &cls;
  const auto source = signatures.classes.find(cls.name);
  // Named and anonymous captured members already have an authoritative
  // Object source projection from the common scope validator.
  // Compose that exact Object-bound projection on both sides of the contract;
  // the complete inherited parameters and result still have to agree below.
  const bool erased_inner_scope = source != signatures.classes.end() &&
                                  objectCapturedScopeProjection(source->second);
  std::map<std::string, SubstitutedType> bindings;
  std::set<std::string> seen;
  while (current->superclass) {
    budget.tick();
    if (!seen.insert(current->name).second || seen.size() > 64)
      return false;
    const auto parent = classes.find(*current->superclass);
    if (parent == classes.end() || has(parent->second.access, "interface"))
      return false;
    const auto &ancestor = parent->second;
    if (ancestor.name == "Ljava/lang/Object;") {
      // Object's final and generic slots are not ordinary interface contracts.
      return inheritedObjectContract(cls, member, specialized, classes, budget);
    }
    const bool captured_sibling =
        knownCapturedSiblingClassEdge(*current, ancestor, signatures);
    if (!independentSourceScope(ancestor) && !captured_sibling)
      return false;
    auto composed = composeClassEdge(*current, ancestor, bindings, signatures,
                                     cls.name, budget, erased_inner_scope);
    if (!composed)
      return false;
    bindings = std::move(*composed);
    if (ancestor.external && !class_contract && !method_formal_count)
      for (const auto &candidate : ancestor.methods) {
        budget.tick();
        const auto proof = signatures.external_class_covariant_contracts.find(
            {candidate.reference, member.reference});
        if (proof == signatures.external_class_covariant_contracts.end())
          continue;
        const auto &full = proof->second;
        bool exact = full.result && full.parameters.size() == parameters.size();
        for (size_t i = 0; exact && i < parameters.size(); ++i)
          exact = substitute(full, full.parameters[i], bindings, cls.name,
                             budget, 0, true)
                      .signature == parameters[i].signature;
        return exact && substitute(full, *full.result, bindings, cls.name,
                                   budget, 0, true)
                                .signature == result.signature;
      }
    for (const auto &candidate : ancestor.methods) {
      budget.tick();
      if (!sameSourceMethodName(candidate.reference, member.reference,
                                method_names) ||
          candidate.reference.parameters != member.reference.parameters)
        continue;
      // The nearest declaration owns this erased slot. Do not bypass an
      // inaccessible/abstract member or invent a bridge for a different ABI.
      if (!ancestor.external && class_contract &&
          has(member.access, "protected") && !method_formal_count &&
          !result.open && !result.parameterized &&
          parameters.size() == candidate.reference.parameters.size() &&
          std::equal(parameters.begin(), parameters.end(),
                     candidate.reference.parameters.begin(),
                     [](const auto &type, const auto &erasure) {
                       return !type.open && !type.parameterized &&
                              type.signature == erasure;
                     })) {
        auto erased = candidate.reference;
        erased.returns = member.reference.returns;
        auto specialized = candidate.reference;
        specialized.returns = result.signature;
        const auto *bridge = findMethod(ancestor, erased);
        const auto *typed = findMethod(ancestor, specialized);
        if (bridge && typed && (&candidate == bridge || &candidate == typed) &&
            compatibleClosedInheritedThrows(*typed, member, signatures) &&
            publicErasedProtectedTarget(*bridge, *typed, classes, budget,
                                        &signatures)) {
          // The exact erased sibling and fully substituted literal result
          // certify one inherited result regardless of which appears first.
          // Reuse the original access and register proofs; do not depend on a
          // receipt from an earlier visit to the source parent.
          verifyGenericBridge(*bridge, *typed, budget, nullptr, nullptr,
                              &classes);
          return true;
        }
      }
      if (candidate.reference.returns != member.reference.returns &&
          !ancestor.external && has(candidate.access, "bridge")) {
        auto exact = candidate.reference;
        exact.returns = member.reference.returns;
        const auto *typed = findMethod(ancestor, exact);
        if (typed && !has(typed->access, "bridge") &&
            !has(typed->access, "abstract") &&
            (has(typed->access, "public") ||
             publicErasedProtectedTarget(candidate, *typed, classes, budget,
                                         &signatures)) &&
            !has(typed->access, "static")) {
          // A source parent can own both runtime return entries. Reprove the
          // original sibling bridge before consuming the exact typed entry;
          // its complete inherited substitution is checked below as usual.
          verifyGenericBridge(candidate, *typed, budget, nullptr, nullptr,
                              &classes);
          continue;
        }
      }
      if (candidate.reference.returns != member.reference.returns &&
          !ancestor.external && !has(candidate.access, "bridge")) {
        auto exact = candidate.reference;
        exact.returns = member.reference.returns;
        const auto *bridge = findMethod(ancestor, exact);
        const auto proof = signatures.covariant_bridge_contracts.find(
            {exact, member.reference});
        if (bridge && has(bridge->access, "bridge") &&
            proof != signatures.covariant_bridge_contracts.end() &&
            proof->second.target == candidate.reference) {
          verifyGenericBridge(*bridge, candidate, budget, nullptr, nullptr,
                              &classes);
          continue;
        }
        if (bridge && !method_formal_count &&
            completeSourceBridgeProvider(ancestor, *bridge, member, classes,
                                         signatures, budget) == &candidate)
          continue; // Consume this typed sibling through its erased entry,
                    // regardless of the original method iteration order.
      }
      const bool class_visibility =
          class_contract && !has(member.access, "public") &&
          !has(candidate.access, "private") &&
          (has(candidate.access, "protected") ||
           (!has(member.access, "protected") &&
            packageOf(cls.name) == packageOf(ancestor.name)));
      if (candidate.reference.returns != member.reference.returns ||
          (!has(candidate.access, "public") && !class_visibility) ||
          has(candidate.access, "static") ||
          has(candidate.access, "abstract") ||
          !compatibleClosedInheritedThrows(candidate, member, signatures))
        return false;
      auto generic = signatures.methods.find(candidate.reference);
      if (has(candidate.access, "bridge")) {
        // A closed interface parameter can already be implemented by the
        // parent's literal typed declaration and original erased bridge. No
        // child declaration is needed. Source parents must reprove the exact
        // checked forwarding body; SDK declarations retain their own runtime
        // entries without claiming a proof of external Code.
        if (!class_contract && !method_formal_count && !result.open &&
            !result.parameterized && result.signature == result.descriptor &&
            candidate.access == Access{"public", "bridge", "synthetic"} &&
            !candidate.generic_signature &&
            parameters.size() == specialized.parameters.size() &&
            std::equal(parameters.begin(), parameters.end(),
                       specialized.parameters.begin(),
                       [](const auto &type, const auto &descriptor) {
                         return !type.open && !type.parameterized &&
                                type.signature == descriptor;
                       })) {
          auto target = candidate.reference;
          target.parameters = specialized.parameters;
          target.returns = result.descriptor;
          const auto *typed = findMethod(ancestor, target);
          if (typed && typed != &candidate && !typed->generic_signature &&
              !signatures.methods.contains(target) &&
              has(typed->access, "public") && !has(typed->access, "static") &&
              !has(typed->access, "abstract") &&
              !has(typed->access, "native") && !has(typed->access, "bridge") &&
              compatibleClosedInheritedThrows(*typed, member, signatures)) {
            if (!ancestor.external)
              verifyGenericBridge(candidate, *typed, budget, nullptr, nullptr,
                                  &classes, &signatures, &member);
            return true;
          }
        }
        if (signatures.closed_reference_getter_bridges.contains(
                candidate.reference))
          return !method_formal_count && parameters.empty() &&
                 genericReferenceGetterBridge(ancestor, candidate, result,
                                              signatures, budget)
                     .has_value();
        if (const auto *full = covariantBridgeContract(candidate.reference,
                                                       signatures, budget)) {
          const auto proof = signatures.covariant_bridge_contracts.lower_bound(
              {candidate.reference, MethodRef{}});
          const auto *typed = findMethod(ancestor, proof->second.target);
          const auto &contract = *full;
          if (!typed || candidate.generic_signature || method_formal_count ||
              !contract.result || !contract.type_parameters.empty() ||
              !contract.throws_types.empty() ||
              !contract.source_projections.empty() ||
              contract.parameters.size() != parameters.size())
            return false;
          verifyGenericBridge(candidate, *typed, budget, nullptr, nullptr,
                              &classes);
          for (size_t i = 0; i < parameters.size(); ++i)
            if (substitute(contract, contract.parameters[i], bindings, cls.name,
                           budget, 0, true)
                    .signature != parameters[i].signature)
              return false;
          return substitute(contract, *contract.result, bindings, cls.name,
                            budget, 0, true)
                     .signature == result.signature;
        }
        // Reprove the original parent body independently of class iteration
        // order. Its final entry point still dispatches the getter virtually;
        // the complete inherited contract is substituted below as usual.
        // Reuse the direct implementation admission, then compare the entire
        // original declaration, including method bounds. This is independent
        // of whether the provider has already been visited by the planner.
        bool retained =
            sourceContractImplementation(ancestor, candidate, member, classes,
                                         signatures, budget) &&
            sameCompleteMethodContract(candidate, member, signatures, cls.name,
                                       budget, classes, class_contract);
        if (!retained && !class_contract && method_formal_count &&
            candidate.access == Access{"public", "bridge", "synthetic"} &&
            !candidate.generic_signature) {
          auto target = candidate.reference;
          target.parameters = specialized.parameters;
          target.returns = specialized.returns;
          const auto *typed = findMethod(ancestor, target);
          const auto actual = signatures.methods.find(target);
          if (typed && typed != &candidate && typed->generic_signature &&
              actual != signatures.methods.end() &&
              actual->second.type_parameters.size() == method_formal_count &&
              onlyBoundPositionProjections(actual->second) &&
              has(typed->access, "public") && !has(typed->access, "static") &&
              !has(typed->access, "abstract") &&
              !has(typed->access, "native") && !has(typed->access, "bridge") &&
              compatibleClosedInheritedThrows(*typed, member, signatures)) {
            // The parent's exact typed sibling owns the method variables.
            // Reprove source forwarding, then compare its complete adapted
            // bounds, parameters and result in the common path below. SDK
            // entries keep their original external runtime implementations.
            if (!ancestor.external)
              verifyGenericBridge(candidate, *typed, budget, nullptr, nullptr,
                                  &classes, &signatures, &member);
            generic = actual;
            retained = true;
          }
        }
        if (!retained && !method_formal_count)
          if (const auto *typed = completeSourceBridgeProvider(
                  ancestor, candidate, member, classes, signatures, budget)) {
            // Keep the original erased entry as the runtime ABI, and consume
            // its target's full contract in the common substitution below.
            generic = signatures.methods.find(typed->reference);
            retained = true;
          }
        if (!retained)
          retained = plainPrimitiveForwardingBridge(ancestor, candidate, member,
                                                    signatures, budget,
                                                    &parameters, &result)
                         .has_value();
        if (!retained && !ancestor.external)
          retained =
              guardedReferenceForwardingBridge(ancestor, candidate, member,
                                               classes, signatures, budget)
                  .has_value();
        if (!retained && generic != signatures.methods.end()) {
          const auto own_result =
              substitute(generic->second, *generic->second.result, {},
                         ancestor.name, budget, 0, true);
          retained = genericReferenceGetterBridge(
                         ancestor, candidate, own_result, signatures, budget)
                         .has_value();
        }
        if (!retained)
          return false;
      }
      if (generic == signatures.methods.end()) {
        if (candidate.generic_signature || method_formal_count ||
            parameters.size() != candidate.reference.parameters.size())
          return false;
        if (originalRawParameterSubsignature(candidate, member, parameters,
                                             result, method_formal_count))
          return true;
        for (size_t i = 0; i < parameters.size(); ++i)
          if (parameters[i].signature != candidate.reference.parameters[i])
            return false;
        return result.signature == candidate.reference.returns;
      }
      const auto &actual = generic->second;
      // Moving a single variable bound between encoded slots preserves its
      // constraint. The common resolver owns that normalization; consume only
      // its explicit receipt and still compare the entire bound below.
      if ((!onlyBoundPositionProjections(actual) &&
           !onlyContainedConsumerProjections(actual) &&
           !(captured_sibling && objectCapturedScopeProjection(actual))) ||
          !actual.throws_types.empty() ||
          actual.type_parameters.size() != method_formal_count ||
          actual.parameters.size() != parameters.size())
        return false;
      auto member_bindings = bindings;
      for (const auto &[formal, alpha] :
           methodFormalBindings(actual, cls.name, budget, &classes, &bindings))
        member_bindings.insert_or_assign(formal, alpha);
      for (size_t i = 0; i < method_formal_count; ++i) {
        const auto &formal = actual.type_parameters[i];
        if (std::pair{member_bindings.at(formal.name).descriptor,
                      substitute(actual,
                                 methodFormalBound(actual, formal, cls.name,
                                                   budget, &classes),
                                 member_bindings, cls.name, budget, 0, true)
                          .signature} != method_bounds[i])
          return false;
      }
      if (signatures.inherited_erased_contracts.contains(
              {candidate.reference, member.reference}))
        return true;
      for (size_t i = 0; i < parameters.size(); ++i)
        if (substitute(actual, actual.parameters[i], member_bindings, cls.name,
                       budget, 0, true)
                .signature != parameters[i].signature)
          return false;
      return sameOrContainedReturnType(substitute(actual, *actual.result,
                                                  member_bindings, cls.name,
                                                  budget, 0, true),
                                       result, budget) ||
             provenSelfCarrierReturn(
                 candidate.reference, member.reference, member_bindings, result,
                 method_formal_count, signatures, cls.name, budget) ||
             provenClassFormalBoundReturn(candidate.reference, member.reference,
                                          result.signature, method_formal_count,
                                          signatures);
    }
    current = &ancestor;
  }
  return false;
}

bool emptyMarkerHierarchy(const std::string &name, const ClassMap &classes,
                          Budget &budget) {
  std::map<std::string, unsigned> states;
  std::function<bool(const std::string &, unsigned)> visit =
      [&](const std::string &owner, unsigned depth) {
        budget.tick();
        const auto found = classes.find(owner);
        if (found == classes.end() || depth > 64 || states.size() > 128)
          return false;
        const auto &cls = found->second;
        if (!has(cls.access, "interface") ||
            cls.superclass != "Ljava/lang/Object;" || !cls.fields.empty() ||
            !cls.methods.empty() || cls.enclosing || cls.enclosing_method ||
            ((depth || !cls.interfaces.empty()) && cls.generic_signature))
          return false;
        auto &state = states[owner];
        if (state)
          return state == 2;
        state = 1;
        for (const auto &parent : cls.interfaces)
          if (!visit(parent, depth + 1))
            return false;
        state = 2;
        return true;
      };
  return visit(name, 0);
}

bool onlyEmptyMarkerParents(const Class &cls, const ClassMap &classes,
                            Budget &budget) {
  return std::all_of(cls.interfaces.begin(), cls.interfaces.end(),
                     [&](const std::string &parent) {
                       return emptyMarkerHierarchy(parent, classes, budget);
                     });
}

// A nearer original plain class declaration owns this unchanged runtime slot,
// including its narrowed checked exceptions. Its own source obligation still
// enters the member pass; an abstract declaration does not supply a body.
bool nearerPlainClassContract(const Class &cls, const Method &required,
                              const ClassMap &classes,
                              const GenericSignaturePlans &signatures,
                              const SourceMethodNames &names, Budget &budget) {
  if (required.generic_signature || has(required.access, "static") ||
      has(required.access, "private") || has(required.access, "final") ||
      required.reference.name.starts_with('<'))
    return false;
  const Class *current = &cls;
  std::set<std::string> seen;
  while (current->superclass) {
    budget.tick();
    if (!seen.insert(current->name).second || seen.size() > 64)
      return false;
    const auto parent = classes.find(*current->superclass);
    if (parent == classes.end() || has(parent->second.access, "interface") ||
        !independentSourceScope(parent->second) ||
        parent->first == required.reference.owner)
      return false;
    current = &parent->second;
    for (const auto &provided : current->methods) {
      budget.tick();
      if (!sameSourceMethodName(provided.reference, required.reference,
                                &names) ||
          provided.reference.parameters != required.reference.parameters ||
          has(provided.access, "private") ||
          inaccessiblePackageMethod(*current, provided, cls))
        continue;
      // Narrower covariant declarations and their original bridges do not
      // consume this unchanged source contract. Keep looking for the nearer
      // plain declaration that owns it; the member pass proves those slots.
      if (provided.reference.returns != required.reference.returns ||
          has(provided.access, "bridge"))
        continue;
      if (provided.generic_signature || has(provided.access, "static") ||
          (has(required.access, "public") && !has(provided.access, "public")) ||
          (has(required.access, "protected") &&
           !has(provided.access, "public") &&
           !has(provided.access, "protected")))
        return false;
      return sameCompleteMethodContract(provided, required, signatures,
                                        cls.name, budget, classes, true);
    }
  }
  return false;
}

bool provenCovariantClassOverride(const Method &provided,
                                  const Method &required,
                                  const GenericSignaturePlans &signatures,
                                  const ClassMap &classes, Budget &budget) {
  if (const auto abstract = signatures.abstract_class_covariant_contracts.find(
          {provided.reference, required.reference});
      abstract != signatures.abstract_class_covariant_contracts.end())
    return abstract->second.type_parameters.empty() &&
           abstract->second.throws_types.empty() &&
           abstract->second.source_projections.empty();
  auto erased = required.reference;
  erased.owner = provided.reference.owner;
  const auto proof =
      signatures.covariant_bridge_contracts.find({erased, required.reference});
  // The canonical signature proof retains independent method formals in
  // the complete ancestor witness; consuming a receipt must not erase them.
  const auto complete = [](const CovariantBridgeContract &candidate) {
    return candidate.contract.throws_types.empty() &&
           candidate.contract.source_projections.empty();
  };
  // A wider original bridge is another runtime entry for its typed source
  // implementation. When that implementation retains the ancestor's exact
  // result slot, no narrower-result bridge exists or should be invented.
  // Consume the original forwarding receipt and compare the implementation's
  // whole contract, including scoped formals, visibility and fixed throws.
  if (has(provided.access, "bridge") && !provided.generic_signature &&
      !has(required.access, "bridge") &&
      provided.reference.name == required.reference.name &&
      provided.reference.parameters == required.reference.parameters &&
      provided.reference.returns != required.reference.returns) {
    auto sibling = signatures.covariant_bridge_contracts.lower_bound(
        {provided.reference, MethodRef{}});
    for (; sibling != signatures.covariant_bridge_contracts.end() &&
           sibling->first.first == provided.reference;
         ++sibling) {
      budget.tick();
      if (!complete(sibling->second) ||
          sibling->second.raw_parameter_subsignature ||
          sibling->second.target.owner != provided.reference.owner)
        continue;
      const auto owner = classes.find(provided.reference.owner);
      const auto *target =
          owner == classes.end()
              ? nullptr
              : findMethod(owner->second, sibling->second.target);
      if (target && !has(target->access, "bridge") &&
          sameCompleteMethodContract(*target, required, signatures,
                                     provided.reference.owner, budget, classes,
                                     true))
        return true;
    }
  }
  // The common validator compares full scoped parameters and nominal result
  // ancestry and verifies this exact original bridge body. Either original
  // sibling consumes that same complete class receipt.
  if (proof == signatures.covariant_bridge_contracts.end() ||
      !complete(proof->second))
    return false;
  if (provided.reference == erased ||
      provided.reference == proof->second.target)
    return true;
  // Several ancestors can contribute different return slots for the same
  // source override. Another original bridge is compatible only when its own
  // complete forwarding receipt selects that identical typed implementation.
  // The required ancestor's exact receipt above remains mandatory.
  if (!has(provided.access, "bridge") ||
      (has(required.access, "public") && !has(provided.access, "public")) ||
      (has(required.access, "protected") && !has(provided.access, "public") &&
       !has(provided.access, "protected")))
    return false;
  auto sibling = signatures.covariant_bridge_contracts.lower_bound(
      {provided.reference, MethodRef{}});
  for (; sibling != signatures.covariant_bridge_contracts.end() &&
         sibling->first.first == provided.reference;
       ++sibling) {
    budget.tick();
    if (sibling->second.target == proof->second.target &&
        complete(sibling->second) &&
        !sibling->second.raw_parameter_subsignature)
      return true;
  }
  return false;
}

bool knownNongenericAncestors(const Class &parent, const Class &child,
                              const ClassMap &classes,
                              const GenericSignaturePlans &signatures,
                              Budget &budget,
                              bool separately_proven_interfaces = false,
                              bool include_parent = false,
                              const SourceMethodNames *method_names = nullptr,
                              bool complete_member_contracts = false) {
  const bool abstract_parent = has(parent.access, "abstract");
  const bool full_contracts = include_parent || complete_member_contracts;
  if (!parent.superclass || !independentSourceScope(parent) ||
      (abstract_parent && parent.external && !full_contracts))
    return false;
  std::map<std::string, unsigned> states;
  std::function<bool(const std::string &, unsigned, bool)> visit =
      [&](const std::string &name, unsigned depth, bool interface) {
        budget.tick();
        if (name == "Ljava/lang/Object;")
          return !interface;
        if (depth > 64 || states.size() > 128)
          return false;
        auto found = classes.find(name);
        if (found == classes.end() || signatures.classes.contains(name) ||
            !independentSourceScope(found->second) ||
            has(found->second.access, "interface") != interface)
          return false;
        auto &state = states[name];
        if (state)
          return state == 2;
        state = 1;
        const auto &ancestor = found->second;
        for (const auto &member : ancestor.methods) {
          budget.tick();
          if (inaccessiblePackageMethod(ancestor, member, child)) {
            if (has(member.access, "abstract"))
              return false;
            continue;
          }
          if (member.reference.name == "<init>" ||
              member.reference.name == "<clinit>" ||
              has(member.access, "private") ||
              (has(ancestor.access, "interface") &&
               has(member.access, "static")))
            continue;
          if (member.generic_signature) {
            const auto shape = signatures.methods.find(member.reference);
            if (!full_contracts || shape == signatures.methods.end() ||
                !shape->second.source_projections.empty())
              return false;
          }
          for (const auto &candidate : child.methods) {
            budget.tick();
            if (!sameSourceMethodName(candidate.reference, member.reference,
                                      method_names) ||
                candidate.reference.parameters != member.reference.parameters)
              continue;
            if (inheritedFinalBridgeUses(candidate, member, signatures, budget))
              continue;
            if (has(member.access, "final") ||
                has(member.access, "static") != has(candidate.access, "static"))
              return false;
            if (full_contracts) {
              // Traversal may omit the generic source parent while its plain
              // ancestors still own complete method contracts. Compare those
              // original scopes through the same shared full-member proof.
              if (!sameCompleteMethodContract(candidate, member, signatures,
                                              child.name, budget, classes,
                                              true) &&
                  !provenCovariantClassOverride(candidate, member, signatures,
                                                classes, budget))
                return false;
              continue;
            }
            const bool direct_slot =
                !include_parent &&
                std::any_of(parent.methods.begin(), parent.methods.end(),
                            [&](const Method &nearest) {
                              return nearest.reference.name ==
                                         candidate.reference.name &&
                                     nearest.reference.parameters ==
                                         candidate.reference.parameters;
                            });
            if (direct_slot)
              continue;
            // These members have no generic metadata in this profile. Their
            // public override needs that exact complete plain contract, with
            // no method formals, added checked throws or regenerated bridge.
            if (!has(candidate.access, "public") ||
                has(candidate.access, "bridge") ||
                candidate.reference.returns != member.reference.returns ||
                (candidate.declared_throws &&
                 !candidate.declared_throws->empty()))
              return false;
            if (auto signature = signatures.methods.find(candidate.reference);
                signature != signatures.methods.end()) {
              const auto &s = signature->second;
              if (!s.type_parameters.empty() || !s.throws_types.empty() ||
                  substitute(s, *s.result, {}, child.name, budget).signature !=
                      member.reference.returns)
                return false;
              for (size_t i = 0; i < s.parameters.size(); ++i)
                if (substitute(s, s.parameters[i], {}, child.name, budget)
                        .signature != member.reference.parameters.at(i))
                  return false;
            }
          }
        }
        if (ancestor.superclass &&
            !visit(*ancestor.superclass, depth + 1, false))
          return false;
        for (const auto &interface : ancestor.interfaces)
          if (!visit(interface, depth + 1, true))
            return false;
        state = 2;
        return true;
      };
  if (!visit(include_parent ? parent.name : *parent.superclass, 0, false))
    return false;
  for (const auto &interface : parent.interfaces) {
    if (separately_proven_interfaces && !parent.external) {
      const auto own = signatures.classes.find(parent.name);
      if (own != signatures.classes.end() &&
          own->second.source_projections.empty() &&
          std::any_of(own->second.interfaces.begin(),
                      own->second.interfaces.end(), [&](GenericTypeId id) {
                        const auto &edge = own->second.types.at(id);
                        return edge.erasure == interface &&
                               edge.segments.size() == 1 &&
                               !edge.segments[0].arguments.empty();
                      }))
        continue; // The input parent's unchanged generic interface edge and
                  // every member are validated by the common inheritance pass.
    }
    if (!visit(interface, 0, true))
      return false;
  }
  // A concrete child must supply these plain abstract contracts. An abstract
  // child may retain the nearest original abstract class declaration when its
  // complete class contract is included. The same scoped member comparison
  // still checks visibility, parameters, result and validated fixed throws.
  std::vector<const Class *> providers;
  if (abstract_parent || full_contracts)
    providers.push_back(&child);
  for (const Class *provider = &parent; provider;) {
    budget.tick();
    providers.push_back(provider);
    provider =
        provider->superclass && *provider->superclass != "Ljava/lang/Object;"
            ? &classes.at(*provider->superclass)
            : nullptr;
  }
  for (const auto &[name, state] : states)
    for (const auto &required : classes.at(name).methods) {
      budget.tick();
      if (!has(required.access, "abstract") || has(required.access, "static"))
        continue;
      const Method *implementation = nullptr;
      for (const auto *provider : providers) {
        const Method *fallback = nullptr;
        for (const auto &candidate : provider->methods) {
          budget.tick();
          if (!sameSourceMethodName(candidate.reference, required.reference,
                                    method_names) ||
              candidate.reference.parameters != required.reference.parameters)
            continue;
          if (!fallback)
            fallback = &candidate;
          if (candidate.reference.returns == required.reference.returns) {
            implementation = &candidate;
            break;
          }
        }
        if (!implementation)
          implementation = fallback;
        if (implementation)
          break;
      }
      if (!implementation || has(implementation->access, "static") ||
          (has(implementation->access, "abstract") &&
           (!full_contracts || !has(child.access, "abstract"))))
        return false;
      if (full_contracts || abstract_parent) {
        if (!sameCompleteMethodContract(*implementation, required, signatures,
                                        child.name, budget, classes, true) &&
            !provenCovariantClassOverride(*implementation, required, signatures,
                                          classes, budget))
          return false;
      } else {
        if (implementation->reference.returns != required.reference.returns ||
            !has(implementation->access, "public"))
          return false;
        if (auto signature = signatures.methods.find(implementation->reference);
            signature != signatures.methods.end()) {
          if (!signature->second.type_parameters.empty())
            return false;
          for (size_t i = 0; i < signature->second.parameters.size(); ++i)
            if (substitute(signature->second, signature->second.parameters[i],
                           {}, parent.name, budget)
                    .signature != required.reference.parameters.at(i))
              return false;
        }
      }
    }
  return true;
}

bool sameCompleteMethodContract(const Method &provided, const Method &required,
                                const GenericSignaturePlans &signatures,
                                const std::string &context, Budget &budget,
                                const ClassMap &classes, bool class_contract) {
  budget.tick();
  const auto owner = classes.find(required.reference.owner);
  const bool class_visibility = class_contract && owner != classes.end() &&
                                !has(owner->second.access, "interface") &&
                                !has(required.access, "public") &&
                                !has(required.access, "private") &&
                                !has(provided.access, "private") &&
                                (has(required.access, "protected")
                                     ? has(provided.access, "protected")
                                     : packageOf(required.reference.owner) ==
                                           packageOf(provided.reference.owner));
  const auto terminal =
      signatures.nonreturning_void_bridges.find(provided.reference);
  const bool raw_terminal =
      terminal != signatures.nonreturning_void_bridges.end() &&
      terminal->second.erased_method_formals &&
      terminal->second.erased_contracts.contains(required.reference);
  if (provided.reference.name != required.reference.name ||
      provided.reference.parameters != required.reference.parameters ||
      provided.reference.returns != required.reference.returns ||
      (!has(provided.access, "public") && !class_visibility) ||
      has(provided.access, "static") ||
      (has(provided.access, "bridge") && !raw_terminal &&
       !signatures.public_superclass_access_bridges.contains(
           provided.reference) &&
       !sourceContractImplementation(classes.at(provided.reference.owner),
                                     provided, required, classes, signatures,
                                     budget)) ||
      !compatibleClosedInheritedThrows(provided, required, signatures))
    return false;
  // The canonical body/ancestor proof owns this exact raw subsignature. Keep
  // the original bridge without method formals rather than copying a foreign
  // ancestor's formal into it during this independent inheritance traversal.
  if (raw_terminal)
    return !has(required.access, "static");
  // The invocation validator already proved this entire closed method shape:
  // one own formal, two plain ordered nominal bounds, its direct parameter
  // position, unchanged scalar context and no throws or projections. Reuse that
  // same contract for an unchanged implementation, including alpha-renamed
  // formals; do not reduce an intersection to its first bound in general
  // substitution.
  const auto provided_intersection =
      signatures.intersection_invocations.find(provided.reference);
  const auto required_intersection =
      signatures.intersection_invocations.find(required.reference);
  if (provided_intersection != signatures.intersection_invocations.end() ||
      required_intersection != signatures.intersection_invocations.end())
    return !has(required.access, "static") &&
           provided_intersection != signatures.intersection_invocations.end() &&
           required_intersection != signatures.intersection_invocations.end() &&
           provided_intersection->second == required_intersection->second;
  // Equivalent validated fixed throws sets and empty narrowing use the same
  // shared compatibility receipt. Keep both original ordered declarations;
  // this does not add exceptions or relax original bridge-set equality.
  const auto completeSignature =
      [&](const Method &method) -> const GenericSignature * {
    if (const auto closed =
            signatures.closed_reference_getter_bridges.find(method.reference);
        closed != signatures.closed_reference_getter_bridges.end())
      return &closed->second.signature;
    if (const auto bridge =
            signatures.nonreturning_void_bridges.find(method.reference);
        bridge != signatures.nonreturning_void_bridges.end() &&
        bridge->second.source_signature)
      return &*bridge->second.source_signature;
    const auto original = signatures.methods.find(method.reference);
    return original == signatures.methods.end() ? nullptr : &original->second;
  };
  auto shape = [&](const Method &method) {
    std::vector<std::string> types;
    size_t formals = 0;
    if (const auto *full = completeSignature(method)) {
      const auto &signature = *full;
      if (!signature.source_projections.empty() ||
          !signature.throws_types.empty() || !signature.result ||
          signature.parameters.size() != method.reference.parameters.size())
        return std::pair<size_t, std::vector<std::string>>{SIZE_MAX, {}};
      const auto bindings =
          methodFormalBindings(signature, context, budget, &classes);
      formals = signature.type_parameters.size();
      for (const auto &formal : signature.type_parameters) {
        const auto bound =
            methodFormalBound(signature, formal, context, budget, &classes);
        types.push_back(
            "bound:" + formal.erasure + ":" +
            substitute(signature, bound, bindings, context, budget, 0, true)
                .signature);
      }
      for (auto parameter : signature.parameters) {
        budget.tick();
        types.push_back(
            substitute(signature, parameter, bindings, context, budget, 0, true)
                .signature);
      }
      types.push_back(substitute(signature, *signature.result, bindings,
                                 context, budget, 0, true)
                          .signature);
    } else {
      types = method.reference.parameters;
      types.push_back(method.reference.returns);
    }
    return std::pair{formals, types};
  };
  const auto actual = shape(provided), expected = shape(required);
  if (actual.first == SIZE_MAX)
    return false;
  if (actual == expected)
    return true;
  if (!provided.generic_signature && !expected.first &&
      !signatures.nonreturning_void_bridges.contains(provided.reference) &&
      !signatures.closed_reference_getter_bridges.contains(
          provided.reference)) {
    const auto full = signatures.methods.find(required.reference);
    if (full != signatures.methods.end()) {
      std::vector<SubstitutedType> parameters;
      for (auto parameter : full->second.parameters)
        parameters.push_back(
            substitute(full->second, parameter, {}, context, budget, 0, true));
      const auto result = substitute(full->second, *full->second.result, {},
                                     context, budget, 0, true);
      if (originalRawParameterSubsignature(provided, required, parameters,
                                           result, expected.first))
        return true;
    }
  }
  if (actual.first != expected.first || actual.second.empty() ||
      actual.second.size() != expected.second.size() ||
      !std::equal(actual.second.begin(), actual.second.end() - 1,
                  expected.second.begin()))
    return false;
  const auto result_type = [&](const Method &method) {
    if (const auto *full = completeSignature(method)) {
      const auto &signature = *full;
      return substitute(
          signature, *signature.result,
          methodFormalBindings(signature, context, budget, &classes), context,
          budget, 0, true);
    }
    return SubstitutedType{method.reference.returns, method.reference.returns};
  };
  return closedParameterizedRawType(result_type(provided),
                                    result_type(required)) ||
         provenClassFormalBoundReturn(provided.reference, required.reference,
                                      expected.second.back(), actual.first,
                                      signatures);
}

bool knownRawSourceInterfaces(const Class &parent, const Class &child,
                              const ClassMap &classes,
                              const GenericSignaturePlans &signatures,
                              Budget &budget,
                              const std::string *root = nullptr);

bool fixedSourceInterfaceContract(const Class &cls, const ClassMap &classes,
                                  const GenericSignaturePlans &signatures,
                                  Budget &budget,
                                  bool include_method_contracts = true);

bool externalGenericParents(const Class &parent, const Class &child,
                            const ClassMap &classes,
                            const GenericSignaturePlans &signatures,
                            Budget &budget);

bool sourceGenericParents(const Class &parent, const Class &child,
                          const ClassMap &classes,
                          const GenericSignaturePlans &signatures,
                          Budget &budget) {
  // Every source edge and ancestor member is checked by the same complete
  // substitution pass, regardless of class order. A forwarding superclass
  // need not redeclare a more distant method for a child to override it.
  std::set<std::string> seen;
  for (const Class *current = &parent; current;) {
    budget.tick();
    if (current->external)
      return current != &parent &&
             externalGenericParents(*current, child, classes, signatures,
                                    budget);
    const auto found = signatures.classes.find(current->name);
    if (!seen.insert(current->name).second || seen.size() > 64 ||
        has(current->access, "interface") ||
        !independentSourceScope(*current) || !current->superclass ||
        found == signatures.classes.end() ||
        std::none_of(current->methods.begin(), current->methods.end(),
                     [](const Method &method) {
                       return method.reference.name == "<init>";
                     }))
      return false;
    if (*current->superclass != "Ljava/lang/Object;") {
      const auto &super = found->second.types.at(*found->second.superclass);
      if (super.segments.size() != 1)
        return false;
      if (super.segments.front().arguments.empty())
        return knownNongenericAncestors(*current, child, classes, signatures,
                                        budget, true, false, nullptr, true);
    }
    for (auto id : found->second.interfaces) {
      budget.tick();
      const auto &edge = found->second.types.at(id);
      const auto definition = classes.find(edge.erasure);
      if (definition == classes.end() ||
          !has(definition->second.access, "interface"))
        return false;
      const bool parameterized = std::any_of(
          edge.segments.begin(), edge.segments.end(),
          [](const auto &segment) { return !segment.arguments.empty(); });
      if (!parameterized &&
          !emptyMarkerHierarchy(definition->first, classes, budget)) {
        // Fixed parameterized ancestry is consumed by the common member
        // queue, including every intervening declaration and original bridge.
        // Method-only plain contracts retain the existing raw proof below.
        if (fixedSourceInterfaceContract(definition->second, classes,
                                         signatures, budget, false))
          continue;
        const auto declaration = signatures.classes.find(edge.erasure);
        if ((declaration != signatures.classes.end() &&
             !declaration->second.type_parameters.empty()) ||
            !knownRawSourceInterfaces(*current, child, classes, signatures,
                                      budget, &edge.erasure))
          return false;
      }
    }
    if (*current->superclass == "Ljava/lang/Object;")
      return true;
    const auto &super = found->second.types.at(*found->second.superclass);
    if (super.segments.size() != 1 || super.segments.front().arguments.empty())
      return false;
    const auto ancestor = classes.find(*current->superclass);
    if (ancestor == classes.end())
      return false;
    current = &ancestor->second;
  }
  return false;
}

bool plainInterfaceFields(const Class &cls, Budget &budget) {
  // The shared declaration validator already checks every interface field's
  // owner, descriptor and public/static/final role, on linked and direct paths.
  // Dependency initialization stays in the unchanged library. Source fields
  // stay in their original interface and use the shared field/initializer
  // planner; this inheritance predicate neither evaluates nor moves them.
  // Generic field scopes still require their separate complete proof.
  return std::all_of(cls.fields.begin(), cls.fields.end(),
                     [&](const Field &field) {
                       budget.tick();
                       return !field.generic_signature;
                     });
}

bool ownsGenericMethodContract(const Class &cls,
                               const GenericSignaturePlans &signatures,
                               Budget &budget) {
  for (const auto &member : cls.methods) {
    budget.tick();
    const auto method = signatures.methods.find(member.reference);
    if (!has(member.access, "public") || has(member.access, "static") ||
        has(member.access, "bridge") || method == signatures.methods.end() ||
        !method->second.source_projections.empty())
      continue;
    if (!method->second.type_parameters.empty())
      return true;
    for (const auto &type : method->second.types) {
      budget.tick();
      if (std::any_of(
              type.segments.begin(), type.segments.end(),
              [](const auto &segment) { return !segment.arguments.empty(); }))
        return true;
    }
  }
  return false;
}

// A raw class type erases member parameter types and method-owned formals.
// An original generic override cannot acquire that slot by equal erasure.
// Unchanged raw defaults remain in their declaration and require no override.
bool rawGenericMethodOverride(const Class &consumer, const Class &declaration,
                              const ClassMap &classes,
                              const GenericSignaturePlans &signatures,
                              const SourceMethodNames &names, Budget &budget) {
  std::vector<const Class *> pending{&declaration};
  std::set<std::string> interfaces;
  for (size_t at = 0; at < pending.size(); ++at) {
    const auto &current = *pending[at];
    budget.tick();
    if (!interfaces.insert(current.name).second)
      continue;
    if (interfaces.size() > 128 || !has(current.access, "interface") ||
        current.superclass != "Ljava/lang/Object;")
      unsupported(consumer.name,
                  "raw method-formal interface graph is incomplete");
    for (const auto &ancestor : current.interfaces) {
      const auto known = classes.find(ancestor);
      if (known == classes.end())
        unsupported(consumer.name,
                    "raw method-formal interface ancestor is unknown");
      pending.push_back(&known->second);
    }
    const auto raw_owner = signatures.classes.find(current.name);
    if (raw_owner == signatures.classes.end() ||
        raw_owner->second.type_parameters.empty())
      continue;
    for (const auto &required : current.methods) {
      budget.tick();
      if (!has(required.access, "public") || has(required.access, "static") ||
          has(required.access, "bridge"))
        continue;
      const Class *owner = &consumer;
      std::set<std::string> visited;
      while (owner) {
        budget.tick();
        if (!visited.insert(owner->name).second || visited.size() > 64)
          unsupported(
              consumer.name,
              "raw method-formal provider graph is cyclic or exceeds limit");
        bool nearest_slot = false;
        for (const auto &provided : owner->methods) {
          budget.tick();
          if ((owner != &consumer &&
               (has(provided.access, "private") ||
                inaccessiblePackageMethod(*owner, provided, consumer))) ||
              !sameSourceMethodName(provided.reference, required.reference,
                                    &names) ||
              provided.reference.parameters != required.reference.parameters)
            continue;
          nearest_slot = true;
          if (signatures.raw_interface_method_sources.contains(
                  provided.reference))
            continue; // Full original method scopes were matched before source
                      // erasure.
          const auto scoped = signatures.methods.find(provided.reference);
          if (scoped == signatures.methods.end())
            continue;
          if (!scoped->second.type_parameters.empty())
            return true;
          // Only parameter types participate in the source subsignature.
          // Generic covariant results retain their existing result proof.
          for (auto parameter : scoped->second.parameters) {
            const GenericType *type = &scoped->second.types.at(parameter);
            while (type->kind == GenericType::Kind::Array) {
              budget.tick();
              type = &scoped->second.types.at(*type->element);
            }
            if (type->kind == GenericType::Kind::TypeVariable ||
                std::any_of(type->segments.begin(), type->segments.end(),
                            [](const auto &segment) {
                              return !segment.arguments.empty();
                            }))
              return true;
          }
        }
        if (nearest_slot || !owner->superclass ||
            owner->superclass == "Ljava/lang/Object;")
          break;
        const auto parent = classes.find(*owner->superclass);
        if (parent == classes.end())
          break; // Existing ancestry validation owns unknown declarations.
        const auto parent_plan = signatures.classes.find(parent->first);
        if (parent_plan != signatures.classes.end() &&
            !parent_plan->second.type_parameters.empty()) {
          const auto child_plan = signatures.classes.find(owner->name);
          if (child_plan == signatures.classes.end() ||
              !child_plan->second.superclass)
            break; // A raw class parent also erases inherited method formals.
          const auto &edge =
              child_plan->second.types.at(*child_plan->second.superclass);
          if (edge.erasure != parent->first ||
              std::none_of(edge.segments.begin(), edge.segments.end(),
                           [](const auto &segment) {
                             return !segment.arguments.empty();
                           }))
            break;
        }
        owner = &parent->second;
      }
    }
  }
  return false;
}

// A raw generic interface erases its own member contract. A signature-free
// class chain must still satisfy every original class override and inherited
// interface obligation before avoiding the full nongeneric-member profile.
bool plainRawClassContracts(const Class &child, const ClassMap &classes,
                            const GenericSignaturePlans &signatures,
                            const SourceMethodNames &names, Budget &budget) {
  std::vector<const Class *> providers;
  std::set<std::string> seen;
  for (const Class *current = &child; current;) {
    budget.tick();
    if (!seen.insert(current->name).second || seen.size() > 64 ||
        current->generic_signature ||
        signatures.classes.contains(current->name) ||
        !independentSourceScope(*current) ||
        has(current->access, "interface") || !current->superclass)
      return false;
    providers.push_back(current);
    if (*current->superclass == "Ljava/lang/Object;")
      break;
    const auto parent = classes.find(*current->superclass);
    if (parent == classes.end())
      return false;
    current = &parent->second;
  }
  std::function<bool(const Method &, const Method &, bool)> matches =
      [&](const Method &provided, const Method &required, bool raw) {
        budget.tick();
        if (!raw)
          return sameCompleteMethodContract(provided, required, signatures,
                                            child.name, budget, classes,
                                            true) ||
                 provenCovariantClassOverride(provided, required, signatures,
                                              classes, budget);
        if (!has(provided.access, "public") || has(provided.access, "static") ||
            !compatibleClosedInheritedThrows(provided, required, signatures))
          return false;
        const auto shape = signatures.methods.find(provided.reference);
        if (shape != signatures.methods.end()) {
          const auto &actual = shape->second;
          if (!raw || !actual.type_parameters.empty() ||
              !actual.source_projections.empty() ||
              !actual.throws_types.empty() || !actual.result ||
              actual.parameters.size() != required.reference.parameters.size())
            return false;
          for (size_t n = 0; n < actual.parameters.size(); ++n)
            if (substitute(actual, actual.parameters[n], {}, child.name, budget,
                           0, true)
                    .signature != required.reference.parameters[n])
              return false;
        } else if (provided.generic_signature)
          return false;
        if (has(provided.access, "bridge") &&
            !classes.at(provided.reference.owner).external) {
          const Method *target = nullptr;
          for (const auto &op : provided.instructions)
            if (const auto *call = std::get_if<MethodRef>(&op.reference);
                call && call->owner == provided.reference.owner &&
                call->name == provided.reference.name &&
                call->parameters == provided.reference.parameters &&
                call->returns != provided.reference.returns) {
              if (target)
                return false;
              target = findMethod(classes.at(call->owner), *call);
            }
          if (!target || has(target->access, "bridge"))
            return false;
          verifyGenericBridge(provided, *target, budget, nullptr, nullptr,
                              &classes, &signatures);
          // The original erased entry delegates to this source implementation.
          // Its wider return is not a competing implementation of a narrower
          // interface slot; the entire original forwarding body is proven
          // above.
          return matches(*target, required, raw);
        }
        if (provided.reference.returns != required.reference.returns) {
          if (!isAssignableReference(provided.reference.returns,
                                     required.reference.returns, classes,
                                     budget))
            return false;
          auto erased = required.reference;
          erased.owner = provided.reference.owner;
          const auto &owner = classes.at(erased.owner);
          const auto *bridge = findMethod(owner, erased);
          if (!bridge ||
              bridge->access != Access{"public", "bridge", "synthetic"})
            return false;
          // A nominal subtype alone cannot invent the erased runtime entry.
          if (!owner.external)
            verifyGenericBridge(*bridge, provided, budget, nullptr, nullptr,
                                &classes, &signatures);
        }
        return true;
      };
  for (size_t n = 1; n < providers.size(); ++n)
    for (const auto &required : providers[n]->methods) {
      if (required.reference.name.starts_with('<') ||
          has(required.access, "private") ||
          inaccessiblePackageMethod(*providers[n], required, child))
        continue;
      for (size_t earlier = 0; earlier < n; ++earlier)
        for (const auto &provided : providers[earlier]->methods)
          if (sameSourceMethodName(provided.reference, required.reference,
                                   &names) &&
              provided.reference.parameters == required.reference.parameters &&
              (has(required.access, "final") ||
               has(required.access, "static") ||
               !matches(provided, required, false)))
            return false;
    }
  std::map<std::string, unsigned> states;
  std::map<std::pair<std::string, std::vector<std::string>>, std::string>
      deferred;
  std::function<bool(const std::string &, unsigned)> visit =
      [&](const std::string &name, unsigned depth) {
        budget.tick();
        const auto found = classes.find(name);
        if (depth > 64 || states.size() > 128 || found == classes.end() ||
            !has(found->second.access, "interface") ||
            !independentSourceScope(found->second) ||
            found->second.superclass != "Ljava/lang/Object;")
          return false;
        auto &state = states[name];
        if (state)
          return state == 2;
        state = 1;
        const auto shape = signatures.classes.find(name);
        const bool raw = shape != signatures.classes.end() &&
                         !shape->second.type_parameters.empty();
        if (shape != signatures.classes.end() &&
            (!shape->second.source_projections.empty() ||
             (!raw && !shape->second.interfaces.empty())))
          return false;
        for (const auto &required : found->second.methods) {
          budget.tick();
          if (has(required.access, "static") || has(required.access, "private"))
            continue;
          if (!has(required.access, "public") ||
              has(required.access, "bridge") ||
              (required.declared_throws && !required.declared_throws->empty() &&
               !validatedClosedDeclaredThrows(required, signatures)))
            return false;
          const Method *implementation = nullptr;
          for (const auto *owner : providers) {
            for (const auto &provided : owner->methods)
              if (sameSourceMethodName(provided.reference, required.reference,
                                       &names) &&
                  provided.reference.parameters ==
                      required.reference.parameters) {
                if (!matches(provided, required, raw))
                  return false;
                if (!implementation ||
                    provided.reference.returns == required.reference.returns)
                  implementation = &provided;
              }
            if (implementation)
              break;
          }
          if (!implementation) {
            // Fixed nongeneric member scopes need their existing complete
            // abstract-obligation proof, not a comparison of erased results.
            if (!raw || !has(child.access, "abstract") ||
                !has(required.access, "abstract"))
              return false;
            auto [prior, inserted] =
                deferred.emplace(std::pair{required.reference.name,
                                           required.reference.parameters},
                                 required.reference.returns);
            if (!inserted && prior->second != required.reference.returns)
              return false;
          } else if (has(implementation->access, "abstract") &&
                     !has(child.access, "abstract"))
            return false;
        }
        for (const auto &ancestor : found->second.interfaces)
          if (!visit(ancestor, depth + 1))
            return false;
        state = 2;
        return true;
      };
  for (const auto *provider : providers)
    for (const auto &interface : provider->interfaces)
      if (!visit(interface, 0))
        return false;
  return true;
}

bool fixedDependencyClassAncestors(const Class &parent, const Class &child,
                                   const ClassMap &classes,
                                   const GenericSignaturePlans &signatures,
                                   Budget &budget) {
  if (!parent.external || has(parent.access, "interface"))
    return false;
  const auto initial = signatures.classes.find(parent.name);
  if (initial != signatures.classes.end() &&
      !initial->second.type_parameters.empty())
    return false;
  // A concrete dependency ancestor owns the implementations. An abstract
  // prefix above it may add concrete members and empty markers, but cannot
  // introduce or reabstract any obligation. Keep the original fixed superclass
  // arguments and library bodies; no dependency declaration is copied to
  // source.
  auto overrides = [&](const Class &owner) {
    for (const auto &member : owner.methods) {
      budget.tick();
      if (member.reference.name == "<init>" ||
          member.reference.name == "<clinit>" ||
          has(member.access, "private") ||
          inaccessiblePackageMethod(owner, member, child) ||
          (has(owner.access, "interface") && has(member.access, "static")))
        continue;
      const auto shape = signatures.methods.find(member.reference);
      if (member.generic_signature &&
          (shape == signatures.methods.end() ||
           !shape->second.source_projections.empty()))
        return false;
      const bool class_formals =
          shape != signatures.methods.end() &&
          std::any_of(shape->second.types.begin(), shape->second.types.end(),
                      [&](const GenericType &type) {
                        return type.kind == GenericType::Kind::TypeVariable &&
                               type.variable_owner !=
                                   member.reference.identity();
                      });
      for (const auto &candidate : child.methods) {
        budget.tick();
        if (candidate.reference.name != member.reference.name)
          continue;
        if (inheritedFinalBridgeUses(candidate, member, signatures, budget))
          continue;
        // This bounded prefix proof admits only independent member contracts.
        // An input override of a class-owned formal needs the full composed
        // member pass, including its original erased bridge, rather than an
        // unbound erasure comparison here.
        if (class_formals && candidate.reference.parameters.size() ==
                                 member.reference.parameters.size())
          return false;
        if (candidate.reference.parameters != member.reference.parameters)
          continue;
        if (has(member.access, "final") ||
            has(member.access, "static") != has(candidate.access, "static") ||
            !sameCompleteMethodContract(candidate, member, signatures,
                                        child.name, budget, classes, true))
          return false;
      }
    }
    return true;
  };
  std::vector<const Class *> providers;
  std::set<std::string> seen;
  std::map<std::string, SubstitutedType> bindings;
  bool fixed_formals = false;
  bool concrete_provider = false;
  for (const Class *current = &parent; current;) {
    budget.tick();
    const auto shape = signatures.classes.find(current->name);
    if (!seen.insert(current->name).second || seen.size() > 64 ||
        !current->external || has(current->access, "interface") ||
        !independentSourceScope(*current) || !current->superclass ||
        (shape != signatures.classes.end() &&
         (!shape->second.source_projections.empty() ||
          shape->second.type_parameters.size() != bindings.size())) ||
        (shape == signatures.classes.end() && !bindings.empty()) ||
        std::none_of(current->methods.begin(), current->methods.end(),
                     [](const Method &method) {
                       return method.reference.name == "<init>";
                     }) ||
        !overrides(*current))
      return false;
    if (!concrete_provider) {
      if (!has(current->access, "abstract"))
        concrete_provider = true;
      else if (!onlyEmptyMarkerParents(*current, classes, budget) ||
               std::any_of(current->methods.begin(), current->methods.end(),
                           [](const Method &method) {
                             return has(method.access, "abstract");
                           }))
        return false;
    }
    providers.push_back(current);
    if (*current->superclass == "Ljava/lang/Object;")
      break;
    const auto ancestor = classes.find(*current->superclass);
    if (ancestor == classes.end())
      return false;
    auto composed = composeClassEdge(*current, ancestor->second, bindings,
                                     signatures, child.name, budget);
    const auto formals = signatures.classes.find(ancestor->first);
    const auto count = formals == signatures.classes.end()
                           ? 0
                           : formals->second.type_parameters.size();
    if (!composed || composed->size() != count ||
        std::any_of(composed->begin(), composed->end(), [&](const auto &entry) {
          budget.tick();
          return entry.second.open;
        }))
      return false;
    fixed_formals |= count != 0;
    bindings = std::move(*composed);
    current = &ancestor->second;
  }
  // A purely plain chain must retain the full nongeneric abstract-obligation
  // proof above; this distinct prefix profile cannot mask its failure.
  if (!fixed_formals || !concrete_provider)
    return false;
  std::map<std::string, unsigned> states;
  std::function<bool(const std::string &, unsigned)> visit =
      [&](const std::string &name, unsigned depth) {
        budget.tick();
        const auto found = classes.find(name);
        const auto shape = signatures.classes.find(name);
        if (depth > 64 || states.size() > 128 || found == classes.end() ||
            !found->second.external ||
            !has(found->second.access, "interface") ||
            found->second.superclass != "Ljava/lang/Object;" ||
            !independentSourceScope(found->second) ||
            (shape != signatures.classes.end() &&
             !shape->second.source_projections.empty()))
          return false;
        auto &state = states[name];
        if (state)
          return state == 2;
        state = 1;
        if (!overrides(found->second))
          return false;
        for (const auto &ancestor : found->second.interfaces)
          if (!visit(ancestor, depth + 1))
            return false;
        state = 2;
        return true;
      };
  for (const auto *provider : providers)
    for (const auto &interface : provider->interfaces)
      if (!visit(interface, 0))
        return false;
  return true;
}

bool plainRuntimeClassOwner(const Class &cls, const ClassMap &classes,
                            const GenericSignaturePlans &signatures,
                            Budget &budget,
                            const SourceMethodNames *method_names = nullptr) {
  if (cls.superclass == "Ljava/lang/Object;")
    return true;
  if (!cls.superclass)
    return false;
  const auto parent = classes.find(*cls.superclass);
  // Plain source chains and closed concrete dependency prefixes use their
  // original declarations. Both proofs check every class/interface role,
  // independent scope and matching input member's complete contract.
  return parent != classes.end() &&
         (knownNongenericAncestors(parent->second, cls, classes, signatures,
                                   budget, false, true, method_names) ||
          fixedDependencyClassAncestors(parent->second, cls, classes,
                                        signatures, budget));
}

// This only discovers a complete zero-formal class header. Its original
// ancestor graph, member substitution and every checked bridge still pass
// through the shared inheritance planner below.
bool closedGenericClassContract(const Class &cls,
                                const GenericSignaturePlans &signatures,
                                Budget &budget) {
  budget.tick();
  const auto *contract = inheritanceClassSignature(cls.name, signatures);
  if (has(cls.access, "interface") || !independentSourceScope(cls) ||
      !contract || !contract->type_parameters.empty() ||
      !contract->source_projections.empty())
    return false;
  return std::any_of(contract->types.begin(), contract->types.end(),
                     [&](const GenericType &type) {
                       budget.tick();
                       return std::any_of(
                           type.segments.begin(), type.segments.end(),
                           [](const GenericClassSegment &segment) {
                             return !segment.arguments.empty();
                           });
                     });
}

bool ownsFixedSourceInterfaceContract(const Class &cls,
                                      const GenericSignaturePlans &signatures,
                                      Budget &budget,
                                      bool include_method_contracts = true) {
  if (!has(cls.access, "interface") || cls.superclass != "Ljava/lang/Object;" ||
      !independentSourceScope(cls))
    return false;
  const auto *contract = inheritanceClassSignature(cls.name, signatures);
  if (contract) {
    if (!contract->type_parameters.empty() ||
        !contract->source_projections.empty())
      return false;
    for (auto id : contract->interfaces) {
      budget.tick();
      const auto &edge = contract->types.at(id);
      if (std::any_of(
              edge.segments.begin(), edge.segments.end(),
              [](const auto &segment) { return !segment.arguments.empty(); }))
        return true;
    }
  } else if (cls.generic_signature)
    return false;
  if (!include_method_contracts)
    return false;
  // A nongeneric declaration can own complete method formals or
  // parameterized method types even when its class has no Signature. Consume
  // those original contracts with the full inherited-member proof below.
  return ownsGenericMethodContract(cls, signatures, budget);
}

// A plain interface can carry a fixed generic ancestor without needing
// its own Signature. Route the complete chain into the common member planner;
// this discovery does not prove a default or omit intervening declarations.
// Transitive discovery includes method-only contracts through plain owned or
// dependency interfaces. Full member substitution remains in the common
// planner.
bool fixedSourceInterfaceContract(const Class &cls, const ClassMap &classes,
                                  const GenericSignaturePlans &signatures,
                                  Budget &budget,
                                  bool include_method_contracts) {
  std::set<std::string> seen;
  std::function<bool(const Class &, unsigned)> visit = [&](const Class &current,
                                                           unsigned depth) {
    budget.tick();
    if (!seen.insert(current.name).second)
      return false;
    if (depth > 64 || seen.size() > 128)
      unsupported(cls.name, "fixed source interface graph exceeds limit");
    // Source and SDK declarations with the same complete fixed ancestor
    // enter the same member/substitution proof. The source-only discovery
    // mode retains its original boundary.
    if (current.external && !include_method_contracts)
      return false;
    if (ownsFixedSourceInterfaceContract(current, signatures, budget,
                                         include_method_contracts))
      return true;
    if (!has(current.access, "interface") ||
        current.superclass != "Ljava/lang/Object;" ||
        !independentSourceScope(current))
      return false;
    if (const auto *plan = inheritanceClassSignature(current.name, signatures)) {
      if (!plan->type_parameters.empty() || !plan->source_projections.empty())
        return false;
    } else if (current.generic_signature)
      return false;
    for (const auto &edge : current.interfaces) {
      const auto parent = classes.find(edge);
      if (parent != classes.end() && visit(parent->second, depth + 1))
        return true;
    }
    return false;
  };
  return visit(cls, 0);
}

GenericSignature plainRuntimeInterfaceSignature(const Class &declaration,
                                                Budget &budget) {
  std::string runtime = declaration.superclass.value_or("Ljava/lang/Object;");
  for (const auto &edge : declaration.interfaces)
    runtime += edge;
  auto shape =
      parseGenericSignature(GenericSignatureKind::Class, runtime, budget);
  shape.types.at(*shape.superclass).erasure =
      declaration.superclass.value_or("Ljava/lang/Object;");
  for (size_t i = 0; i < shape.interfaces.size(); ++i)
    shape.types.at(shape.interfaces[i]).erasure = declaration.interfaces.at(i);
  return shape;
}

bool nonReturningVoidTargetBody(const Method &target, const ClassMap &classes,
                                Budget &budget) {
  if (target.access != Access{"public"} ||
      target.reference.returns != "Ljava/lang/Void;" || !target.tries.empty() ||
      (target.instructions.size() != 4 && target.instructions.size() != 6) ||
      target.registers < target.incomingWords() ||
      (target.declared_throws && !target.declared_throws->empty()))
    return false;
  const auto &body = target.instructions;
  if (body.size() == 6) {
    const auto self = target.registers - target.incomingWords();
    const auto *exception = std::get_if<std::string>(&body[0].reference);
    const auto *message = std::get_if<std::string>(&body[1].literal);
    const auto *stringify = std::get_if<MethodRef>(&body[2].reference);
    const auto *init = std::get_if<MethodRef>(&body[4].reference);
    if (!exception || *exception != "Ljava/lang/IllegalStateException;" ||
        body[0].opcode != "new-instance" || body[0].registers.size() != 1 ||
        body[0].registers[0] >= self || !message ||
        (body[1].opcode != "const-string" &&
         body[1].opcode != "const-string/jumbo") ||
        body[1].registers.size() != 1 || body[1].registers[0] >= self ||
        body[1].registers[0] == body[0].registers[0] ||
        !std::holds_alternative<std::monostate>(body[1].reference) ||
        (body[2].opcode != "invoke-virtual" &&
         body[2].opcode != "invoke-virtual/range") ||
        !stringify ||
        *stringify !=
            MethodRef{
                "Ljava/lang/Object;", "toString", {}, "Ljava/lang/String;"} ||
        body[2].registers != body[1].registers ||
        body[3].opcode != "move-result-object" ||
        body[3].registers != body[1].registers ||
        !std::holds_alternative<std::monostate>(body[3].reference) ||
        (body[4].opcode != "invoke-direct" &&
         body[4].opcode != "invoke-direct/range") ||
        !init ||
        *init != MethodRef{*exception, "<init>", {"Ljava/lang/String;"}, "V"} ||
        body[4].registers !=
            std::vector<unsigned>{body[0].registers[0], body[1].registers[0]} ||
        body[5].opcode != "throw" || body[5].registers != body[0].registers ||
        !std::holds_alternative<std::monostate>(body[5].reference) ||
        !isAssignableReference(*exception, "Ljava/lang/Throwable;", classes,
                               budget))
      return false;
    for (size_t i = 0; i < body.size(); ++i) {
      const auto &op = body[i];
      budget.tick();
      if ((i && body[i - 1].pc >= op.pc) || op.pc >= target.code_end ||
          op.target || !op.keys.empty() || !op.targets.empty() ||
          !op.data.empty() || op.element_width ||
          (i != 1 && !std::holds_alternative<std::monostate>(op.literal)) ||
          std::any_of(op.registers.begin(), op.registers.end(),
                      [&](unsigned r) { return r >= target.registers; }))
        return false;
    }
    return true;
  }
  const auto *prefix = std::get_if<MethodRef>(&body[0].reference);
  const auto *exception = std::get_if<std::string>(&body[1].reference);
  const auto *constructor = std::get_if<MethodRef>(&body[2].reference);
  const bool static_prefix =
      body[0].opcode == "invoke-static" && body[0].registers.empty();
  const bool private_prefix =
      body[0].opcode == "invoke-direct" && prefix &&
      prefix->owner == target.reference.owner &&
      body[0].registers ==
          std::vector<unsigned>{target.registers - target.incomingWords()};
  if ((!static_prefix && !private_prefix) || !prefix ||
      !prefix->parameters.empty() ||
      (prefix->returns != "V" && prefix->returns != "Ljava/lang/Void;") ||
      body[1].opcode != "new-instance" || !exception ||
      body[1].registers.size() != 1 ||
      body[1].registers[0] >= target.registers - target.incomingWords() ||
      body[2].opcode != "invoke-direct" || !constructor ||
      *constructor != MethodRef{*exception, "<init>", {}, "V"} ||
      body[2].registers != body[1].registers || body[3].opcode != "throw" ||
      body[3].registers != body[1].registers ||
      !std::holds_alternative<std::monostate>(body[3].reference) ||
      !isAssignableReference(*exception, "Ljava/lang/Throwable;", classes,
                             budget))
    return false;
  const auto helper = classes.find(prefix->owner);
  const auto *helper_method =
      helper == classes.end() ? nullptr : findMethod(helper->second, *prefix);
  if (!helper_method ||
      (static_prefix ? !has(helper_method->access, "static")
                     : (has(helper_method->access, "static") ||
                        !has(helper_method->access, "private"))) ||
      has(helper_method->access, "abstract") ||
      helper_method->generic_signature)
    return false;
  for (const auto &op : body) {
    budget.tick();
    if (op.target || !op.keys.empty() || !op.targets.empty() ||
        !op.data.empty() || op.element_width ||
        !std::holds_alternative<std::monostate>(op.literal))
      return false;
  }
  return true;
}

bool nonReturningMessageIntegerBridge(const Class &cls, const Method &target,
                                      const Method &bridge,
                                      const ClassMap &classes, Budget &budget) {
  // Keep the Void result and all original conversion operations. Only the
  // conflicting source name changes; no virtual no-return assumption removes
  // the Number cast or unboxing call.
  if (cls.external || !has(cls.access, "abstract") ||
      !independentSourceScope(cls) || cls.superclass != "Ljava/lang/Object;" ||
      target.access != Access{"public"} || target.reference.owner != cls.name ||
      target.reference.returns != "Ljava/lang/Void;" ||
      target.reference.parameters.empty() ||
      target.reference.parameters.size() > 3 ||
      std::any_of(target.reference.parameters.begin(),
                  target.reference.parameters.end(),
                  [](const std::string &type) {
                    return type != "I" && !type.starts_with('L');
                  }) ||
      bridge.access != Access{"public", "bridge", "synthetic"} ||
      bridge.reference.owner != cls.name ||
      bridge.reference.name != target.reference.name ||
      bridge.reference.parameters != target.reference.parameters ||
      bridge.reference.returns != "I" || bridge.generic_signature ||
      !target.tries.empty() || !bridge.tries.empty() ||
      target.instructions.size() != 6 || bridge.instructions.size() != 6 ||
      target.registers < target.incomingWords() + 2 ||
      bridge.registers <= bridge.incomingWords() ||
      (target.declared_throws && !target.declared_throws->empty()) ||
      (bridge.declared_throws && !bridge.declared_throws->empty()))
    return false;
  const auto &body = target.instructions;
  const auto *exception = std::get_if<std::string>(&body[0].reference);
  const auto *message = std::get_if<FieldRef>(&body[1].reference);
  const auto *stringify = std::get_if<MethodRef>(&body[2].reference);
  const auto *init = std::get_if<MethodRef>(&body[4].reference);
  const auto self = target.registers - target.incomingWords();
  if (body[0].opcode != "new-instance" || !exception ||
      *exception != "Ljava/lang/IllegalStateException;" ||
      body[0].registers.size() != 1 || body[0].registers[0] >= self ||
      body[1].opcode != "iget-object" || !message ||
      message->owner != cls.name || message->type != "Ljava/lang/String;" ||
      body[1].registers.size() != 2 || body[1].registers[0] >= self ||
      body[1].registers[0] == body[0].registers[0] ||
      body[1].registers[1] != self || body[2].opcode != "invoke-virtual" ||
      !stringify ||
      *stringify !=
          MethodRef{
              "Ljava/lang/Object;", "toString", {}, "Ljava/lang/String;"} ||
      body[2].registers != std::vector<unsigned>{body[1].registers[0]} ||
      body[3].opcode != "move-result-object" ||
      body[3].registers != body[2].registers ||
      body[4].opcode != "invoke-direct" || !init ||
      *init != MethodRef{*exception, "<init>", {"Ljava/lang/String;"}, "V"} ||
      body[4].registers !=
          std::vector<unsigned>{body[0].registers[0], body[1].registers[0]} ||
      body[5].opcode != "throw" || body[5].registers != body[0].registers ||
      !isAssignableReference(*exception, "Ljava/lang/Throwable;", classes,
                             budget))
    return false;
  const auto field = std::find_if(
      cls.fields.begin(), cls.fields.end(),
      [&](const Field &candidate) { return candidate.reference == *message; });
  if (field == cls.fields.end() ||
      field->access != Access{"private", "final"} || field->generic_signature ||
      !std::holds_alternative<std::monostate>(field->value))
    return false;
  const auto &ops = bridge.instructions;
  const auto *call = std::get_if<MethodRef>(&ops[0].reference);
  const auto *cast = std::get_if<std::string>(&ops[2].reference);
  const auto *unbox = std::get_if<MethodRef>(&ops[3].reference);
  const auto bridge_self = bridge.registers - bridge.incomingWords();
  std::vector<unsigned> arguments;
  for (unsigned I = 0; I < bridge.incomingWords(); ++I)
    arguments.push_back(bridge_self + I);
  if ((ops[0].opcode != "invoke-virtual" &&
       ops[0].opcode != "invoke-virtual/range") ||
      !call || *call != target.reference || ops[0].registers != arguments ||
      ops[1].opcode != "move-result-object" || ops[1].registers.size() != 1 ||
      ops[1].registers[0] >= bridge_self || ops[2].opcode != "check-cast" ||
      !cast || *cast != "Ljava/lang/Number;" ||
      ops[2].registers != ops[1].registers ||
      ops[3].opcode != "invoke-virtual" || !unbox ||
      *unbox != MethodRef{"Ljava/lang/Number;", "intValue", {}, "I"} ||
      ops[3].registers != ops[1].registers || ops[4].opcode != "move-result" ||
      ops[4].registers.size() != 1 || ops[4].registers[0] >= bridge_self ||
      ops[5].opcode != "return" || ops[5].registers != ops[4].registers)
    return false;
  for (const auto *instructions : {&body, &ops}) {
    uint32_t previous = 0;
    for (size_t I = 0; I < instructions->size(); ++I) {
      budget.tick();
      const auto &op = (*instructions)[I];
      const auto &method = instructions == &body ? target : bridge;
      if ((I && op.pc <= previous) || op.pc >= method.code_end || op.target ||
          !op.keys.empty() || !op.targets.empty() || !op.data.empty() ||
          op.element_width ||
          !std::holds_alternative<std::monostate>(op.literal) ||
          std::any_of(op.registers.begin(), op.registers.end(),
                      [&](unsigned R) { return R >= method.registers; }))
        return false;
      previous = op.pc;
    }
  }
  for (const auto I : {3u, 5u})
    if (!std::holds_alternative<std::monostate>(body[I].reference))
      return false;
  for (const auto I : {1u, 4u, 5u})
    if (!std::holds_alternative<std::monostate>(ops[I].reference))
      return false;
  // A new source alias must not split an existing known override slot.
  for (const auto &[name, candidate] : classes) {
    budget.tick();
    if (name == cls.name)
      continue;
    auto slot = target.reference;
    slot.owner = name;
    if (findMethod(candidate, slot) &&
        isAssignableReference(name, cls.name, classes, budget))
      return false;
  }
  return true;
}

bool nonReturningVoidBridge(const Class &cls, const Method &target,
                            const Method &bridge, const ClassMap &classes,
                            Budget &budget) {
  if (cls.external || !has(cls.access, "final") ||
      target.reference.owner != cls.name ||
      bridge.access != Access{"public", "bridge", "synthetic"} ||
      bridge.reference.owner != cls.name ||
      bridge.reference.name != target.reference.name ||
      bridge.reference.parameters != target.reference.parameters ||
      bridge.reference.returns != "V" || bridge.generic_signature ||
      !bridge.tries.empty() || bridge.instructions.size() != 2 ||
      bridge.registers < bridge.incomingWords() ||
      (bridge.declared_throws && !bridge.declared_throws->empty()) ||
      !nonReturningVoidTargetBody(target, classes, budget))
    return false;
  const auto &ops = bridge.instructions;
  const auto *invoke = std::get_if<MethodRef>(&ops[0].reference);
  if ((ops[0].opcode != "invoke-virtual" &&
       ops[0].opcode != "invoke-virtual/range") ||
      !invoke || *invoke != target.reference ||
      ops[0].registers.size() != bridge.incomingWords() ||
      ops[1].opcode != "return-void" || !ops[1].registers.empty() ||
      !std::holds_alternative<std::monostate>(ops[1].reference))
    return false;
  const auto first = bridge.registers - bridge.incomingWords();
  for (size_t i = 0; i < ops[0].registers.size(); ++i)
    if (ops[0].registers[i] != first + i)
      return false;
  for (const auto &op : ops) {
    budget.tick();
    if (op.target || !op.keys.empty() || !op.targets.empty() ||
        !op.data.empty() || op.element_width ||
        !std::holds_alternative<std::monostate>(op.literal))
      return false;
  }
  return true;
}

bool nonReturningBooleanBridge(const Class &cls, const Method &target,
                               const Method &bridge, const ClassMap &classes,
                               Budget &budget) {
  if (!has(cls.access, "final") || !independentSourceScope(cls) ||
      target.access != Access{"public"} || target.reference.owner != cls.name ||
      target.reference.returns != "Ljava/lang/Void;" ||
      target.reference.parameters.size() != 1 ||
      !target.reference.parameters[0].starts_with('L') ||
      !target.tries.empty() || target.instructions.size() != 4 ||
      target.registers < target.incomingWords() ||
      bridge.access != Access{"public", "bridge", "synthetic"} ||
      bridge.reference.owner != cls.name ||
      bridge.reference.name != target.reference.name ||
      (bridge.reference.parameters !=
           std::vector<std::string>{"Ljava/lang/Object;"} &&
       bridge.reference.parameters != target.reference.parameters) ||
      bridge.reference.returns != "Z" || bridge.generic_signature ||
      !bridge.tries.empty() || bridge.registers < bridge.incomingWords() ||
      (target.declared_throws && !target.declared_throws->empty()) ||
      (bridge.declared_throws && !bridge.declared_throws->empty()))
    return false;
  if (!nonReturningVoidTargetBody(target, classes, budget))
    return false;
  const auto &body = target.instructions;
  const auto &ops = bridge.instructions;
  const bool checked_parameter =
      bridge.reference.parameters != target.reference.parameters;
  const bool copied = checked_parameter && ops.size() == 8 &&
                      (ops[0].opcode == "move-object" ||
                       ops[0].opcode == "move-object/from16" ||
                       ops[0].opcode == "move-object/16");
  if (ops.size() != (checked_parameter ? (copied ? 8u : 7u) : 6u))
    return false;
  const unsigned self = bridge.registers - bridge.incomingWords();
  unsigned argument = self + 1;
  if (copied) {
    if (ops[0].registers.size() != 2 || ops[0].registers[1] != argument ||
        ops[0].registers[0] >= bridge.registers ||
        ops[0].registers[0] == self ||
        !std::holds_alternative<std::monostate>(ops[0].reference))
      return false;
    argument = ops[0].registers[0];
  }
  const size_t first = copied ? 1 : 0;
  if (checked_parameter) {
    const auto *cast = std::get_if<std::string>(&ops[first].reference);
    if (ops[first].opcode != "check-cast" || !cast ||
        *cast != target.reference.parameters[0] ||
        ops[first].registers != std::vector<unsigned>{argument})
      return false;
  }
  const size_t call = checked_parameter ? first + 1 : 0;
  const auto *invoke = std::get_if<MethodRef>(&ops[call].reference);
  const auto *result_cast = std::get_if<std::string>(&ops[call + 2].reference);
  const auto *unbox = std::get_if<MethodRef>(&ops[call + 3].reference);
  if ((ops[call].opcode != "invoke-virtual" &&
       ops[call].opcode != "invoke-virtual/range") ||
      !invoke || *invoke != target.reference ||
      ops[call].registers != std::vector<unsigned>{self, argument} ||
      ops[call + 1].opcode != "move-result-object" ||
      ops[call + 1].registers.size() != 1 ||
      ops[call + 1].registers[0] >= bridge.registers ||
      ops[call + 2].opcode != "check-cast" || !result_cast ||
      *result_cast != "Ljava/lang/Boolean;" ||
      ops[call + 2].registers != ops[call + 1].registers ||
      (ops[call + 3].opcode != "invoke-virtual" &&
       ops[call + 3].opcode != "invoke-virtual/range") ||
      !unbox ||
      *unbox != MethodRef{"Ljava/lang/Boolean;", "booleanValue", {}, "Z"} ||
      ops[call + 3].registers != ops[call + 1].registers ||
      ops[call + 4].opcode != "move-result" ||
      ops[call + 4].registers.size() != 1 ||
      ops[call + 4].registers[0] >= bridge.registers ||
      ops[call + 5].opcode != "return" ||
      ops[call + 5].registers != ops[call + 4].registers)
    return false;
  for (const auto *instructions : {&body, &ops})
    for (const auto &op : *instructions) {
      budget.tick();
      if (op.target || !op.keys.empty() || !op.targets.empty() ||
          !op.data.empty() || op.element_width ||
          !std::holds_alternative<std::monostate>(op.literal))
        return false;
    }
  for (const size_t index : {call + 1, call + 4, call + 5})
    if (!std::holds_alternative<std::monostate>(ops[index].reference))
      return false;
  return true;
}

bool plainStaticReceiverImplementation(const Class &cls, const Method &method,
                                       const ClassMap &classes,
                                       const GenericSignaturePlans &signatures,
                                       Budget &budget) {
  const bool integer = method.reference.returns == "I";
  const bool scalar = integer || method.reference.returns == "Z";
  if (method.generic_signature || !method.tries.empty() ||
      (method.instructions.size() != 3 && method.instructions.size() != 5) ||
      method.registers < method.incomingWords() ||
      (!method.reference.returns.starts_with('L') && !scalar))
    return false;
  const auto &ops = method.instructions;
  const size_t at = ops.size() - 3;
  const auto *call = std::get_if<MethodRef>(&ops[at].reference);
  if (!call ||
      (ops[at].opcode != "invoke-static" &&
       ops[at].opcode != "invoke-static/range") ||
      ops[at + 1].opcode != (scalar ? "move-result" : "move-result-object") ||
      ops[at + 1].registers.size() != 1 ||
      ops[at + 1].registers[0] >= method.registers ||
      ops[at + 2].opcode != (scalar ? "return" : "return-object") ||
      ops[at + 2].registers != ops[at + 1].registers ||
      call->parameters.size() != method.reference.parameters.size() + 1 ||
      call->returns != method.reference.returns ||
      !std::equal(method.reference.parameters.begin(),
                  method.reference.parameters.end(),
                  call->parameters.begin() + 1) ||
      std::find(cls.interfaces.begin(), cls.interfaces.end(),
                call->parameters[0]) == cls.interfaces.end())
    return false;
  for (const auto &op : ops) {
    budget.tick();
    if (op.target || !op.keys.empty() || !op.targets.empty() ||
        !op.data.empty() || op.element_width ||
        !std::holds_alternative<std::monostate>(op.literal))
      return false;
  }
  if (!std::holds_alternative<std::monostate>(ops[at + 1].reference) ||
      !std::holds_alternative<std::monostate>(ops[at + 2].reference) ||
      ops[at].registers.size() != method.incomingWords())
    return false;
  const auto first = method.registers - method.incomingWords();
  unsigned forwarded_self = first;
  if (at) {
    const auto *cast = std::get_if<std::string>(&ops[1].reference);
    if ((ops[0].opcode != "move-object" &&
         ops[0].opcode != "move-object/from16" &&
         ops[0].opcode != "move-object/16") ||
        ops[0].registers.size() != 2 || ops[0].registers[0] >= first ||
        ops[0].registers[1] != first ||
        !std::holds_alternative<std::monostate>(ops[0].reference) ||
        ops[1].opcode != "check-cast" || !cast ||
        *cast != call->parameters[0] ||
        ops[1].registers != std::vector<unsigned>{ops[0].registers[0]})
      return false;
    forwarded_self = ops[0].registers[0];
  }
  for (size_t i = 0; i < ops[at].registers.size(); ++i) {
    budget.tick();
    if (ops[at].registers[i] != (i ? first + i : forwarded_self))
      return false;
  }
  const bool scoped =
      method.reference.returns == "Z" &&
      signatures.erased_predicate_receiver_invocations.contains(*call);
  const auto receiver = classes.find(call->parameters[0]);
  const auto helper = classes.find(call->owner);
  if (receiver == classes.end() || !has(receiver->second.access, "interface") ||
      helper == classes.end() ||
      (!scoped && !integer && helper->second.external) ||
      !independentSourceScope(helper->second))
    return false;
  if (integer || (!scoped && receiver->second.generic_signature)) {
    const auto root = signatures.classes.find(receiver->first);
    if (!independentSourceScope(receiver->second) ||
        (root == signatures.classes.end()
             ? receiver->second.generic_signature.has_value()
             : !root->second.type_parameters.empty() ||
                   !root->second.source_projections.empty() ||
                   std::any_of(root->second.types.begin(),
                               root->second.types.end(), [](const auto &type) {
                                 return type.kind ==
                                        GenericType::Kind::TypeVariable;
                               })))
      return false;
    std::vector<const Class *> pending{&receiver->second};
    std::set<std::string> visited;
    bool found = false;
    for (size_t i = 0; i < pending.size(); ++i) {
      budget.tick();
      const auto &owner = *pending[i];
      if (!visited.insert(owner.name).second)
        continue;
      if (visited.size() > 128 || !has(owner.access, "interface"))
        return false;
      auto required = method.reference;
      required.owner = owner.name;
      if (const auto *member = findMethod(owner, required)) {
        if (member->access != Access{"public", "abstract"} ||
            member->generic_signature ||
            signatures.methods.contains(required) ||
            (member->declared_throws && !member->declared_throws->empty()))
          return false;
        found = true;
        continue; // The nearer exact declaration owns this inheritance path.
      }
      for (const auto &parent : owner.interfaces) {
        const auto declaration = classes.find(parent);
        if (declaration == classes.end())
          return false;
        pending.push_back(&declaration->second);
      }
    }
    if (!found)
      return false;
  }
  const auto *target = findMethod(helper->second, *call);
  return target && has(target->access, "public") &&
         has(target->access, "static") && !has(target->access, "bridge") &&
         (scoped ||
          ((integer && helper->second.external ||
            !target->instructions.empty()) &&
           !has(target->access, "abstract") && !has(target->access, "native") &&
           !target->generic_signature &&
           !signatures.methods.contains(*call))) &&
         (!target->declared_throws || target->declared_throws->empty());
}

bool sourceContractImplementation(const Class &cls, const Method &method,
                                  const Method &required,
                                  const ClassMap &classes,
                                  const GenericSignaturePlans &signatures,
                                  Budget &budget) {
  // The same exact scalar forwarding receipt owns member compatibility and
  // retained source emission. Bridge flags alone add no class override.
  if (!cls.external && has(required.access, "public") &&
      !has(required.access, "static") && !has(required.access, "final") &&
      !required.generic_signature &&
      !signatures.methods.contains(required.reference) &&
      method.reference.name == required.reference.name &&
      method.reference.parameters == required.reference.parameters &&
      method.reference.returns == required.reference.returns &&
      plainPrimitiveForwardingBridge(cls, method, required, signatures, budget))
    return true;
  if (cls.external ||
      (method.access != Access{"public", "bridge"} &&
       method.access != Access{"public", "bridge", "synthetic"} &&
       !(method.access == Access{"public", "final", "bridge", "synthetic"} &&
         signatures.closed_reference_getter_bridges.contains(
             method.reference))) ||
      method.instructions.empty() ||
      (method.declared_throws && !method.declared_throws->empty()) ||
      (required.declared_throws && !required.declared_throws->empty()))
    return false;
  if (!required.generic_signature &&
      !signatures.methods.contains(required.reference) &&
      method.reference.name == required.reference.name &&
      method.reference.parameters == required.reference.parameters &&
      method.reference.returns == required.reference.returns &&
      plainInterfaceSuperForwarder(cls, method, classes, signatures, budget))
    return true;
  if (signatures.public_superclass_access_bridges.contains(method.reference) ||
      signatures.nonreturning_void_bridges.contains(method.reference) ||
      signatures.closed_reference_getter_bridges.contains(method.reference))
    return true;
  // An SDK interface's erased compiler slot may be implemented by the same
  // original class bridge already certified against its generic superclass.
  // Preserve that actual bridge body and complete typed target; SDK bridge
  // flags alone never infer its implementation or a generic substitution.
  if (!method.generic_signature && !required.generic_signature &&
      !signatures.methods.contains(required.reference) &&
      required.access == Access{"public", "bridge", "synthetic"} &&
      classes.at(required.reference.owner).external &&
      has(classes.at(required.reference.owner).access, "interface") &&
      method.reference.name == required.reference.name &&
      method.reference.parameters == required.reference.parameters &&
      method.reference.returns == required.reference.returns &&
      std::all_of(method.reference.parameters.begin(),
                  method.reference.parameters.end(), [](const auto &type) {
                    return type.size() == 1 && type != "V";
                  })) {
    auto proof = signatures.covariant_bridge_contracts.lower_bound(
        {method.reference, MethodRef{}});
    for (; proof != signatures.covariant_bridge_contracts.end() &&
           proof->first.first == method.reference;
         ++proof) {
      budget.tick();
      const auto &contract = proof->second.contract;
      const auto *target = findMethod(cls, proof->second.target);
      if (!target || proof->second.target.owner != cls.name ||
          proof->second.target.parameters != method.reference.parameters ||
          !contract.result || !contract.type_parameters.empty() ||
          !contract.source_projections.empty() ||
          !contract.throws_types.empty() ||
          contract.parameters.size() != method.reference.parameters.size() ||
          contract.types.at(*contract.result).erasure !=
              required.reference.returns)
        continue;
      bool exact = true;
      for (size_t i = 0; i < contract.parameters.size(); ++i)
        exact &= contract.types.at(contract.parameters[i]).erasure ==
                 required.reference.parameters[i];
      if (!exact)
        continue;
      verifyGenericBridge(method, *target, budget, nullptr, nullptr, &classes,
                          &signatures, &required);
      return true;
    }
  }
  const auto shape = signatures.methods.find(method.reference);
  if (method.generic_signature && shape != signatures.methods.end() &&
      shape->second.source_projections.empty() &&
      shape->second.throws_types.empty()) {
    if (!shape->second.type_parameters.empty())
      return true;
    const auto owner = signatures.classes.find(cls.name);
    return std::all_of(shape->second.types.begin(), shape->second.types.end(),
                       [&](const auto &type) {
                         budget.tick();
                         if (type.kind != GenericType::Kind::TypeVariable)
                           return true;
                         // Class-owned predicate inputs retain their primitive
                         // result; other open reference entries still need
                         // their separate dispatch proof.
                         return method.reference.returns == "Z" &&
                                independentSourceScope(cls) &&
                                owner != signatures.classes.end() &&
                                owner->second.source_projections.empty() &&
                                type.variable_owner == cls.name &&
                                std::any_of(
                                    owner->second.type_parameters.begin(),
                                    owner->second.type_parameters.end(),
                                    [&](const auto &formal) {
                                      return formal.name == type.variable_name;
                                    });
                       });
  }
  return !method.generic_signature &&
         !signatures.methods.contains(required.reference) &&
         !has(method.access, "synthetic") &&
         plainStaticReceiverImplementation(cls, method, classes, signatures,
                                           budget);
}

std::optional<GenericInheritancePlan::RetainedBridge>
guardedScalarParameterBridge(const Class &cls, const Method &bridge,
                             const ClassMap &classes,
                             const GenericSignaturePlans &signatures,
                             Budget &budget) {
  const auto &ops = bridge.instructions;
  if (cls.external || !has(cls.access, "final") ||
      (!independentSourceScope(cls) &&
       !signatures.closed_static_local_classes.contains(cls.name)) ||
      (bridge.access != Access{"public", "final", "bridge"} &&
       bridge.access != Access{"public", "final", "bridge", "synthetic"}) ||
      bridge.reference.parameters !=
          std::vector<std::string>{"Ljava/lang/Object;"} ||
      (bridge.reference.returns != "Z" && bridge.reference.returns != "I") ||
      bridge.generic_signature ||
      signatures.methods.contains(bridge.reference) || !bridge.tries.empty() ||
      !bridge.annotations.empty() || bridge.annotation_default ||
      bridge.deprecated || bridge.suppress_lint ||
      !bridge.parameter_annotations.empty() ||
      (bridge.declared_throws && !bridge.declared_throws->empty()) ||
      bridge.registers < 3 || bridge.registers > 65535 ||
      (ops.size() != 10 && ops.size() != 11))
    return {};
  const unsigned self = bridge.registers - 2, argument = self + 1;
  const auto *type = std::get_if<std::string>(&ops[0].reference);
  if (!type || !type->starts_with('L') || ops[0].opcode != "instance-of" ||
      ops[0].registers.size() != 2 || ops[0].registers[0] >= self ||
      ops[0].registers[1] != argument || ops[1].opcode != "if-nez" ||
      ops[1].registers != std::vector<unsigned>{ops[0].registers[0]} ||
      ops[1].target != ops[4].pc || ops[2].opcode != "const/4" ||
      ops[2].registers.size() != 1 || ops[2].registers[0] >= self ||
      std::get_if<int64_t>(&ops[2].literal) == nullptr ||
      std::get<int64_t>(ops[2].literal) !=
          (bridge.reference.returns == "Z" ? 0 : -1) ||
      ops[3].opcode != "return" || ops[3].registers != ops[2].registers)
    return {};
  unsigned checked = argument;
  size_t at = 4;
  if (ops.size() == 11) {
    const auto &move = ops[at++];
    if ((move.opcode != "move-object" && move.opcode != "move-object/from16" &&
         move.opcode != "move-object/16") ||
        move.registers.size() != 2 || move.registers[0] >= self ||
        move.registers[1] != argument)
      return {};
    checked = move.registers[0];
  }
  const auto &cast = ops[at], &unbox = ops[at + 1], &capture = ops[at + 2];
  const auto &invoke = ops[at + 3], &result = ops[at + 4], &ret = ops[at + 5];
  const auto *cast_type = std::get_if<std::string>(&cast.reference);
  const auto *read = std::get_if<MethodRef>(&unbox.reference);
  const auto *call = std::get_if<MethodRef>(&invoke.reference);
  const auto guarded = classes.find(*type);
  const auto cast_owner = cast_type ? classes.find(*cast_type) : classes.end();
  const bool same_type = cast_type && *cast_type == *type;
  const bool nominal_parent = guarded != classes.end() && cast_type &&
                              guarded->second.superclass == *cast_type;
  if (cast.opcode != "check-cast" || !cast_type ||
      (!same_type && !nominal_parent) || cast_owner == classes.end() ||
      !has(cast_owner->second.access, "public") ||
      has(cast_owner->second.access, "interface") ||
      cast.registers != std::vector<unsigned>{checked} || !read ||
      read->owner != *cast_type || !read->parameters.empty() ||
      read->returns.size() != 1 ||
      std::string_view("ZBSCIFJD").find(read->returns[0]) ==
          std::string_view::npos ||
      (unbox.opcode != "invoke-virtual" &&
       unbox.opcode != "invoke-virtual/range") ||
      unbox.registers != std::vector<unsigned>{checked} || !call ||
      call->owner != cls.name || call->name.starts_with('<') ||
      call->parameters != std::vector<std::string>{read->returns} ||
      call->returns != bridge.reference.returns ||
      (invoke.opcode != "invoke-virtual" &&
       invoke.opcode != "invoke-virtual/range"))
    return {};
  const unsigned words = width(read->returns);
  if (capture.opcode != (words == 2 ? "move-result-wide" : "move-result") ||
      capture.registers.size() != 1 || words > self ||
      capture.registers[0] > self - words || result.opcode != "move-result" ||
      result.registers.size() != 1 || result.registers[0] >= self ||
      ret.opcode != "return" || ret.registers != result.registers)
    return {};
  std::vector<unsigned> arguments{self};
  for (unsigned i = 0; i < words; ++i)
    arguments.push_back(capture.registers[0] + i);
  if (invoke.registers != arguments)
    return {};
  const auto box = classes.find(*type);
  const auto shape = signatures.classes.find(*type);
  if (box == classes.end() || !has(box->second.access, "public") ||
      has(box->second.access, "interface") ||
      (shape != signatures.classes.end() &&
       !shape->second.type_parameters.empty()))
    return {};
  auto exact = [&](const Class &owner, const MethodRef &ref) {
    const auto *method = findMethod(owner, ref);
    return method && has(method->access, "public") &&
           !has(method->access, "static") && !has(method->access, "abstract") &&
           !has(method->access, "native") && !has(method->access, "bridge") &&
           !method->generic_signature && !signatures.methods.contains(ref) &&
           (!method->declared_throws || method->declared_throws->empty());
  };
  const auto parent_shape = signatures.classes.find(*cast_type);
  if (parent_shape != signatures.classes.end() &&
      (!parent_shape->second.type_parameters.empty() ||
       (!same_type && !parent_shape->second.source_projections.empty())))
    return {};
  bool proven_unbox = exact(cast_owner->second, *read);
  if (!proven_unbox && nominal_parent) {
    const auto *declaration = findMethod(cast_owner->second, *read);
    auto implementation = *read;
    implementation.owner = box->second.name;
    proven_unbox = declaration && has(declaration->access, "public") &&
                   has(declaration->access, "abstract") &&
                   !has(declaration->access, "static") &&
                   !declaration->generic_signature &&
                   !signatures.methods.contains(*read) &&
                   (!declaration->declared_throws ||
                    declaration->declared_throws->empty()) &&
                   exact(box->second, implementation);
  }
  if (!proven_unbox || !exact(cls, *call))
    return {};
  for (size_t i = 0; i < ops.size(); ++i) {
    budget.tick();
    const auto &op = ops[i];
    if ((i && op.pc <= ops[i - 1].pc) || op.pc >= bridge.code_end ||
        (i != 1 && op.target) || !op.targets.empty() || !op.keys.empty() ||
        !op.data.empty() || op.element_width ||
        (i != 2 && !std::holds_alternative<std::monostate>(op.literal)) ||
        std::any_of(op.registers.begin(), op.registers.end(),
                    [&](unsigned reg) { return reg >= bridge.registers; }) ||
        (i != 0 && i != at && i != at + 1 && i != at + 3 &&
         !std::holds_alternative<std::monostate>(op.reference)))
      return {};
  }
  // Guard, false return, original unbox call and scalar dispatch stay in the
  // unchanged Object runtime body. Both callees retain their full ordinary
  // recovery obligations; no field/purity or replacement ABI is inferred.
  GenericInheritancePlan::RetainedBridge proof{*call};
  proof.guarded_scalar_parameter = true;
  proof.unbox_method = *read;
  return proof;
}

std::optional<GenericInheritancePlan::RetainedBridge>
plainNamedScalarGetter(const Class &cls, const Method &bridge,
                       const GenericSignaturePlans &signatures,
                       Budget &budget) {
  if (cls.external || !has(cls.access, "final") ||
      !independentSourceScope(cls) ||
      bridge.access != Access{"public", "bridge", "synthetic"} ||
      !bridge.reference.parameters.empty() ||
      bridge.reference.returns.size() != 1 ||
      std::string_view("ZBSCIFJD").find(bridge.reference.returns[0]) ==
          std::string_view::npos ||
      bridge.generic_signature ||
      signatures.methods.contains(bridge.reference) || !bridge.tries.empty() ||
      !bridge.annotations.empty() || bridge.annotation_default ||
      bridge.deprecated || bridge.suppress_lint ||
      !bridge.parameter_annotations.empty() ||
      (bridge.declared_throws && !bridge.declared_throws->empty()) ||
      bridge.instructions.size() != 3 || !bridge.registers ||
      bridge.registers > 65535)
    return {};
  const auto &ops = bridge.instructions;
  const auto *call = std::get_if<MethodRef>(&ops[0].reference);
  const bool wide =
      bridge.reference.returns == "J" || bridge.reference.returns == "D";
  const unsigned words = wide ? 2 : 1;
  if (!call || call->owner != cls.name || call->name == bridge.reference.name ||
      call->name.starts_with('<') || !call->parameters.empty() ||
      call->returns != bridge.reference.returns ||
      (ops[0].opcode != "invoke-virtual" &&
       ops[0].opcode != "invoke-virtual/range") ||
      ops[0].registers != std::vector<unsigned>{bridge.registers - 1} ||
      ops[1].opcode != (wide ? "move-result-wide" : "move-result") ||
      ops[1].registers.size() != 1 || words > bridge.registers ||
      ops[1].registers[0] > bridge.registers - words ||
      ops[2].opcode != (wide ? "return-wide" : "return") ||
      ops[2].registers != ops[1].registers)
    return {};
  const auto *target = findMethod(cls, *call);
  if (!target || !has(target->access, "public") ||
      has(target->access, "static") || has(target->access, "abstract") ||
      has(target->access, "bridge") || has(target->access, "native") ||
      target->instructions.empty() || target->generic_signature ||
      signatures.methods.contains(*call) ||
      (target->declared_throws && !target->declared_throws->empty()))
    return {};
  for (size_t i = 0; i < ops.size(); ++i) {
    budget.tick();
    const auto &op = ops[i];
    if ((i && op.pc <= ops[i - 1].pc) || op.pc >= bridge.code_end ||
        op.target || !op.targets.empty() || !op.keys.empty() ||
        !op.data.empty() || op.element_width ||
        !std::holds_alternative<std::monostate>(op.literal) ||
        std::any_of(op.registers.begin(), op.registers.end(),
                    [&](unsigned reg) { return reg >= bridge.registers; }) ||
        (i && !std::holds_alternative<std::monostate>(op.reference)))
      return {};
  }
  // The target's full body is recovered independently. Only the unchanged
  // receiver, scalar result width and original virtual delegation are proved
  // here; no field layout, purity or getter-name semantics are assumed.
  GenericInheritancePlan::RetainedBridge proof{*call};
  proof.named_scalar_getter = true;
  return proof;
}

std::optional<GenericInheritancePlan::RetainedBridge> plainBoxedPrimitiveGetter(
    const Class &cls, const Method &bridge, const ClassMap &classes,
    const GenericSignaturePlans &signatures, Budget &budget) {
  if (!has(cls.access, "final") || !independentSourceScope(cls) ||
      bridge.access != Access{"public", "bridge", "synthetic"} ||
      !bridge.reference.parameters.empty() ||
      bridge.reference.returns != "Ljava/lang/Object;" ||
      bridge.generic_signature || !bridge.tries.empty() ||
      !bridge.annotations.empty() || bridge.annotation_default ||
      bridge.deprecated || bridge.suppress_lint ||
      !bridge.parameter_annotations.empty() ||
      (bridge.declared_throws && !bridge.declared_throws->empty()) ||
      bridge.instructions.size() != 5 || !bridge.registers)
    return {};
  const auto &ops = bridge.instructions;
  const auto *call = std::get_if<MethodRef>(&ops[0].reference);
  const auto *box = std::get_if<MethodRef>(&ops[2].reference);
  if (!call || !box || call->owner != cls.name ||
      call->name == bridge.reference.name || !call->parameters.empty() ||
      (ops[0].opcode != "invoke-virtual" &&
       ops[0].opcode != "invoke-virtual/range") ||
      ops[0].registers != std::vector<unsigned>{bridge.registers - 1} ||
      call->returns.size() != 1 ||
      std::string_view("ZBCSIFJD").find(call->returns[0]) ==
          std::string_view::npos ||
      box->parameters != std::vector<std::string>{call->returns} ||
      box->owner != box->returns || !box->returns.starts_with('L') ||
      (ops[2].opcode != "invoke-static" &&
       ops[2].opcode != "invoke-static/range"))
    return {};
  const bool wide = call->returns == "J" || call->returns == "D";
  const unsigned words = wide ? 2 : 1;
  if (ops[1].opcode != (wide ? "move-result-wide" : "move-result") ||
      ops[1].registers.size() != 1 ||
      (words > bridge.registers ||
       ops[1].registers[0] > bridge.registers - words) ||
      ops[3].opcode != "move-result-object" || ops[3].registers.size() != 1 ||
      ops[3].registers[0] >= bridge.registers ||
      ops[4].opcode != "return-object" || ops[4].registers != ops[3].registers)
    return {};
  std::vector<unsigned> argument;
  for (unsigned i = 0; i < words; ++i)
    argument.push_back(ops[1].registers[0] + i);
  if (ops[2].registers != argument)
    return {};
  const auto *target = findMethod(cls, *call);
  const auto boxer = classes.find(box->owner);
  const auto *boxing =
      boxer == classes.end() ? nullptr : findMethod(boxer->second, *box);
  if (!target || !has(target->access, "public") ||
      has(target->access, "static") || has(target->access, "bridge") ||
      target->generic_signature || !target->tries.empty() ||
      (target->declared_throws && !target->declared_throws->empty()) ||
      target->instructions.size() != 2 || !target->registers || !boxing ||
      !has(boxer->second.access, "public") ||
      has(boxer->second.access, "interface") ||
      !has(boxing->access, "public") || !has(boxing->access, "static") ||
      boxing->generic_signature || has(boxing->access, "bridge") ||
      (boxing->declared_throws && !boxing->declared_throws->empty()) ||
      signatures.methods.contains(*call) || signatures.methods.contains(*box))
    return {};
  const auto &read = target->instructions[0];
  const auto &ret = target->instructions[1];
  const auto *field = std::get_if<FieldRef>(&read.reference);
  const auto opcode = call->returns == "Z"   ? "iget-boolean"
                      : call->returns == "B" ? "iget-byte"
                      : call->returns == "C" ? "iget-char"
                      : call->returns == "S" ? "iget-short"
                      : wide                 ? "iget-wide"
                                             : "iget";
  if (!field || field->owner != cls.name || field->type != call->returns ||
      read.opcode != opcode || read.registers.size() != 2 ||
      read.registers[1] != target->registers - 1 ||
      (words > target->registers ||
       read.registers[0] > target->registers - words) ||
      ret.opcode != (wide ? "return-wide" : "return") ||
      ret.registers != std::vector<unsigned>{read.registers[0]})
    return {};
  const auto declaration = std::find_if(
      cls.fields.begin(), cls.fields.end(),
      [&](const Field &value) { return value.reference == *field; });
  if (declaration == cls.fields.end() ||
      declaration->access != Access{"private", "final"} ||
      declaration->generic_signature)
    return {};
  for (const auto *method : {&bridge, target})
    for (const auto &op : method->instructions) {
      budget.tick();
      if (op.target || !op.keys.empty() || !op.targets.empty() ||
          !op.data.empty() || op.element_width ||
          !std::holds_alternative<std::monostate>(op.literal))
        return {};
    }
  for (const auto i : {1u, 3u, 4u})
    if (!std::holds_alternative<std::monostate>(ops[i].reference))
      return {};
  if (!std::holds_alternative<std::monostate>(ret.reference))
    return {};
  GenericInheritancePlan::RetainedBridge proof{target->reference};
  proof.boxed_primitive_getter = true;
  proof.box_method = *box;
  return proof;
}

SourceMethodNames planSelfReturnNames(const ClassMap &classes,
                                      const GenericSignaturePlans &signatures,
                                      Budget &budget) {
  std::map<std::string, std::set<std::string>> ancestry;
  std::set<std::string> active;
  std::function<const std::set<std::string> &(const std::string &, unsigned)>
      ancestors = [&](const std::string &name,
                      unsigned depth) -> const std::set<std::string> & {
    budget.tick();
    if (active.contains(name) || depth > 64)
      unsupported(name, "source return-slot ancestry is cyclic or too deep");
    if (const auto found = ancestry.find(name); found != ancestry.end())
      return found->second;
    active.insert(name);
    auto &result = ancestry[name];
    result.insert(name);
    if (const auto found = classes.find(name); found != classes.end()) {
      std::vector<std::string> parents = found->second.interfaces;
      if (found->second.superclass &&
          *found->second.superclass != "Ljava/lang/Object;")
        parents.push_back(*found->second.superclass);
      for (const auto &parent : parents)
        for (const auto &ancestor : ancestors(parent, depth + 1)) {
          budget.tick();
          if (result.size() >= 256)
            unsupported(name,
                        "source return-slot ancestor count exceeds limit");
          result.insert(ancestor);
        }
    }
    active.erase(name);
    return result;
  };
  std::set<std::string> occupied;
  for (const auto &[name, cls] : classes)
    for (const auto &method : cls.methods) {
      budget.tick();
      occupied.insert(method.reference.name);
    }
  SourceMethodNames result;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    const auto owner = signatures.classes.find(name);
    if (cls.external || !has(cls.access, "interface") ||
        cls.superclass != "Ljava/lang/Object;" ||
        !independentSourceScope(cls) || owner == signatures.classes.end() ||
        owner->second.type_parameters.empty() ||
        !owner->second.source_projections.empty())
      continue;
    for (const auto &method : cls.methods) {
      budget.tick();
      const auto signature = signatures.methods.find(method.reference);
      if (!has(method.access, "public") || !has(method.access, "abstract") ||
          has(method.access, "static") || has(method.access, "bridge") ||
          signature == signatures.methods.end() ||
          !signature->second.type_parameters.empty() ||
          !signature->second.throws_types.empty() ||
          !signature->second.source_projections.empty() ||
          (method.declared_throws && !method.declared_throws->empty()))
        continue;
      const auto &returned =
          signature->second.types.at(*signature->second.result);
      if (returned.kind != GenericType::Kind::Class ||
          returned.erasure != name || returned.segments.size() != 1 ||
          returned.segments[0].arguments.size() !=
              owner->second.type_parameters.size())
        continue;
      bool self = true;
      for (size_t i = 0; i < returned.segments[0].arguments.size(); ++i) {
        const auto &argument = returned.segments[0].arguments[i];
        if (!argument.type ||
            argument.variance != GenericTypeArgument::Variance::Invariant) {
          self = false;
          break;
        }
        const auto &type = signature->second.types.at(*argument.type);
        self &= type.kind == GenericType::Kind::TypeVariable &&
                type.variable_owner == name &&
                type.variable_name == owner->second.type_parameters[i].name;
      }
      if (!self)
        continue;
      std::set<MethodRef> conflicts;
      for (const auto &ancestor : ancestors(name, 0)) {
        budget.tick();
        const auto found = classes.find(ancestor);
        if (found == classes.end() || !found->second.external ||
            !has(found->second.access, "interface"))
          continue;
        for (const auto &required : found->second.methods) {
          budget.tick();
          if (required.reference.name != method.reference.name ||
              required.reference.parameters != method.reference.parameters ||
              !has(required.access, "public") ||
              has(required.access, "static") || has(required.access, "bridge"))
            continue;
          bool incompatible = !required.reference.returns.starts_with('L') &&
                              !required.reference.returns.starts_with('[');
          const auto inherited = signatures.methods.find(required.reference);
          const auto formals = signatures.classes.find(ancestor);
          if (inherited != signatures.methods.end() &&
              formals != signatures.classes.end() &&
              inherited->second.type_parameters.empty()) {
            const auto &type =
                inherited->second.types.at(*inherited->second.result);
            if (type.kind == GenericType::Kind::TypeVariable &&
                type.variable_owner == ancestor)
              for (const auto &formal : formals->second.type_parameters)
                incompatible |=
                    formal.name == type.variable_name &&
                    formal.erasure == "Ljava/lang/Object;" &&
                    formal.interface_bounds.empty() && formal.class_bound &&
                    formals->second.types.at(*formal.class_bound).erasure ==
                        "Ljava/lang/Object;";
          }
          if (incompatible)
            conflicts.insert(required.reference);
        }
      }
      if (conflicts.empty())
        continue;
      std::string spelling;
      if (const auto prior = result.find(method.reference);
          prior != result.end())
        spelling = prior->second.name;
      else {
        spelling = "__neverdReturnSlot";
        while (occupied.contains(spelling)) {
          budget.tick();
          spelling += '_';
        }
        occupied.insert(spelling);
      }
      for (const auto &[child_name, child] : classes) {
        budget.tick();
        if (child.external || !ancestors(child_name, 0).contains(name))
          continue;
        for (const auto &candidate : child.methods) {
          budget.tick();
          if (candidate.reference.name != method.reference.name ||
              candidate.reference.parameters != method.reference.parameters ||
              !has(candidate.access, "public") ||
              has(candidate.access, "static") ||
              !ancestors(candidate.reference.returns, 0).contains(name))
            continue;
          auto [entry, inserted] = result.emplace(
              candidate.reference, GenericInheritancePlan::SourceMethodName{
                                       spelling, method.reference, conflicts});
          if (!inserted && entry->second.name != spelling)
            unsupported(child_name, "source return slot has competing names");
          entry->second.conflicting_contracts.insert(conflicts.begin(),
                                                     conflicts.end());
        }
      }
    }
  }
  return result;
}

// An original return-only bridge can cast a Void result to an unrelated
// nominal slot. Java requires separate source names for those descriptors.
// Retain both bodies, including the cast, without inferring that the target
// never returns. Finality closes dispatch and the complete ancestor walk keeps
// the typed alias from splitting any inherited Void entry.
void planCheckedReferenceReturnNames(const ClassMap &classes,
                                     const GenericSignaturePlans &signatures,
                                     Budget &budget,
                                     GenericInheritancePlan &result) {
  std::set<std::string> occupied;
  std::set<MethodRef> metadata_references;
  for (const auto &[name, cls] : classes) {
    if (cls.enclosing_method)
      metadata_references.insert(*cls.enclosing_method);
    for (const auto &method : cls.methods) {
      budget.tick();
      occupied.insert(method.reference.name);
    }
  }
  for (const auto &[ref, alias] : result.source_method_names)
    occupied.insert(alias.name);
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external ||
        has(cls.access, "interface") || cls.generic_signature)
      continue;
    for (const auto &bridge : cls.methods) {
      budget.tick();
      const auto &ref = bridge.reference;
      const bool unboxes = ref.returns == "Z" && ref.parameters.empty();
      if ((!has(cls.access, "final") && !unboxes) ||
          bridge.access != Access{"public", "bridge", "synthetic"} ||
          bridge.generic_signature || bridge.annotation_default ||
          (!unboxes && (!ref.returns.starts_with('L') ||
                        ref.returns == "Ljava/lang/Void;" ||
                        !classes.contains(ref.returns))) ||
          !bridge.tries.empty() || bridge.instructions.size() != (unboxes ? 6u : 4u) ||
          bridge.registers <= bridge.incomingWords() ||
          (bridge.declared_throws && !bridge.declared_throws->empty()) ||
          result.bridges.contains(ref) ||
          result.retained_plain_bridges.contains(ref))
        continue;
      const auto &ops = bridge.instructions;
      const auto *call = std::get_if<MethodRef>(&ops[0].reference);
      const auto *cast = std::get_if<std::string>(&ops[2].reference);
      if ((ops[0].opcode != "invoke-virtual" &&
           ops[0].opcode != "invoke-virtual/range") ||
          !call || call->owner != name || call->name != ref.name ||
          call->parameters != ref.parameters ||
          call->returns != "Ljava/lang/Void;" ||
          ops[1].opcode != "move-result-object" ||
          ops[1].registers.size() != 1 ||
          ops[1].registers[0] >= bridge.registers - bridge.incomingWords() ||
          ops[2].opcode != "check-cast" || !cast ||
          ops[2].registers != ops[1].registers ||
          !std::holds_alternative<std::monostate>(ops[1].reference))
        continue;
      if (unboxes) {
        const auto conversion = primitiveBridgeConversion(ref.returns, classes);
        const auto *unbox = std::get_if<MethodRef>(&ops[3].reference);
        if (!conversion || *cast != conversion->checked ||
            !unbox || *unbox != conversion->unbox ||
            (ops[3].opcode != "invoke-virtual" &&
             ops[3].opcode != "invoke-virtual/range") ||
            ops[3].registers != ops[2].registers ||
            ops[4].opcode != "move-result" || ops[4].registers.size() != 1 ||
            ops[4].registers.front() >= bridge.registers ||
            ops[5].opcode != "return" ||
            ops[5].registers != ops[4].registers ||
            !std::holds_alternative<std::monostate>(ops[4].reference) ||
            !std::holds_alternative<std::monostate>(ops[5].reference))
          continue;
      } else if (*cast != ref.returns || ops[3].opcode != "return-object" ||
                 ops[3].registers != ops[1].registers ||
                 !std::holds_alternative<std::monostate>(ops[3].reference))
        continue;
      const auto *target = findMethod(cls, *call);
      if (!target ||
          target->access != (unboxes ? Access{"public", "final"} : Access{"public"}) ||
          target->instructions.empty() || target->annotation_default ||
          (target->declared_throws && !target->declared_throws->empty()) ||
          result.source_method_names.contains(*call) ||
          metadata_references.contains(*call))
        continue;
      const auto shape = signatures.methods.find(*call);
      if (target->generic_signature &&
          (shape == signatures.methods.end() || !shape->second.result ||
           !shape->second.type_parameters.empty() ||
           !shape->second.throws_types.empty() ||
           !shape->second.source_projections.empty() ||
           std::any_of(shape->second.types.begin(), shape->second.types.end(),
                       [](const auto &type) {
                         return type.kind == GenericType::Kind::TypeVariable;
                       })))
        continue;
      bool exact = ops[0].registers.size() == bridge.incomingWords();
      const auto first = bridge.registers - bridge.incomingWords();
      for (size_t i = 0; i < ops[0].registers.size(); ++i)
        exact &= ops[0].registers[i] == first + i;
      for (size_t i = 0; i < ops.size(); ++i) {
        budget.tick();
        const auto &op = ops[i];
        exact &= (!i || op.pc > ops[i - 1].pc) && op.pc < bridge.code_end &&
                 !op.target && op.keys.empty() && op.targets.empty() &&
                 op.data.empty() && !op.element_width &&
                 std::holds_alternative<std::monostate>(op.literal);
      }
      std::map<std::string, unsigned> states;
      std::function<bool(const std::string &, unsigned)> known_ancestry =
          [&](const std::string &owner, unsigned depth) {
            budget.tick();
            if (owner == "Ljava/lang/Object;")
              return !objectSourceMethodNames().contains(call->name);
            const auto found = classes.find(owner);
            if (found == classes.end() || depth > 64 || states.size() > 128)
              return false;
            auto &state = states[owner];
            if (state)
              return state == 2;
            state = 1;
            for (const auto &member : found->second.methods) {
              budget.tick();
              if (member.reference.name == call->name &&
                  member.reference.parameters == call->parameters &&
                  member.reference.returns == call->returns &&
                  !has(member.access, "private") &&
                  !inaccessiblePackageMethod(found->second, member, cls))
                return false;
            }
            if (!found->second.superclass ||
                !known_ancestry(*found->second.superclass, depth + 1))
              return false;
            for (const auto &edge : found->second.interfaces)
              if (!known_ancestry(edge, depth + 1))
                return false;
            state = 2;
            return true;
          };
      exact &= cls.superclass && known_ancestry(*cls.superclass, 0);
      for (const auto &edge : cls.interfaces)
        exact &= known_ancestry(edge, 0);
      if (!exact)
        continue;
      std::string spelling = "__neverdCheckedReferenceTarget";
      while (!occupied.insert(spelling).second) {
        budget.tick();
        spelling += '_';
      }
      GenericInheritancePlan::SourceMethodName alias{spelling, *call, {}};
      alias.checked_reference_bridge = ref;
      result.source_method_names.emplace(*call, std::move(alias));
      GenericInheritancePlan::RetainedBridge retained{*call};
      retained.checked_reference_return = true;
      result.retained_plain_bridges.emplace(ref, std::move(retained));
    }
  }
}

void planPlainClassReturnNames(const ClassMap &classes,
                               const GenericSignaturePlans &signatures,
                               Budget &budget, GenericInheritancePlan &result) {
  // A plain input interface's fixed reference getter and an inherited SDK
  // scalar getter occupy distinct runtime descriptors. Their source family
  // needs one disclosed name, while the unchanged SDK slot keeps its name.
  std::map<std::string, std::set<std::string>> children;
  std::set<std::string> occupied;
  std::map<MethodRef, const Method *> roots;
  std::set<std::string> scalar_names;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.superclass)
      children[*cls.superclass].insert(name);
    for (const auto &edge : cls.interfaces)
      children[edge].insert(name);
    for (const auto &method : cls.methods) {
      budget.tick();
      occupied.insert(method.reference.name);
      if (cls.external && !has(cls.access, "interface") &&
          method.reference.parameters.empty() &&
          method.reference.returns.size() == 1 &&
          method.reference.returns != "V" &&
          (has(method.access, "public") || has(method.access, "protected")) &&
          !has(method.access, "static") && !has(method.access, "abstract") &&
          !has(method.access, "bridge") && !method.generic_signature &&
          (!method.declared_throws || method.declared_throws->empty()))
        scalar_names.insert(method.reference.name);
      if (!cls.external && has(cls.access, "interface") &&
          cls.superclass == "Ljava/lang/Object;" && !cls.generic_signature &&
          independentSourceScope(cls) && has(method.access, "public") &&
          has(method.access, "abstract") && !has(method.access, "static") &&
          !has(method.access, "bridge") &&
          method.reference.parameters.empty() &&
          method.reference.returns.starts_with('L') &&
          !method.generic_signature &&
          (!method.declared_throws || method.declared_throws->empty()))
        roots.emplace(method.reference, &method);
    }
  }
  for (const auto &[reference, alias] : result.source_method_names)
    occupied.insert(alias.name);
  std::map<std::string, std::set<std::string>> families;
  for (const auto &[root, declaration] : roots) {
    budget.tick();
    if (!scalar_names.contains(root.name))
      continue; // An unrelated reference interface needs no scalar projection.
    auto family = families.find(root.owner);
    if (family == families.end()) {
      std::set<std::string> members, active;
      bool bounded = true;
      std::function<void(const std::string &, unsigned)> visit =
          [&](const std::string &name, unsigned depth) {
            budget.tick();
            if (!bounded)
              return;
            if (active.contains(name) || depth > 64 || members.size() > 256) {
              bounded = false;
              return;
            }
            if (members.contains(name))
              return;
            active.insert(name);
            members.insert(name);
            for (const auto &child : children[name])
              visit(child, depth + 1);
            active.erase(name);
          };
      visit(root.owner, 0);
      if (!bounded)
        members.clear(); // No receipt; an actual collision still fails the
                         // authoritative class/member compatibility proof.
      family = families.emplace(root.owner, std::move(members)).first;
    }
    std::set<MethodRef> conflicts, members;
    bool complete = true;
    for (const auto &name : family->second) {
      budget.tick();
      const auto &cls = classes.at(name);
      if (has(cls.access, "interface"))
        continue;
      std::set<std::string> seen;
      for (const Class *current = &cls;
           current->superclass &&
           *current->superclass != "Ljava/lang/Object;";) {
        budget.tick();
        const auto parent = classes.find(*current->superclass);
        if (parent == classes.end() || !seen.insert(parent->first).second ||
            seen.size() > 64 || has(parent->second.access, "interface") ||
            !independentSourceScope(parent->second) ||
            parent->second.generic_signature) {
          complete = false;
          break;
        }
        if (parent->second.external)
          for (const auto &method : parent->second.methods) {
            budget.tick();
            const auto &ref = method.reference;
            if (ref.name == root.name && ref.parameters.empty() &&
                ref.returns.size() == 1 && ref.returns != "V" &&
                (has(method.access, "public") ||
                 has(method.access, "protected")) &&
                !has(method.access, "static") &&
                !has(method.access, "abstract") &&
                !has(method.access, "bridge") && !method.generic_signature &&
                (!method.declared_throws || method.declared_throws->empty()))
              conflicts.insert(ref);
          }
        current = &parent->second;
      }
    }
    if (conflicts.empty())
      continue;
    const auto returned = classes.find(root.returns);
    if (!complete || returned == classes.end() ||
        !independentSourceScope(returned->second))
      unsupported(
          root.identity(),
          "plain return-slot ancestry requires known independent scopes");
    for (const auto &name : family->second) {
      budget.tick();
      const auto &cls = classes.at(name);
      if (cls.external || !independentSourceScope(cls) || cls.generic_signature)
        unsupported(
            root.identity(),
            "plain return-slot family requires unchanged plain input owners");
      for (const auto &method : cls.methods) {
        budget.tick();
        if (method.reference.name != root.name ||
            method.reference.parameters != root.parameters ||
            method.reference.returns != root.returns)
          continue;
        if (!has(method.access, "public") || has(method.access, "static") ||
            has(method.access, "bridge") || method.generic_signature ||
            !sameCompleteMethodContract(method, *declaration, signatures,
                                        root.owner, budget, classes))
          unsupported(
              method.reference.identity(),
              "plain return-slot member has an incompatible full contract");
        members.insert(method.reference);
      }
    }
    std::string spelling;
    MethodRef origin = root;
    for (const auto &member : members)
      if (const auto prior = result.source_method_names.find(member);
          prior != result.source_method_names.end()) {
        if (!spelling.empty() && spelling != prior->second.name)
          unsupported(root.identity(),
                      "plain return-slot family has competing names");
        spelling = prior->second.name;
        origin = prior->second.declaration;
      }
    if (spelling.empty()) {
      spelling = "__neverdReturnSlot";
      while (occupied.contains(spelling)) {
        budget.tick();
        spelling += '_';
      }
      occupied.insert(spelling);
    }
    for (const auto &member : members) {
      auto [entry, inserted] = result.source_method_names.emplace(
          member, GenericInheritancePlan::SourceMethodName{spelling, origin,
                                                           conflicts});
      entry->second.conflicting_contracts.insert(conflicts.begin(),
                                                 conflicts.end());
    }
  }
}

void planPrivateProducerReturnNames(const ClassMap &classes,
                                    const GenericSignaturePlans &signatures,
                                    Budget &budget,
                                    GenericInheritancePlan &result) {
  std::map<std::string, std::set<std::string>> cache;
  std::set<std::string> active;
  std::function<const std::set<std::string> &(const std::string &, unsigned)>
      ancestors = [&](const std::string &name,
                      unsigned depth) -> const std::set<std::string> & {
    budget.tick();
    if (active.contains(name) || depth > 64)
      unsupported(name,
                  "private producer return ancestry is cyclic or too deep");
    if (const auto found = cache.find(name); found != cache.end())
      return found->second;
    active.insert(name);
    auto &known = cache[name];
    known.insert(name);
    const auto declaration = classes.find(name);
    if (declaration == classes.end() && name != "Ljava/lang/Object;")
      unsupported(name,
                  "private producer return ancestry needs known declarations");
    if (declaration != classes.end()) {
      auto pending = declaration->second.interfaces;
      if (declaration->second.superclass)
        pending.push_back(*declaration->second.superclass);
      for (const auto &parent : pending)
        for (const auto &ancestor : ancestors(parent, depth + 1)) {
          budget.tick();
          if (known.size() >= 256)
            unsupported(name, "private producer return ancestry exceeds limit");
          known.insert(ancestor);
        }
    }
    active.erase(name);
    return known;
  };
  std::set<std::string> occupied;
  for (const auto &[name, cls] : classes)
    for (const auto &method : cls.methods)
      occupied.insert(method.reference.name);
  for (const auto &[reference, alias] : result.source_method_names)
    occupied.insert(alias.name);
  for (const auto &[pair, producer] :
       signatures.private_producer_return_slots) {
    budget.tick();
    const auto &[broad, narrow] = pair;
    std::set<std::string> providers{narrow.owner};
    const auto matching = [&](const MethodRef &ref) {
      return ref.name == narrow.name && ref.parameters == narrow.parameters &&
             ref.returns.starts_with('L') &&
             ancestors(ref.returns, 0).contains(narrow.returns);
    };
    for (const auto &owner : ancestors(narrow.owner, 0)) {
      const auto known = classes.find(owner);
      if (known == classes.end() || !has(known->second.access, "interface"))
        continue;
      for (const auto &method : known->second.methods) {
        if (!matching(method.reference) || has(method.access, "static") ||
            has(method.access, "private"))
          continue;
        if (known->second.external)
          unsupported(narrow.identity(),
                      "private producer return alias cannot "
                      "rename an external interface contract");
        providers.insert(owner);
      }
    }
    std::set<MethodRef> family;
    for (const auto &[name, cls] : classes) {
      if (cls.external)
        continue;
      for (const auto &method : cls.methods) {
        budget.tick();
        if (!matching(method.reference) || has(method.access, "static") ||
            has(method.access, "private"))
          continue;
        const auto &parents = ancestors(name, 0);
        if (std::any_of(providers.begin(), providers.end(),
                        [&](const auto &provider) {
                          return parents.contains(provider);
                        }))
          family.insert(method.reference);
      }
    }
    if (!family.contains(narrow))
      unsupported(narrow.identity(),
                  "private producer return family lost its owner");
    std::string spelling;
    for (const auto &member : family)
      if (const auto existing = result.source_method_names.find(member);
          existing != result.source_method_names.end()) {
        if (!spelling.empty() && spelling != existing->second.name)
          unsupported(member.identity(), "private producer return family has "
                                         "conflicting source names");
        spelling = existing->second.name;
      }
    if (spelling.empty()) {
      spelling = "__neverdReturnSlot";
      while (occupied.contains(spelling)) {
        budget.tick();
        spelling += '_';
      }
      occupied.insert(spelling);
    }
    for (const auto &member : family) {
      auto [entry, inserted] = result.source_method_names.emplace(
          member, GenericInheritancePlan::SourceMethodName{
                      spelling, narrow, {broad}, producer});
      if (!inserted && entry->second.name != spelling)
        unsupported(member.identity(),
                    "private producer return alias conflicts");
      entry->second.conflicting_contracts.insert(broad);
      entry->second.private_producer = producer;
    }
  }
}

// A local writer's formal may belong to its own class or the direct captured
// enclosing instance. The signature validator has already resolved lexical
// identities; an exact constructor-capture receipt connects the latter scope
// to the anonymous body without inventing a local type parameter.
bool localWriterFormalScope(const Class &cls, const GenericType &value,
                            const ClassMap &classes,
                            const GenericSignaturePlans &signatures,
                            Budget &budget) {
  if (value.kind != GenericType::Kind::TypeVariable || !value.variable_owner)
    return false;
  if (*value.variable_owner == cls.name)
    return independentSourceScope(cls);
  if (!isAnonymousLocal(cls) ||
      cls.enclosing_method->owner != *value.variable_owner ||
      signatures.inheritance_contracts.contains(cls.name))
    return false;
  const auto enclosing = classes.find(*value.variable_owner);
  const auto header = signatures.classes.find(*value.variable_owner);
  const auto local = signatures.classes.find(cls.name);
  if (enclosing == classes.end() || enclosing->second.external ||
      !independentSourceScope(enclosing->second) ||
      header == signatures.classes.end() ||
      !header->second.source_projections.empty() ||
      signatures.inheritance_contracts.contains(enclosing->first) ||
      local == signatures.classes.end() ||
      !local->second.type_parameters.empty())
    return false;
  if (std::none_of(header->second.type_parameters.begin(),
                   header->second.type_parameters.end(), [&](const auto &p) {
                     return p.name == value.variable_name &&
                            p.erasure == "Ljava/lang/Object;";
                   }))
    return false;
  const auto method = std::find_if(
      enclosing->second.methods.begin(), enclosing->second.methods.end(),
      [&](const auto &m) { return m.reference == *cls.enclosing_method; });
  const auto contract = signatures.methods.find(*cls.enclosing_method);
  if (method == enclosing->second.methods.end() ||
      has(method->access, "static") || has(method->access, "abstract") ||
      has(method->access, "native") || method->instructions.empty() ||
      (contract == signatures.methods.end()
           ? bool(method->generic_signature)
           : !contract->second.type_parameters.empty() ||
                 !contract->second.source_projections.empty()))
    return false;
  std::optional<FieldRef> captured;
  unsigned constructors = 0;
  for (const auto &m : cls.methods) {
    budget.tick();
    if (m.reference.name != "<init>")
      continue;
    ++constructors;
    const auto receipt = signatures.constructor_captures.find(m.reference);
    if (receipt == signatures.constructor_captures.end() ||
        !receipt->second.contains(0) ||
        receipt->second.at(0).type != enclosing->first ||
        (captured && *captured != receipt->second.at(0)))
      return false;
    captured = receipt->second.at(0);
  }
  if (!constructors || !captured)
    return false;
  const auto field =
      std::find_if(cls.fields.begin(), cls.fields.end(),
                   [&](const auto &f) { return f.reference == *captured; });
  return field != cls.fields.end() && has(field->access, "final") &&
         has(field->access, "synthetic") && !has(field->access, "static");
}

// A source name must not remove an inherited void implementation. Property
// writers and discarded-result adapters share this complete hierarchy check.
bool knownNonoverridingVoidSlot(const Class &cls, const MethodRef &reference,
                                const ClassMap &classes,
                                const GenericSignaturePlans &signatures,
                                Budget &budget) {
  std::map<std::string, unsigned> states;
  std::function<bool(const std::string &, unsigned)> known =
      [&](const std::string &name, unsigned depth) {
        budget.tick();
        const auto declaration = classes.find(name);
        if (declaration == classes.end() || depth > 64 || states.size() > 128 ||
            !independentSourceScope(declaration->second))
          return false;
        auto &state = states[name];
        if (state)
          return state == 2;
        state = 1;
        const auto shape = signatures.classes.find(name);
        if (shape == signatures.classes.end()
                ? bool(declaration->second.generic_signature)
                : !shape->second.source_projections.empty())
          return false;
        for (const auto &member : declaration->second.methods) {
          budget.tick();
          if (!has(member.access, "private") &&
              member.reference.name == reference.name &&
              member.reference.parameters == reference.parameters &&
              member.reference.returns == "V")
            return false;
        }
        if (declaration->second.superclass &&
            !known(*declaration->second.superclass, depth + 1))
          return false;
        for (const auto &parent : declaration->second.interfaces)
          if (!known(parent, depth + 1))
            return false;
        state = 2;
        return true;
      };
  if (cls.superclass && !known(*cls.superclass, 0))
    return false;
  for (const auto &parent : cls.interfaces)
    if (!known(parent, 0))
      return false;
  return true;
}

// Only complete original parameter equality and a pure own-field write permit
// a source-only name; inherited provider ownership remains unchanged.
std::optional<MethodRef> localVoidSetterConflict(
    const Class &cls, const Method &writer, const ClassMap &classes,
    const GenericSignaturePlans &signatures, Budget &budget) {
  const auto full = signatures.methods.find(writer.reference);
  if ((writer.access != Access{"public"} &&
       writer.access != Access{"public", "final"}) ||
      writer.reference.name.starts_with('<') ||
      writer.reference.parameters !=
          std::vector<std::string>{"Ljava/lang/Object;"} ||
      writer.reference.returns != "V" || writer.registers < 2 ||
      writer.registers > 65535 || !writer.tries.empty() ||
      writer.instructions.size() != 2 ||
      (writer.declared_throws && !writer.declared_throws->empty()) ||
      full == signatures.methods.end() || !full->second.result ||
      !full->second.type_parameters.empty() ||
      !full->second.source_projections.empty() ||
      !full->second.throws_types.empty() || full->second.parameters.size() != 1)
    return {};
  const auto &value = full->second.types.at(full->second.parameters[0]);
  if (!localWriterFormalScope(cls, value, classes, signatures, budget))
    return {};
  const auto same_formal = [&](const GenericType &type) {
    return value.kind == GenericType::Kind::TypeVariable &&
           type.kind == GenericType::Kind::TypeVariable &&
           type.variable_owner == value.variable_owner &&
           value.variable_name == type.variable_name &&
           value.erasure == "Ljava/lang/Object;" &&
           type.erasure == value.erasure;
  };
  const auto &put = writer.instructions[0], &ret = writer.instructions[1];
  const auto *field = std::get_if<FieldRef>(&put.reference);
  const auto self = writer.registers - writer.incomingWords();
  if (!same_formal(value) || put.opcode != "iput-object" || !field ||
      field->owner != cls.name || field->type != "Ljava/lang/Object;" ||
      put.registers != std::vector<unsigned>{self + 1, self} ||
      ret.opcode != "return-void" || !ret.registers.empty())
    return {};
  for (size_t i = 0; i < writer.instructions.size(); ++i) {
    const auto &op = writer.instructions[i];
    budget.tick();
    if ((i && op.pc <= writer.instructions[i - 1].pc) ||
        op.pc >= writer.code_end || op.target || !op.targets.empty() ||
        !op.keys.empty() || !op.data.empty() || op.element_width ||
        !std::holds_alternative<std::monostate>(op.literal) ||
        (i && !std::holds_alternative<std::monostate>(op.reference)))
      return {};
  }
  const auto declaration = std::find_if(
      cls.fields.begin(), cls.fields.end(),
      [&](const Field &candidate) { return candidate.reference == *field; });
  const auto stored = signatures.fields.find(*field);
  if (declaration == cls.fields.end() || has(declaration->access, "static") ||
      has(declaration->access, "final") || stored == signatures.fields.end() ||
      !stored->second.field_type ||
      !stored->second.source_projections.empty() ||
      !same_formal(stored->second.types.at(*stored->second.field_type)))
    return {};
  const Method *sibling = nullptr;
  unsigned group = 0;
  for (const auto &candidate : cls.methods) {
    budget.tick();
    if (candidate.reference.name != writer.reference.name ||
        candidate.reference.parameters != writer.reference.parameters)
      continue;
    ++group;
    if (&candidate == &writer)
      continue;
    const auto shape = signatures.methods.find(candidate.reference);
    if (sibling ||
        (candidate.access != Access{"public"} &&
         candidate.access != Access{"public", "final"}) ||
        candidate.reference.returns != "Ljava/lang/Object;" ||
        candidate.registers < candidate.incomingWords() ||
        candidate.instructions.empty() ||
        (candidate.declared_throws && !candidate.declared_throws->empty()) ||
        shape == signatures.methods.end() || !shape->second.result ||
        !shape->second.type_parameters.empty() ||
        !shape->second.source_projections.empty() ||
        !shape->second.throws_types.empty() ||
        shape->second.parameters.size() != 1 ||
        !same_formal(shape->second.types.at(shape->second.parameters[0])) ||
        !same_formal(shape->second.types.at(*shape->second.result)))
      return {};
    const auto incoming = candidate.registers - candidate.incomingWords();
    unsigned calls = 0;
    for (const auto &op : candidate.instructions) {
      budget.tick();
      const auto *ref = std::get_if<MethodRef>(&op.reference);
      if (!ref || *ref != writer.reference)
        continue;
      if ((op.opcode != "invoke-virtual" &&
           op.opcode != "invoke-virtual/range") ||
          op.registers != std::vector<unsigned>{incoming, incoming + 1})
        return {};
      ++calls;
    }
    if (calls != 1)
      return {};
    sibling = &candidate;
  }
  if (group != 2 || !sibling)
    return {};
  return knownNonoverridingVoidSlot(cls, writer.reference, classes, signatures,
                                    budget)
             ? std::optional(sibling->reference)
             : std::nullopt;
}

std::optional<MethodRef> discardedReferenceResultConflict(
    const Class &cls, const Method &adapter, const ClassMap &classes,
    const GenericSignaturePlans &signatures, Budget &budget) {
  const auto &ref = adapter.reference;
  if (adapter.access != Access{"public", "final", "synthetic"} ||
      ref.owner != cls.name || ref.name.starts_with('<') || ref.returns != "V" ||
      ref.parameters.size() != 1 || ref.parameters[0].size() != 1 ||
      std::string_view("ZBCSIJFD").find(ref.parameters[0][0]) ==
          std::string_view::npos ||
      adapter.generic_signature || adapter.annotation_default ||
      (adapter.declared_throws && !adapter.declared_throws->empty()) ||
      !adapter.tries.empty() || adapter.instructions.size() != 2 ||
      adapter.registers > 1024 || adapter.registers < adapter.incomingWords())
    return {};
  const auto &call = adapter.instructions[0], &ret = adapter.instructions[1];
  const auto *target = std::get_if<MethodRef>(&call.reference);
  if ((call.opcode != "invoke-virtual" &&
       call.opcode != "invoke-virtual/range") ||
      !target || target->owner != cls.name || target->name != ref.name ||
      target->parameters != ref.parameters || target->returns != cls.name ||
      ret.opcode != "return-void" || !ret.registers.empty() ||
      !std::holds_alternative<std::monostate>(ret.reference))
    return {};
  std::vector<unsigned> words;
  for (unsigned reg = adapter.registers - adapter.incomingWords();
       reg < adapter.registers; ++reg)
    words.push_back(reg);
  if (call.registers != words)
    return {};
  for (size_t i = 0; i < adapter.instructions.size(); ++i) {
    const auto &op = adapter.instructions[i];
    budget.tick();
    if ((i && op.pc <= adapter.instructions[i - 1].pc) ||
        op.pc >= adapter.code_end || op.target || !op.targets.empty() ||
        !op.keys.empty() || !op.data.empty() || op.element_width ||
        !std::holds_alternative<std::monostate>(op.literal))
      return {};
  }
  const auto *body = findMethod(cls, *target);
  if (!body || (body->access != Access{"public"} &&
                body->access != Access{"public", "final"}) ||
      body->generic_signature || body->instructions.empty() ||
      (body->declared_throws && !body->declared_throws->empty()) ||
      !knownNonoverridingVoidSlot(cls, ref, classes, signatures, budget))
    return {};
  // The exact original call remains in Body. Its returned reference can be
  // null or another object; naming proves no result value or extra conversion.
  return *target;
}

// The common adapter receipt owns the original superclass call and cast.
// Its distinct local nominal slot may need a name beside a method-formal
// getter; the generic interface and erased map entries retain their names.
void planLocalReferenceAdapterReturnNames(
    const ClassMap &classes, const GenericSignaturePlans &signatures,
    Budget &budget, GenericInheritancePlan &result) {
  std::set<std::string> occupied;
  for (const auto &[name, cls] : classes)
    for (const auto &method : cls.methods) {
      budget.tick();
      occupied.insert(method.reference.name);
    }
  for (const auto &[ref, alias] : result.source_method_names)
    occupied.insert(alias.name);
  for (const auto &[reference, super] :
       signatures.superclass_reference_adapters) {
    budget.tick();
    const auto &cls = classes.at(reference.owner);
    const auto owner = signatures.classes.find(cls.name);
    const auto original = signatures.inheritance_contracts.find(cls.name);
    const auto full = signatures.methods.find(reference);
    if (cls.external || !has(cls.access, "final") ||
        has(cls.access, "interface") || !independentSourceScope(cls) ||
        owner == signatures.classes.end() ||
        !owner->second.type_parameters.empty() ||
        full == signatures.methods.end() ||
        !full->second.type_parameters.empty() ||
        !full->second.throws_types.empty() ||
        !full->second.source_projections.empty() ||
        full->second.parameters.size() != 1 || !full->second.result ||
        reference.parameters.size() != 1 ||
        result.source_method_names.contains(reference))
      continue;
    // A separately certified raw source parent retains its complete original
    // header. Keep this local naming proof on that original zero-formal scope;
    // the source projection supplies no missing bound or dispatch contract.
    const auto &header = original == signatures.inheritance_contracts.end()
                             ? owner->second
                             : original->second;
    if (!header.type_parameters.empty() || !header.source_projections.empty())
      continue;
    const auto &parameter = full->second.types.at(full->second.parameters[0]);
    if (parameter.kind != GenericType::Kind::Class ||
        parameter.segments.size() != 1 ||
        parameter.segments[0].arguments.size() != 1)
      continue;
    const auto &closed = parameter.segments[0].arguments[0];
    if (!closed.type ||
        closed.variance != GenericTypeArgument::Variance::Invariant)
      continue;
    const auto &object = full->second.types.at(*closed.type);
    if (object.kind != GenericType::Kind::Class ||
        object.erasure != "Ljava/lang/Object;" || object.segments.size() != 1 ||
        !object.segments[0].arguments.empty())
      continue;
    const Method *conflict = nullptr;
    bool exact = true;
    for (const auto &method : cls.methods) {
      budget.tick();
      if (method.reference == reference ||
          method.reference.name != reference.name ||
          method.reference.parameters != reference.parameters)
        continue;
      const auto shape = signatures.methods.find(method.reference);
      if (conflict ||
          (method.access != Access{"public"} &&
           method.access != Access{"public", "final"}) ||
          method.reference.returns != "Ljava/lang/Object;" ||
          method.instructions.empty() ||
          (method.declared_throws && !method.declared_throws->empty()) ||
          shape == signatures.methods.end() ||
          shape->second.type_parameters.size() != 1 ||
          !shape->second.throws_types.empty() ||
          !shape->second.source_projections.empty() ||
          shape->second.parameters.size() != 1 || !shape->second.result) {
        exact = false;
        break;
      }
      const auto &sig = shape->second;
      const auto &formal = sig.type_parameters[0];
      const auto &input = sig.types.at(sig.parameters[0]);
      const auto &returned = sig.types.at(*sig.result);
      if (!formal.class_bound || !formal.interface_bounds.empty() ||
          sig.types.at(*formal.class_bound).erasure != "Ljava/lang/Object;" ||
          input.kind != GenericType::Kind::Class ||
          input.erasure != parameter.erasure || input.segments.size() != 1 ||
          input.segments[0].arguments.size() != 1 ||
          returned.kind != GenericType::Kind::TypeVariable ||
          returned.variable_owner != method.reference.identity() ||
          returned.variable_name != formal.name ||
          returned.erasure != "Ljava/lang/Object;") {
        exact = false;
        break;
      }
      const auto &argument = input.segments[0].arguments[0];
      if (!argument.type ||
          argument.variance != GenericTypeArgument::Variance::Invariant) {
        exact = false;
        break;
      }
      const auto &variable = sig.types.at(*argument.type);
      if (variable.kind != GenericType::Kind::TypeVariable ||
          variable.variable_owner != returned.variable_owner ||
          variable.variable_name != returned.variable_name ||
          variable.erasure != returned.erasure) {
        exact = false;
        break;
      }
      conflict = &method;
    }
    if (!exact || !conflict)
      continue;
    // Finality closes descendants; no known ancestor may own the exact local
    // nominal return slot. Source naming never supplies a missing contract.
    std::map<std::string, unsigned> states;
    std::function<bool(const std::string &, unsigned)> closed_ancestry =
        [&](const std::string &name, unsigned depth) {
          budget.tick();
          if (name == "Ljava/lang/Object;")
            return true;
          const auto known = classes.find(name);
          if (known == classes.end() || depth > 64 || states.size() > 128 ||
              !independentSourceScope(known->second))
            return false;
          auto &state = states[name];
          if (state)
            return state == 2;
          state = 1;
          for (const auto &method : known->second.methods) {
            budget.tick();
            if (!has(method.access, "private") &&
                !has(method.access, "static") &&
                method.reference.name == reference.name &&
                method.reference.parameters == reference.parameters &&
                method.reference.returns == reference.returns)
              return false;
          }
          if (known->second.superclass &&
              !closed_ancestry(*known->second.superclass, depth + 1))
            return false;
          for (const auto &parent : known->second.interfaces)
            if (!closed_ancestry(parent, depth + 1))
              return false;
          state = 2;
          return true;
        };
    if (cls.superclass && !closed_ancestry(*cls.superclass, 0))
      continue;
    for (const auto &parent : cls.interfaces)
      exact &= closed_ancestry(parent, 0);
    if (!exact)
      continue;
    std::string spelling = "__neverdReturnSlot";
    while (occupied.contains(spelling)) {
      budget.tick();
      spelling += '_';
    }
    occupied.insert(spelling);
    GenericInheritancePlan::SourceMethodName alias{
        spelling, reference, {conflict->reference}};
    alias.local_reference_return_slot = true;
    result.source_method_names.emplace(reference, std::move(alias));
  }
}

// A static call resolves a complete runtime descriptor, including its return
// slot. In a closed independent owner, source aliases can keep each original
// body without borrowing an inherited contract or coalescing the two entries.
void planStaticReturnNames(const ClassMap &classes,
                           const GenericSignaturePlans &signatures,
                           Budget &budget, GenericInheritancePlan &result) {
  std::set<std::string> occupied;
  std::set<MethodRef> metadata_references;
  size_t package_alias_index = 0;
  for (const auto &[name, cls] : classes) {
    if (cls.enclosing_method)
      metadata_references.insert(*cls.enclosing_method);
    for (const auto &method : cls.methods) {
      budget.tick();
      occupied.insert(method.reference.name);
    }
  }
  for (const auto &[ref, alias] : result.source_method_names)
    occupied.insert(alias.name);
  for (const auto &[name, cls] : classes) {
    budget.tick();
    const auto owner = signatures.classes.find(name);
    const bool closed_root = has(cls.access, "public") &&
                             has(cls.access, "final") &&
                             cls.superclass == "Ljava/lang/Object;";
    const bool package_owner = !has(cls.access, "public") && !cls.enclosing &&
                               !cls.enclosing_method && cls.superclass &&
                               classes.contains(*cls.superclass);
    if (cls.external || (!closed_root && !package_owner) ||
        has(cls.access, "interface") || has(cls.access, "enum") ||
        !independentSourceScope(cls) || cls.generic_signature ||
        !cls.interfaces.empty() ||
        (owner != signatures.classes.end() &&
         (!owner->second.type_parameters.empty() ||
          !owner->second.source_projections.empty())))
      continue;
    using Key = std::pair<std::string, std::vector<std::string>>;
    std::map<Key, std::map<MethodRef, const Method *>> groups;
    for (const auto &method : cls.methods) {
      budget.tick();
      const auto &ref = method.reference;
      if (!ref.name.starts_with('<'))
        groups[{ref.name, ref.parameters}].emplace(ref, &method);
    }
    for (const auto &[key, methods] : groups) {
      budget.tick();
      if (methods.size() < 2)
        continue;
      bool complete = true;
      auto unchanged = methods.begin();
      for (auto i = methods.begin(); i != methods.end(); ++i) {
        budget.tick();
        const auto &[ref, method] = *i;
        const auto shape = signatures.methods.find(ref);
        auto source_access = method->access;
        source_access.erase("synthetic");
        source_access.erase("final");
        const bool eligible_access =
            source_access == Access{"public", "static"} ||
            (package_owner && source_access == Access{"private", "static"});
        complete &= eligible_access &&
                    (package_owner || ref.returns.starts_with('L')) &&
                    !method->instructions.empty() &&
                    !method->annotation_default &&
                    !metadata_references.contains(ref) &&
                    !result.source_method_names.contains(ref) &&
                    (shape == signatures.methods.end()
                         ? !method->generic_signature
                         : shape->second.source_projections.empty());
        // Keep the ordinary erased Object entry when present. Otherwise the
        // sorted full descriptor chooses the unchanged spelling
        // deterministically.
        if (ref.returns == "Ljava/lang/Object;")
          unchanged = i;
      }
      if (!complete)
        continue;
      for (const auto &[ref, method] : methods) {
        budget.tick();
        if (has(method->access, "synthetic"))
          result.static_return_synthetic_methods.insert(ref);
        // The closed root keeps one original spelling. A package class may
        // expose its public statics through an inheriting qualifier. Give the
        // whole group fresh names so no ancestor or child hides a different
        // original return descriptor. Body resolves each symbolic reference
        // before applying the selected declaration's name.
        if (closed_root && ref == unchanged->first)
          continue;
        std::string spelling = "__neverdStaticReturnSlot";
        if (package_owner) {
          do {
            budget.tick();
            spelling = "__neverdStaticReturnSlot" +
                       std::to_string(package_alias_index++);
          } while (!occupied.insert(spelling).second);
        } else {
          while (!occupied.insert(spelling).second) {
            budget.tick();
            spelling += '_';
          }
        }
        std::set<MethodRef> conflicts;
        for (const auto &[other, body] : methods) {
          budget.tick();
          if (other != ref)
            conflicts.insert(other);
        }
        GenericInheritancePlan::SourceMethodName alias{spelling, ref,
                                                       std::move(conflicts)};
        // This proof changes spelling only. The exact original descriptor,
        // generic Signature and body still own every independent semantic
        // check.
        alias.identifier_only = true;
        alias.static_return_slot = true;
        result.source_method_names.emplace(ref, std::move(alias));
      }
    }
  }
}

void planFinalVoidReturnNames(const ClassMap &classes,
                              const GenericSignaturePlans &signatures,
                              Budget &budget, GenericInheritancePlan &result) {
  std::set<std::string> occupied;
  for (const auto &[name, cls] : classes)
    for (const auto &method : cls.methods) {
      budget.tick();
      occupied.insert(method.reference.name);
    }
  for (const auto &[reference, alias] : result.source_method_names)
    occupied.insert(alias.name);
  std::set<MethodRef> metadata_references;
  for (const auto &[name, cls] : classes)
    if (cls.enclosing_method)
      metadata_references.insert(*cls.enclosing_method);
  std::map<MethodRef, MethodRef> discarded_results;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    const auto owner = signatures.classes.find(name);
    if (cls.external || !has(cls.access, "public") ||
        !has(cls.access, "final") || has(cls.access, "abstract") ||
        has(cls.access, "interface") || has(cls.access, "enum") ||
        !independentSourceScope(cls) || cls.generic_signature ||
        (owner != signatures.classes.end() &&
         (!owner->second.type_parameters.empty() ||
          !owner->second.source_projections.empty())))
      continue;
    for (const auto &method : cls.methods) {
      budget.tick();
      if (result.source_method_names.contains(method.reference) ||
          metadata_references.contains(method.reference))
        continue;
      if (auto target = discardedReferenceResultConflict(
              cls, method, classes, signatures, budget);
          target && !result.source_method_names.contains(*target))
        discarded_results.emplace(method.reference, *target);
    }
  }
  // Sort complete runtime identities before reserving names. Input method or
  // class order cannot choose a different spelling for these local adapters.
  for (const auto &[reference, target] : discarded_results) {
    std::string spelling = "__neverdReturnSlot";
    while (!occupied.insert(spelling).second) {
      budget.tick();
      spelling += '_';
    }
    GenericInheritancePlan::SourceMethodName alias{spelling, reference, {target}};
    alias.identifier_only = true;
    alias.discarded_reference_result = true;
    result.source_method_names.emplace(reference, std::move(alias));
  }
  for (const auto &[name, cls] : classes) {
    budget.tick();
    const auto owner = signatures.classes.find(name);
    if (cls.external || !has(cls.access, "final") ||
        has(cls.access, "interface") ||
        (!independentSourceScope(cls) && !isAnonymousLocal(cls)) ||
        owner == signatures.classes.end() ||
        !owner->second.source_projections.empty())
      continue;
    const bool inherited_scope = onlyEmptyMarkerParents(cls, classes, budget);
    for (const auto &method : cls.methods) {
      budget.tick();
      if (!result.source_method_names.contains(method.reference))
        if (auto local = localVoidSetterConflict(cls, method, classes,
                                                 signatures, budget)) {
          std::string spelling = "__neverdReturnSlot";
          while (occupied.contains(spelling)) {
            budget.tick();
            spelling += '_';
          }
          occupied.insert(spelling);
          GenericInheritancePlan::SourceMethodName alias{
              spelling, method.reference, {*local}};
          alias.local_return_slot = true;
          result.source_method_names.emplace(method.reference,
                                             std::move(alias));
          continue;
        }
      if (!inherited_scope || !independentSourceScope(cls))
        continue;
      const auto full = signatures.methods.find(method.reference);
      if (method.access != Access{"public", "final"} ||
          method.reference.returns != "V" || method.instructions.empty() ||
          full == signatures.methods.end() || !full->second.result ||
          !full->second.type_parameters.empty() ||
          !full->second.throws_types.empty() ||
          !full->second.source_projections.empty() ||
          (method.declared_throws && !method.declared_throws->empty()) ||
          result.source_method_names.contains(method.reference))
        continue;
      const Class *current = &cls;
      std::set<std::string> seen;
      std::map<std::string, SubstitutedType> bindings;
      std::optional<MethodRef> conflict;
      bool exact = true;
      while (current->superclass &&
             *current->superclass != "Ljava/lang/Object;") {
        budget.tick();
        const auto parent = classes.find(*current->superclass);
        if (!seen.insert(current->name).second || seen.size() > 64 ||
            parent == classes.end() ||
            has(parent->second.access, "interface") ||
            !independentSourceScope(parent->second)) {
          exact = false;
          break;
        }
        const auto composed = composeClassEdge(
            *current, parent->second, bindings, signatures, name, budget);
        if (!composed) {
          exact = false;
          break;
        }
        bindings = *composed;
        bool nearest = false;
        for (const auto &required : parent->second.methods) {
          budget.tick();
          if (required.reference.name != method.reference.name ||
              required.reference.parameters != method.reference.parameters)
            continue;
          nearest = true;
          const auto contract = signatures.methods.find(required.reference);
          if (conflict || !has(required.access, "public") ||
              has(required.access, "static") ||
              has(required.access, "abstract") ||
              has(required.access, "bridge") ||
              has(required.access, "native") ||
              required.reference.returns.size() != 1 ||
              required.reference.returns == "V" ||
              (required.declared_throws &&
               !required.declared_throws->empty()) ||
              contract == signatures.methods.end() ||
              !contract->second.result ||
              !contract->second.type_parameters.empty() ||
              !contract->second.throws_types.empty() ||
              !contract->second.source_projections.empty() ||
              contract->second.parameters.size() !=
                  full->second.parameters.size()) {
            exact = false;
            continue;
          }
          for (size_t i = 0; i < full->second.parameters.size(); ++i)
            exact &=
                substitute(full->second, full->second.parameters[i], {}, name,
                           budget, 0, true)
                    .signature == substitute(contract->second,
                                             contract->second.parameters[i],
                                             bindings, name, budget, 0, true)
                                      .signature;
          conflict = required.reference;
        }
        if (nearest)
          break;
        current = &parent->second;
      }
      if (!exact || !conflict)
        continue;
      std::string spelling = "__neverdReturnSlot";
      while (occupied.contains(spelling)) {
        budget.tick();
        spelling += '_';
      }
      occupied.insert(spelling);
      result.source_method_names.emplace(
          method.reference, GenericInheritancePlan::SourceMethodName{
                                spelling, method.reference, {*conflict}});
    }
  }
}

void planFinalGetterReturnNames(const ClassMap &classes,
                                const GenericSignaturePlans &signatures,
                                Budget &budget,
                                GenericInheritancePlan &result) {
  std::set<std::string> occupied;
  for (const auto &[name, cls] : classes)
    for (const auto &method : cls.methods) {
      budget.tick();
      occupied.insert(method.reference.name);
    }
  for (const auto &[ref, projection] : result.source_method_names)
    occupied.insert(projection.name);
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external || has(cls.access, "interface"))
      continue;
    for (const auto &bridge : cls.methods) {
      budget.tick();
      const auto own = signatures.methods.find(bridge.reference);
      if (own == signatures.methods.end() || !own->second.result)
        continue;
      const auto contract = substitute(own->second, *own->second.result, {},
                                       name, budget, 0, true);
      const auto target = genericReferenceGetterBridge(cls, bridge, contract,
                                                       signatures, budget);
      if (!target)
        continue;
      result.retained_plain_bridges.emplace(
          bridge.reference,
          GenericInheritancePlan::RetainedBridge{*target, false, false, true});
      std::set<MethodRef> conflicts;
      std::set<std::string> seen{name};
      const Class *current = &cls;
      while (current->superclass &&
             *current->superclass != "Ljava/lang/Object;") {
        budget.tick();
        const auto parent = classes.find(*current->superclass);
        if (parent == classes.end() || !seen.insert(parent->first).second ||
            seen.size() > 64)
          break;
        for (const auto &member : parent->second.methods) {
          budget.tick();
          if (member.reference.name != bridge.reference.name ||
              member.reference.parameters != bridge.reference.parameters ||
              member.reference.returns == bridge.reference.returns ||
              !has(member.access, "final") || parent->second.external)
            continue;
          const auto inherited = signatures.methods.find(member.reference);
          if (inherited == signatures.methods.end() ||
              !inherited->second.result)
            continue;
          const auto parent_result =
              substitute(inherited->second, *inherited->second.result, {},
                         parent->first, budget, 0, true);
          const auto parent_target = genericReferenceGetterBridge(
              parent->second, member, parent_result, signatures, budget);
          if (parent_target && parent_target->name == target->name &&
              signatures.covariant_reference_returns.contains(
                  {*target, *parent_target}))
            conflicts.insert(member.reference);
        }
        current = &parent->second;
      }
      if (conflicts.empty())
        continue;
      std::string spelling = "__neverdReturnSlot";
      while (occupied.contains(spelling)) {
        budget.tick();
        spelling += '_';
      }
      occupied.insert(spelling);
      if (!result.source_method_names
               .emplace(bridge.reference,
                        GenericInheritancePlan::SourceMethodName{
                            spelling, bridge.reference, conflicts})
               .second)
        unsupported(name,
                    "final getter return slot has competing source names");
    }
  }
}

void planBoxedBooleanReturnNames(const ClassMap &classes,
                                 const GenericSignaturePlans &signatures,
                                 Budget &budget,
                                 GenericInheritancePlan &result) {
  std::set<std::string> occupied;
  for (const auto &[name, cls] : classes)
    for (const auto &method : cls.methods) {
      budget.tick();
      occupied.insert(method.reference.name);
    }
  for (const auto &[reference, alias] : result.source_method_names)
    occupied.insert(alias.name);
  for (const auto &[reference, proof] : signatures.value_carrier_bridges) {
    budget.tick();
    // Only the original boxing receipt authorizes separating these two JVM
    // return slots. Keep the primitive body and the original boxing call.
    if (proof.target.returns != "Z" || !proof.box ||
        proof.target.name != reference.name ||
        proof.target.parameters != reference.parameters)
      continue;
    const auto &owner = classes.at(proof.target.owner);
    std::set<std::string> pending(owner.interfaces.begin(),
                                  owner.interfaces.end());
    if (owner.superclass)
      pending.insert(*owner.superclass);
    std::set<std::string> seen;
    while (!pending.empty()) {
      budget.tick();
      const auto parent = *pending.begin();
      pending.erase(pending.begin());
      if (!seen.insert(parent).second)
        continue;
      const auto known = classes.find(parent);
      if (known == classes.end())
        unsupported(reference.identity(),
                    "boxed boolean return slot has an unknown ancestor");
      for (const auto &method : known->second.methods) {
        budget.tick();
        if (method.reference.name == proof.target.name &&
            method.reference.parameters == proof.target.parameters &&
            method.reference.returns == proof.target.returns)
          unsupported(reference.identity(),
                      "boxed boolean return slot also has an inherited "
                      "primitive contract");
      }
      if (known->second.superclass)
        pending.insert(*known->second.superclass);
      pending.insert(known->second.interfaces.begin(),
                     known->second.interfaces.end());
    }
    auto existing = result.source_method_names.find(proof.target);
    if (existing != result.source_method_names.end()) {
      if (existing->second.declaration != proof.target)
        unsupported(reference.identity(),
                    "boxed boolean return slot has a competing source name");
      existing->second.conflicting_contracts.insert(proof.dispatch);
      continue;
    }
    std::string spelling = "__neverdReturnSlot";
    while (occupied.contains(spelling)) {
      budget.tick();
      spelling += '_';
    }
    occupied.insert(spelling);
    result.source_method_names.emplace(
        proof.target, GenericInheritancePlan::SourceMethodName{
                          spelling, proof.target, {proof.dispatch}});
  }
}

bool deferredAbstractClassRequirement(
    const Class &cls, const Method &member, const MethodRef &specialized,
    const std::vector<SubstitutedType> &parameters,
    const SubstitutedType &result,
    const std::vector<std::pair<std::string, std::string>> &method_bounds,
    const ClassMap &classes, const GenericSignaturePlans &signatures,
    Budget &budget, const SourceMethodNames *method_names) {
  const Class *current = &cls;
  std::map<std::string, SubstitutedType> bindings;
  std::set<std::string> seen;
  while (current->superclass) {
    budget.tick();
    if (!seen.insert(current->name).second || seen.size() > 64)
      return false;
    if (*current->superclass == "Ljava/lang/Object;")
      return true;
    const auto parent = classes.find(*current->superclass);
    if (parent == classes.end() || has(parent->second.access, "interface") ||
        !independentSourceScope(parent->second))
      return false;
    auto composed = composeClassEdge(*current, parent->second, bindings,
                                     signatures, cls.name, budget);
    if (!composed)
      return false;
    bindings = std::move(*composed);
    const Method *provider = nullptr;
    for (const auto &candidate : parent->second.methods) {
      budget.tick();
      if (!sameSourceMethodName(candidate.reference, member.reference,
                                method_names) ||
          (candidate.reference.parameters != member.reference.parameters &&
           candidate.reference.parameters != specialized.parameters))
        continue;
      if (provider || !has(parent->second.access, "abstract") ||
          !has(candidate.access, "abstract") ||
          !has(candidate.access, "public") || has(candidate.access, "static") ||
          has(candidate.access, "bridge") ||
          !sameClosedInheritedThrows(candidate, member, signatures))
        return false;
      provider = &candidate;
    }
    if (provider) {
      const auto plan = signatures.methods.find(provider->reference);
      if (!method_bounds.empty()) {
        if (plan == signatures.methods.end() ||
            !plan->second.source_projections.empty() ||
            !plan->second.throws_types.empty())
          return false;
        // Reuse the full class-edge and alpha-equivalent method-bound proof
        // for an original abstract redeclaration; no declaration is invented.
        return inheritedClassContract(cls, member, specialized, parameters,
                                      result, method_bounds, classes,
                                      signatures, budget, true, method_names);
      }
      if (plan == signatures.methods.end()) {
        if (provider->generic_signature ||
            provider->reference.parameters.size() != parameters.size())
          return false;
        for (size_t i = 0; i < parameters.size(); ++i)
          if (parameters[i].signature != provider->reference.parameters[i])
            return false;
        return result.signature == provider->reference.returns;
      }
      const auto &actual = plan->second;
      if (!actual.type_parameters.empty() || !actual.throws_types.empty() ||
          !actual.source_projections.empty() || !actual.result ||
          actual.parameters.size() != parameters.size())
        return false;
      for (size_t i = 0; i < parameters.size(); ++i)
        if (substitute(actual, actual.parameters[i], bindings, cls.name, budget,
                       0, true)
                .signature != parameters[i].signature)
          return false;
      return substitute(actual, *actual.result, bindings, cls.name, budget, 0,
                        true)
                 .signature == result.signature;
    }
    current = &parent->second;
  }
  return false;
}

std::optional<std::map<std::string, SubstitutedType>> sourceInterfaceBindings(
    const Class &consumer, const Class &owner, const ClassMap &classes,
    const GenericSignaturePlans &signatures, Budget &budget) {
  using Bindings = std::map<std::string, SubstitutedType>;
  const auto own = signatures.classes.find(owner.name);
  if (own == signatures.classes.end() || own->second.type_parameters.empty())
    return Bindings{}; // The original closed receipt consumes no class binding.
  struct Use {
    const Class *declaration;
    Bindings bindings;
    std::set<std::string> path;
  };
  std::vector<Use> pending{{&consumer, {}, {}}};
  std::map<std::string, GenericSignature> plain;
  std::map<std::string, std::vector<std::string>> seen;
  std::optional<Bindings> result;
  const auto shape = [&](const Class &declaration) -> const GenericSignature * {
    if (const auto *plan =
            inheritanceClassSignature(declaration.name, signatures))
      return plan;
    if (declaration.generic_signature || !independentSourceScope(declaration))
      return nullptr;
    auto [entry, inserted] = plain.try_emplace(declaration.name);
    if (inserted)
      entry->second = plainRuntimeInterfaceSignature(declaration, budget);
    return &entry->second;
  };
  for (size_t at = 0; at < pending.size(); ++at) {
    budget.tick();
    auto use = pending[at];
    const auto &current = *use.declaration;
    const auto *plan = shape(current);
    const bool captured =
        plan && capturedSiblingClassShape(current, *plan, signatures);
    if (!plan || (!plan->source_projections.empty() && !captured) ||
        (!independentSourceScope(current) && !captured &&
         !(signatures.boxed_superclass_owners.contains(current.name) &&
           signatures.closed_static_local_classes.contains(current.name))) ||
        use.path.size() > 64 || !use.path.insert(current.name).second ||
        pending.size() > 128)
      return {};
    std::vector<std::string> identity;
    for (const auto &formal : plan->type_parameters) {
      const auto bound = use.bindings.find(formal.name);
      identity.push_back(bound == use.bindings.end()
                             ? current.name + "::" + formal.name
                             : bound->second.signature);
    }
    auto [prior, inserted] = seen.emplace(current.name, identity);
    if (!inserted) {
      if (prior->second != identity)
        return {};
      continue;
    }
    if (current.name == owner.name) {
      result = use.bindings;
      continue;
    }
    std::vector<std::pair<GenericTypeId, bool>> edges;
    for (auto id : plan->interfaces)
      edges.emplace_back(id, true);
    if (!has(current.access, "interface") && plan->superclass &&
        current.superclass != "Ljava/lang/Object;")
      edges.emplace_back(*plan->superclass, false);
    for (auto [id, interface] : edges) {
      const auto &edge = plan->types.at(id);
      const auto parent = classes.find(edge.erasure);
      const auto *formals =
          parent == classes.end() ? nullptr : shape(parent->second);
      if (!interface && parent != classes.end() &&
          !has(parent->second.access, "interface") &&
          knownCapturedSiblingClassEdge(current, parent->second, signatures)) {
        auto bindings = composeClassEdge(current, parent->second, use.bindings,
                                         signatures, consumer.name, budget);
        if (!bindings)
          return {};
        pending.push_back({&parent->second, std::move(*bindings), use.path});
        continue;
      }
      if (!formals || !formals->source_projections.empty() ||
          has(parent->second.access, "interface") != interface ||
          edge.segments.size() != 1 ||
          edge.segments.front().arguments.size() !=
              formals->type_parameters.size())
        return {};
      Bindings bindings;
      for (size_t i = 0; i < formals->type_parameters.size(); ++i) {
        const auto &argument = edge.segments.front().arguments[i];
        if (!argument.type ||
            argument.variance != GenericTypeArgument::Variance::Invariant)
          return {};
        bindings.emplace(formals->type_parameters[i].name,
                         substitute(*plan, *argument.type, use.bindings,
                                    consumer.name, budget, 0, true));
      }
      pending.push_back({&parent->second, std::move(bindings), use.path});
    }
  }
  return result;
}

bool sourceDefaultContractMatches(
    const Class &consumer, const Class &owner, const GenericSignature &contract,
    const std::vector<SubstitutedType> &parameters,
    const SubstitutedType &result, const ClassMap &classes,
    const GenericSignaturePlans &signatures, Budget &budget) {
  if (!contract.result || !contract.type_parameters.empty() ||
      !contract.throws_types.empty() || !contract.source_projections.empty() ||
      contract.parameters.size() != parameters.size())
    return false;
  const auto formals = signatures.classes.find(owner.name);
  for (const auto &type : contract.types) {
    budget.tick();
    if (type.kind == GenericType::Kind::TypeVariable &&
        (type.variable_owner != owner.name ||
         formals == signatures.classes.end() ||
         std::none_of(formals->second.type_parameters.begin(),
                      formals->second.type_parameters.end(),
                      [&](const auto &formal) {
                        return formal.name == type.variable_name;
                      })))
      return false;
  }
  const auto bindings =
      sourceInterfaceBindings(consumer, owner, classes, signatures, budget);
  if (!bindings)
    return false;
  for (size_t i = 0; i < parameters.size(); ++i)
    if (substitute(contract, contract.parameters[i], *bindings, consumer.name,
                   budget, 0, true)
            .signature != parameters[i].signature)
      return false;
  return substitute(contract, *contract.result, *bindings, consumer.name,
                    budget, 0, true)
             .signature == result.signature;
}

bool sourceScopedDefaultContract(const Class &consumer, const Class &owner,
                                 const Method &target, const Method &member,
                                 const std::vector<SubstitutedType> *parameters,
                                 const SubstitutedType *result,
                                 const ClassMap &classes,
                                 const GenericSignaturePlans &signatures,
                                 Budget &budget) {
  const auto proof = signatures.source_default_contracts.find(
      {target.reference, member.reference});
  if (owner.external || !parameters || !result ||
      proof == signatures.source_default_contracts.end() ||
      target.reference.parameters != member.reference.parameters ||
      target.reference.returns != member.reference.returns ||
      !target.generic_signature || !has(target.access, "public") ||
      has(target.access, "abstract") || has(target.access, "static") ||
      has(target.access, "bridge") || has(target.access, "native") ||
      target.instructions.empty())
    return false;
  return sourceDefaultContractMatches(consumer, owner, proof->second,
                                      *parameters, *result, classes, signatures,
                                      budget);
}

const Method *sourceDefaultParameterTarget(
    const Class &consumer, const Class &owner, const Method &bridge,
    const Method &member, const std::vector<SubstitutedType> *parameters,
    const SubstitutedType *result, const ClassMap &classes,
    const GenericSignaturePlans &signatures, Budget &budget) {
  const auto proof = signatures.source_default_parameter_bridges.find(
      {bridge.reference, member.reference});
  if (owner.external || !parameters || !result ||
      proof == signatures.source_default_parameter_bridges.end() ||
      bridge.access != Access{"public", "bridge", "synthetic"} ||
      bridge.generic_signature)
    return nullptr;
  const auto &contract = proof->second.contract;
  const auto *target = findMethod(owner, proof->second.target);
  if (!target || !has(target->access, "public") ||
      has(target->access, "abstract") || has(target->access, "static") ||
      has(target->access, "bridge") || !contract.result ||
      !contract.type_parameters.empty() || !contract.throws_types.empty() ||
      !contract.source_projections.empty() ||
      contract.parameters.size() != parameters->size())
    return nullptr;
  if (proof->second.raw_parameter_subsignature) {
    if (!originalRawParameterSubsignature(*target, member, *parameters, *result,
                                          0, true))
      return nullptr;
    verifyGenericBridge(bridge, *target, budget, nullptr, nullptr, &classes);
    return target;
  }
  if (!sourceDefaultContractMatches(consumer, owner, contract, *parameters,
                                    *result, classes, signatures, budget))
    return nullptr;
  verifyGenericBridge(bridge, *target, budget, nullptr, nullptr, &classes);
  return target;
}

const Method *uniqueInterfaceDefault(
    const Class &cls, const Class &contract, const Method &member,
    const MethodRef &specialized, const std::vector<std::string> &roots,
    const ClassMap &classes, const GenericSignaturePlans &signatures,
    Budget &budget, bool allow_source = false,
    bool exact_method_contract = false,
    const SourceMethodNames *method_names = nullptr,
    const std::vector<SubstitutedType> *complete_parameters = nullptr,
    const SubstitutedType *complete_result = nullptr) {
  if (roots.empty() || roots.size() > 128)
    return nullptr;
  // Select declarations over the complete known interface DAG. An overriding
  // abstract slot shadows an older default too; disjoint branches are harmless
  // only when they supply no competing slot or one unique maximal declaration.
  std::map<std::string, unsigned> states;
  std::map<std::string, std::set<std::string>> ancestors;
  std::map<MethodRef, const Method *> declarations;
  bool contains_contract = false;
  std::function<bool(const std::string &, unsigned)> visit =
      [&](const std::string &owner, unsigned depth) {
        budget.tick();
        const auto found = classes.find(owner);
        if (found == classes.end() || depth > 64 || states.size() > 128)
          return false;
        auto &state = states[owner];
        if (state)
          return state == 2;
        const auto &declaration = found->second;
        if (!has(declaration.access, "interface") ||
            declaration.superclass != "Ljava/lang/Object;" ||
            !independentSourceScope(declaration))
          return false;
        if (!declaration.external && !allow_source) {
          // A source interface without either erased or specialized slot
          // cannot select a new provider. Its complete generic edges are
          // checked by the common inheritance pass; keep the actual default
          // body in its original external declaration.
          const auto *plan = inheritanceClassSignature(owner, signatures);
          const auto relevant = [&](const Method &candidate) {
            return sameSourceMethodName(candidate.reference, member.reference,
                                        method_names) &&
                   (candidate.reference.parameters ==
                        member.reference.parameters ||
                    candidate.reference.parameters == specialized.parameters);
          };
          const Method *bridge = nullptr, *typed = nullptr,
                       *scoped_default = nullptr;
          for (const auto &candidate : declaration.methods) {
            budget.tick();
            // An unchanged source getter can implement this SDK slot only
            // through the common full ancestor/return proof. Class-owned
            // formals remain in their declaration scope; matching descriptors
            // alone cannot admit a generic source default.
            if (relevant(candidate) && candidate.reference.parameters.empty() &&
                candidate.reference.returns == member.reference.returns &&
                has(candidate.access, "public") &&
                !has(candidate.access, "abstract") &&
                !has(candidate.access, "static") &&
                !has(candidate.access, "private") &&
                !has(candidate.access, "bridge") &&
                !candidate.instructions.empty() &&
                signatures.covariant_interface_default_returns.contains(
                    {candidate.reference, member.reference})) {
              if (scoped_default)
                return false;
              scoped_default = &candidate;
            }
            if (sourceScopedDefaultContract(
                    cls, declaration, candidate, member, complete_parameters,
                    complete_result, classes, signatures, budget)) {
              if (scoped_default && scoped_default != &candidate)
                return false;
              scoped_default = &candidate;
            }
            if (const auto *target = sourceDefaultParameterTarget(
                    cls, declaration, candidate, member, complete_parameters,
                    complete_result, classes, signatures, budget)) {
              if (bridge)
                return false;
              bridge = &candidate;
              typed = target;
            }
          }
          if (!contract.external || exact_method_contract ||
              (plan && !plan->source_projections.empty()) ||
              std::any_of(
                  declaration.methods.begin(), declaration.methods.end(),
                  [&](const Method &candidate) {
                    budget.tick();
                    return relevant(candidate) && &candidate != bridge &&
                           &candidate != typed && &candidate != scoped_default;
                  }))
            return false;
        }
        state = 1;
        contains_contract |= declaration.name == contract.name;
        auto &reachable = ancestors[owner];
        reachable.insert(owner);
        for (const auto &candidate : declaration.methods) {
          budget.tick();
          if (!sameSourceMethodName(candidate.reference, member.reference,
                                    method_names) ||
              (candidate.reference.parameters != member.reference.parameters &&
               candidate.reference.parameters != specialized.parameters))
            continue;
          if (has(candidate.access, "static") ||
              has(candidate.access, "private")) {
            // A source declaration in this exact closed slot cannot hide
            // behind an older default: Java rejects its incompatible access
            // or static/instance contract even when the runtime slot matches.
            if (exact_method_contract && !declaration.external)
              return false;
            continue;
          }
          if (candidate.reference.owner != declaration.name ||
              !has(candidate.access, "public"))
            return false;
          const auto primitive =
              signatures.primitive_void_bridges.find(candidate.reference);
          const bool primitive_contract =
              primitive != signatures.primitive_void_bridges.end() &&
              primitive->second.contracts.contains(member.reference);
          if (!declaration.external && has(candidate.access, "bridge") &&
              candidate.reference.parameters == member.reference.parameters &&
              member.reference.parameters != specialized.parameters &&
              !primitive_contract &&
              !sourceDefaultParameterTarget(
                  cls, declaration, candidate, member, complete_parameters,
                  complete_result, classes, signatures, budget))
            return false;
          declarations.emplace(candidate.reference, &candidate);
        }
        for (const auto &parent : declaration.interfaces) {
          budget.tick();
          if (emptyMarkerHierarchy(parent, classes, budget))
            continue;
          if (!visit(parent, depth + 1))
            return false;
          for (const auto &ancestor : ancestors.at(parent)) {
            budget.tick();
            reachable.insert(ancestor);
          }
        }
        state = 2;
        return true;
      };
  for (const auto &root : roots)
    if (!visit(root, 0))
      return nullptr;
  std::set<std::string> maximal_owners;
  for (const auto &[reference, candidate] : declarations) {
    budget.tick();
    const bool shadowed = std::any_of(
        declarations.begin(), declarations.end(), [&](const auto &other) {
          budget.tick();
          return other.first.owner != reference.owner &&
                 ancestors.at(other.first.owner).contains(reference.owner);
        });
    if (shadowed)
      continue;
    maximal_owners.insert(reference.owner);
  }
  if (maximal_owners.size() != 1)
    return nullptr;
  const Method *provider = nullptr;
  for (const auto &[reference, candidate] : declarations) {
    budget.tick();
    if (reference.owner != *maximal_owners.begin())
      continue;
    if (has(candidate->access, "abstract") ||
        reference.parameters != member.reference.parameters)
      continue;
    const auto &provider_owner = classes.at(reference.owner);
    const auto *parameter_target = sourceDefaultParameterTarget(
        cls, provider_owner, *candidate, member, complete_parameters,
        complete_result, classes, signatures, budget);
    const bool scoped_contract = sourceScopedDefaultContract(
        cls, provider_owner, *candidate, member, complete_parameters,
        complete_result, classes, signatures, budget);
    const bool compatible = std::all_of(
        declarations.begin(), declarations.end(), [&](const auto &other) {
          budget.tick();
          // An original erased entry and its exact typed default form one
          // source declaration. Ancestor slots consume their own complete
          // receipt after composition into this consumer's class scope.
          if (parameter_target &&
              (other.second == candidate || other.second == parameter_target ||
               sourceDefaultParameterTarget(cls, provider_owner, *candidate,
                                            *other.second, complete_parameters,
                                            complete_result, classes,
                                            signatures, budget)))
            return true;
          if (scoped_contract &&
              (other.second == candidate ||
               sourceScopedDefaultContract(cls, provider_owner, *candidate,
                                           *other.second, complete_parameters,
                                           complete_result, classes, signatures,
                                           budget)))
            return true;
          if (exact_method_contract &&
              sameCompleteMethodContract(*candidate, *other.second, signatures,
                                         cls.name, budget, classes))
            return true;
          // Complete covariance receipts remain authoritative for an original
          // default whose result differs. Equal runtime erasures alone cannot
          // replace the full comparison for a closed generic slot.
          return (!exact_method_contract &&
                  reference.returns == other.first.returns) ||
                 signatures.external_default_return_overrides.contains(
                     {reference, other.first}) ||
                 signatures.covariant_interface_default_returns.contains(
                     {reference, other.first});
        });
    if (!compatible)
      continue;
    if (provider)
      return nullptr;
    provider = candidate;
  }
  if (!contains_contract || !provider)
    return nullptr;
  return provider;
}

void planScopedInterfaceSuperCalls(const ClassMap &classes,
                                   const GenericSignaturePlans &signatures,
                                   Budget &budget,
                                   GenericInheritancePlan &result) {
  for (const auto &[name, cls] : classes) {
    budget.tick();
    const auto own = signatures.classes.find(name);
    if (cls.external || !has(cls.access, "public") ||
        has(cls.access, "interface") || !independentSourceScope(cls) ||
        cls.interfaces.empty() || !cls.superclass ||
        (own != signatures.classes.end() &&
         !own->second.source_projections.empty()))
      continue;
    for (const auto &caller : cls.methods) {
      budget.tick();
      const auto full = signatures.methods.find(caller.reference);
      if (!has(caller.access, "public") || has(caller.access, "static") ||
          has(caller.access, "abstract") || has(caller.access, "native") ||
          has(caller.access, "bridge") || has(caller.access, "varargs") ||
          (caller.declared_throws && !caller.declared_throws->empty()))
        continue;
      // Descriptor-only source scopes have no generic substitution to infer.
      // Keep their exact original declarations and use the same direct-edge,
      // complete graph, class-shadow and most-specific selector below.
      const bool plain = !cls.generic_signature && !caller.generic_signature &&
                         cls.superclass == "Ljava/lang/Object;";
      std::vector<SubstitutedType> parameters;
      std::optional<SubstitutedType> returned;
      const bool scoped = [&] {
        if (own == signatures.classes.end() ||
            full == signatures.methods.end() ||
            sourceMethodSignature(caller.reference, signatures) !=
                &full->second ||
            !full->second.type_parameters.empty() ||
            !full->second.source_projections.empty() ||
            !full->second.throws_types.empty() || !full->second.result ||
            full->second.parameters.size() !=
                caller.reference.parameters.size() ||
            full->second.parameters.size() > 8)
          return false;
        const auto &returns = full->second.types.at(*full->second.result);
        if (returns.kind != GenericType::Kind::Class ||
            returns.erasure != caller.reference.returns ||
            returns.segments.size() != 1 ||
            returns.segments[0].arguments.size() != 1)
          return false;
        const auto &slot = returns.segments[0].arguments[0];
        if (!slot.type ||
            slot.variance != GenericTypeArgument::Variance::Invariant)
          return false;
        const auto &element = full->second.types.at(*slot.type);
        if (element.kind != GenericType::Kind::TypeVariable ||
            element.variable_owner != name ||
            std::none_of(own->second.type_parameters.begin(),
                         own->second.type_parameters.end(), [&](const auto &p) {
                           return p.name == element.variable_name &&
                                  p.erasure == element.erasure;
                         }))
          return false;
        for (size_t i = 0; i < full->second.parameters.size(); ++i) {
          budget.tick();
          const auto id = full->second.parameters[i];
          const auto &type = full->second.types.at(id);
          if (type.kind != GenericType::Kind::Primitive ||
              type.erasure != caller.reference.parameters[i])
            return false;
          parameters.push_back(
              substitute(full->second, id, {}, name, budget, 0, true));
        }
        returned = substitute(full->second, *full->second.result, {}, name,
                              budget, 0, true);
        return true;
      }();
      if (!plain && !scoped)
        continue;
      for (const auto &op : caller.instructions) {
        budget.tick();
        const auto *call = std::get_if<MethodRef>(&op.reference);
        if ((op.opcode != "invoke-super" &&
             op.opcode != "invoke-super/range") ||
            !call || call->name.starts_with('<') ||
            call->parameters != caller.reference.parameters ||
            call->returns != caller.reference.returns ||
            std::find(cls.interfaces.begin(), cls.interfaces.end(),
                      call->owner) == cls.interfaces.end())
          continue;
        // Java cannot qualify a redundant direct interface. The complete
        // consumer graph below also owns all nominal roles and substitutions.
        bool redundant = std::any_of(
            cls.interfaces.begin(), cls.interfaces.end(),
            [&](const auto &other) {
              return other != call->owner &&
                     isAssignableReference(other, call->owner, classes, budget);
            });
        redundant |= isAssignableReference(*cls.superclass, call->owner,
                                           classes, budget);
        if (redundant)
          continue;
        bool class_shadow = false;
        std::string parent = *cls.superclass;
        std::set<std::string> class_path;
        while (parent != "Ljava/lang/Object;") {
          budget.tick();
          const auto known = classes.find(parent);
          if (known == classes.end() || !class_path.insert(parent).second ||
              class_path.size() > 64 ||
              has(known->second.access, "interface") ||
              !known->second.superclass) {
            class_shadow = true;
            break;
          }
          for (const auto &member : known->second.methods) {
            budget.tick();
            class_shadow |= !has(member.access, "static") &&
                            !has(member.access, "private") &&
                            sameSourceMethodName(member.reference, *call,
                                                 &result.source_method_names) &&
                            member.reference.parameters == call->parameters &&
                            member.reference.returns == call->returns;
          }
          parent = *known->second.superclass;
        }
        if (class_shadow)
          continue;
        std::vector<std::string> pending{call->owner};
        std::set<std::string> seen;
        std::vector<const Method *> candidates;
        bool known_graph = true;
        for (size_t at = 0; at < pending.size(); ++at) {
          budget.tick();
          const auto current = pending[at];
          if (!seen.insert(current).second)
            continue;
          const auto known = classes.find(current);
          if (known == classes.end() || seen.size() > 128 ||
              !has(known->second.access, "public") ||
              !has(known->second.access, "interface") ||
              has(known->second.access, "annotation") ||
              (plain && known->second.generic_signature) ||
              !independentSourceScope(known->second)) {
            known_graph = false;
            break;
          }
          for (const auto &candidate : known->second.methods) {
            budget.tick();
            const auto &ref = candidate.reference;
            if (ref.name == call->name && ref.parameters == call->parameters &&
                ref.returns == call->returns)
              candidates.push_back(&candidate);
          }
          pending.insert(pending.end(), known->second.interfaces.begin(),
                         known->second.interfaces.end());
        }
        if (!known_graph)
          continue;
        for (const auto *candidate : candidates) {
          budget.tick();
          const auto &ref = candidate->reference;
          const auto shape = signatures.methods.find(ref);
          if (candidate->access != Access{"public"} ||
              (!classes.at(ref.owner).external &&
               candidate->instructions.empty()) ||
              (candidate->declared_throws &&
               !candidate->declared_throws->empty()) ||
              (plain ? candidate->generic_signature.has_value()
                     : shape == signatures.methods.end() ||
                           sourceMethodSignature(ref, signatures) !=
                               &shape->second ||
                           !sourceDefaultContractMatches(
                               cls, classes.at(ref.owner), shape->second,
                               parameters, *returned, classes, signatures,
                               budget)))
            continue;
          const auto *selected = uniqueInterfaceDefault(
              cls, classes.at(ref.owner), *candidate, *call, {call->owner},
              classes, signatures, budget, true, true,
              &result.source_method_names, plain ? nullptr : &parameters,
              plain ? nullptr : &*returned);
          if (selected == candidate) {
            // Shared full-tree composition and most-specific selection own
            // this declaration. Body still validates the original receiver,
            // argument words, instructions, effects and result consumption.
            result.scoped_interface_super_calls.emplace(
                std::pair{caller.reference, *call}, ref);
            break;
          }
        }
      }
    }
  }
}

bool uncontestedInterfaceDefault(
    const Class &cls, const Class &contract, const Method &member,
    const MethodRef &specialized, const ClassMap &classes,
    const GenericSignaturePlans &signatures, Budget &budget,
    bool allow_source = false, bool exact_method_contract = false,
    const SourceMethodNames *method_names = nullptr,
    const std::vector<SubstitutedType> *complete_parameters = nullptr,
    const SubstitutedType *complete_result = nullptr) {
  std::vector<std::string> roots;
  for (const auto &interface : cls.interfaces) {
    budget.tick();
    if (!emptyMarkerHierarchy(interface, classes, budget))
      roots.push_back(interface);
  }
  std::vector<const Class *> class_path;
  bool has_parent_interfaces = false;
  std::set<std::string> seen;
  for (const Class *current = &cls; current;) {
    budget.tick();
    if (!current->superclass || !seen.insert(current->name).second ||
        seen.size() > 64 ||
        std::any_of(
            current->methods.begin(), current->methods.end(),
            [&](const Method &candidate) {
              return sameSourceMethodName(candidate.reference, member.reference,
                                          method_names) &&
                     (candidate.reference.parameters ==
                          member.reference.parameters ||
                      candidate.reference.parameters == specialized.parameters);
            }))
      return false;
    if (current->external && current == &cls)
      return false;
    if (current != &cls && !independentSourceScope(*current) &&
        (class_path.empty() || !knownCapturedSiblingClassEdge(
                                   *class_path.back(), *current, signatures)))
      return false;
    if (current != &cls &&
        !inheritanceClassSignature(current->name, signatures)) {
      // Default selection must consume the same proved source names as the
      // class-ancestry entry gate. An unrelated original return-only slot may
      // already have a disclosed alias beside an unchanged SDK scalar slot.
      if (!knownNongenericAncestors(*current, cls, classes, signatures, budget,
                                    false, true, method_names))
        return false;
    } else if (current->external &&
               !externalGenericParents(*current, cls, classes, signatures,
                                       budget))
      return false;
    class_path.push_back(current);
    if (current != &cls)
      for (const auto &interface : current->interfaces) {
        budget.tick();
        if (!emptyMarkerHierarchy(interface, classes, budget))
          roots.push_back(interface);
      }
    has_parent_interfaces |=
        current != &cls && !onlyEmptyMarkerParents(*current, classes, budget);
    if (*current->superclass == "Ljava/lang/Object;")
      break;
    const auto ancestor = classes.find(*current->superclass);
    if (ancestor == classes.end())
      return false;
    current = &ancestor->second;
  }
  if (!uniqueInterfaceDefault(cls, contract, member, specialized, roots,
                              classes, signatures, budget, allow_source,
                              exact_method_contract, method_names,
                              complete_parameters, complete_result))
    return false;
  // With no inherited nonempty interface root, the original nominal default
  // proof is complete. Common-validator source projections on unrelated
  // class/formal declarations do not change the selected runtime default.
  if (!has_parent_interfaces)
    return true;
  std::map<std::string, GenericSignature> raw_shapes;
  auto class_shape = [&](const Class &owner) -> const GenericSignature * {
    if (const auto *plan = inheritanceClassSignature(owner.name, signatures))
      return plan;
    if (owner.generic_signature || !independentSourceScope(owner))
      return nullptr;
    // The complete known plain ancestry was proved above. These local shapes
    // retain actual superclass/interface edges without inserting a Signature.
    auto [shape, inserted] = raw_shapes.try_emplace(owner.name);
    if (inserted)
      shape->second = plainRuntimeInterfaceSignature(owner, budget);
    return &shape->second;
  };
  const auto *own = class_shape(cls);
  if (!own)
    return false;
  const bool erased_inner_scope = objectCapturedScopeProjection(*own);
  if (!usableInheritedClassShape(
          *own, erased_inner_scope,
          signatures.raw_marker_superclasses.contains(cls.name)) &&
      !capturedSiblingClassShape(cls, *own, signatures))
    return false;
  std::map<std::string, std::string> own_interfaces;
  struct InterfaceUse {
    const GenericSignature *signature;
    GenericTypeId id;
    std::map<std::string, SubstitutedType> bindings;
    std::set<std::string> path;
  };
  std::vector<InterfaceUse> pending;
  std::map<std::string, SubstitutedType> class_bindings;
  for (size_t i = 0; i < class_path.size(); ++i) {
    budget.tick();
    const auto *plan = class_shape(*class_path[i]);
    if (plan != nullptr) {
      if (!usableInheritedClassShape(
              *plan, erased_inner_scope,
              signatures.raw_marker_superclasses.contains(
                  class_path[i]->name)) &&
          !capturedSiblingClassShape(*class_path[i], *plan, signatures))
        return false;
      for (auto id : plan->interfaces)
        pending.push_back({plan, id, class_bindings, {}});
    }
    if (i + 1 < class_path.size()) {
      auto composed =
          composeClassEdge(*class_path[i], *class_path[i + 1], class_bindings,
                           signatures, cls.name, budget, erased_inner_scope);
      if (!composed)
        return false;
      class_bindings = std::move(*composed);
    }
  }
  for (size_t next = 0; next < pending.size(); ++next) {
    budget.tick();
    auto use = pending[next];
    const auto &edge = use.signature->types.at(use.id);
    const auto declaration = classes.find(edge.erasure);
    if (declaration == classes.end() ||
        !has(declaration->second.access, "interface") ||
        declaration->second.superclass != "Ljava/lang/Object;" ||
        !independentSourceScope(declaration->second) ||
        edge.segments.size() != 1 || use.path.size() > 64 ||
        !use.path.insert(edge.erasure).second || own_interfaces.size() > 128)
      return false;
    const GenericSignature *plan = inheritanceClassSignature(edge.erasure, signatures);
    if (plan) {
      if (!plan->source_projections.empty())
        return false;
    } else {
      auto [shape, inserted] = raw_shapes.try_emplace(edge.erasure);
      if (inserted)
        shape->second =
            plainRuntimeInterfaceSignature(declaration->second, budget);
      plan = &shape->second;
    }
    const auto &arguments = edge.segments[0].arguments;
    if (arguments.size() != plan->type_parameters.size())
      return false;
    const auto identity = substitute(*use.signature, use.id, use.bindings,
                                     cls.name, budget, 0, true)
                              .signature;
    auto [seen, inserted] = own_interfaces.emplace(edge.erasure, identity);
    if (!inserted) {
      if (seen->second != identity)
        return false;
      continue;
    }
    std::map<std::string, SubstitutedType> composed;
    for (size_t i = 0; i < arguments.size(); ++i) {
      if (!arguments[i].type ||
          arguments[i].variance != GenericTypeArgument::Variance::Invariant)
        return false;
      composed.emplace(plan->type_parameters[i].name,
                       substitute(*use.signature, *arguments[i].type,
                                  use.bindings, cls.name, budget, 0, true));
    }
    for (auto id : plan->interfaces)
      pending.push_back({plan, id, composed, use.path});
  }
  std::map<std::string, SubstitutedType> bindings;
  std::map<std::string, unsigned> disjoint_states;
  std::function<bool(const std::string &, unsigned)> disjoint_plain_root =
      [&](const std::string &owner, unsigned depth) {
        budget.tick();
        const auto found = classes.find(owner);
        if (found == classes.end() || depth > 64 ||
            disjoint_states.size() > 128 ||
            !has(found->second.access, "interface") ||
            found->second.superclass != "Ljava/lang/Object;" ||
            !independentSourceScope(found->second))
          return false;
        const auto *signature = inheritanceClassSignature(owner, signatures);
        if (signature &&
            (!signature->type_parameters.empty() ||
             !signature->source_projections.empty() ||
             std::any_of(signature->types.begin(),
                         signature->types.end(), [](const auto &type) {
                           return std::any_of(
                               type.segments.begin(), type.segments.end(),
                               [](const auto &segment) {
                                 return !segment.arguments.empty();
                               });
                         })))
          return false;
        auto &state = disjoint_states[owner];
        if (state)
          return state == 2;
        state = 1;
        for (const auto &candidate : found->second.methods) {
          budget.tick();
          if (sameSourceMethodName(candidate.reference, member.reference,
                                   method_names) &&
              (candidate.reference.parameters == member.reference.parameters ||
               candidate.reference.parameters == specialized.parameters))
            return false;
        }
        for (const auto &parent : found->second.interfaces)
          if (!disjoint_plain_root(parent, depth + 1))
            return false;
        state = 2;
        return true;
      };
  for (const Class *current : class_path) {
    budget.tick();
    const auto *plan = class_shape(*current);
    if (current != &cls && !onlyEmptyMarkerParents(*current, classes, budget)) {
      // Redundant generic roots require complete argument equality. Plain
      // unrelated roots cannot select or shadow this default only after their
      // complete known DAG has no erased or specialized parameter slot.
      // Every captured ancestor edge was certified on the original class
      // path above. Keep complete repeated interface argument equality here;
      // the common source scope receipt does not select a default provider.
      if (plan != nullptr &&
          !usableInheritedClassShape(
              *plan, erased_inner_scope,
              signatures.raw_marker_superclasses.contains(current->name)) &&
          !capturedSiblingClassShape(*current, *plan, signatures))
        return false;
      if (plan == nullptr) {
        for (const auto &root : current->interfaces)
          if (!disjoint_plain_root(root, 0))
            return false;
        // The complete plain class chain was proved above. Continue composing
        // its empty formal bindings rather than stopping at this disjoint root.
      } else {
        for (auto id : plan->interfaces) {
          budget.tick();
          const auto &edge = plan->types.at(id);
          if (emptyMarkerHierarchy(edge.erasure, classes, budget))
            continue;
          const auto matching = own_interfaces.find(edge.erasure);
          if (matching != own_interfaces.end()) {
            if (substitute(*plan, id, bindings, cls.name, budget, 0, true)
                    .signature != matching->second)
              return false;
          } else if (!disjoint_plain_root(edge.erasure, 0))
            return false;
        }
      }
    }
    if (*current->superclass == "Ljava/lang/Object;")
      return true;
    const auto ancestor = classes.find(*current->superclass);
    if (ancestor == classes.end())
      return false;
    auto composed =
        composeClassEdge(*current, ancestor->second, bindings, signatures,
                         cls.name, budget, erased_inner_scope);
    if (!composed)
      return false;
    bindings = std::move(*composed);
  }
  return false;
}

bool knownRawSourceInterfaces(const Class &parent, const Class &child,
                              const ClassMap &classes,
                              const GenericSignaturePlans &signatures,
                              Budget &budget, const std::string *root) {
  std::map<std::string, unsigned> states;
  std::function<bool(const std::string &, unsigned)> visit =
      [&](const std::string &name, unsigned depth) {
        budget.tick();
        const auto found = classes.find(name);
        if (depth > 64 || states.size() > 128 || found == classes.end() ||
            !has(found->second.access, "interface") ||
            found->second.superclass != "Ljava/lang/Object;" ||
            !independentSourceScope(found->second))
          return false;
        const auto plan = signatures.classes.find(name);
        if (plan != signatures.classes.end() &&
            (!plan->second.type_parameters.empty() ||
             !plan->second.source_projections.empty()))
          return false;
        auto &state = states[name];
        if (state)
          return state == 2;
        state = 1;
        const auto &contract = found->second;
        for (const auto &required : contract.methods) {
          budget.tick();
          if (has(required.access, "static") || has(required.access, "private"))
            continue;
          if (!has(required.access, "public") || has(required.access, "bridge"))
            return false;
          auto slot = [&](const Class &owner) -> const Method * {
            for (const auto &candidate : owner.methods) {
              budget.tick();
              if (candidate.reference.name == required.reference.name &&
                  candidate.reference.parameters ==
                      required.reference.parameters)
                return &candidate;
            }
            return nullptr;
          };
          const auto *implementation = slot(parent);
          if (implementation) {
            if (!sameCompleteMethodContract(*implementation, required,
                                            signatures, child.name, budget,
                                            classes))
              return false;
            if (has(implementation->access, "abstract") &&
                !has(parent.access, "abstract"))
              return false;
          } else if (!uncontestedInterfaceDefault(
                         parent, contract, required, required.reference,
                         classes, signatures, budget, true, true)) {
            // Abstract source classes can retain this original interface
            // obligation across multiple class edges. Any declaration must
            // keep the complete contract; a concrete child must implement it.
            const bool can_defer = !parent.external &&
                                   has(parent.access, "abstract") &&
                                   has(required.access, "abstract");
            const auto *deferred = can_defer ? slot(child) : nullptr;
            if (!deferred) {
              if (!can_defer || !has(child.access, "abstract"))
                return false;
            } else if ((has(deferred->access, "abstract") &&
                        !has(child.access, "abstract")) ||
                       !sameCompleteMethodContract(*deferred, required,
                                                   signatures, child.name,
                                                   budget, classes)) {
              return false;
            }
          }
          // A child override of an interface-only slot must retain that
          // complete plain runtime contract; a superclass declaration is
          // checked by the existing authoritative member substitution path
          // instead.
          if (!implementation)
            if (const auto *override = slot(child);
                override &&
                !sameCompleteMethodContract(*override, required, signatures,
                                            child.name, budget, classes))
              return false;
        }
        for (const auto &ancestor : contract.interfaces)
          if (!visit(ancestor, depth + 1))
            return false;
        state = 2;
        return true;
      };
  if (root)
    return visit(*root, 0);
  for (const auto &name : parent.interfaces) {
    const auto plan = signatures.classes.find(name);
    if (plan != signatures.classes.end() &&
        !plan->second.type_parameters.empty())
      continue; // Parameterized roots retain the existing edge composition.
    if (!visit(name, 0))
      return false;
  }
  return !states.empty();
}

bool externalGenericParents(const Class &parent, const Class &child,
                            const ClassMap &classes,
                            const GenericSignaturePlans &signatures,
                            Budget &budget) {
  // Dependency bodies and their existing interface implementation stay in
  // the unchanged binary. Source overrides need a composed class contract;
  // new interface/default overrides require their own ownership proof.
  std::vector<const Class *> providers;
  std::set<std::string> seen;
  for (const Class *current = &parent; current;) {
    budget.tick();
    const auto found = signatures.classes.find(current->name);
    if (!seen.insert(current->name).second || seen.size() > 64 ||
        !current->external || has(current->access, "interface") ||
        !independentSourceScope(*current) || !current->superclass ||
        found == signatures.classes.end() ||
        std::none_of(current->methods.begin(), current->methods.end(),
                     [](const Method &method) {
                       return method.reference.name == "<init>";
                     }))
      return false;
    providers.push_back(current);
    if (*current->superclass == "Ljava/lang/Object;")
      break;
    const auto &super = found->second.types.at(*found->second.superclass);
    if (super.segments.size() != 1)
      return false;
    if (super.segments.front().arguments.empty()) {
      // The same complete plain-ancestor proof owns this terminal edge on
      // source and dependency paths. Keep the generic providers below for
      // their existing interface ownership and member substitution checks.
      if (!knownNongenericAncestors(*current, child, classes, signatures,
                                    budget, true, false, nullptr, true))
        return false;
      break;
    }
    const auto ancestor = classes.find(*current->superclass);
    if (ancestor == classes.end())
      return false;
    current = &ancestor->second;
  }
  if (const auto object = classes.find("Ljava/lang/Object;");
      object != classes.end() && object->second.external)
    providers.push_back(&object->second);
  std::vector<std::string> interface_roots;
  for (const auto *provider : providers)
    for (const auto &root : provider->interfaces)
      if (std::find(interface_roots.begin(), interface_roots.end(), root) ==
          interface_roots.end())
        interface_roots.push_back(root);
  std::map<std::string, unsigned> states;
  std::function<bool(const std::string &, unsigned)> visit =
      [&](const std::string &name, unsigned depth) {
        budget.tick();
        const auto found = classes.find(name);
        if (depth > 64 || states.size() > 128 || found == classes.end() ||
            !found->second.external ||
            !has(found->second.access, "interface") ||
            found->second.superclass != "Ljava/lang/Object;")
          return false;
        auto &state = states[name];
        if (state)
          return state == 2;
        state = 1;
        for (const auto &member : found->second.methods) {
          budget.tick();
          if (has(member.access, "static") || has(member.access, "private"))
            continue;
          const bool class_slot = std::any_of(
              providers.begin(), providers.end(), [&](const Class *provider) {
                return std::any_of(provider->methods.begin(),
                                   provider->methods.end(),
                                   [&](const Method &candidate) {
                                     budget.tick();
                                     return has(candidate.access, "public") &&
                                            !has(candidate.access, "static") &&
                                            candidate.reference.name ==
                                                member.reference.name &&
                                            candidate.reference.parameters ==
                                                member.reference.parameters &&
                                            candidate.reference.returns ==
                                                member.reference.returns;
                                   });
              });
          if (class_slot)
            continue;
          const auto *original_child =
              inheritanceClassSignature(child.name, signatures);
          const auto interface_scope = signatures.classes.find(name);
          const bool composed_override =
              child.generic_signature && original_child &&
              original_child->source_projections.empty() &&
              interface_scope != signatures.classes.end() &&
              !interface_scope->second.type_parameters.empty();
          // Parameterized interface roots enter the authoritative full member
          // pass. Comparing an override here without its class bindings would
          // reject valid fixed scopes before that pass can verify its body.
          if (!composed_override &&
              std::any_of(child.methods.begin(), child.methods.end(),
                          [&](const Method &candidate) {
                            return candidate.reference.name ==
                                       member.reference.name &&
                                   candidate.reference.parameters.size() ==
                                       member.reference.parameters.size() &&
                                   !sameCompleteMethodContract(
                                       candidate, member, signatures,
                                       child.name, budget, classes);
                          }))
            return false;
          if (has(member.access, "abstract")) {
            // A generic abstract SDK ancestor need not itself implement this
            // interface slot. The full member queue composes the child's class
            // and interface arguments, then proves its own or nearest inherited
            // provider. Admission cannot demand an interface default before
            // those authoritative downstream class bindings are available.
            if (composed_override)
              continue;
            const auto *selected = uniqueInterfaceDefault(
                child, found->second, member, member.reference, interface_roots,
                classes, signatures, budget);
            if (!selected ||
                !(signatures.external_default_return_overrides.contains(
                      {selected->reference, member.reference}) ||
                  sameCompleteMethodContract(*selected, member, signatures,
                                             child.name, budget, classes)))
              return false;
          }
        }
        for (const auto &ancestor : found->second.interfaces)
          if (!visit(ancestor, depth + 1))
            return false;
        state = 2;
        return true;
      };
  for (const auto *provider : providers)
    for (const auto &interface : provider->interfaces)
      if (!visit(interface, 0))
        return false;
  return true;
}

bool canNameVoidUnitTarget(const Class &cls, const Method &target,
                           const ClassMap &classes, Budget &budget,
                           std::set<std::string> &occupied_names) {
  if (!has(cls.access, "final") || target.reference.name.starts_with('<') ||
      !cls.superclass)
    return false;
  for (const auto &method : cls.methods) {
    budget.tick();
    occupied_names.insert(method.reference.name);
  }
  std::vector<std::string> pending = cls.interfaces;
  pending.push_back(*cls.superclass);
  std::set<std::string> seen;
  while (!pending.empty()) {
    budget.tick();
    auto parent = std::move(pending.back());
    pending.pop_back();
    if (parent == "Ljava/lang/Object;") {
      // Object may be implicit in a declaration-only input. Its reserved
      // member names cannot use this bounded rename proof; a separate complete
      // slot proof would be needed to exclude inherited behavior there.
      for (const auto &name : objectSourceMethodNames()) {
        occupied_names.emplace(name);
        if (target.reference.name == name)
          return false;
      }
      continue;
    }
    // Missing declarations cannot establish whether renaming loses an
    // inherited void override, regardless of the original method's name.
    if (!seen.insert(parent).second)
      continue;
    if (seen.size() > 128)
      return false;
    const auto found = classes.find(parent);
    if (found == classes.end())
      return false;
    for (const auto &method : found->second.methods) {
      budget.tick();
      occupied_names.insert(method.reference.name);
      if (method.reference.name == target.reference.name &&
          method.reference.parameters == target.reference.parameters &&
          !has(method.access, "private") &&
          (has(method.access, "static") || has(method.access, "final") ||
           (method.reference.returns != "Ljava/lang/Object;" &&
            method.reference.returns != "Lkotlin/Unit;")))
        return false;
    }
    if (found->second.superclass)
      pending.push_back(*found->second.superclass);
    pending.insert(pending.end(), found->second.interfaces.begin(),
                   found->second.interfaces.end());
  }
  return true;
}

std::optional<FieldRef> unitResultField(const ClassMap &classes,
                                        Budget &budget) {
  const auto found = classes.find("Lkotlin/Unit;");
  if (found == classes.end() || !has(found->second.access, "public") ||
      has(found->second.access, "interface"))
    return {};
  const FieldRef reference{"Lkotlin/Unit;", "INSTANCE", "Lkotlin/Unit;"};
  for (const auto &field : found->second.fields) {
    budget.tick();
    if (field.reference == reference && has(field.access, "public") &&
        has(field.access, "static") && has(field.access, "final") &&
        !field.generic_signature)
      return reference;
  }
  return {};
}

bool originalInheritedBridgeThrows(const Method &bridge, const Method &target,
                                   const GenericSignaturePlans *signatures,
                                   const Method *contract) {
  if (signatures && contract &&
      emptyClosedInheritedBridgeThrows(bridge, target, *contract, *signatures))
    return true;
  if (signatures && contract &&
      validatedUncheckedDeclaredThrows(target, *signatures) &&
      (!contract->declared_throws || contract->declared_throws->empty())) {
    auto erased = contract->reference;
    erased.owner = bridge.reference.owner;
    // A typed implementation may declare only proven unchecked exceptions
    // while javac copies its inherited empty Exceptions contract to the bridge.
    // Preserve both original declarations rather than copying target metadata.
    return bridge.reference == erased &&
           bridge.declared_throws == contract->declared_throws;
  }
  if (signatures && contract && contract->declared_throws &&
      !contract->declared_throws->empty() &&
      signatures->closed_declared_throws.contains(contract->reference)) {
    auto erased = contract->reference;
    erased.owner = bridge.reference.owner;
    // javac copies the inherited declaration's ordered Exceptions attribute
    // to the erased entry even when its concrete target declares none.
    return bridge.reference == erased &&
           bridge.declared_throws == contract->declared_throws &&
           sameClosedInheritedThrows(bridge, *contract, *signatures) &&
           (bridge.declared_throws == target.declared_throws ||
            emptyClosedInheritedThrows(target, *contract, *signatures));
  }
  return bridge.declared_throws == target.declared_throws;
}

bool publicErasedProtectedTarget(const Method &bridge, const Method &target,
                                 const ClassMap &classes, Budget &budget,
                                 const GenericSignaturePlans *signatures) {
  budget.tick();
  const auto owner = classes.find(target.reference.owner);
  if (owner == classes.end() || has(owner->second.access, "interface") ||
      bridge.reference.owner != target.reference.owner ||
      bridge.reference.name != target.reference.name ||
      bridge.access != Access{"public", "bridge", "synthetic"} ||
      (target.access != Access{"protected"} &&
       target.access != Access{"protected", "final"}) ||
      target.instructions.empty() ||
      (target.declared_throws && !target.declared_throws->empty()))
    return false;
  const auto preservesSourceOverrides = [&] {
    if (has(owner->second.access, "final"))
      return true;
    // Widening this target must not make an input descendant's protected
    // override illegal Java. External compiled overrides retain their original
    // virtual slots and access metadata.
    for (const auto &[name, child] : classes) {
      budget.tick();
      if (child.external || name == owner->first)
        continue;
      const bool redeclares = std::any_of(
          child.methods.begin(), child.methods.end(),
          [&](const Method &method) {
            budget.tick();
            return method.reference.name == target.reference.name &&
                   method.reference.parameters == target.reference.parameters;
          });
      if (redeclares &&
          isAssignableReference(name, owner->first, classes, budget))
        return false;
    }
    return true;
  };
  const auto narrowedReferenceParameter = [&](const std::string &typed,
                                              const std::string &erased) {
    return (typed.starts_with('L') || typed.starts_with('[')) &&
           erased.starts_with('L') &&
           (!typed.starts_with('L') || classes.contains(typed)) &&
           isAssignableReference(typed, erased, classes, budget);
  };
  if (target.generic_signature) {
    if (signatures &&
        (has(owner->second.access, "final") ||
         (has(owner->second.access, "abstract") &&
          preservesSourceOverrides())) &&
        independentSourceScope(owner->second) &&
        (target.reference.returns == bridge.reference.returns ||
         ((target.reference.returns.starts_with('L') ||
           target.reference.returns.starts_with('[')) &&
          bridge.reference.returns.starts_with('L') &&
          isAssignableReference(target.reference.returns,
                                bridge.reference.returns, classes, budget))) &&
        target.reference.parameters.size() <= 8 &&
        target.reference.parameters.size() ==
            bridge.reference.parameters.size()) {
      const auto known = signatures->methods.find(target.reference);
      if (known == signatures->methods.end())
        return false;
      const auto &actual = known->second;
      const auto nested =
          signatures->nested_return_bridges.find(bridge.reference);
      const bool checked_nested_result =
          nested != signatures->nested_return_bridges.end() &&
          nested->second == target.reference &&
          actual.source_projections.size() == 1 &&
          actual.source_projections[0].kind ==
              "inherited-nested-return-variance";
      if (!actual.result || !actual.type_parameters.empty() ||
          !actual.throws_types.empty() ||
          (!actual.source_projections.empty() && !checked_nested_result) ||
          actual.parameters.size() != target.reference.parameters.size() ||
          actual.types.at(*actual.result).erasure != target.reference.returns)
        return false;
      // Only original class-owned variables are available to this source
      // owner's access projection. Its consumer separately compares the full
      // inherited parameters and result, and the common verifier proves every
      // cast, unchanged incoming argument and original returned value.
      for (const auto &type : actual.types) {
        budget.tick();
        if (type.kind == GenericType::Kind::TypeVariable &&
            type.variable_owner != owner->first)
          return false;
      }
      bool adapted = target.reference.returns != bridge.reference.returns;
      for (size_t i = 0; i < actual.parameters.size(); ++i) {
        budget.tick();
        const auto &typed = target.reference.parameters[i];
        const auto &erased = bridge.reference.parameters[i];
        if (actual.types.at(actual.parameters[i]).erasure != typed)
          return false;
        if (erased == typed)
          continue;
        if (!narrowedReferenceParameter(typed, erased))
          return false;
        adapted = true;
      }
      return adapted;
    }
    if (!signatures || !has(owner->second.access, "final") ||
        bridge.reference.parameters != target.reference.parameters ||
        !target.reference.returns.starts_with('L') ||
        !bridge.reference.returns.starts_with('L') ||
        target.reference.returns == bridge.reference.returns ||
        !isAssignableReference(target.reference.returns,
                               bridge.reference.returns, classes, budget))
      return false;
    const auto known = signatures->methods.find(target.reference);
    if (known == signatures->methods.end())
      return false;
    const auto &actual = known->second;
    // The common resolver owns this complete, unprojected closed signature.
    // Full inherited parameters/results are checked by the contract consumer;
    // this predicate owns only the access needed by its original public bridge.
    if (!actual.result || !actual.type_parameters.empty() ||
        !actual.throws_types.empty() || !actual.source_projections.empty() ||
        actual.parameters.size() != target.reference.parameters.size() ||
        std::any_of(actual.types.begin(), actual.types.end(),
                    [&](const auto &type) {
                      budget.tick();
                      return type.kind == GenericType::Kind::TypeVariable;
                    }))
      return false;
    for (size_t i = 0; i < actual.parameters.size(); ++i)
      if (actual.types.at(actual.parameters[i]).erasure !=
          target.reference.parameters[i])
        return false;
    const auto &result = actual.types.at(*actual.result);
    return result.kind == GenericType::Kind::Class &&
           result.erasure == target.reference.returns &&
           result.segments.size() == 1 &&
           !result.segments.front().arguments.empty();
  }
  if (has(owner->second.access, "final") &&
      independentSourceScope(owner->second) &&
      target.reference.returns == "V" && bridge.reference.returns == "V" &&
      target.reference.parameters.size() > 1 &&
      target.reference.parameters.size() <= 8 &&
      target.reference.parameters.size() ==
          bridge.reference.parameters.size()) {
    // A bounded class formal may have an erased reference slot other than
    // Object. Keep every unchanged scalar/reference position and prove each
    // narrowed nominal or array parameter against that original bound. Complete
    // parent substitution and register-origin verification remain separate
    // mandatory consumers of this source access admission.
    bool adapted = false;
    for (size_t i = 0; i < target.reference.parameters.size(); ++i) {
      budget.tick();
      const auto &typed = target.reference.parameters[i];
      const auto &erased = bridge.reference.parameters[i];
      if (typed == erased)
        continue;
      if (!narrowedReferenceParameter(typed, erased))
        return false;
      adapted = true;
    }
    return adapted;
  }
  const bool reference_result =
      bridge.reference.parameters == target.reference.parameters &&
      target.reference.parameters.size() <= 8 &&
      (target.reference.returns.starts_with('L') ||
       target.reference.returns.starts_with('[')) &&
      bridge.reference.returns.starts_with('L') &&
      target.reference.returns != bridge.reference.returns &&
      isAssignableReference(target.reference.returns, bridge.reference.returns,
                            classes, budget);
  const bool array_result =
      bridge.reference.parameters == std::vector<std::string>{"I"} &&
      target.reference.parameters == bridge.reference.parameters &&
      target.reference.returns.starts_with("[L") &&
      bridge.reference.returns.starts_with("[L") &&
      target.reference.returns != bridge.reference.returns &&
      isAssignableReference(target.reference.returns, bridge.reference.returns,
                            classes, budget);
  const bool array_reference_result =
      target.reference.parameters.size() == 1 &&
      target.reference.parameters.front().starts_with('[') &&
      bridge.reference.parameters ==
          std::vector<std::string>{"Ljava/lang/Object;"} &&
      target.reference.returns.starts_with('L') &&
      target.reference.returns != bridge.reference.returns &&
      bridge.reference.returns == "Ljava/lang/Object;" &&
      classes.contains(target.reference.returns) &&
      independentSourceScope(owner->second) &&
      isAssignableReference(target.reference.returns, bridge.reference.returns,
                            classes, budget);
  const bool scalar_array =
      bridge.reference.parameters ==
          std::vector<std::string>{"Ljava/lang/Object;"} &&
      target.reference.parameters.size() == 1 &&
      target.reference.parameters[0].size() == 2 &&
      target.reference.parameters[0][0] == '[' &&
      std::string("ZBSCIFJD").find(target.reference.parameters[0][1]) !=
          std::string::npos &&
      bridge.reference.returns == "I" && target.reference.returns == "I";
  const bool void_consumer =
      bridge.reference.parameters ==
          std::vector<std::string>{"Ljava/lang/Object;"} &&
      target.reference.parameters.size() == 1 &&
      target.reference.parameters[0].starts_with('L') &&
      target.reference.parameters[0] != "Ljava/lang/Object;" &&
      classes.contains(target.reference.parameters[0]) &&
      bridge.reference.returns == "V" && target.reference.returns == "V" &&
      independentSourceScope(owner->second);
  const bool scoped_target =
      (reference_result || array_result || void_consumer) &&
      independentSourceScope(owner->second) && preservesSourceOverrides();
  return (has(owner->second.access, "final") &&
          (reference_result || scalar_array || array_reference_result)) ||
         scoped_target;
}
} // namespace

std::map<MethodRef, ClosedReferenceGetterBridge>
planClosedReferenceGetterBridges(const ClassMap &classes,
                                 const GenericSignaturePlans &signatures,
                                 Budget &budget) {
  std::map<MethodRef, ClosedReferenceGetterBridge> result;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external || has(cls.access, "interface") ||
        !independentSourceScope(cls))
      continue;
    for (const auto &bridge : cls.methods) {
      budget.tick();
      if ((bridge.access != Access{"public", "bridge", "synthetic"} &&
           bridge.access != Access{"public", "final", "bridge", "synthetic"}) ||
          !bridge.reference.parameters.empty() || bridge.generic_signature ||
          objectSourceMethodNames().contains(bridge.reference.name) ||
          bridge.annotation_default ||
          (bridge.declared_throws && !bridge.declared_throws->empty()))
        continue;
      const auto reference = referenceGetterCall(cls, bridge, budget);
      if (!reference)
        continue;
      const auto *target = findMethod(cls, *reference);
      const auto full = signatures.methods.find(*reference);
      if (!target || !has(target->access, "public") ||
          has(target->access, "static") || has(target->access, "abstract") ||
          has(target->access, "bridge") || has(target->access, "native") ||
          !target->generic_signature || full == signatures.methods.end() ||
          !full->second.result || !full->second.parameters.empty() ||
          !full->second.type_parameters.empty() ||
          !full->second.throws_types.empty() ||
          !full->second.source_projections.empty() ||
          (target->declared_throws && !target->declared_throws->empty()) ||
          std::any_of(full->second.types.begin(), full->second.types.end(),
                      [](const auto &type) {
                        return type.kind == GenericType::Kind::TypeVariable;
                      }))
        continue;
      const auto contract = substitute(full->second, *full->second.result, {},
                                       name, budget, 0, true);
      if (!contract.parameterized || contract.open ||
          contract.descriptor != bridge.reference.returns)
        continue;
      result.emplace(bridge.reference,
                     ClosedReferenceGetterBridge{*reference, full->second});
    }
  }
  return result;
}

// Validate the complete own-formal relation once for the bridge and every
// original ancestor. Raw source calls can infer Object without adding any
// runtime check; no class formal or narrower carrier bound supplies a binding.
static bool nonReturningKeySeedContract(const MethodRef &ref,
                                        const GenericSignature &full,
                                        const GenericSignaturePlans &signatures,
                                        const ClassMap &classes,
                                        Budget &budget) {
  auto objectFormal = [](const GenericSignature &shape,
                         const GenericTypeParameter &formal) {
    if (!formal.class_bound || !formal.interface_bounds.empty() ||
        (!formal.erasure.empty() && formal.erasure != "Ljava/lang/Object;"))
      return false;
    const auto &bound = shape.types.at(*formal.class_bound);
    // A dependency formal's erasure cache is lazy. Its complete plain Object
    // bound is authoritative even before a call needs to populate that cache.
    return bound.kind == GenericType::Kind::Class &&
           bound.erasure == "Ljava/lang/Object;" &&
           bound.segments.size() == 1 && bound.segments[0].arguments.empty();
  };
  if (full.type_parameters.size() != 1 || full.parameters.size() != 2 ||
      ref.parameters.size() != 2 || ref.parameters[1] != "Ljava/lang/Object;" ||
      !full.result || !full.throws_types.empty() ||
      !full.source_projections.empty() ||
      !objectFormal(full, full.type_parameters[0]) ||
      sourceMethodSignature(ref, signatures) != &full)
    return false;
  auto own = [&](const GenericType &type) {
    return type.kind == GenericType::Kind::TypeVariable &&
           type.variable_owner == ref.identity() &&
           type.variable_name == full.type_parameters[0].name &&
           type.erasure == "Ljava/lang/Object;";
  };
  const auto &key = full.types.at(full.parameters[0]);
  const auto &seed = full.types.at(full.parameters[1]);
  const auto &result = full.types.at(*full.result);
  if (key.kind != GenericType::Kind::Class || key.segments.size() != 1 ||
      key.segments[0].arguments.size() != 1 || !own(seed) ||
      key.erasure != ref.parameters[0] || result.erasure != ref.returns ||
      (ref.returns == "V" ? result.kind != GenericType::Kind::Void
                          : ref.returns != "Ljava/lang/Void;" ||
                                result.kind != GenericType::Kind::Class ||
                                result.segments.size() != 1 ||
                                !result.segments[0].arguments.empty()))
    return false;
  const auto &argument = key.segments[0].arguments[0];
  if (argument.variance != GenericTypeArgument::Variance::Invariant ||
      !argument.type || !own(full.types.at(*argument.type)))
    return false;
  for (const auto &type : full.types) {
    budget.tick();
    if (type.kind == GenericType::Kind::TypeVariable && !own(type))
      return false;
  }
  const auto carrier = classes.find(key.erasure);
  const auto scope = signatures.classes.find(key.erasure);
  return carrier != classes.end() && has(carrier->second.access, "public") &&
         !has(carrier->second.access, "enum") &&
         !has(carrier->second.access, "annotation") &&
         independentSourceScope(carrier->second) &&
         !carrier->second.enclosing && !carrier->second.inner_class_present &&
         scope != signatures.classes.end() &&
         scope->second.source_projections.empty() &&
         scope->second.type_parameters.size() == 1 &&
         objectFormal(scope->second, scope->second.type_parameters[0]);
}

std::map<MethodRef, NonReturningVoidBridge>
planNonReturningVoidBridges(const ClassMap &classes,
                            const GenericSignaturePlans &signatures,
                            Budget &budget) {
  std::map<MethodRef, NonReturningVoidBridge> result;
  for (const auto &[name, cls] : classes) {
    if (cls.external || !has(cls.access, "final") ||
        has(cls.access, "interface"))
      continue;
    for (const auto &bridge : cls.methods) {
      budget.tick();
      if (bridge.reference.returns != "V" || !has(bridge.access, "bridge"))
        continue;
      auto typed = bridge.reference;
      typed.returns = "Ljava/lang/Void;";
      const auto *target = findMethod(cls, typed);
      if (!target ||
          !nonReturningVoidBridge(cls, *target, bridge, classes, budget))
        continue;
      NonReturningVoidBridge proof{typed};
      if (const auto signature = signatures.methods.find(typed);
          signature != signatures.methods.end()) {
        const auto &full = signature->second;
        if (!full.result || !full.throws_types.empty() ||
            !full.source_projections.empty() ||
            full.parameters.size() != typed.parameters.size())
          continue;
        if (!full.type_parameters.empty()) {
          if (cls.access != Access{"public", "final"} ||
              cls.generic_signature || !independentSourceScope(cls) ||
              !nonReturningKeySeedContract(typed, full, signatures, classes,
                                           budget))
            continue;
          proof.erased_method_formals = true;
        } else {
          proof.source_signature = full;
          auto &source_result = proof.source_signature->types.at(*full.result);
          source_result = GenericType{};
          source_result.kind = GenericType::Kind::Void;
          source_result.erasure = "V";
        }
      } else if (target->generic_signature)
        continue;
      // Naming the typed sibling must not split an inherited reference slot.
      // Every known original ancestor participates, including dependencies;
      // complete void parameter contracts are checked by the common planner.
      std::map<std::string, unsigned> states;
      std::function<bool(const std::string &, unsigned)> visit =
          [&](const std::string &ancestor, unsigned depth) {
            budget.tick();
            if (ancestor == "Ljava/lang/Object;")
              return !objectSourceMethodNames().contains(typed.name);
            const auto found = classes.find(ancestor);
            if (depth > 64 || states.size() > 128 || found == classes.end())
              return false;
            auto &state = states[ancestor];
            if (state)
              return state == 2;
            state = 1;
            if (proof.erased_method_formals &&
                (found->second.generic_signature ||
                 !independentSourceScope(found->second)))
              return false;
            for (const auto &member : found->second.methods) {
              budget.tick();
              if (member.reference.name == typed.name &&
                  !has(member.access, "private") &&
                  !inaccessiblePackageMethod(found->second, member, cls) &&
                  member.reference.returns != "V")
                return false;
              if (proof.erased_method_formals &&
                  member.reference.name == typed.name &&
                  member.reference.parameters == typed.parameters &&
                  !has(member.access, "private") &&
                  !inaccessiblePackageMethod(found->second, member, cls)) {
                if (has(member.access, "static") || has(member.access, "final"))
                  return false;
                if (member.generic_signature) {
                  const auto contract =
                      signatures.methods.find(member.reference);
                  if (contract == signatures.methods.end() ||
                      !nonReturningKeySeedContract(member.reference,
                                                   contract->second, signatures,
                                                   classes, budget))
                    return false;
                }
                proof.erased_contracts.insert(member.reference);
              }
            }
            if (found->second.superclass &&
                !visit(*found->second.superclass, depth + 1))
              return false;
            for (const auto &edge : found->second.interfaces)
              if (!visit(edge, depth + 1))
                return false;
            state = 2;
            return true;
          };
      bool known = cls.superclass && visit(*cls.superclass, 0);
      for (const auto &edge : cls.interfaces)
        known &= visit(edge, 0);
      if (known)
        result.emplace(bridge.reference, std::move(proof));
    }
  }
  return result;
}

std::optional<PrimitiveBridgeConversion>
primitiveBridgeConversion(const std::string &primitive,
                          const ClassMap &classes) {
  const auto conversion = primitiveUnitConversion(primitive);
  if (!conversion || !knownPrimitiveBridgeConversion(*conversion, classes))
    return {};
  return conversion;
}

namespace {
bool scalarCarrierStorage(std::string_view type) {
  // Input conversion, output factory and inherited target selection must agree
  // on the exact scalar descriptor. Width alone cannot identify its semantics.
  return type.size() == 1 &&
         std::string_view("ZBCSIJFD").find(type[0]) != std::string_view::npos;
}

bool knownScalarCarrierOwner(const std::string &name, const ClassMap &classes,
                             const GenericSignaturePlans &signatures) {
  const auto owner = classes.find(name);
  const auto current = signatures.classes.find(name);
  const auto original = signatures.inheritance_contracts.find(name);
  // Source interface spelling may already have a validated projection. The
  // original closed class declaration still owns this unchanged scalar call.
  const auto *shape = original != signatures.inheritance_contracts.end()
                          ? &original->second
                      : current != signatures.classes.end() ? &current->second
                                                            : nullptr;
  if (owner == classes.end() || !has(owner->second.access, "public") ||
      !has(owner->second.access, "final") ||
      has(owner->second.access, "interface") ||
      owner->second.superclass != "Ljava/lang/Object;" ||
      !independentSourceScope(owner->second) ||
      (shape &&
       (!shape->type_parameters.empty() || !shape->source_projections.empty())))
    return false;
  return true;
}

bool scalarCarrierBridgeFactory(const MethodRef &box, const ClassMap &classes,
                                const GenericSignaturePlans &signatures) {
  if (!knownScalarCarrierOwner(box.owner, classes, signatures) ||
      box.returns != box.owner || box.parameters.size() != 1 ||
      !scalarCarrierStorage(box.parameters[0]))
    return false;
  const auto *method = findMethod(classes.at(box.owner), box);
  return method && has(method->access, "public") &&
         has(method->access, "static") && !has(method->access, "abstract") &&
         !has(method->access, "native") && !has(method->access, "bridge") &&
         !method->generic_signature && !signatures.methods.contains(box) &&
         (!method->declared_throws || method->declared_throws->empty());
}
} // namespace

namespace {
std::optional<PrimitiveBridgeConversion>
carrierBridgeConversion(const MethodRef &unbox, const ClassMap &classes,
                        const GenericSignaturePlans &signatures,
                        bool array_storage) {
  bool storage = !array_storage && scalarCarrierStorage(unbox.returns);
  if (array_storage && unbox.returns.starts_with('[')) {
    std::string_view component = unbox.returns;
    while (component.starts_with('['))
      component.remove_prefix(1);
    storage = (component.size() == 1 &&
               std::string_view("ZBCSIJFD").find(component[0]) !=
                   std::string_view::npos) ||
              classes.contains(std::string(component));
  }
  if (!knownScalarCarrierOwner(unbox.owner, classes, signatures) ||
      !unbox.parameters.empty() || !storage)
    return {};
  const auto *method = findMethod(classes.at(unbox.owner), unbox);
  if (!method || !has(method->access, "public") ||
      has(method->access, "static") || has(method->access, "abstract") ||
      has(method->access, "native") || has(method->access, "bridge") ||
      method->generic_signature || signatures.methods.contains(unbox) ||
      (method->declared_throws && !method->declared_throws->empty()))
    return {};
  return PrimitiveBridgeConversion{unbox.owner, unbox.owner, unbox};
}
} // namespace

std::optional<PrimitiveBridgeConversion>
scalarCarrierBridgeConversion(const MethodRef &unbox, const ClassMap &classes,
                              const GenericSignaturePlans &signatures) {
  return carrierBridgeConversion(unbox, classes, signatures, false);
}
std::optional<PrimitiveBridgeConversion>
arrayCarrierBridgeConversion(const MethodRef &unbox, const ClassMap &classes,
                             const GenericSignaturePlans &signatures) {
  return carrierBridgeConversion(unbox, classes, signatures, true);
}

std::optional<std::string>
primitiveBridgeBoxedType(const std::string &primitive,
                         const ClassMap &classes) {
  const auto conversion = primitiveBridgeConversion(primitive, classes);
  return conversion ? std::optional<std::string>{conversion->boxed}
                    : std::nullopt;
}

// This exact allocation/string/constructor/throw uses only local registers.
bool parameterIndependentTerminalThrow(const Method &method, Budget &budget) {
  budget.tick();
  if (!has(method.access, "public") || has(method.access, "static") ||
      has(method.access, "abstract") || has(method.access, "native") ||
      has(method.access, "synchronized") || !method.tries.empty() ||
      method.instructions.size() != 4 || method.registers > 65535 ||
      method.registers < method.incomingWords() ||
      (method.declared_throws && !method.declared_throws->empty()))
    return false;
  const auto &ops = method.instructions;
  const auto *allocated = std::get_if<std::string>(&ops[0].reference);
  const auto *text = std::get_if<std::string>(&ops[1].literal);
  const auto *constructor = std::get_if<MethodRef>(&ops[2].reference);
  const unsigned local_end = method.registers - method.incomingWords();
  if (ops[0].opcode != "new-instance" || !allocated ||
      *allocated != "Ljava/lang/UnsupportedOperationException;" ||
      ops[0].registers.size() != 1 || ops[0].registers[0] >= local_end ||
      (ops[1].opcode != "const-string" &&
       ops[1].opcode != "const-string/jumbo") ||
      !text || ops[1].registers.size() != 1 ||
      ops[1].registers[0] >= local_end ||
      ops[0].registers == ops[1].registers ||
      (ops[2].opcode != "invoke-direct" &&
       ops[2].opcode != "invoke-direct/range") ||
      !constructor ||
      *constructor !=
          MethodRef{*allocated, "<init>", {"Ljava/lang/String;"}, "V"} ||
      ops[2].registers !=
          std::vector<unsigned>{ops[0].registers[0], ops[1].registers[0]} ||
      ops[3].opcode != "throw" || ops[3].registers != ops[0].registers)
    return false;
  for (size_t i = 0; i < ops.size(); ++i) {
    budget.tick();
    const auto &op = ops[i];
    if ((i && op.pc <= ops[i - 1].pc) || op.pc >= method.code_end ||
        op.target || !op.targets.empty() || !op.keys.empty() ||
        !op.data.empty() || op.element_width ||
        (i != 1 && !std::holds_alternative<std::monostate>(op.literal)) ||
        ((i == 1 || i == 3) &&
         !std::holds_alternative<std::monostate>(op.reference)))
      return false;
  }
  return true;
}

bool identicalTerminalThrowBridge(const Method &bridge, const Method &target,
                                  Budget &budget) {
  budget.tick();
  if (bridge.access != Access{"public", "bridge", "synthetic"} ||
      !has(target.access, "public") || has(target.access, "static") ||
      has(target.access, "abstract") || has(target.access, "native") ||
      has(target.access, "bridge") || has(target.access, "synchronized") ||
      bridge.generic_signature || bridge.annotation_default ||
      !bridge.tries.empty() || !target.tries.empty() ||
      (bridge.declared_throws && !bridge.declared_throws->empty()) ||
      (target.declared_throws && !target.declared_throws->empty()) ||
      bridge.reference.owner != target.reference.owner ||
      bridge.reference.name != target.reference.name ||
      bridge.reference.parameters.size() > 2 ||
      target.reference.parameters.size() !=
          bridge.reference.parameters.size() ||
      (target.reference.parameters == bridge.reference.parameters &&
       target.reference.returns == bridge.reference.returns) ||
      (target.reference.returns != bridge.reference.returns &&
       (bridge.reference.returns != "Ljava/lang/Object;" ||
        !target.reference.returns.starts_with('L'))) ||
      bridge.registers < 3 || bridge.registers > 65535 ||
      bridge.registers < bridge.incomingWords() ||
      target.registers != bridge.registers ||
      bridge.code_end != target.code_end || bridge.instructions.size() != 4 ||
      target.instructions.size() != 4)
    return false;
  for (size_t i = 0; i < bridge.reference.parameters.size(); ++i) {
    budget.tick();
    const auto &erased = bridge.reference.parameters[i];
    const auto &typed = target.reference.parameters[i];
    if (erased != typed &&
        (erased != "Ljava/lang/Object;" || !typed.starts_with('L')))
      return false;
  }
  if (!parameterIndependentTerminalThrow(bridge, budget) ||
      !parameterIndependentTerminalThrow(target, budget))
    return false;
  const auto &ops = bridge.instructions;
  for (size_t i = 0; i < ops.size(); ++i) {
    budget.tick();
    const auto &a = ops[i], &b = target.instructions[i];
    if (a.opcode != b.opcode || a.pc != b.pc || a.registers != b.registers ||
        a.literal != b.literal || a.reference != b.reference || a.target ||
        b.target || !a.targets.empty() || !b.targets.empty() ||
        !a.keys.empty() || !b.keys.empty() || !a.data.empty() ||
        !b.data.empty() || a.element_width || b.element_width ||
        a.pc >= bridge.code_end || (i && a.pc <= ops[i - 1].pc) ||
        (i != 1 && !std::holds_alternative<std::monostate>(a.literal)) ||
        ((i == 1 || i == 3) &&
         !std::holds_alternative<std::monostate>(a.reference)))
      return false;
  }
  return true;
}

bool packageForwardingBridge(const Method &bridge, const Method &target,
                             const ClassMap &classes, Budget &budget) {
  budget.tick();
  const auto owner = classes.find(target.reference.owner);
  return owner != classes.end() && !has(owner->second.access, "interface") &&
         bridge.reference.owner == target.reference.owner &&
         bridge.access == Access{"bridge", "synthetic"} &&
         std::none_of(target.access.begin(), target.access.end(),
                      [](const auto &flag) {
                        return flag == "public" || flag == "protected" ||
                               flag == "private" || flag == "abstract" ||
                               flag == "native" || flag == "static" ||
                               flag == "bridge" || flag == "constructor";
                      });
}

// Prove checked forwarding by register origin, never by a name or bridge flag.
void verifyGenericBridge(const Method &bridge, const Method &target,
                         Budget &budget, const FieldRef *result_field,
                         const std::set<size_t> *integer_parameters,
                         const ClassMap *classes,
                         const GenericSignaturePlans *signatures,
                         const Method *inherited_contract,
                         const PrimitiveBridgeConversion *carrier_conversion,
                         const MethodRef *result_factory) {
  auto fail = [&](const std::string &why) {
    unsupported(bridge.reference.identity(), why);
  };
  const bool public_protected_target =
      classes &&
      publicErasedProtectedTarget(bridge, target, *classes, budget, signatures);
  const bool array_carrier_forwarding =
      carrier_conversion &&
      carrier_conversion->unbox.returns.starts_with('[') && classes &&
      classes->contains(target.reference.owner) &&
      has(classes->at(target.reference.owner).access, "final") &&
      bridge.reference.owner == target.reference.owner &&
      bridge.access == Access{"public", "bridge", "synthetic"} &&
      has(target.access, "protected") && !has(target.access, "private") &&
      !has(target.access, "public") && !has(target.access, "abstract") &&
      !has(target.access, "native") && !has(target.access, "bridge");
  const bool protected_forwarding =
      classes && classes->contains(target.reference.owner) &&
      !has(classes->at(target.reference.owner).access, "interface") &&
      bridge.reference.owner == target.reference.owner &&
      bridge.access == Access{"protected", "bridge", "synthetic"} &&
      has(target.access, "protected") && !has(target.access, "public") &&
      !has(target.access, "private") && !has(target.access, "abstract") &&
      !has(target.access, "native") && !has(target.access, "bridge");
  const bool package_forwarding =
      classes && packageForwardingBridge(bridge, target, *classes, budget);
  if ((bridge.access != Access{"public", "bridge", "synthetic"} &&
       !protected_forwarding && !package_forwarding) ||
      (!has(target.access, "public") && !public_protected_target &&
       !protected_forwarding && !package_forwarding &&
       !array_carrier_forwarding) ||
      has(target.access, "static") || !bridge.tries.empty() ||
      bridge.instructions.empty() || bridge.generic_signature ||
      bridge.annotation_default || !bridge.annotations.empty() ||
      !bridge.parameter_annotations.empty() ||
      !target.parameter_annotations.empty() ||
      target.deprecated != bridge.deprecated || target.suppress_lint ||
      !originalInheritedBridgeThrows(bridge, target, signatures,
                                     inherited_contract) ||
      bridge.suppress_lint || bridge.registers < bridge.incomingWords() ||
      bridge.registers > 65535)
    fail("generic bridge needs a plain public, protected or package synthetic "
         "forwarding body");
  if (result_field &&
      (target.reference.returns != "V" ||
       bridge.reference.returns != "Ljava/lang/Object;" ||
       *result_field != FieldRef{"Lkotlin/Unit;", "INSTANCE", "Lkotlin/Unit;"}))
    fail("void Unit bridge requires its exact singleton result contract");
  if (result_factory &&
      (!classes || !signatures || result_field || integer_parameters ||
       carrier_conversion ||
       !scalarCarrierBridgeFactory(*result_factory, *classes, *signatures) ||
       result_factory->parameters !=
           std::vector<std::string>{target.reference.returns} ||
       bridge.reference.returns != "Ljava/lang/Object;"))
    fail("scalar result bridge needs its exact known factory contract");
  if (carrier_conversion) {
    const auto known =
        classes && signatures
            ? carrierBridgeConversion(
                  carrier_conversion->unbox, *classes, *signatures,
                  carrier_conversion->unbox.returns.starts_with('['))
            : std::nullopt;
    if (!integer_parameters || !known ||
        known->boxed != carrier_conversion->boxed ||
        known->checked != carrier_conversion->checked)
      fail("carrier bridge needs its exact known scalar conversion");
  }
  auto conversion_for = [&](const std::string &primitive)
      -> std::optional<PrimitiveBridgeConversion> {
    if (!carrier_conversion)
      return primitiveUnitConversion(primitive);
    return carrier_conversion->unbox.returns == primitive
               ? std::optional<PrimitiveBridgeConversion>{*carrier_conversion}
               : std::nullopt;
  };
  if (integer_parameters) {
    if (integer_parameters->empty())
      fail("primitive bridge needs an exact conversion parameter set");
    for (auto index : *integer_parameters)
      if (index >= target.reference.parameters.size() ||
          index >= bridge.reference.parameters.size() ||
          !conversion_for(target.reference.parameters[index]) ||
          (bridge.reference.parameters[index] != "Ljava/lang/Object;" &&
           (!inherited_contract || !classes ||
            index >= inherited_contract->reference.parameters.size() ||
            bridge.reference.parameters[index] !=
                inherited_contract->reference.parameters[index] ||
            !isAssignableReference(
                conversion_for(target.reference.parameters[index])->boxed,
                bridge.reference.parameters[index], *classes, budget))))
        fail("integer bridge parameter does not have its exact ABI");
  }
  const bool interface_target =
      classes && classes->contains(target.reference.owner) &&
      has(classes->at(target.reference.owner).access, "interface") &&
      bridge.reference.owner == target.reference.owner;
  struct Value {
    int origin = -1; // -1 uninitialized, -2 self, -3 invocation result
    std::string type;
    bool high = false;
  };
  std::vector<Value> registers(bridge.registers);
  unsigned cursor = bridge.registers - bridge.incomingWords();
  registers[cursor++] = {-2, bridge.reference.owner};
  for (size_t i = 0; i < bridge.reference.parameters.size(); ++i) {
    auto type = bridge.reference.parameters[i];
    registers[cursor++] = {int(i), type};
    if (width(type) == 2)
      registers[cursor++] = {int(i), type, true};
  }
  auto reference = [](const std::string &type) {
    return type.starts_with('L') || type.starts_with('[');
  };
  bool invoked = false, moved_result = false, returned = false,
       result_cast = false, factory_invoked = false, factory_result = false;
  std::optional<Value> integer_result;
  std::set<size_t> unboxed;
  std::set<int> casts;
  int last_cast = -1;
  uint32_t previous = 0;
  for (size_t index = 0; index < bridge.instructions.size(); ++index) {
    budget.tick();
    const auto &op = bridge.instructions[index];
    if ((index && op.pc <= previous) || op.pc >= bridge.code_end || op.target ||
        !op.targets.empty() || !op.keys.empty() || !op.data.empty() ||
        !std::holds_alternative<std::monostate>(op.literal))
      fail("generic bridge has invalid instruction boundaries or branches");
    previous = op.pc;
    if ((result_factory || carrier_conversion) &&
        (op.element_width ||
         ((op.opcode == "nop" || op.opcode.starts_with("move") ||
           op.opcode.starts_with("return")) &&
          !std::holds_alternative<std::monostate>(op.reference))))
      fail("scalar result bridge has unexpected instruction payload");
    if (returned)
      fail("generic bridge has instructions after return");
    for (auto reg : op.registers)
      if (reg >= registers.size())
        fail("generic bridge register is outside its frame");
    auto arity = [&](unsigned count) {
      if (op.registers.size() != count)
        fail("generic bridge register arity disagrees");
    };
    if (factory_invoked && !factory_result) {
      arity(1);
      if (op.opcode != "move-result-object" ||
          !std::holds_alternative<std::monostate>(op.reference))
        fail("scalar factory result is not adjacent to its invocation");
      registers[op.registers[0]] = {-4, result_factory->returns};
      factory_result = true;
      continue;
    }
    if (integer_result) {
      arity(1);
      const auto wide = width(integer_result->type) == 2;
      if (op.opcode != (reference(integer_result->type) ? "move-result-object"
                        : wide                          ? "move-result-wide"
                                                        : "move-result"))
        fail(
            "integer bridge result is not adjacent to its exact unboxing call");
      registers[op.registers[0]] = *integer_result;
      if (wide) {
        const auto high = op.registers[0] + 1;
        if (high >= registers.size())
          fail("primitive bridge unboxing result exceeds its frame");
        registers[high] = {integer_result->origin, integer_result->type, true};
      }
      integer_result.reset();
      continue;
    }
    if (op.opcode == "nop") {
      arity(0);
      if (invoked && !moved_result && target.reference.returns != "V")
        fail("generic bridge result is not adjacent to its invocation");
      continue;
    }
    if (!invoked &&
        (op.opcode == "move-object" || op.opcode == "move-object/from16" ||
         op.opcode == "move-object/16")) {
      arity(2);
      auto value = registers[op.registers[1]];
      if (value.origin == -1 || value.high || !reference(value.type))
        fail("generic bridge copies an invalid reference");
      registers[op.registers[0]] = std::move(value);
    } else if (!invoked &&
               (op.opcode == "move" || op.opcode == "move/from16" ||
                op.opcode == "move/16" || op.opcode == "move-wide" ||
                op.opcode == "move-wide/from16" ||
                op.opcode == "move-wide/16")) {
      arity(2);
      const auto destination = op.registers[0], source = op.registers[1];
      const auto value = registers[source];
      const bool wide = op.opcode.starts_with("move-wide");
      if (!std::holds_alternative<std::monostate>(op.reference) ||
          value.origin < 0 || value.high || value.type.size() != 1 ||
          (wide ? (value.type != "J" && value.type != "D")
                : std::string_view("ZBSCIF").find(value.type[0]) ==
                      std::string_view::npos))
        fail("generic bridge copies an invalid primitive parameter");
      if (wide) {
        if (source + 1 >= registers.size() ||
            destination + 1 >= registers.size())
          fail("generic bridge wide copy exceeds its frame");
        const auto high = registers[source + 1];
        if (!high.high || high.origin != value.origin ||
            high.type != value.type)
          fail("generic bridge wide copy has divergent incoming words");
        // Read both original words before an overlapping copy writes either.
        registers[destination] = value;
        registers[destination + 1] = high;
      } else
        registers[destination] = value;
    } else if (!invoked && op.opcode == "check-cast") {
      arity(1);
      auto &value = registers[op.registers[0]];
      auto type = std::get_if<std::string>(&op.reference);
      const auto conversion =
          value.origin >= 0 &&
                  size_t(value.origin) < target.reference.parameters.size()
              ? conversion_for(target.reference.parameters[value.origin])
              : std::nullopt;
      const auto expected =
          integer_parameters && value.origin >= 0 &&
                  integer_parameters->contains(size_t(value.origin)) &&
                  conversion
              ? conversion->checked
              : (value.origin >= 0 && size_t(value.origin) <
                                          target.reference.parameters.size()
                     ? target.reference.parameters[value.origin]
                     : "");
      if (!type || value.origin < 0 || value.high ||
          size_t(value.origin) >= target.reference.parameters.size() ||
          !reference(value.type) || !reference(*type) || *type != expected ||
          value.type == *type || value.origin <= last_cast ||
          !casts.insert(value.origin).second)
        fail("generic bridge cast does not check its corresponding parameter");
      last_cast = value.origin;
      value.type = *type;
    } else if (!invoked && integer_parameters &&
               (op.opcode == "invoke-virtual" ||
                op.opcode == "invoke-virtual/range") &&
               std::get_if<MethodRef>(&op.reference) &&
               std::get<MethodRef>(op.reference) != target.reference) {
      arity(1);
      const auto value = registers[op.registers[0]];
      const auto conversion =
          value.origin >= 0 &&
                  size_t(value.origin) < target.reference.parameters.size()
              ? conversion_for(target.reference.parameters[value.origin])
              : std::nullopt;
      if (value.origin < 0 || value.high || !conversion ||
          !integer_parameters->contains(size_t(value.origin)) ||
          std::get<MethodRef>(op.reference) != conversion->unbox ||
          value.type != conversion->checked ||
          !unboxed.insert(size_t(value.origin)).second)
        fail(
            "integer bridge does not unbox its exact unchanged parameter once");
      integer_result = Value{value.origin, conversion->unbox.returns};
    } else if (!invoked &&
               ((!interface_target && (op.opcode == "invoke-virtual" ||
                                       op.opcode == "invoke-virtual/range")) ||
                (interface_target &&
                 (op.opcode == "invoke-interface" ||
                  op.opcode == "invoke-interface/range")))) {
      const auto *ref = std::get_if<MethodRef>(&op.reference);
      if (!ref || *ref != target.reference || op.registers.empty() ||
          registers[op.registers[0]].origin != -2)
        fail("generic bridge does not delegate to its exact virtual target");
      size_t word = 1;
      for (size_t i = 0; i < ref->parameters.size(); ++i) {
        if (word >= op.registers.size())
          fail("generic bridge delegation is missing a parameter");
        const auto &value = registers[op.registers[word]];
        if (value.origin != int(i) || value.high ||
            value.type != ref->parameters[i])
          fail("generic bridge delegation changes a parameter or its cast");
        if (width(value.type) == 2) {
          if (++word >= op.registers.size() ||
              op.registers[word] != op.registers[word - 1] + 1)
            fail("generic bridge has an invalid wide argument");
          const auto &high = registers[op.registers[word]];
          if (!high.high || high.origin != int(i) || high.type != value.type)
            fail("generic bridge has a divergent wide argument");
        }
        ++word;
      }
      if (word != op.registers.size() ||
          (integer_parameters && unboxed != *integer_parameters))
        fail("generic bridge delegation has excess arguments");
      invoked = true;
    } else if (result_field && invoked && !moved_result &&
               op.opcode == "sget-object") {
      arity(1);
      const auto *field = std::get_if<FieldRef>(&op.reference);
      if (!field || *field != *result_field)
        fail("void Unit bridge reads a different result field");
      registers[op.registers[0]] = {-3, field->type};
      moved_result = true;
    } else if (!result_field && invoked && !moved_result &&
               op.opcode.starts_with("move-result")) {
      arity(1);
      const auto &type = target.reference.returns;
      auto expected = reference(type)    ? "move-result-object"
                      : width(type) == 2 ? "move-result-wide"
                                         : "move-result";
      if (type == "V" || op.opcode != expected)
        fail("generic bridge result kind disagrees");
      auto reg = op.registers[0];
      registers[reg] = {-3, type};
      if (width(type) == 2) {
        if (reg + 1 >= registers.size())
          fail("generic bridge result exceeds its frame");
        registers[reg + 1] = {-3, type, true};
      }
      moved_result = true;
    } else if (result_factory && invoked && moved_result && !factory_invoked &&
               (op.opcode == "invoke-static" ||
                op.opcode == "invoke-static/range")) {
      const auto *ref = std::get_if<MethodRef>(&op.reference);
      const auto &type = target.reference.returns;
      arity(width(type));
      const auto low = op.registers[0];
      const auto &value = registers[low];
      if (!ref || *ref != *result_factory || value.origin != -3 || value.high ||
          value.type != type)
        fail("scalar factory does not consume the unchanged delegated result");
      if (width(type) == 2) {
        const auto high = op.registers[1];
        if (high != low + 1 || registers[high].origin != -3 ||
            !registers[high].high || registers[high].type != type)
          fail("scalar factory has divergent wide result words");
      }
      factory_invoked = true;
    } else if (!result_field && !result_factory && invoked && moved_result &&
               op.opcode == "check-cast") {
      arity(1);
      const auto *type = std::get_if<std::string>(&op.reference);
      const auto &value = registers[op.registers[0]];
      if (!type || *type != bridge.reference.returns || result_cast ||
          value.origin != -3 || value.high ||
          value.type != target.reference.returns || !classes ||
          !isAssignableReference(value.type, *type, *classes, budget))
        fail("generic bridge result cast is not a proven unchanged upcast");
      // The common nominal assignability proof guarantees this original
      // cast cannot fail, including null. Keep the target-result provenance.
      result_cast = true;
    } else if (invoked && op.opcode.starts_with("return")) {
      const auto &type = bridge.reference.returns;
      auto expected = type == "V"        ? "return-void"
                      : reference(type)  ? "return-object"
                      : width(type) == 2 ? "return-wide"
                                         : "return";
      arity(type == "V" ? 0 : 1);
      if (op.opcode != expected ||
          (type == "V" && target.reference.returns != "V"))
        fail("generic bridge return kind disagrees");
      if (type != "V") {
        const auto &value = registers[op.registers[0]];
        if (!moved_result || (result_factory && !factory_result) ||
            value.origin != (result_factory ? -4 : -3) || value.high ||
            value.type != (result_field     ? result_field->type
                           : result_factory ? result_factory->returns
                                            : target.reference.returns) ||
            (!reference(type) && value.type != type))
          fail("generic bridge does not return the delegated result");
      }
      returned = true;
    } else {
      fail("generic bridge contains effects beyond checked forwarding");
    }
  }
  if (!returned || integer_result)
    fail("generic bridge has no verified return");
}

std::map<MethodRef, IntegerUnitBridge>
planIntegerUnitBridges(const ClassMap &classes,
                       const GenericSignaturePlans &signatures,
                       Budget &budget) {
  std::map<MethodRef, IntegerUnitBridge> result;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external || !has(cls.access, "final"))
      continue;
    const auto plan = signatures.classes.find(name);
    if (plan == signatures.classes.end())
      continue;
    const auto &signature = plan->second;
    for (auto parent_id : signature.interfaces) {
      budget.tick();
      const auto &parent = signature.types.at(parent_id);
      const auto definition = classes.find(parent.erasure);
      const auto formals = signatures.classes.find(parent.erasure);
      if (parent.segments.size() != 1 || parent.segments[0].arguments.empty() ||
          definition == classes.end() || formals == signatures.classes.end() ||
          !has(definition->second.access, "interface") ||
          definition->second.superclass != "Ljava/lang/Object;" ||
          definition->second.enclosing || definition->second.enclosing_method ||
          definition->second.methods.size() != 1 ||
          !onlyEmptyMarkerParents(definition->second, classes, budget))
        continue;
      const auto &declaration = definition->second.methods[0];
      const auto method_plan = signatures.methods.find(declaration.reference);
      const auto &arguments = parent.segments[0].arguments;
      const auto &parameters = formals->second.type_parameters;
      if (declaration.access != Access{"public", "abstract"} ||
          declaration.reference.name != "invoke" ||
          declaration.reference.returns != "Ljava/lang/Object;" ||
          declaration.reference.parameters.empty() ||
          declaration.reference.parameters.size() > 22 ||
          arguments.size() != parameters.size() ||
          parameters.size() != declaration.reference.parameters.size() + 1 ||
          method_plan == signatures.methods.end() ||
          !method_plan->second.type_parameters.empty() ||
          !method_plan->second.throws_types.empty() ||
          (declaration.declared_throws &&
           !declaration.declared_throws->empty()))
        continue;
      std::map<std::string, SubstitutedType> bindings;
      bool closed = true;
      for (size_t i = 0; i < parameters.size(); ++i) {
        budget.tick();
        const auto &formal = parameters[i];
        const auto &argument = arguments[i];
        if (!formal.class_bound || !formal.interface_bounds.empty() ||
            argument.variance != GenericTypeArgument::Variance::Invariant ||
            !argument.type) {
          closed = false;
          break;
        }
        const auto &bound = formals->second.types.at(*formal.class_bound);
        const auto &actual = signature.types.at(*argument.type);
        if (bound.kind != GenericType::Kind::Class ||
            bound.package != "java/lang" || bound.segments.size() != 1 ||
            bound.segments[0].name != "Object" ||
            !bound.segments[0].arguments.empty() ||
            (actual.kind != GenericType::Kind::Class &&
             actual.kind != GenericType::Kind::TypeVariable) ||
            std::any_of(actual.segments.begin(), actual.segments.end(),
                        [](const auto &segment) {
                          return !segment.arguments.empty();
                        })) {
          closed = false;
          break;
        }
        auto type =
            substitute(signature, *argument.type, {}, name, budget, 0, true);
        if (type.parameterized) {
          closed = false;
          break;
        }
        bindings.emplace(formal.name, std::move(type));
      }
      const auto &member = method_plan->second;
      std::set<std::string> used;
      auto formalType = [&](GenericTypeId id) {
        const auto &type = member.types.at(id);
        return type.kind == GenericType::Kind::TypeVariable &&
               (!type.variable_owner ||
                *type.variable_owner == parent.erasure) &&
               bindings.contains(type.variable_name) &&
               used.insert(type.variable_name).second;
      };
      if (!closed ||
          member.parameters.size() != declaration.reference.parameters.size())
        continue;
      MethodRef specialized = declaration.reference;
      specialized.owner = name;
      specialized.parameters.clear();
      for (auto parameter : member.parameters) {
        if (!formalType(parameter)) {
          closed = false;
          break;
        }
        specialized.parameters.push_back(
            substitute(member, parameter, bindings, name, budget).descriptor);
      }
      if (!closed || !formalType(*member.result))
        continue;
      specialized.returns =
          substitute(member, *member.result, bindings, name, budget).descriptor;
      if (specialized.returns != "Lkotlin/Unit;" ||
          findMethod(cls, specialized))
        continue;
      auto target_ref = specialized;
      target_ref.returns = "V";
      std::set<size_t> integer_parameters;
      bool boolean_parameters = false, known_conversions = true;
      for (size_t i = 0; i < target_ref.parameters.size(); ++i)
        for (const std::string primitive : {"I", "Z"}) {
          const auto conversion = primitiveUnitConversion(primitive);
          if (target_ref.parameters[i] != conversion->boxed)
            continue;
          known_conversions &=
              knownPrimitiveBridgeConversion(*conversion, classes);
          boolean_parameters |= primitive == "Z";
          target_ref.parameters[i] = primitive;
          integer_parameters.insert(i);
          break;
        }
      if (integer_parameters.empty() || !known_conversions)
        continue;
      const auto *target = findMethod(cls, target_ref);
      if (!target ||
          (target->declared_throws && !target->declared_throws->empty()))
        continue;
      if (const auto target_plan = signatures.methods.find(target_ref);
          target_plan != signatures.methods.end()) {
        const auto &actual = target_plan->second;
        if (!actual.type_parameters.empty() || !actual.throws_types.empty() ||
            actual.parameters.size() != member.parameters.size())
          continue;
        bool matching =
            substitute(actual, *actual.result, {}, name, budget, 0, true)
                .signature == "V";
        for (size_t i = 0; i < actual.parameters.size(); ++i) {
          budget.tick();
          const auto expected = integer_parameters.contains(i)
                                    ? target_ref.parameters[i]
                                    : substitute(member, member.parameters[i],
                                                 bindings, name, budget)
                                          .signature;
          matching &= substitute(actual, actual.parameters[i], {}, name, budget,
                                 0, true)
                          .signature == expected;
        }
        if (!matching)
          continue;
      }
      auto erased = declaration.reference;
      erased.owner = name;
      const auto *bridge = findMethod(cls, erased);
      const auto field = unitResultField(classes, budget);
      if (!bridge || !field)
        continue;
      verifyGenericBridge(*bridge, *target, budget, &*field,
                          &integer_parameters);
      auto [entry, inserted] = result.emplace(
          erased, IntegerUnitBridge{
                      target_ref, declaration.reference, *field, parent_id,
                      originalClassType(cls, parent.offset, budget),
                      boolean_parameters});
      if (!inserted && entry->second.target != target_ref)
        unsupported(name, "integer Unit bridge maps to conflicting targets");
    }
  }
  return result;
}
namespace {
std::optional<std::pair<FieldRef, SubstitutedType>>
fixedReferenceBoxingRepresentation(const MethodRef &factory,
                                   const ClassMap &classes,
                                   const GenericSignaturePlans &signatures,
                                   Budget &budget) {
  const auto owner = classes.find(factory.owner);
  if (owner == classes.end() || owner->second.external ||
      !has(owner->second.access, "public") ||
      !has(owner->second.access, "final") ||
      has(owner->second.access, "interface") ||
      owner->second.superclass != "Ljava/lang/Object;" ||
      !independentSourceScope(owner->second) ||
      factory.returns != factory.owner || factory.parameters.size() != 1 ||
      (!factory.parameters[0].starts_with('L') &&
       !factory.parameters[0].starts_with('[')))
    return {};
  const auto *box = findMethod(owner->second, factory);
  if (!box || !has(box->access, "public") || !has(box->access, "static") ||
      !has(box->access, "final") || has(box->access, "bridge") ||
      box->generic_signature || box->annotation_default ||
      (box->declared_throws && !box->declared_throws->empty()) ||
      box->registers != 2 || !box->tries.empty() ||
      box->instructions.size() != 3)
    return {};
  const auto &code = box->instructions;
  const auto *allocated = std::get_if<std::string>(&code[0].reference);
  const auto *constructor = std::get_if<MethodRef>(&code[1].reference);
  if (code[0].opcode != "new-instance" || !allocated ||
      *allocated != factory.owner ||
      code[0].registers != std::vector<unsigned>{0} ||
      (code[1].opcode != "invoke-direct" &&
       code[1].opcode != "invoke-direct/range") ||
      !constructor ||
      *constructor !=
          MethodRef{factory.owner, "<init>", factory.parameters, "V"} ||
      code[1].registers != std::vector<unsigned>({0, 1}) ||
      code[2].opcode != "return-object" ||
      code[2].registers != std::vector<unsigned>{0} ||
      !std::holds_alternative<std::monostate>(code[2].reference))
    return {};
  const auto *init = findMethod(owner->second, *constructor);
  if (!init || !has(init->access, "private") ||
      !has(init->access, "constructor") || has(init->access, "static") ||
      init->generic_signature || init->annotation_default ||
      (init->declared_throws && !init->declared_throws->empty()) ||
      init->registers != 2 || !init->tries.empty() ||
      init->instructions.size() != 3)
    return {};
  const auto &body = init->instructions;
  const auto *super = std::get_if<MethodRef>(&body[0].reference);
  const auto *storage = std::get_if<FieldRef>(&body[1].reference);
  if ((body[0].opcode != "invoke-direct" &&
       body[0].opcode != "invoke-direct/range") ||
      !super || *super != MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"} ||
      body[0].registers != std::vector<unsigned>{0} ||
      body[1].opcode != "iput-object" || !storage ||
      storage->owner != factory.owner ||
      storage->type != factory.parameters[0] ||
      body[1].registers != std::vector<unsigned>({1, 0}) ||
      body[2].opcode != "return-void" || !body[2].registers.empty() ||
      !std::holds_alternative<std::monostate>(body[2].reference))
    return {};
  for (const auto *method : {box, init}) {
    uint32_t previous = 0;
    for (size_t i = 0; i < method->instructions.size(); ++i) {
      budget.tick();
      const auto &op = method->instructions[i];
      if ((i && op.pc <= previous) || op.pc >= method->code_end || op.target ||
          !op.targets.empty() || !op.keys.empty() || !op.data.empty() ||
          op.element_width ||
          !std::holds_alternative<std::monostate>(op.literal))
        return {};
      previous = op.pc;
    }
  }
  const auto field = std::find_if(
      owner->second.fields.begin(), owner->second.fields.end(),
      [&](const Field &field) { return field.reference == *storage; });
  if (field == owner->second.fields.end() || !has(field->access, "private") ||
      !has(field->access, "final") || has(field->access, "static"))
    return {};
  SubstitutedType representation{storage->type, storage->type};
  if (const auto generic = signatures.fields.find(*storage);
      generic != signatures.fields.end()) {
    const auto &signature = generic->second;
    if (!signature.field_type || !signature.source_projections.empty())
      return {};
    for (const auto &type : signature.types) {
      budget.tick();
      if (type.kind == GenericType::Kind::TypeVariable)
        return {}; // This bounded proof does not infer phantom carrier formals.
    }
    representation = substitute(signature, *signature.field_type, {},
                                factory.owner, budget, 0, true);
  } else if (field->generic_signature) {
    return {};
  }
  return std::pair{*storage, std::move(representation)};
}
} // namespace
bool provenStaticReceiverImplementation(const Class &cls, const Method &method,
                                        const ClassMap &classes,
                                        const GenericSignaturePlans &signatures,
                                        Budget &budget) {
  return !has(cls.access, "interface") &&
         method.access == Access{"public", "bridge"} &&
         (!method.declared_throws || method.declared_throws->empty()) &&
         plainStaticReceiverImplementation(cls, method, classes, signatures,
                                           budget);
}
std::optional<MethodRef>
provenGenericReferenceGetterBridge(const Class &cls, const Method &bridge,
                                   const GenericSignaturePlans &signatures,
                                   Budget &budget) {
  const auto plan = signatures.methods.find(bridge.reference);
  if (plan == signatures.methods.end() || !plan->second.result)
    return {};
  const auto own_result = substitute(plan->second, *plan->second.result, {},
                                     cls.name, budget, 0, true);
  return genericReferenceGetterBridge(cls, bridge, own_result, signatures,
                                      budget);
}
bool closedUnaryConsumerParents(
    const Class &root, const MethodRef &required,
    const std::map<std::string, SubstitutedType> &root_bindings,
    const SubstitutedType &expected, const std::string &context,
    const ClassMap &classes, const GenericSignaturePlans &signatures,
    Budget &budget) {
  if (expected.open || expected.parameterized)
    return false;
  using Bindings = std::map<std::string, SubstitutedType>;
  std::map<std::string, std::string> visited;
  std::set<std::string> active;
  const auto plain_marker = [&](const Class &cls) {
    const auto plan = signatures.classes.find(cls.name);
    return (plan == signatures.classes.end()
                ? !cls.generic_signature
                : plan->second.type_parameters.empty() &&
                      plan->second.source_projections.empty()) &&
           emptyMarkerHierarchy(cls.name, classes, budget);
  };
  std::function<bool(const Class &, const Bindings &, unsigned)> visit =
      [&](const Class &current, const Bindings &bindings, unsigned depth) {
        budget.tick();
        if (depth > 64 || visited.size() > 128 || active.contains(current.name))
          return false;
        if (plain_marker(current))
          return true;
        const auto own = signatures.classes.find(current.name);
        if (!has(current.access, "interface") ||
            current.superclass != "Ljava/lang/Object;" ||
            !independentSourceScope(current) || !current.fields.empty() ||
            current.methods.size() > 1 || own == signatures.classes.end() ||
            own->second.type_parameters.size() != 1 ||
            !own->second.source_projections.empty())
          return false;
        const auto &plan = own->second;
        const auto &formal = plan.type_parameters[0];
        const auto argument = bindings.find(formal.name);
        if (!formal.class_bound || !formal.interface_bounds.empty() ||
            formal.erasure != "Ljava/lang/Object;" ||
            plan.types.at(*formal.class_bound).kind !=
                GenericType::Kind::Class ||
            substitute(plan, *formal.class_bound, bindings, context, budget, 0,
                       true)
                    .signature != "Ljava/lang/Object;" ||
            argument == bindings.end() || argument->second.open ||
            argument->second.parameterized)
          return false;
        // The single full binding also matters for an unused marker formal.
        // Raw source must not hide conflicting paths to that same interface.
        const auto [seen, inserted] =
            visited.emplace(current.name, argument->second.signature);
        if (!inserted)
          return seen->second == argument->second.signature;
        active.insert(current.name);
        for (const auto &member : current.methods) {
          budget.tick();
          const auto shape = signatures.methods.find(member.reference);
          if (member.access != Access{"public", "abstract"} ||
              member.reference.name != required.name ||
              member.reference.parameters != required.parameters ||
              member.reference.returns != "V" ||
              (member.declared_throws && !member.declared_throws->empty()) ||
              shape == signatures.methods.end() ||
              !shape->second.type_parameters.empty() ||
              !shape->second.throws_types.empty() ||
              !shape->second.source_projections.empty() ||
              shape->second.parameters.size() != 1 || !shape->second.result)
            return false;
          const auto &method = shape->second;
          const auto &parameter = method.types.at(method.parameters[0]);
          if (parameter.kind != GenericType::Kind::TypeVariable ||
              parameter.variable_owner != current.name ||
              parameter.variable_name != formal.name ||
              substitute(method, method.parameters[0], bindings, context,
                         budget, 0, true)
                      .signature != expected.signature ||
              substitute(method, *method.result, bindings, context, budget, 0,
                         true)
                      .signature != "V")
            return false;
        }
        for (auto id : plan.interfaces) {
          budget.tick();
          const auto &edge = plan.types.at(id);
          const auto ancestor = classes.find(edge.erasure);
          if (ancestor == classes.end())
            return false;
          if (plain_marker(ancestor->second)) {
            if (edge.segments.size() != 1 ||
                !edge.segments[0].arguments.empty())
              return false;
            continue;
          }
          const auto parent = signatures.classes.find(edge.erasure);
          if (edge.segments.size() != 1 ||
              edge.segments[0].arguments.size() != 1 ||
              parent == signatures.classes.end() ||
              parent->second.type_parameters.size() != 1)
            return false;
          const auto &argument = edge.segments[0].arguments[0];
          if (!argument.type ||
              argument.variance != GenericTypeArgument::Variance::Invariant)
            return false;
          Bindings composed{{parent->second.type_parameters[0].name,
                             substitute(plan, *argument.type, bindings, context,
                                        budget, 0, true)}};
          if (!visit(ancestor->second, composed, depth + 1))
            return false;
        }
        active.erase(current.name);
        return true;
      };
  return visit(root, root_bindings, 0);
}

namespace {
struct ClosedInterfaceMember {
  const Method *method;
  std::vector<SubstitutedType> parameters;
  SubstitutedType result;
};
std::optional<std::vector<ClosedInterfaceMember>> closedInterfaceMembers(
    const Class &root,
    const std::map<std::string, SubstitutedType> &root_bindings,
    const ClassMap &classes, const GenericSignaturePlans &signatures,
    Budget &budget) {
  using Bindings = std::map<std::string, SubstitutedType>;
  std::vector<ClosedInterfaceMember> result;
  std::map<std::string, std::vector<std::string>> visited;
  std::set<std::string> active;
  std::function<bool(const Class &, const Bindings &, unsigned)> visit =
      [&](const Class &current, const Bindings &bindings, unsigned depth) {
        budget.tick();
        if (depth > 64 || visited.size() > 128 ||
            active.contains(current.name) ||
            !has(current.access, "interface") ||
            current.superclass != "Ljava/lang/Object;" ||
            !independentSourceScope(current) || !current.fields.empty())
          return false;
        const auto shape = signatures.classes.find(current.name);
        if (shape == signatures.classes.end())
          return bindings.empty() &&
                 emptyMarkerHierarchy(current.name, classes, budget);
        const auto &plan = shape->second;
        if (!plan.source_projections.empty() ||
            plan.type_parameters.size() != bindings.size())
          return false;
        std::vector<std::string> arguments;
        for (const auto &formal : plan.type_parameters) {
          const auto bound = bindings.find(formal.name);
          if (!formal.class_bound || !formal.interface_bounds.empty() ||
              formal.erasure != "Ljava/lang/Object;" ||
              substitute(plan, *formal.class_bound, bindings, current.name,
                         budget, 0, true)
                      .signature != "Ljava/lang/Object;" ||
              bound == bindings.end() || bound->second.open ||
              bound->second.parameterized)
            return false;
          arguments.push_back(bound->second.signature);
        }
        const auto [entry, inserted] = visited.emplace(current.name, arguments);
        if (!inserted)
          return entry->second == arguments;
        active.insert(current.name);
        for (const auto &method : current.methods) {
          budget.tick();
          if (method.access != Access{"public", "abstract"} ||
              (method.declared_throws && !method.declared_throws->empty()))
            return false;
          ClosedInterfaceMember member{
              &method,
              {},
              {method.reference.returns, method.reference.returns}};
          const auto full = signatures.methods.find(method.reference);
          if (full == signatures.methods.end()) {
            if (method.generic_signature)
              return false;
            for (const auto &parameter : method.reference.parameters)
              member.parameters.push_back({parameter, parameter});
          } else {
            const auto &signature = full->second;
            if (!signature.type_parameters.empty() ||
                !signature.throws_types.empty() ||
                !signature.source_projections.empty() || !signature.result)
              return false;
            for (auto id : signature.parameters)
              member.parameters.push_back(substitute(
                  signature, id, bindings, current.name, budget, 0, true));
            member.result = substitute(signature, *signature.result, bindings,
                                       current.name, budget, 0, true);
          }
          if (member.result.open || member.result.parameterized ||
              std::any_of(member.parameters.begin(), member.parameters.end(),
                          [](const auto &type) {
                            return type.open || type.parameterized;
                          }))
            return false;
          result.push_back(std::move(member));
        }
        for (auto id : plan.interfaces) {
          const auto &edge = plan.types.at(id);
          const auto parent = classes.find(edge.erasure);
          const auto formals = signatures.classes.find(edge.erasure);
          if (parent == classes.end() || edge.segments.size() != 1)
            return false;
          const auto &arguments = edge.segments[0].arguments;
          if (formals == signatures.classes.end()) {
            if (!arguments.empty() ||
                !emptyMarkerHierarchy(parent->second.name, classes, budget))
              return false;
            continue;
          }
          if (arguments.size() != formals->second.type_parameters.size())
            return false;
          Bindings composed;
          for (size_t i = 0; i < arguments.size(); ++i) {
            const auto &argument = arguments[i];
            if (!argument.type ||
                argument.variance != GenericTypeArgument::Variance::Invariant)
              return false;
            composed.emplace(formals->second.type_parameters[i].name,
                             substitute(plan, *argument.type, bindings,
                                        current.name, budget, 0, true));
          }
          if (!visit(parent->second, composed, depth + 1))
            return false;
        }
        active.erase(current.name);
        return true;
      };
  if (!visit(root, root_bindings, 0))
    return {};
  return result;
}
} // namespace

std::map<MethodRef, ValueCarrierBridge>
planValueCarrierBridges(const ClassMap &classes,
                        const GenericSignaturePlans &signatures,
                        Budget &budget) {
  std::map<MethodRef, ValueCarrierBridge> result;
  auto known_reference = [&](const std::string &type) {
    std::string_view component = type;
    while (component.starts_with('[')) {
      budget.tick();
      component.remove_prefix(1);
    }
    return (component.size() == 1 && component.size() < type.size() &&
            std::string_view("ZBCSIJFD").find(component[0]) !=
                std::string_view::npos) ||
           classes.contains(std::string(component));
  };
  auto declaration = [&](const MethodRef &ref, bool is_static,
                         bool allow_signature = false) {
    const auto owner = classes.find(ref.owner);
    if (owner == classes.end())
      return false;
    const auto *method = findMethod(owner->second, ref);
    return method && has(method->access, "public") &&
           has(method->access, "static") == is_static &&
           !has(method->access, "abstract") && !has(method->access, "bridge") &&
           (allow_signature || (!method->generic_signature &&
                                !signatures.methods.contains(ref))) &&
           (!method->declared_throws || method->declared_throws->empty());
  };
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external || !has(cls.access, "final"))
      continue;
    std::vector<std::pair<const GenericSignature *, const Class *>> contracts;
    if (const auto own = signatures.classes.find(name);
        own != signatures.classes.end())
      contracts.emplace_back(&own->second, &cls);
    else if (!cls.generic_signature && cls.superclass == "Ljava/lang/Object;" &&
             independentSourceScope(cls))
      for (const auto &edge : cls.interfaces) {
        budget.tick();
        const auto source = classes.find(edge);
        const auto complete = signatures.classes.find(edge);
        if (source == classes.end() || source->second.external ||
            !has(source->second.access, "interface") ||
            source->second.superclass != "Ljava/lang/Object;" ||
            !independentSourceScope(source->second) ||
            complete == signatures.classes.end() ||
            !complete->second.type_parameters.empty() ||
            !complete->second.source_projections.empty())
          continue;
        contracts.emplace_back(&complete->second, &source->second);
      }
    for (const auto &[shape, definition] : contracts) {
      const auto &owner = *shape;
      const std::optional<std::string> interface_owner =
          definition->name == name
              ? std::nullopt
              : std::optional<std::string>{definition->name};
      // A closed self carrier can implement a scalar comparison through its
      // original checked short, float or wide conversion and differently named
      // target. Keep the entire bridge under a disclosed raw source edge; no
      // typed comparison overload or conversion is synthesized.
      for (auto id : owner.interfaces) {
        budget.tick();
        const auto &edge = owner.types.at(id);
        const auto api = classes.find(edge.erasure);
        const auto formals = signatures.classes.find(edge.erasure);
        if (!owner.type_parameters.empty() ||
            !owner.source_projections.empty() || edge.segments.size() != 1 ||
            edge.segments[0].arguments.size() != 1 || api == classes.end() ||
            formals == signatures.classes.end() ||
            formals->second.type_parameters.size() != 1 ||
            !has(api->second.access, "interface") ||
            api->second.superclass != "Ljava/lang/Object;" ||
            !api->second.fields.empty() || api->second.methods.size() != 1 ||
            !onlyEmptyMarkerParents(api->second, classes, budget))
          continue;
        const auto &formal = formals->second.type_parameters[0];
        const auto &argument = edge.segments[0].arguments[0];
        const auto &member = api->second.methods[0];
        const auto contract = signatures.methods.find(member.reference);
        if (!formal.class_bound || formal.erasure != "Ljava/lang/Object;" ||
            !formal.interface_bounds.empty() || !argument.type ||
            argument.variance != GenericTypeArgument::Variance::Invariant ||
            member.access != Access{"public", "abstract"} ||
            member.reference.parameters !=
                std::vector<std::string>{"Ljava/lang/Object;"} ||
            member.reference.returns != "I" ||
            contract == signatures.methods.end() ||
            !contract->second.type_parameters.empty() ||
            !contract->second.throws_types.empty() ||
            !contract->second.source_projections.empty() ||
            contract->second.parameters.size() != 1 ||
            !contract->second.result ||
            (member.declared_throws && !member.declared_throws->empty()))
          continue;
        const auto actual =
            substitute(owner, *argument.type, {}, name, budget, 0, true);
        const auto &parameter =
            contract->second.types.at(contract->second.parameters[0]);
        if (actual.signature != name || actual.open || actual.parameterized ||
            parameter.kind != GenericType::Kind::TypeVariable ||
            parameter.variable_owner != edge.erasure ||
            parameter.variable_name != formal.name ||
            contract->second.types.at(*contract->second.result).erasure != "I")
          continue;
        auto erased = member.reference;
        erased.owner = name;
        const auto *bridge = findMethod(cls, erased);
        if (!bridge ||
            bridge->access != Access{"public", "bridge", "synthetic"} ||
            bridge->generic_signature || !bridge->tries.empty() ||
            (bridge->declared_throws && !bridge->declared_throws->empty()) ||
            bridge->registers < 3 || bridge->instructions.size() != 7)
          continue;
        const auto &ops = bridge->instructions;
        const unsigned self = bridge->registers - 2;
        const auto *cast = std::get_if<std::string>(&ops[1].reference);
        const auto *unbox = std::get_if<MethodRef>(&ops[2].reference);
        const auto *target = std::get_if<MethodRef>(&ops[4].reference);
        const bool wide =
            unbox && (unbox->returns == "J" || unbox->returns == "D");
        if ((ops[0].opcode != "move-object" &&
             ops[0].opcode != "move-object/from16" &&
             ops[0].opcode != "move-object/16") ||
            ops[0].registers.size() != 2 || ops[0].registers[0] >= self ||
            ops[0].registers[1] != self + 1 ||
            !std::holds_alternative<std::monostate>(ops[0].reference) ||
            ops[1].opcode != "check-cast" || !cast || *cast != name ||
            ops[1].registers != std::vector<unsigned>{ops[0].registers[0]} ||
            !unbox || unbox->owner != name || !unbox->parameters.empty() ||
            (unbox->returns != "S" && unbox->returns != "F" && !wide) ||
            !declaration(*unbox, false) ||
            (ops[2].opcode != "invoke-virtual" &&
             ops[2].opcode != "invoke-virtual/range") ||
            ops[2].registers != ops[1].registers ||
            ops[3].opcode != (wide ? "move-result-wide" : "move-result") ||
            ops[3].registers.size() != 1 || ops[3].registers[0] >= self ||
            (wide && self - ops[3].registers[0] < 2) ||
            !std::holds_alternative<std::monostate>(ops[3].reference) ||
            !target || target->owner != name ||
            target->name == member.reference.name ||
            target->parameters != std::vector<std::string>{unbox->returns} ||
            target->returns != "I" || !declaration(*target, false) ||
            (ops[4].opcode != "invoke-virtual" &&
             ops[4].opcode != "invoke-virtual/range") ||
            ops[4].registers !=
                (wide ? std::vector<unsigned>{self, ops[3].registers[0],
                                              ops[3].registers[0] + 1}
                      : std::vector<unsigned>{self, ops[3].registers[0]}) ||
            ops[5].opcode != "move-result" || ops[5].registers.size() != 1 ||
            ops[5].registers[0] >= self ||
            !std::holds_alternative<std::monostate>(ops[5].reference) ||
            ops[6].opcode != "return" || ops[6].registers != ops[5].registers ||
            !std::holds_alternative<std::monostate>(ops[6].reference))
          continue;
        bool exact = true;
        uint32_t previous = 0;
        for (size_t i = 0; i < ops.size(); ++i) {
          budget.tick();
          const auto &op = ops[i];
          if ((i && op.pc <= previous) || op.pc >= bridge->code_end ||
              op.target || !op.keys.empty() || !op.targets.empty() ||
              !op.data.empty() || op.element_width ||
              !std::holds_alternative<std::monostate>(op.literal))
            exact = false;
          previous = op.pc;
        }
        if (exact)
          result.emplace(
              erased, ValueCarrierBridge{
                          *target,
                          member.reference,
                          {},
                          {*unbox},
                          id,
                          originalClassType(*definition, edge.offset, budget),
                          {},
                          {},
                          {},
                          interface_owner});
      }
      // Keep the original graph when a closed scalar conversion needs a raw
      // source edge. Every sibling still goes through complete inheritance
      // and its original body proof after this member is certified.
      if (!interface_owner && !has(cls.access, "interface") &&
          cls.superclass == "Ljava/lang/Object;" &&
          independentSourceScope(cls) && owner.type_parameters.empty() &&
          owner.source_projections.empty())
        for (auto id : owner.interfaces) {
          budget.tick();
          const auto &edge = owner.types.at(id);
          const auto api = classes.find(edge.erasure);
          const auto formals = signatures.classes.find(edge.erasure);
          if (edge.segments.size() != 1 || edge.segments[0].arguments.empty() ||
              api == classes.end() || formals == signatures.classes.end() ||
              edge.segments[0].arguments.size() !=
                  formals->second.type_parameters.size())
            continue;
          std::map<std::string, SubstitutedType> bindings;
          bool exact = true;
          for (size_t i = 0; i < edge.segments[0].arguments.size(); ++i) {
            const auto &argument = edge.segments[0].arguments[i];
            if (!argument.type ||
                argument.variance != GenericTypeArgument::Variance::Invariant) {
              exact = false;
              break;
            }
            bindings.emplace(
                formals->second.type_parameters[i].name,
                substitute(owner, *argument.type, {}, name, budget, 0, true));
          }
          if (!exact)
            continue;
          const auto members = closedInterfaceMembers(
              api->second, bindings, classes, signatures, budget);
          if (!members)
            continue;
          for (const auto &contract : *members) {
            budget.tick();
            const auto &member = *contract.method;
            if (member.reference.returns == "Ljava/lang/Object;" &&
                !contract.result.open && !contract.result.parameterized &&
                contract.result.signature.starts_with('L') &&
                contract.parameters.size() <= 8) {
              auto erased = member.reference;
              erased.owner = name;
              const auto *bridge = findMethod(cls, erased);
              std::vector<std::string> parameters;
              for (const auto &type : contract.parameters)
                parameters.push_back(type.signature);
              // Fixed parameters retain their complete inherited types. Only
              // the original scalar result is converted by its known factory.
              if (!bridge || parameters != member.reference.parameters)
                continue;
              for (const auto &target : cls.methods) {
                budget.tick();
                if (target.reference.parameters != parameters ||
                    !scalarCarrierStorage(target.reference.returns) ||
                    !has(target.access, "public") ||
                    has(target.access, "static") ||
                    has(target.access, "abstract") ||
                    has(target.access, "native") ||
                    has(target.access, "bridge") || target.generic_signature ||
                    signatures.methods.contains(target.reference) ||
                    (target.declared_throws &&
                     !target.declared_throws->empty()))
                  continue;
                bool original_target = false;
                std::optional<MethodRef> factory;
                for (const auto &op : bridge->instructions) {
                  budget.tick();
                  const auto *call = std::get_if<MethodRef>(&op.reference);
                  if (!call)
                    continue;
                  if ((op.opcode == "invoke-virtual" ||
                       op.opcode == "invoke-virtual/range") &&
                      *call == target.reference)
                    original_target = true;
                  if ((op.opcode == "invoke-static" ||
                       op.opcode == "invoke-static/range") &&
                      call->returns == contract.result.signature &&
                      call->parameters ==
                          std::vector<std::string>{target.reference.returns} &&
                      scalarCarrierBridgeFactory(*call, classes, signatures))
                    factory = *call;
                }
                if (!original_target || !factory)
                  continue;
                verifyGenericBridge(*bridge, target, budget, nullptr, nullptr,
                                    &classes, &signatures, &member, nullptr,
                                    &*factory);
                ValueCarrierBridge proof{
                    target.reference,
                    member.reference,
                    factory,
                    {},
                    id,
                    originalClassType(*definition, edge.offset, budget)};
                proof.inherited_contracts.emplace(
                    member.reference,
                    InheritedCarrierContract{parameters,
                                             contract.result.signature});
                auto [entry, inserted] =
                    result.emplace(erased, std::move(proof));
                if (!inserted) {
                  if (entry->second.target != target.reference ||
                      entry->second.box != factory ||
                      entry->second.interface_type != id ||
                      entry->second.inherited_contracts.empty())
                    unsupported(name, "closed scalar result has competing "
                                      "original source edges");
                  entry->second.inherited_contracts.emplace(
                      member.reference,
                      InheritedCarrierContract{parameters,
                                               contract.result.signature});
                }
              }
              continue;
            }
            if (contract.result.signature != "V" ||
                contract.parameters.empty() || contract.parameters.size() > 8)
              continue;
            auto erased = member.reference;
            erased.owner = name;
            const auto *bridge = findMethod(cls, erased);
            if (!bridge)
              continue;
            for (const auto &target : cls.methods) {
              budget.tick();
              const bool original_target = std::any_of(
                  bridge->instructions.begin(), bridge->instructions.end(),
                  [&](const Instruction &op) {
                    budget.tick();
                    const auto *call = std::get_if<MethodRef>(&op.reference);
                    return (op.opcode == "invoke-virtual" ||
                            op.opcode == "invoke-virtual/range") &&
                           call && *call == target.reference;
                  });
              if (!original_target ||
                  target.reference.parameters.size() !=
                      contract.parameters.size() ||
                  target.reference.returns != "V" ||
                  !has(target.access, "public") ||
                  has(target.access, "static") ||
                  has(target.access, "abstract") ||
                  has(target.access, "native") ||
                  has(target.access, "bridge") ||
                  (target.declared_throws && !target.declared_throws->empty()))
                continue;
              const auto full = signatures.methods.find(target.reference);
              if (full != signatures.methods.end() &&
                  (!full->second.type_parameters.empty() ||
                   !full->second.throws_types.empty() ||
                   !full->second.source_projections.empty() ||
                   !full->second.result ||
                   full->second.parameters.size() !=
                       contract.parameters.size() ||
                   substitute(full->second, *full->second.result, {}, name,
                              budget, 0, true)
                           .signature != "V"))
                continue;
              if (full == signatures.methods.end() && target.generic_signature)
                continue;
              std::set<size_t> conversions;
              std::vector<MethodRef> unbox;
              std::optional<PrimitiveBridgeConversion> carrier_conversion;
              std::vector<std::string> parameters;
              exact = true;
              for (size_t i = 0; i < contract.parameters.size(); ++i) {
                budget.tick();
                const auto &runtime = target.reference.parameters[i];
                const auto actual =
                    full == signatures.methods.end()
                        ? SubstitutedType{runtime, runtime}
                        : substitute(full->second, full->second.parameters[i],
                                     {}, name, budget, 0, true);
                parameters.push_back(contract.parameters[i].signature);
                if (actual.signature == contract.parameters[i].signature)
                  continue;
                if (actual.signature != runtime ||
                    member.reference.parameters[i] != "Ljava/lang/Object;") {
                  exact = false;
                  break;
                }
                auto conversion = primitiveUnitConversion(runtime);
                if (!conversion ||
                    contract.parameters[i].signature != conversion->boxed ||
                    !knownPrimitiveBridgeConversion(*conversion, classes)) {
                  conversion.reset();
                  // A closed scalar carrier owns its exact original unbox
                  // call. Preserve arbitrary method effects and select the
                  // delegate from the checked body, even when its name differs.
                  for (const auto &op : bridge->instructions) {
                    budget.tick();
                    const auto *call = std::get_if<MethodRef>(&op.reference);
                    if ((op.opcode != "invoke-virtual" &&
                         op.opcode != "invoke-virtual/range") ||
                        !call ||
                        call->owner != contract.parameters[i].signature)
                      continue;
                    const auto candidate = scalarCarrierBridgeConversion(
                        *call, classes, signatures);
                    if (!candidate || candidate->unbox.returns != runtime)
                      continue;
                    if (conversion && conversion->unbox != candidate->unbox) {
                      exact = false;
                      break;
                    }
                    conversion = candidate;
                  }
                  if (!exact || !conversion ||
                      (carrier_conversion &&
                       carrier_conversion->unbox != conversion->unbox)) {
                    exact = false;
                    break;
                  }
                  carrier_conversion = conversion;
                }
                conversions.insert(i);
                unbox.push_back(conversion->unbox);
              }
              if (!exact || conversions.empty() ||
                  (!carrier_conversion &&
                   target.reference.name != member.reference.name) ||
                  (carrier_conversion &&
                   std::any_of(unbox.begin(), unbox.end(),
                               [&](const auto &call) {
                                 return call != carrier_conversion->unbox;
                               })))
                continue;
              verifyGenericBridge(*bridge, target, budget, nullptr,
                                  &conversions, &classes, &signatures, &member,
                                  carrier_conversion ? &*carrier_conversion
                                                     : nullptr);
              ValueCarrierBridge proof{
                  target.reference,
                  member.reference,
                  {},
                  std::move(unbox),
                  id,
                  originalClassType(*definition, edge.offset, budget)};
              proof.inherited_contracts.emplace(
                  member.reference, InheritedCarrierContract{parameters, "V"});
              auto [entry, inserted] = result.emplace(erased, std::move(proof));
              if (!inserted) {
                if (entry->second.target != target.reference ||
                    entry->second.interface_type != id ||
                    entry->second.inherited_contracts.empty())
                  unsupported(name, "closed primitive consumer has competing "
                                    "original source edges");
                entry->second.inherited_contracts.emplace(
                    member.reference,
                    InheritedCarrierContract{std::move(parameters), "V"});
              }
            }
          }
        }
      // Ordinary generic interfaces may mix fixed reference parameters with
      // class-formal parameters converted by the original Number calls. Prove
      // the complete resolved member before applying the same raw source edge.
      for (auto id : owner.interfaces) {
        budget.tick();
        const auto &edge = owner.types.at(id);
        const auto api = classes.find(edge.erasure);
        const auto formals = signatures.classes.find(edge.erasure);
        if (edge.segments.size() != 1 || edge.segments[0].arguments.empty() ||
            edge.segments[0].arguments.size() > 4 || api == classes.end() ||
            formals == signatures.classes.end() ||
            !formals->second.source_projections.empty() ||
            !has(api->second.access, "interface") ||
            api->second.superclass != "Ljava/lang/Object;" ||
            !api->second.fields.empty() || api->second.methods.empty() ||
            formals->second.type_parameters.size() !=
                edge.segments[0].arguments.size())
          continue;
        const Method *entry = nullptr;
        bool unique = true;
        for (const auto &candidate : api->second.methods) {
          budget.tick();
          if (candidate.reference.parameters.empty())
            continue;
          if (entry)
            unique = false;
          entry = &candidate;
        }
        if (!entry || !unique)
          continue;
        const auto &member = *entry;
        const auto contract = signatures.methods.find(member.reference);
        const size_t arity = member.reference.parameters.size();
        const bool integer_void_contract =
            member.reference.returns == "V" &&
            member.reference.parameters ==
                std::vector<std::string>{"Ljava/lang/Object;"};
        if (member.access != Access{"public", "abstract"} || !arity ||
            arity > 8 ||
            (!integer_void_contract &&
             member.reference.returns != "Ljava/lang/Object;") ||
            contract == signatures.methods.end() ||
            !contract->second.type_parameters.empty() ||
            !contract->second.throws_types.empty() ||
            !contract->second.source_projections.empty() ||
            contract->second.parameters.size() != arity ||
            !contract->second.result ||
            (member.declared_throws && !member.declared_throws->empty()))
          continue;
        std::map<std::string, SubstitutedType> bindings;
        bool exact = true;
        for (size_t i = 0; i < edge.segments[0].arguments.size(); ++i) {
          const auto &argument = edge.segments[0].arguments[i];
          if (argument.variance != GenericTypeArgument::Variance::Invariant ||
              !argument.type) {
            exact = false;
            break;
          }
          bindings.emplace(
              formals->second.type_parameters[i].name,
              substitute(owner, *argument.type, {}, name, budget, 0, true));
        }
        if (!exact)
          continue;
        // A raw source edge also affects every sibling. Admit only original
        // reference getters with unchanged erasure and exactly equal complete
        // resolved results, before any enclosing scope is projected in source.
        for (const auto &sibling : api->second.methods) {
          budget.tick();
          if (&sibling == entry)
            continue;
          const auto full = signatures.methods.find(sibling.reference);
          auto implementation = sibling.reference;
          implementation.owner = name;
          const auto *getter = findMethod(cls, implementation);
          const auto actual = signatures.methods.find(implementation);
          if (integer_void_contract ||
              sibling.access != Access{"public", "abstract"} ||
              !sibling.reference.parameters.empty() ||
              (!sibling.reference.returns.starts_with('L') &&
               !sibling.reference.returns.starts_with('[')) ||
              (sibling.declared_throws && !sibling.declared_throws->empty()) ||
              full == signatures.methods.end() || !getter ||
              actual == signatures.methods.end() ||
              !has(getter->access, "public") || has(getter->access, "static") ||
              has(getter->access, "abstract") ||
              has(getter->access, "bridge") ||
              (getter->declared_throws && !getter->declared_throws->empty())) {
            exact = false;
            break;
          }
          for (const auto *plan : {&full->second, &actual->second})
            if (!plan->parameters.empty() || !plan->result ||
                !plan->type_parameters.empty() || !plan->throws_types.empty() ||
                !plan->source_projections.empty())
              exact = false;
          if (!exact)
            break;
          const auto required = substitute(full->second, *full->second.result,
                                           bindings, name, budget, 0, true);
          const auto provided =
              substitute(actual->second, *actual->second.result, {}, name,
                         budget, 0, true);
          if (required.descriptor != sibling.reference.returns ||
              provided.descriptor != required.descriptor ||
              provided.signature != required.signature) {
            exact = false;
            break;
          }
        }
        if (!exact)
          continue;
        std::vector<SubstitutedType> expected;
        for (auto parameter : contract->second.parameters)
          expected.push_back(substitute(contract->second, parameter, bindings,
                                        name, budget, 0, true));
        const auto expected_result =
            substitute(contract->second, *contract->second.result, bindings,
                       name, budget, 0, true);
        if (integer_void_contract
                ? !closedUnaryConsumerParents(api->second, member.reference,
                                              bindings, expected[0], name,
                                              classes, signatures, budget)
                : !onlyEmptyMarkerParents(api->second, classes, budget))
          continue;
        auto erased = member.reference;
        erased.owner = name;
        const auto *bridge = findMethod(cls, erased);
        if (!bridge)
          continue;
        for (const auto &target : cls.methods) {
          budget.tick();
          const auto found = signatures.methods.find(target.reference);
          if (target.reference.name != member.reference.name ||
              target.reference.parameters.size() != arity ||
              target.reference.returns != member.reference.returns ||
              !has(target.access, "public") || has(target.access, "static") ||
              has(target.access, "abstract") || has(target.access, "bridge") ||
              (integer_void_contract &&
               target.reference.parameters != std::vector<std::string>{"I"}) ||
              (found == signatures.methods.end() &&
               (!integer_void_contract || target.generic_signature)) ||
              (target.declared_throws && !target.declared_throws->empty()))
            continue;
          const auto *signature =
              found == signatures.methods.end() ? nullptr : &found->second;
          if (signature ? !signature->type_parameters.empty() ||
                              !signature->throws_types.empty() ||
                              !signature->source_projections.empty() ||
                              signature->parameters.size() != arity ||
                              !signature->result ||
                              substitute(*signature, *signature->result, {},
                                         name, budget, 0, true)
                                      .signature != expected_result.signature
                        : target.reference.returns != expected_result.signature)
            continue;
          std::set<size_t> conversions;
          std::vector<MethodRef> unbox;
          exact = true;
          for (size_t i = 0; i < arity; ++i) {
            budget.tick();
            const auto &runtime = target.reference.parameters[i];
            // A closed (I)V target carries its full type in the descriptor.
            // Reference targets still require their original scoped Signature.
            const auto actual =
                signature ? substitute(*signature, signature->parameters[i], {},
                                       name, budget, 0, true)
                          : SubstitutedType{runtime, runtime};
            if (runtime == expected[i].descriptor &&
                actual.signature == expected[i].signature)
              continue;
            const auto conversion = primitiveUnitConversion(runtime);
            if ((runtime != "I" && runtime != "Z" && runtime != "F" &&
                 runtime != "C") ||
                member.reference.parameters[i] != "Ljava/lang/Object;" ||
                !conversion || actual.signature != runtime ||
                expected[i].signature != conversion->boxed ||
                !knownPrimitiveBridgeConversion(*conversion, classes)) {
              exact = false;
              break;
            }
            conversions.insert(i);
          }
          if (!exact || conversions.empty())
            continue;
          verifyGenericBridge(*bridge, target, budget, nullptr, &conversions,
                              &classes);
          for (const auto &op : bridge->instructions) {
            const auto *ref = std::get_if<MethodRef>(&op.reference);
            if (!ref || *ref == target.reference)
              continue;
            const auto conversion = primitiveUnitConversion(ref->returns);
            if (conversion && *ref == conversion->unbox)
              unbox.push_back(*ref);
          }
          auto [entry, inserted] = result.emplace(
              erased, ValueCarrierBridge{
                          target.reference,
                          member.reference,
                          {},
                          std::move(unbox),
                          id,
                          originalClassType(*definition, edge.offset, budget),
                          {},
                          {},
                          {},
                          interface_owner});
          if (!inserted && entry->second.target != target.reference)
            unsupported(name, "primitive bridge has conflicting full targets");
        }
      }
      for (auto id : owner.interfaces) {
        budget.tick();
        const auto &edge = owner.types.at(id);
        const auto api = classes.find(edge.erasure);
        const auto formals = signatures.classes.find(edge.erasure);
        if (edge.segments.size() != 1 || edge.segments[0].arguments.empty() ||
            edge.segments[0].arguments.size() > 23 || api == classes.end() ||
            formals == signatures.classes.end() ||
            !has(api->second.access, "interface") ||
            api->second.superclass != "Ljava/lang/Object;" ||
            api->second.methods.size() != 1 ||
            !onlyEmptyMarkerParents(api->second, classes, budget) ||
            formals->second.type_parameters.size() !=
                edge.segments[0].arguments.size())
          continue;
        const unsigned arity = edge.segments[0].arguments.size() - 1;
        const auto &member = api->second.methods[0];
        const auto contract = signatures.methods.find(member.reference);
        if (member.access != Access{"public", "abstract"} ||
            member.reference.name != "invoke" ||
            member.reference.parameters !=
                std::vector<std::string>(arity, "Ljava/lang/Object;") ||
            member.reference.returns != "Ljava/lang/Object;" ||
            contract == signatures.methods.end() ||
            !contract->second.type_parameters.empty() ||
            !contract->second.throws_types.empty() ||
            contract->second.parameters.size() != arity ||
            !contract->second.result ||
            (member.declared_throws && !member.declared_throws->empty()))
          continue;
        std::vector<std::string> carriers;
        std::vector<SubstitutedType> input_types;
        bool exact = true;
        for (size_t i = 0; i <= arity; ++i) {
          budget.tick();
          const auto &formal = formals->second.type_parameters[i];
          const auto &argument = edge.segments[0].arguments[i];
          if (!formal.class_bound) {
            exact = false;
            break;
          }
          const auto &bound = formals->second.types.at(*formal.class_bound);
          if (formal.erasure != "Ljava/lang/Object;" ||
              !formal.interface_bounds.empty() ||
              bound.kind != GenericType::Kind::Class ||
              bound.erasure != "Ljava/lang/Object;" || !argument.type ||
              argument.variance != GenericTypeArgument::Variance::Invariant) {
            exact = false;
            break;
          }
          const auto &actual = owner.types.at(*argument.type);
          const auto &used = contract->second.types.at(
              i < arity ? contract->second.parameters[i]
                        : *contract->second.result);
          // Array results are references with a complete generic shape, not
          // class declarations. Their exact target result is checked below.
          const bool array_result =
              i == arity && actual.kind == GenericType::Kind::Array;
          if (used.kind != GenericType::Kind::TypeVariable ||
              used.variable_owner != edge.erasure ||
              used.variable_name != formal.name ||
              (actual.kind != GenericType::Kind::Class && !array_result &&
               !(actual.kind == GenericType::Kind::TypeVariable &&
                 actual.erasure == "Ljava/lang/Object;")) ||
              (!array_result && !classes.contains(actual.erasure))) {
            exact = false;
            break;
          }
          carriers.push_back(actual.erasure);
          if (i < arity)
            input_types.push_back(
                substitute(owner, *argument.type, {}, name, budget, 0, true));
        }
        if (!exact)
          continue;
        const auto expected_result =
            substitute(owner, *edge.segments[0].arguments[arity].type, {}, name,
                       budget, 0, true);
        auto erased = member.reference;
        erased.owner = name;
        const auto *bridge = findMethod(cls, erased);
        if (!bridge ||
            bridge->access != Access{"public", "bridge", "synthetic"} ||
            bridge->generic_signature || bridge->annotation_default ||
            !bridge->tries.empty() ||
            (bridge->declared_throws && !bridge->declared_throws->empty()) ||
            bridge->registers < arity + 2 || bridge->registers > 65535 ||
            bridge->instructions.size() < 3 ||
            bridge->instructions.size() > 4 * arity + 5)
          continue;
        const auto &ops = bridge->instructions;
        uint32_t previous = 0;
        for (size_t i = 0; i < ops.size(); ++i) {
          budget.tick();
          const auto &op = ops[i];
          if ((i && op.pc <= previous) || op.pc >= bridge->code_end ||
              op.target || !op.targets.empty() || !op.keys.empty() ||
              !op.data.empty() || op.element_width ||
              !std::holds_alternative<std::monostate>(op.literal) ||
              std::any_of(
                  op.registers.begin(), op.registers.end(),
                  [&](unsigned reg) { return reg >= bridge->registers; }))
            exact = false;
          previous = op.pc;
        }
        if (!exact)
          continue;
        const unsigned self = bridge->registers - arity - 1;
        size_t at = 0;
        std::vector<unsigned> values{self};
        std::set<unsigned> live_registers;
        std::vector<std::string> parameters;
        std::vector<std::string> target_input_signatures;
        std::vector<MethodRef> unbox;
        bool nonstandard_conversion = false;
        for (unsigned i = 0; i < arity && exact; ++i) {
          unsigned checked = self + 1 + i;
          auto live = [&](unsigned reg) {
            return live_registers.contains(reg);
          };
          const bool copies_input = at < ops.size() &&
                                    (ops[at].opcode == "move-object" ||
                                     ops[at].opcode == "move-object/from16" ||
                                     ops[at].opcode == "move-object/16") &&
                                    ops[at].registers.size() == 2 &&
                                    ops[at].registers[1] == checked;
          if (carriers[i] == "Ljava/lang/Object;" && at < ops.size() &&
              ops[at].opcode != "check-cast" && !copies_input) {
            values.push_back(checked);
            parameters.push_back(carriers[i]);
            target_input_signatures.push_back(input_types[i].signature);
            live_registers.insert(checked);
            continue;
          }
          if (at < ops.size() && (ops[at].opcode == "move-object" ||
                                  ops[at].opcode == "move-object/from16" ||
                                  ops[at].opcode == "move-object/16")) {
            const auto &move = ops[at++];
            if (move.registers.size() != 2 || move.registers[1] != checked ||
                move.registers[0] >= self || live(move.registers[0]) ||
                !std::holds_alternative<std::monostate>(move.reference)) {
              exact = false;
              break;
            }
            checked = move.registers[0];
          }
          if (at >= ops.size()) {
            exact = false;
            break;
          }
          // Object-erased inputs need no invented cast. Their complete class
          // scope still has to agree with the original target Signature below.
          if (ops[at].opcode != "check-cast" &&
              carriers[i] == "Ljava/lang/Object;") {
            values.push_back(checked);
            parameters.push_back(carriers[i]);
            target_input_signatures.push_back(input_types[i].signature);
            live_registers.insert(checked);
            continue;
          }
          const auto &cast = ops[at++];
          const auto *type = std::get_if<std::string>(&cast.reference);
          if (cast.opcode != "check-cast" || !type ||
              cast.registers != std::vector<unsigned>{checked} ||
              live(checked)) {
            exact = false;
            break;
          }
          // A checked reference may flow unchanged to the target. A carrier
          // conversion is recognized only by its exact known virtual call ABI.
          if (at >= ops.size() ||
              (ops[at].opcode != "invoke-virtual" &&
               ops[at].opcode != "invoke-virtual/range") ||
              ops[at].registers != cast.registers) {
            if (*type != carriers[i]) {
              exact = false;
              break;
            }
            values.push_back(checked);
            parameters.push_back(carriers[i]);
            target_input_signatures.push_back(input_types[i].signature);
            live_registers.insert(checked);
            continue;
          }
          if (at + 1 >= ops.size()) {
            exact = false;
            break;
          }
          const auto &call = ops[at++], &move = ops[at++];
          const auto *ref = std::get_if<MethodRef>(&call.reference);
          const auto standard =
              ref ? primitiveUnitConversion(ref->returns) : std::nullopt;
          const bool narrow =
              standard && standard->boxed == carriers[i] &&
              standard->checked == *type && standard->unbox == *ref &&
              knownPrimitiveBridgeConversion(*standard, classes);
          const bool carrier_scalar =
              ref && (ref->returns == "I" || ref->returns == "J") &&
              *type == carriers[i] && ref->owner == *type &&
              ref->parameters.empty() && declaration(*ref, false);
          const bool reference_unbox =
              ref &&
              (ref->returns.starts_with('L') ||
               ref->returns.starts_with('[')) &&
              known_reference(ref->returns) && *type == carriers[i] &&
              ref->owner == *type && ref->parameters.empty() &&
              declaration(*ref, false);
          const bool wide_result = (carrier_scalar || narrow) &&
                                   (ref->returns == "J" || ref->returns == "D");
          const unsigned words = wide_result ? 2 : 1;
          if ((call.opcode != "invoke-virtual" &&
               call.opcode != "invoke-virtual/range") ||
              call.registers != cast.registers || !ref ||
              (!carrier_scalar && !narrow && !reference_unbox) ||
              // Class arguments do not enter a Signature-free literal
              // reference conversion. Preserve that original call and result;
              // class- or method-owned conversion results remain rejected by
              // declaration(), and unchanged inputs still compare full types.
              ((input_types[i].parameterized || input_types[i].open) &&
               !reference_unbox) ||
              move.opcode != (wide_result       ? "move-result-wide"
                              : reference_unbox ? "move-result-object"
                                                : "move-result") ||
              move.registers.size() != 1 || move.registers[0] + words > self ||
              live(move.registers[0]) ||
              (wide_result && live(move.registers[0] + 1)) ||
              !std::holds_alternative<std::monostate>(move.reference)) {
            exact = false;
            break;
          }
          values.push_back(move.registers[0]);
          if (wide_result)
            values.push_back(move.registers[0] + 1);
          parameters.push_back(ref->returns);
          target_input_signatures.push_back(ref->returns);
          live_registers.insert(move.registers[0]);
          if (wide_result)
            live_registers.insert(move.registers[0] + 1);
          unbox.push_back(*ref);
          nonstandard_conversion |=
              carrier_scalar || reference_unbox ||
              (narrow && ref->returns != "I" && ref->returns != "Z");
        }
        if (!exact)
          continue;
        // A range call may pack the proven receiver and reference arguments
        // after conversions. Track those copies without permitting any live
        // primitive or reference value to be overwritten.
        std::vector<bool> reference_words{true};
        for (const auto &parameter : parameters) {
          reference_words.push_back(parameter.starts_with('L') ||
                                    parameter.starts_with('['));
          if (parameter == "J" || parameter == "D")
            reference_words.push_back(false);
        }
        while (at < ops.size() && (ops[at].opcode == "move-object" ||
                                   ops[at].opcode == "move-object/from16" ||
                                   ops[at].opcode == "move-object/16")) {
          budget.tick();
          const auto &move = ops[at++];
          if (move.registers.size() != 2 || move.registers[0] >= self ||
              !std::holds_alternative<std::monostate>(move.reference) ||
              std::find(values.begin(), values.end(), move.registers[0]) !=
                  values.end()) {
            exact = false;
            break;
          }
          bool proven_source = false;
          for (size_t i = 0; i < values.size(); ++i)
            if (values[i] == move.registers[1] && reference_words[i]) {
              values[i] = move.registers[0];
              proven_source = true;
            }
          if (!proven_source) {
            exact = false;
            break;
          }
        }
        if (!exact || at + 3 > ops.size())
          continue;
        const auto &call = ops[at++];
        const auto *target = std::get_if<MethodRef>(&call.reference);
        if ((call.opcode != "invoke-virtual" &&
             call.opcode != "invoke-virtual/range") ||
            !target || target->owner != name ||
            target->parameters != parameters ||
            !declaration(*target, false, true) || call.registers != values)
          continue;
        const auto target_signature = signatures.methods.find(*target);
        std::optional<MethodRef> reference_factory;
        std::optional<std::pair<FieldRef, SubstitutedType>> representation;
        if (!expected_result.open && known_reference(target->returns) &&
            (target->returns.starts_with('L') ||
             target->returns.starts_with('[')) &&
            at + 4 == ops.size()) {
          const auto *ref = std::get_if<MethodRef>(&ops[at + 1].reference);
          if (ref && ref->owner == carriers[arity] &&
              ref->parameters == std::vector<std::string>{target->returns} &&
              ref->returns == carriers[arity] && declaration(*ref, true)) {
            representation = fixedReferenceBoxingRepresentation(
                *ref, classes, signatures, budget);
            if (representation)
              reference_factory = *ref;
          }
        }
        const auto boolean_conversion = primitiveUnitConversion("Z");
        const bool boxed_boolean_result =
            target->returns == "Z" && !expected_result.open &&
            !expected_result.parameterized && boolean_conversion &&
            expected_result.signature == boolean_conversion->boxed &&
            knownPrimitiveBridgeConversion(*boolean_conversion, classes);
        // Standard Integer/Boolean Unit bridges retain their existing owner.
        const bool void_unit = nonstandard_conversion &&
                               target->returns == "V" &&
                               expected_result.signature == "Lkotlin/Unit;";
        if (target_signature != signatures.methods.end()) {
          const auto &signature = target_signature->second;
          if (!signature.type_parameters.empty() ||
              !signature.throws_types.empty() ||
              !signature.source_projections.empty() || !signature.result ||
              signature.parameters.size() != arity)
            continue;
          for (size_t i = 0; i < arity; ++i) {
            budget.tick();
            if (substitute(signature, signature.parameters[i], {}, name, budget,
                           0, true)
                        .signature != target_input_signatures[i] &&
                (input_types[i].open ||
                 input_types[i].signature != target_input_signatures[i] ||
                 !equivalentClosedParameterType(
                     signature, signature.parameters[i], owner,
                     *edge.segments[0].arguments[i].type, classes, budget)))
              exact = false;
          }
          const auto actual_result = substitute(signature, *signature.result,
                                                {}, name, budget, 0, true);
          if (actual_result.signature !=
              (representation ? representation->second.signature
               : target->returns == "J" || void_unit || boxed_boolean_result
                   ? target->returns
                   : expected_result.signature))
            exact = false;
          if (!exact)
            continue;
        } else if ((representation ? representation->second.parameterized ||
                                         representation->second.open
                                   : expected_result.parameterized ||
                                         expected_result.open) ||
                   target_input_signatures != target->parameters) {
          continue;
        }
        std::optional<MethodRef> factory;
        std::optional<FieldRef> result_field;
        if (void_unit) {
          result_field = unitResultField(classes, budget);
          if (!result_field || at + 2 != ops.size())
            continue;
        } else if (target->returns == "J" || boxed_boolean_result ||
                   reference_factory) {
          if (!reference_factory &&
              (expected_result.parameterized || expected_result.open))
            continue;
          if (at + 4 != ops.size())
            continue;
          const auto &move = ops[at++], &box = ops[at++];
          const auto *ref = std::get_if<MethodRef>(&box.reference);
          const bool wide_result = !reference_factory && !boxed_boolean_result;
          if (move.opcode != (reference_factory ? "move-result-object"
                              : wide_result     ? "move-result-wide"
                                                : "move-result") ||
              move.registers.size() != 1 ||
              move.registers[0] + (wide_result ? 1 : 0) >= self ||
              !std::holds_alternative<std::monostate>(move.reference) ||
              (box.opcode != "invoke-static" &&
               box.opcode != "invoke-static/range") ||
              !ref || ref->owner != carriers[arity] ||
              ref->parameters != std::vector<std::string>{target->returns} ||
              ref->returns != carriers[arity] || !declaration(*ref, true) ||
              (boxed_boolean_result && ref->name != "valueOf") ||
              box.registers !=
                  (wide_result ? std::vector<unsigned>{move.registers[0],
                                                       move.registers[0] + 1}
                               : std::vector<unsigned>{move.registers[0]}))
            continue;
          factory = *ref;
        } else if (target->returns != carriers[arity] || at + 2 != ops.size() ||
                   (unbox.empty() && target->name == member.reference.name)) {
          continue;
        }
        // A differently named reference target may already return the exact
        // boxed result. Its complete original contract and sole virtual call
        // have been proved above; do not invent a conversion or rename its
        // runtime slot. Same-name ordinary generic bridges keep their owner.
        const auto &boxed = ops[at++], &ret = ops[at];
        const auto *field = std::get_if<FieldRef>(&boxed.reference);
        if ((result_field ? boxed.opcode != "sget-object" || !field ||
                                *field != *result_field
                          : boxed.opcode != "move-result-object" ||
                                !std::holds_alternative<std::monostate>(
                                    boxed.reference)) ||
            boxed.registers.size() != 1 || ret.opcode != "return-object" ||
            ret.registers != boxed.registers ||
            !std::holds_alternative<std::monostate>(ret.reference))
          continue;
        auto [entry, inserted] = result.emplace(
            erased,
            ValueCarrierBridge{
                *target, member.reference, factory, std::move(unbox), id,
                originalClassType(*definition, edge.offset, budget),
                result_field,
                representation ? std::optional<FieldRef>{representation->first}
                               : std::nullopt,
                representation ? representation->second.signature
                               : std::string{},
                interface_owner});
        if (!inserted && entry->second.target != *target)
          unsupported(name, "value-carrier bridge has conflicting targets");
      }
    }
  }
  return result;
}

bool projectGuardedDefaultResults(const ClassMap &classes,
                                  GenericSignaturePlans &signatures,
                                  Budget &budget) {
  using Bindings = std::map<std::string, SubstitutedType>;
  const auto shape = [&](const MethodRef &ref) -> const GenericSignature * {
    const auto found = signatures.methods.find(ref);
    if (found == signatures.methods.end() || !found->second.result ||
        !found->second.type_parameters.empty() ||
        !found->second.throws_types.empty() ||
        !found->second.source_projections.empty())
      return nullptr;
    return &found->second;
  };
  const auto complete = [&](const GenericSignature &sig,
                            const Bindings &bindings,
                            const std::string &context) {
    std::vector<std::string> result;
    for (const auto id : sig.parameters)
      result.push_back(
          substitute(sig, id, bindings, context, budget, 0, true).signature);
    result.push_back(
        substitute(sig, *sig.result, bindings, context, budget, 0, true)
            .signature);
    return result;
  };
  const auto bind =
      [&](const GenericSignature &sig, GenericTypeId id,
          const Bindings &prior, const std::string &context,
          const GenericSignature &parent) -> std::optional<Bindings> {
    const auto &edge = sig.types.at(id);
    if (edge.segments.size() != 1 ||
        edge.segments[0].arguments.size() != parent.type_parameters.size())
      return {};
    Bindings result;
    for (size_t i = 0; i < parent.type_parameters.size(); ++i) {
      budget.tick();
      const auto &arg = edge.segments[0].arguments[i];
      if (!arg.type ||
          arg.variance != GenericTypeArgument::Variance::Invariant)
        return {};
      result.emplace(
          parent.type_parameters[i].name,
          substitute(sig, *arg.type, prior, context, budget, 0, true));
    }
    return result;
  };
  // Resolve the original class-super call, including a unique inherited
  // interface default. All edges retain full original scoped arguments.
  const auto inherited =
      [&](const Class &cls, const MethodRef &call,
          const std::vector<std::string> &wanted) -> std::optional<MethodRef> {
    struct Use {
      const GenericSignature *sig;
      GenericTypeId edge;
      Bindings bindings;
      std::set<std::string> path;
    };
    std::vector<Use> interfaces;
    std::map<std::string, GenericSignature> plain;
    const Class *current = &cls;
    Bindings bindings;
    std::set<std::string> seen{cls.name};
    std::optional<MethodRef> class_target;
    while (current->superclass &&
           *current->superclass != "Ljava/lang/Object;") {
      budget.tick();
      const auto source = signatures.classes.find(current->name);
      const auto parent = classes.find(*current->superclass);
      const auto formals = signatures.classes.find(*current->superclass);
      if (source == signatures.classes.end() || !source->second.superclass ||
          !source->second.source_projections.empty() ||
          parent == classes.end() || has(parent->second.access, "interface") ||
          formals == signatures.classes.end() ||
          !formals->second.source_projections.empty() ||
          !seen.insert(parent->first).second || seen.size() > 64)
        return {};
      auto next = bind(source->second, *source->second.superclass, bindings, cls.name,
                       formals->second);
      if (!next)
        return {};
      bindings = std::move(*next);
      current = &parent->second;
      for (auto id : formals->second.interfaces)
        interfaces.push_back({&formals->second, id, bindings, {}});
      const auto member =
          std::find_if(current->methods.begin(), current->methods.end(),
                       [&](const Method &m) {
                         return m.reference.name == call.name &&
                                m.reference.parameters == call.parameters &&
                                m.reference.returns == call.returns;
                       });
      if (member != current->methods.end()) {
        const auto *sig = shape(member->reference);
        if (!sig || !has(member->access, "public") ||
            has(member->access, "static") || has(member->access, "abstract") ||
            has(member->access, "bridge") || has(member->access, "final") ||
            (member->declared_throws && !member->declared_throws->empty()) ||
            complete(*sig, bindings, cls.name) != wanted)
          return {};
        class_target = member->reference;
        break; // A nearest class declaration dominates all interface defaults.
      }
    }
    if (class_target)
      return class_target;
    std::map<std::string, std::vector<std::string>> uses;
    std::vector<const Method *> declarations;
    for (size_t at = 0; at < interfaces.size(); ++at) {
      budget.tick();
      const auto use = interfaces[at];
      const auto &edge = use.sig->types.at(use.edge);
      const auto declaration = classes.find(edge.erasure);
      if (declaration == classes.end() ||
          !has(declaration->second.access, "interface") ||
          declaration->second.superclass != "Ljava/lang/Object;" ||
          use.path.contains(edge.erasure) || use.path.size() > 64 ||
          uses.size() > 128)
        return {};
      const GenericSignature *parent = nullptr;
      if (const auto found = signatures.classes.find(edge.erasure);
          found != signatures.classes.end())
        parent = &found->second;
      else {
        auto [plain_entry, inserted] = plain.try_emplace(edge.erasure);
        if (inserted)
          plain_entry->second =
              plainRuntimeInterfaceSignature(declaration->second, budget);
        parent = &plain_entry->second;
      }
      if (!parent->source_projections.empty())
        return {};
      auto arguments = bind(*use.sig, use.edge, use.bindings, cls.name, *parent);
      if (!arguments)
        return {};
      std::vector<std::string> identity;
      for (const auto &formal : parent->type_parameters)
        identity.push_back(arguments->at(formal.name).signature);
      auto [prior, inserted] = uses.emplace(edge.erasure, identity);
      if (!inserted && prior->second != identity)
        return {};
      if (!inserted)
        continue;
      auto path = use.path;
      path.insert(edge.erasure);
      for (auto id : parent->interfaces)
        interfaces.push_back({parent, id, *arguments, path});
      for (const auto &member : declaration->second.methods) {
        budget.tick();
        if (member.reference.name != call.name ||
            member.reference.parameters != call.parameters ||
            member.reference.returns != call.returns ||
            has(member.access, "static") || has(member.access, "private") ||
            has(member.access, "bridge"))
          continue;
        const auto *sig = shape(member.reference);
        if (!sig || !has(member.access, "public") ||
            (member.declared_throws && !member.declared_throws->empty()) ||
            complete(*sig, *arguments, cls.name) != wanted)
          return {};
        declarations.push_back(&member);
      }
    }
    // Traverse the known nominal DAG independently of argument deduplication,
    // so a diamond cannot hide a more-specific abstract/default declaration.
    const auto reaches = [&](const std::string &child,
                             const std::string &parent) {
      std::vector<std::string> pending{child};
      std::set<std::string> visited;
      for (size_t at = 0; at < pending.size(); ++at) {
        budget.tick();
        if (!visited.insert(pending[at]).second)
          continue;
        if (visited.size() > 128)
          return false;
        const auto known = classes.find(pending[at]);
        if (known == classes.end())
          return false;
        for (const auto &edge : known->second.interfaces) {
          if (edge == parent)
            return true;
          pending.push_back(edge);
        }
      }
      return false;
    };
    const Method *selected = nullptr;
    for (const auto *candidate : declarations) {
      if (std::any_of(declarations.begin(), declarations.end(),
                      [&](const Method *other) {
                        return other != candidate &&
                               reaches(other->reference.owner,
                                       candidate->reference.owner);
                      }))
        continue;
      if (selected || has(candidate->access, "abstract"))
        return {};
      selected = candidate;
    }
    return selected ? std::optional<MethodRef>{selected->reference}
                    : std::nullopt;
  };
  const auto guard = [&](const Class &cls, const Method &method,
                         const MethodRef &typed, bool erased) {
    const auto &ops = method.instructions;
    if ((method.access != Access{"public", "final", "bridge"} &&
         method.access != Access{"public", "final", "bridge", "synthetic"}) ||
        method.registers < 4 || method.registers > 65535 ||
        !method.tries.empty() || ops.size() < 7 || ops.size() > 10 ||
        (method.declared_throws && !method.declared_throws->empty()))
      return false;
    const unsigned self = method.registers - 3, key = self + 1,
                   value = self + 2;
    if (ops[0].opcode != "instance-of" || ops[0].registers.size() != 2 ||
        ops[0].registers[0] >= self || ops[0].registers[1] != key ||
        std::get_if<std::string>(&ops[0].reference) == nullptr ||
        std::get<std::string>(ops[0].reference) != typed.parameters[0] ||
        ops[1].opcode != "if-nez" ||
        ops[1].registers != std::vector<unsigned>{ops[0].registers[0]} ||
        ops[1].target != ops[3].pc || ops[2].opcode != "return-object" ||
        ops[2].registers != std::vector<unsigned>{value})
      return false;
    size_t at = 3;
    const auto checked = [&](unsigned incoming) -> std::optional<unsigned> {
      unsigned reg = incoming;
      if (at < ops.size() && (ops[at].opcode == "move-object" ||
                              ops[at].opcode == "move-object/from16" ||
                              ops[at].opcode == "move-object/16")) {
        const auto &move = ops[at++];
        if (move.registers.size() != 2 || move.registers[0] >= self ||
            move.registers[1] != incoming)
          return {};
        reg = move.registers[0];
      }
      return reg;
    };
    const auto checked_key = checked(key);
    if (!checked_key || at >= ops.size())
      return false;
    const auto key_cast = at++;
    if (ops[key_cast].opcode != "check-cast" ||
        ops[key_cast].registers != std::vector<unsigned>{*checked_key} ||
        std::get_if<std::string>(&ops[key_cast].reference) == nullptr ||
        std::get<std::string>(ops[key_cast].reference) != typed.parameters[0])
      return false;
    unsigned checked_value = value;
    std::optional<size_t> value_cast;
    if (erased) {
      const auto reg = checked(value);
      if (!reg || *reg == *checked_key || at >= ops.size())
        return false;
      checked_value = *reg;
      value_cast = at++;
      const auto &cast = ops[*value_cast];
      if (cast.opcode != "check-cast" ||
          cast.registers != std::vector<unsigned>{checked_value} ||
          std::get_if<std::string>(&cast.reference) == nullptr ||
          std::get<std::string>(cast.reference) != typed.parameters[1])
        return false;
    }
    if (at + 3 != ops.size())
      return false;
    const auto invoke = at;
    if ((ops[at].opcode != "invoke-virtual" &&
         ops[at].opcode != "invoke-virtual/range") ||
        std::get_if<MethodRef>(&ops[at].reference) == nullptr ||
        std::get<MethodRef>(ops[at].reference) != typed ||
        ops[at].registers !=
            std::vector<unsigned>{self, *checked_key, checked_value} ||
        ops[at + 1].opcode != "move-result-object" ||
        ops[at + 1].registers.size() != 1 || ops[at + 1].registers[0] >= self ||
        ops[at + 2].opcode != "return-object" ||
        ops[at + 2].registers != ops[at + 1].registers)
      return false;
    for (size_t i = 0; i < ops.size(); ++i) {
      budget.tick();
      const auto &op = ops[i];
      if ((i && op.pc <= ops[i - 1].pc) || op.pc >= method.code_end ||
          (i != 1 && op.target) || !op.targets.empty() || !op.keys.empty() ||
          !op.data.empty() || op.element_width ||
          !std::holds_alternative<std::monostate>(op.literal) ||
          (i != 0 && i != key_cast && i != value_cast && i != invoke &&
           !std::holds_alternative<std::monostate>(op.reference)))
        return false;
    }
    return classes.contains(typed.parameters[0]) &&
           classes.contains(typed.parameters[1]);
  };
  bool projected = false;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    const auto owner = signatures.classes.find(name);
    if (cls.external || has(cls.access, "interface") ||
        !cls.generic_signature || owner == signatures.classes.end() ||
        !owner->second.superclass || !owner->second.source_projections.empty())
      continue;
    for (const auto &typed : cls.methods) {
      budget.tick();
      const auto &ref = typed.reference;
      if ((typed.access != Access{"public", "bridge"} &&
           typed.access != Access{"public", "bridge", "synthetic"}) ||
          ref.parameters.size() != 2 || !ref.parameters[0].starts_with('L') ||
          ref.parameters[0] == "Ljava/lang/Object;" ||
          ref.parameters[1] != ref.returns || !ref.returns.starts_with('L') ||
          ref.returns == "Ljava/lang/Object;" ||
          typed.instructions.size() != 4 || typed.registers < 4 ||
          typed.registers > 65535 || !typed.tries.empty() ||
          (typed.declared_throws && !typed.declared_throws->empty()))
        continue;
      const auto *typed_sig = shape(ref);
      if (!typed_sig || typed_sig->parameters.size() != 2)
        continue;
      auto wrapper_ref = ref;
      wrapper_ref.parameters[0] = "Ljava/lang/Object;";
      const auto *wrapper = findMethod(cls, wrapper_ref);
      auto erased_ref = wrapper_ref;
      erased_ref.parameters[1] = erased_ref.returns = "Ljava/lang/Object;";
      const auto *erased = findMethod(cls, erased_ref);
      const auto *wrapper_sig = shape(wrapper_ref);
      if (!wrapper || !erased || !wrapper_sig || erased->generic_signature ||
          !has(erased->access, "synthetic") ||
          !guard(cls, *wrapper, ref, false) || !guard(cls, *erased, ref, true))
        continue;
      auto wanted = complete(*typed_sig, {}, name);
      if (wanted.size() != 3 || wanted[1] != wanted[2])
        continue;
      wanted[0] = "Ljava/lang/Object;";
      if (complete(*wrapper_sig, {}, name) != wanted)
        continue;
      const auto &ops = typed.instructions;
      const auto *call = std::get_if<MethodRef>(&ops[0].reference);
      const unsigned self = typed.registers - 3;
      if ((ops[0].opcode != "invoke-super" &&
           ops[0].opcode != "invoke-super/range") ||
          !call || call->owner != cls.superclass || call->name != ref.name ||
          call->parameters != std::vector<std::string>{"Ljava/lang/Object;",
                                                       "Ljava/lang/Object;"} ||
          call->returns != "Ljava/lang/Object;" ||
          ops[0].registers != std::vector<unsigned>{self, self + 1, self + 2} ||
          ops[1].opcode != "move-result-object" ||
          ops[1].registers.size() != 1 || ops[1].registers[0] >= self ||
          ops[2].opcode != "check-cast" ||
          ops[2].registers != ops[1].registers ||
          std::get_if<std::string>(&ops[2].reference) == nullptr ||
          std::get<std::string>(ops[2].reference) != ref.returns ||
          ops[3].opcode != "return-object" ||
          ops[3].registers != ops[1].registers)
        continue;
      bool exact = true;
      for (size_t i = 0; i < ops.size(); ++i) {
        const auto &op = ops[i];
        exact &= (!i || op.pc > ops[i - 1].pc) && op.pc < typed.code_end &&
                 !op.target && op.targets.empty() && op.keys.empty() &&
                 op.data.empty() && !op.element_width &&
                 std::holds_alternative<std::monostate>(op.literal) &&
                 (i == 0 || i == 2 ||
                  std::holds_alternative<std::monostate>(op.reference));
      }
      const auto declaration =
          exact ? inherited(cls, *call, wanted) : std::nullopt;
      if (!declaration)
        continue;
      signatures.guarded_default_results.emplace(
          erased_ref, GuardedDefaultResultBridge{ref, wrapper_ref, *call,
                                                 *declaration, *wrapper_sig});
      signatures.superclass_calls.emplace(std::pair{ref, *call}, *wrapper_sig);
      signatures.superclass_call_declarations.emplace(std::pair{ref, *call},
                                                      *declaration);
      signatures.superclass_reference_adapters.emplace(ref, *call);
      signatures.inheritance_contracts.try_emplace(name, owner->second);
      auto &source = owner->second;
      std::vector<GenericTypeId> edges = source.interfaces;
      edges.insert(edges.begin(), *source.superclass);
      for (auto id : edges) {
        auto &edge = source.types.at(id);
        bool changed = false;
        for (auto &segment : edge.segments) {
          changed |= !segment.arguments.empty();
          segment.arguments.clear();
        }
        if (changed)
          source.source_projections.push_back(
              {edge.offset,
               id == *source.superclass ? "guarded-default-raw-superclass"
                                        : "guarded-default-raw-interface",
               *cls.generic_signature, edge.erasure, erased_ref.identity()});
      }
      signatures.guarded_default_raw_owners.insert(name);
      projected = true;
      break;
    }
  }
  // A non-generic interface cannot itself be used raw. If its only direct
  // implementation needs a raw superclass for the verified erased guard,
  // reconcile a shared generic ancestor on the interface's declaration.
  // Compare the full original arguments along both paths before projecting it.
  std::map<std::string, std::set<std::string>> implementers;
  for (const auto &[name, cls] : classes)
    for (const auto &edge : cls.interfaces) {
      budget.tick();
      implementers[edge].insert(name);
    }
  for (const auto &name : signatures.guarded_default_raw_owners) {
    const auto &cls = classes.at(name);
    const auto &original = signatures.inheritance_contracts.at(name);
    const auto parent = signatures.classes.find(*cls.superclass);
    if (parent == signatures.classes.end() ||
        !parent->second.source_projections.empty())
      continue;
    auto arguments =
        bind(original, *original.superclass, {}, name, parent->second);
    if (!arguments)
      continue;
    for (const auto &apiName : cls.interfaces) {
      const auto apiClass = classes.find(apiName);
      auto api = signatures.classes.find(apiName);
      if (apiClass == classes.end() || apiClass->second.external ||
          !apiClass->second.generic_signature ||
          api == signatures.classes.end() ||
          !api->second.type_parameters.empty() ||
          !api->second.source_projections.empty() ||
          implementers[apiName] != std::set<std::string>{name})
        continue;
      for (auto edgeId : api->second.interfaces) {
        auto &edge = api->second.types.at(edgeId);
        if (edge.segments.size() != 1 || edge.segments[0].arguments.empty())
          continue;
        const auto use =
            substitute(api->second, edgeId, {}, apiName, budget, 0, true);
        if (use.open ||
            !std::any_of(parent->second.interfaces.begin(),
                         parent->second.interfaces.end(), [&](auto id) {
                           return substitute(parent->second, id, *arguments,
                                             name, budget, 0, true)
                                      .signature == use.signature;
                         }))
          continue;
        signatures.inheritance_contracts.try_emplace(apiName, api->second);
        api->second.source_projections.push_back(
            {edge.offset, "guarded-default-shared-interface",
             originalClassType(apiClass->second, edge.offset, budget),
             edge.erasure, name});
        edge.segments[0].arguments.clear();
      }
    }
  }
  return projected;
}

namespace {
std::optional<GenericInheritancePlan::RetainedBridge>
arrayCarrierSuperclassBridge(const Class &cls, const Method &bridge,
                             const Method &required,
                             const MethodRef &specialized,
                             const ClassMap &classes,
                             const GenericSignaturePlans &signatures,
                             Budget &budget) {
  if (bridge.access != Access{"public", "bridge", "synthetic"} ||
      bridge.reference.parameters.size() != specialized.parameters.size() ||
      bridge.reference.parameters.empty() ||
      specialized.parameters.size() > 8 || bridge.generic_signature ||
      bridge.reference == specialized ||
      (bridge.reference.returns != specialized.returns &&
       !isAssignableReference(specialized.returns, bridge.reference.returns,
                              classes, budget)))
    return {};
  std::optional<GenericInheritancePlan::RetainedBridge> proof;
  for (const auto &target : cls.methods) {
    budget.tick();
    if (target.reference.name == bridge.reference.name ||
        target.reference.parameters.size() != specialized.parameters.size() ||
        target.reference.returns != specialized.returns ||
        (!has(target.access, "public") && !has(target.access, "protected")) ||
        has(target.access, "private") || has(target.access, "static") ||
        has(target.access, "abstract") || has(target.access, "native") ||
        has(target.access, "bridge") || target.generic_signature ||
        signatures.methods.contains(target.reference) ||
        (target.declared_throws && !target.declared_throws->empty()))
      continue;
    bool original_call = false;
    for (const auto &op : bridge.instructions) {
      budget.tick();
      const auto *call = std::get_if<MethodRef>(&op.reference);
      original_call |= call && *call == target.reference &&
                       (op.opcode == "invoke-virtual" ||
                        op.opcode == "invoke-virtual/range");
    }
    if (!original_call)
      continue;
    std::optional<size_t> converted;
    bool exact = true;
    for (size_t i = 0; i < specialized.parameters.size(); ++i) {
      budget.tick();
      if (target.reference.parameters[i] == specialized.parameters[i] &&
          bridge.reference.parameters[i] == specialized.parameters[i])
        continue;
      if (converted || !target.reference.parameters[i].starts_with('[') ||
          !specialized.parameters[i].starts_with('L') ||
          !bridge.reference.parameters[i].starts_with('L') ||
          specialized.parameters[i] == bridge.reference.parameters[i] ||
          !isAssignableReference(specialized.parameters[i],
                                 bridge.reference.parameters[i], classes,
                                 budget)) {
        exact = false;
        break;
      }
      converted = i;
    }
    if (!exact || !converted)
      continue;
    std::optional<PrimitiveBridgeConversion> conversion;
    for (const auto &op : bridge.instructions) {
      budget.tick();
      const auto *call = std::get_if<MethodRef>(&op.reference);
      if (!call || call->owner != specialized.parameters[*converted] ||
          call->returns != target.reference.parameters[*converted] ||
          (op.opcode != "invoke-virtual" &&
           op.opcode != "invoke-virtual/range"))
        continue;
      const auto known =
          arrayCarrierBridgeConversion(*call, classes, signatures);
      if (!known)
        continue;
      if (conversion && conversion->unbox != known->unbox)
        unsupported(bridge.reference.identity(),
                    "array carrier has competing original unbox declarations");
      conversion = known;
    }
    if (!conversion)
      continue;
    const std::set<size_t> conversions{*converted};
    verifyGenericBridge(bridge, target, budget, nullptr, &conversions, &classes,
                        &signatures, &required, &*conversion);
    if (proof && proof->target != target.reference)
      unsupported(bridge.reference.identity(),
                  "array carrier has competing original delegates");
    proof = GenericInheritancePlan::RetainedBridge{target.reference};
    proof->source_reference = specialized;
    proof->unbox_method = conversion->unbox;
    proof->inherited_class_contract = required.reference;
    proof->carrier_contracts.insert(required.reference);
  }
  return proof;
}

// A literal superclass edge contributes no formals, but it must not hide a
// more distant generic parameter contract. This closed profile consumes the
// original scoped substitutions and the common forwarding-body verifier.
// An own final target still has its original non-final compiler bridge; final
// ancestor slots remain forbidden by the override checks below.
std::optional<GenericBridge> literalSuperclassParameterBridge(
    const Class &cls, const Method &bridge, const ClassMap &classes,
    const GenericSignaturePlans &signatures,
    const NamedAnonymousMembers &named_members, Budget &budget) {
  const auto &result_type = bridge.reference.returns;
  const bool scalar_result =
      result_type.size() == 1 &&
      std::string_view("VZBCSIJFD").find(result_type.front()) !=
          std::string_view::npos;
  const bool named_member = isNamedAnonymousMember(cls, named_members);
  if (cls.external || cls.generic_signature ||
      (!independentSourceScope(cls) && !named_member) ||
      has(cls.access, "interface") || !cls.superclass ||
      bridge.access != Access{"public", "bridge", "synthetic"} ||
      !scalar_result || bridge.reference.parameters.empty())
    return {};
  const Method *target = nullptr;
  for (const auto &op : bridge.instructions) {
    budget.tick();
    const auto *call = std::get_if<MethodRef>(&op.reference);
    if (!call ||
        (op.opcode != "invoke-virtual" && op.opcode != "invoke-virtual/range"))
      continue;
    if (target || call->owner != cls.name ||
        call->name != bridge.reference.name || call->returns != result_type)
      return {};
    target = findMethod(cls, *call);
  }
  if (!target || target->generic_signature ||
      signatures.methods.contains(target->reference) ||
      (target->access != Access{"public"} &&
       target->access != Access{"public", "final"}) ||
      target->reference.parameters == bridge.reference.parameters ||
      target->reference.parameters.size() !=
          bridge.reference.parameters.size() ||
      (target->declared_throws && !target->declared_throws->empty()))
    return {};
  std::map<std::string, unsigned> interface_states;
  std::function<bool(const std::string &, unsigned)> unchanged_interface =
      [&](const std::string &name, unsigned depth) {
        budget.tick();
        if (depth > 64 || interface_states.size() > 128)
          return false;
        const auto known = classes.find(name);
        if (known == classes.end() || !has(known->second.access, "interface") ||
            known->second.superclass != "Ljava/lang/Object;" ||
            !independentSourceScope(known->second))
          return false;
        auto &state = interface_states[name];
        if (state)
          return state == 2;
        state = 1;
        for (const auto &member : known->second.methods) {
          budget.tick();
          if (member.reference.name == bridge.reference.name)
            return false; // Competing interface slots need their full proof.
        }
        for (const auto &ancestor : known->second.interfaces)
          if (!unchanged_interface(ancestor, depth + 1))
            return false;
        state = 2;
        return true;
      };
  std::map<std::string, SubstitutedType> bindings;
  std::set<std::string> seen;
  const Method *contract = nullptr;
  const Class *current = &cls;
  while (current) {
    budget.tick();
    if (!seen.insert(current->name).second || seen.size() > 64 ||
        has(current->access, "interface") ||
        (!independentSourceScope(*current) &&
         !(current == &cls && named_member)) ||
        !current->superclass ||
        std::none_of(current->methods.begin(), current->methods.end(),
                     [](const Method &method) {
                       return method.reference.name == "<init>";
                     }))
      return {};
    const auto *shape = inheritanceClassSignature(current->name, signatures);
    if ((current->generic_signature && !shape) ||
        (shape && !shape->source_projections.empty()))
      return {};
    for (const auto &interface : current->interfaces)
      if (!unchanged_interface(interface, 0))
        return {};
    if (current != &cls) {
      if (has(current->access, "final"))
        return {};
      for (const auto &member : current->methods) {
        budget.tick();
        const auto &ref = member.reference;
        if (ref.name != bridge.reference.name)
          continue;
        if (!has(member.access, "public") || has(member.access, "static") ||
            has(member.access, "final") || ref.returns != result_type ||
            (member.declared_throws && !member.declared_throws->empty()))
          return {};
        if (has(member.access, "bridge")) {
          if (member.access != Access{"public", "bridge", "synthetic"} ||
              ref.parameters != bridge.reference.parameters)
            return {};
          if (!current->external) {
            auto typed = target->reference;
            typed.owner = current->name;
            const auto *original = findMethod(*current, typed);
            if (!original)
              return {};
            verifyGenericBridge(member, *original, budget, nullptr, nullptr,
                                &classes, &signatures);
          }
          continue;
        }
        const auto method_shape = signatures.methods.find(ref);
        if (method_shape == signatures.methods.end()) {
          if (member.generic_signature ||
              ref.parameters != target->reference.parameters)
            return {};
          continue;
        }
        const auto &full = method_shape->second;
        if (!full.result || !full.type_parameters.empty() ||
            !full.throws_types.empty() || !full.source_projections.empty() ||
            full.parameters.size() != target->reference.parameters.size() ||
            substitute(full, *full.result, bindings, cls.name, budget, 0, true)
                    .signature != result_type)
          return {};
        for (size_t i = 0; i < full.parameters.size(); ++i)
          if (substitute(full, full.parameters[i], bindings, cls.name, budget,
                         0, true)
                  .signature != target->reference.parameters[i])
            return {};
        const bool uses_class_formal = std::any_of(
            full.types.begin(), full.types.end(), [&](const GenericType &type) {
              budget.tick();
              return type.kind == GenericType::Kind::TypeVariable &&
                     type.variable_owner == current->name;
            });
        if (uses_class_formal && ref.parameters == bridge.reference.parameters)
          contract = &member;
      }
    }
    if (*current->superclass == "Ljava/lang/Object;")
      break;
    const auto ancestor = classes.find(*current->superclass);
    if (ancestor == classes.end())
      return {};
    const auto *parent = inheritanceClassSignature(ancestor->first, signatures);
    if ((!shape && parent && !parent->type_parameters.empty()) ||
        (shape &&
         (!shape->superclass ||
          shape->types.at(*shape->superclass).segments.size() != 1 ||
          shape->types.at(*shape->superclass).segments[0].arguments.size() !=
              (parent ? parent->type_parameters.size() : 0))))
      return {};
    auto composed = composeClassEdge(*current, ancestor->second, bindings,
                                     signatures, cls.name, budget);
    if (!composed)
      return {};
    bindings = std::move(*composed);
    current = &ancestor->second;
  }
  if (!contract)
    return {};
  verifyGenericBridge(bridge, *target, budget, nullptr, nullptr, &classes,
                      &signatures, contract);
  return GenericBridge{target->reference, contract->reference};
}

} // namespace

namespace {
bool exactInterfaceDefaultForwarder(const Method &accessor, Budget &budget) {
  const auto &ref = accessor.reference;
  const auto &ops = accessor.instructions;
  const bool value = ref.returns != "V";
  const auto conversion = primitiveUnitConversion(ref.returns);
  const bool unboxed = conversion && ops.size() == 5 &&
                       ref.parameters == std::vector<std::string>{ref.owner};
  if (accessor.access != Access{"public", "static", "synthetic"} ||
      accessor.generic_signature || !accessor.tries.empty() ||
      (accessor.declared_throws && !accessor.declared_throws->empty()) ||
      ref.parameters.empty() || ref.parameters[0] != ref.owner ||
      (ops.size() != (value ? 3u : 2u) && !unboxed) ||
      accessor.registers < accessor.incomingWords())
    return false;
  const auto *dispatch = std::get_if<MethodRef>(&ops[0].reference);
  if (!dispatch || dispatch->owner != ref.owner ||
      dispatch->parameters !=
          std::vector<std::string>(ref.parameters.begin() + 1,
                                   ref.parameters.end()) ||
      dispatch->returns != (unboxed ? conversion->boxed : ref.returns) ||
      (ops[0].opcode != "invoke-super" &&
       ops[0].opcode != "invoke-super/range"))
    return false;
  std::vector<unsigned> words;
  for (unsigned i = accessor.registers - accessor.incomingWords();
       i < accessor.registers; ++i)
    words.push_back(i);
  if (ops[0].registers != words)
    return false;
  for (size_t i = 0; i < ops.size(); ++i) {
    budget.tick();
    const auto &op = ops[i];
    if ((i && op.pc <= ops[i - 1].pc) || op.pc >= accessor.code_end ||
        op.target || !op.targets.empty() || !op.keys.empty() ||
        !op.data.empty() || op.element_width ||
        !std::holds_alternative<std::monostate>(op.literal) ||
        (i && !(unboxed && i == 2) &&
         !std::holds_alternative<std::monostate>(op.reference)))
      return false;
    for (auto reg : op.registers)
      if (reg >= accessor.registers)
        return false;
  }
  if (!value)
    return ops[1].opcode == "return-void" && ops[1].registers.empty();
  if (unboxed) {
    // A scalar compatibility entry keeps the exact boxed default result,
    // original wrapper member and primitive return. The helper moves only
    // the default body; this complete unbox tail stays in the static entry.
    // Ordinary Body validation still owns every call, register and effect.
    auto original_unbox = conversion->unbox;
    original_unbox.owner = conversion->boxed;
    const auto *unbox = std::get_if<MethodRef>(&ops[2].reference);
    const bool wide = width(ref.returns) == 2;
    return ops[1].opcode == "move-result-object" &&
           ops[1].registers.size() == 1 &&
           (ops[2].opcode == "invoke-virtual" ||
            ops[2].opcode == "invoke-virtual/range") &&
           unbox && *unbox == original_unbox &&
           ops[2].registers == ops[1].registers &&
           ops[3].opcode == (wide ? "move-result-wide" : "move-result") &&
           ops[3].registers.size() == 1 &&
           (!wide || ops[3].registers[0] + 1 < accessor.registers) &&
           ops[4].opcode == (wide ? "return-wide" : "return") &&
           ops[4].registers == ops[3].registers;
  }
  const bool reference =
      ref.returns.starts_with('L') || ref.returns.starts_with('[');
  const bool wide = width(ref.returns) == 2;
  return ops[1].opcode == (reference ? "move-result-object"
                           : wide    ? "move-result-wide"
                                     : "move-result") &&
         ops[1].registers.size() == 1 &&
         ops[2].opcode == (reference ? "return-object"
                           : wide    ? "return-wide"
                                     : "return") &&
         ops[2].registers == ops[1].registers;
}

void planInterfaceDefaultAccessors(const ClassMap &classes,
                                   const GenericSignaturePlans &signatures,
                                   Budget &budget,
                                   GenericInheritancePlan &result) {
  std::set<std::string> reserved;
  for (const auto &[name, cls] : classes)
    for (const auto &method : cls.methods) {
      budget.tick();
      reserved.insert(method.reference.name);
    }
  for (const auto &[ref, alias] : result.source_method_names)
    reserved.insert(alias.name);
  for (const auto &[ref, name] : result.void_unit_targets)
    reserved.insert(name);
  unsigned suffix = 0;
  for (const auto &[name, cls] : classes) {
    budget.tick();
    const auto scope = signatures.classes.find(name);
    // The original body stays in this interface's semantic scope. Unused
    // class formals do not alter its nongeneric default or accessor slots.
    // Method formals need a complete canonical invocation receipt below;
    // fully closed method contracts remain intact on the private helper.
    // Other method Signatures and class projections stay unsupported.
    if (cls.external || !has(cls.access, "interface") ||
        has(cls.access, "annotation") || !independentSourceScope(cls) ||
        (scope != signatures.classes.end() &&
         !scope->second.source_projections.empty()))
      continue;
    for (const auto &accessor : cls.methods) {
      budget.tick();
      const auto &ref = accessor.reference;
      const auto &ops = accessor.instructions;
      if (!exactInterfaceDefaultForwarder(accessor, budget))
        continue;
      const auto *dispatch = std::get_if<MethodRef>(&ops[0].reference);
      const auto *target = dispatch ? findMethod(cls, *dispatch) : nullptr;
      const bool owner_formal_default = [&] {
        if (!dispatch || !target || scope == signatures.classes.end() ||
            scope->second.type_parameters.size() != 1 ||
            !scope->second.source_projections.empty())
          return false;
        const auto &formal = scope->second.type_parameters[0];
        if (!formal.class_bound || !formal.interface_bounds.empty())
          return false;
        const auto &bound = scope->second.types.at(*formal.class_bound);
        const bool predicate =
            bound.erasure == "Ljava/lang/Object;" &&
            dispatch->parameters ==
                std::vector<std::string>{"Ljava/lang/Object;",
                                         "Ljava/lang/Object;"} &&
            dispatch->returns == "Z";
        const bool ternary_result =
            bound.kind == GenericType::Kind::Class &&
            bound.segments.size() == 1 && bound.segments[0].arguments.empty() &&
            dispatch->parameters ==
                std::vector<std::string>(3, bound.erasure) &&
            (dispatch->returns == bound.erasure || dispatch->returns == "J");
        if (!predicate && !ternary_result)
          return false;
        const auto full = signatures.methods.find(*dispatch);
        if (full == signatures.methods.end() ||
            sourceMethodSignature(*dispatch, signatures) != &full->second ||
            !full->second.type_parameters.empty() ||
            !full->second.source_projections.empty() ||
            !full->second.throws_types.empty() ||
            full->second.parameters.size() != dispatch->parameters.size() ||
            !full->second.result)
          return false;
        const auto owner_formal = [&](GenericTypeId id) {
          const auto &type = full->second.types.at(id);
          return type.kind == GenericType::Kind::TypeVariable &&
                 type.variable_owner == name &&
                 type.variable_name == formal.name &&
                 type.erasure == bound.erasure;
        };
        const auto &returns = full->second.types.at(*full->second.result);
        const bool primitive_result = predicate || dispatch->returns == "J";
        if (primitive_result ? (returns.kind != GenericType::Kind::Primitive ||
                                returns.erasure != dispatch->returns)
                             : !owner_formal(*full->second.result))
          return false;
        return std::all_of(full->second.parameters.begin(),
                           full->second.parameters.end(), owner_formal);
      }();
      const bool scoped_default = [&] {
        if (!dispatch || !target || !target->generic_signature)
          return false;
        const auto full = signatures.methods.find(*dispatch);
        if (full == signatures.methods.end() ||
            sourceMethodSignature(*dispatch, signatures) != &full->second ||
            !full->second.source_projections.empty() ||
            !full->second.throws_types.empty() || !full->second.result ||
            full->second.parameters.size() != dispatch->parameters.size())
          return false;
        // The canonical erased call proof already binds every method formal
        // through its complete original input/result trees. In a nongeneric
        // owner the exact default body and its full method Signature can move
        // together to the private helper without introducing another scope
        // binding or adapting the original static accessor's argument words.
        if (!full->second.type_parameters.empty())
          return (scope == signatures.classes.end() ||
                  scope->second.type_parameters.empty()) &&
                 signatures.erased_generic_invocations.contains(*dispatch);
        // The common validator owns the complete type trees and bounds. A
        // void default can also keep its owner's original formals, including
        // nested callbacks and arrays: the private instance helper stays in
        // that exact scope. Its unique raw-receiver call forwards the original
        // descriptor words and has no generic result binding to infer.
        const bool owner_void = dispatch->returns == "V" &&
                                scope != signatures.classes.end() &&
                                scope->second.source_projections.empty();
        for (const auto &type : full->second.types) {
          budget.tick();
          if (type.kind != GenericType::Kind::TypeVariable)
            continue;
          if (!owner_void || type.variable_owner != name ||
              std::none_of(scope->second.type_parameters.begin(),
                           scope->second.type_parameters.end(),
                           [&](const GenericTypeParameter &formal) {
                             return formal.name == type.variable_name &&
                                    formal.erasure == type.erasure;
                           }))
            return false;
        }
        for (size_t i = 0; i < dispatch->parameters.size(); ++i) {
          budget.tick();
          if (full->second.types.at(full->second.parameters[i]).erasure !=
              dispatch->parameters[i])
            return false;
        }
        return full->second.types.at(*full->second.result).erasure ==
               dispatch->returns;
      }();
      if ((ops[0].opcode != "invoke-super" &&
           ops[0].opcode != "invoke-super/range") ||
          !dispatch || dispatch->owner != name || !target ||
          dispatch->parameters !=
              std::vector<std::string>(ref.parameters.begin() + 1,
                                       ref.parameters.end()) ||
          !has(target->access, "public") ||
          has(target->access, "static") || has(target->access, "private") ||
          has(target->access, "protected") || has(target->access, "native") ||
          has(target->access, "abstract") || has(target->access, "bridge") ||
          // A compiler-generated non-bridge default has an ordinary body.
          // Its synthetic flag is disclosed by the same source projection;
          // exact forwarding, complete contracts and Body remain mandatory.
          (has(target->access, "synthetic") &&
           target->access != Access{"public", "synthetic"}) ||
          (target->generic_signature && !owner_formal_default &&
           !scoped_default &&
           !signatures.erased_bounded_converter_invocations.contains(
               *dispatch)) ||
          target->instructions.empty() ||
          (target->declared_throws && !target->declared_throws->empty()) ||
          result.bridges.contains(*dispatch) ||
          result.retained_plain_bridges.contains(*dispatch) ||
          result.source_method_names.contains(*dispatch) ||
          result.void_unit_targets.contains(*dispatch))
        continue;
      result.interface_default_accessors.emplace(ref, *dispatch);
      if (result.interface_default_helpers.contains(*dispatch))
        continue;
      std::string helper;
      do {
        budget.tick();
        helper = "__neverdInterfaceDefault" + std::to_string(suffix++);
      } while (!reserved.insert(helper).second);
      result.interface_default_helpers.emplace(*dispatch, std::move(helper));
    }
  }
  // Resolve inherited compatibility accessors only after every owned default
  // accessor has its exact body receipt. A single direct interface chain keeps
  // default selection and the complete scoped type arguments unambiguous.
  for (const auto &[name, cls] : classes) {
    if (cls.external || !has(cls.access, "interface") ||
        !independentSourceScope(cls) || cls.interfaces.size() != 1)
      continue;
    for (const auto &accessor : cls.methods) {
      budget.tick();
      const auto &ref = accessor.reference;
      const auto &ops = accessor.instructions;
      const auto *dispatch =
          ops.empty() ? nullptr : std::get_if<MethodRef>(&ops[0].reference);
      if (result.interface_default_accessors.contains(ref) ||
          !exactInterfaceDefaultForwarder(accessor, budget) || !dispatch ||
          dispatch->returns != ref.returns ||
          findMethod(cls, *dispatch))
        continue;
      const Class *current = &cls;
      std::set<std::string> seen;
      while (current && seen.insert(current->name).second &&
             seen.size() <= 64) {
        budget.tick();
        if (current->interfaces.size() != 1)
          break;
        const auto parent = classes.find(current->interfaces[0]);
        if (parent == classes.end() || parent->second.external ||
            !has(parent->second.access, "interface") ||
            !independentSourceScope(parent->second))
          break;
        current = &parent->second;
        // A new static call must not introduce interface initialization
        // effects.
        if (std::any_of(current->methods.begin(), current->methods.end(),
                        [](const Method &method) {
                          return method.reference.name == "<clinit>";
                        }))
          break;
        auto inherited = *dispatch;
        inherited.owner = current->name;
        const auto *target = findMethod(*current, inherited);
        if (!target)
          continue;
        const auto bindings =
            sourceInterfaceBindings(cls, *current, classes, signatures, budget);
        const auto *own = inheritanceClassSignature(cls.name, signatures);
        const auto *ancestor =
            inheritanceClassSignature(current->name, signatures);
        if (!bindings || (own && !own->source_projections.empty()) ||
            (ancestor && !ancestor->source_projections.empty()))
          break;
        bool same = !own && !ancestor;
        if (own && ancestor && own->type_parameters.size() == 1 &&
            ancestor->type_parameters.size() == 1 &&
            bindings->contains(ancestor->type_parameters[0].name)) {
          const auto &formal = own->type_parameters[0];
          const auto &mapped = bindings->at(ancestor->type_parameters[0].name);
          same =
              formal.erasure == ancestor->type_parameters[0].erasure &&
              mapped.signature == "T[" + cls.name + "]" + formal.name + ";" &&
              formal.class_bound && ancestor->type_parameters[0].class_bound &&
              formal.interface_bounds.empty() &&
              ancestor->type_parameters[0].interface_bounds.empty() &&
              substitute(*own, *formal.class_bound, {}, name, budget, 0, true)
                      .signature ==
                  substitute(*ancestor,
                             *ancestor->type_parameters[0].class_bound, {},
                             name, budget, 0, true)
                      .signature;
        }
        if (!same)
          break;
        const MethodRef *forward = nullptr;
        for (const auto &[candidate, body] : result.interface_default_accessors)
          if (body == target->reference) {
            if (forward) {
              forward = nullptr;
              break;
            }
            forward = &candidate;
          }
        if (forward)
          result.inherited_interface_default_accessors.emplace(
              ref, std::pair{*dispatch, *forward});
        break;
      }
    }
  }
  // A descriptor-only inherited default need not have an ancestor static
  // compatibility entry. Resolve its complete owned interface graph once;
  // source can then qualify the direct parent from a private instance helper.
  // Unlike the static fallback above, this introduces no ancestor static call
  // and therefore needs no extra interface-initialization assumption.
  for (const auto &[name, cls] : classes) {
    const auto *owner = inheritanceClassSignature(name, signatures);
    const bool plain_owner_edge = [&] {
      if (!cls.generic_signature)
        return true;
      if (!owner || !owner->source_projections.empty() || !owner->superclass ||
          owner->interfaces.size() != 1 || cls.interfaces.size() != 1 ||
          owner->types.at(*owner->superclass).erasure != "Ljava/lang/Object;")
        return false;
      const auto &edge = owner->types.at(owner->interfaces[0]);
      return edge.kind == GenericType::Kind::Class &&
             edge.erasure == cls.interfaces[0] && edge.segments.size() == 1 &&
             edge.segments[0].arguments.empty();
    }();
    if (cls.external || !plain_owner_edge || !has(cls.access, "public") ||
        !has(cls.access, "interface") || has(cls.access, "annotation") ||
        !independentSourceScope(cls) || cls.interfaces.empty())
      continue;
    for (const auto &accessor : cls.methods) {
      budget.tick();
      const auto &ref = accessor.reference;
      const auto *dispatch =
          accessor.instructions.empty()
              ? nullptr
              : std::get_if<MethodRef>(&accessor.instructions[0].reference);
      if (result.interface_default_accessors.contains(ref) ||
          result.inherited_interface_default_accessors.contains(ref) ||
          !exactInterfaceDefaultForwarder(accessor, budget) || !dispatch ||
          dispatch->returns != ref.returns || findMethod(cls, *dispatch))
        continue;
      std::vector<const Class *> pending{&cls};
      const Method *target = nullptr;
      std::set<std::string> seen;
      bool complete = true;
      while (!pending.empty()) {
        budget.tick();
        const auto *current = pending.back();
        pending.pop_back();
        if (!seen.insert(current->name).second)
          continue;
        if (seen.size() > 128 || current->external ||
            (current->generic_signature && current != &cls) ||
            !has(current->access, "public") ||
            !has(current->access, "interface") ||
            has(current->access, "annotation") ||
            !independentSourceScope(*current) ||
            current->superclass != "Ljava/lang/Object;") {
          complete = false;
          break;
        }
        auto inherited = *dispatch;
        inherited.owner = current->name;
        if (!target)
          target = findMethod(*current, inherited);
        for (const auto &edge : current->interfaces) {
          budget.tick();
          const auto parent = classes.find(edge);
          if (parent == classes.end()) {
            complete = false;
            break;
          }
          pending.push_back(&parent->second);
        }
        if (!complete)
          break;
      }
      if (!complete || !target)
        continue;
      // The shared selector owns cycles, abstract shadowing and competing
      // declarations. The graph walk above only establishes source scope.
      target = uniqueInterfaceDefault(cls, classes.at(target->reference.owner),
                                      *target, *dispatch, cls.interfaces,
                                      classes, signatures, budget, true, true,
                                      &result.source_method_names);
      // Closed parameter trees need no formal binding. A scalar result also
      // needs no reference-result substitution. The common validator owns the
      // full original Signature, including every bound and nested argument.
      const GenericSignature *closed = [&]() -> const GenericSignature * {
        if (!target || !target->generic_signature)
          return nullptr;
        const auto full = signatures.methods.find(target->reference);
        if (full == signatures.methods.end() ||
            sourceMethodSignature(target->reference, signatures) !=
                &full->second ||
            !full->second.type_parameters.empty() ||
            !full->second.source_projections.empty() ||
            !full->second.throws_types.empty() || !full->second.result ||
            full->second.parameters.size() !=
                target->reference.parameters.size() ||
            std::any_of(full->second.types.begin(), full->second.types.end(),
                        [](const GenericType &type) {
                          return type.kind == GenericType::Kind::TypeVariable;
                        }))
          return nullptr;
        for (size_t i = 0; i < full->second.parameters.size(); ++i)
          if (full->second.types.at(full->second.parameters[i]).erasure !=
              target->reference.parameters[i])
            return nullptr;
        if (full->second.types.at(*full->second.result).erasure != ref.returns)
          return nullptr;
        return &full->second;
      }();
      const bool closed_void = closed && ref.returns == "V";
      const bool closed_scalar =
          closed &&
          closed->types.at(*closed->result).kind ==
              GenericType::Kind::Primitive &&
          std::all_of(closed->types.begin(), closed->types.end(),
                      [&](const GenericType &type) {
                        return type.kind != GenericType::Kind::Class ||
                               classes.contains(type.erasure);
                      }) &&
          std::any_of(closed->types.begin(), closed->types.end(),
                      [](const GenericType &type) {
                        return std::any_of(
                            type.segments.begin(), type.segments.end(),
                            [](const GenericClassSegment &segment) {
                              return !segment.arguments.empty();
                            });
                      });
      const bool erased_method_result = [&] {
        if (!target || target->reference.returns != "Ljava/lang/Object;" ||
            !target->generic_signature ||
            !signatures.erased_generic_invocations.contains(target->reference))
          return false;
        const auto full = signatures.methods.find(target->reference);
        if (full == signatures.methods.end() ||
            sourceMethodSignature(target->reference, signatures) !=
                &full->second ||
            full->second.type_parameters.empty() || !full->second.result ||
            !full->second.source_projections.empty() ||
            !full->second.throws_types.empty())
          return false;
        const auto &result = full->second.types.at(*full->second.result);
        if (result.kind != GenericType::Kind::TypeVariable ||
            result.variable_owner != target->reference.identity() ||
            result.erasure != "Ljava/lang/Object;")
          return false;
        // The invocation validator owns the complete input/result relation.
        // This consumer checks scope and forwards its exact original erasure;
        // a same-spelled class formal cannot become the method's binding.
        return std::all_of(
            full->second.types.begin(), full->second.types.end(),
            [&](const GenericType &type) {
              return type.kind == GenericType::Kind::TypeVariable
                         ? type.variable_owner == target->reference.identity()
                     : type.kind == GenericType::Kind::Class
                         ? type.erasure == "Ljava/lang/Object;" ||
                               classes.contains(type.erasure)
                         : true;
            });
      }();
      const bool fluent_descriptor =
          target && !target->generic_signature &&
          target->reference.returns == target->reference.owner &&
          target->reference.parameters ==
              std::vector<std::string>{target->reference.owner};
      if (!target || target->access != Access{"public"} ||
          (cls.generic_signature && !closed_scalar && !erased_method_result &&
           !fluent_descriptor) ||
          (target->generic_signature && !closed_void && !closed_scalar &&
           !erased_method_result) ||
          target->instructions.empty() ||
          (target->declared_throws && !target->declared_throws->empty()) ||
          result.bridges.contains(target->reference) ||
          result.retained_plain_bridges.contains(target->reference) ||
          result.source_method_names.contains(target->reference) ||
          result.void_unit_targets.contains(target->reference))
        continue;
      // A default's declaring owner can be an indirect ancestor. Prove the
      // unique legal direct qualifier separately, then share both decisions.
      std::optional<std::string> qualifier;
      for (const auto &root : cls.interfaces) {
        budget.tick();
        const bool redundant = std::any_of(
            cls.interfaces.begin(), cls.interfaces.end(),
            [&](const auto &other) {
              return other != root &&
                     isAssignableReference(other, root, classes, budget);
            });
        if (redundant || uniqueInterfaceDefault(
                             cls, classes.at(target->reference.owner), *target,
                             *dispatch, {root}, classes, signatures, budget,
                             true, true, &result.source_method_names) != target)
          continue;
        if (qualifier) {
          qualifier.reset();
          break;
        }
        qualifier = root;
      }
      if (qualifier) {
        result.scoped_interface_super_calls.emplace(std::pair{ref, *dispatch},
                                                    target->reference);
        result.static_interface_super_qualifiers.emplace(ref, *qualifier);
      }
    }
  }
}
// A class with a raw generic superinterface can declare both an erased input
// slot and a typed overload. Regenerating a casting bridge there would change
// an unconditional throw. Only result covariance needs javac to supply Code.
void planRawInterfaceSlots(const Class &cls, const ClassMap &classes,
                           const GenericSignaturePlans &signatures,
                           Budget &budget, GenericInheritancePlan &result) {
  if (cls.generic_signature || !independentSourceScope(cls))
    return;
  const Class *api = nullptr;
  for (const auto &edge : cls.interfaces) {
    if (emptyMarkerHierarchy(edge, classes, budget))
      continue;
    const auto shape = signatures.classes.find(edge);
    const auto found = classes.find(edge);
    if (api || found == classes.end() ||
        !has(found->second.access, "interface") ||
        shape == signatures.classes.end() ||
        shape->second.type_parameters.empty() ||
        !shape->second.source_projections.empty())
      return;
    api = &found->second;
  }
  // A lone functional contract has its own complete SAM bridge proof.
  if (!api || (cls.interfaces.size() == 1 && api->methods.size() == 1 &&
               onlyEmptyMarkerParents(*api, classes, budget)))
    return;
  for (const auto &bridge : cls.methods) {
    budget.tick();
    const auto &erased = bridge.reference;
    if (objectSourceMethodNames().contains(erased.name) ||
        !has(bridge.access, "public") || !has(bridge.access, "bridge") ||
        bridge.generic_signature || bridge.instructions.empty() ||
        result.source_method_names.contains(erased))
      continue;
    auto inherited = erased;
    inherited.owner = api->name;
    const auto *member = findMethod(*api, inherited);
    const bool indirect = !member;
    if (indirect) {
      // A raw generic interface inherits the erasures of its superinterfaces.
      // Require a complete graph and one abstract declaration of this name.
      // A nongeneric intermediate interface could fix the parent's arguments.
      std::vector<std::string> pending{api->name};
      std::set<std::string> seen;
      bool complete = true;
      while (complete && !pending.empty()) {
        budget.tick();
        auto name = std::move(pending.back());
        pending.pop_back();
        if (!seen.insert(name).second)
          continue;
        const auto parent = classes.find(name);
        const auto header = signatures.classes.find(name);
        if (parent == classes.end() || !has(parent->second.access, "interface") ||
            header == signatures.classes.end() || header->second.type_parameters.empty() ||
            !header->second.source_projections.empty()) {
          complete = false;
          break;
        }
        for (const auto &candidate : parent->second.methods) {
          budget.tick();
          if (candidate.reference.name != erased.name)
            continue;
          if (member || candidate.reference.parameters != erased.parameters ||
              candidate.reference.returns != erased.returns) {
            complete = false;
            break;
          }
          member = &candidate;
          inherited = candidate.reference;
        }
        pending.insert(pending.end(), parent->second.interfaces.begin(),
                       parent->second.interfaces.end());
      }
      if (!complete)
        continue;
    }
    if (!member || member->access != Access{"public", "abstract"} ||
        (member->declared_throws && !member->declared_throws->empty()))
      continue;
    const auto contract = signatures.methods.find(inherited);
    // Raw contracts expose their original reference result, including a
    // parameterized interface return. An explicit differently named getter
    // with that same descriptor retains the original virtual call in source.
    if (bridge.access == Access{"public", "final", "bridge"} &&
        !bridge.annotation_default &&
        (!bridge.declared_throws || bridge.declared_throws->empty()) &&
        (!member->generic_signature ||
         (contract != signatures.methods.end() &&
          contract->second.type_parameters.empty() &&
          contract->second.source_projections.empty() &&
          contract->second.throws_types.empty()))) {
      if (auto call = referenceGetterCall(cls, bridge, budget)) {
        const auto *target = findMethod(cls, *call);
        if (target && has(target->access, "public") &&
            !has(target->access, "static") && !has(target->access, "bridge") &&
            !has(target->access, "native") && !has(target->access, "abstract") &&
            !target->instructions.empty() && !target->generic_signature &&
            !result.source_method_names.contains(*call) &&
            (!target->declared_throws || target->declared_throws->empty())) {
          GenericInheritancePlan::RetainedBridge proof{*call};
          proof.reference_getter = true;
          result.retained_plain_bridges.emplace(erased, std::move(proof));
          continue;
        }
      }
    }
    if (!member->generic_signature) {
      if (auto target = guardedReferenceForwardingBridge(
              cls, bridge, *member, classes, signatures, budget)) {
        result.retained_plain_bridges.emplace(
            erased, GenericInheritancePlan::RetainedBridge{*target, true});
      } else if (auto target = plainPrimitiveForwardingBridge(
                     cls, bridge, *member, signatures, budget)) {
        GenericInheritancePlan::RetainedBridge proof{*target};
        proof.primitive_forwarding = true;
        result.retained_plain_bridges.emplace(erased, std::move(proof));
      }
      continue;
    }
    if (bridge.access != Access{"public", "bridge", "synthetic"} ||
        contract == signatures.methods.end() ||
        !contract->second.type_parameters.empty() ||
        !contract->second.throws_types.empty() ||
        !contract->second.source_projections.empty())
      continue;
    // A raw interface requires the erased parameter types even when the
    // implementation's result is covariant. Verify every forwarded word.
    if (const auto *call =
            std::get_if<MethodRef>(&bridge.instructions.front().reference);
        call && bridge.instructions.size() == 3 &&
        (bridge.instructions.front().opcode == "invoke-virtual" ||
         bridge.instructions.front().opcode == "invoke-virtual/range") &&
        call->owner == cls.name && call->name == erased.name &&
        call->parameters == erased.parameters && call->returns != erased.returns &&
        isAssignableReference(call->returns, erased.returns, classes, budget)) {
      const auto *target = findMethod(cls, *call);
      if (!target || target->generic_signature ||
          result.source_method_names.contains(*call) ||
          std::any_of(cls.methods.begin(), cls.methods.end(),
                      [&](const Method &candidate) {
                        const auto &ref = candidate.reference;
                        return ref.name == erased.name &&
                               ref.parameters == erased.parameters &&
                               ref != erased && ref != *call;
                      }))
        continue;
      verifyGenericBridge(bridge, *target, budget, nullptr, nullptr, &classes,
                          &signatures, member);
      result.bridges.emplace(erased, GenericBridge{*call, inherited});
      continue;
    }
    if (indirect && erased.returns == "V" &&
        contract->second.parameters.size() == erased.parameters.size()) {
      // Distinct raw and typed parameter lists can both be written in Java.
      // Preserve the erased implementation and its checked forwarding body.
      const Method *forward = nullptr;
      for (const auto &op : bridge.instructions) {
        budget.tick();
        const auto *callee = std::get_if<MethodRef>(&op.reference);
        if (!callee || callee->owner != cls.name || callee->name != erased.name)
          continue;
        forward = findMethod(cls, *callee);
        break;
      }
      if (forward && forward->reference.returns == "V" &&
          forward->reference.parameters.size() == erased.parameters.size() &&
          forward->reference.parameters != erased.parameters &&
          !forward->generic_signature && !result.source_method_names.contains(forward->reference)) {
        bool formalInputs = true;
        for (size_t i = 0; i < erased.parameters.size(); ++i) {
          budget.tick();
          if (erased.parameters[i] == forward->reference.parameters[i])
            continue;
          const auto &type = contract->second.types.at(contract->second.parameters[i]);
          formalInputs &= type.kind == GenericType::Kind::TypeVariable &&
                          (!type.variable_owner || type.variable_owner == inherited.owner) &&
                          isAssignableReference(forward->reference.parameters[i],
                                                erased.parameters[i], classes, budget);
        }
        if (formalInputs) {
          verifyGenericBridge(bridge, *forward, budget, nullptr, nullptr,
                              &classes, &signatures, member);
          GenericInheritancePlan::RetainedBridge proof{forward->reference};
          proof.raw_parameter_forwarding = true;
          proof.inherited_class_contract = inherited;
          result.retained_plain_bridges.emplace(erased, std::move(proof));
          continue;
        }
      }
    }
    const Method *target = nullptr;
    for (const auto &candidate : cls.methods) {
      budget.tick();
      if (candidate.reference.name != erased.name ||
          (candidate.reference.returns != erased.returns &&
           !isAssignableReference(candidate.reference.returns, erased.returns,
                                  classes, budget)) ||
          candidate.generic_signature ||
          result.source_method_names.contains(candidate.reference) ||
          !identicalTerminalThrowBridge(bridge, candidate, budget))
        continue;
      if (target) {
        target = nullptr;
        break;
      }
      target = &candidate;
    }
    if (!target)
      continue;
    if (erased.parameters == target->reference.parameters) {
      GenericBridge proof{target->reference, inherited};
      proof.terminal_throw_result = true;
      result.bridges.emplace(erased, std::move(proof));
    } else {
      result.retained_plain_bridges.emplace(
          erased, GenericInheritancePlan::RetainedBridge{
                      target->reference, false, true});
    }
  }
}

// An abstract implementation can inherit both the narrow and broad getters
// through a raw subinterface. A plain narrow result then implements the two
// erased contracts, and javac supplies the original result-only bridge.
std::optional<GenericBridge> rawCovariantInterfaceMember(
    const Class &cls, const Method &bridge, const ClassMap &classes,
    const GenericSignaturePlans &signatures, const GenericInheritancePlan &plan,
    Budget &budget) {
  const auto &erased = bridge.reference;
  if (bridge.access != Access{"public", "bridge", "synthetic"} ||
      !erased.returns.starts_with('L') ||
      std::any_of(erased.parameters.begin(), erased.parameters.end(),
                  [](const std::string &type) {
                    return type.starts_with('L') || type.starts_with('[');
                  }) ||
      (bridge.instructions.size() != 3 && bridge.instructions.size() != 4) ||
      bridge.generic_signature ||
      objectSourceMethodNames().contains(erased.name) ||
      plan.source_method_names.contains(erased))
    return {};
  const auto &invoke = bridge.instructions.front();
  const auto *call = std::get_if<MethodRef>(&invoke.reference);
  if (!call || (invoke.opcode != "invoke-virtual" && invoke.opcode != "invoke-virtual/range") ||
      call->owner != cls.name || call->name != erased.name ||
      call->parameters != erased.parameters || !call->returns.starts_with('L') ||
      call->returns == erased.returns ||
      !isAssignableReference(call->returns, erased.returns, classes, budget))
    return {};
  const auto *target = findMethod(cls, *call);
  if (!target || target->access != Access{"public"} || target->generic_signature ||
      target->instructions.empty() || plan.source_method_names.contains(*call))
    return {};
  // Check every inherited declaration of this source slot. A class member,
  // typed edge or unrelated third return needs a different override proof.
  std::vector<std::string> pending = cls.interfaces;
  std::set<std::string> seen;
  for (auto parent = cls.superclass; parent && *parent != "Ljava/lang/Object;";) {
    budget.tick();
    const auto c = classes.find(*parent);
    if (c == classes.end() || !seen.insert(*parent).second ||
        c->second.generic_signature || !c->second.superclass ||
        std::any_of(c->second.methods.begin(), c->second.methods.end(),
                    [&](const Method &m) { return m.reference.name == erased.name; }))
      return {};
    pending.insert(pending.end(), c->second.interfaces.begin(), c->second.interfaces.end());
    parent = c->second.superclass;
  }
  const Method *required = nullptr, *narrow = nullptr;
  while (!pending.empty()) {
    budget.tick();
    auto name = std::move(pending.back());
    pending.pop_back();
    if (!seen.insert(name).second)
      continue;
    const auto owner = classes.find(name);
    if (owner == classes.end() || !has(owner->second.access, "interface"))
      return {};
    const auto header = signatures.classes.find(name);
    if (header == signatures.classes.end()) {
      // A Signature-free declaration introduces no parameterized parent.
      if (owner->second.generic_signature)
        return {};
    } else if (header->second.type_parameters.empty() ||
               !header->second.source_projections.empty()) {
      return {};
    }
    const auto &iface = owner->second;
    for (const auto &member : iface.methods) {
      budget.tick();
      const auto &slot = member.reference;
      if (slot.name != erased.name || slot.parameters != erased.parameters)
        continue;
      const auto shape = signatures.methods.find(slot);
      if (header == signatures.classes.end() ||
          member.access != Access{"public", "abstract"} ||
          shape == signatures.methods.end() || !shape->second.result ||
          !shape->second.type_parameters.empty() || !shape->second.throws_types.empty() ||
          !shape->second.source_projections.empty() ||
          (member.declared_throws && !member.declared_throws->empty()))
        return {};
      const auto &result = shape->second.types.at(*shape->second.result);
      if (result.kind != GenericType::Kind::Class || result.erasure != slot.returns ||
          (slot.returns != erased.returns && slot.returns != call->returns))
        return {};
      auto &match = slot.returns == erased.returns ? required : narrow;
      if (match)
        return {};
      match = &member;
    }
    pending.insert(pending.end(), iface.interfaces.begin(), iface.interfaces.end());
  }
  if (!required)
    return {};
  if (narrow) {
    if (!isAssignableReference(narrow->reference.owner, required->reference.owner,
                               classes, budget))
      return {};
  } else if (!has(cls.access, "final") || call->returns != cls.name ||
             std::find(cls.interfaces.begin(), cls.interfaces.end(), erased.returns) ==
                 cls.interfaces.end()) {
    // A final implementation may narrow an erased interface result to
    // itself. Its raw header proves the covariant class-to-interface return;
    // primitive inputs cannot be specialized by a class formal.
    return {};
  }
  verifyGenericBridge(bridge, *target, budget, nullptr, nullptr, &classes,
                      &signatures, required);
  return GenericBridge{target->reference, required->reference};
}

// A raw interface owns its original erased entry without invented arguments.
// Input adapters retain their bodies; closed zero-input reference covariance
// can regenerate the exact bridge. The common verifier owns casts, origins,
// conversions, dispatch and post-call result handling in both profiles.
void planRawCallbackBridges(const ClassMap &classes,
                            const GenericSignaturePlans &signatures,
                            Budget &budget, GenericInheritancePlan &result) {
  const auto unit = unitResultField(classes, budget);
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (!cls.external && !has(cls.access, "interface") && !cls.generic_signature &&
        independentSourceScope(cls) &&
        ((has(cls.access, "abstract") && cls.interfaces.size() == 1) ||
         (has(cls.access, "final") && !cls.interfaces.empty())))
      for (const auto &method : cls.methods)
        if (auto proof = rawCovariantInterfaceMember(cls, method, classes, signatures, result, budget))
          result.bridges.emplace(method.reference, std::move(*proof));
    if (cls.external || !has(cls.access, "final") ||
        has(cls.access, "interface") ||
        cls.superclass != "Ljava/lang/Object;" || cls.interfaces.empty())
      continue;
    planRawInterfaceSlots(cls, classes, signatures, budget, result);
    if (cls.interfaces.size() != 1)
      continue;
    const auto *owner = inheritanceClassSignature(name, signatures);
    if (owner) {
      if (!owner->type_parameters.empty() ||
          !owner->source_projections.empty() || owner->interfaces.size() != 1)
        continue;
      const auto &edge = owner->types.at(owner->interfaces[0]);
      if (edge.erasure != cls.interfaces[0] || edge.segments.size() != 1 ||
          !edge.segments[0].arguments.empty())
        continue;
    }
    const auto api = classes.find(cls.interfaces[0]);
    const auto api_shape = signatures.classes.find(cls.interfaces[0]);
    if (api == classes.end() || api_shape == signatures.classes.end() ||
        !has(api->second.access, "public") ||
        !has(api->second.access, "interface") ||
        api->second.superclass != "Ljava/lang/Object;" ||
        !independentSourceScope(api->second) ||
        api->second.methods.size() != 1 ||
        !onlyEmptyMarkerParents(api->second, classes, budget) ||
        !api_shape->second.source_projections.empty())
      continue;
    const auto &declaration = api->second.methods[0];
    const auto shape = signatures.methods.find(declaration.reference);
    const auto count = declaration.reference.parameters.size();
    if (declaration.access != Access{"public", "abstract"} || count > 22 ||
        declaration.reference.name.starts_with('<') ||
        declaration.reference.returns != "Ljava/lang/Object;" ||
        (declaration.declared_throws &&
         !declaration.declared_throws->empty()) ||
        shape == signatures.methods.end() ||
        !shape->second.type_parameters.empty() ||
        !shape->second.throws_types.empty() ||
        !shape->second.source_projections.empty() || !shape->second.result ||
        shape->second.parameters.size() != count ||
        api_shape->second.type_parameters.size() != count + 1)
      continue;
    std::set<std::string> formals, used;
    bool exact = true;
    for (const auto &formal : api_shape->second.type_parameters) {
      budget.tick();
      if (!formal.class_bound || !formal.interface_bounds.empty()) {
        exact = false;
        break;
      }
      const auto &bound = api_shape->second.types.at(*formal.class_bound);
      exact &= bound.kind == GenericType::Kind::Class &&
               bound.package == "java/lang" && bound.segments.size() == 1 &&
               bound.segments[0].name == "Object" &&
               bound.segments[0].arguments.empty();
      formals.insert(formal.name);
    }
    const auto formal_use = [&](GenericTypeId id) {
      const auto &type = shape->second.types.at(id);
      // External declarations can retain unresolved type variables. This
      // method has no own formals: each use must name a distinct API formal
      // whose only bound above is exactly Object, proving the raw erasure.
      return type.kind == GenericType::Kind::TypeVariable &&
             (!type.variable_owner || type.variable_owner == api->first) &&
             formals.contains(type.variable_name) &&
             used.insert(type.variable_name).second;
    };
    for (size_t i = 0; i < count; ++i) {
      budget.tick();
      exact &= declaration.reference.parameters[i] == "Ljava/lang/Object;" &&
               formal_use(shape->second.parameters[i]);
    }
    if (!exact || !formal_use(*shape->second.result))
      continue;
    auto erased = declaration.reference;
    erased.owner = name;
    const auto *bridge = findMethod(cls, erased);
    if (!bridge || bridge->access != Access{"public", "bridge", "synthetic"} ||
        bridge->generic_signature || bridge->instructions.size() > 128)
      continue;
    std::optional<MethodRef> dispatch;
    for (const auto &op : bridge->instructions) {
      budget.tick();
      if (op.opcode != "invoke-virtual" && op.opcode != "invoke-virtual/range")
        continue;
      const auto *call = std::get_if<MethodRef>(&op.reference);
      if (!call) {
        exact = false;
        break;
      }
      if (call->owner != name || call->name != erased.name)
        continue;
      if (dispatch) {
        exact = false;
        break;
      }
      dispatch = *call;
    }
    if (!exact || !dispatch || dispatch->parameters.size() != count ||
        (count && dispatch->parameters == erased.parameters &&
         dispatch->returns != "Ljava/lang/Void;"))
      continue;
    const auto *target = findMethod(cls, *dispatch);
    if (!target || !has(target->access, "public") ||
        has(target->access, "static") || has(target->access, "abstract") ||
        has(target->access, "native") || has(target->access, "bridge") ||
        target->instructions.empty() ||
        (target->declared_throws && !target->declared_throws->empty()))
      continue;
    if (dispatch->parameters == erased.parameters) {
      const auto returned = classes.find(dispatch->returns);
      const auto result_shape = signatures.classes.find(dispatch->returns);
      // Object overrides and additional return-only slots need their own
      // inherited-slot/alias receipts. This profile owns just one typed entry
      // and its original Object bridge, independent of method traversal order.
      if (objectSourceMethodNames().contains(dispatch->name) ||
          std::any_of(cls.methods.begin(), cls.methods.end(),
                      [&](const Method &candidate) {
                        return candidate.reference.name == dispatch->name &&
                               candidate.reference.parameters == erased.parameters &&
                               candidate.reference != target->reference &&
                               candidate.reference != bridge->reference;
                      }) ||
          !dispatch->returns.starts_with('L') ||
          dispatch->returns == "Ljava/lang/Object;" ||
          returned == classes.end() ||
          !has(returned->second.access, "public") ||
          !independentSourceScope(returned->second) ||
          target->generic_signature ||
          signatures.methods.contains(target->reference) ||
          (result_shape != signatures.classes.end() &&
           (!result_shape->second.type_parameters.empty() ||
            !result_shape->second.source_projections.empty())) ||
          std::any_of(
              cls.fields.begin(), cls.fields.end(), [](const auto &field) {
                return !has(field.access, "static") || field.generic_signature;
              }))
        continue;
      // With unchanged inputs, a raw API still owns Object's return slot. Its
      // Object-bounded result formal and the closed nominal target
      // let javac regenerate this exact covariant bridge without inventing
      // owner arguments or assuming anything about the target's result/body.
      verifyGenericBridge(*bridge, *target, budget, nullptr, nullptr, &classes,
                          &signatures, &declaration);
      result.bridges.emplace(erased,
                             GenericBridge{*dispatch, declaration.reference});
      continue;
    }
    GenericInheritancePlan::RetainedBridge retained{*dispatch};
    if (dispatch->returns == "V") {
      if (!unit || target->generic_signature)
        continue;
      const auto conversion = primitiveBridgeConversion("I", classes);
      std::set<size_t> integers;
      for (size_t i = 0; i < count; ++i) {
        budget.tick();
        const auto &type = dispatch->parameters[i];
        if (type == "I" && conversion)
          integers.insert(i);
        else if (!type.starts_with('L') || !classes.contains(type))
          exact = false;
      }
      if (!exact)
        continue;
      // Reference casts and original Number.intValue calls compose with the
      // same post-delegation Unit read. The shared bridge verifier proves
      // their complete origin/order relation; a raw API must not acquire an
      // Integer-only cast or an inferred generic interface specialization.
      verifyGenericBridge(*bridge, *target, budget, &*unit,
                          integers.empty() ? nullptr : &integers, &classes,
                          &signatures, &declaration);
      retained.raw_unit_result = *unit;
      if (!integers.empty()) {
        retained.raw_indexed_parameters = std::move(integers);
        retained.unbox_method = conversion->unbox;
      }
    } else {
      const auto full = signatures.methods.find(*dispatch);
      const auto returned = classes.find(dispatch->returns);
      if (!target->generic_signature && full == signatures.methods.end() &&
          target->access == Access{"public", "final"} && count == 1 &&
          dispatch->returns == "Ljava/lang/Void;" &&
          std::all_of(cls.fields.begin(), cls.fields.end(),
                      [&](const Field &field) {
                        return field.access == Access{"public", "static", "final"} &&
                               field.reference.type == name && !field.generic_signature;
                      }) &&
          dispatch->parameters.front().starts_with('L') &&
          dispatch->parameters.front() != erased.parameters.front()) {
        const auto checked = classes.find(dispatch->parameters.front());
        if (checked != classes.end() && has(checked->second.access, "public") &&
            !checked->second.generic_signature && independentSourceScope(checked->second)) {
          // A raw functional interface needs the distinct Object entry in
          // source. Its original check-cast and virtual call must both remain.
          verifyGenericBridge(*bridge, *target, budget, nullptr, nullptr, &classes,
                              &signatures, &declaration);
          retained.raw_parameter_forwarding = true;
          retained.inherited_class_contract = declaration.reference;
          result.retained_plain_bridges.emplace(erased, std::move(retained));
          continue;
        }
      }
      const auto conversion = primitiveBridgeConversion("I", classes);
      if (!cls.enclosing_method || cls.generic_signature || !conversion ||
          std::any_of(cls.fields.begin(), cls.fields.end(),
                      [](const auto &field) {
                        return !has(field.access, "static") ||
                               field.generic_signature;
                      }) ||
          returned == classes.end() ||
          dispatch->returns == "Ljava/lang/Object;" ||
          !has(returned->second.access, "public") ||
          !independentSourceScope(returned->second))
        continue;
      std::set<size_t> integers;
      if (!target->generic_signature) {
        const auto result_shape = signatures.classes.find(dispatch->returns);
        if (full != signatures.methods.end() ||
            (result_shape != signatures.classes.end() &&
             (!result_shape->second.type_parameters.empty() ||
              !result_shape->second.source_projections.empty())))
          continue;
        // No method scope is erased in this profile. Retain each original
        // int/Object slot and its explicit Number conversion instead of
        // inventing Integer interface arguments and a narrower bridge.
        for (size_t i = 0; i < count; ++i) {
          budget.tick();
          if (dispatch->parameters[i] == "I")
            integers.insert(i);
          else if (dispatch->parameters[i] != "Ljava/lang/Object;")
            exact = false;
        }
        if (!exact || integers.empty())
          continue;
        retained.raw_integer_result = true;
      } else {
        if (full == signatures.methods.end() ||
            sourceMethodSignature(*dispatch, signatures) != &full->second ||
            !full->second.type_parameters.empty() ||
            !full->second.throws_types.empty() || !full->second.result ||
            full->second.parameters.size() != count ||
            full->second.source_projections.empty())
          continue;
        const auto &output = full->second.types.at(*full->second.result);
        if (output.kind != GenericType::Kind::Class ||
            output.erasure != dispatch->returns ||
            output.segments.size() != 1 ||
            !output.segments[0].arguments.empty())
          continue;
        std::set<size_t> scoped_offsets;
        const auto scopedObject = [&](const GenericType &type) {
          if (type.kind != GenericType::Kind::Class ||
              type.erasure != "Ljava/lang/Object;" || type.segments.size() != 1 ||
              !type.segments.front().arguments.empty())
            return false;
          scoped_offsets.insert(type.offset);
          return true;
        };
        for (size_t i = 0; i < count; ++i) {
          budget.tick();
          const auto &type = full->second.types.at(full->second.parameters[i]);
          const auto &descriptor = dispatch->parameters[i];
          if (type.kind == GenericType::Kind::Primitive &&
              (descriptor == "I" || descriptor == "J") &&
              type.primitive == descriptor.front()) {
            const auto conversion = primitiveBridgeConversion(descriptor, classes);
            if (!conversion) {
              exact = false;
              break;
            }
            integers.insert(i);
            retained.raw_parameter_conversions.emplace(i, conversion->unbox);
          } else if (descriptor == "Ljava/lang/Object;") {
            exact &= scopedObject(type);
          } else if (type.kind == GenericType::Kind::Class &&
                     type.erasure == descriptor && classes.contains(descriptor) &&
                     type.segments.size() == 1 &&
                     type.segments.front().arguments.size() == 1) {
            // A captured formal can occur inside a nominal parameter. Its
            // erasure leaves the bridge's original check-cast on that carrier.
            const auto &arg = type.segments.front().arguments.front();
            exact &= arg.variance == GenericTypeArgument::Variance::Invariant &&
                     arg.type && scopedObject(full->second.types.at(*arg.type));
          } else {
            exact = false;
          }
        }
        if (!exact || integers.empty() || scoped_offsets.empty() ||
            scoped_offsets.size() != full->second.source_projections.size())
          continue;
        // resolve/defaultScope owns these Object-bound erasures and their
        // witnessed method origin. Compose only its exact parameter receipts.
        // The raw entry must stay explicit: a regenerated Integer bridge would
        // narrow the original Number.intValue contract and its failure order.
        for (const auto &projection : full->second.source_projections) {
          budget.tick();
          exact &=
              (projection.kind == "synthetic-default-method-type-variable" ||
               (projection.kind == "synthetic-default-receiver-type-variable" &&
                projection.scope_origin == cls.enclosing_method->owner)) &&
              projection.source_type == "Ljava/lang/Object;" &&
              projection.scope_origin &&
              projection.original_type.starts_with(
                  cls.enclosing_method->identity() + "::") &&
              scoped_offsets.erase(projection.offset) == 1;
        }
        if (!exact || !scoped_offsets.empty())
          continue;
      }
      verifyGenericBridge(*bridge, *target, budget, nullptr, &integers,
                          &classes, &signatures, &declaration);
      retained.raw_indexed_parameters = std::move(integers);
      if (retained.raw_parameter_conversions.empty())
        retained.unbox_method = conversion->unbox;
      else {
        const auto &first = retained.raw_parameter_conversions.begin()->second;
        if (std::all_of(retained.raw_parameter_conversions.begin(),
                        retained.raw_parameter_conversions.end(),
                        [&](const auto &entry) { return entry.second == first; }))
          retained.unbox_method = first;
      }
    }
    retained.inherited_class_contract = declaration.reference;
    result.retained_plain_bridges.emplace(erased, std::move(retained));
  }
}
} // namespace

GenericInheritancePlan
planGenericInheritance(const ClassMap &classes,
                       const GenericSignaturePlans &signatures, Budget &budget,
                       const EnumPlans *enums) {
  const auto owned_enums =
      enums ? EnumPlans{} : planEnumClasses(classes, budget);
  const auto &enum_plans = enums ? *enums : owned_enums;
  GenericInheritancePlan result;
  result.erased_void_parameters = signatures.erased_void_parameters;
  result.source_method_names = planSelfReturnNames(classes, signatures, budget);
  planCheckedReferenceReturnNames(classes, signatures, budget, result);
  planPrivateProducerReturnNames(classes, signatures, budget, result);
  planFinalVoidReturnNames(classes, signatures, budget, result);
  planFinalGetterReturnNames(classes, signatures, budget, result);
  planBoxedBooleanReturnNames(classes, signatures, budget, result);
  planPlainClassReturnNames(classes, signatures, budget, result);
  planLocalReferenceAdapterReturnNames(classes, signatures, budget, result);
  planStaticReturnNames(classes, signatures, budget, result);
  planRawCallbackBridges(classes, signatures, budget, result);
  for (const auto &[name, cls] : classes)
    for (const auto &method : cls.methods)
      if (auto target = plainInterfaceSuperForwarder(cls, method, classes,
                                                     signatures, budget)) {
        GenericInheritancePlan::RetainedBridge retained{*target};
        retained.interface_super_forwarder = true;
        result.retained_plain_bridges.emplace(method.reference,
                                              std::move(retained));
      }
  for (const auto &[pair, proof] : signatures.raw_interface_parameter_bridges) {
    GenericInheritancePlan::RetainedBridge retained{proof.target};
    retained.raw_parameter_forwarding = true;
    retained.inherited_class_contract = pair.second;
    result.retained_plain_bridges.emplace(pair.first, std::move(retained));
  }
  for (const auto &[bridge, proof] :
       signatures.closed_reference_getter_bridges) {
    GenericInheritancePlan::RetainedBridge retained{proof.target};
    retained.closed_reference_getter = true;
    // The original raw source return already overrides its closed ancestor.
    // Keep its absent Signature; use the target's full type only as proof.
    result.retained_plain_bridges.emplace(bridge, std::move(retained));
  }
  if (!signatures.nonreturning_void_bridges.empty()) {
    std::set<std::string> void_target_names;
    for (const auto &[name, cls] : classes)
      for (const auto &method : cls.methods) {
        budget.tick();
        void_target_names.insert(method.reference.name);
      }
    for (const auto &[ref, alias] : result.source_method_names)
      void_target_names.insert(alias.name);
    for (const auto &[bridge, proof] : signatures.nonreturning_void_bridges) {
      std::string spelling = "__neverdNonReturningTarget";
      while (void_target_names.contains(spelling)) {
        budget.tick();
        spelling += '_';
      }
      void_target_names.insert(spelling);
      GenericInheritancePlan::SourceMethodName alias{
          spelling, proof.target, {}};
      alias.nonreturning_void_bridge = bridge;
      if (!result.source_method_names.emplace(proof.target, std::move(alias))
               .second)
        unsupported(bridge.identity(),
                    "nonreturning void target has conflicting source names");
      GenericInheritancePlan::RetainedBridge retained{proof.target};
      retained.nonreturning_void = true;
      retained.source_signature = proof.source_signature;
      result.retained_plain_bridges.emplace(bridge, std::move(retained));
    }
  }
  if (!signatures.nominal_boxed_results.empty()) {
    std::set<std::string> occupied;
    for (const auto &[name, cls] : classes)
      for (const auto &method : cls.methods) {
        budget.tick();
        occupied.insert(method.reference.name);
      }
    for (const auto &[ref, alias] : result.source_method_names)
      occupied.insert(alias.name);
    for (const auto &[bridge, proof] : signatures.nominal_boxed_results) {
      if (proof.target.name == bridge.name) {
        auto alias = result.source_method_names.find(proof.target);
        if (alias == result.source_method_names.end()) {
          std::string spelling = "__neverdReturnSlot";
          while (occupied.contains(spelling)) {
            budget.tick();
            spelling += '_';
          }
          occupied.insert(spelling);
          alias = result.source_method_names
                      .emplace(proof.target,
                               GenericInheritancePlan::SourceMethodName{
                                   spelling, proof.target, proof.contracts})
                      .first;
        } else if (alias->second.declaration != proof.target)
          unsupported(bridge.identity(),
                      "boxed scalar return slot has a competing source name");
        alias->second.conflicting_contracts.insert(proof.contracts.begin(),
                                                   proof.contracts.end());
      }
      if (proof.forward_to) {
        GenericBridge forwarding{*proof.forward_to, *proof.contracts.begin()};
        forwarding.boxed_reference_result = true;
        result.bridges.emplace(bridge, std::move(forwarding));
      } else {
        GenericInheritancePlan::RetainedBridge retained{proof.target};
        retained.boxed_primitive_getter = true;
        retained.nominal_boxed_result = true;
        retained.box_method = proof.box;
        retained.inherited_class_contract = *proof.contracts.begin();
        result.retained_plain_bridges.emplace(bridge, std::move(retained));
      }
    }
  }
  for (const auto &[pair, proof] :
       signatures.inherited_checked_result_bridges) {
    auto [entry, inserted] = result.bridges.emplace(
        pair.first, GenericBridge{proof.target, pair.second});
    if (!inserted && entry->second.target != proof.target)
      unsupported(
          pair.first.identity(),
          "inherited checked result has conflicting original providers");
  }
  for (const auto &[pair, proof] : signatures.unboxed_predicate_bridges) {
    GenericInheritancePlan::RetainedBridge retained{proof.target};
    retained.unboxed_predicate = true;
    retained.unbox_method = proof.unbox;
    retained.inherited_class_contract = pair.second;
    result.retained_plain_bridges.emplace(pair.first, std::move(retained));
  }
  for (const auto &[pair, proof] : signatures.erased_terminal_consumers) {
    GenericInheritancePlan::RetainedBridge retained{pair.first};
    retained.contract_implementation = true;
    retained.erased_terminal_consumer = true;
    retained.inherited_class_contract = pair.second;
    result.retained_plain_bridges.emplace(pair.first, std::move(retained));
  }
  for (const auto &[ref, proof] :
       signatures.inherited_wildcard_parameter_sources) {
    GenericInheritancePlan::RetainedBridge retained{ref};
    retained.erased_terminal_member = true;
    retained.inherited_wildcard_parameter = true;
    retained.source_signature = proof.signature;
    retained.inherited_class_contract = proof.contract;
    result.retained_plain_bridges.emplace(ref, std::move(retained));
  }
  for (const auto &[ref, proof] : signatures.raw_interface_method_sources) {
    GenericInheritancePlan::RetainedBridge retained{ref};
    retained.erased_terminal_member = true;
    retained.raw_interface_method_formals = true;
    retained.source_signature = proof.signature;
    retained.inherited_class_contract = proof.contract;
    result.retained_plain_bridges.emplace(ref, std::move(retained));
  }
  for (const auto &[ref, proof] : signatures.erased_terminal_member_sources) {
    GenericInheritancePlan::RetainedBridge retained{ref};
    retained.erased_terminal_member = true;
    retained.source_signature = proof.signature;
    retained.inherited_class_contract = proof.contract;
    result.retained_plain_bridges.emplace(ref, std::move(retained));
  }
  std::set<std::string> boxed_return_names;
  if (std::any_of(signatures.boxed_interface_results.begin(),
                  signatures.boxed_interface_results.end(),
                  [](const auto &entry) {
                    return entry.second.typed_entry.has_value();
                  })) {
    for (const auto &[name, cls] : classes)
      for (const auto &method : cls.methods) {
        budget.tick();
        boxed_return_names.insert(method.reference.name);
      }
    for (const auto &[reference, alias] : result.source_method_names)
      boxed_return_names.insert(alias.name);
  }
  for (const auto &[pair, proof] : signatures.boxed_interface_results) {
    if (proof.preserve_typed_interface) {
      GenericBridge bridge{*proof.typed_entry, pair.second};
      bridge.boxed_reference_result = true;
      result.bridges.emplace(pair.first, std::move(bridge));
      continue;
    }
    GenericInheritancePlan::RetainedBridge retained{proof.target};
    retained.boxed_primitive_getter = true;
    retained.boxed_interface_result = true;
    retained.box_method = proof.box;
    retained.inherited_class_contract = pair.second;
    result.retained_plain_bridges.emplace(pair.first, std::move(retained));
    if (proof.typed_entry) {
      std::string spelling = "__neverdReturnSlot";
      while (boxed_return_names.contains(spelling)) {
        budget.tick();
        spelling += '_';
      }
      boxed_return_names.insert(spelling);
      if (!result.source_method_names
               .emplace(*proof.typed_entry,
                        GenericInheritancePlan::SourceMethodName{
                            spelling, *proof.typed_entry, {pair.second}})
               .second)
        unsupported(pair.first.identity(),
                    "boxed typed return slot has a competing source name");
    }
  }
  for (const auto &[pair, proof] : signatures.boxed_superclass_results) {
    GenericInheritancePlan::RetainedBridge retained{proof.target};
    retained.boxed_primitive_getter = true;
    retained.boxed_superclass_result = true;
    retained.box_method = proof.box;
    retained.inherited_class_contract = pair.second;
    result.retained_plain_bridges.emplace(pair.first, std::move(retained));
  }
  for (const auto &[pair, proof] :
       signatures.checked_inlined_interface_bridges) {
    GenericInheritancePlan::RetainedBridge retained{pair.first};
    retained.contract_implementation = true;
    retained.checked_inlined_interface = true;
    retained.inherited_class_contract = pair.second;
    result.retained_plain_bridges.emplace(pair.first, std::move(retained));
  }
  for (const auto &[reference, proof] :
       signatures.public_superclass_access_bridges) {
    if (proof.regenerate_public_access) {
      GenericBridge bridge{proof.target, reference};
      bridge.public_superclass_access = true;
      result.bridges.emplace(reference, std::move(bridge));
      continue;
    }
    GenericInheritancePlan::RetainedBridge retained{reference};
    retained.contract_implementation = true;
    retained.public_superclass_access = true;
    retained.inherited_class_contract = proof.target;
    result.retained_plain_bridges.emplace(reference, std::move(retained));
  }
  for (const auto &[reference, call] : signatures.superclass_scalar_adapters) {
    GenericInheritancePlan::RetainedBridge retained{call};
    retained.superclass_scalar_adapter = true;
    result.retained_plain_bridges.emplace(reference, std::move(retained));
  }
  for (const auto &[reference, call] :
       signatures.superclass_reference_adapters) {
    GenericInheritancePlan::RetainedBridge retained{call};
    retained.superclass_reference_adapter = true;
    result.retained_plain_bridges.emplace(reference, std::move(retained));
  }
  for (const auto &[reference, proof] : signatures.guarded_default_results) {
    for (const auto &entry : {reference, proof.wrapper}) {
      GenericInheritancePlan::RetainedBridge retained{proof.typed};
      retained.guarded_default_result = true;
      retained.inherited_class_contract = proof.dispatch;
      result.retained_plain_bridges.emplace(entry, std::move(retained));
    }
  }
  for (const auto &[reference, call] : signatures.superclass_getter_results) {
    GenericInheritancePlan::RetainedBridge retained{call};
    retained.superclass_getter = true;
    result.retained_plain_bridges.emplace(reference, std::move(retained));
  }
  for (const auto &[reference, proof] : signatures.superclass_scalar_getters) {
    GenericInheritancePlan::RetainedBridge retained{proof.first};
    retained.superclass_getter = true;
    result.retained_plain_bridges.emplace(reference, std::move(retained));
    result.retained_plain_bridges.emplace(
        proof.second, GenericInheritancePlan::RetainedBridge{reference});
  }
  // A certified plain scalar-super family can also contain final wrappers
  // around ordinary scalar conversion methods. Reuse the complete nongeneric
  // ancestor obligations and the same exact call/move/return receipt; no raw
  // generic superclass edge or bridge flag supplies a member contract.
  std::set<std::string> scalar_owners;
  for (const auto &[reference, proof] : signatures.superclass_scalar_getters)
    scalar_owners.insert(reference.owner);
  for (const auto &name : scalar_owners) {
    const auto &cls = classes.at(name);
    if (cls.generic_signature || signatures.classes.contains(name) ||
        !independentSourceScope(cls) ||
        !plainRuntimeClassOwner(cls, classes, signatures, budget,
                                &result.source_method_names))
      continue;
    std::set<std::string> visited;
    auto current = cls.superclass;
    while (current && *current != "Ljava/lang/Object;") {
      budget.tick();
      if (!visited.insert(*current).second || visited.size() > 64)
        unsupported(name, "plain scalar bridge ancestry is cyclic or too deep");
      const auto ancestor = classes.find(*current);
      if (ancestor == classes.end())
        unsupported(name,
                    "plain scalar bridge ancestor declaration is missing");
      for (const auto &member : ancestor->second.methods) {
        if (!has(member.access, "public") || has(member.access, "static") ||
            has(member.access, "final") || member.generic_signature ||
            !member.reference.parameters.empty())
          continue;
        auto reference = member.reference;
        reference.owner = name;
        const auto *bridge = findMethod(cls, reference);
        if (!bridge)
          continue;
        const auto target = plainPrimitiveForwardingBridge(cls, *bridge, member,
                                                           signatures, budget);
        if (!target)
          continue;
        auto [entry, inserted] = result.retained_plain_bridges.emplace(
            reference, GenericInheritancePlan::RetainedBridge{*target});
        if (!inserted && entry->second.target != *target)
          unsupported(name,
                      "plain scalar bridge has competing original targets");
        entry->second.primitive_forwarding = true;
        entry->second.inherited_class_contract = member.reference;
      }
      current = ancestor->second.superclass;
    }
  }
  for (const auto &[pair, contract] :
       signatures.abstract_class_covariant_contracts) {
    budget.tick();
    const auto &owner = classes.at(pair.first.owner);
    const auto *target = findMethod(owner, pair.first);
    auto erased = pair.second;
    erased.owner = owner.name;
    if (!target || owner.external || !has(owner.access, "abstract") ||
        !has(target->access, "abstract") || findMethod(owner, erased) ||
        !contract.type_parameters.empty() || !contract.throws_types.empty() ||
        !contract.source_projections.empty())
      unsupported(
          pair.first.identity(),
          "abstract class covariance lost its complete declaration proof");
    GenericBridge bridge{pair.first, pair.second};
    bridge.protected_contract = has(target->access, "protected");
    auto [entry, inserted] =
        result.generated_class_bridges.emplace(erased, bridge);
    if (!inserted && entry->second.target != pair.first)
      unsupported(
          erased.identity(),
          "abstract class compiler slot has competing original declarations");
  }
  for (const auto &[pair, proof] : signatures.covariant_bridge_contracts) {
    budget.tick();
    const auto owner = classes.find(pair.first.owner);
    const auto required_owner = classes.find(pair.second.owner);
    if (owner == classes.end() || owner->second.external ||
        has(owner->second.access, "interface") ||
        required_owner == classes.end())
      continue;
    const auto *member = findMethod(required_owner->second, pair.second);
    const auto *bridge = findMethod(owner->second, pair.first);
    const auto *target = findMethod(owner->second, proof.target);
    // A plain class may inherit this interface through its superclass without
    // a class Signature or direct interface edge. Reuse the complete original
    // bridge receipt and the whole nongeneric ancestry/obligation proof. A
    // captured owner supplies its existing constructor and outer-field receipt.
    if (has(required_owner->second.access, "interface") &&
        (!member || !bridge || !target || owner->second.generic_signature ||
         !owner->second.interfaces.empty() || member->generic_signature ||
         bridge->generic_signature || target->generic_signature ||
         (!independentSourceScope(owner->second) &&
          !signatures.captured_inner_classes.contains(owner->first)) ||
         !plainRuntimeClassOwner(owner->second, classes, signatures, budget,
                                 &result.source_method_names)))
      continue;
    if (!member || !bridge || !target ||
        !provenCovariantClassOverride(*bridge, *member, signatures, classes,
                                      budget))
      unsupported(pair.first.identity(),
                  "plain covariant class bridge lost its original proof");
    const bool protected_contract =
        !has(member->access, "public") && has(member->access, "protected");
    GenericBridge inherited{proof.target, pair.second, std::nullopt, false,
                            protected_contract};
    if (emptyClosedInheritedBridgeThrows(*bridge, *target, *member, signatures))
      inherited.source_declared_throws = member->declared_throws;
    auto [entry, inserted] =
        result.bridges.emplace(pair.first, std::move(inherited));
    if (!inserted && entry->second.target != proof.target)
      unsupported(
          pair.first.identity(),
          "plain covariant class bridge has competing original targets");
  }
  // Signature-free descendants need no invented class Signature to satisfy a
  // deferred literal slot. Check nearest original declarations directly, while
  // keeping full contract/access checks shared with ordinary inheritance.
  std::map<std::string, std::vector<const Class *>> plain_slot_children;
  for (const auto &[name, cls] : classes)
    if (!cls.external && cls.superclass)
      plain_slot_children[*cls.superclass].push_back(&cls);
  auto require_plain_descendants = [&](const Class &root,
                                       const Method &required) {
    std::set<std::string> active;
    std::function<void(const Class &, const Method *, unsigned)> visit =
        [&](const Class &current, const Method *inherited, unsigned depth) {
          budget.tick();
          if (depth > 64 || !active.insert(current.name).second)
            unsupported(
                root.name,
                "deferred plain descendant ancestry is cyclic or too deep");
          const Method *effective = inherited;
          const Method *own = nullptr;
          for (const auto &candidate : current.methods) {
            budget.tick();
            if (!sameSourceMethodName(candidate.reference, required.reference,
                                      &result.source_method_names) ||
                candidate.reference.parameters != required.reference.parameters)
              continue;
            if (own || (inherited && has(inherited->access, "final")) ||
                !sameCompleteMethodContract(candidate, required, signatures,
                                            current.name, budget, classes,
                                            true))
              unsupported(current.name, "deferred plain slot has an "
                                        "incompatible nearest declaration: " +
                                            required.reference.identity());
            own = &candidate;
          }
          if (own)
            effective = own;
          if (!has(current.access, "abstract") &&
              (!effective || has(effective->access, "abstract")))
            unsupported(current.name,
                        "concrete descendant has no original implementation of "
                        "deferred plain slot: " +
                            required.reference.identity());
          if (const auto children = plain_slot_children.find(current.name);
              children != plain_slot_children.end())
            for (const auto *child : children->second)
              visit(*child, effective, depth + 1);
          active.erase(current.name);
        };
    visit(root, nullptr, 0);
  };
  std::set<MethodRef> inherited_source_bridges;
  std::map<std::string, GenericSignature> raw_interface_plans;
  for (const auto &[reference, proof] : signatures.primitive_void_bridges) {
    budget.tick();
    GenericInheritancePlan::RetainedBridge retained{proof.target};
    retained.primitive_void = true;
    result.retained_plain_bridges.emplace(reference, std::move(retained));
  }
  for (const auto &[reference, proof] : signatures.value_carrier_bridges) {
    budget.tick();
    result.retained_plain_bridges.emplace(
        reference, GenericInheritancePlan::RetainedBridge{proof.target, false,
                                                          false, false, true});
  }
  for (const auto &[reference, target] : signatures.terminal_throw_bridges) {
    budget.tick();
    result.retained_plain_bridges.emplace(
        reference, GenericInheritancePlan::RetainedBridge{target, false, true});
  }
  for (const auto &[reference, proof] : signatures.integer_unit_bridges) {
    budget.tick();
    result.bridges.emplace(reference,
                           GenericBridge{proof.target, proof.dispatch,
                                         proof.result_field, true, false,
                                         proof.boolean_parameters});
  }
  for (const auto &[reference, proof] :
       signatures.raw_local_covariant_bridges) {
    budget.tick();
    result.bridges.emplace(reference, GenericBridge{proof.first, proof.second});
  }
  std::set<std::pair<std::string, std::string>> covariant_interface_edges;
  for (const auto &[provided, required] :
       signatures.covariant_interface_returns) {
    budget.tick();
    covariant_interface_edges.emplace(provided.owner, required.owner);
  }
  for (const auto &[name, cls] : classes) {
    budget.tick();
    if (cls.external)
      continue;
    // Use the same proven plain edge to discover both its owner and its
    // member graph. Neither a missing Signature nor an empty argument list
    // may discard this original wrapper before the common contract pass.
    std::set<std::string> plain_integer_contracts;
    if (!cls.generic_signature && independentSourceScope(cls))
      for (const auto &edge : cls.interfaces) {
        budget.tick();
        const auto parent = classes.find(edge);
        if (parent == classes.end() || parent->second.generic_signature ||
            signatures.classes.contains(edge) ||
            !has(parent->second.access, "interface"))
          continue;
        for (const auto &member : parent->second.methods) {
          budget.tick();
          if (!member.reference.parameters.empty() ||
              member.reference.returns != "I")
            continue;
          auto ref = member.reference;
          ref.owner = cls.name;
          const auto *bridge = findMethod(cls, ref);
          if (bridge && plainPrimitiveForwardingBridge(cls, *bridge, member,
                                                       signatures, budget)) {
            plain_integer_contracts.insert(edge);
            break;
          }
        }
      }
    // Complete earlier bridge receipts keep ownership of their method scopes.
    // A proven closed anonymous scope also inherits the parent's complete
    // fixed contract. Discover this edge only for a bridge awaiting a proof.
    const auto closed_parent =
        cls.superclass ? classes.find(*cls.superclass) : classes.end();
    const bool closed_class_parent =
        !cls.generic_signature &&
        (independentSourceScope(cls) ||
         signatures.closed_anonymous_classes.contains(name)) &&
        closed_parent != classes.end() &&
        closedGenericClassContract(closed_parent->second, signatures, budget) &&
        std::any_of(cls.methods.begin(), cls.methods.end(),
                    [&](const Method &method) {
                      return has(method.access, "bridge") &&
                             !has(method.access, "static") &&
                             !result.bridges.contains(method.reference) &&
                             !result.retained_plain_bridges.contains(
                                 method.reference);
                    }) &&
        (sourceGenericParents(closed_parent->second, cls, classes, signatures,
                              budget) ||
         externalGenericParents(closed_parent->second, cls, classes, signatures,
                                budget));
    const auto plan = signatures.classes.find(name);
    const auto covariance = covariant_interface_edges.lower_bound({name, {}});
    const bool plain_covariant_owner =
        !cls.generic_signature &&
        covariance != covariant_interface_edges.end() &&
        covariance->first == name;
    std::optional<GenericSignature> runtime_owner;
    const GenericSignature *owner_plan = nullptr;
    if (plan != signatures.classes.end())
      owner_plan = &plan->second;
    else if (!cls.generic_signature &&
             (!has(cls.access, "interface") || plain_covariant_owner) &&
             (independentSourceScope(cls) || closed_class_parent ||
              signatures.stateless_anonymous_members.contains(name) ||
              (plain_covariant_owner &&
               (signatures.captured_inner_classes.contains(name) ||
                signatures.closed_static_local_classes.contains(name) ||
                signatures.constructor_scoped_static_locals.contains(name)))) &&
             (plain_covariant_owner || closed_class_parent ||
              !plain_integer_contracts.empty() ||
              std::any_of(
                  cls.methods.begin(), cls.methods.end(),
                  [&](const Method &method) {
                    return signatures.nonreturning_void_bridges.contains(
                               method.reference) ||
                           signatures.closed_reference_getter_bridges.contains(
                               method.reference);
                  }) ||
              std::any_of(
                  cls.interfaces.begin(), cls.interfaces.end(),
                  [&](const std::string &edge) {
                    const auto parent = classes.find(edge);
                    return parent != classes.end() &&
                           (fixedSourceInterfaceContract(
                                parent->second, classes, signatures, budget) ||
                            (has(parent->second.access, "interface") &&
                             (!signatures.classes.contains(edge) ||
                              signatures.classes.at(edge)
                                  .type_parameters.empty() ||
                              !plainRawClassContracts(
                                  cls, classes, signatures,
                                  result.source_method_names, budget)) &&
                             ownsGenericMethodContract(parent->second,
                                                       signatures, budget)) ||
                            (has(parent->second.access, "interface") &&
                             signatures.classes.contains(edge) &&
                             !signatures.classes.at(edge)
                                  .type_parameters.empty() &&
                             rawGenericMethodOverride(
                                 cls, parent->second, classes, signatures,
                                 result.source_method_names, budget)) ||
                            std::any_of(
                                cls.methods.begin(), cls.methods.end(),
                                [&](const Method &method) {
                                  return signatures.raw_interface_method_sources
                                      .contains(method.reference);
                                }));
                  }))) {
      if (has(cls.access, "interface")
              ? cls.superclass != "Ljava/lang/Object;"
              : !closed_class_parent &&
                    !plainRuntimeClassOwner(cls, classes, signatures, budget,
                                            &result.source_method_names))
        unsupported(name, "plain member inheritance requires known "
                          "independently scoped nongeneric runtime ancestry");
      // A nongeneric runtime owner can still consume an owned interface's
      // complete fixed contract. This local shape adds no input Signature.
      runtime_owner = plainRuntimeInterfaceSignature(cls, budget);
      owner_plan = &*runtime_owner;
    } else
      continue;
    if (has(cls.access, "abstract") && !has(cls.access, "interface") &&
        !owner_plan->source_projections.empty())
      for (const auto &required : cls.methods)
        if (required.access == Access{"public", "abstract"} &&
            !required.generic_signature &&
            !signatures.methods.contains(required.reference))
          require_plain_descendants(cls, required);
    const auto original = signatures.inheritance_contracts.find(name);
    const auto &contract = original == signatures.inheritance_contracts.end()
                               ? *owner_plan
                               : original->second;
    struct ParentUse {
      const GenericSignature *signature;
      GenericTypeId type;
      std::map<std::string, SubstitutedType> bindings;
      std::set<std::string> path;
      bool raw_contract = false;
      bool fixed_source_contract = false;
      bool closed_class_contract = false;
    };
    std::set<std::string> unit_target_names;
    struct DeferredAbstractContract {
      MethodRef declaration;
      std::vector<std::string> parameters;
      std::string result;
      std::vector<std::pair<std::string, std::string>> method_bounds;
    };
    std::map<std::pair<std::string, std::vector<std::string>>,
             DeferredAbstractContract>
        deferred_abstract_contracts;
    std::vector<ParentUse> parents;
    for (auto id : contract.interfaces) {
      const auto &edge = contract.types.at(id);
      const auto input = classes.find(edge.erasure);
      const bool fixed_source_contract =
          input != classes.end() && !input->second.external &&
          fixedSourceInterfaceContract(input->second, classes, signatures,
                                       budget);
      const bool raw_member_contract =
          !fixed_source_contract && input != classes.end() &&
          has(input->second.access, "interface") &&
          std::all_of(
              edge.segments.begin(), edge.segments.end(),
              [](const auto &segment) { return segment.arguments.empty(); }) &&
          (signatures.classes.find(edge.erasure) == signatures.classes.end() ||
           signatures.classes.at(edge.erasure).type_parameters.empty()) &&
          (ownsGenericMethodContract(input->second, signatures, budget) ||
           // Discovery can cross empty SDK interfaces; their declarations
           // retain the existing complete raw member admission below.
           fixedSourceInterfaceContract(input->second, classes, signatures,
                                        budget) ||
           plain_integer_contracts.contains(edge.erasure) ||
           // Covariance receipts also need plain parents, including a class's
           // complete graph through undeclared intermediate slots. This is
           // discovery only: the shared planner still proves every original
           // target and bridge, even when another edge is generic.
           plain_covariant_owner ||
           (has(cls.access, "interface") &&
            covariant_interface_edges.contains({name, edge.erasure})));
      parents.push_back(
          {&contract, id, {}, {}, raw_member_contract, fixed_source_contract});
    }
    if (contract.superclass) {
      // A proven erasure-neutral marker projection adds only the source
      // superclass argument. Original complete interface contracts still
      // come from the retained snapshot when other projections need one.
      if (signatures.raw_marker_superclasses.contains(name))
        parents.push_back({owner_plan, *owner_plan->superclass, {}, {}});
      else
        parents.push_back({&contract,
                           *contract.superclass,
                           {},
                           {},
                           false,
                           false,
                           closed_class_parent});
    }
    std::map<std::string, std::string> interface_uses;
    std::set<std::string> class_uses;
    std::map<std::string, unsigned> interface_states;
    std::function<void(const std::string &, unsigned)>
        validate_interface_graph = [&](const std::string &owner,
                                       unsigned depth) {
          budget.tick();
          if (depth > 64 || interface_states.size() > 128)
            unsupported(name, "generic interface ancestor graph exceeds limit");
          auto found = classes.find(owner);
          if (found == classes.end() ||
              !has(found->second.access, "interface") ||
              found->second.superclass != "Ljava/lang/Object;")
            unsupported(name, "generic interface ancestor is not a known "
                              "Object-based interface declaration");
          auto &state = interface_states[owner];
          if (state == 1)
            unsupported(name, "generic interface ancestor graph is cyclic");
          if (state == 2)
            return;
          state = 1;
          for (const auto &ancestor : found->second.interfaces)
            validate_interface_graph(ancestor, depth + 1);
          state = 2;
        };
    for (size_t next = 0; next < parents.size(); ++next) {
      const auto use = parents[next];
      const auto &signature = *use.signature;
      const auto id = use.type;
      const auto &parent = signature.types.at(id);
      if (enum_plans.classes.contains(name) && use.path.empty() &&
          signature.superclass == id && parent.erasure == "Ljava/lang/Enum;")
        continue; // Enum<Self> is supplied by the verified enum declaration.
      bool parameterized =
          std::any_of(parent.segments.begin(), parent.segments.end(),
                      [](const auto &s) { return !s.arguments.empty(); });
      if (!parameterized && !use.raw_contract && !use.fixed_source_contract &&
          !use.closed_class_contract) {
        const auto raw = signatures.classes.find(parent.erasure);
        const auto declaration = classes.find(parent.erasure);
        // A raw method-owned contract must not disappear from this proof
        // merely because its class formals have no actual arguments. Other
        // raw class-formal profiles keep their existing bridge admission.
        // Existing source erasures keep their exact validated edge receipt.
        const bool proven_raw_projection = std::any_of(
            signature.source_projections.begin(),
            signature.source_projections.end(), [&](const auto &projection) {
              budget.tick();
              return projection.offset == parent.offset &&
                     projection.source_type == parent.erasure &&
                     projection.scope_origin.has_value();
            });
        if (raw != signatures.classes.end() &&
            !raw->second.type_parameters.empty() &&
            declaration != classes.end() &&
            has(declaration->second.access, "interface") &&
            !proven_raw_projection &&
            rawGenericMethodOverride(cls, declaration->second, classes,
                                     signatures, result.source_method_names,
                                     budget))
          unsupported(name, "raw generic interface member contract needs "
                            "complete arguments or an exact source erasure "
                            "proof: " +
                                parent.erasure);
        if (raw != signatures.classes.end() &&
            !raw->second.type_parameters.empty() &&
            declaration != classes.end() &&
            !has(declaration->second.access, "interface")) {
          std::set<std::string> parameterized_interfaces;
          for (const auto &pending : parents) {
            const auto &edge = pending.signature->types.at(pending.type);
            const auto known = classes.find(edge.erasure);
            if (known != classes.end() &&
                has(known->second.access, "interface") &&
                std::any_of(edge.segments.begin(), edge.segments.end(),
                            [](const auto &segment) {
                              return !segment.arguments.empty();
                            }))
              parameterized_interfaces.insert(edge.erasure);
          }
          std::vector<std::string> interface_roots(
              parameterized_interfaces.begin(), parameterized_interfaces.end());
          for (size_t root = 0; root < interface_roots.size(); ++root) {
            budget.tick();
            if (parameterized_interfaces.size() > 128)
              unsupported(
                  name, "parameterized interface ancestor graph exceeds limit");
            const auto shape = signatures.classes.find(interface_roots[root]);
            if (shape == signatures.classes.end())
              continue;
            for (auto id : shape->second.interfaces) {
              const auto &edge = shape->second.types.at(id);
              if (std::any_of(edge.segments.begin(), edge.segments.end(),
                              [](const auto &segment) {
                                return !segment.arguments.empty();
                              }) &&
                  parameterized_interfaces.insert(edge.erasure).second)
                interface_roots.push_back(edge.erasure);
            }
          }
          std::vector<std::string> roots{parent.erasure};
          std::set<std::string> visited;
          for (size_t root = 0;
               root < roots.size() && !parameterized_interfaces.empty();
               ++root) {
            budget.tick();
            if (!visited.insert(roots[root]).second)
              continue;
            if (visited.size() > 128)
              unsupported(name, "raw generic ancestor graph exceeds limit");
            if (parameterized_interfaces.contains(roots[root]))
              unsupported(name, "raw generic superclass conflicts with a "
                                "parameterized interface contract");
            const auto known = classes.find(roots[root]);
            if (known == classes.end())
              unsupported(name,
                          "raw generic ancestor is not a known declaration");
            roots.insert(roots.end(), known->second.interfaces.begin(),
                         known->second.interfaces.end());
            if (known->second.superclass &&
                *known->second.superclass != "Ljava/lang/Object;")
              roots.push_back(*known->second.superclass);
          }
        }
        continue;
      }
      auto definition = classes.find(parent.erasure);
      const auto formal = signatures.classes.find(parent.erasure);
      const bool interface = definition != classes.end() &&
                             has(definition->second.access, "interface");
      const GenericSignature *parent_plan = interface
          ? inheritanceClassSignature(parent.erasure, signatures)
          : formal == signatures.classes.end() ? nullptr : &formal->second;
      std::set<MethodRef> closed_default_members;
      if (use.raw_contract || use.fixed_source_contract) {
        if (!interface || !independentSourceScope(definition->second) ||
            definition->second.superclass != "Ljava/lang/Object;" ||
            (!use.fixed_source_contract && !definition->second.fields.empty() &&
             !plainInterfaceFields(definition->second, budget)) ||
            (parent_plan && (!parent_plan->type_parameters.empty() ||
                             !parent_plan->source_projections.empty())))
          unsupported(name, "raw inherited interface needs a complete "
                            "nongeneric Object-based declaration: " +
                                parent.erasure);
        if (!parent_plan) {
          // This operation-local shape comes only from known plain runtime
          // edges. No Signature is inserted into the model or source report.
          auto found = raw_interface_plans.find(parent.erasure);
          if (found == raw_interface_plans.end()) {
            auto shape =
                plainRuntimeInterfaceSignature(definition->second, budget);
            found =
                raw_interface_plans.emplace(parent.erasure, std::move(shape))
                    .first;
          }
          parent_plan = &found->second;
        }
        for (const auto &required : definition->second.methods) {
          budget.tick();
          if (has(required.access, "static") || has(required.access, "private"))
            continue;
          const auto method = signatures.methods.find(required.reference);
          const bool parameterized_contract =
              method != signatures.methods.end() &&
              std::any_of(method->second.types.begin(),
                          method->second.types.end(),
                          [&](const GenericType &type) {
                            budget.tick();
                            return std::any_of(
                                type.segments.begin(), type.segments.end(),
                                [](const GenericClassSegment &segment) {
                                  return !segment.arguments.empty();
                                });
                          });
          const bool closed_exception_contract =
              !required.declared_throws || required.declared_throws->empty() ||
              signatures.closed_declared_throws.contains(required.reference);
          const bool closed_contract =
              closed_exception_contract &&
              (method == signatures.methods.end() ||
               (method->second.type_parameters.empty() &&
                method->second.throws_types.empty() &&
                method->second.source_projections.empty() &&
                std::none_of(
                    method->second.types.begin(), method->second.types.end(),
                    [](const GenericType &type) {
                      return type.kind == GenericType::Kind::TypeVariable;
                    })));
          // A known nongeneric independent owner cannot lend class or
          // enclosing formals. Method-owned variables retain their complete
          // bounds and alpha-equivalent parameter/result scope on both source
          // and dependency paths.
          const bool scoped_method_contract =
              method != signatures.methods.end() &&
              !method->second.type_parameters.empty() &&
              method->second.throws_types.empty() &&
              method->second.source_projections.empty() &&
              closed_exception_contract;
          if (use.fixed_source_contract) {
            // A fixed source declaration keeps its existing member admission.
            // Closed generic defaults still need the same full slot comparison
            // as an equivalent raw SDK declaration; erasure cannot certify it.
            if ((parameterized_contract && closed_contract) ||
                scoped_method_contract)
              closed_default_members.insert(required.reference);
            continue;
          }
          // An original source bridge may forward to an abstract interface
          // member. Its complete inherited type and body receipt is already
          // available, independent of which consumer is visited first.
          const auto forwarding =
              signatures.covariant_bridge_contracts.lower_bound(
                  {required.reference, MethodRef{}});
          const bool abstract_bridge =
              forwarding != signatures.covariant_bridge_contracts.end() &&
              forwarding->first.first == required.reference &&
              forwarding->second.contract.type_parameters.empty() &&
              forwarding->second.contract.throws_types.empty() &&
              forwarding->second.contract.source_projections.empty();
          auto inherited_entry = required.reference;
          inherited_entry.owner = cls.name;
          // An opaque dependency bridge contributes its original runtime
          // declaration. The consumer must have its own exact forwarding-body
          // and full covariant contract receipt for that declaration.
          const bool dependency_bridge =
              definition->second.external && closed_contract &&
              signatures.covariant_bridge_contracts.contains(
                  {inherited_entry, required.reference});
          const bool covariant_bridge =
              abstract_bridge || dependency_bridge ||
              std::any_of(
                  signatures.covariant_interface_default_returns.begin(),
                  signatures.covariant_interface_default_returns.end(),
                  [&](const auto &proof) {
                    budget.tick();
                    return proof.second == required.reference;
                  });
          if ((parameterized_contract && has(cls.access, "interface")) ||
              (has(required.access, "bridge") && !covariant_bridge) ||
              (!closed_contract && !scoped_method_contract))
            unsupported(name, "raw inherited member needs a closed method "
                              "contract and a proven class consumer without "
                              "formals or throws: " +
                                  required.reference.identity());
          if (parameterized_contract || scoped_method_contract)
            closed_default_members.insert(required.reference);
        }
      }
      const bool known_ancestors =
          definition != classes.end() && !interface &&
          knownNongenericAncestors(definition->second, cls, classes, signatures,
                                   budget);
      const bool source_parents =
          definition != classes.end() && !interface &&
          sourceGenericParents(definition->second, cls, classes, signatures,
                               budget);
      const bool external_parents =
          definition != classes.end() && !interface &&
          externalGenericParents(definition->second, cls, classes, signatures,
                                 budget);
      if (definition == classes.end() || !parent_plan ||
          (!interface && !known_ancestors && !source_parents &&
           !external_parents &&
           !onlyEmptyMarkerParents(definition->second, classes, budget)) ||
          parent.segments.size() != 1 ||
          (!interface &&
           ((!known_ancestors && !source_parents && !external_parents &&
             definition->second.superclass != "Ljava/lang/Object;") ||
            std::none_of(definition->second.methods.begin(),
                         definition->second.methods.end(),
                         [](const Method &method) {
                           return method.reference.name == "<init>";
                         }))))
        unsupported(name,
                    "generic inheritance requires known parent declarations "
                    "and a complete member substitution proof: " +
                        parent.erasure);
      const auto &arguments = parent.segments.front().arguments;
      if (arguments.size() != parent_plan->type_parameters.size())
        unsupported(name, "generic inheritance formal count disagrees");
      if ((external_parents || source_parents) &&
          definition->second.superclass != "Ljava/lang/Object;") {
        if (!class_uses.insert(parent.erasure).second || class_uses.size() > 64)
          unsupported(name,
                      "generic superclass graph is cyclic or exceeds limit");
        std::map<std::string, SubstitutedType> composed;
        for (size_t i = 0; i < arguments.size(); ++i) {
          if (arguments[i].variance !=
                  GenericTypeArgument::Variance::Invariant ||
              !arguments[i].type)
            unsupported(
                name,
                "generic superclass edges require exact invariant arguments");
          composed.emplace(parent_plan->type_parameters[i].name,
                           substitute(signature, *arguments[i].type,
                                      use.bindings, name, budget, 0, true));
        }
        parents.push_back(
            {parent_plan, *parent_plan->superclass, std::move(composed), {}});
      }
      if (interface) {
        // Validate the nominal graph before deduplicating equal arguments:
        // a previously queued diamond arm must not hide a cycle in another.
        validate_interface_graph(parent.erasure, 0);
        if (use.path.contains(parent.erasure) || use.path.size() > 64 ||
            interface_uses.size() > 128)
          unsupported(name,
                      "generic interface graph is cyclic or exceeds limit");
        if (definition->second.superclass != "Ljava/lang/Object;" ||
            (!use.path.empty() && !use.fixed_source_contract &&
             !definition->second.fields.empty() &&
             !plainInterfaceFields(definition->second, budget)))
          unsupported(name, "inherited generic interface needs its exact "
                            "Object-based contract without fields");
        const auto identity =
            substitute(signature, id, use.bindings, name, budget, 0, true)
                .signature;
        auto [seen, inserted] =
            interface_uses.emplace(parent.erasure, identity);
        if (!inserted) {
          if (seen->second != identity)
            unsupported(name, "generic interface is inherited with conflicting "
                              "complete arguments");
          continue;
        }
      }
      // Class-declared interface roots enter the same complete substitution
      // graph as direct roots. A plain member contract cannot hide different
      // generic arguments inherited through a superclass.
      if (!interface || use.raw_contract || use.fixed_source_contract ||
          !onlyEmptyMarkerParents(definition->second, classes, budget)) {
        auto path = use.path;
        if (interface)
          path.insert(parent.erasure);
        std::map<std::string, SubstitutedType> composed;
        for (size_t i = 0; i < arguments.size(); ++i) {
          if (arguments[i].variance !=
                  GenericTypeArgument::Variance::Invariant ||
              !arguments[i].type)
            unsupported(name, "generic interface edges require exact "
                              "invariant arguments");
          composed.emplace(parent_plan->type_parameters[i].name,
                           substitute(signature, *arguments[i].type,
                                      use.bindings, name, budget, 0, true));
        }
        for (auto inherited : parent_plan->interfaces) {
          const auto &edge = parent_plan->types.at(inherited);
          const auto ancestor = classes.find(edge.erasure);
          if (ancestor == classes.end() ||
              !has(ancestor->second.access, "interface"))
            unsupported(name, "generic interface ancestor is not a known "
                              "interface declaration");
          const bool generic_edge = std::any_of(
              edge.segments.begin(), edge.segments.end(),
              [](const auto &segment) { return !segment.arguments.empty(); });
          if (!generic_edge) {
            const auto raw = signatures.classes.find(edge.erasure);
            if (emptyMarkerHierarchy(edge.erasure, classes, budget) &&
                (raw == signatures.classes.end() ||
                 (raw->second.type_parameters.empty() &&
                  raw->second.source_projections.empty())))
              continue;
            const bool known_contract = fixedSourceInterfaceContract(
                ancestor->second, classes, signatures, budget);
            const bool fixed = !ancestor->second.external && known_contract;
            if (!interface && !known_contract)
              continue; // Other plain class roots retain the existing
                        // nongeneric member/constant ownership proof.
            parents.push_back(
                {parent_plan, inherited, {}, path, !fixed, fixed});
            continue;
          }
          for (const auto &type : parent_plan->types) {
            budget.tick();
            if (type.kind == GenericType::Kind::TypeVariable &&
                (!type.variable_owner ||
                 *type.variable_owner != parent.erasure))
              unsupported(name, "generic interface edge formal belongs to "
                                "a different declaration scope");
          }
          parents.push_back({parent_plan, inherited, composed, path});
        }
      }
      std::map<std::string, SubstitutedType> bindings;
      std::map<std::string, GenericTypeId> actual_arguments;
      for (size_t i = 0; i < arguments.size(); ++i) {
        const auto &argument = arguments[i];
        if (argument.variance != GenericTypeArgument::Variance::Invariant ||
            !argument.type)
          unsupported(
              name,
              "generic inheritance requires exact invariant type arguments");
        actual_arguments.emplace(parent_plan->type_parameters[i].name,
                                 *argument.type);
      }
      for (const auto &member : definition->second.methods) {
        budget.tick();
        if (inaccessiblePackageMethod(definition->second, member, cls)) {
          if (has(member.access, "abstract"))
            unsupported(name, "generic inaccessible package abstract member "
                              "needs a proven ancestor implementation: " +
                                  member.reference.identity());
          // This original body is not inherited by a class in another package.
          // Its independent same-name member is neither an override nor a
          // final/static competitor. Source ancestor bodies remain unchanged.
          continue;
        }
        if (!interface && !member.reference.name.starts_with('<') &&
            !has(member.access, "private"))
          for (const auto &candidate : cls.methods) {
            budget.tick();
            if (candidate.reference.name == member.reference.name &&
                candidate.reference.parameters == member.reference.parameters &&
                candidate.reference.returns == member.reference.returns &&
                has(member.access, "final"))
              unsupported(name,
                          "generic inherited final member cannot be overridden "
                          "(exact final runtime slot): " +
                              member.reference.identity() + " by " +
                              candidate.reference.identity());
            if (!sameSourceMethodName(candidate.reference, member.reference,
                                      &result.source_method_names) ||
                candidate.reference.parameters != member.reference.parameters)
              continue;
            if (inheritedFinalBridgeUses(candidate, member, signatures, budget))
              continue;
            if (has(member.access, "static") != has(candidate.access, "static"))
              unsupported(
                  name, "generic inherited slot changes static/instance kind");
            if (has(member.access, "final"))
              unsupported(
                  name,
                  "generic inherited final member cannot be overridden: " +
                      member.reference.identity() + " by " +
                      candidate.reference.identity());
          }
        if (!definition->second.external && has(member.access, "bridge") &&
            (interface || !member.generic_signature)) {
          // Every input parent is validated by this same operation. Defer
          // its compiler runtime entry until its own complete proof exists,
          // so class iteration order cannot hide an unsupported parent body.
          // A signature-less class bridge is not a separate source contract;
          // the parent's typed declaration supplies that complete contract.
          inherited_source_bridges.insert(member.reference);
          continue;
        }
        if (has(member.access, "static") || has(member.access, "private") ||
            (!interface && member.reference.name == "<init>"))
          continue;
        if (!interface &&
            nearerPlainClassContract(cls, member, classes, signatures,
                                     result.source_method_names, budget))
          continue;
        const bool protected_contract =
            !interface && has(member.access, "protected");
        const bool interface_default =
            interface && !has(member.access, "abstract");
        const bool package_contract =
            !interface && !has(member.access, "public") &&
            !has(member.access, "protected") && !has(member.access, "private");
        if ((!has(member.access, "public") && !protected_contract &&
             !package_contract) ||
            (interface && !has(member.access, "abstract") &&
             !interface_default))
          unsupported(name,
                      "generic inherited member requires a public contract "
                      "or a protected superclass contract; interface defaults "
                      "are not proven");
        MethodRef specialized = member.reference;
        specialized.owner = name;
        if (const auto terminal =
                signatures.nonreturning_void_bridges.find(specialized);
            terminal != signatures.nonreturning_void_bridges.end() &&
            terminal->second.erased_method_formals &&
            terminal->second.erased_contracts.contains(member.reference)) {
          const auto *implementation = findMethod(cls, specialized);
          auto alias = result.source_method_names.find(terminal->second.target);
          auto retained = result.retained_plain_bridges.find(specialized);
          if (!implementation ||
              !sameCompleteMethodContract(*implementation, member, signatures,
                                          name, budget, classes, !interface) ||
              alias == result.source_method_names.end() ||
              alias->second.nonreturning_void_bridge != specialized ||
              retained == result.retained_plain_bridges.end() ||
              !retained->second.nonreturning_void ||
              retained->second.target != terminal->second.target)
            unsupported(name, "raw terminal void inheritance has conflicting "
                              "complete contracts or source plans");
          alias->second.conflicting_contracts.insert(member.reference);
          retained->second.inherited_class_contract = member.reference;
          continue;
        }
        if (signatures.intersection_invocations.contains(member.reference)) {
          // A literal own implementation needs no class substitution. The
          // shared complete-contract check compares both ordered bounds,
          // the formal position, access, dispatch, descriptors and throws.
          // Parent graph, final member and visibility checks above still apply;
          // inherited or default providers stay on their separate ownership
          // proof paths.
          if (const auto *implementation = findMethod(cls, specialized);
              implementation &&
              (!has(implementation->access, "abstract") ||
               has(cls.access, "abstract")) &&
              sameSourceMethodName(implementation->reference, member.reference,
                                   &result.source_method_names) &&
              sameCompleteMethodContract(*implementation, member, signatures,
                                         name, budget, classes, !interface))
            continue;
        }
        std::vector<SubstitutedType> parameter_types;
        SubstitutedType result_type{member.reference.returns,
                                    member.reference.returns};
        size_t method_formal_count = 0;
        std::vector<std::pair<std::string, std::string>> method_bounds;
        const auto method_signature = signatures.methods.find(member.reference);
        const GenericSignature *member_signature =
            method_signature == signatures.methods.end()
                ? nullptr
                : &method_signature->second;
        if (member_signature) {
          const auto &s = *member_signature;
          method_formal_count = s.type_parameters.size();
          if (method_formal_count && !s.throws_types.empty())
            unsupported(name,
                        "inherited method formals with throws are not proven");
          auto member_bindings = bindings;
          for (const auto &[formal, alpha] :
               methodFormalBindings(s, name, budget, &classes))
            member_bindings.insert_or_assign(formal, alpha);
          // A marker or scalar-only member does not consume the parent's
          // formal. Keep its original declaration without inventing an erased
          // binding for an otherwise open class argument.
          for (const auto &type : s.types) {
            budget.tick();
            if (type.kind != GenericType::Kind::TypeVariable ||
                member_bindings.contains(type.variable_name))
              continue;
            if (type.variable_owner && *type.variable_owner != parent.erasure)
              unsupported(name,
                          "inherited formal belongs to a different scope");
            if (const auto actual = actual_arguments.find(type.variable_name);
                actual != actual_arguments.end())
              member_bindings.emplace(type.variable_name,
                                      substitute(signature, actual->second,
                                                 use.bindings, name, budget, 0,
                                                 true));
          }
          for (const auto &[formal, alpha] : methodFormalBindings(
                   s, name, budget, &classes, &member_bindings))
            member_bindings.insert_or_assign(formal, alpha);
          for (const auto &formal : s.type_parameters)
            method_bounds.emplace_back(
                member_bindings.at(formal.name).descriptor,
                substitute(s,
                           methodFormalBound(s, formal, name, budget, &classes),
                           member_bindings, name, budget, 0, true)
                    .signature);
          specialized.parameters.clear();
          for (auto parameter : s.parameters) {
            auto type = substitute(s, parameter, member_bindings, name, budget);
            specialized.parameters.push_back(type.descriptor);
            parameter_types.push_back(std::move(type));
          }
          result_type = substitute(s, *s.result, member_bindings, name, budget);
          specialized.returns = result_type.descriptor;
        } else {
          for (const auto &type : member.reference.parameters)
            parameter_types.push_back({type, type});
        }
        if (!interface) {
          // An unrelated overload cannot replace this inherited declaration.
          // Compare both erased and complete substituted parameter slots, as
          // well as the authoritative planned Java spelling.
          const bool overrides =
              std::any_of(cls.methods.begin(), cls.methods.end(),
                          [&](const Method &candidate) {
                            return sameSourceMethodName(
                                       candidate.reference, member.reference,
                                       &result.source_method_names) &&
                                   (candidate.reference.parameters ==
                                        member.reference.parameters ||
                                    candidate.reference.parameters ==
                                        specialized.parameters);
                          });
          if (!overrides &&
              (!has(member.access, "abstract") || has(cls.access, "abstract")))
            continue;
        }
        bool inherited_checked_result = false;
        auto checked = signatures.inherited_checked_result_bridges.lower_bound(
            {specialized, MethodRef{}});
        for (; checked != signatures.inherited_checked_result_bridges.end() &&
               checked->first.first == specialized;
             ++checked) {
          budget.tick();
          if (interface && checked->first.second != member.reference) {
            // The inherited broad entry can also implement an unchanged
            // generic interface slot. It needs no child declaration: retain
            // the exact provider descriptor and compare the complete scope
            // below, just as for the provider's own superclass contract.
            auto original_slot = member.reference;
            original_slot.owner = checked->second.target.owner;
            if (original_slot != checked->second.target)
              continue;
          }
          const auto &full = checked->second.contract;
          bool exact = !method_formal_count && full.result &&
                       full.type_parameters.empty() &&
                       full.throws_types.empty() &&
                       full.source_projections.empty() &&
                       full.parameters.size() == parameter_types.size();
          for (size_t i = 0; exact && i < parameter_types.size(); ++i)
            exact =
                substitute(full, full.parameters[i], {}, name, budget, 0, true)
                    .signature == parameter_types[i].signature;
          exact =
              exact && substitute(full, *full.result, {}, name, budget, 0, true)
                               .signature == result_type.signature;
          if (!exact)
            unsupported(name,
                        "inherited checked result lost its complete contract");
          inherited_checked_result = true;
        }
        if (inherited_checked_result)
          continue; // The unchanged superclass owns the broad runtime entry.
        const bool primitive_void_contract = std::any_of(
            signatures.primitive_void_bridges.begin(),
            signatures.primitive_void_bridges.end(), [&](const auto &entry) {
              budget.tick();
              return entry.first.owner == name &&
                     entry.second.contracts.contains(member.reference);
            });
        if (primitive_void_contract)
          continue; // Complete boxed substitution and original wide forwarding
                    // were certified together before any source projection.
        auto original_inlined = member.reference;
        original_inlined.owner = name;
        if (const auto proof =
                signatures.checked_inlined_interface_bridges.find(
                    {original_inlined, member.reference});
            proof != signatures.checked_inlined_interface_bridges.end()) {
          const auto &full = proof->second.contract;
          bool exact = interface && !method_formal_count && full.result &&
                       full.type_parameters.empty() &&
                       full.throws_types.empty() &&
                       full.source_projections.empty() &&
                       full.parameters.size() == parameter_types.size() &&
                       proof->second.target == original_inlined;
          for (size_t i = 0; exact && i < parameter_types.size(); ++i)
            exact &=
                substitute(full, full.parameters[i], {}, name, budget, 0, true)
                    .signature == parameter_types[i].signature;
          exact &= full.result &&
                   substitute(full, *full.result, {}, name, budget, 0, true)
                           .signature == result_type.signature;
          if (!exact)
            unsupported(name,
                        "inlined interface body differs from its complete "
                        "original inherited contract");
          continue;
        }
        if (const auto proof =
                signatures.nominal_boxed_results.find(original_inlined);
            proof != signatures.nominal_boxed_results.end() &&
            proof->second.contracts.contains(member.reference)) {
          bool exact =
              interface && !method_formal_count &&
              parameter_types.size() == original_inlined.parameters.size() &&
              result_type.signature == original_inlined.returns;
          for (size_t i = 0; exact && i < parameter_types.size(); ++i)
            exact &=
                parameter_types[i].signature == original_inlined.parameters[i];
          if (!exact)
            unsupported(name, "nominal boxed result differs from its complete "
                              "inherited contract");
          continue;
        }
        if (const auto proof = signatures.boxed_interface_results.find(
                {original_inlined, member.reference});
            proof != signatures.boxed_interface_results.end()) {
          const auto &full = proof->second.contract;
          if (!interface || method_formal_count || !parameter_types.empty() ||
              !full.parameters.empty() || !full.result ||
              !full.type_parameters.empty() || !full.throws_types.empty() ||
              !full.source_projections.empty() ||
              substitute(full, *full.result, {}, name, budget, 0, true)
                      .signature != result_type.signature ||
              proof->second.box.returns != result_type.signature)
            unsupported(name,
                        "boxed interface result differs from its complete "
                        "original inherited contract");
          continue;
        }
        if (const auto proof = signatures.boxed_superclass_results.find(
                {original_inlined, member.reference});
            proof != signatures.boxed_superclass_results.end()) {
          const auto &full = proof->second.contract;
          bool exact = !method_formal_count && full.result &&
                       full.type_parameters.empty() &&
                       full.throws_types.empty() &&
                       full.source_projections.empty() &&
                       full.parameters.size() == parameter_types.size();
          for (size_t i = 0; exact && i < parameter_types.size(); ++i)
            exact &=
                substitute(full, full.parameters[i], {}, name, budget, 0, true)
                    .signature == parameter_types[i].signature;
          exact &= full.result &&
                   substitute(full, *full.result, {}, name, budget, 0, true)
                           .signature == result_type.signature &&
                   proof->second.box.returns == result_type.signature;
          if (!exact)
            unsupported(name, "boxed superclass result differs from its "
                              "complete original inherited contract");
          continue;
        }
        if (const auto proof = signatures.unboxed_predicate_bridges.find(
                {original_inlined, member.reference});
            proof != signatures.unboxed_predicate_bridges.end()) {
          const auto &full = proof->second.contract;
          bool exact = interface && !method_formal_count && full.result &&
                       full.type_parameters.empty() &&
                       full.throws_types.empty() &&
                       full.source_projections.empty() &&
                       full.parameters.size() == parameter_types.size();
          for (size_t i = 0; exact && i < parameter_types.size(); ++i)
            exact &=
                substitute(full, full.parameters[i], {}, name, budget, 0, true)
                    .signature == parameter_types[i].signature;
          exact &= full.result &&
                   substitute(full, *full.result, {}, name, budget, 0, true)
                           .signature == result_type.signature;
          if (!exact)
            unsupported(name,
                        "checked scalar predicate differs from its complete "
                        "original inherited contract");
          continue;
        }
        if (const auto proof = signatures.erased_terminal_consumers.find(
                {original_inlined, member.reference});
            proof != signatures.erased_terminal_consumers.end()) {
          const auto &full = proof->second.contract;
          bool exact = interface && !method_formal_count && full.result &&
                       full.type_parameters.empty() &&
                       full.throws_types.empty() &&
                       full.source_projections.empty() &&
                       full.parameters.size() == parameter_types.size();
          for (size_t i = 0; exact && i < parameter_types.size(); ++i)
            exact &=
                substitute(full, full.parameters[i], {}, name, budget, 0, true)
                    .signature == parameter_types[i].signature;
          exact &= full.result &&
                   substitute(full, *full.result, {}, name, budget, 0, true)
                           .signature == result_type.signature;
          if (!exact)
            unsupported(name, "erased terminal consumer differs from its "
                              "complete inherited contract");
          continue;
        }
        auto compiler_slot = member.reference;
        compiler_slot.owner = name;
        // Reuse the original covariance receipt after full class substitution.
        // Generic ancestors can own literal slots independent of their class
        // formals; another checked bridge must not infer those slots again.
        if (!interface && parent_plan->source_projections.empty()) {
          const auto proof = signatures.covariant_bridge_contracts.find(
              {compiler_slot, member.reference});
          const auto retained = result.bridges.find(compiler_slot);
          if (proof != signatures.covariant_bridge_contracts.end() &&
              retained != result.bridges.end() &&
              !proof->second.raw_parameter_subsignature &&
              retained->second.target == proof->second.target &&
              sameSourceMethodName(proof->second.target, member.reference,
                                   &result.source_method_names)) {
            const auto &full = proof->second.contract;
            if (method_formal_count && parent_plan->type_parameters.empty() &&
                full.type_parameters.size() == method_formal_count)
              continue;
            const auto &typed = *findMethod(cls, proof->second.target);
            bool exact =
                !method_formal_count && full.result &&
                full.type_parameters.empty() && full.throws_types.empty() &&
                full.source_projections.empty() &&
                full.parameters.size() == parameter_types.size() &&
                signatures.covariant_reference_returns.contains(
                    {typed.reference, member.reference}) &&
                compatibleClosedInheritedThrows(typed, member, signatures);
            for (size_t i = 0; exact && i < parameter_types.size(); ++i)
              exact &= substitute(full, full.parameters[i], {}, name, budget, 0,
                                  true)
                           .signature == parameter_types[i].signature;
            exact &= full.result &&
                     substitute(full, *full.result, {}, name, budget, 0, true)
                             .signature == result_type.signature;
            if (exact)
              continue;
          }
        }
        if (!interface) {
          const auto generated =
              result.generated_class_bridges.find(compiler_slot);
          if (generated != result.generated_class_bridges.end()) {
            const auto proof =
                signatures.abstract_class_covariant_contracts.find(
                    {generated->second.target, member.reference});
            if (proof != signatures.abstract_class_covariant_contracts.end()) {
              const auto &full = proof->second;
              bool exact = !method_formal_count && full.result &&
                           full.parameters.size() == parameter_types.size();
              for (size_t i = 0; exact && i < parameter_types.size(); ++i)
                exact &= substitute(full, full.parameters[i], {}, name, budget)
                             .signature == parameter_types[i].signature;
              exact &=
                  full.result &&
                  substitute(full, *full.result, {}, name, budget).signature ==
                      result_type.signature;
              if (!exact)
                unsupported(name, "abstract class compiler slot differs from "
                                  "its complete ancestor contract");
              continue;
            }
          }
        }
        const auto *target = findMethod(cls, specialized);
        // A closed final superclass consumer can preserve every original
        // generic argument while exposing the exact boxed source parameter.
        // The common verifier retains the original cast, array unbox and
        // delegate call; javac supplies the original erased entry.
        if (!target && !cls.external && has(cls.access, "final") &&
            !has(cls.access, "interface") && independentSourceScope(cls) &&
            cls.superclass && cls.superclass != "Ljava/lang/Object;" &&
            owner_plan->type_parameters.empty() &&
            owner_plan->source_projections.empty() &&
            has(member.access, "abstract") && !method_formal_count &&
            member_signature && member_signature->type_parameters.empty() &&
            member_signature->throws_types.empty() &&
            member_signature->source_projections.empty() &&
            (!member.declared_throws || member.declared_throws->empty()) &&
            !result_type.open && !result_type.parameterized &&
            result_type.signature == result_type.descriptor &&
            std::all_of(parameter_types.begin(), parameter_types.end(),
                        [](const auto &type) {
                          return !type.open && !type.parameterized &&
                                 type.signature == type.descriptor;
                        })) {
          auto erased = member.reference;
          erased.owner = name;
          const auto *original = findMethod(cls, erased);
          if (original)
            if (auto proof = arrayCarrierSuperclassBridge(
                    cls, *original, member, specialized, classes, signatures,
                    budget)) {
              auto [found, inserted] =
                  result.retained_plain_bridges.emplace(erased, *proof);
              if (!inserted &&
                  (found->second.source_reference != proof->source_reference ||
                   found->second.target != proof->target ||
                   found->second.unbox_method != proof->unbox_method))
                unsupported(name, "array carrier has conflicting complete "
                                  "inherited contracts");
              found->second.carrier_contracts.insert(member.reference);
              continue;
            }
        }
        if (!target && !method_formal_count &&
            (!interface || has(member.access, "abstract"))) {
          auto erased = member.reference;
          erased.owner = name;
          const auto proof = signatures.covariant_bridge_contracts.find(
              {erased, member.reference});
          // A narrower original result need not have the substituted result's
          // descriptor. Select its original erased entry only through the
          // common complete scoped covariance and forwarding-body receipt.
          // The existing member pass still checks that exact target and bridge.
          if (proof != signatures.covariant_bridge_contracts.end() &&
              proof->second.contract.type_parameters.empty() &&
              proof->second.contract.throws_types.empty() &&
              proof->second.contract.source_projections.empty() &&
              (interface ? signatures.covariant_interface_returns
                         : signatures.covariant_reference_returns)
                  .contains({proof->second.target, member.reference}))
            target = findMethod(cls, erased);
        }
        if ((!target || has(target->access, "bridge")) &&
            !method_formal_count && specialized.returns == "Z") {
          auto erased = member.reference;
          erased.owner = name;
          const auto *bridge = findMethod(cls, erased);
          const Method *projected_target = nullptr;
          for (const auto &candidate : cls.methods) {
            budget.tick();
            if (!bridge || candidate.reference.name != specialized.name ||
                candidate.reference.parameters != specialized.parameters ||
                !nonReturningBooleanBridge(cls, candidate, *bridge, classes,
                                           budget))
              continue;
            if (projected_target && projected_target != &candidate)
              unsupported(name, "nonreturning result has competing targets");
            projected_target = &candidate;
            result.nonreturning_results.emplace(
                candidate.reference,
                GenericInheritancePlan::NonReturningResult{erased, "Z"});
          }
          if (projected_target)
            target = projected_target;
        }
        const Method *nonreturning_integer_bridge = nullptr;
        if (target && has(target->access, "bridge") && !method_formal_count &&
            specialized.returns == "I") {
          const auto *bridge = target;
          const Method *typed = nullptr;
          for (const auto &candidate : cls.methods) {
            budget.tick();
            if (!nonReturningMessageIntegerBridge(cls, candidate, *bridge,
                                                  classes, budget))
              continue;
            if (typed)
              unsupported(name,
                          "terminal integer bridge has competing targets");
            typed = &candidate;
          }
          if (typed) {
            target = typed;
            nonreturning_integer_bridge = bridge;
          }
        }
        const Method *nonreturning_void_bridge = nullptr;
        if (target && !method_formal_count && specialized.returns == "V") {
          const auto proof =
              signatures.nonreturning_void_bridges.find(target->reference);
          if (proof != signatures.nonreturning_void_bridges.end()) {
            nonreturning_void_bridge = target;
            target = findMethod(cls, proof->second.target);
          }
        }
        const Method *renamed_void_bridge = nullptr;
        if (target && !interface && has(cls.access, "final") &&
            has(member.access, "abstract") && !method_formal_count &&
            specialized.parameters ==
                std::vector<std::string>{"Ljava/lang/Object;"} &&
            specialized.returns == "V" && has(target->access, "bridge") &&
            target->instructions.size() == 2) {
          const auto &invoke = target->instructions.front();
          const auto *call = std::get_if<MethodRef>(&invoke.reference);
          if ((invoke.opcode == "invoke-virtual" ||
               invoke.opcode == "invoke-virtual/range") &&
              call && call->owner == name && call->name != specialized.name &&
              call->parameters == specialized.parameters &&
              call->returns == "V") {
            const auto *candidate = findMethod(cls, *call);
            const auto signature = signatures.methods.find(*call);
            if (candidate && !has(candidate->access, "bridge") &&
                !has(candidate->access, "abstract") &&
                signature != signatures.methods.end() &&
                signature->second.source_projections.empty()) {
              renamed_void_bridge = target;
              target = candidate;
            }
          }
        }
        std::optional<MethodRef> bounded_return_bridge;
        bool inherited_covariant_contract = false;
        if (!method_formal_count && target && has(target->access, "bridge")) {
          const Method *forwarded = nullptr;
          const auto *full =
              !interface && has(member.access, "bridge") && member_signature
                  ? covariantBridgeContract(target->reference, signatures,
                                            budget)
                  : nullptr;
          bool same_full_contract =
              full && full->result && full->type_parameters.empty() &&
              full->throws_types.empty() && full->source_projections.empty() &&
              full->parameters.size() == parameter_types.size();
          if (same_full_contract) {
            for (size_t i = 0; i < parameter_types.size(); ++i)
              same_full_contract &=
                  substitute(*full, full->parameters[i], {}, name, budget, 0,
                             true)
                      .signature == parameter_types[i].signature;
            same_full_contract &=
                substitute(*full, *full->result, {}, name, budget, 0, true)
                    .signature == result_type.signature;
          }
          for (const auto &op : target->instructions) {
            budget.tick();
            const auto *call = std::get_if<MethodRef>(&op.reference);
            if ((op.opcode != "invoke-virtual" &&
                 op.opcode != "invoke-virtual/range" &&
                 !(has(cls.access, "interface") &&
                   (op.opcode == "invoke-interface" ||
                    op.opcode == "invoke-interface/range"))) ||
                !call || call->owner != name ||
                call->name != specialized.name ||
                call->parameters != specialized.parameters)
              continue;
            const auto original =
                signatures.covariant_bridge_contracts.lower_bound(
                    {target->reference, MethodRef{}});
            const bool original_full_contract =
                same_full_contract &&
                original != signatures.covariant_bridge_contracts.end() &&
                original->first.first == target->reference &&
                original->second.target == *call;
            if (!original_full_contract &&
                !signatures.covariant_reference_returns.contains(
                    {*call, member.reference}) &&
                !signatures.covariant_interface_returns.contains(
                    {*call, member.reference}))
              continue;
            const auto *candidate = findMethod(cls, *call);
            if (!candidate ||
                (has(candidate->access, "bridge") &&
                 !signatures.reference_getter_bridges.contains(*call) &&
                 !signatures.covariant_static_receiver_implementations.contains(
                     *call)))
              continue;
            if (forwarded && forwarded != candidate)
              unsupported(name,
                          "covariant result bridge has competing targets");
            forwarded = candidate;
            inherited_covariant_contract |= original_full_contract;
          }
          if (forwarded) {
            bounded_return_bridge = target->reference;
            specialized = forwarded->reference;
            target = forwarded;
            if (const auto getter =
                    signatures.reference_getter_bridges.find(target->reference);
                getter != signatures.reference_getter_bridges.end())
              result.retained_plain_bridges.emplace(
                  target->reference, GenericInheritancePlan::RetainedBridge{
                                         getter->second, false, false, true});
          }
        }
        bool generated_interface_bridge = false;
        // The common validator compared the complete original inherited
        // contract and proved all three original bodies before projecting
        // source edges. Keep both guards, including the late default cast.
        auto original_erased = member.reference;
        original_erased.owner = name;
        if (const auto proof =
                signatures.guarded_default_results.find(original_erased);
            proof != signatures.guarded_default_results.end()) {
          const auto &full = proof->second.contract;
          bool exact = !method_formal_count && full.result &&
                       full.parameters.size() == parameter_types.size();
          for (size_t i = 0; exact && i < parameter_types.size(); ++i)
            exact &=
                substitute(full, full.parameters[i], {}, name, budget, 0, true)
                    .signature == parameter_types[i].signature;
          exact &= full.result &&
                   substitute(full, *full.result, {}, name, budget, 0, true)
                           .signature == result_type.signature;
          if (!exact)
            unsupported(name,
                        "guarded default result conflicts with a complete "
                        "inherited contract");
          continue;
        }
        if (!target && has(cls.access, "interface") &&
            has(member.access, "abstract")) {
          for (const auto &candidate : cls.methods) {
            budget.tick();
            if (!has(candidate.access, "abstract") ||
                candidate.reference.name != specialized.name ||
                candidate.reference.parameters != specialized.parameters ||
                !(signatures.bounded_covariant_returns.contains(
                      {candidate.reference, member.reference}) ||
                  signatures.covariant_interface_returns.contains(
                      {candidate.reference, member.reference})))
              continue;
            if (target)
              unsupported(name,
                          "abstract covariant slot has competing targets");
            target = &candidate;
          }
          if (target) {
            bounded_return_bridge = specialized;
            specialized = target->reference;
            generated_interface_bridge = true;
          }
        }
        if (method_formal_count && target && has(target->access, "bridge")) {
          const Method *forwarded = nullptr;
          for (const auto &op : target->instructions) {
            budget.tick();
            const auto *call = std::get_if<MethodRef>(&op.reference);
            if ((op.opcode != "invoke-virtual" &&
                 op.opcode != "invoke-virtual/range" &&
                 !(has(cls.access, "interface") &&
                   (op.opcode == "invoke-interface" ||
                    op.opcode == "invoke-interface/range"))) ||
                !call || call->owner != name ||
                call->name != specialized.name ||
                call->parameters != specialized.parameters ||
                !signatures.bounded_covariant_returns.contains(
                    {*call, member.reference}))
              continue;
            const auto *candidate = findMethod(cls, *call);
            if (!candidate || has(candidate->access, "bridge"))
              continue;
            if (forwarded && forwarded != candidate)
              unsupported(name, "bounded return bridge has competing targets");
            forwarded = candidate;
          }
          if (forwarded) {
            bounded_return_bridge = target->reference;
            specialized = forwarded->reference;
            target = forwarded;
          }
        }
        if (target && has(target->access, "bridge") && !method_formal_count &&
            member.reference.parameters == target->reference.parameters &&
            (!member.declared_throws || member.declared_throws->empty())) {
          if (auto forwarding = genericReferenceGetterBridge(
                  cls, *target, result_type, signatures, budget)) {
            result.retained_plain_bridges.emplace(
                target->reference, GenericInheritancePlan::RetainedBridge{
                                       *forwarding, false, false, true});
            continue;
          } else if (signatures.closed_reference_getter_bridges.contains(
                         target->reference))
            unsupported(name, "closed getter return differs from its complete "
                              "inherited contract");
        }
        const bool certified_super_scalar_wrapper =
            target && !interface && has(member.access, "public") &&
            !has(member.access, "static") && !has(member.access, "final") &&
            std::any_of(signatures.superclass_scalar_getters.begin(),
                        signatures.superclass_scalar_getters.end(),
                        [&](const auto &entry) {
                          budget.tick();
                          return entry.second.second == target->reference;
                        });
        const bool plain_class_scalar_contract =
            target && !interface && has(member.access, "public") &&
            !has(member.access, "static") && !has(member.access, "final") &&
            !member.generic_signature && member.reference.parameters.empty() &&
            member.reference.returns.size() == 1 &&
            std::string_view("ZBCSIJFD")
                    .find(member.reference.returns.front()) !=
                std::string_view::npos &&
            parameter_types.empty() &&
            result_type.signature == member.reference.returns;
        // A capturing member can implement a fixed boolean slot through a
        // differently named virtual body. The capture does not participate
        // in its descriptor or in the verified call/move/return sequence.
        const bool captured_predicate =
            target && !interface &&
            signatures.captured_inner_classes.contains(name) &&
            has(cls.access, "final") && has(member.access, "public") &&
            !has(member.access, "static") && !has(member.access, "final") &&
            !member.generic_signature && member.reference.returns == "Z" &&
            member.reference.parameters.size() == 1;
        if (target &&
            (interface || certified_super_scalar_wrapper ||
             plain_class_scalar_contract || captured_predicate) &&
            has(target->access, "bridge") && !method_formal_count)
          if (auto forwarding = plainPrimitiveForwardingBridge(
                  cls, *target, member, signatures, budget, &parameter_types,
                  &result_type)) {
            auto [entry, inserted] = result.retained_plain_bridges.emplace(
                target->reference,
                GenericInheritancePlan::RetainedBridge{*forwarding, false});
            if (!inserted && entry->second.target != *forwarding)
              unsupported(name,
                          "scalar wrapper has conflicting original targets");
            entry->second.primitive_forwarding = true;
            if (!interface)
              entry->second.inherited_class_contract = member.reference;
            continue;
          }
        if (target && has(target->access, "bridge") && !method_formal_count &&
            ((interface && definition->second.external) ||
             (!interface && has(member.access, "public") &&
              !has(member.access, "static") && !has(member.access, "final") &&
              !has(member.access, "bridge"))))
          if (auto forwarding = guardedReferenceForwardingBridge(
                  cls, *target, member, classes, signatures, budget,
                  parameter_types.size() == 1 &&
                          parameter_types[0].signature ==
                              "Ljava/lang/Object;" &&
                          result_type.descriptor.starts_with('L')
                      ? &result_type
                      : nullptr)) {
            auto [retained, inserted] = result.retained_plain_bridges.emplace(
                target->reference,
                GenericInheritancePlan::RetainedBridge{*forwarding, true});
            if (!interface)
              retained->second.inherited_class_contract = member.reference;
            if (result_type.descriptor.starts_with('L') &&
                result_type.descriptor != member.reference.returns) {
              auto erased = member.reference;
              erased.owner = name;
              const auto *original_bridge = findMethod(cls, erased);
              if (!original_bridge)
                unsupported(name, "guarded reference result lacks its original "
                                  "erased runtime entry: " +
                                      erased.identity());
              const auto original_target = guardedReferenceForwardingBridge(
                  cls, *original_bridge, member, classes, signatures, budget,
                  &result_type);
              bool same_body =
                  original_target && *original_target == *forwarding &&
                  original_bridge->registers == target->registers &&
                  original_bridge->code_end == target->code_end &&
                  original_bridge->instructions.size() ==
                      target->instructions.size();
              for (size_t i = 0; same_body && i < target->instructions.size();
                   ++i) {
                budget.tick();
                const auto &a = original_bridge->instructions[i];
                const auto &b = target->instructions[i];
                same_body = a.pc == b.pc && a.opcode == b.opcode &&
                            a.registers == b.registers &&
                            a.reference == b.reference &&
                            a.target == b.target && a.targets == b.targets &&
                            a.keys == b.keys && a.data == b.data &&
                            a.element_width == b.element_width &&
                            a.literal == b.literal;
              }
              if (!same_body)
                unsupported(name,
                            "guarded erased reference result needs its "
                            "identical original guard and typed invocation");
              GenericBridge proof{target->reference, member.reference};
              proof.guarded_reference_result = true;
              auto [entry, inserted] =
                  result.bridges.emplace(erased, std::move(proof));
              if (!inserted && (entry->second.target != target->reference ||
                                !entry->second.guarded_reference_result))
                unsupported(
                    name, "guarded erased return slot has conflicting targets");
            }
            continue;
          }
        if (interface && !method_formal_count && parameter_types.empty()) {
          auto inherited_erased = member.reference;
          inherited_erased.owner = name;
          const auto proof = signatures.inherited_final_covariant_bridges.find(
              {inherited_erased, member.reference});
          if (proof != signatures.inherited_final_covariant_bridges.end()) {
            const auto &full = proof->second.contract;
            if (!full.result || !full.parameters.empty() ||
                !full.type_parameters.empty() || !full.throws_types.empty() ||
                !full.source_projections.empty() ||
                substitute(full, *full.result, {}, name, budget, 0, true)
                        .signature != result_type.signature)
              unsupported(name, "inherited final getter lost its complete "
                                "interface contract");
            auto [entry, inserted] = result.bridges.emplace(
                inherited_erased,
                GenericBridge{proof->second.target, member.reference});
            if (!inserted && entry->second.target != proof->second.target)
              unsupported(
                  name,
                  "inherited final bridge has conflicting original providers");
            continue;
          }
        }
        if (!target && interface &&
            (has(member.access, "abstract") || interface_default) &&
            inheritedClassContract(cls, member, specialized, parameter_types,
                                   result_type, method_bounds, classes,
                                   signatures, budget, false,
                                   &result.source_method_names))
          continue; // A proven class implementation takes precedence over
                    // defaults.
        if (!target && !interface && has(member.access, "abstract") &&
            inheritedClassContract(cls, member, specialized, parameter_types,
                                   result_type, method_bounds, classes,
                                   signatures, budget, true,
                                   &result.source_method_names))
          continue; // A nearer proven class body also implements the
                    // inherited abstract superclass requirement.
        if (!target && interface && has(member.access, "abstract") &&
            uncontestedInterfaceDefault(
                cls, definition->second, member, specialized, classes,
                signatures, budget,
                !definition->second.external ||
                    closed_default_members.contains(member.reference),
                closed_default_members.contains(member.reference),
                &result.source_method_names, &parameter_types, &result_type))
          continue; // A more specific unchanged default implements this
                    // ancestor slot, including a proven covariant return.
        if (interface_default &&
            std::none_of(cls.methods.begin(), cls.methods.end(),
                         [&](const Method &candidate) {
                           return sameSourceMethodName(
                                      candidate.reference, member.reference,
                                      &result.source_method_names) &&
                                  (candidate.reference.parameters ==
                                       member.reference.parameters ||
                                   candidate.reference.parameters ==
                                       specialized.parameters);
                         })) {
          // An unrelated overload cannot replace this default. Check both
          // erased and fully substituted parameters so an unbridged generic
          // implementation cannot silently acquire a compiler bridge.
          if (!uncontestedInterfaceDefault(
                  cls, definition->second, member, specialized, classes,
                  signatures, budget,
                  !definition->second.external ||
                      closed_default_members.contains(member.reference),
                  closed_default_members.contains(member.reference),
                  &result.source_method_names, &parameter_types, &result_type))
            unsupported(name,
                        std::string(definition->second.external ? "external"
                                                                : "source") +
                            " default inheritance requires known "
                            "interface chains without competing slots or "
                            "class ancestors: " +
                            member.reference.identity());
          continue;
        }
        // An abstract class may leave an unchanged interface
        // requirement to its subclasses. Keep the inherited declaration in
        // its owner; do not invent a body, bridge, or new method inventory.
        // Compare complete substituted types across every erased slot so
        // equal runtime erasures cannot hide conflicting source formals.
        // Fixed exception declarations consume the common Throwable receipt;
        // generic throws and unvalidated declarations cannot be deferred.
        // A Signature-free member's complete literal types cannot depend on
        // unrelated, already-proved owner argument projections. Keep comparing
        // its full slot and class ancestor path; never invent a declaration.
        if (!target && interface && has(member.access, "abstract") &&
            has(cls.access, "abstract") && !has(cls.access, "interface") &&
            independentSourceScope(cls) &&
            (owner_plan->source_projections.empty() ||
             (!member.generic_signature &&
              !signatures.methods.contains(member.reference) &&
              member.reference.parameters == specialized.parameters &&
              member.reference.returns == specialized.returns &&
              result_type.signature == member.reference.returns &&
              std::equal(parameter_types.begin(), parameter_types.end(),
                         member.reference.parameters.begin(),
                         member.reference.parameters.end(),
                         [](const auto &type, const auto &parameter) {
                           return type.signature == parameter;
                         }))) &&
            parent_plan->source_projections.empty() &&
            (!signatures.methods.contains(member.reference) ||
             (signatures.methods.at(member.reference).throws_types.empty() &&
              signatures.methods.at(member.reference)
                  .source_projections.empty())) &&
            (!member.declared_throws || member.declared_throws->empty() ||
             signatures.closed_declared_throws.contains(member.reference)) &&
            std::none_of(cls.methods.begin(), cls.methods.end(),
                         [&](const Method &candidate) {
                           return sameSourceMethodName(
                                      candidate.reference, member.reference,
                                      &result.source_method_names) &&
                                  (candidate.reference.parameters ==
                                       member.reference.parameters ||
                                   candidate.reference.parameters ==
                                       specialized.parameters);
                         }) &&
            deferredAbstractClassRequirement(
                cls, member, specialized, parameter_types, result_type,
                method_bounds, classes, signatures, budget,
                &result.source_method_names)) {
          if (!owner_plan->source_projections.empty() &&
              !member.generic_signature)
            require_plain_descendants(cls, member);
          std::vector<std::string> complete;
          for (const auto &type : parameter_types)
            complete.push_back(type.signature);
          for (const auto &parameters :
               {member.reference.parameters, specialized.parameters}) {
            auto [found, inserted] = deferred_abstract_contracts.emplace(
                std::pair{specialized.name, parameters},
                DeferredAbstractContract{member.reference, complete,
                                         result_type.signature, method_bounds});
            if (inserted)
              continue;
            auto &prior = found->second;
            if (prior.parameters != complete ||
                prior.method_bounds != method_bounds)
              unsupported(name, "deferred abstract interface slot has "
                                "conflicting complete substitutions: " +
                                    member.reference.identity());
            if (prior.result == result_type.signature)
              continue;
            if (signatures.covariant_interface_returns.contains(
                    {prior.declaration, member.reference}))
              continue; // Keep the previously proved more-specific requirement.
            if (signatures.covariant_interface_returns.contains(
                    {member.reference, prior.declaration})) {
              prior.declaration = member.reference;
              prior.result = result_type.signature;
              continue;
            }
            unsupported(name, "deferred abstract interface slot has "
                              "conflicting complete substitutions: " +
                                  member.reference.identity());
          }
          continue;
        }
        // An interface inherits an abstract ancestor declaration directly. It
        // does not need a new body or bridge for a slot it never redeclared.
        if (!target && interface && has(cls.access, "interface") &&
            std::none_of(cls.methods.begin(), cls.methods.end(),
                         [&](const Method &candidate) {
                           return sameSourceMethodName(
                                      candidate.reference, member.reference,
                                      &result.source_method_names) &&
                                  candidate.reference.parameters ==
                                      specialized.parameters;
                         }))
          continue;
        if (!interface) {
          const bool exact_slot = std::any_of(
              cls.methods.begin(), cls.methods.end(),
              [&](const Method &candidate) {
                return !has(candidate.access, "bridge") &&
                       sameSourceMethodName(candidate.reference,
                                            member.reference,
                                            &result.source_method_names) &&
                       candidate.reference.parameters == specialized.parameters;
              });
          if (!exact_slot && (!has(member.access, "abstract") ||
                              has(cls.access, "abstract"))) {
            // An inherited concrete body does not excuse a source declaration
            // that collides with its erased slot after class substitution.
            // Already-proven checked and raw bridges retain their earlier
            // contract paths; an unrelated overload can still be inherited.
            if (std::any_of(cls.methods.begin(), cls.methods.end(),
                            [&](const Method &candidate) {
                              return !has(candidate.access, "bridge") &&
                                     sameSourceMethodName(
                                         candidate.reference, member.reference,
                                         &result.source_method_names) &&
                                     candidate.reference.parameters ==
                                         member.reference.parameters;
                            }))
              unsupported(name, "inherited class parameter specialization "
                                "conflicts with an original erased source "
                                "declaration: " +
                                    member.reference.identity());
            continue;
          }
          if (exact_slot && has(member.access, "final"))
            unsupported(
                name, "generic inherited final member cannot be overridden: " +
                          member.reference.identity() + " as " +
                          specialized.identity());
        }
        auto original_erased_carrier = member.reference;
        original_erased_carrier.owner = name;
        if (const auto proof = signatures.value_carrier_bridges.find(original_erased_carrier);
            proof != signatures.value_carrier_bridges.end() &&
            proof->second.dispatch == member.reference && !proof->second.interface_owner &&
            std::any_of(owner_plan->source_projections.begin(), owner_plan->source_projections.end(),
                        [](const auto &change) { return change.kind == "checked-object-carrier-interface"; })) {
          // This exact raw runtime slot keeps its fully validated carrier body;
          // its complete original interface result also fixes the marker edge.
          continue;
        }
        if (const auto proof =
                signatures.value_carrier_bridges.find(original_erased_carrier);
            proof != signatures.value_carrier_bridges.end() &&
            proof->second.interface_owner &&
            proof->second.dispatch == member.reference) {
          // Compare this inherited edge to the exact original source contract
          // consumed by the body proof. A raw projection never bypasses other
          // members, implementations or different complete edge arguments.
          const auto original_shape = signatures.inheritance_contracts.find(
              *proof->second.interface_owner);
          if (original_shape == signatures.inheritance_contracts.end() ||
              substitute(original_shape->second, proof->second.interface_type,
                         {}, name, budget, 0, true)
                      .signature !=
                  substitute(signature, id, use.bindings, name, budget, 0, true)
                      .signature)
            unsupported(name, "inherited carrier differs from its complete "
                              "original source interface edge");
          continue;
        }
        if (const auto proof =
                signatures.value_carrier_bridges.find(original_erased_carrier);
            proof != signatures.value_carrier_bridges.end())
          if (const auto checked =
                  proof->second.inherited_contracts.find(member.reference);
              checked != proof->second.inherited_contracts.end()) {
            std::vector<std::string> parameters;
            for (const auto &type : parameter_types)
              parameters.push_back(type.signature);
            if (method_formal_count ||
                result_type.signature != checked->second.result ||
                parameters != checked->second.parameters)
              unsupported(name, "scalar carrier differs from its complete "
                                "inherited contract");
            continue;
          }
        std::optional<FieldRef> result_field;
        if (!target && interface && specialized.returns == "Lkotlin/Unit;" &&
            member.reference.returns == "Ljava/lang/Object;") {
          auto void_target = specialized;
          void_target.returns = "V";
          const auto *candidate = findMethod(cls, void_target);
          if (candidate && canNameVoidUnitTarget(cls, *candidate, classes,
                                                 budget, unit_target_names)) {
            result_field = unitResultField(classes, budget);
            if (result_field)
              target = candidate;
          }
        }
        const bool same_runtime_slot =
            specialized.parameters == member.reference.parameters &&
            specialized.returns == member.reference.returns;
        auto erased_contract = member.reference;
        erased_contract.owner = name;
        const auto terminal_throw =
            signatures.terminal_throw_bridges.find(erased_contract);
        const bool terminal_throw_contract =
            target &&
            terminal_throw != signatures.terminal_throw_bridges.end() &&
            terminal_throw->second == target->reference;
        const auto *original_default_bridge =
            interface_default ? findMethod(cls, erased_contract) : nullptr;
        const bool original_default_forwarding =
            target && !method_formal_count && original_default_bridge &&
            original_default_bridge->access ==
                Access{"public", "bridge", "synthetic"} &&
            !original_default_bridge->generic_signature &&
            sameClosedInheritedThrows(*original_default_bridge, member,
                                      signatures);
        const Method *original_method_parameter_bridge = nullptr;
        if (method_formal_count && interface &&
            has(member.access, "abstract") && member_signature &&
            onlyBoundPositionProjections(*member_signature) && target &&
            !has(target->access, "bridge") && !same_runtime_slot &&
            (specialized.returns == member.reference.returns ||
             isAssignableReference(specialized.returns,
                                   member.reference.returns, classes,
                                   budget)) &&
            (!member.declared_throws || member.declared_throws->empty()) &&
            (!target->declared_throws || target->declared_throws->empty())) {
          const auto actual = signatures.methods.find(target->reference);
          const auto *original = findMethod(cls, erased_contract);
          if (original && actual != signatures.methods.end() &&
              actual->second.type_parameters.size() == method_formal_count &&
              onlyBoundPositionProjections(actual->second) &&
              actual->second.throws_types.empty()) {
            // Class substitution also specializes erasures of dependent
            // method bounds. Preserve the original erased entry only through
            // its exact checked forwarding body; all scoped bounds, parameters
            // and results still compare in full below.
            verifyGenericBridge(*original, *target, budget, nullptr, nullptr,
                                &classes, &signatures, &member);
            original_method_parameter_bridge = original;
          }
        }
        if (method_formal_count &&
            ((!same_runtime_slot &&
              !(bounded_return_bridge &&
                specialized.parameters == member.reference.parameters) &&
              !original_method_parameter_bridge) ||
             !target ||
             (((member.declared_throws && !member.declared_throws->empty()) ||
               (target->declared_throws &&
                !target->declared_throws->empty())) &&
              (!same_runtime_slot ||
               !compatibleClosedInheritedThrows(*target, member, signatures)))))
          unsupported(name, "inherited method formals need unchanged runtime "
                            "descriptors or an exact original checked "
                            "parameter bridge and proven closed exceptions: " +
                                member.reference.identity());
        const bool closed_default_override =
            target && (same_runtime_slot || original_default_forwarding) &&
            (!member.declared_throws || member.declared_throws->empty() ||
             validatedClosedDeclaredThrows(member, signatures)) &&
            (!target->declared_throws || target->declared_throws->empty() ||
             validatedClosedDeclaredThrows(*target, signatures)) &&
            compatibleClosedInheritedThrows(*target, member, signatures);
        if (interface_default &&
            ((!same_runtime_slot && !terminal_throw_contract &&
              !original_default_forwarding) ||
             (((member.declared_throws && !member.declared_throws->empty()) ||
               (target && target->declared_throws &&
                !target->declared_throws->empty())) &&
              !closed_default_override)))
          unsupported(name,
                      "interface default override needs unchanged runtime "
                      "descriptors or an original forwarding bridge and "
                      "proven compatible closed exceptions: " +
                          member.reference.identity());
        if (same_runtime_slot && target && !method_formal_count &&
            !member.generic_signature && !member_signature &&
            has(member.access, "public") && !has(member.access, "static") &&
            !has(member.access, "final") &&
            (!member.declared_throws || member.declared_throws->empty()))
          if (auto proof = guardedScalarParameterBridge(cls, *target, classes,
                                                        signatures, budget)) {
            proof->inherited_class_contract = member.reference;
            result.retained_plain_bridges.emplace(target->reference,
                                                  std::move(*proof));
            continue;
          }
        if (interface && same_runtime_slot && target &&
            !member.generic_signature && !member_signature &&
            (!member.declared_throws || member.declared_throws->empty()))
          if (auto proof =
                  plainNamedScalarGetter(cls, *target, signatures, budget)) {
            proof->inherited_class_contract = member.reference;
            result.retained_plain_bridges.emplace(target->reference,
                                                  std::move(*proof));
            continue;
          }
        if (interface && same_runtime_slot && target &&
            !member.generic_signature && !member_signature &&
            (!member.declared_throws || member.declared_throws->empty()))
          if (auto proof = plainBoxedPrimitiveGetter(cls, *target, classes,
                                                     signatures, budget)) {
            proof->inherited_class_contract = member.reference;
            result.retained_plain_bridges.emplace(target->reference,
                                                  std::move(*proof));
            continue;
          }
        const auto *public_erased_target =
            protected_contract && !same_runtime_slot && !method_formal_count &&
                    target
                ? findMethod(cls, erased_contract)
                : nullptr;
        const bool promote_protected_target =
            public_erased_target &&
            publicErasedProtectedTarget(*public_erased_target, *target, classes,
                                        budget, &signatures);
        const auto *package_erased_bridge =
            package_contract && !same_runtime_slot && target
                ? findMethod(cls, erased_contract)
                : nullptr;
        const auto package_covariance =
            package_erased_bridge
                ? signatures.covariant_bridge_contracts.find(
                      {package_erased_bridge->reference, member.reference})
                : signatures.covariant_bridge_contracts.end();
        const bool certified_package_covariance =
            package_covariance != signatures.covariant_bridge_contracts.end() &&
            package_covariance->second.target == target->reference &&
            signatures.covariant_reference_returns.contains(
                {target->reference, member.reference});
        const bool package_bridged_target =
            package_erased_bridge &&
            (packageForwardingBridge(*package_erased_bridge, *target, classes,
                                     budget) ||
             certified_package_covariance);
        const bool inherited_visibility =
            promote_protected_target || package_bridged_target ||
            target &&
                ((protected_contract && has(target->access, "protected")) ||
                 (same_runtime_slot && package_contract &&
                  !has(target->access, "private")));
        // A typed integer implementation may itself carry ACC_BRIDGE while
        // the erased ancestor entry forwards to it. The direct nongeneric
        // interface declaration and original static receiver body prove that
        // role independently of traversal order; full inherited types still
        // compare below, and the outer bridge uses the common body verifier.
        const bool integer_static_implementation =
            target && interface && !method_formal_count &&
            target->reference.returns == "I" &&
            target->access == Access{"public", "bridge"} &&
            !target->generic_signature &&
            std::none_of(parameter_types.begin(), parameter_types.end(),
                         [](const auto &type) {
                           return type.open || type.parameterized;
                         }) &&
            plainStaticReceiverImplementation(cls, *target, classes, signatures,
                                              budget);
        const bool covariant_static_implementation =
            target &&
            signatures.covariant_static_receiver_implementations.contains(
                target->reference);
        const auto checked_reference =
            target ? result.retained_plain_bridges.find(target->reference)
                   : result.retained_plain_bridges.end();
        const bool checked_reference_implementation =
            same_runtime_slot &&
            checked_reference != result.retained_plain_bridges.end() &&
            checked_reference->second.checked_reference_return;
        const bool contract_implementation =
            (same_runtime_slot || integer_static_implementation ||
             covariant_static_implementation) &&
            target &&
            (target->access == Access{"public", "bridge"} ||
             target->access == Access{"public", "bridge", "synthetic"}) &&
            !target->instructions.empty() &&
            (!target->declared_throws || target->declared_throws->empty()) &&
            (!member.declared_throws || member.declared_throws->empty()) &&
            (checked_reference_implementation ||
             integer_static_implementation || covariant_static_implementation ||
             sourceContractImplementation(cls, *target, member, classes,
                                          signatures, budget));
        if (!target ||
            (has(target->access, "bridge") && !contract_implementation &&
             !signatures.reference_getter_bridges.contains(
                 target->reference)) ||
            has(target->access, "static") ||
            (has(target->access, "abstract") && !has(cls.access, "abstract") &&
             !has(cls.access, "interface")) ||
            (!has(target->access, "public") && !inherited_visibility) ||
            (package_contract && !same_runtime_slot && !package_bridged_target))
          unsupported(
              name,
              "generic inheritance has no exact specialized implementation: " +
                  specialized.identity());
        if (same_runtime_slot &&
            ((member.declared_throws && !member.declared_throws->empty()) ||
             (target->declared_throws && !target->declared_throws->empty())) &&
            !compatibleClosedInheritedThrows(*target, member, signatures))
          unsupported(name, "inherited runtime slot needs compatible original "
                            "closed exception declarations: " +
                                member.reference.identity());
        if (!interface && same_runtime_slot && !method_formal_count &&
            signatures.erased_parameter_overrides.contains(
                {target->reference, member.reference}))
          continue; // The common proof retains the original generic return
                    // and class-formal erasure without a bridge or projection.
        const bool parameterized_member =
            result_type.parameterized ||
            std::any_of(parameter_types.begin(), parameter_types.end(),
                        [](const auto &type) { return type.parameterized; });
        const auto target_signature =
            signatures.methods.find(target->reference);
        const auto erased_getter =
            signatures.nominal_getter_return_erasures.find(target->reference);
        const bool nominal_getter_result =
            erased_getter != signatures.nominal_getter_return_erasures.end() &&
            erased_getter->second.contains(member.reference);
        const bool nominal_result =
            !method_formal_count &&
            (inherited_covariant_contract ||
             signatures.covariant_interface_returns.contains(
                 {target->reference, member.reference}) ||
             signatures.covariant_reference_returns.contains(
                 {target->reference, member.reference}));
        const bool open_member =
            result_type.open ||
            std::any_of(parameter_types.begin(), parameter_types.end(),
                        [](const auto &type) { return type.open; });
        auto raw_erased = member.reference;
        raw_erased.owner = name;
        const auto raw_default =
            signatures.source_default_parameter_bridges.find(
                {raw_erased, member.reference});
        const bool checked_raw_interface_parameters =
            raw_default != signatures.source_default_parameter_bridges.end() &&
            raw_default->second.raw_parameter_subsignature &&
            raw_default->second.target == target->reference;
        const auto raw_class = signatures.scoped_raw_parameter_bridges.find(
            {raw_erased, member.reference});
        bool checked_raw_class_parameters =
            raw_class != signatures.scoped_raw_parameter_bridges.end() &&
            raw_class->second.target == target->reference &&
            raw_class->second.contract.parameters.size() ==
                parameter_types.size();
        if (checked_raw_class_parameters) {
          const auto &contract = raw_class->second.contract;
          for (size_t i = 0;
               checked_raw_class_parameters && i < parameter_types.size(); ++i)
            checked_raw_class_parameters =
                substitute(contract, contract.parameters[i], {}, name, budget,
                           0, true)
                    .signature == parameter_types[i].signature;
          checked_raw_class_parameters &=
              contract.result &&
              substitute(contract, *contract.result, {}, name, budget, 0, true)
                      .signature == result_type.signature;
        }
        const bool original_raw_parameters =
            target_signature == signatures.methods.end() &&
            (same_runtime_slot || checked_raw_interface_parameters ||
             checked_raw_class_parameters) &&
            member_signature && member_signature->source_projections.empty() &&
            member_signature->throws_types.empty() &&
            originalRawParameterSubsignature(*target, member, parameter_types,
                                             result_type, method_formal_count,
                                             checked_raw_interface_parameters ||
                                                 checked_raw_class_parameters);
        if ((open_member || method_formal_count) && !original_raw_parameters &&
            target_signature == signatures.methods.end())
          unsupported(name, "open inherited member needs its exact scoped "
                            "target signature: " +
                                member.reference.identity());
        // Java permits an original raw reference result to implement a
        // closed parameterized result through unchecked return conversion.
        // All parameters must remain complete plain runtime types; open
        // variables, method formals and missing parameter contracts do not
        // enter this profile. Keep the original Signature-free result.
        const bool original_raw_result =
            target_signature == signatures.methods.end() &&
            !target->generic_signature && same_runtime_slot &&
            !method_formal_count && !open_member && result_type.parameterized &&
            !result_type.class_name.empty() &&
            result_type.descriptor == target->reference.returns &&
            target->reference.returns.starts_with('L') &&
            std::all_of(parameter_types.begin(), parameter_types.end(),
                        [](const auto &type) {
                          return !type.parameterized && !type.open &&
                                 type.signature == type.descriptor;
                        });
        const bool closed_parameterized_exceptions =
            (!method_formal_count || same_runtime_slot) &&
            (!member_signature ||
             (member_signature->throws_types.empty() &&
              member_signature->source_projections.empty())) &&
            (target_signature == signatures.methods.end() ||
             (target_signature->second.throws_types.empty() &&
              target_signature->second.source_projections.empty())) &&
            compatibleClosedInheritedThrows(*target, member, signatures);
        if (parameterized_member &&
            ((target_signature == signatures.methods.end() && !nominal_result &&
              !original_raw_result && !original_raw_parameters) ||
             !independentSourceScope(definition->second) ||
             (((member.declared_throws && !member.declared_throws->empty()) ||
               (target->declared_throws &&
                !target->declared_throws->empty())) &&
              !closed_parameterized_exceptions)))
          unsupported(
              name, "parameterized member needs an exact target "
                    "signature, independent inherited scope and proven closed "
                    "exceptions");
        if (original_raw_result)
          result.raw_result_contracts[target->reference].insert(
              member.reference);
        if (target_signature != signatures.methods.end()) {
          const auto &actual = target_signature->second;
          const bool actual_parameterized_result =
              actual.result &&
              std::any_of(actual.types.at(*actual.result).segments.begin(),
                          actual.types.at(*actual.result).segments.end(),
                          [](const auto &segment) {
                            return !segment.arguments.empty();
                          });
          if (actual.type_parameters.size() != method_formal_count ||
              actual.parameters.size() != parameter_types.size() ||
              ((parameterized_member || method_formal_count ||
                actual_parameterized_result) &&
               !actual.throws_types.empty()))
            unsupported(name, "generic target signature is not an exact "
                              "member substitution");
          const auto method_bindings =
              methodFormalBindings(actual, name, budget, &classes);
          for (size_t i = 0; i < actual.type_parameters.size(); ++i) {
            const auto &formal = actual.type_parameters[i];
            if (std::pair{formal.erasure,
                          substitute(actual,
                                     methodFormalBound(actual, formal, name,
                                                       budget, &classes),
                                     method_bindings, name, budget, 0, true)
                              .signature} != method_bounds[i])
              unsupported(name, "generic target method bound differs from "
                                "the complete inherited bound");
          }
          const auto wildcard_source =
              signatures.inherited_wildcard_parameter_sources.find(
                  target->reference);
          const bool wildcard_erasure =
              !interface && same_runtime_slot && !method_formal_count &&
              wildcard_source !=
                  signatures.inherited_wildcard_parameter_sources.end() &&
              wildcard_source->second.contract == member.reference;
          for (size_t i = 0; i < parameter_types.size(); ++i) {
            const auto provided =
                substitute(actual, actual.parameters[i], method_bindings, name,
                           budget, 0, true);
            // A Unit wrapper uses the bridge's checked runtime parameter
            // erasures and retains its original call into the renamed void
            // target. This bounded containment does not widen ordinary Java
            // override parameters or change the void target's Signature.
            if (result_field
                    ? !sameOrContainedType(provided, parameter_types[i], budget)
                    : !wildcard_erasure &&
                          provided.signature != parameter_types[i].signature)
              unsupported(name,
                          "generic target parameter differs from the full "
                          "inherited substitution: " +
                              member.reference.identity() + " parameter " +
                              std::to_string(i) + " expected " +
                              parameter_types[i].signature + " provided " +
                              provided.signature);
          }
          const auto expected_result =
              result_field ? SubstitutedType{"V", "V"} : result_type;
          const auto provided_result = substitute(
              actual, *actual.result, method_bindings, name, budget, 0, true);
          const auto lower_return = signatures.closed_object_lower_returns.find(
              {target->reference, member.reference});
          const bool closed_object_lower =
              same_runtime_slot && !method_formal_count && !result_field &&
              lower_return != signatures.closed_object_lower_returns.end() &&
              lower_return->second.result &&
              lower_return->second.parameters.empty() &&
              parameter_types.empty() &&
              substitute(lower_return->second, *lower_return->second.result, {},
                         name, budget, 0, true)
                      .signature == expected_result.signature;
          if (!sameOrContainedReturnType(provided_result, expected_result,
                                         budget) &&
              !nominal_getter_result && !closed_object_lower &&
              !provenSelfCarrierReturn(target->reference, member.reference,
                                       method_bindings, expected_result,
                                       method_formal_count, signatures, name,
                                       budget) &&
              !provenClassFormalBoundReturn(target->reference, member.reference,
                                            expected_result.signature,
                                            method_formal_count, signatures) &&
              !(result.nonreturning_results.contains(target->reference) &&
                expected_result.signature == "Z" &&
                provided_result.signature == "Ljava/lang/Void;") &&
              !(nonreturning_void_bridge && expected_result.signature == "V" &&
                provided_result.signature == "Ljava/lang/Void;") &&
              !(nonreturning_integer_bridge &&
                expected_result.signature == "I" &&
                provided_result.signature == "Ljava/lang/Void;") &&
              !(bounded_return_bridge &&
                (inherited_covariant_contract ||
                 signatures.bounded_covariant_returns.contains(
                     {target->reference, member.reference}) ||
                 signatures.covariant_reference_returns.contains(
                     {target->reference, member.reference}) ||
                 signatures.covariant_interface_returns.contains(
                     {target->reference, member.reference}))))
            unsupported(name, "generic target return differs from the full "
                              "inherited substitution: " +
                                  target->reference.identity() + " expected " +
                                  expected_result.signature + " provided " +
                                  provided_result.signature);
        }
        auto erased = member.reference;
        erased.owner = name;
        if (const auto projection =
                signatures.erased_void_parameters.find(member.reference);
            projection != signatures.erased_void_parameters.end()) {

          if (erased == specialized) {
            if (target_signature != signatures.methods.end() &&
                target_signature->second.parameters.size() == 1 &&
                target_signature->second.types
                        .at(target_signature->second.parameters[0])
                        .kind == GenericType::Kind::TypeVariable)
              result.erased_void_parameters.emplace(target->reference,
                                                    projection->second);
            continue;
          }
          const auto *original_bridge = findMethod(cls, erased);
          if (!original_bridge)
            unsupported(name, "projected void contract lacks its original "
                              "runtime bridge: " +
                                  erased.identity());
          verifyGenericBridge(*original_bridge, *target, budget, nullptr,
                              nullptr, &classes);
          GenericInheritancePlan::RetainedBridge proof{target->reference};
          proof.generic_dispatch = member.reference;
          result.retained_plain_bridges.emplace(erased, std::move(proof));
          continue;
        }
        if (nonreturning_integer_bridge) {
          if (erased != specialized ||
              nonreturning_integer_bridge->reference != erased)
            unsupported(
                name,
                "terminal integer alias requires unchanged full parameters");
          auto alias = result.source_method_names.find(target->reference);
          if (alias == result.source_method_names.end()) {
            std::string spelling = "__neverdNonReturningIntegerTarget";
            while (std::any_of(cls.methods.begin(), cls.methods.end(),
                               [&](const Method &method) {
                                 return method.reference.name == spelling;
                               }) ||
                   std::any_of(result.source_method_names.begin(),
                               result.source_method_names.end(),
                               [&](const auto &entry) {
                                 return entry.first.owner == name &&
                                        entry.second.name == spelling;
                               })) {
              budget.tick();
              spelling += '_';
            }
            alias =
                result.source_method_names
                    .emplace(
                        target->reference,
                        GenericInheritancePlan::SourceMethodName{
                            spelling, target->reference, {member.reference}})
                    .first;
          }
          alias->second.conflicting_contracts.insert(member.reference);
          alias->second.nonreturning_message_integer_bridge =
              nonreturning_integer_bridge->reference;
          GenericInheritancePlan::RetainedBridge proof{target->reference};
          proof.aliased_return_forwarding = true;
          proof.inherited_class_contract = member.reference;
          auto [entry, inserted] =
              result.retained_plain_bridges.emplace(erased, std::move(proof));
          if (!inserted && (!entry->second.aliased_return_forwarding ||
                            entry->second.target != target->reference))
            unsupported(name,
                        "terminal integer alias has conflicting source plans");
          continue;
        }
        if (nonreturning_void_bridge) {
          if (erased != specialized ||
              nonreturning_void_bridge->reference != erased)
            unsupported(
                name,
                "nonreturning void contract requires unchanged parameters");
          auto alias = result.source_method_names.find(target->reference);
          auto retained = result.retained_plain_bridges.find(erased);
          if (alias == result.source_method_names.end() ||
              alias->second.nonreturning_void_bridge != erased ||
              retained == result.retained_plain_bridges.end() ||
              !retained->second.nonreturning_void ||
              retained->second.target != target->reference)
            unsupported(
                name,
                "nonreturning void contract has conflicting source plans");
          alias->second.conflicting_contracts.insert(member.reference);
          retained->second.inherited_class_contract = member.reference;
          continue;
        }
        if (renamed_void_bridge) {
          // Complete scoped member substitution above is independent of the
          // unchanged Object ABI. Reuse the original forwarding body proof.
          verifyGenericBridge(*renamed_void_bridge, *target, budget, nullptr,
                              nullptr, &classes);
          GenericInheritancePlan::RetainedBridge proof{target->reference};
          proof.generic_dispatch = member.reference;
          auto [entry, inserted] =
              result.retained_plain_bridges.emplace(erased, std::move(proof));
          if (!inserted && entry->second.target != target->reference)
            unsupported(name, "generic setter bridge has conflicting targets");
          continue;
        }
        if (contract_implementation) {
          // ACC_BRIDGE can mark the only implementation of an unchanged
          // inherited ABI. Retain its complete original body only after
          // comparing all scoped formals, bounds, parameters and results.
          GenericInheritancePlan::RetainedBridge proof{target->reference};
          proof.contract_implementation = true;
          proof.inherited_class_contract = member.reference;
          result.retained_plain_bridges.emplace(target->reference,
                                                std::move(proof));
        }
        if (erased == specialized) {
          if (result.nonreturning_results.contains(target->reference)) {
            GenericBridge proof{target->reference, member.reference};
            proof.nonreturning_boolean_result = true;
            result.bridges.emplace(erased, std::move(proof));
          }
          continue;
        }
        if (generated_interface_bridge) {
          auto [entry, inserted] = result.generated_interface_bridges.emplace(
              erased, GenericBridge{target->reference, member.reference});
          if (!inserted && entry->second.target != target->reference)
            unsupported(name,
                        "generated interface bridge has conflicting targets");
          continue;
        }
        const auto *bridge = findMethod(cls, erased);
        bool generated_class_parameters =
            !bridge && !interface && !has(cls.access, "interface") &&
            !cls.external && has(cls.access, "abstract") &&
            independentSourceScope(cls) &&
            independentSourceScope(definition->second) &&
            owner_plan->source_projections.empty() &&
            !owner_plan->type_parameters.empty() && !method_formal_count &&
            (target->access == Access{"public", "abstract"} ||
             target->access == Access{"protected", "abstract"}) &&
            (member.access == Access{"public", "abstract"} ||
             member.access == Access{"protected", "abstract"} ||
             member.access == Access{"public"} ||
             member.access == Access{"protected"}) &&
            target->instructions.empty() && target->tries.empty() &&
            !target->registers && !target->code_end &&
            target_signature != signatures.methods.end() &&
            target_signature->second.type_parameters.empty() &&
            target_signature->second.source_projections.empty() &&
            target_signature->second.throws_types.empty() && member_signature &&
            member_signature->source_projections.empty() &&
            member_signature->throws_types.empty() &&
            !result.source_method_names.contains(target->reference) &&
            (!target->declared_throws || target->declared_throws->empty()) &&
            (!member.declared_throws || member.declared_throws->empty()) &&
            target->reference.returns == "V" && erased.returns == "V" &&
            !erased.parameters.empty() && erased.parameters.size() <= 8 &&
            target->reference.parameters.size() == erased.parameters.size() &&
            target->reference.parameters != erased.parameters;
        for (size_t i = 0;
             generated_class_parameters && i < erased.parameters.size(); ++i) {
          const auto &typed = target->reference.parameters[i];
          const auto &broad = erased.parameters[i];
          if (typed != broad)
            generated_class_parameters =
                typed.starts_with('L') && broad.starts_with('L') &&
                classes.contains(typed) &&
                isAssignableReference(typed, broad, classes, budget);
        }
        if (generated_class_parameters) {
          // Complete scoped parameter/result equality was checked above for
          // every ancestor. Preserve the original abstract declaration; the
          // absent broader entry is a disclosed javac projection, not a
          // recovered input body. Concrete descendants still prove each of
          // their original forwarding entries through the common verifier.
          GenericBridge proof{target->reference, member.reference};
          proof.generated_parameter_adaptation = true;
          proof.protected_contract = has(target->access, "protected");
          auto [entry, inserted] =
              result.generated_class_bridges.emplace(erased, std::move(proof));
          if (!inserted && (entry->second.target != target->reference ||
                            !entry->second.generated_parameter_adaptation ||
                            entry->second.protected_contract !=
                                has(target->access, "protected")))
            unsupported(name, "generated abstract class parameter bridge has "
                              "conflicting targets");
          continue;
        }
        const bool can_generate_interface_bridge =
            !bridge && interface && has(cls.access, "interface") &&
            independentSourceScope(cls) &&
            owner_plan->source_projections.empty() && !method_formal_count &&
            target->access == Access{"public", "abstract"} &&
            has(member.access, "abstract") &&
            !result.source_method_names.contains(target->reference) &&
            (!target->declared_throws || target->declared_throws->empty()) &&
            (!member.declared_throws || member.declared_throws->empty());
        const bool generated_parameter_adaptation =
            can_generate_interface_bridge &&
            owner_plan->type_parameters.empty() && !open_member &&
            !parameterized_member && !target->generic_signature &&
            target_signature == signatures.methods.end() &&
            target->reference.returns == "I" && erased.returns == "I" &&
            target->reference.parameters.size() == 1 &&
            erased.parameters ==
                std::vector<std::string>{"Ljava/lang/Object;"} &&
            target->reference.parameters[0].starts_with('L') &&
            target->reference.parameters != erased.parameters &&
            member.access == Access{"public", "abstract"};
        const bool generated_return_adaptation =
            can_generate_interface_bridge &&
            target->reference.parameters == erased.parameters &&
            erased.returns.starts_with('L') &&
            (target->reference.returns.starts_with('L') ||
             target->reference.returns.starts_with('[')) &&
            isAssignableReference(target->reference.returns, erased.returns,
                                  classes, budget);
        if (generated_parameter_adaptation || generated_return_adaptation) {
          // Complete original parameter/result substitution was checked above.
          // Javac adds this absent interface slot; disclose it separately from
          // input recovery. Original class bridges still prove their bodies.
          GenericBridge proof{target->reference, member.reference};
          proof.generated_parameter_adaptation = generated_parameter_adaptation;
          auto [entry, inserted] = result.generated_interface_bridges.emplace(
              erased, std::move(proof));
          if (!inserted && (entry->second.target != target->reference ||
                            entry->second.generated_parameter_adaptation !=
                                generated_parameter_adaptation))
            unsupported(name, "generated substituted interface bridge has "
                              "conflicting targets");
          continue;
        }
        if (!bridge)
          unsupported(name, "generic bridge definition is missing: " +
                                erased.identity());
        if (const auto projection =
                result.nonreturning_results.find(target->reference);
            projection != result.nonreturning_results.end()) {
          if (projection->second.bridge != erased ||
              !nonReturningBooleanBridge(cls, *target, *bridge, classes,
                                         budget))
            unsupported(name, "nonreturning result lost its original bridge");
          GenericBridge proof{target->reference, member.reference};
          proof.nonreturning_boolean_result = true;
          result.bridges.emplace(erased, std::move(proof));
          continue;
        }
        // Suppressing a result-only source overload must not consume its
        // original witness: another interface ancestor can require the same
        // erased slot, and its complete member contract was checked above.
        if (terminal_throw != signatures.terminal_throw_bridges.end()) {
          if (terminal_throw->second != target->reference ||
              !identicalTerminalThrowBridge(*bridge, *target, budget))
            unsupported(name, "terminal throw bridge lost its exact target");
          if (bridge->reference.parameters == target->reference.parameters) {
            // Java cannot declare a result-only overload. The identical
            // parameter-independent throw occurs in the original typed body;
            // javac supplies this original runtime slot with an extra frame.
            GenericBridge proof{target->reference, member.reference};
            proof.terminal_throw_result = true;
            auto [entry, inserted] =
                result.bridges.emplace(erased, std::move(proof));
            if (!inserted && (entry->second.target != target->reference ||
                              !entry->second.terminal_throw_result))
              unsupported(name,
                          "terminal throw slot has conflicting source plans");
            result.retained_plain_bridges.erase(erased);
          }
          continue;
        }
        verifyGenericBridge(*bridge, *target, budget,
                            result_field ? &*result_field : nullptr, nullptr,
                            &classes, &signatures, &member);
        if (promote_protected_target) {
          if (bridge != public_erased_target)
            unsupported(name, "protected target lost its public erased entry");
          result.public_protected_bridge_targets.emplace(target->reference,
                                                         bridge->reference);
        }
        if (result_field &&
            !result.void_unit_targets.contains(target->reference)) {
          std::string source_name = "__neverdVoidBridgeTarget";
          while (unit_target_names.contains(source_name)) {
            budget.tick();
            source_name += '_';
          }
          unit_target_names.insert(source_name);
          result.void_unit_targets.emplace(target->reference,
                                           std::move(source_name));
        }
        GenericBridge proof{target->reference, member.reference, result_field,
                            false, protected_contract};
        if (emptyClosedInheritedBridgeThrows(*bridge, *target, member,
                                             signatures))
          proof.source_declared_throws = member.declared_throws;
        const auto source_throws = proof.source_declared_throws;
        auto [existing, inserted] =
            result.bridges.emplace(erased, std::move(proof));
        if (!inserted &&
            existing->second.source_declared_throws != source_throws)
          unsupported(
              name,
              "generic bridge has conflicting inherited exception metadata");
        if (!inserted && (existing->second.target != target->reference ||
                          existing->second.result_field != result_field))
          unsupported(name,
                      "generic bridge maps to conflicting specialized targets");
        if (!inserted && existing->second.protected_contract &&
            !protected_contract) {
          existing->second.dispatch = member.reference;
          existing->second.protected_contract = false;
        }
      }
    }
  }
  const auto named_members = planNamedAnonymousMembers(classes, budget);
  for (const auto &[name, cls] : classes) {
    if (cls.external || cls.generic_signature)
      continue;
    for (const auto &method : cls.methods) {
      budget.tick();
      if (method.access == Access{"public", "bridge", "synthetic"} &&
          method.reference.returns == "V" &&
          std::any_of(cls.methods.begin(), cls.methods.end(),
                      [&](const Method &candidate) {
                        budget.tick();
                        return !has(candidate.access, "bridge") &&
                               !has(candidate.access, "static") &&
                               candidate.reference.name ==
                                   method.reference.name &&
                               candidate.reference.returns == "V" &&
                               candidate.reference.parameters.size() ==
                                   method.reference.parameters.size() &&
                               candidate.reference.parameters !=
                                   method.reference.parameters;
                      }))
        result.required_parameter_bridges.insert(method.reference);
      if (result.bridges.contains(method.reference) ||
          result.retained_plain_bridges.contains(method.reference))
        continue;
      if (auto proof = literalSuperclassParameterBridge(
              cls, method, classes, signatures, named_members, budget))
        result.bridges.emplace(method.reference, std::move(*proof));
    }
  }
  for (auto it = result.bridges.begin(); it != result.bridges.end();) {
    const auto &[bridge, proof] = *it;
    const auto alias = result.source_method_names.find(proof.target);
    if (alias == result.source_method_names.end() ||
        !alias->second.private_producer ||
        sameSourceMethodName(bridge, proof.target,
                             &result.source_method_names)) {
      ++it;
      continue;
    }
    // Renamed typed return entries cannot regenerate an original differently
    // named erased bridge. Keep its independently verified original body.
    if (proof.result_field || proof.preserve_body)
      unsupported(bridge.identity(),
                  "aliased return requires an original plain "
                  "forwarding source profile");
    GenericInheritancePlan::RetainedBridge retained{proof.target};
    retained.aliased_return_forwarding = true;
    result.retained_plain_bridges.emplace(bridge, std::move(retained));
    it = result.bridges.erase(it);
  }
  for (auto it = result.bridges.begin(); it != result.bridges.end();) {
    const auto &[bridge, proof] = *it;
    const bool rawCallback = signatures.raw_marker_callbacks.contains(bridge);
    if ((!rawCallback && !signatures.guarded_default_raw_owners.contains(bridge.owner)) ||
        bridge.parameters == proof.target.parameters) {
      ++it;
      continue;
    }
    // A raw source parent cannot cause javac to regenerate a parameter
    // bridge. Its original body has already passed the complete forwarding
    // proof; emit that original runtime entry explicitly instead.
    if ((proof.result_field || proof.preserve_body) && !rawCallback)
      unsupported(bridge.identity(), "raw default owner has an incompatible "
                                     "carrier bridge source profile");
    GenericInheritancePlan::RetainedBridge retained{proof.target};
    if (proof.result_field) {
      // The original void dispatch and the singleton read already implement
      // the raw slot. Java can now declare both distinct parameter overloads.
      retained.raw_unit_result = proof.result_field;
      retained.inherited_class_contract = proof.dispatch;
      result.void_unit_targets.erase(proof.target);
    } else {
      retained.raw_parameter_forwarding = true;
      if (rawCallback)
        retained.inherited_class_contract = proof.dispatch;
    }
    result.retained_plain_bridges.emplace(bridge, std::move(retained));
    it = result.bridges.erase(it);
  }
  for (const auto &bridge : signatures.raw_marker_callbacks)
    if (!result.bridges.contains(bridge) && !result.retained_plain_bridges.contains(bridge))
      unsupported(bridge.identity(), "raw callback lacks its original verified bridge");
  for (const auto &bridge : inherited_source_bridges) {
    budget.tick();
    if (!result.bridges.contains(bridge) &&
        !result.retained_plain_bridges.contains(bridge))
      unsupported(
          bridge.identity(),
          "inherited source bridge lacks its own complete original proof");
  }
  // A plain interface can have an exact implementation in an owned static
  // receiver helper even when no generic inheritance edge needs visiting.
  // Keep the original wrapper body and record the interface slot it fulfils.
  for (const auto &[name, cls] : classes) {
    if (cls.external || has(cls.access, "interface") || cls.interfaces.empty())
      continue;
    for (const auto &method : cls.methods) {
      budget.tick();
      const auto &ref = method.reference;
      if (method.access != Access{"public", "bridge"} ||
          result.bridges.contains(ref) ||
          result.retained_plain_bridges.contains(ref) ||
          !provenStaticReceiverImplementation(cls, method, classes, signatures,
                                              budget))
        continue;
      const auto &op = method.instructions[method.instructions.size() - 3];
      const auto &helper = std::get<MethodRef>(op.reference);
      auto slot = ref;
      slot.owner = helper.parameters.front();
      const auto &provider = classes.at(helper.owner);
      if (provider.enclosing != slot.owner ||
          !has(provider.inner_access, "static"))
        continue;
      const auto *required = findMethod(classes.at(slot.owner), slot);
      if (!required || required->access != Access{"public", "abstract"} ||
          required->generic_signature ||
          (required->declared_throws && !required->declared_throws->empty()))
        continue;
      GenericInheritancePlan::RetainedBridge retained{ref};
      retained.contract_implementation = true;
      retained.inherited_class_contract = slot;
      result.retained_plain_bridges.emplace(ref, std::move(retained));
    }
  }
  for (const auto &[bridge, proof] : result.bridges) {
    budget.tick();
    const auto name = result.source_method_names.find(bridge);
    if (name == result.source_method_names.end())
      continue;
    auto [entry, inserted] =
        result.source_method_names.emplace(proof.target, name->second);
    if (!inserted && entry->second.name != name->second.name)
      unsupported(proof.target.identity(),
                  "proven bridge has competing source return-slot names");
  }
  planInterfaceDefaultAccessors(classes, signatures, budget, result);
  planScopedInterfaceSuperCalls(classes, signatures, budget, result);
  return result;
}
} // namespace neverd::mobile::dalvik
