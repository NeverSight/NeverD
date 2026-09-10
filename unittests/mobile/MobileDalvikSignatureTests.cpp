//===- MobileDalvikSignatureTests.cpp - Generic grammar and binding checks --===//
#include "MobileDalvikSignature.h"
#include "gtest/gtest.h"

#include <functional>

using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;

namespace {
Class genericClass(std::string signature =
                       "<T:Ljava/lang/Object;>Ljava/lang/Object;") {
  Class result;
  result.name = "Lfixture/Box;";
  result.superclass = "Ljava/lang/Object;";
  result.access = {"public", "abstract"};
  result.source_id = "generic.smali";
  result.generic_signature = std::move(signature);
  return result;
}
Method genericMethod(const Class &owner, std::string signature,
                     std::vector<std::string> parameters,
                     std::string returns) {
  Method result;
  result.reference = {owner.name, "convert", std::move(parameters),
                      std::move(returns)};
  result.access = {"public", "abstract"};
  result.generic_signature = std::move(signature);
  return result;
}
GenericSignaturePlans validate(const Class &cls) {
  Budget budget;
  return validateGenericSignatures({{cls.name, cls}}, budget);
}
void rejected(const std::function<void()> &action, std::string_view message) {
  try {
    action();
    FAIL() << "Expected rejection: " << message;
  } catch (const Error &error) {
    EXPECT_NE(std::string(error.what()).find(message), std::string::npos)
        << error.what();
  }
}
} // namespace

TEST(MobileDalvikSignature, GrammarPreservesArgumentsArraysAndInnerSegments) {
  Budget budget;
  auto signature = parseGenericSignature(
      GenericSignatureKind::Field,
      "Lpkg/Outer<Ljava/lang/String;>.Inner<+[TT;-Ljava/lang/Number;*>;",
      budget);
  ASSERT_TRUE(signature.field_type);
  const auto &node = signature.types[*signature.field_type];
  EXPECT_EQ(node.kind, GenericType::Kind::Class);
  EXPECT_EQ(node.package, "pkg");
  ASSERT_EQ(node.segments.size(), 2u);
  EXPECT_EQ(node.segments[0].name, "Outer");
  EXPECT_EQ(node.segments[1].name, "Inner");
  ASSERT_EQ(node.segments[1].arguments.size(), 3u);
  const auto &args = node.segments[1].arguments;
  EXPECT_EQ(args[0].variance, GenericTypeArgument::Variance::Extends);
  ASSERT_TRUE(args[0].type);
  const auto &array = signature.types[*args[0].type];
  EXPECT_EQ(array.kind, GenericType::Kind::Array);
  ASSERT_TRUE(array.element);
  EXPECT_EQ(signature.types[*array.element].variable_name, "T");
  EXPECT_EQ(args[1].variance, GenericTypeArgument::Variance::Super);
  EXPECT_EQ(args[2].variance, GenericTypeArgument::Variance::Any);
  EXPECT_FALSE(args[2].type);
  EXPECT_TRUE(node.erasure.empty());
}

TEST(MobileDalvikSignature, FBoundsAndForwardReferencesHaveFiniteErasure) {
  auto cls = genericClass(
      "<T:TU;U::Ljava/lang/Comparable<TU;>;>Ljava/lang/Object;");
  Field field;
  field.reference = {cls.name, "value", "Ljava/lang/Comparable;"};
  field.generic_signature = "TT;";
  cls.fields.push_back(field);
  auto plans = validate(cls);
  const auto &signature = plans.classes.at(cls.name);
  ASSERT_EQ(signature.type_parameters.size(), 2u);
  EXPECT_EQ(signature.type_parameters[0].erasure, "Ljava/lang/Comparable;");
  EXPECT_EQ(signature.type_parameters[1].erasure, "Ljava/lang/Comparable;");
  const auto &field_plan = plans.fields.at(field.reference);
  const auto &value = field_plan.types[*field_plan.field_type];
  EXPECT_EQ(value.erasure, field.reference.type);
  EXPECT_EQ(value.variable_owner, cls.name);
}

