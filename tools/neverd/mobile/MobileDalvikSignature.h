//===- MobileDalvikSignature.h - Parsed JVM generic source declarations -----===//
#pragma once

#include "MobileDalvik.h"

namespace neverd::mobile::dalvik {
enum class GenericSignatureKind { Class, Field, Method };
using GenericTypeId = uint32_t;

struct GenericTypeArgument {
  enum class Variance { Invariant, Extends, Super, Any };
  Variance variance = Variance::Invariant;
  std::optional<GenericTypeId> type;
};
struct GenericClassSegment {
  std::string name;
  std::vector<GenericTypeArgument> arguments;
};
struct GenericType {
  enum class Kind { Primitive, Void, Class, TypeVariable, Array };
  Kind kind = Kind::Void;
  char primitive = 0;
  // Slash-separated package, without a trailing slash. Segments preserve
  // explicitly encoded inner suffixes; a literal '$' is not split.
  std::string package;
  std::vector<GenericClassSegment> segments;
  std::string variable_name;
  std::optional<GenericTypeId> element;
  // Filled by validation, not by the syntax-only parser. A variable owner is
  // its class descriptor or exact MethodRef::identity(), never a guessed scope.
  std::string erasure;
  std::optional<std::string> variable_owner;
  size_t offset = 0;
};
struct GenericTypeParameter {
  std::string name;
  std::optional<GenericTypeId> class_bound;
  std::vector<GenericTypeId> interface_bounds;
  std::string erasure;
};
struct GenericSignature {
  GenericSignatureKind kind = GenericSignatureKind::Field;
  std::vector<GenericType> types;
  std::vector<GenericTypeParameter> type_parameters;
  std::optional<GenericTypeId> superclass;
  std::vector<GenericTypeId> interfaces;
  std::optional<GenericTypeId> field_type;
  std::vector<GenericTypeId> parameters;
  std::optional<GenericTypeId> result;
  std::vector<GenericTypeId> throws_types;
  std::set<std::string> visible_type_variables;
};
struct GenericSignaturePlans {
  std::map<std::string, GenericSignature> classes;
  std::map<FieldRef, GenericSignature> fields;
  std::map<MethodRef, GenericSignature> methods;
  // Includes present-empty Throws annotations. A method signature may legally
  // omit concrete throws; this map preserves that independent declaration.
  std::map<MethodRef, std::vector<std::string>> declared_throws;
};

// Syntax-only parsing is bounded and does not establish source validity.
GenericSignature parseGenericSignature(GenericSignatureKind kind,
                                       std::string_view signature,
                                       Budget &budget);
// Shared by linkClasses and direct recoverJava callers. It validates lexical
// scopes and descriptor erasures; it does not prove arbitrary external generic
// assignability or body substitutions. Those remain source-emitter obligations.
// Parameterized external declarations require a known contract: currently only
// Java SE 8 List<E> and Comparable<T>, each an Object-bounded interface. Input
// declarations override these contracts; unknown instantiations are rejected.
GenericSignaturePlans validateGenericSignatures(const ClassMap &classes,
                                                Budget &budget);
} // namespace neverd::mobile::dalvik