TEST(MobileDalvikSignature, MethodShadowingDoesNotRebindClassBounds) {
  auto cls = genericClass(
      "<T:TU;U:Ljava/lang/Number;>Ljava/lang/Object;");
  cls.methods.push_back(genericMethod(
      cls, "<U:Ljava/lang/String;>(TU;)TT;", {"Ljava/lang/String;"},
      "Ljava/lang/Number;"));
  auto plans = validate(cls);
  const auto &ref = cls.methods[0].reference;
  const auto &signature = plans.methods.at(ref);
  EXPECT_EQ(signature.types[signature.parameters[0]].variable_owner,
            ref.identity());
  EXPECT_EQ(signature.types[*signature.result].variable_owner, cls.name);
  EXPECT_EQ(signature.types[*signature.result].erasure, "Ljava/lang/Number;");
  EXPECT_EQ(signature.visible_type_variables,
            (std::set<std::string>{"T", "U"}));
}

TEST(MobileDalvikSignature, StaticDeclarationsCannotUseClassFormal) {
  auto cls = genericClass();
  auto method = genericMethod(cls, "(TT;)TT;", {"Ljava/lang/Object;"},
                              "Ljava/lang/Object;");
  method.access = {"public", "static"};
  cls.methods = {method};
  rejected([&] { validate(cls); }, "unbound type variable T");
  cls.methods[0].generic_signature =
      "<T:Ljava/lang/Object;>(TT;)TT;";
  auto plans = validate(cls);
  const auto &signature = plans.methods.at(method.reference);
  EXPECT_EQ(signature.types[*signature.result].variable_owner,
            method.reference.identity());
  cls.methods.clear();
  Field field;
  field.reference = {cls.name, "value", "Ljava/lang/Object;"};
  field.access = {"static"};
  field.generic_signature = "TT;";
  cls.fields.push_back(field);
  rejected([&] { validate(cls); }, "unbound type variable T");
}

TEST(MobileDalvikSignature, ErasureChecksAllDeclarationPositions) {
  auto cls = genericClass();
  cls.methods.push_back(genericMethod(cls, "([TT;)TT;",
                                      {"[Ljava/lang/Object;"},
                                      "Ljava/lang/Object;"));
  auto plans = validate(cls);
  const auto &signature = plans.methods.at(cls.methods[0].reference);
  EXPECT_EQ(signature.types[signature.parameters[0]].erasure,
            "[Ljava/lang/Object;");
  auto changed = cls;
  changed.methods[0].reference.parameters = {"Ljava/lang/Object;"};
  rejected([&] { validate(changed); }, "parameter erasure mismatch");
  changed = cls;
  changed.methods[0].reference.returns = "Ljava/lang/String;";
  rejected([&] { validate(changed); }, "return erasure mismatch");
  changed = cls;
  changed.superclass = "Ljava/lang/Number;";
  rejected([&] { validate(changed); }, "superclass erasure mismatch");
  changed = cls;
  changed.interfaces = {"Ljava/io/Serializable;"};
  rejected([&] { validate(changed); }, "interface erasure mismatch");
  changed = cls;
  Field field;
  field.reference = {cls.name, "value", "Ljava/lang/String;"};
  field.generic_signature = "TT;";
  changed.fields.push_back(field);
  rejected([&] { validate(changed); }, "field erasure mismatch");
}

TEST(MobileDalvikSignature, ConstructorParameterOmissionIsNotGuessed) {
  auto cls = genericClass();
  auto method = genericMethod(cls, "()V", {cls.name}, "V");
  method.reference.name = "<init>";
  cls.methods = {method};
  rejected([&] { validate(cls); }, "implicit parameter mapping is not proven");
}

TEST(MobileDalvikSignature, ThrowsPreserveConcreteAndResolveGenericLists) {
  auto cls = genericClass();
  auto method = genericMethod(cls, "<E:Ljava/lang/Exception;>()V^TE;", {},
                              "V");
  method.declared_throws = std::vector<std::string>{"Ljava/lang/Exception;"};
  cls.methods = {method};
  auto plans = validate(cls);
  const auto &signature = plans.methods.at(method.reference);
  ASSERT_EQ(signature.throws_types.size(), 1u);
  EXPECT_EQ(signature.types[signature.throws_types[0]].variable_name, "E");
  EXPECT_EQ(plans.declared_throws.at(method.reference),
            *method.declared_throws);
  cls.methods[0].generic_signature = "()V";
  plans = validate(cls);
  EXPECT_TRUE(plans.methods.at(method.reference).throws_types.empty());
  EXPECT_EQ(plans.declared_throws.at(method.reference),
            *method.declared_throws);
  cls.methods[0].generic_signature.reset();
  cls.methods[0].declared_throws = std::vector<std::string>{};
  plans = validate(cls);
  EXPECT_TRUE(plans.declared_throws.contains(method.reference));
  EXPECT_TRUE(plans.declared_throws.at(method.reference).empty());
}

TEST(MobileDalvikSignature, ThrowsCannotInventOrLoseHierarchyEvidence) {
  auto cls = genericClass();
  auto method = genericMethod(cls, "<E:Ljava/lang/Exception;>()V^TE;", {},
                              "V");
  cls.methods = {method};
  rejected([&] { validate(cls); }, "disagrees with Throws annotation");
  cls.methods[0].declared_throws =
      std::vector<std::string>{"Ljava/lang/Throwable;"};
  rejected([&] { validate(cls); }, "disagrees with Throws annotation");
  cls.methods[0].generic_signature.reset();
  cls.methods[0].declared_throws =
      std::vector<std::string>{"Lexternal/Unverified;"};
  rejected([&] { validate(cls); }, "throws hierarchy is not proven");
  Class exception;
  exception.name = "Lexternal/Unverified;";
  exception.superclass = "Ljava/lang/Exception;";
  Budget budget;
  auto plans = validateGenericSignatures(
      {{cls.name, cls}, {exception.name, exception}}, budget);
  EXPECT_EQ(plans.declared_throws.at(method.reference)[0], exception.name);
  cls.methods[0].declared_throws =
      std::vector<std::string>{"[Ljava/lang/Exception;"};
  rejected([&] { validate(cls); }, "throws entry is not a class");
}

TEST(MobileDalvikSignature, CyclesAndInvalidBoundsDoNotDefaultToObject) {
  for (const auto &signature : {
           "<T:TU;U:TT;>Ljava/lang/Object;",
           "<T:TT;>Ljava/lang/Object;"}) {
    auto cls = genericClass(signature);
    rejected([&] { validate(cls); }, "cyclic type-variable erasure");
  }
  auto cls = genericClass("<T:>Ljava/lang/Object;");
  rejected([&] { validate(cls); }, "no representable bound");
  cls.generic_signature = "<T:[I>Ljava/lang/Object;";
  rejected([&] { validate(cls); }, "Java type bound");
  cls.generic_signature =
      "<T::Ljava/io/Serializable;:Ljava/io/Serializable;>Ljava/lang/Object;";
  rejected([&] { validate(cls); }, "duplicate bound erasure");
}

TEST(MobileDalvikSignature, SyntaxRejectsTruncationTrailingAndWrongTypeKinds) {
  for (const auto &value : {"I", "V", "TT", "Ljava/util/List<>;",
                            "Ljava/lang/String;;", "[V", "Lpkg//Type;"}) {
    Budget budget;
    SCOPED_TRACE(value);
    rejected(
        [&] {
          parseGenericSignature(GenericSignatureKind::Field, value, budget);
        },
        "Invalid JVM generic signature");
  }
  for (const auto &value : {"(V)V", "()V^[I", "<T:Ljava/lang/Object;T:TT;>()V",
                            "()", "(I)Vgarbage"}) {
    Budget budget;
    SCOPED_TRACE(value);
    rejected(
        [&] {
          parseGenericSignature(GenericSignatureKind::Method, value, budget);
        },
        "Invalid JVM generic signature");
  }
}

TEST(MobileDalvikSignature, BudgetsBoundBytesNodesAndNestedGrammar) {
  Budget budget;
  budget.remaining = 5;
  rejected(
      [&] {
        parseGenericSignature(GenericSignatureKind::Field,
                              "Ljava/lang/String;", budget);
      },
      "work budget");
  std::string nested = "Ljava/lang/String;";
  for (unsigned i = 0; i < 66; ++i)
    nested = "Ljava/util/List<" + nested + ">;";
  Budget deep_budget;
  rejected(
      [&] {
        parseGenericSignature(GenericSignatureKind::Field, nested, deep_budget);
      },
      "nesting exceeds limit");
  Budget array_budget;
  auto arrays = parseGenericSignature(GenericSignatureKind::Field,
                                      std::string(255, '[') + "I", array_budget);
  EXPECT_EQ(arrays.types.size(), 256u);
  rejected(
      [&] {
        parseGenericSignature(GenericSignatureKind::Field,
                              std::string(256, '[') + "I", array_budget);
      },
      "dimensions exceed limit");
}

TEST(MobileDalvikSignature, DirectModelsCannotBypassIdentityAndLinkValidation) {
  auto cls = genericClass();
  auto method = genericMethod(cls, "(TT;)TT;", {"Ljava/lang/Object;"},
                              "Ljava/lang/String;");
  cls.methods = {method};
  Budget budget;
  rejected([&] { linkClasses({cls}, budget); }, "return erasure mismatch");
  cls.methods[0].reference.owner = "Lother/Owner;";
  rejected([&] { validate(cls); }, "method owner mismatch");
  cls.methods.clear();
  Budget direct_budget;
  rejected(
      [&] { validateGenericSignatures({{"Lwrong/Key;", cls}}, direct_budget); },
      "class map identity mismatch");
}

TEST(MobileDalvikSignature, LiteralDollarIsNotAnInnerOwnerProof) {
  auto cls = genericClass();
  Field field;
  field.reference = {cls.name, "value", "Lexternal/Outer$Inner;"};
  field.generic_signature = "Lexternal/Outer$Inner<Ljava/lang/String;>;";
  cls.fields = {field};
  Budget budget;
  const auto signature = parseGenericSignature(
      GenericSignatureKind::Field, *field.generic_signature, budget);
  const auto &node = signature.types[*signature.field_type];
  ASSERT_EQ(node.segments.size(), 1u);
  EXPECT_EQ(node.segments[0].name, "Outer$Inner");
  EXPECT_TRUE(node.erasure.empty());
  rejected([&] { validate(cls); }, "external generic declaration is not proven");
  cls.fields[0].generic_signature =
      "Lexternal/Outer<Ljava/lang/String;>.Inner;";
  rejected([&] { validate(cls); }, "inner owner binding is not proven");
}

TEST(MobileDalvikSignature, PlatformContractsAreNarrowAndCannotOverrideInput) {
  auto cls = genericClass();
  Field field;
  field.reference = {cls.name, "value", "Ljava/util/List;"};
  field.generic_signature = "Ljava/util/List<Ljava/lang/String;>;";
  cls.fields = {field};
  EXPECT_TRUE(validate(cls).fields.contains(field.reference));
  cls.fields[0].generic_signature =
      "Ljava/util/List<Ljava/lang/String;Ljava/lang/Object;>;";
  rejected([&] { validate(cls); }, "platform type argument count mismatch");
  for (const auto &name : {"Lunknown/Generic;", "Ljava/lang/String;"}) {
    cls.fields[0].reference.type = name;
    std::string raw = name;
    raw.pop_back();
    cls.fields[0].generic_signature = raw + "<Ljava/lang/Object;>;";
    rejected([&] { validate(cls); },
             "external generic declaration is not proven");
  }
  cls.fields = {field};
  auto supplied = genericClass("<E:Lfixture/Base;>Ljava/lang/Object;");
  supplied.name = "Ljava/util/List;";
  supplied.access = {"public", "interface", "abstract"};
  Class base;
  base.name = "Lfixture/Base;";
  base.superclass = "Ljava/lang/Object;";
  Budget budget;
  rejected(
      [&] {
        validateGenericSignatures(
            {{cls.name, cls}, {supplied.name, supplied}, {base.name, base}},
            budget);
      },
      "type argument bound is not proven");
}

TEST(MobileDalvikSignature, KnownTypeArgumentBoundsRequireActualSubtypeProof) {
  auto box = genericClass("<T:Lfixture/Base;>Ljava/lang/Object;");
  Class base;
  base.name = "Lfixture/Base;";
  base.superclass = "Ljava/lang/Object;";
  Class other;
  other.name = "Lfixture/Other;";
  other.superclass = "Ljava/lang/Object;";
  Field field;
  field.reference = {box.name, "other", box.name};
  field.generic_signature = "Lfixture/Box<Lfixture/Other;>;";
  box.fields.push_back(field);
  auto check = [&] {
    Budget budget;
    return validateGenericSignatures(
        {{box.name, box}, {base.name, base}, {other.name, other}}, budget);
  };
  rejected([&] { check(); }, "type argument bound is not proven");
  other.superclass = base.name;
  auto plans = check();
  EXPECT_EQ(plans.fields.at(field.reference)
                .types[*plans.fields.at(field.reference).field_type]
                .erasure,
            box.name);
  box.fields[0].generic_signature = "Lfixture/Box<TT;>;";
  plans = check();
  EXPECT_TRUE(plans.fields.contains(field.reference));
  box.fields[0].generic_signature = "Lfixture/Box<*>;";
  plans = check();
  EXPECT_TRUE(plans.fields.contains(field.reference));
}

TEST(MobileDalvikSignature, DependentBoundsSubstituteExactDeclarationFormals) {
  auto box = genericClass(
      "<T:TU;U:Ljava/lang/Object;>Ljava/lang/Object;");
  Class base;
  base.name = "Lfixture/Base;";
  base.superclass = "Ljava/lang/Object;";
  Class child;
  child.name = "Lfixture/Child;";
  child.superclass = base.name;
  Field field;
  field.reference = {box.name, "other", box.name};
  field.generic_signature =
      "Lfixture/Box<Lfixture/Child;Lfixture/Base;>;";
  box.fields.push_back(field);
  auto check = [&] {
    Budget budget;
    return validateGenericSignatures(
        {{box.name, box}, {base.name, base}, {child.name, child}}, budget);
  };
  auto plans = check();
  EXPECT_TRUE(plans.fields.contains(field.reference));
  box.fields[0].generic_signature =
      "Lfixture/Box<Lfixture/Base;Lfixture/Child;>;";
  rejected([&] { check(); }, "type argument bound is not proven");
}

TEST(MobileDalvikSignature, KnownInterfaceCannotChangeClassBoundPosition) {
  auto cls = genericClass("<T:Lfixture/Contract;>Ljava/lang/Object;");
  Class contract;
  contract.name = "Lfixture/Contract;";
  contract.superclass = "Ljava/lang/Object;";
  contract.access = {"interface", "abstract"};
  auto check = [&] {
    Budget budget;
    return validateGenericSignatures(
        {{cls.name, cls}, {contract.name, contract}}, budget);
  };
  rejected([&] { check(); }, "class-bound position names a known interface");
  cls.generic_signature = "<T::Lfixture/Contract;>Ljava/lang/Object;";
  auto plans = check();
  EXPECT_EQ(plans.classes.at(cls.name).type_parameters[0].erasure,
            contract.name);
}

TEST(MobileDalvikSignature, ExternalBoundKindsPreserveClassAndInterfaceSlots) {
  for (const auto &name : {"Ljava/lang/Object;", "Ljava/lang/Number;",
                           "Ljava/lang/String;", "Ljava/lang/Exception;",
                           "Ljava/lang/Throwable;"}) {
    SCOPED_TRACE(name);
    auto cls = genericClass("<T:" + std::string(name) + ">Ljava/lang/Object;");
    const auto plans = validate(cls);
    const auto &signature = plans.classes.at(cls.name);
    const auto &formal = signature.type_parameters[0];
    ASSERT_TRUE(formal.class_bound);
    EXPECT_TRUE(formal.interface_bounds.empty());
    EXPECT_EQ(signature.types[*formal.class_bound].erasure, name);
    cls.generic_signature = "<T::" + std::string(name) + ">Ljava/lang/Object;";
    rejected([&] { validate(cls); }, "interface bound names a known class");
  }
  for (const auto &name : {"Ljava/lang/CharSequence;",
                           "Ljava/io/Serializable;", "Ljava/lang/Comparable;",
                           "Ljava/util/List;"}) {
    SCOPED_TRACE(name);
    auto cls = genericClass("<T::" + std::string(name) + ">Ljava/lang/Object;");
    const auto plans = validate(cls);
    const auto &signature = plans.classes.at(cls.name);
    const auto &formal = signature.type_parameters[0];
    EXPECT_FALSE(formal.class_bound);
    ASSERT_EQ(formal.interface_bounds.size(), 1u);
    EXPECT_EQ(signature.types[formal.interface_bounds[0]].erasure, name);
    cls.generic_signature = "<T:" + std::string(name) + ">Ljava/lang/Object;";
    rejected([&] { validate(cls); },
             "class-bound position names a known interface");
  }
}

TEST(MobileDalvikSignature, InputBoundKindOverridesPlatformFactInBothDirections) {
  for (const auto &name : {"Ljava/lang/CharSequence;", "Ljava/lang/Number;"}) {
    SCOPED_TRACE(name);
    Class supplied;
    supplied.name = name;
    supplied.superclass = "Ljava/lang/Object;";
    bool supplied_interface = supplied.name == "Ljava/lang/Number;";
    supplied.access = supplied_interface ? Access{"interface", "abstract"}
                                        : Access{"public"};
    auto prefix = supplied_interface ? "<T::" : "<T:";
    auto cls = genericClass(prefix + supplied.name + ">Ljava/lang/Object;");
    auto check = [&] {
      Budget budget;
      return validateGenericSignatures(
          {{cls.name, cls}, {supplied.name, supplied}}, budget);
    };
    const auto plans = check();
    const auto &formal = plans.classes.at(cls.name).type_parameters[0];
    EXPECT_EQ(bool(formal.class_bound), !supplied_interface);
    EXPECT_EQ(formal.interface_bounds.size(), supplied_interface ? 1u : 0u);
    cls.generic_signature =
        (supplied_interface ? "<T:" : "<T::") + supplied.name +
        ">Ljava/lang/Object;";
    rejected([&] { check(); },
             supplied_interface
                 ? "class-bound position names a known interface"
                 : "interface bound names a known class");
  }
}

TEST(MobileDalvikSignature, UnknownNamedBoundsCannotChooseTheirOwnKind) {
  for (const auto &prefix : {"<T:", "<T::"}) {
    auto cls = genericClass(std::string(prefix) +
                            "Lunknown/Bound;>Ljava/lang/Object;");
    rejected([&] { validate(cls); },
             "formal-bound declaration kind is not proven for Lunknown/Bound;");
  }
}

TEST(MobileDalvikSignature, TypeVariableBoundsKeepTheirOwnSignaturePosition) {
  auto cls =
      genericClass("<T:TU;U::Ljava/lang/CharSequence;>Ljava/lang/Object;");
  const auto plans = validate(cls);
  const auto &signature = plans.classes.at(cls.name);
  const auto &first = signature.type_parameters[0];
  ASSERT_TRUE(first.class_bound);
  EXPECT_TRUE(first.interface_bounds.empty());
  const auto &bound = signature.types[*first.class_bound];
  EXPECT_EQ(bound.kind, GenericType::Kind::TypeVariable);
  EXPECT_EQ(bound.variable_name, "U");
  EXPECT_EQ(bound.variable_owner, cls.name);
  EXPECT_EQ(bound.erasure, "Ljava/lang/CharSequence;");
  EXPECT_EQ(first.erasure, "Ljava/lang/CharSequence;");
  cls.generic_signature = "<T::TU;U::Ljava/lang/CharSequence;>Ljava/lang/Object;";
  rejected([&] { validate(cls); },
           "bound cannot be expressed as a Java type bound");
}

TEST(MobileDalvikSignature, BoundKindFactsDoNotAuthorizeTypeArguments) {
  for (const auto &name : {"Ljava/lang/CharSequence;",
                           "Ljava/io/Serializable;"}) {
    auto cls = genericClass();
    Field field;
    field.reference = {cls.name, "value", name};
    std::string parameterized = name;
    parameterized.pop_back();
    field.generic_signature = parameterized + "<Ljava/lang/Object;>;";
    cls.fields.push_back(field);
    rejected([&] { validate(cls); },
             "external generic declaration is not proven for " +
                 std::string(name));
  }
}
