//===- MobileDalvikSignatureTests.cpp - Generic grammar and binding checks
// --===//
#include "MobileDalvikSignature.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <functional>
#include <tuple>

using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;

namespace {
Class genericClass(
    std::string signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;") {
  Class result;
  result.name = "Lfixture/Box;";
  result.superclass = "Ljava/lang/Object;";
  result.access = {"public", "abstract"};
  result.source_id = "generic.smali";
  result.generic_signature = std::move(signature);
  return result;
}
Method genericMethod(const Class &owner, std::string signature,
                     std::vector<std::string> parameters, std::string returns) {
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

TEST(MobileDalvikSignature, ObjectWildcardEquivalenceDoesNotEraseOtherBounds) {
  Budget B;
  using Variance = GenericTypeArgument::Variance;
  for (const auto &[Argument, Expected] :
       std::vector<std::pair<std::string, Variance>>{
           {"*", Variance::Any},
           {"+Ljava/lang/Object;", Variance::Any},
           {"+TT;", Variance::Extends},
           {"+Ljava/lang/String;", Variance::Extends},
           {"+[Ljava/lang/Object;", Variance::Extends},
           {"-Ljava/lang/Object;", Variance::Super},
           {"Ljava/lang/Object;", Variance::Invariant}}) {
    auto Signature = parseGenericSignature(
        GenericSignatureKind::Field, "Lfixture/Box<" + Argument + ">;", B);
    const auto &Node = Signature.types.at(*Signature.field_type);
    const auto &ArgumentNode = Node.segments[0].arguments[0];
    const auto Original = ArgumentNode.variance;
    EXPECT_EQ(canonicalGenericArgumentVariance(Signature, ArgumentNode, B),
              Expected);
    EXPECT_EQ(ArgumentNode.variance, Original);
  }
}

TEST(MobileDalvikSignature, FBoundsAndForwardReferencesHaveFiniteErasure) {
  auto cls =
      genericClass("<T:TU;U::Ljava/lang/Comparable<TU;>;>Ljava/lang/Object;");
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
  auto cls = genericClass("<T:TU;U:Ljava/lang/Number;>Ljava/lang/Object;");
  cls.methods.push_back(genericMethod(cls, "<U:Ljava/lang/String;>(TU;)TT;",
                                      {"Ljava/lang/String;"},
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
  cls.methods[0].generic_signature = "<T:Ljava/lang/Object;>(TT;)TT;";
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
  cls.methods.push_back(genericMethod(cls, "([TT;)TT;", {"[Ljava/lang/Object;"},
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
  auto method = genericMethod(cls, "<E:Ljava/lang/Exception;>()V^TE;", {}, "V");
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
  auto method = genericMethod(cls, "<E:Ljava/lang/Exception;>()V^TE;", {}, "V");
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
  for (const auto &signature :
       {"<T:TU;U:TT;>Ljava/lang/Object;", "<T:TT;>Ljava/lang/Object;"}) {
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
        parseGenericSignature(GenericSignatureKind::Field, "Ljava/lang/String;",
                              budget);
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
  auto arrays = parseGenericSignature(
      GenericSignatureKind::Field, std::string(255, '[') + "I", array_budget);
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
  rejected([&] { validate(cls); },
           "external generic declaration is not proven");
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
  auto box = genericClass("<T:TU;U:Ljava/lang/Object;>Ljava/lang/Object;");
  Class base;
  base.name = "Lfixture/Base;";
  base.superclass = "Ljava/lang/Object;";
  Class child;
  child.name = "Lfixture/Child;";
  child.superclass = base.name;
  Field field;
  field.reference = {box.name, "other", box.name};
  field.generic_signature = "Lfixture/Box<Lfixture/Child;Lfixture/Base;>;";
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

ClassMap inheritedBoundClasses(bool Interface = false) {
  auto Base = genericClass("<T:Ljava/lang/Object;>Ljava/lang/Object;");
  Base.name = "Lfixture/Base;";
  if (Interface)
    Base.access.insert("interface");
  auto Middle =
      genericClass("<U:Ljava/lang/Object;>" +
                   std::string(Interface ? "Ljava/lang/Object;" : "") +
                   "Lfixture/Base<Ljava/util/List<TU;>;>;");
  Middle.name = "Lfixture/Middle;";
  if (Interface)
    Middle.interfaces = {Base.name};
  else
    Middle.superclass = Base.name;
  auto Leaf = genericClass("<W:Ljava/lang/Object;>Lfixture/Middle<TW;>;");
  Leaf.name = "Lfixture/Leaf;";
  Leaf.superclass = Middle.name;
  auto Bounded = genericClass(
      "<A:Ljava/lang/Object;E:" + std::string(Interface ? ":" : "") +
      "Lfixture/Base<Ljava/util/List<TA;>;>;>Ljava/lang/Object;");
  Bounded.name = "Lfixture/Bounded;";
  auto Host = genericClass("<X:Ljava/lang/Object;>Ljava/lang/Object;");
  Host.name = "Lfixture/Host;";
  Host.fields.push_back({{Host.name, "value", Bounded.name},
                         {"public"},
                         {},
                         "Lfixture/Bounded<TX;Lfixture/Leaf<TX;>;>;"});
  return {{Base.name, Base},
          {Middle.name, Middle},
          {Leaf.name, Leaf},
          {Bounded.name, Bounded},
          {Host.name, Host}};
}

TEST(MobileDalvikSignature,
     ParameterizedBoundsFollowExactClassAndInterfacePaths) {
  for (bool Interface : {false, true}) {
    auto Input = inheritedBoundClasses(Interface);
    Budget B;
    const auto Plans = validateGenericSignatures(Input, B);
    const auto &Field = Input.at("Lfixture/Host;").fields[0];
    const auto &Signature = Plans.fields.at(Field.reference);
    EXPECT_EQ(Signature.types[*Signature.field_type].erasure,
              "Lfixture/Bounded;");
    EXPECT_TRUE(Signature.source_projections.empty());
  }
}

TEST(MobileDalvikSignature,
     ParameterizedBoundPathsCannotConflateInstantiations) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    auto Input = inheritedBoundClasses();
    switch (Mutation) {
    case 0:
      Input.at("Lfixture/Leaf;").generic_signature =
          "<W:Ljava/lang/Object;>Lfixture/Middle<Ljava/lang/String;>;";
      break;
    case 1:
      Input.at("Lfixture/Host;").fields[0].generic_signature =
          "Lfixture/Bounded<Ljava/lang/Object;Lfixture/Leaf<Ljava/lang/"
          "String;>;>;";
      break;
    case 2:
      Input.at("Lfixture/Host;").fields[0].generic_signature =
          "Lfixture/Bounded<TX;Lfixture/Leaf;>;";
      break;
    }
    Budget B;
    rejected([&] { validateGenericSignatures(Input, B); },
             "type argument bound is not proven");
  }
}

TEST(MobileDalvikSignature,
     GenericBoundPathsVerifyDependencyEdgesAndStopCycles) {
  auto Input = inheritedBoundClasses();
  Input.at("Lfixture/Middle;").external = true;
  Input.at("Lfixture/Middle;").superclass = "Ljava/lang/Object;";
  Budget B;
  rejected([&] { validateGenericSignatures(Input, B); },
           "superclass erasure mismatch");
  Input = inheritedBoundClasses();
  Input.at("Lfixture/Middle;").superclass = "Lfixture/Leaf;";
  Input.at("Lfixture/Middle;").generic_signature =
      "<U:Ljava/lang/Object;>Lfixture/Leaf<TU;>;";
  rejected([&] { validateGenericSignatures(Input, B); },
           "generic bound proof exceeds depth limit");
}

ClassMap wildcardBoundClasses() {
  auto Contract = genericClass("<E:Ljava/lang/Object;>Ljava/lang/Object;");
  Contract.name = "Lfixture/Contract;";
  Contract.access.insert("interface");
  auto Base = genericClass("Ljava/lang/Object;");
  Base.name = "Lfixture/Base;";
  auto Child = genericClass("Lfixture/Base;Lfixture/Contract<Lfixture/Base;>;");
  Child.name = "Lfixture/Child;";
  Child.superclass = Base.name;
  Child.interfaces = {Contract.name};
  auto Bounded =
      genericClass("<T::Lfixture/Contract<-TT;>;>Ljava/lang/Object;");
  Bounded.name = "Lfixture/Bounded;";
  auto Host = genericClass("Ljava/lang/Object;");
  Host.name = "Lfixture/Host;";
  Host.fields.push_back({{Host.name, "value", Bounded.name},
                         {"public"},
                         {},
                         "Lfixture/Bounded<Lfixture/Child;>;"});
  return {{Contract.name, Contract},
          {Base.name, Base},
          {Child.name, Child},
          {Bounded.name, Bounded},
          {Host.name, Host}};
}

TEST(MobileDalvikSignature, WildcardBoundsRespectUpperAndLowerDirections) {
  auto Input = wildcardBoundClasses();
  Budget B;
  EXPECT_NO_THROW(validateGenericSignatures(Input, B));
  Input.at("Lfixture/Child;").generic_signature =
      "Lfixture/Base;Lfixture/Contract<Lfixture/Child;>;";
  EXPECT_NO_THROW(validateGenericSignatures(Input, B));
  Input.at("Lfixture/Bounded;").generic_signature =
      "<T::Lfixture/Contract<+Lfixture/Base;>;>Ljava/lang/Object;";
  EXPECT_NO_THROW(validateGenericSignatures(Input, B));
  Input.at("Lfixture/Bounded;").generic_signature =
      "<T::Lfixture/Contract<*>;>Ljava/lang/Object;";
  EXPECT_NO_THROW(validateGenericSignatures(Input, B));
  Input.at("Lfixture/Bounded;").generic_signature =
      "<T::Lfixture/Contract<Lfixture/Base;>;>Ljava/lang/Object;";
  rejected([&] { validateGenericSignatures(Input, B); },
           "type argument bound is not proven");
  Input = wildcardBoundClasses();
  Input.at("Lfixture/Bounded;").generic_signature =
      "<T::Lfixture/Contract<+TT;>;>Ljava/lang/Object;";
  rejected([&] { validateGenericSignatures(Input, B); },
           "type argument bound is not proven");
}

TEST(MobileDalvikSignature, WildcardBoundsDoNotReverseInvariantSubstitutions) {
  auto Input = wildcardBoundClasses();
  Input.at("Lfixture/Child;").generic_signature =
      "Lfixture/Base;Lfixture/Contract<Ljava/lang/String;>;";
  Budget B;
  rejected([&] { validateGenericSignatures(Input, B); },
           "type argument bound is not proven");
  auto &Bounded = Input.at("Lfixture/Bounded;");
  Bounded.generic_signature =
      "<T::Lfixture/Contract<+Lfixture/Base;>;>Ljava/lang/Object;";
  auto &Host = Input.at("Lfixture/Host;");
  Host.fields[0].generic_signature =
      "Lfixture/Bounded<Lfixture/Contract<+Lfixture/Child;>;>;";
  EXPECT_NO_THROW(validateGenericSignatures(Input, B));
  Host.fields[0].generic_signature =
      "Lfixture/Bounded<Lfixture/Contract<-Lfixture/Child;>;>;";
  rejected([&] { validateGenericSignatures(Input, B); },
           "type argument bound is not proven");
  Host.fields[0].generic_signature = "Lfixture/Bounded<Lfixture/Contract<*>;>;";
  rejected([&] { validateGenericSignatures(Input, B); },
           "type argument bound is not proven");
}

TEST(MobileDalvikSignature,
     RequiredWildcardsCannotHideUnboundDependencyVariables) {
  auto Input = wildcardBoundClasses();
  Input.at("Lfixture/Child;").external = true;
  Input.at("Lfixture/Child;").generic_signature =
      "Lfixture/Base;Lfixture/Contract<TMissing;>;";
  Input.at("Lfixture/Bounded;").generic_signature =
      "<T::Lfixture/Contract<*>;>Ljava/lang/Object;";
  Budget B;
  rejected([&] { validateGenericSignatures(Input, B); },
           "unbound type variable Missing");
}

namespace {
ClassMap forwardedWildcardBoundClasses(bool Interface) {
  auto Base = genericClass("<E:Ljava/lang/Object;>Ljava/lang/Object;");
  Base.name = "Lfixture/Base;";
  auto Middle =
      genericClass("<U:Ljava/lang/Object;>" +
                   std::string(Interface ? "Ljava/lang/Object;" : "") +
                   "Lfixture/Base<TU;>;");
  Middle.name = "Lfixture/Middle;";
  auto Leaf = genericClass("<W:Ljava/lang/Object;>" +
                           std::string(Interface ? "Ljava/lang/Object;" : "") +
                           "Lfixture/Middle<TW;>;");
  Leaf.name = "Lfixture/Leaf;";
  if (Interface) {
    Base.access.insert("interface");
    Middle.access.insert("interface");
    Leaf.access.insert("interface");
    Middle.interfaces = {Base.name};
    Leaf.interfaces = {Middle.name};
  } else {
    Middle.superclass = Base.name;
    Leaf.superclass = Middle.name;
  }
  auto Bounded = genericClass(
      "<T:Ljava/lang/Object;C:" + std::string(Interface ? ":" : "") +
      "Lfixture/Base<+TT;>;>Ljava/lang/Object;");
  Bounded.name = "Lfixture/Bounded;";
  auto Host = genericClass("<X:Ljava/lang/Object;>Ljava/lang/Object;");
  Host.name = "Lfixture/Host;";
  Host.fields.push_back({{Host.name, "value", Bounded.name},
                         {"public"},
                         {},
                         "Lfixture/Bounded<TX;Lfixture/Leaf<+TX;>;>;"});
  auto Value = genericClass("Ljava/lang/Object;");
  Value.name = "Lfixture/Value;";
  auto Child = genericClass("Lfixture/Value;");
  Child.name = "Lfixture/ChildValue;";
  Child.superclass = Value.name;
  return {{Base.name, Base},       {Middle.name, Middle}, {Leaf.name, Leaf},
          {Bounded.name, Bounded}, {Host.name, Host},     {Value.name, Value},
          {Child.name, Child}};
}
} // namespace

TEST(MobileDalvikSignature, WildcardInheritanceForwardsExactNominalArguments) {
  for (bool Interface : {false, true}) {
    for (const auto &Variance : {"+", "-", "*"}) {
      auto Input = forwardedWildcardBoundClasses(Interface);
      const auto Argument =
          std::string(Variance) == "*" ? "*" : std::string(Variance) + "TT;";
      Input.at("Lfixture/Bounded;").generic_signature =
          "<T:Ljava/lang/Object;C:" + std::string(Interface ? ":" : "") +
          "Lfixture/Base<" + Argument + ">;>Ljava/lang/Object;";
      Input.at("Lfixture/Host;").fields[0].generic_signature =
          "Lfixture/Bounded<TX;Lfixture/Leaf<" +
          (std::string(Variance) == "*" ? "*" : std::string(Variance) + "TX;") +
          ">;>;";
      Budget B;
      const auto Plans = validateGenericSignatures(Input, B);
      const auto &Middle = Plans.classes.at("Lfixture/Middle;");
      const auto Edge = Interface ? Middle.interfaces[0] : *Middle.superclass;
      EXPECT_EQ(Middle.types[Edge].segments[0].arguments[0].variance,
                GenericTypeArgument::Variance::Invariant);
      for (const auto &[Name, Plan] : Plans.classes)
        EXPECT_TRUE(Plan.source_projections.empty());
      EXPECT_TRUE(Plans.fields.begin()->second.source_projections.empty());
    }
    auto Input = forwardedWildcardBoundClasses(Interface);
    Input.at("Lfixture/Host;").fields[0].generic_signature =
        "Lfixture/Bounded<Lfixture/Value;Lfixture/Leaf<+Lfixture/"
        "ChildValue;>;>;";
    Budget B;
    EXPECT_NO_THROW(validateGenericSignatures(Input, B));
    Input.at("Lfixture/Bounded;").generic_signature =
        "<T:Ljava/lang/Object;C:" + std::string(Interface ? ":" : "") +
        "Lfixture/Base<-TT;>;>Ljava/lang/Object;";
    Input.at("Lfixture/Host;").fields[0].generic_signature =
        "Lfixture/Bounded<Lfixture/ChildValue;Lfixture/Leaf<-Lfixture/"
        "Value;>;>;";
    EXPECT_NO_THROW(validateGenericSignatures(Input, B));
  }
}

TEST(MobileDalvikSignature,
     WildcardInheritanceRetainsVarianceAndScopeSeparation) {
  const std::vector<std::pair<std::string, std::string>> Cases = {
      {"TT;", "+TX;"}, {"TT;", "-TX;"},  {"TT;", "*"}, {"+TT;", "-TX;"},
      {"+TT;", "*"},   {"-TT;", "+TX;"}, {"-TT;", "*"}};
  for (bool Interface : {false, true}) {
    for (const auto &[Required, Actual] : Cases) {
      auto Input = forwardedWildcardBoundClasses(Interface);
      Input.at("Lfixture/Bounded;").generic_signature =
          "<T:Ljava/lang/Object;C:" + std::string(Interface ? ":" : "") +
          "Lfixture/Base<" + Required + ">;>Ljava/lang/Object;";
      Input.at("Lfixture/Host;").fields[0].generic_signature =
          "Lfixture/Bounded<TX;Lfixture/Leaf<" + Actual + ">;>;";
      Budget B;
      rejected([&] { validateGenericSignatures(Input, B); },
               "type argument bound is not proven");
    }
    for (const auto &Variance : {"+", "-"}) {
      auto Input = forwardedWildcardBoundClasses(Interface);
      Input.at("Lfixture/Bounded;").generic_signature =
          "<T:Ljava/lang/Object;C:" + std::string(Interface ? ":" : "") +
          "Lfixture/Base<" + Variance + "TT;>;>Ljava/lang/Object;";
      Input.at("Lfixture/Host;").fields[0].generic_signature =
          std::string(Variance) == "+"
              ? "Lfixture/Bounded<Lfixture/ChildValue;Lfixture/Leaf<+Lfixture/"
                "Value;>;>;"
              : "Lfixture/Bounded<Lfixture/Value;Lfixture/Leaf<-Lfixture/"
                "ChildValue;>;>;";
      Budget B;
      rejected([&] { validateGenericSignatures(Input, B); },
               "type argument bound is not proven");
    }
  }
}

TEST(MobileDalvikSignature,
     WildcardInheritanceCannotRewriteNestedCaptureIdentity) {
  for (bool Interface : {false, true}) {
    for (bool Array : {false, true}) {
      auto Input = forwardedWildcardBoundClasses(Interface);
      const auto Actual = Array ? "[TU;" : "Ljava/util/List<TU;>;";
      const auto Required = Array ? "[TT;" : "Ljava/util/List<+TT;>;";
      Input.at("Lfixture/Middle;").generic_signature =
          "<U:Ljava/lang/Object;>" +
          std::string(Interface ? "Ljava/lang/Object;" : "") +
          "Lfixture/Base<" + Actual + ">;";
      Input.at("Lfixture/Bounded;").generic_signature =
          "<T:Ljava/lang/Object;C:" + std::string(Interface ? ":" : "") +
          "Lfixture/Base<" + Required + ">;>Ljava/lang/Object;";
      Budget B;
      rejected([&] { validateGenericSignatures(Input, B); },
               "type argument bound is not proven");
    }
  }
}

TEST(MobileDalvikSignature, WildcardInheritanceVerifiesEdgesAndStopsCycles) {
  for (bool Interface : {false, true}) {
    auto Input = forwardedWildcardBoundClasses(Interface);
    auto &Middle = Input.at("Lfixture/Middle;");
    Middle.external = true;
    Middle.generic_signature =
        "<U:Ljava/lang/Object;>" +
        std::string(Interface ? "Ljava/lang/Object;" : "") +
        "Lfixture/Base<TMissing;>;";
    Budget B;
    rejected([&] { validateGenericSignatures(Input, B); },
             "unbound type variable Missing");
    Input = forwardedWildcardBoundClasses(Interface);
    auto &Cyclic = Input.at("Lfixture/Middle;");
    Cyclic.generic_signature =
        "<U:Ljava/lang/Object;>" +
        std::string(Interface ? "Ljava/lang/Object;" : "") +
        "Lfixture/Leaf<TU;>;";
    if (Interface)
      Cyclic.interfaces = {"Lfixture/Leaf;"};
    else
      Cyclic.superclass = "Lfixture/Leaf;";
    rejected([&] { validateGenericSignatures(Input, B); },
             "generic bound proof exceeds depth limit");
  }
}

namespace {
ClassMap selfWildcardBoundClasses(bool Conflict) {
  auto Root = genericClass(
      "<P:Ljava/lang/Object;Q:Ljava/lang/Object;>Ljava/lang/Object;");
  Root.name = "Lfixture/Root;";
  Root.access.insert("interface");
  auto Source = genericClass(
      "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;"
      "Lfixture/Root<TT;TU;>;");
  Source.name = "Lfixture/Source;";
  Source.interfaces = {Root.name};
  auto Bound = genericClass(
      Conflict ? "<C::Lfixture/Root<+Ljava/lang/String;Ljava/lang/String;>;>"
                 "Ljava/lang/Object;"
               : "<A:Ljava/lang/Object;B:Ljava/lang/Object;"
                 "C::Lfixture/Root<+TA;TB;>;>Ljava/lang/Object;");
  Bound.name = "Lfixture/Bound;";
  Source.fields.push_back({{Source.name, "value", Bound.name},
                           {"public"},
                           {},
                           Conflict ? "Lfixture/Bound<Lfixture/Source<"
                                      "+Ljava/lang/String;TT;>;>;"
                                    : "Lfixture/Bound<TT;TU;"
                                      "Lfixture/Source<+TT;TU;>;>;"});
  return {{Root.name, Root}, {Source.name, Source}, {Bound.name, Bound}};
}
} // namespace

TEST(MobileDalvikSignature, WildcardInheritanceKeepsIdentitySelfBindings) {
  auto Input = selfWildcardBoundClasses(false);
  Budget B;
  EXPECT_NO_THROW(validateGenericSignatures(Input, B));
}

TEST(MobileDalvikSignature,
     WildcardInheritanceCannotRebindOuterInstanceFormals) {
  auto Input = selfWildcardBoundClasses(true);
  Budget B;
  rejected([&] { validateGenericSignatures(Input, B); },
           "type argument bound is not proven");
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
  for (const auto &name :
       {"Ljava/lang/Object;", "Ljava/lang/Number;", "Ljava/lang/String;",
        "Ljava/lang/Exception;", "Ljava/lang/Throwable;"}) {
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
  for (const auto &name : {"Ljava/lang/CharSequence;", "Ljava/io/Serializable;",
                           "Ljava/lang/Comparable;", "Ljava/util/List;"}) {
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

TEST(MobileDalvikSignature,
     InputBoundKindOverridesPlatformFactInBothDirections) {
  for (const auto &name : {"Ljava/lang/CharSequence;", "Ljava/lang/Number;"}) {
    SCOPED_TRACE(name);
    Class supplied;
    supplied.name = name;
    supplied.superclass = "Ljava/lang/Object;";
    bool supplied_interface = supplied.name == "Ljava/lang/Number;";
    supplied.access =
        supplied_interface ? Access{"interface", "abstract"} : Access{"public"};
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
    cls.generic_signature = (supplied_interface ? "<T:" : "<T::") +
                            supplied.name + ">Ljava/lang/Object;";
    rejected([&] { check(); },
             supplied_interface ? "class-bound position names a known interface"
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

TEST(MobileDalvikSignature, TypeVariableBoundsReportSourcePositionChanges) {
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
  cls.generic_signature =
      "<T::TU;U::Ljava/lang/CharSequence;>Ljava/lang/Object;";
  const auto projected = validate(cls);
  const auto &source = projected.classes.at(cls.name);
  ASSERT_TRUE(source.type_parameters[0].class_bound);
  EXPECT_TRUE(source.type_parameters[0].interface_bounds.empty());
  EXPECT_EQ(source.type_parameters[0].erasure, "Ljava/lang/CharSequence;");
  EXPECT_EQ(source.types[*source.type_parameters[0].class_bound].variable_owner,
            cls.name);
  ASSERT_EQ(source.source_projections.size(), 1u);
  EXPECT_EQ(source.source_projections[0].kind, "type-variable-bound-position");
  EXPECT_EQ(*cls.generic_signature,
            "<T::TU;U::Ljava/lang/CharSequence;>Ljava/lang/Object;");
}

TEST(MobileDalvikSignature, TypeVariableBoundProjectionCannotDropOtherBounds) {
  for (const auto &raw : {"<T::TU;:Ljava/io/Serializable;U::Ljava/lang/"
                          "CharSequence;>Ljava/lang/Object;",
                          "<T:Ljava/lang/Object;:TU;U::Ljava/lang/"
                          "CharSequence;>Ljava/lang/Object;"}) {
    auto cls = genericClass(raw);
    rejected([&] { validate(cls); },
             "bound cannot be expressed as a Java type bound");
  }
  auto cls = genericClass("<T::TU;>Ljava/lang/Object;");
  rejected([&] { validate(cls); }, "unbound type variable");
  cls.generic_signature = "<T::TU;U::TT;>Ljava/lang/Object;";
  rejected([&] { validate(cls); }, "cyclic type-variable erasure");
}

TEST(MobileDalvikSignature, BoundKindFactsDoNotAuthorizeTypeArguments) {
  for (const auto &name :
       {"Ljava/lang/CharSequence;", "Ljava/io/Serializable;"}) {
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

namespace {
std::string metadataVarint(uint64_t value) {
  std::string result;
  do {
    auto low = uint8_t(value & 127);
    value >>= 7;
    result += char(low | (value ? 128 : 0));
  } while (value);
  return result;
}
std::string metadataInt(unsigned field, uint64_t value) {
  return metadataVarint(field << 3) + metadataVarint(value);
}
std::string metadataBytes(unsigned field, const std::string &value) {
  return metadataVarint((field << 3) | 2) + metadataVarint(value.size()) +
         value;
}
AnnotationValue metadataNumber(int64_t value) {
  AnnotationValue result;
  result.kind = 0x04;
  result.value = value;
  return result;
}
AnnotationValue metadataText(const std::string &value) {
  AnnotationValue result;
  result.kind = 0x17;
  result.value = value;
  return result;
}
AnnotationValue metadataArray(std::vector<AnnotationValue> values) {
  AnnotationValue result;
  result.kind = 0x1c;
  result.array = std::move(values);
  return result;
}
struct MetadataFixture {
  Class owner;
  Method method;
  std::string function, records, extra_function;
  std::vector<std::string> strings;
};
MetadataFixture metadataFixture() {
  MetadataFixture result;
  result.owner.name = "Lfixture/ScopeHost;";
  result.owner.superclass = "Ljava/lang/Object;";
  result.owner.access = {"public", "final"};
  result.method.reference = {result.owner.name,
                             "convert",
                             {"Lfixture/Input;", "Lfixture/Callback;"},
                             "Lfixture/Output;"};
  result.method.access = {"public", "static", "synthetic"};
  result.strings = {"convert",
                    "A",
                    "B",
                    "fixture/Input",
                    "fixture/Callback",
                    "fixture/Output",
                    result.owner.name,
                    "(Lfixture/Input;Lfixture/Callback;)Lfixture/Output;",
                    ""};
  result.records = metadataBytes(1, metadataInt(1, 6)) +
                   metadataBytes(1, metadataInt(3, 2)) + metadataBytes(1, "") +
                   metadataBytes(1, metadataInt(2, 0));
  result.function =
      metadataInt(2, 0) + metadataBytes(3, metadataInt(6, 5)) +
      metadataBytes(4, metadataInt(1, 0) + metadataInt(2, 1)) +
      metadataBytes(4, metadataInt(1, 1) + metadataInt(2, 2)) +
      metadataBytes(5, metadataInt(6, 3)) +
      metadataBytes(6, metadataInt(2, 0) + metadataBytes(3, metadataInt(6, 4)));
  return result;
}
void encodeMetadata(MetadataFixture &fixture, bool class_owner = false,
                    bool duplicate = false) {
  auto body = metadataBytes(class_owner ? 9 : 3, fixture.function);
  if (duplicate)
    body += body;
  if (!fixture.extra_function.empty())
    body += metadataBytes(class_owner ? 9 : 3, fixture.extra_function);
  if (class_owner)
    body += metadataInt(3, 6);
  auto data = metadataVarint(fixture.records.size()) + fixture.records + body;
  std::string encoded(1, '\0');
  for (uint8_t byte : data) {
    if (byte < 128)
      encoded += char(byte);
    else {
      encoded += char(0xc0 | (byte >> 6));
      encoded += char(0x80 | (byte & 63));
    }
  }
  std::vector<AnnotationValue> names;
  for (const auto &name : fixture.strings)
    names.push_back(metadataText(name));
  AnnotationUse metadata;
  metadata.type = "Lkotlin/Metadata;";
  metadata.elements = {
      {"k", metadataNumber(class_owner ? 1 : 2)},
      {"mv", metadataArray(
                 {metadataNumber(2), metadataNumber(0), metadataNumber(0)})},
      {"d1", metadataArray({metadataText(encoded)})},
      {"d2", metadataArray(std::move(names))}};
  fixture.owner.annotations = {metadata};
  fixture.owner.methods = {fixture.method};
}
} // namespace

TEST(MobileDalvikSignature, KotlinMetadataRequiresAnExactMethodDeclaration) {
  for (bool ClassOwner : {false, true}) {
    auto fixture = metadataFixture();
    encodeMetadata(fixture, ClassOwner);
    Budget budget;
    const auto names =
        kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method, budget);
    ASSERT_TRUE(names);
    EXPECT_EQ(*names, (std::vector<std::string>{"A", "B"}));
    fixture.method.reference.parameters[1] = "Lfixture/DifferentCallback;";
    EXPECT_FALSE(kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method,
                                                  budget));
  }
  auto fixture = metadataFixture();
  fixture.function += metadataBytes(100, metadataInt(1, 0) + metadataInt(2, 7));
  encodeMetadata(fixture);
  Budget budget;
  EXPECT_TRUE(
      kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method, budget));
  fixture.strings[7] = "()Ljava/lang/Object;";
  encodeMetadata(fixture);
  EXPECT_FALSE(
      kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method, budget));
}

TEST(MobileDalvikSignature, KotlinMetadataSupportsIndexedTypesAndSplitBytes) {
  auto fixture = metadataFixture();
  fixture.function = metadataInt(2, 0) + metadataInt(7, 0) +
                     metadataBytes(4, metadataInt(1, 0) + metadataInt(2, 1) +
                                          metadataBytes(5, metadataInt(6, 8))) +
                     metadataInt(8, 1) +
                     metadataBytes(6, metadataInt(2, 0) + metadataInt(5, 2)) +
                     metadataBytes(30, metadataBytes(1, metadataInt(6, 5)) +
                                           metadataBytes(1, metadataInt(6, 3)) +
                                           metadataBytes(1, metadataInt(6, 4)));
  encodeMetadata(fixture);
  auto &parts = fixture.owner.annotations[0].elements["d1"].array;
  auto encoded = std::get<std::string>(parts[0].value);
  parts = {metadataText(encoded.substr(0, 1)), metadataText(encoded.substr(1))};
  Budget budget;
  auto names =
      kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method, budget);
  ASSERT_TRUE(names);
  EXPECT_EQ(*names, (std::vector<std::string>{"A"}));
}

TEST(MobileDalvikSignature,
     KotlinMetadataCannotInventBoundsOrChooseDuplicates) {
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    auto fixture = metadataFixture();
    if (mutation < 4) {
      std::string formal = metadataInt(1, 2) + metadataInt(2, 2);
      if (mutation == 0)
        formal += metadataBytes(5, metadataInt(6, 3));
      if (mutation == 1)
        formal += metadataInt(3, 1);
      if (mutation == 2)
        formal += metadataBytes(6, metadataVarint(0));
      if (mutation == 3)
        formal += metadataInt(4, 1);
      // Replace the second formal so duplicate names cannot mask the bound
      // check.
      auto original = metadataBytes(4, metadataInt(1, 1) + metadataInt(2, 2));
      fixture.function.replace(fixture.function.find(original), original.size(),
                               metadataBytes(4, formal));
    }
    if (mutation == 4)
      fixture.function += metadataInt(9, 1 << 13);
    if (mutation == 5)
      fixture.function += metadataBytes(10, metadataInt(6, 3));
    encodeMetadata(fixture, false, mutation == 6);
    Budget budget;
    EXPECT_FALSE(
        kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method, budget))
        << mutation;
  }
}

TEST(MobileDalvikSignature, KotlinMetadataCannotSkipAConflictingBoundedOrigin) {
  for (bool reverse : {false, true}) {
    auto fixture = metadataFixture();
    const auto original =
        metadataBytes(4, metadataInt(1, 1) + metadataInt(2, 2));
    fixture.extra_function = fixture.function;
    fixture.extra_function.replace(
        fixture.extra_function.find(original), original.size(),
        metadataBytes(4, metadataInt(1, 1) + metadataInt(2, 2) +
                             metadataBytes(5, metadataInt(6, 3))));
    if (reverse)
      std::swap(fixture.function, fixture.extra_function);
    encodeMetadata(fixture);
    Budget budget;
    EXPECT_FALSE(kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method,
                                                  budget));
  }
}

TEST(MobileDalvikSignature,
     KotlinMetadataRejectsMalformedOrUnsupportedEncoding) {
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    auto fixture = metadataFixture();
    encodeMetadata(fixture);
    auto &metadata = fixture.owner.annotations[0];
    if (mutation == 0)
      metadata.elements["mv"].array[0] = metadataNumber(3);
    if (mutation == 1)
      metadata.elements["k"] = metadataNumber(3);
    if (mutation == 2)
      std::get<std::string>(metadata.elements["d1"].array[0].value)[0] = 'x';
    if (mutation == 3)
      metadata.elements["d1"].array[0] = metadataText(std::string("\0\x80", 2));
    if (mutation == 4)
      metadata.elements["d2"].array.pop_back();
    if (mutation == 5)
      fixture.owner.annotations.push_back(metadata);
    if (mutation == 6)
      fixture.method.access.erase("synthetic");
    if (mutation == 7)
      fixture.method.generic_signature = "()V";
    Budget budget;
    EXPECT_FALSE(
        kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method, budget))
        << mutation;
  }
}

TEST(MobileDalvikSignature,
     KotlinMetadataScopeIsSharedAndKeepsMethodShadowing) {
  auto fixture = metadataFixture();
  encodeMetadata(fixture);
  Class local;
  local.name = "Lfixture/ScopeHost$1;";
  local.superclass = "Ljava/lang/Object;";
  local.inner_class_present = true;
  local.enclosing_method = fixture.method.reference;
  local.access = {"final"};
  Field field;
  field.reference = {local.name, "value", "Ljava/lang/Object;"};
  field.generic_signature = "TA;";
  local.fields = {field};
  auto method = genericMethod(local, "<A:Ljava/lang/Object;>(TA;)TA;",
                              {"Ljava/lang/Object;"}, "Ljava/lang/Object;");
  local.methods = {method};
  Budget budget;
  auto plans = validateGenericSignatures(
      {{fixture.owner.name, fixture.owner}, {local.name, local}}, budget);
  const auto &changes = plans.fields.at(field.reference).source_projections;
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].kind, "kotlin-metadata-method-type-variable");
  EXPECT_EQ(changes[0].scope_origin, fixture.method.reference.identity());
  EXPECT_EQ(changes[0].source_type, "Ljava/lang/Object;");
  EXPECT_TRUE(plans.methods.at(method.reference).source_projections.empty());
  fixture.owner.annotations.clear();
  rejected(
      [&] {
        validateGenericSignatures(
            {{fixture.owner.name, fixture.owner}, {local.name, local}}, budget);
      },
      "unbound type variable A");
}

namespace {
MetadataFixture unitPackagePartMetadataFixture() {
  auto fixture = metadataFixture();
  fixture.method.reference.returns = "V";
  fixture.strings[5] = "kotlin/Unit";
  fixture.strings[7] = "(Lfixture/Input;Lfixture/Callback;)V";
  fixture.records = metadataBytes(1, metadataInt(1, 5)) +
                    metadataBytes(1, metadataInt(2, 2)) +
                    metadataBytes(1, metadataInt(3, 2)) + metadataBytes(1, "") +
                    metadataBytes(1, metadataInt(2, 0));
  fixture.function += metadataBytes(100, metadataInt(2, 7));
  encodeMetadata(fixture);
  fixture.owner.annotations[0].elements["k"] = metadataNumber(5);
  fixture.owner.annotations[0].elements["xs"] =
      metadataText("fixture/ScopeFacade");
  return fixture;
}
} // namespace

TEST(MobileDalvikSignature, KotlinPackagePartsProveExactVoidMethodScopes) {
  auto fixture = unitPackagePartMetadataFixture();
  Budget budget;
  const auto names =
      kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method, budget);
  ASSERT_TRUE(names);
  EXPECT_EQ(*names, (std::vector<std::string>{"A", "B"}));
  fixture.owner.name = "LScopeHost;";
  fixture.method.reference.owner = fixture.owner.name;
  fixture.owner.annotations[0].elements["xs"] = metadataText("ScopeFacade");
  EXPECT_TRUE(
      kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method, budget));
}

TEST(MobileDalvikSignature, KotlinPackagePartsRejectUnprovenFacadeNames) {
  for (unsigned mutation = 0; mutation < 10; ++mutation) {
    SCOPED_TRACE(mutation);
    auto fixture = unitPackagePartMetadataFixture();
    auto &metadata = fixture.owner.annotations[0];
    switch (mutation) {
    case 0:
      metadata.elements.erase("xs");
      break;
    case 1:
      metadata.elements["xs"] = metadataNumber(1);
      break;
    case 2:
      metadata.elements["xs"] = metadataText("");
      break;
    case 3:
      metadata.elements["xs"] = metadataText("fixture/ScopeHost");
      break;
    case 4:
      metadata.elements["xs"] = metadataText("other/ScopeFacade");
      break;
    case 5:
      metadata.elements["xs"] = metadataText("Lfixture/ScopeFacade;");
      break;
    case 6:
      metadata.elements["xs"] = metadataText("fixture//ScopeFacade");
      break;
    case 7:
      metadata.elements["xs"] = metadataText("fixture/1ScopeFacade");
      break;
    case 8:
      metadata.elements["xs"] = metadataText("fixture/ScopeFacade/");
      break;
    case 9:
      metadata.elements["k"] = metadataNumber(4);
      break;
    }
    Budget budget;
    EXPECT_FALSE(kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method,
                                                  budget));
  }
}

TEST(MobileDalvikSignature, KotlinUnitIsVoidOnlyInAnExactNonNullReturn) {
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    SCOPED_TRACE(mutation);
    auto fixture = unitPackagePartMetadataFixture();
    if (mutation == 0)
      fixture.method.reference.returns = "Lkotlin/Unit;";
    if (mutation == 1) {
      const auto old = metadataBytes(3, metadataInt(6, 5));
      fixture.function.replace(
          fixture.function.find(old), old.size(),
          metadataBytes(3, metadataInt(6, 5) + metadataInt(3, 1)));
    }
    if (mutation == 2) {
      const auto old = metadataBytes(5, metadataInt(6, 3));
      fixture.function.replace(fixture.function.find(old), old.size(),
                               metadataBytes(5, metadataInt(6, 5)));
    }
    if (mutation == 3) {
      const auto old = metadataBytes(4, metadataInt(1, 0) + metadataInt(2, 1));
      fixture.function.replace(
          fixture.function.find(old), old.size(),
          metadataBytes(4, metadataInt(1, 0) + metadataInt(2, 1) +
                               metadataBytes(5, metadataInt(6, 5))));
    }
    if (mutation == 4)
      fixture.strings[7] =
          "(Lfixture/Input;Lfixture/Callback;)Ljava/lang/Object;";
    encodeMetadata(fixture);
    fixture.owner.annotations[0].elements["k"] = metadataNumber(5);
    fixture.owner.annotations[0].elements["xs"] =
        metadataText("fixture/ScopeFacade");
    Budget budget;
    EXPECT_FALSE(kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method,
                                                  budget));
  }
}

TEST(MobileDalvikSignature, KotlinMethodFormalsAcceptDirectIdsAndNames) {
  for (bool named : {false, true}) {
    auto fixture = unitPackagePartMetadataFixture();
    const auto old = metadataBytes(6, metadataInt(2, 0) +
                                          metadataBytes(3, metadataInt(6, 4)));
    fixture.function.replace(
        fixture.function.find(old), old.size(),
        metadataBytes(6, metadataInt(2, 0) +
                             metadataBytes(3, metadataInt(named ? 9 : 7,
                                                          named ? 1 : 0))));
    fixture.method.reference.parameters[1] = "Ljava/lang/Object;";
    fixture.strings[7] = "(Lfixture/Input;Ljava/lang/Object;)V";
    encodeMetadata(fixture);
    fixture.owner.annotations[0].elements["k"] = metadataNumber(5);
    fixture.owner.annotations[0].elements["xs"] =
        metadataText("fixture/ScopeFacade");
    Budget budget;
    const auto names =
        kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method, budget);
    ASSERT_TRUE(names);
    EXPECT_EQ(*names, (std::vector<std::string>{"A", "B"}));
  }
}

TEST(MobileDalvikSignature, KotlinMethodFormalsRejectUnknownOrMixedOrigins) {
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    SCOPED_TRACE(mutation);
    auto fixture = unitPackagePartMetadataFixture();
    const auto old = metadataBytes(6, metadataInt(2, 0) +
                                          metadataBytes(3, metadataInt(6, 4)));
    auto type = metadataInt(9, mutation == 0 ? 0 : 1);
    if (mutation == 1)
      type += metadataInt(7, 0);
    if (mutation == 2)
      type += metadataInt(6, 4);
    if (mutation == 3)
      type = metadataInt(7, 99);
    fixture.function.replace(
        fixture.function.find(old), old.size(),
        metadataBytes(6, metadataInt(2, 0) + metadataBytes(3, type)));
    fixture.method.reference.parameters[1] = "Ljava/lang/Object;";
    fixture.strings[7] = "(Lfixture/Input;Ljava/lang/Object;)V";
    encodeMetadata(fixture);
    Budget budget;
    EXPECT_FALSE(kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method,
                                                  budget));
  }
}

TEST(MobileDalvikSignature, KotlinPrimitiveTypesKeepExactBoxedDescriptors) {
  const std::vector<std::tuple<unsigned, std::string, std::string>> types = {
      {5, "B", "Byte"},     {6, "D", "Double"},    {7, "F", "Float"},
      {8, "I", "Integer"},  {9, "J", "Long"},      {10, "S", "Short"},
      {11, "Z", "Boolean"}, {12, "C", "Character"}};
  for (const auto &[predefined, primitive, boxed] : types)
    for (bool nullable : {false, true}) {
      SCOPED_TRACE(primitive + (nullable ? " boxed" : " primitive"));
      auto fixture = unitPackagePartMetadataFixture();
      const auto type = metadataInt(6, 4) + metadataInt(3, nullable);
      const auto old = metadataBytes(
          6, metadataInt(2, 0) + metadataBytes(3, metadataInt(6, 4)));
      fixture.function.replace(
          fixture.function.find(old), old.size(),
          metadataBytes(6, metadataInt(2, 0) + metadataBytes(3, type)));
      fixture.records = metadataBytes(1, metadataInt(1, 4)) +
                        metadataBytes(1, metadataInt(2, predefined)) +
                        metadataBytes(1, metadataInt(2, 2)) +
                        metadataBytes(1, metadataInt(3, 2)) +
                        metadataBytes(1, "") +
                        metadataBytes(1, metadataInt(2, 0));
      auto descriptor = nullable ? "Ljava/lang/" + boxed + ";" : primitive;
      fixture.method.reference.parameters[1] = descriptor;
      fixture.strings[7] = "(Lfixture/Input;" + descriptor + ")V";
      encodeMetadata(fixture);
      Budget budget;
      EXPECT_TRUE(kotlinUnconstrainedMethodFormals(fixture.owner,
                                                   fixture.method, budget));
      fixture.method.reference.parameters[1] = "Ljava/lang/Object;";
      EXPECT_FALSE(kotlinUnconstrainedMethodFormals(fixture.owner,
                                                    fixture.method, budget));
    }
}

TEST(MobileDalvikSignature, KotlinTypeTableNullabilityCannotUnboxAParameter) {
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    SCOPED_TRACE(mutation);
    auto fixture = unitPackagePartMetadataFixture();
    const auto old = metadataBytes(6, metadataInt(2, 0) +
                                          metadataBytes(3, metadataInt(6, 4)));
    fixture.function.replace(
        fixture.function.find(old), old.size(),
        metadataBytes(6, metadataInt(2, 0) + metadataInt(5, 0)));
    fixture.records = metadataBytes(1, metadataInt(1, 4)) +
                      metadataBytes(1, metadataInt(2, 8)) +
                      metadataBytes(1, metadataInt(2, 2)) +
                      metadataBytes(1, metadataInt(3, 2)) +
                      metadataBytes(1, "") +
                      metadataBytes(1, metadataInt(2, 0));
    fixture.function += metadataBytes(
        30, metadataBytes(1, metadataInt(6, 4) + metadataInt(3, 0)) +
                metadataInt(2, mutation == 3 ? 2 : 0));
    fixture.method.reference.parameters[1] =
        mutation == 1 ? "I" : "Ljava/lang/Integer;";
    fixture.strings[7] =
        "(Lfixture/Input;" + fixture.method.reference.parameters[1] + ")V";
    if (mutation == 2)
      fixture.method.reference.parameters[1] = "Ljava/lang/Object;";
    encodeMetadata(fixture);
    Budget budget;
    EXPECT_EQ(bool(kotlinUnconstrainedMethodFormals(fixture.owner,
                                                    fixture.method, budget)),
              mutation == 0);
  }
}

TEST(MobileDalvikSignature, KotlinOrdinaryCoroutineReferencesKeepTheirNames) {
  for (const auto &name : {"kotlin/coroutines/CoroutineContext",
                           "kotlin/coroutines/Continuation"}) {
    auto fixture = unitPackagePartMetadataFixture();
    fixture.strings[4] = name;
    fixture.method.reference.parameters[1] = "L" + std::string(name) + ";";
    fixture.strings[7] =
        "(Lfixture/Input;" + fixture.method.reference.parameters[1] + ")V";
    encodeMetadata(fixture);
    Budget budget;
    EXPECT_TRUE(kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method,
                                                 budget));
    fixture.method.reference.parameters[1] = "Ljava/lang/Object;";
    EXPECT_FALSE(kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method,
                                                  budget));
  }
}

TEST(MobileDalvikSignature, KotlinFunctionClassifiersMatchRuntimeInterfaces) {
  for (unsigned arity = 0; arity < 23; ++arity) {
    SCOPED_TRACE(arity);
    auto fixture = metadataFixture();
    fixture.strings[4] = "kotlin/Function" + std::to_string(arity);
    fixture.method.reference.parameters[1] =
        "Lkotlin/jvm/functions/Function" + std::to_string(arity) + ";";
    encodeMetadata(fixture);
    Budget budget;
    EXPECT_TRUE(kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method,
                                                 budget));
  }
  for (const auto &suffix : {"23", "00", "-1", "N"}) {
    auto fixture = metadataFixture();
    fixture.strings[4] = "kotlin/Function" + std::string(suffix);
    fixture.method.reference.parameters[1] =
        "Lkotlin/jvm/functions/Function" + std::string(suffix) + ";";
    encodeMetadata(fixture);
    Budget budget;
    EXPECT_FALSE(kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method,
                                                  budget));
  }
}

namespace {
MetadataFixture suspendCallbackMetadataFixture(unsigned mutation = 99) {
  auto fixture = metadataFixture();
  fixture.strings[4] = mutation == 3 ? "kotlin/Function0" : "kotlin/Function2";
  fixture.strings.push_back(mutation == 6 ? "kotlin/coroutines/Other"
                                          : "kotlin/coroutines/Continuation");
  fixture.records += metadataBytes(1, "");
  const auto formal = metadataBytes(2, metadataInt(9, 1));
  auto continuation = metadataInt(6, 9) + metadataBytes(2, formal);
  if (mutation == 5)
    continuation += metadataInt(3, 1);
  auto result = mutation == 8 ? metadataInt(9, 1) : metadataInt(6, 8);
  if (mutation != 7)
    result += metadataInt(3, 1);
  auto callback = metadataInt(1, mutation == 2 ? 2 : 1) + metadataInt(6, 4) +
                  metadataBytes(2, formal);
  if (mutation != 4)
    callback += metadataBytes(2, metadataBytes(2, continuation));
  callback += metadataBytes(2, metadataBytes(2, result));
  const auto old =
      metadataBytes(6, metadataInt(2, 0) + metadataBytes(3, metadataInt(6, 4)));
  fixture.function.replace(
      fixture.function.find(old), old.size(),
      metadataBytes(6, metadataInt(2, 0) + metadataBytes(3, callback)));
  fixture.method.reference.parameters[1] = "Lkotlin/jvm/functions/Function2;";
  fixture.strings[7] = "(Lfixture/Input;Lkotlin/jvm/functions/Function2;)" +
                       fixture.method.reference.returns;
  if (mutation == 1)
    fixture.strings[7] = "(Lfixture/Input;Ljava/lang/Object;)Lfixture/Output;";
  if (mutation != 0)
    fixture.function += metadataBytes(100, metadataInt(2, 7));
  encodeMetadata(fixture);
  return fixture;
}
} // namespace

TEST(MobileDalvikSignature, KotlinSuspendCallbacksRequireExactJvmAndAbiTail) {
  auto fixture = suspendCallbackMetadataFixture();
  Budget budget;
  const auto names =
      kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method, budget);
  ASSERT_TRUE(names);
  EXPECT_EQ(*names, (std::vector<std::string>{"A", "B"}));
}

TEST(MobileDalvikSignature, KotlinSuspendCallbacksRejectUnprovenJvmOrAbiTail) {
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    SCOPED_TRACE(mutation);
    auto fixture = suspendCallbackMetadataFixture(mutation);
    Budget budget;
    EXPECT_FALSE(kotlinUnconstrainedMethodFormals(fixture.owner, fixture.method,
                                                  budget));
  }
}

namespace {
ClassMap lambdaScopeFixture() {
  Budget budget;
  std::vector<std::string> sources = {R"(
.class public interface abstract Lfixture/Mapper;
.super Ljava/lang/Object;
.annotation system Ldalvik/annotation/Signature;
 value = {"<E:Ljava/lang/Object;>Ljava/lang/Object;"}
.end annotation
.method public abstract map(Ljava/lang/Object;)Ljava/lang/Object;
 .annotation system Ldalvik/annotation/Signature;
  value = {"(TE;)TE;"}
 .end annotation
.end method
)",
                                      R"(
.class public interface abstract Lfixture/Factory;
.super Ljava/lang/Object;
.method public abstract create(Ljava/lang/Object;)Lfixture/Mapper;
.end method
)",
                                      R"(
.class public final Lfixture/LambdaHost;
.super Ljava/lang/Object;
.method public static wrap(Ljava/lang/Object;)Lfixture/Mapper;
 .registers 2
 .annotation system Ldalvik/annotation/Signature;
  value = {"<T:Ljava/lang/Object;>(TT;)Lfixture/Mapper<TT;>;"}
 .end annotation
 new-instance v0, Lfixture/Adapter;
 invoke-direct {v0}, Lfixture/Adapter;-><init>()V
 invoke-interface {v0, p0}, Lfixture/Factory;->create(Ljava/lang/Object;)Lfixture/Mapper;
 move-result-object v0
 return-object v0
.end method
.method static synthetic lambda$wrap$0(Ljava/lang/Object;)Lfixture/Mapper;
 .registers 2
 new-instance v0, Lfixture/LambdaHost$1;
 invoke-direct {v0, p0}, Lfixture/LambdaHost$1;-><init>(Ljava/lang/Object;)V
 return-object v0
.end method
)",
                                      R"(
.class public final synthetic Lfixture/Adapter;
.super Ljava/lang/Object;
.implements Lfixture/Factory;
.method public constructor synthetic <init>()V
 .registers 1
 invoke-direct {p0}, Ljava/lang/Object;-><init>()V
 return-void
.end method
.method public final create(Ljava/lang/Object;)Lfixture/Mapper;
 .registers 2
 invoke-static {p1}, Lfixture/LambdaHost;->lambda$wrap$0(Ljava/lang/Object;)Lfixture/Mapper;
 move-result-object p1
 return-object p1
.end method
)",
                                      R"(
.class final Lfixture/LambdaHost$1;
.super Ljava/lang/Object;
.implements Lfixture/Mapper;
.annotation system Ldalvik/annotation/Signature;
 value = {"Ljava/lang/Object;Lfixture/Mapper<TT;>;"}
.end annotation
.annotation system Ldalvik/annotation/EnclosingMethod;
 value = Lfixture/LambdaHost;->lambda$wrap$0(Ljava/lang/Object;)Lfixture/Mapper;
.end annotation
.annotation system Ldalvik/annotation/InnerClass;
 accessFlags = 0x8
 name = null
.end annotation
.field final synthetic captured:Ljava/lang/Object;
.method constructor <init>(Ljava/lang/Object;)V
 .registers 2
 iput-object p1, p0, Lfixture/LambdaHost$1;->captured:Ljava/lang/Object;
 invoke-direct {p0}, Ljava/lang/Object;-><init>()V
 return-void
.end method
.method public map(Ljava/lang/Object;)Ljava/lang/Object;
 .registers 2
 .annotation system Ldalvik/annotation/Signature;
  value = {"(TT;)TT;"}
 .end annotation
 return-object p1
.end method
)"};
  ClassMap result;
  for (const auto &source : sources) {
    auto cls = parseSmali(source, "lambda-scope.smali", budget);
    result.emplace(cls.name, std::move(cls));
  }
  return result;
}
} // namespace

TEST(MobileDalvikSignature,
     DesugaredLambdaScopeRequiresTheCreationAndCallChain) {
  auto classes = lambdaScopeFixture();
  Budget budget;
  auto plans = validateGenericSignatures(classes, budget);
  const auto &changes =
      plans.classes.at("Lfixture/LambdaHost$1;").source_projections;
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].kind, "desugared-lambda-method-type-variable");
  EXPECT_EQ(changes[0].scope_origin,
            "Lfixture/LambdaHost;->wrap(Ljava/lang/Object;)Lfixture/Mapper;");
  EXPECT_EQ(changes[0].source_type, "Ljava/lang/Object;");
  auto &local = classes.at("Lfixture/LambdaHost$1;");
  local.methods.back().generic_signature = "<T:Ljava/lang/Object;>(TT;)TT;";
  plans = validateGenericSignatures(classes, budget);
  EXPECT_TRUE(plans.methods.at(local.methods.back().reference)
                  .source_projections.empty());
  EXPECT_FALSE(plans.methods.contains(
      classes.at("Lfixture/LambdaHost;").methods[1].reference));
}

TEST(MobileDalvikSignature,
     DesugaredLambdaScopeCannotGuessMissingWitnessesOrBounds) {
  for (unsigned mutation = 0; mutation < 10; ++mutation) {
    SCOPED_TRACE(mutation);
    auto classes = lambdaScopeFixture();
    auto &host = classes.at("Lfixture/LambdaHost;");
    auto &wrapper = classes.at("Lfixture/Adapter;");
    switch (mutation) {
    case 0:
      host.methods[0].instructions[0].reference =
          std::string("Lfixture/Other;");
      break;
    case 1:
      wrapper.methods[1].instructions[0].registers = {0};
      break;
    case 2:
      wrapper.methods[1].instructions[0].opcode = "invoke-virtual";
      break;
    case 3:
      wrapper.access.erase("synthetic");
      break;
    case 4:
      host.methods[0].generic_signature =
          "<T:Ljava/lang/Object;:Ljava/io/Serializable;>(TT;)Lfixture/"
          "Mapper<TT;>;";
      break;
    case 5:
      host.methods[0].generic_signature.reset();
      break;
    case 6:
      host.methods[1].instructions[1].registers = {0, 0};
      break;
    case 7:
      host.methods[0].reference.name = "anotherName";
      break;
    case 8: {
      auto other = host.methods[0];
      other.reference.parameters.push_back("I");
      other.generic_signature =
          "<T:Ljava/lang/Object;>(TT;I)Lfixture/Mapper<TT;>;";
      host.methods.push_back(std::move(other));
      break;
    }
    case 9:
      classes.at("Lfixture/LambdaHost$1;").generic_signature =
          "Ljava/lang/Object;Lfixture/Mapper<TMissing;>;";
      break;
    }
    Budget budget;
    rejected([&] { validateGenericSignatures(classes, budget); },
             "unbound type variable");
  }
}

namespace {
ClassMap arrayIntersectionFixture() {
  Class callback;
  callback.name = "Lfixture/Fallback;";
  callback.superclass = "Ljava/lang/Object;";
  callback.access = {"public", "interface", "abstract"};
  callback.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  auto get = genericMethod(callback, "()TE;", {}, "Ljava/lang/Object;");
  get.reference.name = "get";
  callback.methods = {get};
  Class host;
  host.name = "Lfixture/ArrayChoice;";
  host.superclass = "Ljava/lang/Object;";
  host.access = {"public", "final"};
  auto choose = genericMethod(host,
                              "<C:[Ljava/lang/Object;:TR;R:Ljava/lang/Object;>"
                              "(TC;Lfixture/Fallback<+TR;>;)TR;",
                              {"[Ljava/lang/Object;", callback.name},
                              "Ljava/lang/Object;");
  choose.reference.name = "choose";
  choose.access = {"private", "static"};
  choose.registers = 2;
  Instruction op;
  op.opcode = "return-object";
  op.registers = {0};
  choose.instructions = {op};
  host.methods = {choose};
  return {{host.name, host}, {callback.name, callback}};
}
} // namespace

TEST(MobileDalvikSignature, PrivateArrayIntersectionRetainsRuntimeDescriptor) {
  const auto classes = arrayIntersectionFixture();
  const auto &method = classes.at("Lfixture/ArrayChoice;").methods[0];
  Budget budget;
  auto plans = validateGenericSignatures(classes, budget);
  const auto &plan = plans.methods.at(method.reference);
  EXPECT_TRUE(plan.type_parameters.empty());
  EXPECT_EQ(plan.types[plan.parameters[0]].erasure, "[Ljava/lang/Object;");
  EXPECT_TRUE(plan.types[plan.parameters[1]].segments[0].arguments.empty());
  EXPECT_EQ(plan.types[*plan.result].erasure, "Ljava/lang/Object;");
  ASSERT_EQ(plan.source_projections.size(), 1u);
  EXPECT_EQ(plan.source_projections[0].kind,
            "array-intersection-method-erasure");
  EXPECT_EQ(plan.source_projections[0].original_type,
            *method.generic_signature);
  EXPECT_EQ(plan.source_projections[0].source_type,
            method.reference.signature());
  EXPECT_EQ(plan.source_projections[0].scope_origin,
            method.reference.identity());
}

TEST(MobileDalvikSignature, ArrayIntersectionCannotEraseOtherBoundsOrScopes) {
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    SCOPED_TRACE(mutation);
    auto classes = arrayIntersectionFixture();
    auto &method = classes.at("Lfixture/ArrayChoice;").methods[0];
    switch (mutation) {
    case 0:
      method.access = {"public", "static"};
      break;
    case 1:
      method.generic_signature =
          "<C:[Ljava/lang/Object;:TR;:Ljava/io/Serializable;R:Ljava/lang/"
          "Object;>(TC;Lfixture/Fallback<+TR;>;)TR;";
      break;
    case 2:
      method.generic_signature = "<C:[Ljava/lang/Object;:TR;R:Ljava/lang/"
                                 "Number;>(TC;Lfixture/Fallback<+TR;>;)TR;";
      break;
    case 3:
      method.generic_signature = "<C:[Ljava/lang/String;:TR;R:Ljava/lang/"
                                 "Object;>(TC;Lfixture/Fallback<+TR;>;)TR;";
      break;
    case 4:
      method.generic_signature = "<C:[Ljava/lang/Object;:TR;R:Ljava/lang/"
                                 "Object;>(TC;Lfixture/Fallback<TR;>;)TR;";
      break;
    case 5:
      classes.at("Lfixture/Fallback;").generic_signature =
          "<E:Ljava/lang/Number;>Ljava/lang/Object;";
      break;
    case 6: {
      Class local;
      local.name = "Lfixture/ArrayChoice$1;";
      local.superclass = "Ljava/lang/Object;";
      local.inner_class_present = true;
      local.enclosing_method = method.reference;
      classes.emplace(local.name, std::move(local));
      break;
    }
    }
    Budget budget;
    rejected([&] { validateGenericSignatures(classes, budget); },
             "bound cannot be expressed as a Java type bound");
  }
}

TEST(MobileDalvikSignature, ArrayIntersectionStillValidatesOriginalErasure) {
  auto classes = arrayIntersectionFixture();
  auto other = classes.at("Lfixture/Fallback;");
  other.name = "Lfixture/Alternate;";
  for (auto &method : other.methods)
    method.reference.owner = other.name;
  classes.at("Lfixture/ArrayChoice;").methods[0].reference.parameters[1] =
      other.name;
  classes.emplace(other.name, std::move(other));
  Budget budget;
  rejected([&] { validateGenericSignatures(classes, budget); },
           "parameter erasure mismatch");
}

namespace {
ClassMap interfaceIntersectionFixture() {
  auto classes = arrayIntersectionFixture();
  Class container;
  container.name = "Lfixture/Container;";
  container.superclass = "Ljava/lang/Object;";
  container.access = {"public", "interface", "abstract"};
  container.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  classes.emplace(container.name, container);
  auto &method = classes.at("Lfixture/ArrayChoice;").methods[0];
  method.reference.parameters[0] = container.name;
  method.generic_signature =
      "<C::Lfixture/Container<*>;:TR;R:Ljava/lang/Object;>"
      "(TC;Lfixture/Fallback<+TR;>;)TR;";
  return classes;
}
} // namespace

TEST(MobileDalvikSignature,
     PrivateInterfaceIntersectionRetainsRuntimeDescriptor) {
  for (bool generic : {true, false}) {
    auto classes = interfaceIntersectionFixture();
    auto &method = classes.at("Lfixture/ArrayChoice;").methods[0];
    if (!generic) {
      classes.at("Lfixture/Container;").generic_signature.reset();
      method.generic_signature =
          "<C::Lfixture/Container;:TR;R:Ljava/lang/Object;>"
          "(TC;Lfixture/Fallback<+TR;>;)TR;";
    }
    Budget budget;
    const auto plans = validateGenericSignatures(classes, budget);
    const auto &plan = plans.methods.at(method.reference);
    EXPECT_TRUE(plan.type_parameters.empty());
    EXPECT_EQ(plan.types[plan.parameters[0]].erasure, "Lfixture/Container;");
    EXPECT_TRUE(plan.types[plan.parameters[0]].segments[0].arguments.empty());
    ASSERT_EQ(plan.source_projections.size(), 1u);
    EXPECT_EQ(plan.source_projections[0].kind,
              "interface-intersection-method-erasure");
    EXPECT_EQ(plan.source_projections[0].original_type,
              *method.generic_signature);
    EXPECT_EQ(plan.source_projections[0].source_type,
              method.reference.signature());
  }
}

TEST(MobileDalvikSignature, InterfaceIntersectionRequiresPrivateProvenBounds) {
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    SCOPED_TRACE(mutation);
    auto classes = interfaceIntersectionFixture();
    auto &method = classes.at("Lfixture/ArrayChoice;").methods[0];
    switch (mutation) {
    case 0:
      method.access = {"public", "static"};
      break;
    case 1:
      method.access = {"private"};
      break;
    case 2:
      classes.at("Lfixture/Container;").generic_signature =
          "<E:Ljava/lang/Number;>Ljava/lang/Object;";
      break;
    case 3:
      classes.at("Lfixture/Container;").access = {"public"};
      break;
    case 4:
      classes.erase("Lfixture/Container;");
      break;
    case 5:
      method.generic_signature =
          "<C::Lfixture/Container<TR;>;:TR;R:Ljava/lang/Object;>"
          "(TC;Lfixture/Fallback<+TR;>;)TR;";
      break;
    case 6:
      method.generic_signature =
          "<C::Lfixture/Container<*>;:TR;R:Ljava/lang/Number;>"
          "(TC;Lfixture/Fallback<+TR;>;)TR;";
      break;
    case 7:
      method.declared_throws =
          std::vector<std::string>{"Ljava/lang/Exception;"};
      break;
    case 8: {
      Class local;
      local.name = "Lfixture/ArrayChoice$1;";
      local.superclass = "Ljava/lang/Object;";
      local.inner_class_present = true;
      local.enclosing_method = method.reference;
      classes.emplace(local.name, std::move(local));
      break;
    }
    }
    Budget budget;
    rejected([&] { validateGenericSignatures(classes, budget); },
             mutation == 3 ? "interface bound names a known class"
             : mutation == 4
                 ? "formal-bound declaration kind is not proven"
                 : "bound cannot be expressed as a Java type bound");
  }
}

TEST(MobileDalvikSignature, InterfaceIntersectionValidatesOriginalErasure) {
  auto classes = interfaceIntersectionFixture();
  classes.at("Lfixture/ArrayChoice;").methods[0].reference.parameters[0] =
      "Ljava/lang/Object;";
  Budget budget;
  rejected([&] { validateGenericSignatures(classes, budget); },
           "parameter erasure mismatch");
}

namespace {
ClassMap checkedSyntheticBoundFixture() {
  Class bounded;
  bounded.name = "Lfixture/Bounded;";
  bounded.superclass = "Ljava/lang/Object;";
  bounded.access = {"public", "abstract", "interface"};
  bounded.generic_signature =
      "<E::Ljava/lang/Comparable<-TE;>;>Ljava/lang/Object;";
  auto accept =
      genericMethod(bounded, "(TE;)Z", {"Ljava/lang/Comparable;"}, "Z");
  accept.reference.name = "accept";
  bounded.methods = {accept};
  Class iterable;
  iterable.name = "Ljava/lang/Iterable;";
  iterable.superclass = "Ljava/lang/Object;";
  iterable.access = {"public", "abstract", "interface"};
  iterable.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  Class host;
  host.name = "Lfixture/CheckedBound;";
  host.superclass = "Ljava/lang/Object;";
  host.access = {"public", "final"};
  auto method = genericMethod(host,
                              "<T:Ljava/lang/Object;R::Lfixture/"
                              "Bounded<TT;>;:Ljava/lang/Iterable<+TT;>;>"
                              "(TR;TT;)Z",
                              {bounded.name, "Ljava/lang/Object;"}, "Z");
  method.reference.name = "probe";
  method.access = {"private", "static", "synthetic"};
  method.registers = 3;
  auto op = [&](uint32_t pc, std::string name,
                std::vector<unsigned> registers) {
    Instruction instruction;
    instruction.pc = pc;
    instruction.opcode = std::move(name);
    instruction.registers = std::move(registers);
    return instruction;
  };
  auto guard = op(0, "if-eqz", {2});
  guard.target = 12;
  auto copy = op(2, "move-object", {0, 2});
  auto cast = op(3, "check-cast", {0});
  cast.reference = std::string("Ljava/lang/Comparable;");
  auto call = op(5, "invoke-interface", {1, 0});
  call.reference = accept.reference;
  auto zero = op(12, "const/4", {0});
  zero.literal = int64_t(0);
  method.instructions = {guard,
                         copy,
                         cast,
                         call,
                         op(8, "move-result", {0}),
                         op(9, "return", {0}),
                         zero,
                         op(13, "return", {0})};
  method.code_end = 14;
  host.methods = {method};
  return {
      {host.name, host}, {bounded.name, bounded}, {iterable.name, iterable}};
}
} // namespace

TEST(MobileDalvikSignature, CheckedSyntheticBoundRetainsRuntimeDescriptor) {
  for (bool copied : {false, true}) {
    auto classes = checkedSyntheticBoundFixture();
    auto &method = classes.at("Lfixture/CheckedBound;").methods[0];
    if (!copied) {
      method.instructions[2].registers = {2};
      method.instructions[3].registers = {1, 2};
      method.instructions.erase(method.instructions.begin() + 1);
    }
    Budget budget;
    const auto plans = validateGenericSignatures(classes, budget);
    const auto &plan = plans.methods.at(method.reference);
    EXPECT_TRUE(plan.type_parameters.empty());
    EXPECT_EQ(plan.types[plan.parameters[0]].erasure, "Lfixture/Bounded;");
    EXPECT_EQ(plan.types[plan.parameters[1]].erasure, "Ljava/lang/Object;");
    EXPECT_EQ(plan.types[*plan.result].erasure, "Z");
    ASSERT_EQ(plan.source_projections.size(), 1u);
    EXPECT_EQ(plan.source_projections[0].kind,
              "checked-synthetic-bound-erasure");
    EXPECT_EQ(plan.source_projections[0].original_type,
              *method.generic_signature);
    EXPECT_EQ(plan.source_projections[0].scope_origin,
              method.reference.identity());
  }
}

TEST(MobileDalvikSignature, CheckedSyntheticBoundRequiresOriginalCheckedCall) {
  for (unsigned mutation = 0; mutation < 11; ++mutation) {
    SCOPED_TRACE(mutation);
    auto classes = checkedSyntheticBoundFixture();
    auto &method = classes.at("Lfixture/CheckedBound;").methods[0];
    switch (mutation) {
    case 0:
      method.access.erase("synthetic");
      break;
    case 1:
      method.access.erase("private");
      method.access.insert("public");
      break;
    case 2:
      method.instructions[2].reference = std::string("Ljava/lang/String;");
      break;
    case 3:
      method.instructions[1].registers = {0, 1};
      break;
    case 4:
      method.instructions[0].target = 3;
      break;
    case 5:
      method.instructions[0].target = 5;
      break;
    case 6:
      method.instructions[6].registers = {2};
      break;
    case 7:
      method.instructions[6].opcode = "goto";
      method.instructions[6].target = 0;
      break;
    case 8:
      method.instructions[3].registers = {1, 2};
      break;
    case 9:
      classes.at("Lfixture/Bounded;").methods.clear();
      break;
    case 10: {
      Class local;
      local.name = "Lfixture/CheckedBound$1;";
      local.superclass = "Ljava/lang/Object;";
      local.enclosing_method = method.reference;
      local.inner_class_present = true;
      classes.emplace(local.name, std::move(local));
      break;
    }
    }
    Budget budget;
    rejected([&] { validateGenericSignatures(classes, budget); },
             "type argument bound is not proven");
  }
}

TEST(MobileDalvikSignature,
     CheckedSyntheticBoundStillValidatesOriginalErasure) {
  auto classes = checkedSyntheticBoundFixture();
  classes.at("Lfixture/CheckedBound;").methods[0].reference.parameters[0] =
      "Ljava/lang/Object;";
  Budget budget;
  rejected([&] { validateGenericSignatures(classes, budget); },
           "parameter erasure mismatch");
}

namespace {
ClassMap instanceDefaultScopeFixture() {
  Class host;
  host.name = "Lfixture/Receiver;";
  host.superclass = "Ljava/lang/Object;";
  host.access = {"public"};
  host.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  Method read;
  read.reference = {host.name, "read", {}, "Ljava/lang/Object;"};
  read.access = {"public"};
  auto original =
      genericMethod(host, "<R:Ljava/lang/Object;>(I)Ljava/lang/Object;", {"I"},
                    "Ljava/lang/Object;");
  original.reference.name = "make";
  original.access = {"private"};
  original.registers = 3;
  Instruction call;
  call.opcode = "invoke-virtual";
  call.registers = {1};
  call.reference = read.reference;
  Instruction result;
  result.pc = 3;
  result.opcode = "move-result-object";
  result.registers = {0};
  Instruction done;
  done.pc = 4;
  done.opcode = "return-object";
  done.registers = {0};
  original.instructions = {call, result, done};
  original.code_end = 5;
  auto wrapper = original;
  wrapper.reference.name += "$default";
  wrapper.reference.parameters = {host.name, "I", "I", "Ljava/lang/Object;"};
  wrapper.generic_signature.reset();
  wrapper.access = {"static", "synthetic"};
  wrapper.registers = 5;
  host.methods = {read, original, wrapper};
  Class local;
  local.name = "Lfixture/Receiver$1;";
  local.superclass = "Ljava/lang/Object;";
  local.access = {"final"};
  local.inner_class_present = true;
  local.enclosing_method = wrapper.reference;
  auto identity = genericMethod(local, "(TE;)TE;", {"Ljava/lang/Object;"},
                                "Ljava/lang/Object;");
  identity.reference.name = "identity";
  identity.access = {"public"};
  identity.registers = 2;
  done.pc = 0;
  done.registers = {1};
  identity.instructions = {done};
  identity.code_end = 1;
  local.methods = {identity};
  return {{host.name, host}, {local.name, local}};
}
} // namespace

TEST(MobileDalvikSignature,
     InstanceDefaultScopesUseTheUnchangedDeclaredReceiver) {
  auto classes = instanceDefaultScopeFixture();
  const auto &method = classes.at("Lfixture/Receiver$1;").methods[0];
  Budget budget;
  const auto plans = validateGenericSignatures(classes, budget);
  const auto &plan = plans.methods.at(method.reference);
  ASSERT_EQ(plan.source_projections.size(), 2u);
  for (const auto &projection : plan.source_projections) {
    EXPECT_EQ(projection.kind, "synthetic-default-receiver-type-variable");
    EXPECT_EQ(projection.scope_origin, "Lfixture/Receiver;");
    EXPECT_EQ(projection.original_type,
              classes.at("Lfixture/Receiver;").methods[2].reference.identity() +
                  "::E");
    EXPECT_EQ(projection.source_type, "Ljava/lang/Object;");
  }
  classes.at("Lfixture/Receiver$1;").methods[0].generic_signature =
      "<E:Ljava/lang/Object;>(TE;)TE;";
  EXPECT_TRUE(validateGenericSignatures(classes, budget)
                  .methods.at(method.reference)
                  .source_projections.empty());
}

namespace {
ClassMap staticDefaultConstructorScopeFixture() {
  Class token;
  token.name = "Lfixture/Token;";
  token.superclass = "Ljava/lang/Object;";
  token.access = {"public", "final"};
  token.generic_signature = "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
  Class holder = token;
  holder.name = "Lfixture/Holder;";
  holder.generic_signature = "<Payload:Ljava/lang/Object;>Ljava/lang/Object;";
  auto constructor =
      genericMethod(holder, "(Lfixture/Token<TPayload;>;Ljava/lang/Object;)V",
                    {token.name, "Ljava/lang/Object;"}, "V");
  constructor.reference.name = "<init>";
  constructor.access = {"public", "constructor"};
  holder.methods = {constructor};
  Class host;
  host.name = "Lfixture/Scopes;";
  host.superclass = "Ljava/lang/Object;";
  host.access = {"public", "final"};
  auto original = genericMethod(
      host,
      "<E:Ljava/lang/Object;>(JLfixture/Token<TE;>;Ljava/lang/Object;)"
      "Ljava/lang/Object;",
      {"J", token.name, "Ljava/lang/Object;"}, "Ljava/lang/Object;");
  original.reference.name = "make";
  original.access = {"public", "static"};
  original.registers = original.incomingWords() + 2;
  Instruction allocation;
  allocation.opcode = "new-instance";
  allocation.registers = {0};
  allocation.reference = holder.name;
  Instruction call;
  call.pc = 2;
  call.opcode = "invoke-direct";
  call.registers = {0, 4, 5};
  call.reference = constructor.reference;
  Instruction done;
  done.pc = 5;
  done.opcode = "return-object";
  done.registers = {0};
  original.instructions = {allocation, call, done};
  original.code_end = 6;
  auto wrapper = original;
  wrapper.reference.name += "$default";
  wrapper.reference.parameters.insert(wrapper.reference.parameters.end(),
                                      {"I", "Ljava/lang/Object;"});
  wrapper.generic_signature.reset();
  wrapper.access = {"public", "static", "synthetic"};
  wrapper.registers = wrapper.incomingWords() + 2;
  host.methods = {original, wrapper};
  Class local;
  local.name = "Lfixture/Scopes$1;";
  local.superclass = "Ljava/lang/Object;";
  local.access = {"final"};
  local.inner_class_present = true;
  local.enclosing_method = wrapper.reference;
  auto identity = genericMethod(local, "(TE;)TE;", {"Ljava/lang/Object;"},
                                "Ljava/lang/Object;");
  identity.reference.name = "identity";
  identity.access = {"public"};
  identity.registers = 2;
  done.pc = 0;
  done.registers = {1};
  identity.instructions = {done};
  identity.code_end = 1;
  local.methods = {identity};
  return {{token.name, token},
          {holder.name, holder},
          {host.name, host},
          {local.name, local}};
}
} // namespace

TEST(MobileDalvikSignature, StaticDefaultScopesUseExactGenericConstruction) {
  auto classes = staticDefaultConstructorScopeFixture();
  Budget budget;
  const auto plans = validateGenericSignatures(classes, budget);
  const auto &host = classes.at("Lfixture/Scopes;");
  const auto &ref = classes.at("Lfixture/Scopes$1;").methods[0].reference;
  const auto &plan = plans.methods.at(ref);
  ASSERT_EQ(plan.source_projections.size(), 2u);
  for (const auto &projection : plan.source_projections) {
    EXPECT_EQ(projection.kind, "synthetic-default-method-type-variable");
    EXPECT_EQ(projection.scope_origin, host.methods[0].reference.identity());
    EXPECT_EQ(projection.original_type,
              host.methods[1].reference.identity() + "::E");
    EXPECT_EQ(projection.source_type, "Ljava/lang/Object;");
  }
}

TEST(MobileDalvikSignature, StaticDefaultConstructorInputsFollowEntryCopies) {
  for (const auto &opcode : {"invoke-direct", "invoke-direct/range"}) {
    auto classes = staticDefaultConstructorScopeFixture();
    for (auto &method : classes.at("Lfixture/Scopes;").methods) {
      method.registers = method.incomingWords() + 16;
      Instruction copy;
      copy.opcode = "move-object/from16";
      copy.registers = {1, 18};
      for (auto &op : method.instructions)
        op.pc += 2;
      method.instructions[1].opcode = opcode;
      method.instructions[1].registers = {0, 1, 19};
      method.instructions.insert(method.instructions.begin(), copy);
      method.code_end += 2;
    }
    Budget budget;
    const auto plans = validateGenericSignatures(classes, budget);
    EXPECT_EQ(
        plans.methods.at(classes.at("Lfixture/Scopes$1;").methods[0].reference)
            .source_projections.size(),
        2u);
  }
}

TEST(MobileDalvikSignature,
     StaticDefaultConstructorScopeRejectsGuessedOrigins) {
  for (unsigned mutation = 0; mutation < 21; ++mutation) {
    SCOPED_TRACE(mutation);
    auto classes = staticDefaultConstructorScopeFixture();
    auto &host = classes.at("Lfixture/Scopes;");
    auto &holder = classes.at("Lfixture/Holder;");
    auto &original = host.methods[0];
    auto &wrapper = host.methods[1];
    Instruction write;
    write.pc = 6;
    write.opcode = "const/4";
    write.registers = {4};
    write.literal = 0;
    switch (mutation) {
    case 0:
      wrapper.access.erase("synthetic");
      break;
    case 1:
      original.generic_signature =
          "<E:Ljava/lang/Number;>(JLfixture/Token<TE;>;Ljava/lang/Object;)"
          "Ljava/lang/Object;";
      break;
    case 2:
      holder.generic_signature =
          "<Payload:Ljava/lang/Number;>Ljava/lang/Object;";
      break;
    case 3:
      wrapper.instructions[1].registers[1] = 5;
      break;
    case 4:
      std::get<MethodRef>(wrapper.instructions[1].reference).name = "missing";
      break;
    case 5:
      holder.methods[0].generic_signature =
          "(Lfixture/Token<*>;Ljava/lang/Object;)V";
      break;
    case 6:
      wrapper.instructions.push_back(write);
      break;
    case 7:
      write.opcode = "const-wide/16";
      write.registers = {3};
      wrapper.instructions.push_back(write);
      break;
    case 8:
      write.opcode = "move-object";
      write.registers = {4, 5};
      wrapper.instructions.push_back(write);
      break;
    case 9:
      wrapper.instructions[0].opcode = "nop";
      break;
    case 10:
      write.opcode = "if-eqz";
      write.target = 2;
      wrapper.instructions.push_back(write);
      break;
    case 11:
      original.generic_signature =
          "<E:Ljava/lang/Object;>(JLfixture/Token<*>;Ljava/lang/Object;)"
          "Ljava/lang/Object;";
      break;
    case 12:
      original.access.erase("static");
      break;
    case 13:
      holder.methods[0].generic_signature =
          "<Payload:Ljava/lang/Object;>(Lfixture/Token<TPayload;>;"
          "Ljava/lang/Object;)V";
      break;
    case 14:
      holder.methods[0].access.erase("public");
      break;
    case 15:
      holder.methods[0].generic_signature =
          "(Lfixture/Token<+TPayload;>;Ljava/lang/Object;)V";
      break;
    case 16:
      classes.at("Lfixture/Scopes$1;").methods[0].generic_signature =
          "(TUnknown;)TUnknown;";
      break;
    case 17:
      write.pc = 0;
      write.opcode = "move-object";
      write.registers = {1, 4};
      wrapper.instructions.insert(wrapper.instructions.begin() + 1, write);
      wrapper.instructions[2].registers[1] = 1;
      break;
    case 18:
      wrapper.instructions[0].reference = std::string("Lfixture/Token;");
      break;
    case 19:
      original.instructions[1].opcode = "nop";
      break;
    case 20:
      original.generic_signature =
          "<E:Ljava/lang/Object;Other:Ljava/lang/Object;>"
          "(JLfixture/Token<TE;>;Ljava/lang/Object;)Ljava/lang/Object;";
      break;
    }
    classes.at("Lfixture/Scopes$1;").enclosing_method = wrapper.reference;
    Budget budget;
    rejected([&] { validateGenericSignatures(classes, budget); },
             "unbound type variable");
  }
}

TEST(MobileDalvikSignature,
     InstanceDefaultScopeCannotGuessAnUnrelatedReceiver) {
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    SCOPED_TRACE(mutation);
    auto classes = instanceDefaultScopeFixture();
    auto &host = classes.at("Lfixture/Receiver;");
    auto &local = classes.at("Lfixture/Receiver$1;");
    auto &original = host.methods[1];
    auto &wrapper = host.methods[2];
    switch (mutation) {
    case 0:
      wrapper.access.erase("synthetic");
      break;
    case 1:
      wrapper.reference.parameters[0] = "Ljava/lang/Object;";
      break;
    case 2:
      wrapper.instructions[0].registers = {2};
      break;
    case 3:
      original.instructions[0].registers = {2};
      break;
    case 4:
      wrapper.instructions[1].registers = {1};
      break;
    case 5:
      host.generic_signature = "<E:Ljava/lang/Number;>Ljava/lang/Object;";
      break;
    case 6:
      original.generic_signature =
          "<R:Ljava/lang/Number;>(I)Ljava/lang/Object;";
      break;
    case 7:
      std::get<MethodRef>(wrapper.instructions[0].reference).name = "unknown";
      break;
    case 8:
      local.methods[0].generic_signature = "(TUnknown;)TUnknown;";
      break;
    }
    local.enclosing_method = wrapper.reference;
    Budget budget;
    rejected([&] { validateGenericSignatures(classes, budget); },
             "unbound type variable");
  }
}

namespace {
ClassMap copiedInstanceDefaultScopeFixture() {
  auto classes = instanceDefaultScopeFixture();
  auto &host = classes.at("Lfixture/Receiver;");
  for (size_t index : {1u, 2u}) {
    auto &method = host.methods[index];
    method.registers = method.incomingWords() + 16;
    auto call = method.instructions[0];
    call.pc = 3;
    call.registers = {1};
    auto result = method.instructions[1];
    result.pc = 6;
    result.registers = {2};
    auto done = method.instructions[2];
    done.pc = 7;
    done.registers = {2};
    Instruction copy;
    copy.opcode = "move-object/from16";
    copy.registers = {0, 16};
    Instruction alias;
    alias.pc = 2;
    alias.opcode = "move-object";
    alias.registers = {1, 0};
    method.instructions = {copy, alias, call, result, done};
    method.code_end = 8;
  }
  return classes;
}
} // namespace

TEST(MobileDalvikSignature, InstanceDefaultScopesFollowStableEntryCopies) {
  for (const auto &opcode : {"invoke-virtual", "invoke-virtual/range",
                             "invoke-direct", "invoke-direct/range"}) {
    SCOPED_TRACE(opcode);
    auto classes = copiedInstanceDefaultScopeFixture();
    classes.at("Lfixture/Receiver;").methods[2].instructions[2].opcode = opcode;
    Budget budget;
    const auto plans = validateGenericSignatures(classes, budget);
    const auto &method = classes.at("Lfixture/Receiver$1;").methods[0];
    const auto &plan = plans.methods.at(method.reference);
    ASSERT_EQ(plan.source_projections.size(), 2u);
    for (const auto &projection : plan.source_projections) {
      EXPECT_EQ(projection.kind, "synthetic-default-receiver-type-variable");
      EXPECT_EQ(projection.scope_origin, "Lfixture/Receiver;");
      EXPECT_EQ(projection.source_type, "Ljava/lang/Object;");
    }
  }
}

TEST(MobileDalvikSignature, InstanceDefaultScopesRejectOverwrittenEntryCopies) {
  for (unsigned mutation = 0; mutation < 12; ++mutation) {
    SCOPED_TRACE(mutation);
    auto classes = copiedInstanceDefaultScopeFixture();
    auto &host = classes.at("Lfixture/Receiver;");
    auto &wrapper = host.methods[2];
    Instruction write;
    write.pc = 8;
    write.opcode = "const/4";
    write.registers = {0};
    write.literal = 0;
    switch (mutation) {
    case 0:
      wrapper.instructions.erase(wrapper.instructions.begin());
      break;
    case 1:
      wrapper.instructions[0].registers = {0, 17};
      break;
    case 2:
      wrapper.instructions.push_back(write);
      break;
    case 3:
      write.registers = {1};
      wrapper.instructions.push_back(write);
      break;
    case 4:
      write.opcode = "const-wide/16";
      wrapper.instructions.push_back(write);
      break;
    case 5:
      write.opcode = "move-object/from16";
      write.registers = {0, 17};
      wrapper.instructions.push_back(write);
      break;
    case 6:
      wrapper.instructions[1].opcode = "move";
      break;
    case 7:
      write.pc = 0;
      write.opcode = "if-eqz";
      write.registers = {17};
      write.target = 3;
      wrapper.instructions.insert(wrapper.instructions.begin(), write);
      break;
    case 8:
      wrapper.instructions[0].registers = {0, 1};
      break;
    case 9:
      write.registers = {16};
      wrapper.instructions.push_back(write);
      break;
    case 10:
      host.methods[1].instructions[0].registers = {0, 17};
      break;
    case 11:
      host.methods[0].access.insert("static");
      break;
    }
    Budget budget;
    rejected([&] { validateGenericSignatures(classes, budget); },
             "unbound type variable");
  }
}

namespace {
ClassMap externalInterfaceContracts() {
  auto Root = genericClass();
  Root.name = "Lfixture/Root;";
  Root.external = true;
  Root.access = {"public", "abstract", "interface"};
  Root.methods = {genericMethod(Root, "(TT;)TT;", {"Ljava/lang/Object;"},
                                "Ljava/lang/Object;")};
  auto Middle = genericClass(
      "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;"
      "Lfixture/Root<TT;>;");
  Middle.name = "Lfixture/Middle;";
  Middle.external = true;
  Middle.access = Root.access;
  Middle.interfaces = {Root.name};
  Middle.methods = {genericMethod(
      Middle, "(TT;I)TT;", {"Ljava/lang/Object;", "I"}, "Ljava/lang/Object;")};
  auto Child = genericClass("<T:Lfixture/Tag;U:Lfixture/Tag;>Ljava/lang/Object;"
                            "Lfixture/Middle<TT;TU;>;");
  Child.interfaces = {Middle.name};
  Class Tag;
  Tag.name = "Lfixture/Tag;";
  Tag.superclass = "Ljava/lang/Object;";
  Tag.access = {"public"};
  auto Unused = genericClass("<Q:Lexternal/Unavailable;>Ljava/lang/Object;");
  Unused.name = "Lfixture/Unused;";
  Unused.external = true;
  Unused.methods = {
      genericMethod(Unused, "()TMissing;", {}, "Ljava/lang/Object;")};
  return {{Root.name, Root},
          {Middle.name, Middle},
          {Child.name, Child},
          {Tag.name, Tag},
          {Unused.name, Unused}};
}
} // namespace

TEST(MobileDalvikSignature, ExternalInterfaceContractsBindOnlyConsumedScopes) {
  auto Input = externalInterfaceContracts();
  Budget B;
  const auto Plans = validateGenericSignatures(Input, B);
  const auto &Middle = Plans.classes.at("Lfixture/Middle;");
  ASSERT_EQ(Middle.interfaces.size(), 1u);
  const auto &Edge = Middle.types.at(Middle.interfaces[0]);
  EXPECT_EQ(Edge.erasure, "Lfixture/Root;");
  ASSERT_TRUE(Edge.segments[0].arguments[0].type);
  const auto &Argument = Middle.types.at(*Edge.segments[0].arguments[0].type);
  EXPECT_EQ(Argument.variable_owner, "Lfixture/Middle;");
  EXPECT_EQ(Argument.erasure, "Ljava/lang/Object;");
  for (const auto &Name : {"Lfixture/Root;", "Lfixture/Middle;"}) {
    const auto &Method = Input.at(Name).methods[0];
    const auto &Signature = Plans.methods.at(Method.reference);
    EXPECT_EQ(Signature.types[Signature.parameters[0]].variable_owner, Name);
    EXPECT_EQ(Signature.types[*Signature.result].variable_owner, Name);
  }
  const auto &Unused = Plans.classes.at("Lfixture/Unused;");
  EXPECT_TRUE(Unused.type_parameters[0].erasure.empty());
  const auto &UnusedMethod =
      Plans.methods.at(Input.at("Lfixture/Unused;").methods[0].reference);
  EXPECT_TRUE(UnusedMethod.types[*UnusedMethod.result].erasure.empty());
}

TEST(MobileDalvikSignature,
     ExternalInterfaceContractsRejectScopeAndDescriptorDisagreement) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    auto Input = externalInterfaceContracts();
    auto &Root = Input.at("Lfixture/Root;");
    auto &Middle = Input.at("Lfixture/Middle;");
    std::string Expected;
    switch (Mutation) {
    case 0:
      Root.methods[0].generic_signature = "(TU;)TT;";
      Expected = "unbound type variable U";
      break;
    case 1:
      Root.methods[0].reference.returns = "Lfixture/Tag;";
      Expected = "dependency return erasure mismatch";
      break;
    case 2:
      Root.methods[0].reference.parameters = {"I"};
      Expected = "dependency parameter erasure mismatch";
      break;
    case 3:
      Middle.generic_signature =
          "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;"
          "Ljava/io/Serializable;";
      Expected = "dependency interface erasure mismatch";
      break;
    case 4:
      Root.methods[0].generic_signature = "(TT;)TT;^Ljava/lang/Exception;";
      Expected = "dependency generic throws erasure disagrees";
      break;
    case 5:
      Root.interfaces = {Middle.name};
      Root.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;"
                               "Lfixture/Middle<TT;TT;>;";
      Expected = "cyclic inherited interface dependency";
      break;
    }
    Budget B;
    rejected([&] { validateGenericSignatures(Input, B); }, Expected);
  }
}

namespace {
ClassMap externalSuperclassContracts() {
  auto Input = externalInterfaceContracts();
  auto &Root = Input.at("Lfixture/Root;");
  auto &Middle = Input.at("Lfixture/Middle;");
  auto &Child = Input.at("Lfixture/Box;");
  Root.access.erase("interface");
  Middle.access.erase("interface");
  Middle.interfaces.clear();
  Middle.superclass = Root.name;
  Middle.generic_signature =
      "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Lfixture/Root<TT;>;";
  Child.interfaces.clear();
  Child.superclass = Middle.name;
  Child.generic_signature =
      "<T:Lfixture/Tag;U:Lfixture/Tag;>Lfixture/Middle<TT;TU;>;";
  auto Contract = genericClass();
  Contract.name = "Lfixture/Contract;";
  Contract.external = true;
  Contract.access = {"public", "abstract", "interface"};
  Contract.methods = {genericMethod(
      Contract, "(TT;)TT;", {"Ljava/lang/Object;"}, "Ljava/lang/Object;")};
  Root.interfaces = {Contract.name};
  Root.generic_signature =
      "<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Contract<TT;>;";
  Input.emplace(Contract.name, Contract);
  auto Private = genericMethod(Root, "()TUnused;", {}, "Ljava/lang/Object;");
  Private.reference.name = "unusedPrivate";
  Private.access = {"private"};
  Root.methods.push_back(Private);
  auto Constructor =
      genericMethod(Root, "(TT;)V", {"Ljava/lang/Object;", "Z"}, "V");
  Constructor.reference.name = "<init>";
  Constructor.access = {"protected", "constructor"};
  Root.methods.push_back(Constructor);
  return Input;
}
} // namespace

TEST(MobileDalvikSignature, ExternalSuperclassContractsBindTheirExactScopes) {
  auto Input = externalSuperclassContracts();
  Budget B;
  const auto Plans = validateGenericSignatures(Input, B);
  const auto &Middle = Plans.classes.at("Lfixture/Middle;");
  const auto &Edge = Middle.types.at(*Middle.superclass);
  EXPECT_EQ(Edge.erasure, "Lfixture/Root;");
  const auto &Argument = Middle.types.at(*Edge.segments[0].arguments[0].type);
  EXPECT_EQ(Argument.variable_owner, "Lfixture/Middle;");
  for (const auto &Name :
       {"Lfixture/Root;", "Lfixture/Middle;", "Lfixture/Contract;"}) {
    const auto &Signature =
        Plans.methods.at(Input.at(Name).methods[0].reference);
    EXPECT_EQ(Signature.types[Signature.parameters[0]].variable_owner, Name);
    EXPECT_EQ(Signature.types[*Signature.result].variable_owner, Name);
  }
  const auto &Root = Input.at("Lfixture/Root;");
  const auto &Unused = Plans.methods.at(Root.methods[1].reference);
  EXPECT_TRUE(Unused.types[*Unused.result].erasure.empty());
  EXPECT_EQ(Root.methods[2].reference.parameters.size(), 2u);
  EXPECT_TRUE(
      Plans.classes.at("Lfixture/Unused;").type_parameters[0].erasure.empty());
}

TEST(MobileDalvikSignature, ExternalSuperclassContractsRejectUnprovenMetadata) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    auto Input = externalSuperclassContracts();
    auto &Root = Input.at("Lfixture/Root;");
    auto &Middle = Input.at("Lfixture/Middle;");
    std::string Expected;
    switch (Mutation) {
    case 0:
      Root.methods[0].generic_signature = "(TU;)TT;";
      Expected = "unbound type variable U";
      break;
    case 1:
      Root.methods[0].reference.returns = "Lfixture/Tag;";
      Expected = "dependency return erasure mismatch";
      break;
    case 2:
      Root.methods[0].reference.parameters = {"I"};
      Expected = "dependency parameter erasure mismatch";
      break;
    case 3:
      Middle.generic_signature =
          "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;";
      Expected = "dependency superclass erasure mismatch";
      break;
    case 4:
      Root.methods[0].generic_signature = "(TT;)TT;^Ljava/lang/Exception;";
      Expected = "dependency generic throws erasure disagrees";
      break;
    case 5:
      Root.superclass = Middle.name;
      Root.generic_signature = "<T:Ljava/lang/Object;>Lfixture/"
                               "Middle<TT;TT;>;Lfixture/Contract<TT;>;";
      Expected = "cyclic inherited superclass dependency";
      break;
    }
    Budget B;
    rejected([&] { validateGenericSignatures(Input, B); }, Expected);
  }
}

namespace {
std::vector<Class> guardedClosedClasses(unsigned Dependencies, bool Synthetic,
                                        bool Wide, bool Range, bool Frame,
                                        bool Declared, bool Header) {
  auto C = [](std::string Name, std::vector<Method> Methods = {}) {
    Class Result;
    Result.name = std::move(Name);
    Result.superclass = "Ljava/lang/Object;";
    Result.access = {"public"};
    Result.source_id = "owned-guarded-closed";
    Result.methods = std::move(Methods);
    for (auto &M : Result.methods)
      M.reference.owner = Result.name;
    return Result;
  };
  auto I = [](uint32_t PC, std::string Name,
              std::vector<unsigned> Registers = {}, Literal Value = {},
              Reference Ref = {}, std::optional<uint32_t> Target = {}) {
    Instruction Op;
    Op.pc = PC;
    Op.opcode = std::move(Name);
    Op.registers = std::move(Registers);
    Op.literal = std::move(Value);
    Op.reference = std::move(Ref);
    Op.target = Target;
    return Op;
  };
  Method Base;
  Base.reference = {"Lfixture/Parent;", "<init>", {}, "V"};
  Base.access = {"public", "constructor"};
  Base.registers = 1;
  Base.instructions = {I(0, "invoke-direct", {0}, {},
                         MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                       I(1, "return-void")};
  Base.code_end = 2;
  auto Parent = C(Base.reference.owner, {Base});
  Parent.external = Dependencies & 1;
  if (Parent.external) {
    Parent.methods[0].instructions.clear();
    Parent.methods[0].registers = Parent.methods[0].code_end = 0;
  }
  auto List = C("Lfixture/List;"), Pair = C("Lfixture/Pair;");
  List.access = Pair.access = {"public", "abstract", "interface"};
  List.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  Pair.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  List.external = true;
  Pair.external = Dependencies & 2;
  auto Left = C("Lfixture/Left;"), Right = C("Lfixture/Right;");
  const std::string First = "Lfixture/List<+Lfixture/Left;>;";
  const std::string Second = "Lfixture/List<+Lfixture/Right;>;";
  Method FirstFactory;
  FirstFactory.reference = {"Lfixture/Provider;", "first", {}, List.name};
  FirstFactory.access = {"public", "static"};
  FirstFactory.generic_signature = "()" + First;
  FirstFactory.registers = 1;
  FirstFactory.instructions = {I(0, "const/4", {0}, int64_t(0)),
                               I(1, "return-object", {0})};
  FirstFactory.code_end = 2;
  auto SecondFactory = FirstFactory;
  SecondFactory.reference.name = "second";
  SecondFactory.generic_signature = "()" + Second;
  auto Provider =
      C(FirstFactory.reference.owner, {FirstFactory, SecondFactory});
  Provider.external = Dependencies & 2;
  if (Provider.external)
    for (auto &M : Provider.methods) {
      M.instructions.clear();
      M.registers = M.code_end = 0;
    }
  const std::string Scalar = Wide ? "J" : "F";
  const unsigned Words = Wide ? 2 : 1, Self = Frame ? 32 : 8;
  Method Target;
  Target.reference = {
      "Lfixture/Core;", "<init>", {List.name, List.name, Scalar}, "V"};
  Target.access = Base.access;
  Target.generic_signature = "(" + First + Second + Scalar + ")V";
  Target.registers = 3 + Words;
  Target.instructions = {I(0, "invoke-direct", {0}, {}, Base.reference),
                         I(1, "return-void")};
  Target.code_end = 2;
  auto Caller = Target;
  Caller.reference.parameters.insert(Caller.reference.parameters.end(),
                                     {"I", "Lfixture/Marker;"});
  Caller.generic_signature.reset();
  if (Declared)
    Caller.generic_signature =
        "(" + First + Second + Scalar + "ILfixture/Marker;)V";
  if (Synthetic)
    Caller.access.insert("synthetic");
  Caller.registers = Self + 5 + Words;
  std::vector<unsigned> Arguments{0, 1, 2, 3};
  if (Wide)
    Arguments.push_back(4);
  Caller.instructions = {
      I(0, "move-object/from16", {0, Self}),
      I(1, "move-object/from16", {1, Self + 1}),
      I(2, "move-object/from16", {2, Self + 2}),
      I(3, Wide ? "move-wide/from16" : "move/from16", {3, Self + 3}),
      I(4, "and-int/lit8", {6, Self + 3 + Words}, int64_t(1)),
      I(5, "if-eqz", {6}, {}, {}, 8),
      I(6, "invoke-static", {}, {}, FirstFactory.reference),
      I(7, "move-result-object", {1}),
      I(8, "and-int/lit8", {6, Self + 3 + Words}, int64_t(2)),
      I(9, "if-eqz", {6}, {}, {}, 12),
      I(10, "invoke-static", {}, {}, SecondFactory.reference),
      I(11, "move-result-object", {2}),
      I(12, Range ? "invoke-direct/range" : "invoke-direct", Arguments, {},
        Target.reference),
      I(13, "return-void")};
  Caller.code_end = 14;
  auto Core = C(Target.reference.owner, {Target, Caller});
  Core.access.insert("final");
  Core.superclass = Parent.name;
  if (Header) {
    Core.interfaces = {Pair.name};
    Core.generic_signature =
        "Lfixture/Parent;Lfixture/Pair<Lfixture/Left;Lfixture/Right;>;";
  }
  return {Core, Parent, Pair,     List,
          Left, Right,  Provider, C("Lfixture/Marker;")};
}

ClassMap guardedClosedMap(std::vector<Class> Input, bool Linked, Budget &B) {
  if (Linked) {
    std::reverse(Input.begin(), Input.end());
    return linkClasses(Input, B);
  }
  ClassMap Result;
  for (auto &C : Input) {
    const auto Name = C.name;
    Result.emplace(Name, std::move(C));
  }
  return Result;
}
} // namespace

TEST(MobileDalvikSignature, GuardedClosedDelegationsReuseExactControlFlow) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (bool Synthetic : {false, true})
      for (bool Wide : {false, true})
        for (bool Range : {false, true})
          for (bool Frame : {false, true})
            for (bool Declared : {false, true})
              for (bool Header : {false, true})
                for (bool Linked : {false, true})
                  for (bool Final : {false, true}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{Dependencies, Synthetic, Wide, Range, Frame,
                                   Declared, Header, Linked, Final}));
                    auto Input =
                        guardedClosedClasses(Dependencies, Synthetic, Wide,
                                             Range, Frame, Declared, Header);
                    if (!Final)
                      Input[0].access.erase("final");
                    const auto Caller = Input[0].methods[1].reference;
                    const auto Target = Input[0].methods[0].reference;
                    Budget B;
                    const auto Classes =
                        guardedClosedMap(std::move(Input), Linked, B);
                    const auto Plans = validateGenericSignatures(Classes, B);
                    ASSERT_EQ(Plans.constructor_delegations.at(Caller), Target);
                    ASSERT_TRUE(
                        Plans.constructor_calls.contains({Caller, Target}));
                    EXPECT_EQ(Plans.methods.contains(Caller), Declared);
                    EXPECT_EQ(Classes.at(Caller.owner)
                                  .methods[1]
                                  .generic_signature.has_value(),
                              Declared);
                    const auto &Contract =
                        Plans.constructor_calls.at({Caller, Target});
                    EXPECT_TRUE(Contract.type_parameters.empty());
                    EXPECT_TRUE(Contract.source_projections.empty());
                    ASSERT_EQ(Contract.parameters.size(), 3u);
                    for (size_t P = 0; P < 2; ++P) {
                      const auto &Type =
                          Contract.types.at(Contract.parameters[P]);
                      ASSERT_EQ(Type.segments.size(), 1u);
                      const auto &Args = Type.segments[0].arguments;
                      ASSERT_EQ(Args.size(), 1u);
                      EXPECT_EQ(Args[0].variance,
                                GenericTypeArgument::Variance::Extends);
                      ASSERT_TRUE(Args[0].type);
                      EXPECT_EQ(Contract.types.at(*Args[0].type).erasure,
                                P ? "Lfixture/Right;" : "Lfixture/Left;");
                    }
                    EXPECT_EQ(Contract.types.at(Contract.parameters[2]).erasure,
                              Wide ? "J" : "F");
                    const auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_EQ(R.getInteger("minimum_java_release"), 25);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    EXPECT_FALSE(R.getArray("class_source_bindings"));
                    bool Found = false;
                    for (const auto &Unit : *R.getArray("source_units"))
                      Found |=
                          Unit.getAsObject()->getString("source")->contains(
                              "this(((fixture.List<? extends "
                              "fixture.Left>)");
                    EXPECT_TRUE(Found);
                  }
}

TEST(MobileDalvikSignature,
     GuardedClosedDelegationsPrecedeExpandedVarargsSiblings) {
  for (unsigned Dependencies : {0u, 3u})
    for (bool Wide : {false, true})
      for (bool Linked : {false, true}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{Dependencies, Wide, Linked}));
        auto Input = guardedClosedClasses(Dependencies, false, Wide, true, true,
                                          false, true);
        const auto Caller = Input[0].methods[1].reference;
        const auto Target = Input[0].methods[0].reference;
        auto Other = Input[0].methods[0];
        Other.reference.parameters = {"[Ljava/lang/Object;"};
        Other.generic_signature.reset();
        Other.access.insert("varargs");
        Other.registers = 2;
        Input[0].methods.push_back(std::move(Other));
        Budget B;
        const auto Classes = guardedClosedMap(std::move(Input), Linked, B);
        const auto Plans = validateGenericSignatures(Classes, B);
        ASSERT_EQ(Plans.constructor_delegations.at(Caller), Target);
        ASSERT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
        const auto Report = recoverJava(Classes, B);
        EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
        EXPECT_EQ(Report.getInteger("minimum_java_release"), 25);
        EXPECT_FALSE(Report.getArray("generic_signature_projections"));
        EXPECT_FALSE(Report.getArray("class_source_bindings"));
      }
}

TEST(MobileDalvikSignature, GuardedClosedDelegationsRejectOpenAndBrokenInputs) {
  for (unsigned Dependencies : {0u, 3u})
    for (unsigned Mutation = 0; Mutation < 24; ++Mutation)
      for (bool Linked : {false, true}) {
        SCOPED_TRACE(::testing::PrintToString(
            std::tuple{Dependencies, Mutation, Linked}));
        auto Input = guardedClosedClasses(Dependencies, false, true, true, true,
                                          false, true);
        auto &Core = Input[0];
        auto &Target = Core.methods[0];
        auto &Caller = Core.methods[1];
        auto &Code = Caller.instructions;
        switch (Mutation) {
        case 0:
          Target.generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/List<TX;>;"
              "Lfixture/List<+Lfixture/Right;>;J)V";
          break;
        case 1:
          Caller.generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/List;Lfixture/List;"
              "JILfixture/Marker;)V";
          break;
        case 2:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 3:
          Target.access.insert("varargs");
          break;
        case 4:
          Caller.access.insert("varargs");
          break;
        case 5:
          Input.erase(Input.begin() + 4);
          break;
        case 6:
          Input.erase(Input.begin() + 3);
          break;
        case 7:
          Core.generic_signature =
              "<T:Ljava/lang/Object;>Lfixture/Parent;"
              "Lfixture/Pair<Lfixture/Left;Lfixture/Right;>;";
          break;
        case 8:
          Core.access.erase("final");
          Core.access.insert("abstract");
          break;
        case 9:
          Core.access.erase("public");
          break;
        case 10:
          Target.generic_signature = "(Lfixture/List<TUnknown;>;"
                                     "Lfixture/List<+Lfixture/Right;>;J)V";
          break;
        case 11:
          Target.generic_signature =
              "(Ljava/lang/Object;Lfixture/List<+Lfixture/Right;>;J)V";
          break;
        case 12:
          Code[12].registers[0] = 1;
          break;
        case 13:
          Code[12].registers[1] = 0;
          break;
        case 14:
          Code[12].registers[4] = 3;
          break;
        case 15:
          Code[3].registers[0] = 31;
          break;
        case 16:
          Code[5].target = 13;
          break;
        case 17:
          Code[5].target = 7;
          break;
        case 18:
          Code[4].opcode = "unsupported-prefix";
          break;
        case 19:
          Target.instructions.erase(Target.instructions.begin());
          break;
        case 20: {
          auto Other = Target;
          Other.reference.parameters[0] = "Ljava/lang/Object;";
          Other.generic_signature = "<X:Ljava/lang/Object;>(TX;"
                                    "Lfixture/List<+Lfixture/Right;>;J)V";
          Core.methods.push_back(Other);
          break;
        }
        case 21: {
          auto Other = Target;
          Other.reference.parameters.back() = "[Ljava/lang/Object;";
          Other.generic_signature =
              "<Element:Ljava/lang/Object;>(Lfixture/List<+Lfixture/Left;>;"
              "Lfixture/List<+Lfixture/Right;>;[TElement;)V";
          Other.access.insert("varargs");
          Other.registers = 4;
          Core.methods.push_back(Other);
          break;
        }
        case 22:
          Input[3].generic_signature =
              "<E:Ljava/lang/CharSequence;>Ljava/lang/Object;";
          break;
        case 23:
          Code[0].registers[1] = Caller.registers;
          break;
        }
        Budget B;
        EXPECT_THROW(
            recoverJava(guardedClosedMap(std::move(Input), Linked, B), B),
            std::runtime_error);
      }
}

TEST(MobileDalvikSignature, GuardedClosedDelegationsKeepRawAncestorUnbound) {
  for (unsigned Dependencies : {0u, 3u})
    for (bool Synthetic : {false, true})
      for (bool Linked : {false, true}) {
        SCOPED_TRACE(::testing::PrintToString(
            std::tuple{Dependencies, Synthetic, Linked}));
        auto Input = guardedClosedClasses(Dependencies, Synthetic, true, true,
                                          true, false, false);
        Input[1].generic_signature = "<P:Lfixture/Left;>Ljava/lang/Object;";
        const auto Caller = Input[0].methods[1].reference;
        const auto Target = Input[0].methods[0].reference;
        Budget B;
        const auto Classes = guardedClosedMap(std::move(Input), Linked, B);
        const auto Plans = validateGenericSignatures(Classes, B);
        EXPECT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
        EXPECT_FALSE(Plans.classes.contains(Caller.owner));
        EXPECT_FALSE(Plans.methods.contains(Caller));
        const auto R = recoverJava(Classes, B);
        EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
        EXPECT_FALSE(R.getArray("generic_signature_projections"));
        bool Raw = false;
        for (const auto &Unit : *R.getArray("source_units"))
          Raw |= Unit.getAsObject()->getString("source")->contains(
              "class Core extends fixture.Parent {");
        EXPECT_TRUE(Raw);
        auto Bad = Classes;
        Bad.at(Caller.owner).methods[0].generic_signature =
            "(Lfixture/List<TP;>;Lfixture/List<+Lfixture/Right;>;J)V";
        Budget Invalid;
        rejected([&] { validateGenericSignatures(Bad, Invalid); },
                 "unbound type variable P");
      }
}

namespace {
std::vector<Class> scopedSuspendClasses(unsigned Prefix, bool ExternalTarget,
                                        bool ExternalCarriers, bool Range,
                                        bool Synthetic, bool TypedCaller) {
  auto C = [](std::string Name) {
    Class Result;
    Result.name = std::move(Name);
    Result.superclass = "Ljava/lang/Object;";
    Result.access = {"public"};
    Result.source_id = "owned-scoped-suspend";
    return Result;
  };
  auto I = [](uint32_t PC, std::string Name,
              std::vector<unsigned> Registers = {}, Literal Value = {},
              Reference Ref = {}) {
    Instruction Result;
    Result.pc = PC;
    Result.opcode = std::move(Name);
    Result.registers = std::move(Registers);
    Result.literal = std::move(Value);
    Result.reference = std::move(Ref);
    return Result;
  };
  auto Scope = C("Lfixture/Scope;"), Callback = C("Lfixture/Callback;"),
       Completion = C("Lfixture/Completion;"), Tag = C("Lfixture/Tag;");
  Scope.access = Callback.access =
      Completion.access = {"public", "abstract", "interface"};
  Callback.generic_signature = "<A:Ljava/lang/Object;B:Ljava/lang/Object;"
                               "R:Ljava/lang/Object;>Ljava/lang/Object;";
  Completion.generic_signature = "<R:Ljava/lang/Object;>Ljava/lang/Object;";
  Callback.external = Completion.external = Tag.external = ExternalCarriers;
  const std::vector<std::vector<std::string>> Prefixes{{},
                                                       {Scope.name},
                                                       {"J"},
                                                       {"D"},
                                                       {"I"},
                                                       {Scope.name, "J"},
                                                       {Scope.name, "D"},
                                                       {"J", Scope.name},
                                                       {Scope.name, Tag.name}};
  std::string PrefixSignature;
  unsigned Words = 2;
  for (const auto &Type : Prefixes.at(Prefix)) {
    PrefixSignature += Type;
    Words += Type == "J" || Type == "D" ? 2 : 1;
  }
  Method Work;
  Work.reference = {Scope.name, "relay", Prefixes.at(Prefix),
                    "Ljava/lang/Object;"};
  Work.reference.parameters.insert(Work.reference.parameters.end(),
                                   {Callback.name, Completion.name});
  Work.access = {"public", "static"};
  if (Synthetic)
    Work.access.insert("synthetic");
  auto Signature = [&](const std::string &Formal) {
    return "<" + Formal + ":Ljava/lang/Object;>(" + PrefixSignature +
           "Lfixture/Callback<-Lfixture/Scope;-Lfixture/Completion<-T" +
           Formal + ";>;+Ljava/lang/Object;>;Lfixture/Completion<-T" + Formal +
           ";>;)Ljava/lang/Object;";
  };
  Work.generic_signature = Signature("T");
  Work.registers = Words + 1;
  Work.instructions = {I(0, "const/4", {0}, int64_t(0)),
                       I(1, "return-object", {0})};
  Work.code_end = 2;
  Scope.external = ExternalTarget;
  if (ExternalTarget) {
    Work.instructions.clear();
    Work.registers = Work.code_end = 0;
  }
  Scope.methods = {Work};
  auto Client = C("Lfixture/Client;");
  Method Call;
  Call.reference = Work.reference;
  Call.reference.owner = Client.name;
  Call.reference.name = "call";
  Call.access = {"public", "static"};
  if (TypedCaller)
    Call.generic_signature = Signature("U");
  const unsigned Start = Range ? 16 : 1;
  Call.registers = Start + Words;
  std::vector<unsigned> Arguments;
  for (unsigned N = 0; N < Words; ++N)
    Arguments.push_back(Start + N);
  Call.instructions = {I(0, Range ? "invoke-static/range" : "invoke-static",
                         Arguments, {}, Work.reference),
                       I(3, "move-result-object", {0}),
                       I(4, "return-object", {0})};
  Call.code_end = 5;
  Client.methods = {Call};
  auto Object = C("Ljava/lang/Object;");
  Object.external = true;
  Object.superclass.reset();
  return {Client, Scope, Callback, Completion, Tag, Object};
}
} // namespace

TEST(MobileDalvikSignature, ScopedSuspendPrefixesShareCompletePairedBinding) {
  for (unsigned Prefix = 0; Prefix < 9; ++Prefix)
    for (bool ExternalTarget : {false, true})
      for (bool ExternalCarriers : {false, true})
        for (bool Range : {false, true})
          for (bool Synthetic : {false, true})
            for (bool TypedCaller : {false, true})
              for (bool Linked : {false, true}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{Prefix, ExternalTarget, ExternalCarriers, Range,
                               Synthetic, TypedCaller, Linked}));
                auto Input = scopedSuspendClasses(Prefix, ExternalTarget,
                                                  ExternalCarriers, Range,
                                                  Synthetic, TypedCaller);
                const auto Target = Input[1].methods[0].reference;
                const auto Signature = Input[1].methods[0].generic_signature;
                const auto Caller = Input[0].methods[0].reference;
                Budget B;
                const auto Classes =
                    guardedClosedMap(std::move(Input), Linked, B);
                const auto Plans = validateGenericSignatures(Classes, B);
                ASSERT_TRUE(Plans.erased_generic_invocations.contains(Target));
                EXPECT_EQ(Plans.methods.at(Target).type_parameters[0].name,
                          "T");
                if (TypedCaller) {
                  EXPECT_EQ(Plans.methods.at(Caller).type_parameters[0].name,
                            "U");
                  for (const auto &Type : Plans.methods.at(Caller).types)
                    if (Type.variable_owner)
                      EXPECT_EQ(Type.variable_owner, Caller.identity());
                }
                EXPECT_EQ(Classes.at(Target.owner).methods[0].generic_signature,
                          Signature);
                const auto R = recoverJava(Classes, B);
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
                EXPECT_FALSE(R.getArray("class_source_bindings"));
                bool FoundTarget = false;
                for (const auto &Value : *R.getArray("methods")) {
                  const auto &Row = *Value.getAsObject();
                  if (Row.getString("identity") != Target.identity())
                    continue;
                  FoundTarget = true;
                  if (Synthetic) {
                    EXPECT_EQ(Row.getString("status"), "source-projected");
                    EXPECT_EQ(Row.getString("projection_kind"),
                              "synthetic-method");
                    ASSERT_TRUE(Row.getArray("metadata_differences"));
                    bool Flag = false;
                    for (const auto &Difference :
                         *Row.getArray("metadata_differences"))
                      Flag |= Difference.getAsString()->contains("synthetic");
                    EXPECT_TRUE(Flag);
                  } else {
                    EXPECT_EQ(Row.getString("status"), "recovered");
                    EXPECT_FALSE(Row.getArray("metadata_differences"));
                  }
                }
                EXPECT_EQ(FoundTarget, !ExternalTarget);
              }
}

TEST(MobileDalvikSignature, ScopedSuspendPrefixesRejectOpenContractsAndWords) {
  for (bool Linked : {false, true})
    for (bool Range : {false, true})
      for (unsigned Mutation = 0; Mutation < 28; ++Mutation) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{Linked, Range, Mutation}));
        auto Input = scopedSuspendClasses(Mutation >= 25 ? 8 : 6, false, true,
                                          Range, true, false);
        auto &Work = Input[1].methods[0];
        auto &Call = Input[0].methods[0];
        auto &Signature = *Work.generic_signature;
        auto Replace = [&](std::string From, std::string To) {
          const auto At = Signature.find(From);
          ASSERT_NE(At, std::string::npos);
          Signature.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Completion<-TT;>", "Completion<+TT;>");
          break;
        case 1:
          Signature.replace(Signature.rfind("<-TT;"), 5, "<*");
          break;
        case 2:
          Replace("+Ljava/lang/Object;", "-Ljava/lang/Object;");
          break;
        case 3:
          Work.access.insert("varargs");
          break;
        case 4:
          Work.declared_throws = {"Ljava/lang/Exception;"};
          break;
        case 5:
          Input[2].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;"
              "R:Ljava/lang/CharSequence;>Ljava/lang/Object;";
          break;
        case 6:
          Input[3].generic_signature.reset();
          break;
        case 7:
          Input[1].generic_signature =
              "<Q:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 8:
          Replace("<-Lfixture/Scope;", "<-Lfixture/Missing;");
          break;
        case 9:
          Input[1].access.erase("public");
          break;
        case 10:
          Replace("T:Ljava/lang/Object;", "T:Ljava/lang/CharSequence;");
          break;
        case 11:
          Replace("T:Ljava/lang/Object;>",
                  "T:Ljava/lang/Object;U:Ljava/lang/Object;>");
          break;
        case 12:
          Replace(";D", ";I");
          break;
        case 13:
          Replace("<-TT;>", "<-TUnknown;>");
          break;
        case 14:
          Call.instructions[0].opcode =
              Range ? "invoke-virtual/range" : "invoke-virtual";
          break;
        case 15:
          Call.instructions[0].registers.pop_back();
          break;
        case 16:
          Call.instructions[0].registers[2] = Call.instructions[0].registers[1];
          break;
        case 17:
          ++Call.instructions[0].registers[2];
          break;
        case 18:
          Replace("(Lfixture/Scope;", "(Ljava/lang/Object;");
          break;
        case 19:
          Call.instructions[0].registers.back() = Call.registers;
          break;
        case 20:
          Work.access.erase("public");
          Work.access.insert("private");
          break;
        case 21:
          Work.access.insert("bridge");
          break;
        case 22:
          Work.reference.parameters.insert(Work.reference.parameters.begin(),
                                           "I");
          Signature.insert(Signature.find('(') + 1, "I");
          Call.reference.parameters = Work.reference.parameters;
          Call.instructions[0].reference = Work.reference;
          Call.instructions[0].opcode = "invoke-static/range";
          Call.instructions[0].registers.clear();
          for (unsigned I = 1; I <= 6; ++I)
            Call.instructions[0].registers.push_back(I);
          Call.registers = 7;
          break;
        case 23:
          Work.access.insert("native");
          Work.instructions.clear();
          Work.registers = Work.code_end = 0;
          break;
        case 24:
          Input[2].generic_signature =
              "<A:Ljava/lang/Object;"
              "R:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 25:
          Input[4].generic_signature =
              "<Q:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 26:
          Input[4].access.erase("public");
          break;
        case 27:
          Input.erase(Input.begin() + 4);
          break;
        }
        Budget B;
        EXPECT_THROW(
            recoverJava(guardedClosedMap(std::move(Input), Linked, B), B),
            std::runtime_error);
      }
}

namespace {
std::vector<Class> closedNominalOverloadClasses(bool Interface,
                                                bool ExternalLeaf,
                                                bool ExternalCarriers,
                                                bool Range, bool Frame,
                                                bool NullHandler) {
  auto C = [](std::string Name) {
    Class Result;
    Result.name = std::move(Name);
    Result.superclass = "Ljava/lang/Object;";
    Result.access = {"public"};
    Result.source_id = "owned-nominal-overload";
    return Result;
  };
  auto I = [](uint32_t PC, std::string Name,
              std::vector<unsigned> Registers = {}, Literal Value = {},
              Reference Ref = {}, std::optional<uint32_t> Target = {}) {
    Instruction Result;
    Result.pc = PC;
    Result.opcode = std::move(Name);
    Result.registers = std::move(Registers);
    Result.literal = std::move(Value);
    Result.reference = std::move(Ref);
    Result.target = Target;
    return Result;
  };
  auto Core = C("Lfixture/Core;"), Handler = C("Lfixture/Handler;"),
       Callback = C("Lfixture/Callback;"),
       Completion = C("Lfixture/Completion;"), Context = C("Lfixture/Context;"),
       Value = C("Lfixture/Value;"), Marker = C("Lfixture/Marker;");
  Core.access.insert("final");
  if (Interface)
    Handler.access = {"public", "abstract", "interface"};
  Handler.external = ExternalLeaf;
  Callback.access = Completion.access = {"public", "abstract", "interface"};
  Callback.generic_signature = "<A:Ljava/lang/Object;B:Ljava/lang/Object;"
                               "R:Ljava/lang/Object;>Ljava/lang/Object;";
  Completion.generic_signature = "<R:Ljava/lang/Object;>Ljava/lang/Object;";
  Callback.external = Completion.external = Context.external = Value.external =
      ExternalCarriers;
  Method Target;
  Target.reference = {Core.name,
                      "<init>",
                      {"Ljava/lang/Object;", "Ljava/lang/Object;",
                       "[Ljava/lang/Object;", Handler.name},
                      "V"};
  Target.access = {"public", "constructor"};
  Target.registers = 5;
  Target.instructions = {I(0, "invoke-direct", {0}, {},
                           MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                         I(3, "return-void")};
  Target.code_end = 4;
  auto Sibling = Target;
  Sibling.reference.parameters.back() = Callback.name;
  Sibling.generic_signature =
      "(Ljava/lang/Object;Ljava/lang/Object;[Ljava/lang/Object;"
      "Lfixture/Callback<-Lfixture/Context;-Lfixture/Completion<-Lfixture/"
      "Value;>;"
      "+Ljava/lang/Object;>;)V";
  auto Caller = Target;
  Caller.reference.parameters.insert(Caller.reference.parameters.end(),
                                     {"I", Marker.name});
  Caller.access.insert("synthetic");
  const unsigned Self = Frame ? 8 : 1, Mask = Self + 5, Scratch = Self + 6;
  Caller.registers = Self + 7;
  Caller.instructions = {I(0, "and-int/lit8", {Scratch, Mask}, int64_t(1)),
                         I(2, "const/4", {0}, int64_t(0)),
                         I(3, "if-eqz", {Scratch}, {}, {}, 6),
                         I(5, "move-object", {Self + 1, 0}),
                         I(6, "and-int/lit8", {Scratch, Mask}, int64_t(2)),
                         I(8, "if-eqz", {Scratch}, {}, {}, 11),
                         I(10, "move-object", {Self + 2, 0}),
                         I(11, "and-int/lit8", {Mask, Mask}, int64_t(4)),
                         I(13, "if-eqz", {Mask}, {}, {}, 16),
                         I(15, "move-object", {Self + 3, 0})};
  unsigned PC = 16;
  if (NullHandler)
    Caller.instructions.push_back(I(PC++, "const/4", {Self + 4}, int64_t(0)));
  Caller.instructions.push_back(
      I(PC, Range ? "invoke-direct/range" : "invoke-direct",
        {Self, Self + 1, Self + 2, Self + 3, Self + 4}, {}, Target.reference));
  Caller.instructions.push_back(I(PC + 3, "return-void"));
  Caller.code_end = PC + 4;
  Core.methods = {Target, Sibling, Caller};
  auto Object = C("Ljava/lang/Object;");
  Object.external = true;
  Object.superclass.reset();
  Method ObjectInit;
  ObjectInit.reference = {Object.name, "<init>", {}, "V"};
  ObjectInit.access = {"public", "constructor"};
  Object.methods = {ObjectInit};
  return {Core, Handler, Callback, Completion, Context, Value, Marker, Object};
}
} // namespace

TEST(MobileDalvikSignature, ClosedNominalOverloadsRetainEveryDescriptorView) {
  for (bool Interface : {false, true})
    for (bool ExternalLeaf : {false, true})
      for (bool ExternalCarriers : {false, true})
        for (bool Range : {false, true})
          for (bool Frame : {false, true})
            for (bool NullHandler : {false, true})
              for (bool Linked : {false, true}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{Interface, ExternalLeaf, ExternalCarriers, Range,
                               Frame, NullHandler, Linked}));
                auto Input = closedNominalOverloadClasses(
                    Interface, ExternalLeaf, ExternalCarriers, Range, Frame,
                    NullHandler);
                const auto Target = Input[0].methods[0].reference;
                const auto Sibling = Input[0].methods[1].reference;
                const auto Caller = Input[0].methods[2].reference;
                const auto Signature = Input[0].methods[1].generic_signature;
                Budget B;
                const auto Classes =
                    guardedClosedMap(std::move(Input), Linked, B);
                const auto Plans = validateGenericSignatures(Classes, B);
                EXPECT_FALSE(Plans.methods.contains(Target));
                EXPECT_FALSE(Plans.methods.contains(Caller));
                EXPECT_TRUE(Plans.methods.contains(Sibling));
                EXPECT_FALSE(
                    Plans.constructor_calls.contains({Caller, Target}));
                const auto R = recoverJava(Classes, B);
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
                EXPECT_FALSE(R.getArray("class_source_bindings"));
                EXPECT_EQ(
                    Classes.at(Sibling.owner).methods[1].generic_signature,
                    Signature);
                bool View = false;
                for (const auto &Unit : *R.getArray("source_units"))
                  View |= Unit.getAsObject()->getString("source")->contains(
                      "((fixture.Handler)");
                EXPECT_TRUE(View);
              }
}

TEST(MobileDalvikSignature, ClosedNominalOverloadsRejectMissingProofsAndWords) {
  for (bool ExternalLeaf : {false, true})
    for (bool Linked : {false, true})
      for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
        SCOPED_TRACE(::testing::PrintToString(
            std::tuple{ExternalLeaf, Linked, Mutation}));
        auto Input = closedNominalOverloadClasses(true, ExternalLeaf, true,
                                                  false, false, false);
        auto &Target = Input[0].methods[0];
        auto &Sibling = Input[0].methods[1];
        auto &Caller = Input[0].methods[2];
        switch (Mutation) {
        case 0:
          Input[1].access = {"public"};
          Input[1].superclass = "Lfixture/Missing;";
          break;
        case 1:
          Input[1].interfaces = {Input[2].name};
          break;
        case 2:
          Input[1].generic_signature =
              "<Q:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 3:
          Input[1].access.erase("public");
          break;
        case 4:
          Input.erase(Input.begin() + 1);
          break;
        case 5:
          Input.erase(Input.begin() + 2);
          break;
        case 6:
          *Sibling.generic_signature =
              "<X:Ljava/lang/Object;>" + *Sibling.generic_signature;
          break;
        case 7:
          Sibling.access.insert("varargs");
          break;
        case 8:
          *Sibling.generic_signature = "(Ljava/lang/Object;Ljava/lang/Object;"
                                       "[Ljava/lang/Object;TX;)V";
          break;
        case 9:
          Sibling.reference.parameters.back() = "Ljava/lang/Object;";
          break;
        case 10:
          Input[1].enclosing = "Lfixture/Outer;";
          break;
        case 11:
          Target.access.insert("varargs");
          break;
        case 12:
          Target.generic_signature = Target.reference.signature();
          break;
        case 13:
          Caller.instructions[10].registers[0] = 0;
          break;
        case 14:
          Input[2].generic_signature =
              "<A:Ljava/lang/Object;B:Lfixture/Value;"
              "R:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 15:
          Caller.instructions[10].registers.pop_back();
          break;
        case 16:
          Caller.instructions[10].registers[4] = Caller.registers;
          break;
        case 17:
          Caller.instructions[2].target = 7;
          break;
        case 18:
          Caller.instructions[0].opcode = "and-int/unknown";
          break;
        case 19:
          Caller.instructions[1].registers[0] = 1;
          break;
        }
        Budget B;
        EXPECT_THROW(
            recoverJava(guardedClosedMap(std::move(Input), Linked, B), B),
            std::runtime_error);
      }
}

namespace {
std::vector<Class> fixedCompletionFactoryClasses(bool Seeded, bool External,
                                                 bool ExternalCarriers,
                                                 bool Range, bool TypedCaller,
                                                 bool ReverseFormals,
                                                 unsigned Variance) {
  auto C = [](std::string Name) {
    Class Result;
    Result.name = std::move(Name);
    Result.superclass = "Ljava/lang/Object;";
    Result.access = {"public"};
    Result.source_id = "owned-fixed-completion";
    return Result;
  };
  auto I = [](uint32_t PC, std::string Name,
              std::vector<unsigned> Registers = {}, Literal Value = {},
              Reference Ref = {}) {
    Instruction Result;
    Result.pc = PC;
    Result.opcode = std::move(Name);
    Result.registers = std::move(Registers);
    Result.literal = std::move(Value);
    Result.reference = std::move(Ref);
    return Result;
  };
  auto Unary = C("Lfixture/Unary;"), Binary = C("Lfixture/Binary;"),
       Completion = C("Lfixture/Completion;"), Leaf = C("Lfixture/Leaf;"),
       Other = C("Lfixture/Other;");
  Unary.access = Binary.access = Completion.access =
      Other.access = {"public", "abstract", "interface"};
  Unary.generic_signature =
      "<I:Ljava/lang/Object;O:Ljava/lang/Object;>Ljava/lang/Object;";
  Binary.generic_signature = "<A:Ljava/lang/Object;B:Ljava/lang/Object;"
                             "O:Ljava/lang/Object;>Ljava/lang/Object;";
  Completion.generic_signature = Other.generic_signature =
      "<R:Ljava/lang/Object;>Ljava/lang/Object;";
  Unary.external = Binary.external = Completion.external = Leaf.external =
      ExternalCarriers;
  Other.external = true;
  auto Signature = [&](const std::string &A, const std::string &B) {
    auto Formal = [](const std::string &Name) {
      return Name + ":Ljava/lang/Object;";
    };
    const std::string Formals =
        Seeded
            ? (ReverseFormals ? Formal(B) + Formal(A) : Formal(A) + Formal(B))
            : Formal(B);
    const std::string Nested = "Lfixture/Completion<" +
                               std::string(Variance & 4 ? "" : "-") + "T" + B +
                               ";>;";
    const std::string Callback =
        std::string(Seeded ? "Lfixture/Binary<" : "Lfixture/Unary<") +
        (Seeded ? std::string(Variance & 1 ? "" : "-") + "T" + A + ";" : "") +
        (Variance & 2 ? "" : "-") + Nested + (Variance & 8 ? "" : "+") +
        "Ljava/lang/Object;>;";
    return "<" + Formals + ">(" + Callback + (Seeded ? "T" + A + ";" : "") +
           Nested + ")Lfixture/Completion<Lfixture/Leaf;>;";
  };
  auto Factory = C("Lfixture/Factory;");
  Method Target;
  Target.reference = {
      Factory.name, "produce",
      Seeded ? std::vector<std::string>{Binary.name, "Ljava/lang/Object;",
                                        Completion.name}
             : std::vector<std::string>{Unary.name, Completion.name},
      Completion.name};
  Target.generic_signature = Signature("A", "B");
  Target.access = {"public", "static"};
  const unsigned Words = Target.reference.parameters.size();
  Target.registers = Words + 1;
  Target.instructions = {I(0, "const/4", {0}, int64_t(0)),
                         I(1, "return-object", {0})};
  Target.code_end = 2;
  Factory.external = External;
  if (External) {
    Target.instructions.clear();
    Target.registers = Target.code_end = 0;
  }
  Factory.methods = {Target};
  auto Client = C("Lfixture/Client;");
  Method Call;
  Call.reference = Target.reference;
  Call.reference.owner = Client.name;
  Call.reference.name = "call";
  Call.access = {"public", "static"};
  if (TypedCaller)
    Call.generic_signature = Signature("X", "Y");
  const unsigned Start = Range ? 16 : 1;
  Call.registers = Start + Words;
  std::vector<unsigned> Arguments;
  for (unsigned N = 0; N < Words; ++N)
    Arguments.push_back(Start + N);
  Call.instructions = {I(0, Range ? "invoke-static/range" : "invoke-static",
                         Arguments, {}, Target.reference),
                       I(3, "move-result-object", {0}),
                       I(4, "return-object", {0})};
  Call.code_end = 5;
  Client.methods = {Call};
  auto Object = C("Ljava/lang/Object;");
  Object.external = true;
  Object.superclass.reset();
  return {Client, Factory, Unary, Binary, Completion, Leaf, Other, Object};
}
} // namespace

TEST(MobileDalvikSignature,
     FixedCompletionFactoriesKeepIndependentOwnedFormals) {
  for (bool Seeded : {false, true})
    for (bool External : {false, true})
      for (bool ExternalCarriers : {false, true})
        for (bool Range : {false, true})
          for (bool TypedCaller : {false, true})
            for (bool ReverseFormals : {false, true})
              for (unsigned Variance = 0; Variance < 16; ++Variance)
                for (bool Linked : {false, true}) {
                  SCOPED_TRACE(::testing::PrintToString(std::tuple{
                      Seeded, External, ExternalCarriers, Range, TypedCaller,
                      ReverseFormals, Variance, Linked}));
                  auto Input = fixedCompletionFactoryClasses(
                      Seeded, External, ExternalCarriers, Range, TypedCaller,
                      ReverseFormals, Variance);
                  const auto Ref = Input[1].methods[0].reference;
                  const auto Signature = Input[1].methods[0].generic_signature;
                  const auto Caller = Input[0].methods[0].reference;
                  Budget B;
                  const auto Classes =
                      guardedClosedMap(std::move(Input), Linked, B);
                  const auto Plans = validateGenericSignatures(Classes, B);
                  ASSERT_TRUE(Plans.erased_generic_invocations.contains(Ref));
                  EXPECT_EQ(Plans.methods.at(Ref).type_parameters.size(),
                            Seeded ? 2u : 1u);
                  if (TypedCaller)
                    for (const auto &Type : Plans.methods.at(Caller).types)
                      if (Type.variable_owner)
                        EXPECT_EQ(Type.variable_owner, Caller.identity());
                  const auto R = recoverJava(Classes, B);
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                  EXPECT_FALSE(R.getArray("class_source_bindings"));
                  EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature,
                            Signature);
                }
}

TEST(MobileDalvikSignature,
     FixedCompletionFactoriesRejectOpenOrMismatchedGraphs) {
  for (bool Seeded : {false, true})
    for (bool External : {false, true})
      for (bool Linked : {false, true})
        for (unsigned Mutation = 0; Mutation < 24; ++Mutation) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{Seeded, External, Linked, Mutation}));
          auto Input = fixedCompletionFactoryClasses(Seeded, External, true,
                                                     false, false, false, 0);
          auto &Target = Input[1].methods[0];
          auto &Call = Input[0].methods[0];
          auto &S = *Target.generic_signature;
          auto Replace = [&](std::string From, std::string To) {
            const auto At = S.find(From);
            ASSERT_NE(At, std::string::npos);
            S.replace(At, From.size(), To);
          };
          switch (Mutation) {
          case 0:
            Replace("Completion<Lfixture/Leaf;>",
                    "Completion<+Lfixture/Leaf;>");
            break;
          case 1:
            Replace("Completion<Lfixture/Leaf;>", "Completion<TB;>");
            break;
          case 2:
            Replace(")Lfixture/Completion<", ")Lfixture/Other<");
            Target.reference.returns = Input[6].name;
            Call.reference.returns = Input[6].name;
            Call.instructions[0].reference = Target.reference;
            break;
          case 3:
            Replace(")Lfixture/Completion<Lfixture/Leaf;>;",
                    ")Lfixture/Completion;");
            break;
          case 4:
            Replace("B:Ljava/lang/Object;", "Q:Ljava/lang/Object;"
                                            "B:Ljava/lang/Object;");
            break;
          case 5:
            if (Seeded)
              Replace("Binary<-TA;", "Binary<-TB;");
            else
              Replace("Unary<-Lfixture/Completion<-TB;>",
                      "Unary<-Lfixture/Completion<*>");
            break;
          case 6:
            Replace("Completion<-TB;>", "Completion<+TB;>");
            break;
          case 7:
            S.replace(S.rfind("Completion<-TB;>"),
                      std::string("Completion<-TB;>").size(),
                      Seeded ? "Completion<-TA;>" : "Completion<*>");
            break;
          case 8:
            Replace("+Ljava/lang/Object;", "-Ljava/lang/Object;");
            break;
          case 9:
            Input.erase(Input.begin() + 5);
            break;
          case 10:
            Input[5].generic_signature =
                "<Q:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 11:
            Input[4].generic_signature = "<R:Lfixture/Leaf;>Ljava/lang/Object;";
            break;
          case 12:
            if (Seeded)
              Input[3].generic_signature =
                  "<A:Ljava/lang/Object;B:Lfixture/Leaf;"
                  "O:Ljava/lang/Object;>Ljava/lang/Object;";
            else
              Input[2].generic_signature =
                  "<I:Lfixture/Leaf;"
                  "O:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 13:
            Input[1].generic_signature =
                "<Q:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 14:
            Replace("B:Ljava/lang/Object;", "B:Lfixture/Leaf;");
            break;
          case 15:
            Target.access.insert("varargs");
            break;
          case 16:
            Target.access.insert("synthetic");
            break;
          case 17:
            Target.access.erase("public");
            break;
          case 18:
            Target.access.erase("static");
            break;
          case 19:
            Target.reference.parameters.pop_back();
            Call.instructions[0].reference = Target.reference;
            break;
          case 20:
            Call.instructions[0].registers.pop_back();
            break;
          case 21:
            if (Seeded)
              Input[3].generic_signature = "<A:Ljava/lang/Object;O:Ljava/lang/"
                                           "Object;>Ljava/lang/Object;";
            else
              Input[2].generic_signature =
                  "<I:Ljava/lang/Object;Q:Ljava/lang/Object;"
                  "O:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 22:
            Target.reference.returns = "Ljava/lang/Object;";
            Call.reference.returns = Target.reference.returns;
            Call.instructions[0].reference = Target.reference;
            break;
          case 23:
            Target.declared_throws = {"Ljava/lang/Exception;"};
            break;
          }
          Budget B;
          EXPECT_THROW(
              recoverJava(guardedClosedMap(std::move(Input), Linked, B), B),
              std::runtime_error);
        }
}

namespace {
std::vector<Class> scopedOwnerCompletionClasses(bool Captured, bool Shadow,
                                                bool ExternalCarriers,
                                                bool Range, bool TypedCaller) {
  auto Input = scopedSuspendClasses(2, false, ExternalCarriers, Range, false,
                                    TypedCaller);
  auto I = [](uint32_t PC, std::string Name,
              std::vector<unsigned> Registers = {}, Reference Ref = {}) {
    Instruction Result;
    Result.pc = PC;
    Result.opcode = std::move(Name);
    Result.registers = std::move(Registers);
    Result.reference = std::move(Ref);
    return Result;
  };
  auto &Outer = Input[0];
  auto &Scope = Input[1];
  Class Inner;
  Inner.name = Captured ? "Lfixture/Client$Session;" : "Lfixture/Session;";
  Inner.superclass = "Ljava/lang/Object;";
  Inner.interfaces = {Scope.name};
  Inner.access = Captured ? Access{"final"} : Access{"public", "final"};
  Inner.source_id = "owned-scoped-owner";
  Inner.generic_signature =
      std::string("<") + (Shadow ? "T" : "R") +
      ":Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Scope;";
  auto Work = Scope.methods[0];
  Work.reference.owner = Inner.name;
  Work.access = {"public"};
  ++Work.registers;
  Inner.methods = {Work};
  auto Required = Work;
  Required.reference.owner = Scope.name;
  Required.access = {"public", "abstract"};
  Required.instructions.clear();
  Required.registers = Required.code_end = 0;
  Scope.methods = {Required};
  if (Captured) {
    Inner.inner_class_present = true;
    Inner.inner_name = "Session";
    Inner.enclosing = Outer.name;
    Inner.inner_access = {"private", "final"};
    Field Capture;
    Capture.reference = {Inner.name, "outer$", Outer.name};
    Capture.access = {"final", "synthetic"};
    Inner.fields = {Capture};
    Method Init;
    Init.reference = {Inner.name, "<init>", {Outer.name}, "V"};
    Init.access = {"public", "constructor"};
    Init.registers = 2;
    Init.instructions = {I(0, "iput-object", {1, 0}, Capture.reference),
                         I(2, "invoke-direct", {0},
                           MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                         I(5, "return-void")};
    Init.code_end = 6;
    Inner.methods.push_back(Init);
  }
  auto &Call = Outer.methods[0];
  Call.reference.parameters.insert(Call.reference.parameters.begin(),
                                   Inner.name);
  if (Call.generic_signature)
    Call.generic_signature->insert(Call.generic_signature->find('(') + 1,
                                   Inner.name);
  const unsigned Start = Range ? 16 : 1;
  Call.registers = Start + 5;
  Call.instructions[0] =
      I(0, Range ? "invoke-virtual/range" : "invoke-virtual",
        {Start, Start + 1, Start + 2, Start + 3, Start + 4}, Work.reference);
  Method ObjectInit;
  ObjectInit.reference = {"Ljava/lang/Object;", "<init>", {}, "V"};
  ObjectInit.access = {"public", "constructor"};
  Input[5].methods = {ObjectInit};
  Input.push_back(std::move(Inner));
  return Input;
}
} // namespace

TEST(MobileDalvikSignature, CapturedGenericOwnersKeepMethodFormalOwnership) {
  for (bool Captured : {false, true})
    for (bool Shadow : {false, true})
      for (bool ExternalCarriers : {false, true})
        for (bool Range : {false, true})
          for (bool TypedCaller : {false, true})
            for (bool Linked : {false, true}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{Captured, Shadow, ExternalCarriers, Range,
                             TypedCaller, Linked}));
              auto Input = scopedOwnerCompletionClasses(
                  Captured, Shadow, ExternalCarriers, Range, TypedCaller);
              const auto Target = Input.back().methods[0].reference;
              const auto Original = Input.back().methods[0].generic_signature;
              Budget B;
              const auto Classes =
                  guardedClosedMap(std::move(Input), Linked, B);
              const auto Plans = validateGenericSignatures(Classes, B);
              EXPECT_EQ(Plans.captured_inner_classes.contains(Target.owner),
                        Captured);
              ASSERT_TRUE(Plans.erased_generic_invocations.contains(Target));
              const auto &Signature = Plans.methods.at(Target);
              for (const auto &Type : Signature.types)
                if (Type.kind == GenericType::Kind::TypeVariable)
                  EXPECT_EQ(Type.variable_owner, Target.identity());
              EXPECT_EQ(Classes.at(Target.owner).methods[0].generic_signature,
                        Original);
              const auto R = recoverJava(Classes, B);
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikSignature, CapturedMethodBindingsRejectBorrowedAndOpenScopes) {
  for (bool Linked : {false, true})
    for (bool Range : {false, true})
      for (unsigned Mutation = 0; Mutation < 24; ++Mutation) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{Linked, Range, Mutation}));
        auto Input =
            scopedOwnerCompletionClasses(true, false, true, Range, false);
        auto &Inner = Input.back();
        auto &Work = Inner.methods[0];
        auto &Call = Input[0].methods[0];
        auto &Signature = *Work.generic_signature;
        auto Replace = [&](std::string From, std::string To) {
          const auto At = Signature.find(From);
          ASSERT_NE(At, std::string::npos);
          Signature.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Completion<-TT;>", "Completion<-TR;>");
          break;
        case 1:
          Signature.replace(Signature.rfind("<-TT;"), 5, "<-TR;");
          break;
        case 2:
          Signature.replace(Signature.rfind("Ljava/lang/Object;"), 18, "TR;");
          break;
        case 3:
          Inner.fields[0].access.erase("synthetic");
          break;
        case 4:
          Inner.inner_name.reset();
          break;
        case 5:
          Inner.external = true;
          for (auto &M : Inner.methods) {
            M.instructions.clear();
            M.registers = M.code_end = 0;
          }
          break;
        case 6:
          Work.access = {"private"};
          break;
        case 7:
          Work.access.insert("static");
          break;
        case 8:
          Replace("T:Ljava/lang/Object;", "T:Ljava/lang/CharSequence;");
          break;
        case 9:
          Replace("T:Ljava/lang/Object;>",
                  "T:Ljava/lang/Object;U:Ljava/lang/Object;>");
          break;
        case 10:
          Input[2].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;"
              "R:Ljava/lang/CharSequence;>Ljava/lang/Object;";
          break;
        case 11:
          Replace("Completion<-TT;>", "Completion<+TT;>");
          break;
        case 12:
          Replace("+Ljava/lang/Object;", "-Ljava/lang/Object;");
          break;
        case 13:
          Work.access.insert("varargs");
          break;
        case 14:
          Work.access.insert("bridge");
          break;
        case 15:
          Work.declared_throws = {"Ljava/lang/Exception;"};
          break;
        case 16:
          Work.access.insert("native");
          Work.instructions.clear();
          Work.registers = Work.code_end = 0;
          break;
        case 17:
          Work.access.insert("abstract");
          Work.instructions.clear();
          Work.registers = Work.code_end = 0;
          break;
        case 18:
          Call.instructions[0].opcode = "invoke-static/range";
          break;
        case 19:
          Call.instructions[0].registers.pop_back();
          break;
        case 20:
          Call.instructions[0].registers[2] = Call.instructions[0].registers[1];
          break;
        case 21:
          Call.instructions[0].registers.back() = Call.registers;
          break;
        case 22:
          Input[3].generic_signature.reset();
          break;
        case 23:
          Replace("<-Lfixture/Scope;", "<-Lfixture/Missing;");
          break;
        }
        const auto Target = Work.reference;
        try {
          Budget B;
          const auto Classes = guardedClosedMap(std::move(Input), Linked, B);
          const auto Plans = validateGenericSignatures(Classes, B);
          // A declaration binding does not validate a particular physical call.
          // Wrong words and registers remain rejected by Body below.
          if (Mutation < 18 || Mutation > 21)
            EXPECT_FALSE(Plans.erased_generic_invocations.contains(Target));
          EXPECT_THROW(recoverJava(Classes, B), std::runtime_error);
        } catch (const Error &) {
          SUCCEED();
        }
      }
}

namespace {
std::vector<Class> boundedFixedFactoryClasses(bool Private, bool External,
                                              bool ExternalCarriers, bool Range,
                                              bool TypedCaller,
                                              unsigned Variance,
                                              const std::string &Scalar) {
  auto C = [](std::string Name, bool SDK = false) {
    Class V;
    V.name = std::move(Name);
    V.access = {"public"};
    V.superclass = "Ljava/lang/Object;";
    V.source_id = "owned-bounded-factory";
    V.external = SDK;
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Regs = {},
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Regs);
    V.reference = std::move(Ref);
    return V;
  };
  auto Client = C("Lfixture/Client;"),
       Provider = C("Lfixture/Provider;", External);
  auto Producer = C("Lfixture/Producer;", ExternalCarriers);
  auto Factory = C("Lfixture/Factory;", ExternalCarriers);
  auto Base = C("Lfixture/Base;", ExternalCarriers);
  auto Context = C("Lfixture/Context;", ExternalCarriers);
  auto Leaf = C("Lfixture/Leaf;", ExternalCarriers);
  Producer.access = Factory.access = {"public", "abstract", "interface"};
  Producer.generic_signature =
      "<I:Ljava/lang/Object;O:Ljava/lang/Object;>Ljava/lang/Object;";
  Factory.generic_signature = "<R:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Signature = [&](std::string Formal) {
    return "<" + Formal + ":Lfixture/Base;>(Lfixture/Producer<" +
           (Variance & 1 ? "" : "-") + "Lfixture/Context;" +
           (Variance & 2 ? "" : "+") + "T" + Formal + ";>;Lfixture/Context;" +
           Scalar + ")Lfixture/Factory<Lfixture/Leaf;>;";
  };
  Method Target;
  Target.reference = {Provider.name,
                      "make",
                      {Producer.name, Context.name, Scalar},
                      Factory.name};
  Target.generic_signature = Signature("T");
  Target.access = {Private ? "private" : "public", "static"};
  const unsigned Words = Scalar == "J" || Scalar == "D" ? 4 : 3;
  Target.registers = Words + 1;
  Target.code_end = 2;
  Target.instructions = {I(0, "const/4", {0}), I(1, "return-object", {0})};
  Target.instructions[0].literal = int64_t(0);
  if (External) {
    Target.instructions.clear();
    Target.registers = Target.code_end = 0;
  }
  Method Call;
  Call.reference = Target.reference;
  Call.reference.name = "call";
  Call.reference.owner = Private ? Provider.name : Client.name;
  Call.access = {"public", "static"};
  if (TypedCaller)
    Call.generic_signature = Signature("X");
  const unsigned Start = Range ? 16 : 1;
  Call.registers = Start + Words;
  std::vector<unsigned> Regs;
  for (unsigned N = 0; N < Words; ++N)
    Regs.push_back(Start + N);
  Call.instructions = {I(0, Range ? "invoke-static/range" : "invoke-static",
                         Regs, Target.reference),
                       I(3, "move-result-object", {0}),
                       I(4, "return-object", {0})};
  Call.code_end = 5;
  Provider.methods = {Target};
  if (Private)
    Provider.methods.push_back(Call);
  else
    Client.methods = {Call};
  auto Object = C("Ljava/lang/Object;", true);
  Object.superclass.reset();
  return {Client, Provider, Producer, Factory, Base, Context, Leaf, Object};
}
ClassMap boundedFixedMap(std::vector<Class> Input, unsigned Route, Budget &B) {
  if (Route == 2)
    std::reverse(Input.begin(), Input.end());
  if (Route)
    return linkClasses(std::move(Input), B);
  ClassMap Out;
  for (auto &C : Input)
    Out.emplace(C.name, std::move(C));
  return Out;
}
} // namespace

TEST(MobileDalvikSignature, BoundedProducersKeepFixedFactoryResults) {
  for (bool Private : {false, true})
    for (bool External : {false, true}) {
      if (Private && External)
        continue;
      for (bool SDK : {false, true})
        for (bool Range : {false, true})
          for (bool Typed : {false, true})
            for (unsigned Variance = 0; Variance < 4; ++Variance)
              for (const std::string Scalar : {"I", "J", "F", "D"})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{Private, External, SDK, Range, Typed, Variance,
                                 Scalar, Route}));
                  auto Input = boundedFixedFactoryClasses(
                      Private, External, SDK, Range, Typed, Variance, Scalar);
                  const auto Ref = Input[1].methods[0].reference;
                  const auto Signature = Input[1].methods[0].generic_signature;
                  Budget B;
                  const auto Classes =
                      boundedFixedMap(std::move(Input), Route, B);
                  const auto Plans = validateGenericSignatures(Classes, B);
                  ASSERT_TRUE(Plans.erased_generic_invocations.contains(Ref));
                  auto R = recoverJava(Classes, B);
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                  EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature,
                            Signature);
                }
    }
}

TEST(MobileDalvikSignature,
     BoundedFixedFactoriesRejectOpenGraphsAndForeignAccess) {
  for (unsigned Mutation = 0; Mutation < 20; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input =
          boundedFixedFactoryClasses(true, false, true, false, false, 0, "J");
      auto &Target = Input[1].methods[0];
      auto &S = *Target.generic_signature;
      auto Replace = [&](const std::string &From, const std::string &To) {
        const auto At = S.find(From);
        ASSERT_NE(At, std::string::npos);
        S.replace(At, From.size(), To);
      };
      switch (Mutation) {
      case 0:
        Replace("Factory<Lfixture/Leaf;>", "Factory<+Lfixture/Leaf;>");
        break;
      case 1:
        Replace("Factory<Lfixture/Leaf;>", "Factory<TT;>");
        break;
      case 2:
        Replace("+TT;", "-TT;");
        break;
      case 3:
        Replace("Producer<-Lfixture/Context;", "Producer<+Lfixture/Context;");
        break;
      case 4:
        Input[4].access = {"public", "interface", "abstract"};
        break;
      case 5:
        Input[2].generic_signature = "<I:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 6:
        Input[3].generic_signature = "<R:Lfixture/Base;>Ljava/lang/Object;";
        break;
      case 7:
        Target.access.insert("varargs");
        break;
      case 8:
        Target.access.insert("synthetic");
        break;
      case 9:
        Target.access.erase("static");
        break;
      case 10:
        Input.erase(Input.begin() + 4);
        break;
      case 11:
        Input[4].generic_signature = "<Q:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 12:
        Input[4].access = {"private"};
        break;
      case 13:
        Replace("<T:Lfixture/Base;>", "<Q:Ljava/lang/Object;T:Lfixture/Base;>");
        break;
      case 14:
        Replace(";J)", ";I)");
        break;
      case 15:
        Input[5].generic_signature = "<Q:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 16:
        Input[6].generic_signature = "<Q:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 17:
        Replace("+TT;", "+TQ;");
        break;
      case 18:
        Target.instructions.clear();
        Target.registers = Target.code_end = 0;
        break;
      case 19: {
        auto Foreign = Input[1].methods[1];
        Foreign.reference.owner = Input[0].name;
        Input[0].methods = {Foreign};
        break;
      }
      }
      Budget B;
      EXPECT_THROW(recoverJava(boundedFixedMap(std::move(Input), Route, B), B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> unusedNominalFactoryClasses(bool Private, bool External,
                                               bool SDK, bool Range, bool Typed,
                                               bool Void, bool TwoFormals) {
  auto Input =
      boundedFixedFactoryClasses(Private, External, SDK, Range, Typed, 0, "J");
  auto &Provider = Input[1];
  auto &Target = Provider.methods[0];
  auto &Call = Private ? Provider.methods[1] : Input[0].methods[0];
  auto Close = [&](Method &M) {
    if (!M.generic_signature)
      return;
    auto &S = *M.generic_signature;
    const auto At = S.find("+T");
    const auto End = S.find(';', At);
    S.replace(At, End - At + 1, "+Lfixture/Base;");
    if (TwoFormals)
      S.insert(S.find(">("), "U:Ljava/lang/Object;");
    if (Void)
      S.replace(S.find(')'), std::string::npos, ")V");
  };
  Close(Target);
  Close(Call);
  if (Void) {
    Target.reference.returns = Call.reference.returns = "V";
    if (!External) {
      Target.instructions.resize(1);
      Target.instructions[0].opcode = "return-void";
      Target.instructions[0].registers.clear();
      Target.instructions[0].literal = std::monostate{};
      Target.code_end = 1;
    }
    Call.instructions.erase(Call.instructions.begin() + 1);
    Call.instructions.back().opcode = "return-void";
    Call.instructions.back().registers.clear();
  }
  Call.instructions[0].reference = Target.reference;
  return Input;
}
} // namespace

TEST(MobileDalvikSignature, UnusedNominalFormalsKeepClosedRuntimeSlots) {
  for (bool Private : {false, true})
    for (bool External : {false, true}) {
      if (Private && External)
        continue;
      for (bool SDK : {false, true})
        for (bool Range : {false, true})
          for (bool Typed : {false, true})
            for (bool Void : {false, true})
              for (bool Two : {false, true})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  SCOPED_TRACE(::testing::PrintToString(std::tuple{
                      Private, External, SDK, Range, Typed, Void, Two, Route}));
                  auto Input = unusedNominalFactoryClasses(
                      Private, External, SDK, Range, Typed, Void, Two);
                  const auto Ref = Input[1].methods[0].reference;
                  const auto Signature = Input[1].methods[0].generic_signature;
                  Budget B;
                  const auto Classes =
                      boundedFixedMap(std::move(Input), Route, B);
                  const auto Plans = validateGenericSignatures(Classes, B);
                  ASSERT_TRUE(
                      Plans.erased_unused_formal_invocations.contains(Ref));
                  auto R = recoverJava(Classes, B);
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                  EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature,
                            Signature);
                }
    }
}

TEST(MobileDalvikSignature, UnusedNominalFormalsDoNotBindUsesOrUnprovedBounds) {
  for (unsigned Mutation = 0; Mutation < 13; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = unusedNominalFactoryClasses(true, false, true, false, false,
                                               false, false);
      auto &Target = Input[1].methods[0];
      const auto Ref = Target.reference;
      auto &S = *Target.generic_signature;
      auto Replace = [&](const std::string &From, const std::string &To) {
        const auto At = S.find(From);
        ASSERT_NE(At, std::string::npos);
        S.replace(At, From.size(), To);
      };
      switch (Mutation) {
      case 0:
        Replace("+Lfixture/Base;", "+TT;");
        break;
      case 1:
        Replace("Factory<Lfixture/Leaf;>", "Factory<TT;>");
        break;
      case 2:
        Replace("T:Lfixture/Base;", "T:Lmissing/Base;");
        break;
      case 3:
        Input[4].generic_signature = "<Q:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 4:
        Input[4].access = {"public", "interface", "abstract"};
        break;
      case 5:
        Input[4].access = {"private"};
        break;
      case 6:
        Replace("T:Lfixture/Base;", "T:TT;");
        break;
      case 7:
        Replace("T:Lfixture/Base;", "T:Lfixture/Base;:Ljava/lang/Comparable;");
        break;
      case 8:
        Input[1].generic_signature = "<U:Ljava/lang/Object;>Ljava/lang/Object;";
        Replace("T:Lfixture/Base;", "T:TU;");
        break;
      case 9: {
        auto Foreign = Input[1].methods[1];
        Foreign.reference.owner = Input[0].name;
        Input[0].methods = {Foreign};
        break;
      }
      case 10:
        Target.access.erase("static");
        break;
      case 11:
        Target.declared_throws = {"Ljava/lang/Exception;"};
        break;
      case 12:
        Replace(";Lfixture/Context;J)", ";TT;J)");
        break;
      }
      Budget B;
      // A declaration binding grants neither private access nor a valid call
      // kind. Body still rejects the unchanged foreign/static call fixtures.
      if (Mutation == 10) {
        const auto Classes = boundedFixedMap(std::move(Input), Route, B);
        const auto Plans = validateGenericSignatures(Classes, B);
        EXPECT_TRUE(Plans.erased_unused_formal_invocations.contains(Ref));
        EXPECT_TRUE(Plans.erased_generic_invocations.contains(Ref));
        EXPECT_THROW(recoverJava(Classes, B), std::runtime_error);
      } else if (Mutation == 9) {
        EXPECT_THROW(
            recoverJava(boundedFixedMap(std::move(Input), Route, B), B),
            std::runtime_error);
      } else {
        try {
          const auto Plans = validateGenericSignatures(
              boundedFixedMap(std::move(Input), Route, B), B);
          EXPECT_FALSE(Plans.erased_unused_formal_invocations.contains(Ref));
        } catch (const std::runtime_error &) {
          // Independently malformed signatures may fail before any receipt.
        }
      }
    }
}

namespace {
std::vector<Class> nominalResultFactoryClasses(bool Private, bool External,
                                               bool SDK, bool Range, bool Typed,
                                               bool ObjectBound, bool Synthetic,
                                               bool Parent) {
  auto Input =
      boundedFixedFactoryClasses(Private, External, SDK, Range, Typed, 0, "J");
  auto &Target = Input[1].methods[0];
  auto &Call = Private ? Input[1].methods[1] : Input[0].methods[0];
  auto &Carrier = Input[3];
  Carrier.access = {"public", "final"};
  Carrier.superclass = Parent ? Input[5].name : "Ljava/lang/Object;";
  Carrier.generic_signature =
      std::string("<R:") +
      (ObjectBound ? "Ljava/lang/Object;" : "Lfixture/Base;") + ">" +
      *Carrier.superclass;
  Target.reference.parameters = {Input[5].name};
  Target.generic_signature =
      "<T:Lfixture/Base;>(Lfixture/Context;)Lfixture/Factory<TT;>;";
  if (!External)
    Target.registers = 2;
  Call.reference.parameters = Target.reference.parameters;
  if (Typed)
    Call.generic_signature =
        "<X:Lfixture/Base;>(Lfixture/Context;)Lfixture/Factory<TX;>;";
  if (Synthetic)
    Call.access.insert("synthetic");
  const unsigned Start = Range ? 16 : 1;
  Call.registers = Start + 1;
  Call.instructions[0].registers = {Start};
  Call.instructions[0].reference = Target.reference;
  return Input;
}
} // namespace

TEST(MobileDalvikSignature,
     NominalResultOnlyFactoriesKeepKnownBoundsAndRawFacades) {
  for (bool Private : {false, true})
    for (bool External : {false, true}) {
      if (Private && External)
        continue;
      for (bool SDK : {false, true})
        for (bool Range : {false, true})
          for (bool Typed : {false, true})
            for (bool ObjectBound : {false, true})
              for (bool Synthetic : {false, true})
                for (bool Parent : {false, true})
                  for (unsigned Route = 0; Route < 3; ++Route) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{Private, External, SDK, Range, Typed,
                                   ObjectBound, Synthetic, Parent, Route}));
                    auto Input = nominalResultFactoryClasses(
                        Private, External, SDK, Range, Typed, ObjectBound,
                        Synthetic, Parent);
                    const auto Ref = Input[1].methods[0].reference;
                    const auto Original = Input[1].methods[0].generic_signature;
                    Budget B;
                    const auto Classes =
                        boundedFixedMap(std::move(Input), Route, B);
                    const auto Plans = validateGenericSignatures(Classes, B);
                    ASSERT_TRUE(Plans.erased_generic_invocations.contains(Ref));
                    auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    EXPECT_EQ(
                        Classes.at(Ref.owner).methods[0].generic_signature,
                        Original);
                  }
    }
}

TEST(MobileDalvikSignature, NominalResultOnlyFactoriesRejectUnprovedRelations) {
  for (unsigned Mutation = 0; Mutation < 16; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = nominalResultFactoryClasses(true, false, true, false, false,
                                               false, false, false);
      auto &Target = Input[1].methods[0];
      auto &S = *Target.generic_signature;
      auto Replace = [&](const std::string &From, const std::string &To) {
        const auto At = S.find(From);
        ASSERT_NE(At, std::string::npos);
        S.replace(At, From.size(), To);
      };
      switch (Mutation) {
      case 0:
        Replace("Factory<TT;>", "Factory<+TT;>");
        break;
      case 1:
        Replace("Factory<TT;>", "Factory<TQ;>");
        break;
      case 2:
        Input[3].generic_signature = "<R:Lfixture/Context;>Ljava/lang/Object;";
        break;
      case 3:
        Replace("Factory<TT;>", "Factory<-TT;>");
        break;
      case 4:
        Replace("(Lfixture/Context;)", "(TT;)");
        break;
      case 5:
        Input[4].generic_signature = "<Q:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 6:
        Input[3].generic_signature =
            "<R:Lfixture/Base;S:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 7:
        Target.access.insert("synthetic");
        break;
      case 8:
        Target.access.insert("varargs");
        break;
      case 9:
        Target.access.erase("static");
        break;
      case 10:
        Input[3].access = {"private", "final"};
        break;
      case 11:
        Target.instructions.clear();
        Target.registers = Target.code_end = 0;
        break;
      case 12: {
        auto Foreign = Input[1].methods[1];
        Foreign.reference.owner = Input[0].name;
        Input[0].methods = {Foreign};
        break;
      }
      case 13:
        Replace("T:Lfixture/Base;", "T:Lmissing/Base;");
        break;
      case 14:
        Replace("T:Lfixture/Base;", "T:Lfixture/Base;:Ljava/lang/Comparable;");
        break;
      case 15:
        Target.declared_throws = {"Ljava/lang/Exception;"};
        break;
      }
      Budget B;
      EXPECT_THROW(recoverJava(boundedFixedMap(std::move(Input), Route, B), B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> genericPairFactoryClasses(bool SDK, bool InputsSDK,
                                             bool Range, bool Typed, bool Alpha,
                                             bool ReverseFormals, bool Shadow) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.access = {"public"};
    V.superclass = "Ljava/lang/Object;";
    V.source_id = "owned-generic-pair";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}, Literal Value = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    V.literal = std::move(Value);
    return V;
  };
  auto Factory = C("Lfixture/Factory;"), Token = C("Lfixture/Token;"),
       Context = C("Lfixture/Context;"), Left = C("Lfixture/Left;"),
       Right = C("Lfixture/Right;"), Client = C("Lfixture/Client;");
  const std::string A = Alpha ? "First" : "X", B = Alpha ? "Second" : "Y";
  auto Formal = [](const std::string &N) { return N + ":Ljava/lang/Object;"; };
  Factory.generic_signature = "<" + Formal(Shadow ? A : "P") +
                              Formal(Shadow ? B : "Q") + ">Ljava/lang/Object;";
  Token.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  Factory.external = SDK;
  Token.external = Context.external = Left.external = Right.external =
      InputsSDK;
  Method Make;
  Make.reference = {Factory.name,
                    "make",
                    {Token.name, Token.name, Context.name},
                    Factory.name};
  Make.access = {"public", "static"};
  Make.generic_signature =
      "<" + (ReverseFormals ? Formal(B) + Formal(A) : Formal(A) + Formal(B)) +
      ">(Lfixture/Token<T" + A + ";>;Lfixture/Token<T" + B +
      ";>;Lfixture/Context;)Lfixture/Factory<T" + A + ";T" + B + ";>;";
  if (!SDK) {
    Make.registers = 4;
    Make.instructions = {I(0, "const/4", {0}, {}, int64_t{0}),
                         I(1, "return-object", {0})};
    Make.code_end = 2;
  }
  Factory.methods = {Make};
  Method Call;
  Call.reference = {Client.name, "call", Make.reference.parameters,
                    Factory.name};
  Call.access = {"public", "static"};
  if (Typed)
    Call.generic_signature =
        "(Lfixture/Token<Lfixture/Left;>;Lfixture/Token<Lfixture/Right;>;"
        "Lfixture/Context;)Lfixture/Factory<Lfixture/Left;Lfixture/Right;>;";
  Call.registers = 4;
  Call.instructions = {I(0, Range ? "invoke-static/range" : "invoke-static",
                         {1, 2, 3}, Make.reference),
                       I(1, "move-result-object", {0}),
                       I(2, "return-object", {0})};
  Call.code_end = 3;
  Client.methods = {Call};
  return {Factory, Token, Context, Left, Right, Client};
}
} // namespace

TEST(MobileDalvikSignature, GenericPairFactoriesKeepIndependentStaticSeeds) {
  for (bool SDK : {false, true})
    for (bool InputsSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Typed : {false, true})
          for (bool Alpha : {false, true})
            for (bool ReverseFormals : {false, true})
              for (bool Shadow : {false, true})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  auto Input =
                      genericPairFactoryClasses(SDK, InputsSDK, Range, Typed,
                                                Alpha, ReverseFormals, Shadow);
                  const auto Ref = Input[0].methods[0].reference;
                  const auto Signature = Input[0].methods[0].generic_signature;
                  const auto Header = Input[0].generic_signature;
                  if (Route == 2)
                    std::reverse(Input.begin(), Input.end());
                  ClassMap Classes;
                  Budget B;
                  if (Route)
                    Classes = linkClasses(std::move(Input), B);
                  else
                    for (const auto &C : Input)
                      Classes.emplace(C.name, C);
                  const auto Plans = validateGenericSignatures(Classes, B);
                  ASSERT_TRUE(Plans.erased_generic_invocations.contains(Ref));
                  EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature,
                            Signature);
                  EXPECT_EQ(Classes.at(Ref.owner).generic_signature, Header);
                }
}

TEST(MobileDalvikSignature, GenericPairFactoriesRejectUnprovenRelations) {
  for (unsigned Mutation = 0; Mutation < 17; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(Mutation);
      auto Input = genericPairFactoryClasses(false, true, false, false, false,
                                             false, false);
      auto &Factory = Input[0];
      auto &Make = Factory.methods[0];
      auto replace = [&](const std::string &From, const std::string &To) {
        auto &S = *Make.generic_signature;
        const auto At = S.find(From);
        ASSERT_NE(At, std::string::npos);
        S.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        replace("Token<TX;>", "Token<+TX;>");
      else if (Mutation == 1)
        replace("Token<TY;>", "Token<-TY;>");
      else if (Mutation == 2)
        replace("Factory<TX;TY;>", "Factory<TX;TX;>");
      else if (Mutation == 3)
        replace("Factory<TX;TY;>", "Factory<TY;TX;>");
      else if (Mutation == 4)
        replace("Factory<TX;TY;>", "Factory<+TX;TY;>");
      else if (Mutation == 5)
        Input[1].generic_signature = "<T:Lfixture/Left;>Ljava/lang/Object;";
      else if (Mutation == 6)
        Factory.generic_signature =
            "<P:Ljava/lang/Object;Q:Lfixture/Left;>Ljava/lang/Object;";
      else if (Mutation == 7)
        replace("Y:Ljava/lang/Object;", "Y:Lfixture/Right;");
      else if (Mutation == 8)
        Input[1].access.erase("public");
      else if (Mutation == 9)
        Factory.access.erase("public");
      else if (Mutation == 10)
        Make.access = {"private", "static"};
      else if (Mutation == 11)
        Make.access.insert("varargs");
      else if (Mutation == 12)
        Make.access.insert("synthetic");
      else if (Mutation == 13)
        Make.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 14)
        replace("Token<TY;>", "Token<TX;>");
      else if (Mutation == 15)
        Input[2].generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
      else {
        Factory.interfaces = {Input[3].name};
        Input[3].access.insert("interface");
        Input[3].access.insert("abstract");
      }
      if (Route == 2)
        std::reverse(Input.begin(), Input.end());
      try {
        Budget B;
        ClassMap Classes;
        if (Route)
          Classes = linkClasses(Input, B);
        else
          for (const auto &C : Input)
            Classes.emplace(C.name, C);
        const auto Plans = validateGenericSignatures(Classes, B);
        EXPECT_FALSE(Plans.erased_generic_invocations.contains(Make.reference));
      } catch (const Error &) {
        // Malformed complete declarations may fail before call admission.
      }
    }
}

namespace {
std::vector<Class> packageCheckedCarrierClasses(bool GenericOwner, bool Edge,
                                                bool SDK, bool Range,
                                                bool Alpha, bool Multiple,
                                                bool Typed) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.access = {"public", "abstract"};
    V.superclass = "Ljava/lang/Object;";
    V.source_id = "owned-package-checked";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}, Literal Value = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    V.literal = std::move(Value);
    return V;
  };
  auto Core = C("Lfixture/Core;"), Task = C("Lfixture/Task;"),
       Failure = C("Lfixture/Failure;"), Other = C("Lfixture/OtherFailure;"),
       Value = C("Lfixture/Value;"), Object = C("Ljava/lang/Object;");
  Object.superclass.reset();
  Object.external = true;
  const std::string V = Alpha ? "Item" : "V";
  Task.access.insert("interface");
  Task.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  Task.external = Value.external = SDK;
  Failure.superclass = Other.superclass = "Ljava/lang/Exception;";
  Failure.external = Other.external = true;
  if (GenericOwner) {
    Core.generic_signature = "<" + V + ":Ljava/lang/Object;>Ljava/lang/Object;";
    if (Edge) {
      Core.generic_signature =
          *Core.generic_signature + "Lfixture/Task<T" + V + ";>;";
      Core.interfaces = {Task.name};
    }
  }
  Method Read;
  Read.reference = {Core.name, "read", {Task.name}, "Ljava/lang/Object;"};
  Read.access = {"static"};
  Read.generic_signature =
      "<" + V + ":Ljava/lang/Object;>(Lfixture/Task<T" + V + ";>;)T" + V + ";";
  Read.declared_throws =
      Multiple ? std::vector<std::string>{Other.name, Failure.name}
               : std::vector<std::string>{Failure.name};
  Read.registers = 2;
  Read.instructions = {I(0, "const/4", {0}, {}, int64_t{0}),
                       I(1, "return-object", {0})};
  Read.code_end = 2;
  Method Call;
  Call.reference = {Core.name, "call", {Task.name}, "Ljava/lang/Object;"};
  Call.access = {"public", "static"};
  Call.declared_throws = Read.declared_throws;
  if (Typed) {
    Call.reference.returns = Value.name;
    Call.generic_signature = "(Lfixture/Task<Lfixture/Value;>;)Lfixture/Value;";
  }
  Call.registers = 2;
  Call.instructions = {I(0, Range ? "invoke-static/range" : "invoke-static",
                         {1}, Read.reference),
                       I(1, "move-result-object", {0})};
  if (Typed)
    Call.instructions.push_back(I(2, "check-cast", {0}, Value.name));
  Call.instructions.push_back(I(Typed ? 3 : 2, "return-object", {0}));
  Call.code_end = Call.instructions.back().pc + 1;
  Core.methods = {Read, Call};
  return {Core, Task, Failure, Other, Value, Object};
}
} // namespace

TEST(MobileDalvikSignature, PackageCheckedCarriersKeepClosedThrowsAndScopes) {
  for (bool GenericOwner : {false, true})
    for (bool Edge : {false, true})
      for (bool SDK : {false, true})
        for (bool Range : {false, true})
          for (bool Alpha : {false, true})
            for (bool Multiple : {false, true})
              for (bool Typed : {false, true})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  auto Input = packageCheckedCarrierClasses(
                      GenericOwner, Edge, SDK, Range, Alpha, Multiple, Typed);
                  const auto Ref = Input[0].methods[0].reference;
                  const auto Throws = Input[0].methods[0].declared_throws;
                  if (Route == 2)
                    std::reverse(Input.begin(), Input.end());
                  Budget B;
                  ClassMap Classes;
                  if (Route)
                    Classes = linkClasses(Input, B);
                  else
                    for (const auto &C : Input)
                      Classes.emplace(C.name, C);
                  const auto Plans = validateGenericSignatures(Classes, B);
                  ASSERT_TRUE(validatedClosedDeclaredThrows(
                      Classes.at(Ref.owner).methods[0], Plans));
                  ASSERT_TRUE(Plans.erased_generic_invocations.contains(Ref))
                      << GenericOwner << Edge << SDK << Range << Alpha
                      << Multiple << Typed << Route << " parameter kind "
                      << int(Plans.methods.at(Ref)
                                 .types.at(Plans.methods.at(Ref).parameters[0])
                                 .kind)
                      << " result owner "
                      << Plans.methods.at(Ref)
                             .types.at(*Plans.methods.at(Ref).result)
                             .variable_owner.value_or("");
                  EXPECT_EQ(Classes.at(Ref.owner).methods[0].declared_throws,
                            Throws);
                }
}

TEST(MobileDalvikSignature,
     PackageCheckedCarriersRejectOpenOrChangedContracts) {
  for (unsigned Mutation = 0; Mutation < 14; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(Mutation);
      auto Input = packageCheckedCarrierClasses(true, true, true, false, false,
                                                false, false);
      auto &Read = Input[0].methods[0];
      if (Mutation == 0)
        Read.generic_signature =
            "<V:Ljava/lang/Object;>(Lfixture/Task<+TV;>;)TV;";
      else if (Mutation == 1)
        Read.generic_signature =
            "<V:Ljava/lang/Object;>(Lfixture/Task<TV;>;)Ljava/lang/Object;";
      else if (Mutation == 2)
        Read.generic_signature = "<V:Lfixture/Value;>(Lfixture/Task<TV;>;)TV;";
      else if (Mutation == 3)
        Read.access.insert("public");
      else if (Mutation == 4)
        Read.access.insert("private");
      else if (Mutation == 5)
        Read.access.insert("synthetic");
      else if (Mutation == 6)
        Read.access.insert("varargs");
      else if (Mutation == 7)
        Read.declared_throws = {"Lfixture/MissingException;"};
      else if (Mutation == 8)
        Read.declared_throws = {Input[4].name};
      else if (Mutation == 9)
        Input[1].access.erase("public");
      else if (Mutation == 10)
        Input[1].generic_signature = "<T:Lfixture/Value;>Ljava/lang/Object;";
      else if (Mutation == 11)
        Read.generic_signature =
            "<U:Ljava/lang/Object;>(Lfixture/Task<TU;>;)TU;";
      else if (Mutation == 12) {
        Read.generic_signature = "<V:Ljava/lang/Object;E:Ljava/lang/"
                                 "Exception;>(Lfixture/Task<TV;>;)TV;^TE;";
      } else
        Input.pop_back();
      const auto Ref = Read.reference;
      if (Route == 2)
        std::reverse(Input.begin(), Input.end());
      try {
        Budget B;
        ClassMap Classes;
        if (Route)
          Classes = linkClasses(Input, B);
        else
          for (const auto &C : Input)
            Classes.emplace(C.name, C);
        const auto Plans = validateGenericSignatures(Classes, B);
        EXPECT_FALSE(Plans.erased_generic_invocations.contains(Ref));
      } catch (const Error &) {
      }
    }
}

namespace {
std::vector<Class> packageFormalIdentityClasses(bool Many, bool Edge, bool SDK,
                                               bool Range, bool Alpha,
                                               bool Typed, bool Shadow) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.access = {"public", "abstract"};
    V.superclass = "Ljava/lang/Object;";
    V.source_id = "owned-package-identity";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    return V;
  };
  auto Core = C("Lfixture/Core;"), Marker = C("Lfixture/Marker;"),
       Value = C("Lfixture/Value;"), Object = C("Ljava/lang/Object;");
  Object.superclass.reset();
  Object.external = true;
  Marker.access.insert("interface");
  Marker.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  Marker.external = Value.external = SDK;
  const std::string T = Alpha ? "Item" : "T";
  const std::string V = Shadow ? T : (Alpha ? "Owner" : "V");
  Core.generic_signature = "<" + V + ":Ljava/lang/Object;" +
                           (Many ? "Other:Ljava/lang/Object;" : "") +
                           ">Ljava/lang/Object;";
  if (Edge) {
    Core.generic_signature =
        *Core.generic_signature + "Lfixture/Marker<T" + V + ";>;";
    Core.interfaces = {Marker.name};
  }
  Method Check;
  Check.reference = {
      Core.name, "check", {"Ljava/lang/Object;"}, "Ljava/lang/Object;"};
  Check.access = {"static"};
  Check.generic_signature =
      "<" + T + ":Ljava/lang/Object;>(T" + T + ";)T" + T + ";";
  Check.registers = 1;
  Check.instructions = {I(0, "return-object", {0})};
  Check.code_end = 1;
  Method Call;
  Call.reference = {Core.name,
                    "call",
                    {"Ljava/lang/Object;"},
                    Typed ? Value.name : "Ljava/lang/Object;"};
  Call.access = {"public", "static"};
  Call.registers = 2;
  Call.instructions = {I(0, Range ? "invoke-static/range" : "invoke-static",
                         {1}, Check.reference),
                       I(1, "move-result-object", {0})};
  if (Typed)
    Call.instructions.push_back(I(2, "check-cast", {0}, Value.name));
  Call.instructions.push_back(I(Typed ? 3 : 2, "return-object", {0}));
  Call.code_end = Call.instructions.back().pc + 1;
  Core.methods = {Check, Call};
  return {Core, Marker, Value, Object};
}
} // namespace

TEST(MobileDalvikSignature, PackageFormalIdentityKeepsIndependentMethodScope) {
  for (bool Many : {false, true})
    for (bool Edge : {false, true})
      for (bool SDK : {false, true})
        for (bool Range : {false, true})
          for (bool Alpha : {false, true})
            for (bool Typed : {false, true})
              for (bool Shadow : {false, true})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  auto Input = packageFormalIdentityClasses(
                      Many, Edge, SDK, Range, Alpha, Typed, Shadow);
                  const auto Ref = Input[0].methods[0].reference;
                  const auto Signature = Input[0].methods[0].generic_signature;
                  const auto Header = Input[0].generic_signature;
                  if (Route == 2)
                    std::reverse(Input.begin(), Input.end());
                  Budget B;
                  ClassMap Classes;
                  if (Route)
                    Classes = linkClasses(Input, B);
                  else
                    for (const auto &C : Input)
                      Classes.emplace(C.name, C);
                  const auto Plans = validateGenericSignatures(Classes, B);
                  ASSERT_TRUE(Plans.erased_generic_invocations.contains(Ref))
                      << Many << Edge << SDK << Range << Alpha << Typed
                      << Shadow << Route;
                  const auto &Shape = Plans.methods.at(Ref);
                  EXPECT_EQ(Shape.types.at(Shape.parameters[0]).variable_owner,
                            Ref.identity());
                  EXPECT_EQ(Shape.types.at(*Shape.result).variable_owner,
                            Ref.identity());
                  EXPECT_TRUE(Shape.source_projections.empty());
                  EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature,
                            Signature);
                  EXPECT_EQ(Classes.at(Ref.owner).generic_signature, Header);
                }
}

TEST(MobileDalvikSignature,
     PackageFormalIdentityRejectsBorrowedOrChangedScope) {
  for (unsigned Mutation = 0; Mutation < 20; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(Mutation);
      auto Input = packageFormalIdentityClasses(false, true, true, false, false,
                                                false, false);
      auto &Check = Input[0].methods[0];
      if (Mutation == 0)
        Check.generic_signature = "<T:Ljava/lang/Object;>(TV;)TT;";
      else if (Mutation == 1)
        Check.generic_signature = "<T:Ljava/lang/Object;>(TT;)TV;";
      else if (Mutation == 2)
        Check.reference.parameters = {"Ljava/lang/String;"};
      else if (Mutation == 3)
        Check.generic_signature =
            "<T:Ljava/lang/Object;>(TT;)Ljava/lang/Object;";
      else if (Mutation == 4)
        Check.generic_signature = "<T:Lfixture/Value;>(TT;)TT;";
      else if (Mutation == 5)
        Check.generic_signature =
            "<T:Ljava/lang/Object;U:Ljava/lang/Object;>(TT;)TT;";
      else if (Mutation == 6)
        Check.generic_signature =
            "<T:Ljava/lang/Object;>(TT;)TT;^Ljava/lang/Exception;";
      else if (Mutation == 7)
        Check.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 8)
        Check.access.insert("public");
      else if (Mutation == 9)
        Check.access.insert("private");
      else if (Mutation == 10)
        Check.access.insert("synthetic");
      else if (Mutation == 11)
        Check.access.insert("varargs");
      else if (Mutation == 12)
        Input[0].external = true;
      else if (Mutation == 13)
        Input.pop_back();
      else if (Mutation == 14) {
        Check.reference.parameters.push_back("I");
        Check.generic_signature = "<T:Ljava/lang/Object;>(TT;I)TT;";
      } else if (Mutation == 15)
        Check.access.clear();
      else if (Mutation == 16)
        Check.generic_signature = "<T:Ljava/lang/Object;>(TT;)TU;";
      else if (Mutation == 17)
        Check.generic_signature = "<T:Lfixture/Missing;>(TT;)TT;";
      else if (Mutation == 18) {
        Check.reference.parameters = {Input[1].name};
        Check.generic_signature =
            "<T:Ljava/lang/Object;>(Lfixture/Marker<TT;>;)TT;";
      } else
        Check.reference.returns = Input[2].name;
      Input[0].methods[1].instructions[0].reference = Check.reference;
      const auto Ref = Check.reference;
      if (Route == 2)
        std::reverse(Input.begin(), Input.end());
      try {
        Budget B;
        ClassMap Classes;
        if (Route)
          Classes = linkClasses(Input, B);
        else
          for (const auto &C : Input)
            Classes.emplace(C.name, C);
        const auto Plans = validateGenericSignatures(Classes, B);
        EXPECT_FALSE(Plans.erased_generic_invocations.contains(Ref));
      } catch (const Error &) {
      }
    }
}

namespace {
std::vector<Class> seededPairFactoryClasses(bool SDK, bool Alpha, bool Reorder,
                                            bool Range, bool Typed,
                                            bool Shadow) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.access = {"public"};
    V.superclass = "Ljava/lang/Object;";
    V.source_id = "owned-seeded-pair";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}, Literal Value = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    V.literal = std::move(Value);
    return V;
  };
  auto Core = C("Lfixture/Core;"), Pair = C("Lfixture/PairValue;"),
       Base = C("Lfixture/Base;"), Object = C("Ljava/lang/Object;");
  Object.superclass.reset();
  Object.external = Base.external = true;
  Pair.external = SDK;
  const std::string A = Alpha ? "Left" : "A", B = Alpha ? "Right" : "B";
  const std::string F = Shadow ? A : "F", S = Shadow ? B : "S";
  Pair.generic_signature = "<" + F + ":Ljava/lang/Object;" + S +
                           ":Ljava/lang/Object;>Ljava/lang/Object;";
  Method Make;
  Make.reference = {Pair.name,
                    "make",
                    {"Ljava/lang/Object;", "Ljava/lang/Object;"},
                    Pair.name};
  Make.access = {"public", "static"};
  Make.generic_signature = "<" + (Reorder ? B : A) + ":Ljava/lang/Object;" +
                           (Reorder ? A : B) + ":Ljava/lang/Object;>(T" + A +
                           ";T" + B + ";)Lfixture/PairValue<T" + A + ";T" + B +
                           ";>;";
  if (!SDK) {
    Make.registers = 3;
    Make.instructions = {I(0, "const/4", {0}, {}, int64_t{0}),
                         I(1, "return-object", {0})};
    Make.code_end = 2;
  }
  Pair.methods = {Make};
  Method Call;
  Call.reference = {Core.name, "call", Make.reference.parameters, Pair.name};
  Call.access = {"public", "static"};
  if (Typed)
    Call.generic_signature =
        "(Ljava/lang/Object;Ljava/lang/Object;)"
        "Lfixture/PairValue<Lfixture/Base;Lfixture/Base;>;";
  Call.registers = 3;
  Call.instructions = {I(0, Range ? "invoke-static/range" : "invoke-static",
                         {1, 2}, Make.reference),
                       I(1, "move-result-object", {0}),
                       I(2, "return-object", {0})};
  Call.code_end = 3;
  Core.methods = {Call};
  return {Core, Pair, Base, Object};
}
} // namespace

TEST(MobileDalvikSignature, SeededPairsKeepIndependentInputAndResultSlots) {
  for (bool SDK : {false, true})
    for (bool Alpha : {false, true})
      for (bool Reorder : {false, true})
        for (bool Range : {false, true})
          for (bool Typed : {false, true})
            for (bool Shadow : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                auto Input = seededPairFactoryClasses(SDK, Alpha, Reorder,
                                                      Range, Typed, Shadow);
                const auto Ref = Input[1].methods[0].reference;
                const auto Header = Input[1].generic_signature;
                if (Route == 2)
                  std::reverse(Input.begin(), Input.end());
                Budget B;
                ClassMap Classes;
                if (Route)
                  Classes = linkClasses(Input, B);
                else
                  for (const auto &C : Input)
                    Classes.emplace(C.name, C);
                const auto Plans = validateGenericSignatures(Classes, B);
                ASSERT_TRUE(Plans.erased_generic_invocations.contains(Ref));
                const auto &Shape = Plans.methods.at(Ref);
                for (size_t I = 0; I < 2; ++I) {
                  const auto &Seed = Shape.types.at(Shape.parameters[I]);
                  const auto &Slot =
                      Shape.types.at(*Shape.types.at(*Shape.result)
                                          .segments[0]
                                          .arguments[I]
                                          .type);
                  EXPECT_EQ(Seed.variable_owner, Ref.identity());
                  EXPECT_EQ(Slot.variable_owner, Ref.identity());
                  EXPECT_EQ(Seed.variable_name, Slot.variable_name);
                }
                EXPECT_TRUE(Shape.source_projections.empty());
                EXPECT_EQ(Classes.at(Ref.owner).generic_signature, Header);
              }
}

TEST(MobileDalvikSignature, SeededPairsRejectBorrowedBoundsAndChangedSlots) {
  for (unsigned Mutation = 0; Mutation < 18; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(Mutation);
      auto Input =
          seededPairFactoryClasses(true, false, false, false, false, false);
      auto &Owner = Input[1];
      auto &Make = Owner.methods[0];
      auto replace = [&](std::string From, std::string To) {
        auto &Text = *Make.generic_signature;
        const auto At = Text.find(From);
        ASSERT_NE(At, std::string::npos);
        Text.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        Make.reference.parameters[0] = "Ljava/lang/String;";
      else if (Mutation == 1)
        Make.reference.returns = "Ljava/lang/Object;";
      else if (Mutation == 2)
        replace("(TA;TB;)", "(TF;TB;)");
      else if (Mutation == 3)
        replace("PairValue<TA;TB;>", "PairValue<TF;TB;>");
      else if (Mutation == 4)
        replace("PairValue<TA;TB;>", "PairValue<+TA;TB;>");
      else if (Mutation == 5)
        replace("PairValue<TA;TB;>", "PairValue<TA;TA;>");
      else if (Mutation == 6)
        replace("(TA;TB;)", "(TA;TA;)");
      else if (Mutation == 7)
        replace("A:Ljava/lang/Object;", "A:Lfixture/Base;");
      else if (Mutation == 8)
        Owner.generic_signature =
            "<F:Lfixture/Base;S:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 9)
        Owner.interfaces = {"Ljava/lang/Runnable;"};
      else if (Mutation == 10)
        Owner.superclass = Input[2].name;
      else if (Mutation == 11)
        Make.access.insert("synthetic");
      else if (Mutation == 12)
        Make.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 13)
        Make.access = {"private", "static"};
      else if (Mutation == 14)
        Make.access.erase("static");
      else if (Mutation == 15)
        Make.access.erase("public");
      else if (Mutation == 16)
        Owner.generic_signature = "<F:Ljava/lang/Object;>Ljava/lang/Object;";
      else
        replace("B:Ljava/lang/Object;", "B:Lfixture/Missing;");
      const auto Ref = Make.reference;
      Input[0].methods[0].instructions[0].reference = Ref;
      if (Route == 2)
        std::reverse(Input.begin(), Input.end());
      try {
        Budget B;
        ClassMap Classes;
        if (Route)
          Classes = linkClasses(Input, B);
        else
          for (const auto &C : Input)
            Classes.emplace(C.name, C);
        EXPECT_FALSE(validateGenericSignatures(Classes, B)
                         .erased_generic_invocations.contains(Ref));
      } catch (const Error &) {
      }
    }
}

namespace {
std::vector<Class> boundArrayConsumerClasses(unsigned Kind, bool SDK,
                                             bool Range, bool Alpha, bool Final,
                                             bool Typed) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.access = {"public"};
    V.superclass = "Ljava/lang/Object;";
    V.source_id = "owned-bound-array";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    return V;
  };
  auto Core = C("Lfixture/Core;"), Consumer = C("Lfixture/Consumer;"),
       Element = C("Lfixture/Element;"), Value = C("Lfixture/Value;"),
       Object = C("Ljava/lang/Object;");
  Object.superclass.reset();
  Object.external = true;
  Consumer.external = Element.external = Value.external = SDK;
  if (Final)
    Consumer.access.insert("final");
  if (Kind == 2) {
    Element.access.insert("interface");
    Element.access.insert("abstract");
    Value.interfaces = {Element.name};
  } else if (Kind == 1)
    Value.superclass = Element.name;
  const std::string Bound = Kind ? Element.name : Object.name;
  const std::string T = Alpha ? "Item" : "T";
  Method Accept;
  Accept.reference = {Consumer.name, "accept", {"[" + Bound, "I"}, "V"};
  Accept.access = {"public"};
  Accept.generic_signature =
      "<" + T + (Kind == 2 ? "::" : ":") + Bound + ">([T" + T + ";I)V";
  if (!SDK) {
    Accept.registers = 3;
    Accept.instructions = {I(0, "return-void", {})};
    Accept.code_end = 1;
  }
  Consumer.methods = {Accept};
  Method Call;
  Call.reference = {Core.name,
                    "call",
                    {Consumer.name, "[" + (Typed ? Value.name : Bound), "I"},
                    "V"};
  Call.access = {"public", "static"};
  Call.registers = 3;
  Call.instructions = {I(0, Range ? "invoke-virtual/range" : "invoke-virtual",
                         {0, 1, 2}, Accept.reference),
                       I(1, "return-void", {})};
  Call.code_end = 2;
  Core.methods = {Call};
  return {Core, Consumer, Element, Value, Object};
}
} // namespace

TEST(MobileDalvikSignature, BoundArrayConsumersKeepFullComponentAndFlag) {
  for (unsigned Kind = 0; Kind < 3; ++Kind)
    for (bool SDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Final : {false, true})
            for (bool Typed : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                auto Input = boundArrayConsumerClasses(Kind, SDK, Range, Alpha,
                                                       Final, Typed);
                const auto Ref = Input[1].methods[0].reference;
                const auto Signature = Input[1].methods[0].generic_signature;
                if (Route == 2)
                  std::reverse(Input.begin(), Input.end());
                Budget B;
                ClassMap Classes;
                if (Route)
                  Classes = linkClasses(Input, B);
                else
                  for (const auto &C : Input)
                    Classes.emplace(C.name, C);
                const auto Plans = validateGenericSignatures(Classes, B);
                ASSERT_TRUE(Plans.erased_generic_invocations.contains(Ref))
                    << Kind << SDK << Range << Alpha << Final << Typed << Route;
                const auto &Shape = Plans.methods.at(Ref);
                const auto &Array = Shape.types.at(Shape.parameters[0]);
                ASSERT_TRUE(Array.element);
                EXPECT_EQ(Array.erasure, Ref.parameters[0]);
                EXPECT_EQ(Shape.types.at(*Array.element).variable_owner,
                          Ref.identity());
                EXPECT_EQ(Shape.types.at(Shape.parameters[1]).erasure, "I");
                EXPECT_TRUE(Shape.source_projections.empty());
                EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature,
                          Signature);
              }
}

TEST(MobileDalvikSignature, BoundArrayConsumersRejectUnprovedSlotsAndFlags) {
  for (unsigned Mutation = 0; Mutation < 19; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(Mutation);
      auto Input =
          boundArrayConsumerClasses(2, true, false, false, false, false);
      auto &Owner = Input[1];
      auto &Accept = Owner.methods[0];
      if (Mutation == 0)
        Accept.generic_signature = "<T::Lfixture/Element;>([[TT;I)V";
      else if (Mutation == 1)
        Accept.generic_signature =
            "<T::Lfixture/Element;>([Lfixture/Element;I)V";
      else if (Mutation == 2)
        Accept.generic_signature = "<T::Lfixture/Element;>([TU;I)V";
      else if (Mutation == 3)
        Accept.reference.parameters[0] = "[Ljava/lang/Object;";
      else if (Mutation == 4)
        Accept.reference.parameters[1] = "J";
      else if (Mutation == 5)
        Accept.generic_signature =
            "<T::Lfixture/Element;U:Ljava/lang/Object;>([TT;I)V";
      else if (Mutation == 6)
        Accept.generic_signature = "<T::Lfixture/Element;>([TT;I)TT;";
      else if (Mutation == 7)
        Accept.access.insert("static");
      else if (Mutation == 8)
        Accept.access = {"private"};
      else if (Mutation == 9)
        Accept.access = {"protected", "final"};
      else if (Mutation == 10)
        Accept.access.insert("synthetic");
      else if (Mutation == 11)
        Accept.access.insert("varargs");
      else if (Mutation == 12)
        Accept.access.insert("native");
      else if (Mutation == 13)
        Accept.access.insert("abstract");
      else if (Mutation == 14)
        Accept.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 15)
        Input[2].access.erase("public");
      else if (Mutation == 16)
        Owner.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 17)
        Input[2].generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
      else
        Owner.access.erase("public");
      const auto Ref = Accept.reference;
      Input[0].methods[0].instructions[0].reference = Ref;
      if (Route == 2)
        std::reverse(Input.begin(), Input.end());
      try {
        Budget B;
        ClassMap Classes;
        if (Route)
          Classes = linkClasses(Input, B);
        else
          for (const auto &C : Input)
            Classes.emplace(C.name, C);
        EXPECT_FALSE(validateGenericSignatures(Classes, B)
                         .erased_generic_invocations.contains(Ref));
      } catch (const Error &) {
      }
    }
}

namespace {
std::vector<Class> collectionArrayBooleanClasses(bool SDK, bool Range,
                                                 bool Alpha, bool Interface,
                                                 bool Super, bool Varargs,
                                                 bool Used) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.access = {"public"};
    V.superclass = "Ljava/lang/Object;";
    V.source_id = "owned-array-carrier-boolean";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    return V;
  };
  auto Core = C("Lfixture/Core;"), Provider = C("Lfixture/Provider;"),
       Carrier = C("Lfixture/Carrier;"), Object = C("Ljava/lang/Object;");
  Object.superclass.reset();
  Object.external = true;
  Provider.external = Carrier.external = SDK;
  Carrier.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  if (Interface)
    Carrier.access = {"public", "abstract", "interface"};
  const std::string T = Alpha ? "Item" : "T";
  Method Transfer;
  Transfer.reference = {
      Provider.name, "transfer", {Carrier.name, "[Ljava/lang/Object;"}, "Z"};
  Transfer.access = {"public", "static"};
  if (Varargs)
    Transfer.access.insert("varargs");
  Transfer.generic_signature =
      "<" + T + ":Ljava/lang/Object;>(Lfixture/Carrier<" + (Super ? "-" : "") +
      "T" + T + ";>;[T" + T + ";)Z";
  if (!SDK) {
    Transfer.registers = 3;
    auto Constant = I(0, "const/4", {0});
    Constant.literal = int64_t(1);
    Transfer.instructions = {Constant, I(1, "return", {0})};
    Transfer.code_end = 2;
  }
  Provider.methods = {Transfer};
  Method Call;
  Call.reference = {Core.name, "call", Transfer.reference.parameters,
                    Used ? "Z" : "V"};
  Call.access = {"public", "static"};
  Call.registers = 3;
  Call.instructions = {I(0, Range ? "invoke-static/range" : "invoke-static",
                         {1, 2}, Transfer.reference)};
  if (Used) {
    Call.instructions.push_back(I(1, "move-result", {0}));
    Call.instructions.push_back(I(2, "return", {0}));
  } else
    Call.instructions.push_back(I(1, "return-void", {}));
  Call.code_end = Call.instructions.size();
  Core.methods = {Call};
  return {Core, Provider, Carrier, Object};
}
} // namespace

TEST(MobileDalvikSignature, CollectionArrayBooleansKeepFixedArityAndResultUse) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Interface : {false, true})
          for (bool Super : {false, true})
            for (bool Varargs : {false, true})
              for (bool Used : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, Range, Alpha, Interface, Super, Varargs,
                                 Used, Route}));
                  auto Input = collectionArrayBooleanClasses(
                      SDK, Range, Alpha, Interface, Super, Varargs, Used);
                  const auto Target = Input[1].methods[0].reference;
                  const auto Original = Input[1].methods[0].generic_signature;
                  if (Route == 2)
                    std::reverse(Input.begin(), Input.end());
                  Budget B;
                  ClassMap Classes;
                  for (const auto &C : Input)
                    Classes.emplace(C.name, C);
                  if (Route)
                    Classes = linkClasses(Input, B);
                  const auto Plans = validateGenericSignatures(Classes, B);
                  EXPECT_TRUE(
                      Plans.erased_generic_invocations.contains(Target));
                  EXPECT_EQ(
                      Classes.at(Target.owner).methods[0].generic_signature,
                      Original);
                  const auto R = recoverJava(Classes, B);
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                }
}

TEST(MobileDalvikSignature,
     CollectionArrayBooleansRejectUnprovedSlotsAndWords) {
  for (unsigned Mutation = 0; Mutation < 24; ++Mutation)
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = collectionArrayBooleanClasses(true, false, false, true, true,
                                                 true, true);
      auto Element = Input.back();
      Element.name = "Lfixture/Element;";
      Element.superclass = "Ljava/lang/Object;";
      Input.push_back(Element);
      auto &M = Input[1].methods[0];
      auto &Call = Input[0].methods[0];
      switch (Mutation) {
      case 0:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>(Lfixture/Carrier<-TT;>;[TU;)Z";
        break;
      case 1:
        M.generic_signature =
            "<T:Lfixture/Element;>(Lfixture/Carrier<-TT;>;[TT;)Z";
        M.reference.parameters[1] = Call.reference.parameters[1] =
            "[Lfixture/Element;";
        break;
      case 2:
        M.generic_signature =
            "<T:Ljava/lang/Object;>(Lfixture/Carrier<-TT;>;[[TT;)Z";
        M.reference.parameters[1] = Call.reference.parameters[1] =
            "[[Ljava/lang/Object;";
        break;
      case 3:
        M.generic_signature =
            "<T:Ljava/lang/Object;>(Lfixture/Carrier<+TT;>;[TT;)Z";
        break;
      case 4:
        M.generic_signature = "<T:Ljava/lang/Object;>(Lfixture/Carrier;[TT;)Z";
        break;
      case 5:
        Input[2].name = "Lfixture/Missing;";
        break;
      case 6:
        Input[2].generic_signature =
            "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 7:
        Input[2].generic_signature = "<T:Lfixture/Element;>Ljava/lang/Object;";
        break;
      case 8:
        M.access.erase("public");
        M.access.insert("private");
        break;
      case 9:
        M.access.erase("static");
        break;
      case 10:
        M.access.insert("native");
        break;
      case 11:
        M.access.insert("abstract");
        break;
      case 12:
        M.access.insert("synthetic");
        break;
      case 13:
        M.declared_throws = {"Ljava/lang/Exception;"};
        break;
      case 14:
        M.generic_signature =
            "<T:Ljava/lang/Object;>(Lfixture/Carrier<-TU;>;[TT;)Z";
        break;
      case 15:
        M.generic_signature =
            "<T:Ljava/lang/Object;>(Lfixture/Carrier<-TT;>;[TT;)I";
        break;
      case 16:
        M.generic_signature = "<T:Ljava/lang/Object;>(Lfixture/"
                              "Carrier<-TT;>;[Ljava/lang/Object;)Z";
        break;
      case 17:
        M.generic_signature = "<T:Ljava/lang/Object;>(Lfixture/Carrier<-Ljava/"
                              "lang/Object;>;[TT;)Z";
        break;
      case 18:
        Input[3].name = "Lfixture/MissingObject;";
        break;
      case 19:
        Call.instructions[0].registers[1] = Call.registers;
        break;
      case 20:
        Call.instructions[0].opcode = "invoke-virtual";
        break;
      case 21:
        Call.instructions[1].opcode = "move-result-object";
        break;
      case 22:
        Call.tries = {{0, 2, {{std::nullopt, 2}}}};
        break;
      case 23:
        Call.instructions[0].opcode = "unsupported-invocation";
        break;
      }
      Call.instructions[0].reference = M.reference;
      if (Route == 2)
        std::reverse(Input.begin(), Input.end());
      Budget B;
      ClassMap Classes;
      for (const auto &C : Input)
        Classes.emplace(C.name, C);
      EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> arrayTypeTokenClasses(bool SDK, bool Range, bool Alpha,
                                         bool Extends, bool Used) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.access = {"public"};
    V.superclass = "Ljava/lang/Object;";
    V.source_id = "owned-array-type-token";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    return V;
  };
  auto Core = C("Lfixture/Core;"), Provider = C("Lfixture/Provider;"),
       Token = C("Lfixture/Token;"), Object = C("Ljava/lang/Object;");
  Object.superclass.reset();
  Object.external = true;
  Provider.external = Token.external = SDK;
  Token.generic_signature = "<V:Ljava/lang/Object;>Ljava/lang/Object;";
  const std::string T = Alpha ? "Output" : "T", U = Alpha ? "Input" : "U";
  Method Copy;
  Copy.reference = {Provider.name,
                    "select",
                    {"[Ljava/lang/Object;", "I", Token.name},
                    "[Ljava/lang/Object;"};
  Copy.access = {"public", "static"};
  Copy.generic_signature = "<" + T + ":Ljava/lang/Object;" + U +
                           ":Ljava/lang/Object;>([T" + U + ";ILfixture/Token<" +
                           (Extends ? "+" : "") + "[T" + T + ";>;)[T" + T + ";";
  if (!SDK) {
    Copy.registers = 3;
    Copy.instructions = {I(0, "return-object", {0})};
    Copy.code_end = 1;
  }
  Provider.methods = {Copy};
  Method Call;
  Call.reference = {Core.name, "call", Copy.reference.parameters,
                    Used ? Copy.reference.returns : "V"};
  Call.access = {"public", "static"};
  Call.registers = 4;
  Call.instructions = {I(0, Range ? "invoke-static/range" : "invoke-static",
                         {1, 2, 3}, Copy.reference)};
  if (Used) {
    Call.instructions.push_back(I(1, "move-result-object", {0}));
    Call.instructions.push_back(I(2, "return-object", {0}));
  } else
    Call.instructions.push_back(I(1, "return-void", {}));
  Call.code_end = Call.instructions.size();
  Core.methods = {Call};
  return {Core, Provider, Token, Object};
}
} // namespace

TEST(MobileDalvikSignature,
     ArrayTypeTokensKeepIndependentComponentsAndResultUse) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Extends : {false, true})
          for (bool Used : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, Extends, Used, Route}));
              auto Input =
                  arrayTypeTokenClasses(SDK, Range, Alpha, Extends, Used);
              const auto Target = Input[1].methods[0].reference;
              const auto Original = Input[1].methods[0].generic_signature;
              if (Route == 2)
                std::reverse(Input.begin(), Input.end());
              Budget B;
              ClassMap Classes;
              for (const auto &C : Input)
                Classes.emplace(C.name, C);
              if (Route)
                Classes = linkClasses(Input, B);
              const auto Plans = validateGenericSignatures(Classes, B);
              EXPECT_TRUE(Plans.erased_generic_invocations.contains(Target));
              EXPECT_EQ(Classes.at(Target.owner).methods[0].generic_signature,
                        Original);
              const auto R = recoverJava(Classes, B);
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikSignature, ArrayTypeTokensRejectLostBindingsAndCallWords) {
  for (unsigned Mutation = 0; Mutation < 30; ++Mutation)
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = arrayTypeTokenClasses(true, false, false, true, true);
      auto Element = Input.back();
      Element.name = "Lfixture/Element;";
      Element.superclass = "Ljava/lang/Object;";
      Input.push_back(Element);
      auto &M = Input[1].methods[0];
      auto &Call = Input[0].methods[0];
      switch (Mutation) {
      case 0:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([TU;ILfixture/Token<+[TV;>;)[TT;";
        break;
      case 1:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([[TU;ILfixture/Token<+[TT;>;)[TT;";
        M.reference.parameters[0] = Call.reference.parameters[0] =
            "[[Ljava/lang/Object;";
        break;
      case 2:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([TU;ILfixture/Token<+[[TT;>;)[[TT;";
        M.reference.returns = Call.reference.returns = "[[Ljava/lang/Object;";
        break;
      case 3:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([TU;ILfixture/Token<-[TT;>;)[TT;";
        break;
      case 4:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([TU;ILfixture/Token<*>;)[TT;";
        break;
      case 5:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([TU;ILfixture/Token;)[TT;";
        break;
      case 6:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([TU;ILfixture/Token<+TT;>;)[TT;";
        break;
      case 7:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([TU;ILfixture/Token<+[TU;>;)[TT;";
        break;
      case 8:
        M.generic_signature =
            "<T:Ljava/lang/Object;U:Ljava/lang/Object;V:Ljava/lang/"
            "Object;>([TU;ILfixture/Token<+[TV;>;)[TT;";
        break;
      case 9:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([TT;ILfixture/Token<+[TT;>;)[TT;";
        break;
      case 10:
        M.generic_signature = "<T:Ljava/lang/Object;U:Lfixture/"
                              "Element;>([TU;ILfixture/Token<+[TT;>;)[TT;";
        M.reference.parameters[0] = Call.reference.parameters[0] =
            "[Lfixture/Element;";
        break;
      case 11:
        Input[2].generic_signature =
            "<V:Ljava/lang/Object;W:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 12:
        Input[2].generic_signature.reset();
        break;
      case 13:
        Input[2].name = "Lfixture/Missing;";
        break;
      case 14:
        Input[2].generic_signature = "<V:Lfixture/Element;>Ljava/lang/Object;";
        break;
      case 15:
        Input[1].access = {"public", "interface", "abstract"};
        break;
      case 16:
        M.access.insert("native");
        break;
      case 17:
        M.access.insert("abstract");
        break;
      case 18:
        M.access.insert("synthetic");
        break;
      case 19:
        M.access.insert("bridge");
        break;
      case 20:
        M.access.insert("varargs");
        break;
      case 21:
        M.access.erase("public");
        M.access.insert("private");
        break;
      case 22:
        M.access.erase("static");
        break;
      case 23:
        M.declared_throws = {"Ljava/lang/Exception;"};
        break;
      case 24:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([TU;JLfixture/Token<+[TT;>;)[TT;";
        M.reference.parameters[1] = Call.reference.parameters[1] = "J";
        break;
      case 25:
        M.generic_signature = "<T:Ljava/lang/Object;U:Ljava/lang/"
                              "Object;>([TU;ILfixture/Token<+[TT;>;)TT;";
        M.reference.returns = Call.reference.returns = "Ljava/lang/Object;";
        break;
      case 26:
        Call.instructions[0].registers[2] = Call.registers;
        break;
      case 27:
        Call.instructions[0].opcode = "invoke-virtual";
        break;
      case 28:
        Call.instructions[1].opcode = "move-result";
        break;
      case 29:
        Call.instructions[0].opcode = "unsupported-invocation";
        break;
      }
      Call.instructions[0].reference = M.reference;
      if (Route == 2)
        std::reverse(Input.begin(), Input.end());
      Budget B;
      ClassMap Classes;
      for (const auto &C : Input)
        Classes.emplace(C.name, C);
      EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> genericParentResultBridgeClasses(bool SDK, bool Open,
                                                    bool Range, bool Alpha) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.superclass = "Ljava/lang/Object;";
    V.access = {"public"};
    V.source_id = "owned-result-bridge.smali";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    return V;
  };
  auto Parent = C("Lfixture/Parent;"), Child = C("Lfixture/Child;"),
       Object = C("Ljava/lang/Object;"), Marker = C("Ljava/lang/Cloneable;"),
       Failure = C("Ljava/lang/CloneNotSupportedException;"),
       String = C("Ljava/lang/String;");
  Object.superclass.reset();
  Object.external = Marker.external = Failure.external = String.external = true;
  Failure.superclass = "Ljava/lang/Exception;";
  Marker.access = {"public", "abstract", "interface"};
  const std::string K = Alpha ? "Key" : "K", V = Alpha ? "Value" : "V",
                    U = Alpha ? "Result" : "U";
  Parent.generic_signature =
      "<" + K + ":Ljava/lang/Object;" + V +
      ":Ljava/lang/Object;>Ljava/lang/Object;Ljava/lang/Cloneable;";
  Parent.interfaces = {Marker.name};
  Child.superclass = Parent.name;
  Child.generic_signature = (Open ? "<" + U + ":Ljava/lang/Object;>" : "") +
                            "Lfixture/Parent<Ljava/lang/String;" +
                            (Open ? "T" + U + ";" : "Ljava/lang/Object;") +
                            ">;";
  Method Root;
  Root.reference = {Object.name, "clone", {}, Object.name};
  Root.access = {"protected", "native"};
  Root.declared_throws = {Failure.name};
  Object.methods = {Root};
  const unsigned Frame = Range ? 301 : 2;
  auto Typed = [&](const Class &Owner, const Method &Super) {
    Method M;
    M.reference = {Owner.name, "clone", {}, Owner.name};
    M.access = {"public"};
    M.declared_throws = {Failure.name};
    M.registers = Frame;
    M.instructions = {I(0, Range ? "invoke-super/range" : "invoke-super",
                        {Frame - 1}, Super.reference),
                      I(3, "move-result-object", {0}),
                      I(4, "check-cast", {0}, Owner.name),
                      I(6, "return-object", {0})};
    M.code_end = 7;
    return M;
  };
  auto Bridge = [&](const Method &Target, std::string Result) {
    Method M;
    M.reference = Target.reference;
    M.reference.returns = std::move(Result);
    M.access = {"public", "bridge", "synthetic"};
    M.declared_throws = {Failure.name};
    M.registers = Frame;
    M.instructions = {I(0, Range ? "invoke-virtual/range" : "invoke-virtual",
                        {Frame - 1}, Target.reference),
                      I(3, "move-result-object", {0}),
                      I(4, "return-object", {0})};
    M.code_end = 5;
    return M;
  };
  auto P = Typed(Parent, Root);
  P.generic_signature = "()Lfixture/Parent<T" + K + ";T" + V + ";>;";
  Parent.methods = {P, Bridge(P, Object.name)};
  auto Own = Typed(Child, P);
  if (Open)
    Own.generic_signature = "()Lfixture/Child<T" + U + ";>;";
  Child.methods = {Own, Bridge(Own, Parent.name), Bridge(Own, Object.name)};
  Method RootInit;
  RootInit.reference = {Object.name, "<init>", {}, "V"};
  RootInit.access = {"public", "constructor"};
  Object.methods.push_back(RootInit);
  auto Init = RootInit;
  Init.reference.owner = Parent.name;
  Init.registers = 1;
  Init.instructions = {I(0, "invoke-direct", {0}, RootInit.reference),
                       I(3, "return-void", {})};
  Init.code_end = 4;
  Parent.methods.push_back(Init);
  Init.reference.owner = Child.name;
  Init.instructions[0].reference = Parent.methods.back().reference;
  Child.methods.push_back(Init);
  Parent.external = SDK;
  if (SDK)
    for (auto &M : Parent.methods) {
      M.instructions.clear();
      M.registers = M.code_end = 0;
    }
  return {Child, Parent, Object, Marker, Failure, String};
}
} // namespace

TEST(MobileDalvikSignature,
     GenericParentResultBridgesKeepScopedCovarianceAndCheckedThrows) {
  for (bool SDK : {false, true})
    for (bool Open : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (unsigned Route : {0u, 1u, 2u}) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{SDK, Open, Range, Alpha, Route}));
            auto Input =
                genericParentResultBridgeClasses(SDK, Open, Range, Alpha);
            const auto Own = Input[0].methods[0].reference;
            const auto Broad = Input[0].methods[2].reference;
            const auto ParentBroad = Input[1].methods[1].reference;
            if (Route == 2) {
              std::reverse(Input.begin(), Input.end());
              for (auto &Owner : Input)
                std::reverse(Owner.methods.begin(), Owner.methods.end());
            }
            Budget B;
            ClassMap Classes;
            for (const auto &Owner : Input)
              Classes.emplace(Owner.name, Owner);
            if (Route)
              Classes = linkClasses(Input, B);
            const auto Plans = validateGenericSignatures(Classes, B);
            const auto Proof =
                Plans.covariant_bridge_contracts.find({Broad, ParentBroad});
            ASSERT_NE(Proof, Plans.covariant_bridge_contracts.end());
            EXPECT_EQ(Proof->second.target, Own);
            EXPECT_TRUE(
                Plans.covariant_reference_returns.contains({Own, ParentBroad}));
            llvm::json::Object R;
            ASSERT_NO_THROW(R = recoverJava(Classes, B));
            EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
            EXPECT_EQ(R.getInteger("method_count"), SDK ? 4 : 7);
            EXPECT_EQ(R.getInteger("projected_method_count"), SDK ? 2 : 3);
            EXPECT_FALSE(R.getArray("generic_signature_projections"));
            // An inherited-only wider slot belongs to the existing inheritance
            // proof. This composition must not invent an original child entry.
            auto Missing = Classes;
            auto &Members = Missing.at(Own.owner).methods;
            std::erase_if(
                Members, [&](const Method &M) { return M.reference == Broad; });
            Budget MB;
            const auto MissingPlans = validateGenericSignatures(Missing, MB);
            EXPECT_FALSE(MissingPlans.covariant_bridge_contracts.contains(
                {Broad, ParentBroad}));
          }
}

TEST(MobileDalvikSignature,
     GenericParentResultBridgesRejectMissingContractsAndChangedForwarders) {
  for (bool SDK : {false, true})
    for (unsigned Route : {0u, 1u, 2u})
      for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Route, Mutation}));
        auto Input = genericParentResultBridgeClasses(SDK, true, false, false);
        auto &Child = Input[0], &Parent = Input[1];
        auto &Broad = Child.methods[2];
        if (Mutation == 0) {
          Broad.access.erase("public");
          Broad.access.insert("private");
        } else if (Mutation == 1)
          Child.methods.erase(Child.methods.begin() + 1);
        else if (Mutation == 2)
          Broad.access.erase("synthetic");
        else if (Mutation == 3)
          Broad.instructions[0].opcode = "invoke-super";
        else if (Mutation == 4)
          Broad.instructions[0].registers = {0};
        else if (Mutation == 5)
          Broad.instructions[0].reference = Child.methods[1].reference;
        else if (Mutation == 6)
          Child.methods[1].instructions[0].opcode = "invoke-direct";
        else if (Mutation == 7)
          Child.methods[0].declared_throws = {"Ljava/lang/Exception;"};
        else if (Mutation == 8)
          Broad.declared_throws = {"Ljava/lang/Exception;"};
        else if (Mutation == 9)
          Child.generic_signature =
              "<U:Ljava/lang/Object;>Lfixture/Parent<TU;>;";
        else if (Mutation == 10)
          Parent.methods[0].generic_signature =
              "()Lfixture/Parent<TMissing;TV;>;";
        else if (Mutation == 11)
          Parent.methods[0].access.insert("final");
        else if (Mutation == 12)
          Parent.methods[0].access.insert("static");
        else if (Mutation == 13)
          Input[4].superclass = "Lfixture/Missing;";
        else if (Mutation == 14)
          Input[4].superclass = Input[4].name;
        else if (Mutation == 15) {
          Parent.methods[1].access.erase("synthetic");
          Parent.methods[1].access.insert("final");
        } else if (Mutation == 16)
          Broad.instructions.back().registers = {1};
        else
          Broad.code_end = 3;
        if (Route == 2) {
          std::reverse(Input.begin(), Input.end());
          for (auto &Owner : Input)
            std::reverse(Owner.methods.begin(), Owner.methods.end());
        }
        Budget B;
        ClassMap Classes;
        for (const auto &Owner : Input)
          Classes.emplace(Owner.name, Owner);
        EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> closedClassConstructorOverloadClasses(bool SDK, bool Super,
                                                         bool Range, bool Null,
                                                         bool Checked) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.superclass = "Ljava/lang/Object;";
    V.access = {"public"};
    V.source_id = "owned-closed-class-constructor.smali";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    return V;
  };
  auto Parent = C("Lfixture/Parent;"), Child = C("Lfixture/Child;"),
       Input = C("Lfixture/Input;"), Token = C("Lfixture/Token;"),
       Marker = C("Lfixture/Marker;"), Object = C("Ljava/lang/Object;"),
       Failure = C("Ljava/io/IOException;");
  Child.superclass = Parent.name;
  Object.superclass.reset();
  Object.external = Token.external = Marker.external = Failure.external = true;
  Failure.superclass = "Ljava/lang/Exception;";
  Input.external = SDK;
  Marker.access = {"public", "abstract", "interface"};
  Marker.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  Token.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  Input.generic_signature =
      "Ljava/lang/Object;Lfixture/Marker<Lfixture/Input;>;";
  Input.interfaces = {Marker.name};
  Method Init;
  Init.reference = {Object.name, "<init>", {}, "V"};
  Init.access = {"public", "constructor"};
  Object.methods = {Init};
  Method Target = Init;
  Target.reference = {Parent.name, "<init>", {Input.name}, "V"};
  if (Checked)
    Target.declared_throws = {Failure.name};
  Target.registers = 2;
  Target.instructions = {I(0, "invoke-direct", {0}, Init.reference),
                         I(3, "return-void", {})};
  Target.code_end = 4;
  Method Sibling = Target;
  Sibling.reference.parameters = {Token.name};
  Sibling.generic_signature = "(Lfixture/Token<*>;)V";
  Parent.methods = {Target, Sibling};
  Method Call = Target;
  Call.reference = {
      Super ? Child.name : Parent.name, "<init>", {Input.name, "I"}, "V"};
  Call.registers = Range ? 302 : 3;
  const unsigned Self = Call.registers - 3;
  Call.instructions.clear();
  unsigned PC = 0;
  if (Null) {
    auto Zero = I(PC++, "const/4", {Range ? 0u : Self + 1});
    Zero.literal = int64_t(0);
    Call.instructions.push_back(Zero);
    if (Range) {
      Call.instructions.push_back(I(PC, "move-object/16", {Self + 1, 0}));
      PC += 3;
    }
  }
  Call.instructions.push_back(I(PC,
                                Range ? "invoke-direct/range" : "invoke-direct",
                                {Self, Self + 1}, Target.reference));
  PC += 3;
  Call.instructions.push_back(I(PC++, "return-void", {}));
  Call.code_end = PC;
  if (Super)
    Child.methods = {Call};
  else
    Parent.methods.push_back(Call);
  Parent.external = SDK && Super;
  if (Parent.external)
    for (auto &M : Parent.methods) {
      M.registers = M.code_end = 0;
      M.instructions.clear();
    }
  return {Parent, Child, Input, Token, Marker, Object, Failure};
}
} // namespace

TEST(MobileDalvikSignature,
     ClosedClassConstructorViewsKeepGenericInterfacesAndExactOverloads) {
  for (bool SDK : {false, true})
    for (bool Super : {false, true})
      for (bool Range : {false, true})
        for (bool Null : {false, true})
          for (bool Checked : {false, true})
            for (bool Raw : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Super, Range, Null, Checked, Raw, Route}));
                auto Input = closedClassConstructorOverloadClasses(
                    SDK, Super, Range, Null, Checked);
                if (Raw)
                  Input[2].generic_signature.reset();
                if (Route == 2) {
                  std::reverse(Input.begin(), Input.end());
                  for (auto &Owner : Input)
                    std::reverse(Owner.methods.begin(), Owner.methods.end());
                }
                ClassMap Classes;
                for (const auto &Owner : Input)
                  Classes.emplace(Owner.name, Owner);
                Budget B;
                if (Route)
                  Classes = linkClasses(Input, B);
                llvm::json::Object R;
                ASSERT_NO_THROW(R = recoverJava(Classes, B));
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(R.getInteger("projected_method_count").value_or(0),
                          0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
                for (const auto &Owner : Input)
                  for (const auto &M : Owner.methods) {
                    const auto &After = Classes.at(Owner.name).methods;
                    const auto Found = std::find_if(
                        After.begin(), After.end(), [&](const Method &A) {
                          return A.reference == M.reference;
                        });
                    ASSERT_NE(Found, After.end());
                    EXPECT_EQ(Found->generic_signature, M.generic_signature);
                    EXPECT_EQ(Found->declared_throws, M.declared_throws);
                  }
              }
}

TEST(MobileDalvikSignature,
     ClosedClassConstructorViewsRejectOpenOrIncompleteSeparation) {
  for (bool SDK : {false, true})
    for (bool Super : {false, true})
      for (unsigned Route : {0u, 1u, 2u})
        for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{SDK, Super, Route, Mutation}));
          auto Input = closedClassConstructorOverloadClasses(SDK, Super, false,
                                                             true, true);
          auto &Source = Input[2];
          auto &Call = Super ? Input[1].methods[0] : Input[0].methods.back();
          if (Mutation == 0)
            Source.generic_signature =
                "Ljava/lang/Object;Lfixture/Marker<TMissing;>;";
          else if (Mutation == 1)
            Source.generic_signature =
                "<A:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/"
                "Marker<Lfixture/Input<TA;>;>;";
          else if (Mutation == 2) {
            Source.superclass = Input[3].name;
            Source.generic_signature =
                "Lfixture/Token<Ljava/lang/Object;>;Lfixture/Marker<Lfixture/"
                "Input;>;";
          } else if (Mutation == 3) {
            Source.interfaces = {"Lfixture/Missing;"};
            Source.generic_signature =
                "Ljava/lang/Object;Lfixture/Missing<Lfixture/Input;>;";
          } else if (Mutation == 4)
            Input[3].access = {"public", "abstract", "interface"};
          else if (Mutation == 5)
            Source.access.erase("public");
          else if (Mutation == 6)
            Input[0].methods[1].generic_signature =
                "<U:Ljava/lang/Object;>(Lfixture/Token<TU;>;)V";
          else if (Mutation == 7)
            Call.declared_throws.reset();
          else if (Mutation == 8)
            Call.instructions[1].registers[0] = 1;
          else if (Mutation == 9)
            Call.instructions[1].registers[1] = Call.registers;
          else if (Mutation == 10)
            Input.erase(Input.begin() + 2);
          else
            Input.erase(Input.begin() + 3);
          Budget B;
          if (Route == 2) {
            std::reverse(Input.begin(), Input.end());
            for (auto &Owner : Input)
              std::reverse(Owner.methods.begin(), Owner.methods.end());
          }
          ClassMap Classes;
          for (const auto &Owner : Input)
            Classes.emplace(Owner.name, Owner);
          EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                       std::runtime_error);
        }
}

namespace {
std::vector<Class> ancestorConstructorOverloadClasses(bool SDK, bool Super,
                                                      bool Range, bool Null,
                                                      bool Checked) {
  auto Input =
      closedClassConstructorOverloadClasses(SDK, Super, Range, Null, Checked);
  Class Ancestor;
  Ancestor.name = "Lfixture/Ancestor;";
  Ancestor.access = {"public"};
  Ancestor.superclass = "Ljava/lang/Object;";
  Ancestor.interfaces = {Input[4].name};
  Ancestor.external = SDK;
  Ancestor.source_id = "owned-constructor-ancestor.smali";
  Input[2].generic_signature.reset();
  Input[2].interfaces.clear();
  Input[2].superclass = Ancestor.name;
  Input[4].generic_signature.reset();
  Input.push_back(Ancestor);
  return Input;
}
} // namespace

TEST(MobileDalvikSignature,
     ConstructorClassAncestorsKeepNominalSeparationAndCheckedThrows) {
  for (bool SDK : {false, true})
    for (bool Super : {false, true})
      for (bool Range : {false, true})
        for (bool Null : {false, true})
          for (bool Checked : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Super, Range, Null, Checked, Route}));
              auto Input = ancestorConstructorOverloadClasses(SDK, Super, Range,
                                                              Null, Checked);
              if (Route == 2) {
                std::reverse(Input.begin(), Input.end());
                for (auto &Owner : Input)
                  std::reverse(Owner.methods.begin(), Owner.methods.end());
              }
              ClassMap Classes;
              for (const auto &Owner : Input)
                Classes.emplace(Owner.name, Owner);
              Budget B;
              if (Route)
                Classes = linkClasses(Input, B);
              llvm::json::Object R;
              ASSERT_NO_THROW(R = recoverJava(Classes, B));
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikSignature,
     ConstructorClassAncestorsRejectMissingCyclicOrApplicableChains) {
  for (bool SDK : {false, true})
    for (unsigned Route : {0u, 1u, 2u})
      for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Route, Mutation}));
        auto Input =
            ancestorConstructorOverloadClasses(SDK, true, true, false, true);
        auto &Source = Input[2], &Ancestor = Input[7];
        if (Mutation == 0)
          Input.pop_back();
        else if (Mutation == 1)
          Ancestor.superclass = Source.name;
        else if (Mutation == 2)
          Ancestor.superclass = "Lfixture/Missing;";
        else if (Mutation == 3)
          Ancestor.access.insert("interface");
        else if (Mutation == 4)
          Input.erase(Input.begin() + 4);
        else if (Mutation == 5) {
          Ancestor.superclass = Input[3].name;
          Ancestor.generic_signature =
              "Lfixture/Token<Ljava/lang/Object;>;Lfixture/Marker;";
        } else if (Mutation == 6)
          Source.generic_signature = "<T:Ljava/lang/Object;>Lfixture/Ancestor;";
        else if (Mutation == 7)
          Source.access.erase("public");
        else if (Mutation == 8)
          Input[1].methods[0].declared_throws.reset();
        else if (Mutation == 9)
          Input[1].methods[0].instructions[0].registers[0] = 0;
        else if (Mutation == 10)
          Input[0].methods[1].generic_signature =
              "<T:Ljava/lang/Object;>(Lfixture/Token<TT;>;)V";
        else
          Ancestor.generic_signature =
              "Ljava/lang/Object;Lfixture/Marker<Lfixture/Input;>;";
        if (Route == 2) {
          std::reverse(Input.begin(), Input.end());
          for (auto &Owner : Input)
            std::reverse(Owner.methods.begin(), Owner.methods.end());
        }
        ClassMap Classes;
        for (const auto &Owner : Input)
          Classes.emplace(Owner.name, Owner);
        Budget B;
        EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> closedSuperReferenceClasses(bool SDK, bool Range, bool Chain,
                                               unsigned Shape, bool Dropped) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.superclass = "Ljava/lang/Object;";
    V.access = {"public"};
    V.source_id = "owned-closed-super-reference.smali";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    return V;
  };
  auto Base = C("Lfixture/Base;"), Child = C("Lfixture/Child;"),
       Middle = C("Lfixture/Middle;"), Box = C("Lfixture/Box;"),
       Leaf = C("Lfixture/Leaf;"), Object = C("Ljava/lang/Object;"),
       Failure = C("Ljava/io/IOException;");
  Object.superclass.reset();
  Object.external = Box.external = Leaf.external = Failure.external = true;
  Failure.superclass = "Ljava/lang/Exception;";
  Box.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  Base.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  Method Init;
  Init.reference = {Object.name, "<init>", {}, "V"};
  Init.access = {"public", "constructor"};
  Object.methods = {Init};
  auto Constructor = [&](const Class &Owner) {
    auto M = Init;
    M.reference.owner = Owner.name;
    M.registers = 1;
    M.instructions = {I(0, "invoke-direct", {0},
                        MethodRef{*Owner.superclass, "<init>", {}, "V"}),
                      I(3, "return-void", {})};
    M.code_end = 4;
    return M;
  };
  Method Target;
  Target.reference = {
      Base.name, "exchange", {Object.name, Object.name}, Object.name};
  Target.access = {"public"};
  Target.generic_signature = "(TA;TB;)TB;";
  Target.declared_throws = {Failure.name};
  Target.registers = 3;
  Target.instructions = {I(0, "return-object", {2})};
  Target.code_end = 1;
  Base.methods = {Constructor(Base), Target};
  auto OtherArity = Target;
  OtherArity.reference.parameters.push_back("I");
  OtherArity.registers = 4;
  OtherArity.generic_signature = "(TA;TB;I)TB;";
  Base.methods.push_back(OtherArity);
  Middle.superclass = Base.name;
  Middle.generic_signature =
      "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Base<TK;TV;>;";
  Middle.methods = {Constructor(Middle)};
  Child.superclass = Chain ? Middle.name : Base.name;
  const std::string Carrier = "Lfixture/Box<Lfixture/Box<Lfixture/Leaf;>;>;";
  Child.generic_signature =
      Child.superclass->substr(0, Child.superclass->size() - 1) +
      "<Lfixture/Leaf;" + Carrier + ">;";
  auto Call = Target;
  Call.reference = {Child.name,
                    "run",
                    {Shape == 1 ? Object.name : Leaf.name, Box.name},
                    Dropped ? "V" : Box.name};
  Call.generic_signature = "(" + Call.reference.parameters[0] + Carrier + ")" +
                           (Dropped ? "V" : Carrier);
  Call.registers = Range ? 304 : 4;
  const unsigned Self = Call.registers - 3;
  Call.instructions.clear();
  unsigned PC = 0;
  if (Shape == 1) {
    if (Range) {
      Call.instructions.push_back(I(PC, "move-object/from16", {0, Self + 1}));
      PC += 2;
      Call.instructions.push_back(I(PC, "check-cast", {0}, Leaf.name));
      PC += 2;
      Call.instructions.push_back(I(PC, "move-object/16", {Self + 1, 0}));
      PC += 3;
    } else {
      Call.instructions.push_back(I(PC, "check-cast", {Self + 1}, Leaf.name));
      PC += 2;
    }
  } else if (Shape == 2) {
    auto Zero = I(PC++, "const/4", {Range ? 0u : Self + 2});
    Zero.literal = int64_t(0);
    Call.instructions.push_back(Zero);
    if (Range) {
      Call.instructions.push_back(I(PC, "move-object/16", {Self + 2, 0}));
      PC += 3;
    }
  }
  auto Ref = Target.reference;
  Ref.owner = *Child.superclass;
  Call.instructions.push_back(I(PC,
                                Range ? "invoke-super/range" : "invoke-super",
                                {Self, Self + 1, Self + 2}, Ref));
  PC += 3;
  if (!Dropped) {
    Call.instructions.push_back(I(PC++, "move-result-object", {0}));
    Call.instructions.push_back(I(PC, "check-cast", {0}, Box.name));
    PC += 2;
    Call.instructions.push_back(I(PC++, "return-object", {0}));
  } else
    Call.instructions.push_back(I(PC++, "return-void", {}));
  Call.code_end = PC;
  Child.methods = {Constructor(Child), Call};
  if (SDK)
    for (auto *Owner : {&Base, &Middle}) {
      Owner->external = true;
      for (auto &M : Owner->methods) {
        M.registers = M.code_end = 0;
        M.instructions.clear();
      }
    }
  return {Base, Child, Middle, Box, Leaf, Object, Failure};
}
} // namespace

TEST(MobileDalvikSignature,
     ClosedSuperclassReferenceResultsKeepFullArgumentsAndOriginalChecks) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Chain : {false, true})
        for (unsigned Shape = 0; Shape < 3; ++Shape)
          for (bool Dropped : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Chain, Shape, Dropped, Route}));
              auto Input = closedSuperReferenceClasses(SDK, Range, Chain, Shape,
                                                       Dropped);
              const auto Caller = Input[1].methods[1].reference;
              auto Ref = Input[0].methods[1].reference;
              const auto Target = Ref;
              Ref.owner = *Input[1].superclass;
              if (Route == 2) {
                std::reverse(Input.begin(), Input.end());
                for (auto &Owner : Input)
                  std::reverse(Owner.methods.begin(), Owner.methods.end());
              }
              ClassMap Classes;
              for (const auto &Owner : Input)
                Classes.emplace(Owner.name, Owner);
              Budget B;
              if (Route)
                Classes = linkClasses(Input, B);
              const auto Plans = validateGenericSignatures(Classes, B);
              ASSERT_TRUE(Plans.superclass_calls.contains({Caller, Ref}));
              EXPECT_EQ(Plans.superclass_call_declarations.at({Caller, Ref}),
                        Target);
              const auto &Contract = Plans.superclass_calls.at({Caller, Ref});
              EXPECT_EQ(Contract.types.at(Contract.parameters[1]).erasure,
                        "Lfixture/Box;");
              EXPECT_EQ(Contract.types.at(*Contract.result).erasure,
                        "Lfixture/Box;");
              llvm::json::Object R;
              ASSERT_NO_THROW(R = recoverJava(Classes, B));
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikSignature,
     ClosedSuperclassReferenceResultsRejectOpenScopesAndUnprovedWords) {
  for (bool SDK : {false, true})
    for (unsigned Route : {0u, 1u, 2u})
      for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Route, Mutation}));
        auto Input = closedSuperReferenceClasses(SDK, true, true, 0, false);
        auto &Caller = Input[1].methods[1];
        if (Mutation == 0) {
          Caller.reference.parameters[1] = "Ljava/lang/Object;";
          Caller.generic_signature.reset();
        } else if (Mutation == 1)
          Input.erase(Input.begin() + 3);
        else if (Mutation == 2)
          Input[3].generic_signature =
              "<E:Ljava/lang/Object;F:Ljava/lang/Object;>Ljava/lang/Object;";
        else if (Mutation == 3)
          Input[1].generic_signature =
              "<T:Ljava/lang/Object;>Lfixture/Middle<Lfixture/Leaf;Lfixture/"
              "Box<TT;>;>;";
        else if (Mutation == 4)
          Input[0].methods[1].generic_signature =
              "<B:Ljava/lang/Object;>(TA;TB;)TB;";
        else if (Mutation == 5)
          Caller.instructions[0].registers[0] = 0;
        else if (Mutation == 6) {
          auto &Target = Input[0].methods[1];
          Target.access.insert("abstract");
          Target.registers = Target.code_end = 0;
          Target.instructions.clear();
        } else if (Mutation == 7)
          Input[0].methods[1].access.insert("static");
        else if (Mutation == 8)
          Input[0].methods[1].generic_signature =
              "(TA;TB;)TB;^Ljava/lang/Exception;";
        else if (Mutation == 9)
          Caller.instructions[0].registers[2] = Caller.registers;
        else if (Mutation == 10)
          Input[2].superclass = Input[1].name;
        else
          Input[3].access = {"private"};
        if (Route == 2) {
          std::reverse(Input.begin(), Input.end());
          for (auto &Owner : Input)
            std::reverse(Owner.methods.begin(), Owner.methods.end());
        }
        ClassMap Classes;
        for (const auto &Owner : Input)
          Classes.emplace(Owner.name, Owner);
        Budget B;
        EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                     std::runtime_error);
      }
}

TEST(MobileDalvikSignature,
     ClosedSuperclassReferenceResultsRejectCompetingSpecificOverloads) {
  for (bool SDK : {false, true})
    for (bool Near : {false, true})
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Near, Route}));
        auto Input = closedSuperReferenceClasses(SDK, false, true, 0, false);
        auto Sibling = Input[0].methods[1];
        auto &Owner = Input[Near ? 2 : 0];
        Sibling.reference = {Owner.name,
                             "exchange",
                             {Input[4].name, Input[3].name},
                             Input[3].name};
        Sibling.generic_signature.reset();
        if (!SDK) {
          Instruction Zero, Return;
          Zero.opcode = "const/4";
          Zero.registers = {0};
          Zero.literal = int64_t(0);
          Return.pc = 1;
          Return.opcode = "return-object";
          Return.registers = {0};
          Sibling.instructions = {Zero, Return};
          Sibling.registers = 3;
          Sibling.code_end = 2;
        }
        Owner.methods.push_back(Sibling);
        if (Route == 2) {
          std::reverse(Input.begin(), Input.end());
          for (auto &C : Input)
            std::reverse(C.methods.begin(), C.methods.end());
        }
        ClassMap Classes;
        for (const auto &C : Input)
          Classes.emplace(C.name, C);
        Budget B;
        EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> enumStringDefaultClasses(bool SDK, bool Static, bool Range,
                                             bool Alpha) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.superclass = "Ljava/lang/Object;";
    V.access = {"public"};
    V.source_id = "owned-enum-string-default.smali";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    return V;
  };
  auto Provider = C("Lfixture/Provider;"), Caller = C("Lfixture/Caller;"),
       Enum = C("Ljava/lang/Enum;"), Token = C("Ljava/lang/Class;"),
       String = C("Ljava/lang/String;"), Object = C("Ljava/lang/Object;");
  Object.superclass.reset();
  Enum.external = Token.external = String.external = Object.external = true;
  Enum.access.insert("abstract");
  Enum.generic_signature = "<E:Ljava/lang/Enum<TE;>;>Ljava/lang/Object;";
  Token.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  Method Init;
  Init.reference = {Object.name, "<init>", {}, "V"};
  Init.access = {"public", "constructor"};
  Object.methods = {Init};
  auto Constructor = [&](const Class &Owner) {
    auto M = Init;
    M.reference.owner = Owner.name;
    M.registers = 1;
    M.instructions = {I(0, "invoke-direct", {0}, Init.reference),
                      I(3, "return-void", {})};
    M.code_end = 4;
    return M;
  };
  const std::string T = Alpha ? "X" : "E";
  auto Signature = [&](const std::string &Formal, bool Receiver) {
    return "<" + Formal + ":Ljava/lang/Enum<T" + Formal + ";>;>(" +
           (Receiver ? Provider.name : "") + "Ljava/lang/Class<T" + Formal +
           ";>;Ljava/lang/String;T" + Formal + ";)T" + Formal + ";";
  };
  Method Target;
  Target.reference = {
      Provider.name, "select", {Token.name, String.name, Enum.name}, Enum.name};
  Target.access = {"public"};
  if (Static)
    Target.access.insert("static");
  Target.generic_signature = Signature(T, false);
  Target.registers = Static ? 3 : 4;
  Target.instructions = {I(0, "return-object", {Target.registers - 1})};
  Target.code_end = 1;
  Provider.methods = {Constructor(Provider), Target};
  Provider.external = SDK;
  if (SDK)
    for (auto &M : Provider.methods) {
      M.instructions.clear();
      M.registers = M.code_end = 0;
    }
  auto Call = Target;
  Call.reference.owner = Caller.name;
  Call.reference.name = "call";
  Call.access = {"public", "static"};
  if (!Static)
    Call.reference.parameters.insert(Call.reference.parameters.begin(),
                                     Provider.name);
  Call.generic_signature = Signature(Alpha ? "Y" : "R", !Static);
  Call.registers = Range ? 304 : 5;
  std::vector<unsigned> Words;
  for (unsigned R = Call.registers - Call.reference.parameters.size();
       R < Call.registers; ++R)
    Words.push_back(R);
  Call.instructions = {
      I(0,
        Static ? (Range ? "invoke-static/range" : "invoke-static")
               : (Range ? "invoke-virtual/range" : "invoke-virtual"),
        Words, Target.reference),
      I(3, "move-result-object", {0}), I(4, "return-object", {0})};
  Call.code_end = 5;
  Caller.methods = {Constructor(Caller), Call};
  return {Provider, Caller, Enum, Token, String, Object};
}
} // namespace

TEST(MobileDalvikSignature, EnumStringDefaultsRetainTokenFormalAndDispatch) {
  for (bool SDK : {false, true})
    for (bool Static : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (unsigned Route : {0u, 1u, 2u}) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{SDK, Static, Range, Alpha, Route}));
            auto Input = enumStringDefaultClasses(SDK, Static, Range, Alpha);
            const auto Target = Input[0].methods[1].reference;
            if (Route == 2) {
              std::reverse(Input.begin(), Input.end());
              for (auto &C : Input)
                std::reverse(C.methods.begin(), C.methods.end());
            }
            ClassMap Classes;
            for (const auto &C : Input)
              Classes.emplace(C.name, C);
            Budget B;
            if (Route)
              Classes = linkClasses(Input, B);
            const auto Plans = validateGenericSignatures(Classes, B);
            ASSERT_TRUE(Plans.erased_generic_invocations.contains(Target));
            llvm::json::Object R;
            ASSERT_NO_THROW(R = recoverJava(Classes, B));
            EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
            EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
            EXPECT_FALSE(R.getArray("generic_signature_projections"));
          }
}

TEST(MobileDalvikSignature, EnumStringDefaultsRejectChangedScopesAndRoles) {
  for (bool SDK : {false, true})
    for (bool Static : {false, true})
      for (unsigned Route : {0u, 1u, 2u})
        for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{SDK, Static, Route, Mutation}));
          auto Input = enumStringDefaultClasses(SDK, Static, true, false);
          auto &Target = Input[0].methods[1];
          auto &Caller = Input[1].methods[1];
          if (Mutation == 0)
            Target.generic_signature =
                "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/Class<TE;>;Ljava/lang/"
                "String;TMissing;)TE;";
          else if (Mutation == 1)
            Target.generic_signature = "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/"
                                       "Class<+TE;>;Ljava/lang/String;TE;)TE;";
          else if (Mutation == 2)
            Target.generic_signature =
                "<E:Ljava/lang/Enum<TE;>;R:Ljava/lang/Enum<TR;>;>(Ljava/lang/"
                "Class<TE;>;Ljava/lang/String;TE;)TR;";
          else if (Mutation == 3)
            Target.generic_signature =
                "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/Class<TE;>;Ljava/lang/"
                "String;Ljava/lang/Enum;)TE;";
          else if (Mutation == 4)
            Input[4].generic_signature =
                "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          else if (Mutation == 5)
            Input[2].generic_signature =
                "<E:Ljava/lang/Object;>Ljava/lang/Object;";
          else if (Mutation == 6)
            Caller.instructions[0].registers.back() = Caller.registers;
          else if (Mutation == 7) {
            Target.access.erase("public");
            Target.access.insert("protected");
          } else if (Mutation == 8)
            Target.declared_throws = {"Ljava/io/IOException;"};
          else if (Mutation == 9)
            Target.access.insert("varargs");
          else
            Input[2].access.erase("abstract");
          if (Route == 2) {
            std::reverse(Input.begin(), Input.end());
            for (auto &C : Input)
              std::reverse(C.methods.begin(), C.methods.end());
          }
          ClassMap Classes;
          for (const auto &C : Input)
            Classes.emplace(C.name, C);
          Budget B;
          EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                       std::runtime_error);
        }
}

namespace {
std::vector<Class> anchoredSeedNominalClasses(bool SDK, bool Abstract, bool Range,
                                               bool Dropped, bool Alpha) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.superclass = "Ljava/lang/Object;";
    V.access = {"public"};
    V.source_id = "owned-anchored-seed-nominal.smali";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    return V;
  };
  auto Provider = C("Lfixture/Provider;"), Caller = C("Lfixture/Caller;"),
       Token = C("Lfixture/Token;"), Object = C("Ljava/lang/Object;"),
       Failure = C("Ljava/io/IOException;");
  Object.superclass.reset();
  Object.external = Token.external = Failure.external = true;
  Failure.superclass = "Ljava/lang/Exception;";
  Token.access = {"public", "abstract", "interface"};
  Token.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  Method Init;
  Init.reference = {Object.name, "<init>", {}, "V"};
  Init.access = {"public", "constructor"};
  Object.methods = {Init};
  auto Constructor = [&](const Class &Owner) {
    auto M = Init;
    M.reference.owner = Owner.name;
    M.registers = 1;
    M.instructions = {I(0, "invoke-direct", {0}, Init.reference),
                      I(3, "return-void", {})};
    M.code_end = 4;
    return M;
  };
  auto Signature = [&](const std::string &Formal, bool Receiver) {
    return "<" + Formal + ":Ljava/lang/Object;>(" +
           (Receiver ? Provider.name : "") + "Lfixture/Token<T" + Formal +
           ";>;T" + Formal + ";)Lfixture/Provider;";
  };
  Method Target;
  Target.reference = {
      Provider.name, "accept", {Token.name, Object.name}, Provider.name};
  Target.access = {"public"};
  Target.generic_signature = Signature(Alpha ? "X" : "T", false);
  Target.declared_throws = {Failure.name};
  Target.registers = 3;
  Target.instructions = {I(0, "return-object", {0})};
  Target.code_end = 1;
  if (Abstract) {
    Provider.access.insert("abstract");
    Target.access.insert("abstract");
    Target.instructions.clear();
    Target.registers = Target.code_end = 0;
  }
  Provider.methods = {Constructor(Provider), Target};
  Provider.external = SDK;
  if (SDK)
    for (auto &M : Provider.methods) {
      M.instructions.clear();
      M.registers = M.code_end = 0;
    }
  auto Call = Target;
  Call.reference.owner = Caller.name;
  Call.reference.name = "call";
  Call.reference.parameters.insert(Call.reference.parameters.begin(),
                                   Provider.name);
  Call.access = {"public", "static"};
  Call.generic_signature = Signature(Alpha ? "Y" : "R", true);
  Call.registers = Range ? 304 : 5;
  const unsigned Receiver = Call.registers - 3;
  Call.instructions = {I(0, Range ? "invoke-virtual/range" : "invoke-virtual",
                         {Receiver, Receiver + 1, Receiver + 2},
                         Target.reference)};
  if (Dropped) {
    Call.instructions.push_back(I(3, "return-object", {Receiver}));
    Call.code_end = 4;
  } else {
    Call.instructions.push_back(I(3, "move-result-object", {0}));
    Call.instructions.push_back(I(4, "return-object", {0}));
    Call.code_end = 5;
  }
  Caller.methods = {Constructor(Caller), Call};
  return {Provider, Caller, Token, Object, Failure};
}
} // namespace

TEST(MobileDalvikSignature, AnchoredSeedsRetainCheckedNominalResults) {
  for (bool SDK : {false, true})
    for (bool Abstract : {false, true})
      for (bool Range : {false, true})
        for (bool Dropped : {false, true})
          for (bool Alpha : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Abstract, Range, Dropped, Alpha, Route}));
              auto Input = anchoredSeedNominalClasses(SDK, Abstract, Range,
                                                      Dropped, Alpha);
              const auto Target = Input[0].methods[1].reference;
              if (Route == 2) {
                std::reverse(Input.begin(), Input.end());
                for (auto &C : Input)
                  std::reverse(C.methods.begin(), C.methods.end());
              }
              ClassMap Classes;
              for (const auto &C : Input)
                Classes.emplace(C.name, C);
              Budget B;
              if (Route)
                Classes = linkClasses(Input, B);
              const auto Plans = validateGenericSignatures(Classes, B);
              ASSERT_TRUE(Plans.erased_generic_invocations.contains(Target));
              llvm::json::Object R;
              ASSERT_NO_THROW(R = recoverJava(Classes, B));
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikSignature, AnchoredSeedsRejectIncompleteScopeAndThrows) {
  for (bool SDK : {false, true})
    for (unsigned Route : {0u, 1u, 2u})
      for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Route, Mutation}));
        auto Input = anchoredSeedNominalClasses(SDK, true, true, false, false);
        auto &Target = Input[0].methods[1];
        if (Mutation == 0)
          Target.generic_signature = "<T:Ljava/lang/Object;>(Lfixture/"
                                     "Token<TT;>;TMissing;)Lfixture/Provider;";
        else if (Mutation == 1)
          Target.generic_signature = "<T:Ljava/lang/Object;>(Lfixture/"
                                     "Token<+TT;>;TT;)Lfixture/Provider;";
        else if (Mutation == 2)
          Target.generic_signature =
              "<T:Ljava/lang/Object;U:Ljava/lang/Object;>(Lfixture/"
              "Token<TT;>;TU;)Lfixture/Provider;";
        else if (Mutation == 3)
          Target.generic_signature =
              "<T:Ljava/lang/Object;>(Lfixture/Token<TT;>;Ljava/lang/"
              "String;)Lfixture/Provider;";
        else if (Mutation == 4)
          Input.erase(Input.begin() + 2);
        else if (Mutation == 5)
          Input[2].generic_signature =
              "<E:Ljava/lang/Number;>Ljava/lang/Object;";
        else if (Mutation == 6)
          Input[1].methods[1].instructions[0].registers.back() = 304;
        else if (Mutation == 7) {
          Target.access.erase("public");
          Target.access.insert("protected");
        } else if (Mutation == 8)
          Target.declared_throws = {"Lfixture/Missing;"};
        else if (Mutation == 9)
          Target.access.insert("varargs");
        else if (Mutation == 10)
          Target.declared_throws = {"Lfixture/Provider;"};
        else
          Target.generic_signature = "<T:Ljava/lang/Number;>(Lfixture/"
                                     "Token<TT;>;TT;)Lfixture/Provider;";
        if (Route == 2) {
          std::reverse(Input.begin(), Input.end());
          for (auto &C : Input)
            std::reverse(C.methods.begin(), C.methods.end());
        }
        ClassMap Classes;
        for (const auto &C : Input)
          Classes.emplace(C.name, C);
        Budget B;
        EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> statefulAccessMarkerClasses(bool SDK, bool Range,
                                               bool Utility) {
  auto C = [](std::string Name) {
    Class V;
    V.name = std::move(Name);
    V.superclass = "Ljava/lang/Object;";
    V.access = {"public"};
    V.source_id = "owned-stateful-access-marker.smali";
    return V;
  };
  auto I = [](unsigned PC, std::string Name, std::vector<unsigned> Registers,
              Reference Ref = {}) {
    Instruction V;
    V.pc = PC;
    V.opcode = std::move(Name);
    V.registers = std::move(Registers);
    V.reference = std::move(Ref);
    return V;
  };
  auto Core = C("Lfixture/Core;"), Marker = C("Lfixture/Core$1;"),
       Carrier = C("Lfixture/Carrier;"), Value = C("Lfixture/Value;"),
       Object = C("Ljava/lang/Object;");
  Object.superclass.reset();
  Object.external = true;
  Carrier.external = Value.external = SDK;
  Carrier.access = {"public", "interface", "abstract"};
  Carrier.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  Method Init;
  Init.reference = {Object.name, "<init>", {}, "V"};
  Init.access = {"public", "constructor"};
  Object.methods = {Init};
  auto Target = Init;
  Target.reference = {Core.name, "<init>", {Carrier.name, "J", "I"}, "V"};
  Target.access = {"private", "constructor"};
  Target.generic_signature = "(Lfixture/Carrier<Lfixture/Value;>;JI)V";
  Target.registers = 5;
  Target.instructions = {I(0, "invoke-direct", {0}, Init.reference),
                         I(3, "return-void", {})};
  Target.code_end = 4;
  auto Access = Target;
  Access.reference.parameters.push_back(Marker.name);
  Access.access = {"synthetic", "constructor"};
  Access.generic_signature.reset();
  Access.registers = Range ? 304 : 7;
  unsigned Self = Access.registers - 6;
  Access.instructions = {I(0, Range ? "invoke-direct/range" : "invoke-direct",
                           {Self, Self + 1, Self + 2, Self + 3, Self + 4},
                           Target.reference),
                         I(3, "return-void", {})};
  Core.methods = {Target, Access};
  Marker.access = {"synthetic"};
  Marker.inner_class_present = true;
  Marker.inner_access = {"static", "synthetic"};
  Marker.enclosing = Core.name;
  Field State;
  State.reference = {Marker.name, "state", "I"};
  State.access = {"static", "final", "synthetic"};
  Marker.fields = {State};
  Method Clinit;
  Clinit.reference = {Marker.name, "<clinit>", {}, "V"};
  Clinit.access = {"static", "constructor"};
  Clinit.registers = 1;
  auto Constant = I(0, "const/4", {0});
  Constant.literal = int64_t(7);
  Clinit.instructions = {Constant, I(1, "sput", {0}, State.reference),
                         I(3, "return-void", {})};
  Clinit.code_end = 4;
  Marker.methods = {Clinit};
  if (Utility) {
    auto Read = Clinit;
    Read.reference.name = "read";
    Read.reference.returns = "I";
    Read.access = {"public", "static"};
    Read.instructions = {I(0, "sget", {0}, State.reference),
                         I(2, "return", {0})};
    Read.code_end = 3;
    Marker.methods.push_back(Read);
  }
  return {Core, Marker, Carrier, Value, Object};
}
} // namespace

TEST(MobileDalvikSignature,
     StatefulAccessMarkersKeepClosedForwardingAndStaticBodies) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Utility : {false, true})
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(
              ::testing::PrintToString(std::tuple{SDK, Range, Utility, Route}));
          auto Input = statefulAccessMarkerClasses(SDK, Range, Utility);
          const auto Caller = Input[0].methods[1].reference,
                     Target = Input[0].methods[0].reference;
          if (Route == 2) {
            std::reverse(Input.begin(), Input.end());
            for (auto &C : Input)
              std::reverse(C.methods.begin(), C.methods.end());
          }
          ClassMap Classes;
          for (const auto &C : Input)
            Classes.emplace(C.name, C);
          Budget B;
          if (Route)
            Classes = linkClasses(Input, B);
          const auto Plans = validateGenericSignatures(Classes, B);
          ASSERT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
          EXPECT_FALSE(Plans.methods.contains(Caller));
          llvm::json::Object R;
          ASSERT_NO_THROW(R = recoverJava(Classes, B));
          EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
          EXPECT_FALSE(R.getArray("generic_signature_projections"));
          EXPECT_TRUE(R.getBoolean("requires_java_rebuild_postprocessing")
                          .value_or(false));
          const auto *Fields = R.getArray("field_source_bindings");
          ASSERT_TRUE(Fields);
          ASSERT_EQ(Fields->size(), 1u);
          EXPECT_EQ(Fields->front().getAsObject()->getString("binding_kind"),
                    "static-synthetic-field");
        }
}

TEST(MobileDalvikSignature,
     StatefulAccessMarkersRejectInstanceStateAndBrokenWords) {
  for (unsigned Route : {0u, 1u, 2u})
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = statefulAccessMarkerClasses(true, true, true);
      auto &Marker = Input[1];
      auto &Caller = Input[0].methods[1];
      if (Mutation == 0)
        Marker.fields[0].access.erase("static");
      else if (Mutation == 1)
        Marker.methods[1].access.erase("static");
      else if (Mutation == 2)
        Marker.methods[1].reference.name = "<init>";
      else if (Mutation == 3)
        Marker.enclosing = Input[3].name;
      else if (Mutation == 4)
        Caller.instructions[0].registers.back()++;
      else if (Mutation == 5)
        Caller.instructions[0].registers[3]++;
      else if (Mutation == 6)
        Marker.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
      else
        Marker.external = true;
      if (Route == 2) {
        std::reverse(Input.begin(), Input.end());
        for (auto &C : Input)
          std::reverse(C.methods.begin(), C.methods.end());
      }
      ClassMap Classes;
      for (const auto &C : Input)
        Classes.emplace(C.name, C);
      Budget B;
      EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> checkedAccessMarkerClasses(bool SDK, bool High, bool Pair,
                                              bool Stateful, bool Interfaces) {
  auto Input = statefulAccessMarkerClasses(SDK, High || Pair, false);
  auto &Target = Input[0].methods[0];
  if (Pair) {
    Target.reference.parameters.insert(Target.reference.parameters.begin() + 1,
                                        Input[2].name);
    Target.generic_signature = "(Lfixture/Carrier<Lfixture/Value;>;"
                               "Lfixture/Carrier<Lfixture/Value;>;JI)V";
    Target.registers = 6;
  }
  Target.declared_throws = {"Ljava/io/IOException;"};
  auto &Caller = Input[0].methods[1];
  Caller.declared_throws = Target.declared_throws;
  Caller.reference.parameters = Target.reference.parameters;
  Caller.reference.parameters.push_back(Input[1].name);
  Caller.registers = High ? 304 : 9;
  const unsigned Self = Caller.registers - Caller.incomingWords();
  auto &Call = Caller.instructions[0];
  Call.reference = Target.reference;
  Call.opcode = High || Pair ? "invoke-direct/range" : "invoke-direct";
  Call.registers.clear();
  for (unsigned Word = 0; Word < Target.incomingWords(); ++Word)
    Call.registers.push_back(Self + Word);
  if (!Stateful) {
    Input[1].fields.clear();
    Input[1].methods.clear();
  }
  if (Interfaces) {
    Class Contract;
    Contract.name = "Lfixture/Contract;";
    Contract.superclass = "Ljava/lang/Object;";
    Contract.access = {"public", "abstract", "interface"};
    Contract.external = SDK;
    auto Root = Contract;
    Root.name = "Lfixture/Root;";
    Contract.interfaces = {Root.name};
    Input[0].interfaces = {Contract.name};
    Input.push_back(Contract);
    Input.push_back(Root);
  }
  Class IOException;
  IOException.name = "Ljava/io/IOException;";
  IOException.superclass = "Ljava/lang/Exception;";
  IOException.access = {"public"};
  IOException.external = true;
  Input.push_back(IOException);
  return Input;
}
} // namespace

TEST(MobileDalvikSignature, CheckedAccessMarkersKeepClosedThrowsAndBindings) {
  for (bool SDK : {false, true})
    for (bool High : {false, true})
      for (bool Pair : {false, true})
        for (bool Stateful : {false, true})
          for (bool Interfaces : {false, true})
            for (unsigned Route = 0; Route != 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, High, Pair, Stateful, Interfaces, Route}));
              auto Input =
                  checkedAccessMarkerClasses(SDK, High, Pair, Stateful, Interfaces);
              if (Route == 2)
                Input[0].methods[1].declared_throws = {"Ljava/lang/Exception;"};
              const auto Caller = Input[0].methods[1].reference;
              const auto Target = Input[0].methods[0].reference;
              const auto Exceptions = *Input[0].methods[1].declared_throws;
              if (Route == 2) {
                std::reverse(Input.begin(), Input.end());
                for (auto &C : Input)
                  std::reverse(C.methods.begin(), C.methods.end());
              }
              ClassMap Classes;
              for (const auto &C : Input)
                Classes.emplace(C.name, C);
              Budget B;
              if (Route)
                Classes = linkClasses(Input, B);
              const auto Plans = validateGenericSignatures(Classes, B);
              ASSERT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
              ASSERT_TRUE(Plans.constructor_exception_contracts.contains(Caller));
              EXPECT_EQ(Plans.constructor_exception_contracts.at(Caller)
                            .source_declared_throws,
                        Exceptions);
              EXPECT_TRUE(Plans.closed_declared_throws.contains(Target));
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikSignature, CheckedAccessMarkersRejectOpenThrowsAndWordDrift) {
  for (unsigned Route = 0; Route != 3; ++Route)
    for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = checkedAccessMarkerClasses(true, true, true, true, true);
      auto &Target = Input[0].methods[0];
      auto &Caller = Input[0].methods[1];
      if (Mutation == 0)
        Target.declared_throws = {"Lfixture/Missing;"};
      else if (Mutation == 1)
        Target.declared_throws = {"Lfixture/Value;"};
      else if (Mutation == 2)
        *Target.generic_signature += "^TUnknown;";
      else if (Mutation == 3)
        Target.generic_signature = "<E:Ljava/lang/Exception;>" +
                                   *Target.generic_signature + "^TE;";
      else if (Mutation == 4)
        Caller.declared_throws.reset();
      else if (Mutation == 5)
        Caller.declared_throws = {"Ljava/lang/RuntimeException;"};
      else if (Mutation == 6)
        Target.generic_signature = "(Lfixture/Carrier<Lfixture/Value;>;JI)V";
      else if (Mutation == 7)
        Caller.instructions[0].registers[4] =
            Caller.instructions[0].registers[3];
      else if (Mutation == 8)
        Input[1].enclosing = Input[3].name;
      else if (Mutation == 9)
        Input.back().superclass = Input[3].name;
      else if (Mutation == 10)
        Input.back().superclass = Input.back().name;
      else if (Mutation == 11)
        Target.declared_throws = {"Ljava/io/IOException;", "Ljava/io/IOException;"};
      else if (Mutation == 12)
        Input[5].interfaces = {"Lfixture/Missing;"};
      else if (Mutation == 13)
        Input[5].generic_signature =
            "<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Root;";
      else if (Mutation == 14)
        Input[6].interfaces = {Input[5].name};
      else if (Mutation == 15)
        Input[5].access = {"public", "abstract"};
      else if (Mutation == 16)
        Input[6].access.erase("public");
      else
        Input[6].superclass.reset();
      if (Route == 2)
        std::reverse(Input.begin(), Input.end());
      ClassMap Classes;
      for (const auto &C : Input)
        Classes.emplace(C.name, C);
      Budget B;
      EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> nestedAccessMarkerClasses(bool SDK, bool High, bool Checked,
                                             bool Stateful) {
  auto Input = checkedAccessMarkerClasses(SDK, High, false, Stateful, true);
  Class Outer;
  Outer.name = Input[0].name;
  Outer.access = {"public"};
  Outer.superclass = "Ljava/lang/Object;";
  Outer.source_id = Input[0].source_id;
  auto &Member = Input[0];
  Member.name = "Lfixture/Core$Entry;";
  Member.access.clear();
  Member.inner_class_present = true;
  Member.inner_name = "Entry";
  Member.inner_access = {"private", "static"};
  Member.enclosing = Outer.name;
  for (auto &Method : Member.methods) {
    Method.reference.owner = Member.name;
    if (!Checked)
      Method.declared_throws.reset();
  }
  Member.methods[1].instructions[0].reference = Member.methods[0].reference;
  Input.push_back(Outer);
  return Input;
}
} // namespace

TEST(MobileDalvikSignature, NestedAccessMarkersKeepExactEnclosingOwner) {
  for (bool SDK : {false, true})
    for (bool High : {false, true})
      for (bool Checked : {false, true})
        for (bool Stateful : {false, true})
          for (unsigned Route : {0u, 1u, 2u}) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{SDK, High, Checked, Stateful, Route}));
            auto Input = nestedAccessMarkerClasses(SDK, High, Checked, Stateful);
            const auto Caller = Input[0].methods[1].reference;
            const auto Target = Input[0].methods[0].reference;
            if (Route == 2) {
              std::reverse(Input.begin(), Input.end());
              for (auto &C : Input)
                std::reverse(C.methods.begin(), C.methods.end());
            }
            ClassMap Classes;
            for (const auto &C : Input)
              Classes.emplace(C.name, C);
            Budget B;
            if (Route)
              Classes = linkClasses(Input, B);
            const auto Plans = validateGenericSignatures(Classes, B);
            ASSERT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
            EXPECT_FALSE(Plans.methods.contains(Caller));
            const auto Report = recoverJava(Classes, B);
            EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
            EXPECT_FALSE(Report.getArray("generic_signature_projections"));
          }
}

TEST(MobileDalvikSignature, NestedAccessMarkersRejectForeignOrOpenOwners) {
  for (unsigned Route : {0u, 1u, 2u})
    for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = nestedAccessMarkerClasses(true, true, true, true);
      auto &Member = Input[0];
      auto &Marker = Input[1];
      if (Mutation == 0)
        Input.back().external = true;
      else if (Mutation == 1)
        Input.pop_back();
      else if (Mutation == 2)
        Marker.enclosing = Input[3].name;
      else if (Mutation == 3)
        Member.inner_access.erase("static");
      else if (Mutation == 4)
        Member.inner_class_present = false;
      else if (Mutation == 5)
        Member.inner_name.reset();
      else if (Mutation == 6)
        Member.enclosing = Input[3].name;
      else if (Mutation == 7)
        Member.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Contract;";
      else if (Mutation == 8)
        Marker.external = true;
      else if (Mutation == 9)
        Marker.fields[0].access.erase("static");
      else if (Mutation == 10)
        Member.methods[1].instructions[0].registers.back()++;
      else
        Member.methods[1].declared_throws.reset();
      if (Route == 2) {
        std::reverse(Input.begin(), Input.end());
        for (auto &C : Input)
          std::reverse(C.methods.begin(), C.methods.end());
      }
      ClassMap Classes;
      for (const auto &C : Input)
        Classes.emplace(C.name, C);
      Budget B;
      EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> privateSiblingConstructorClasses(bool SDK, bool High,
                                                    bool Checked, bool Context,
                                                    bool Nested) {
  auto Input = nestedAccessMarkerClasses(SDK, High, Checked, false);
  auto &Value = Input[3];
  Value.name = "Lfixture/Core$Value;";
  Value.external = false;
  Value.access.clear();
  Value.enclosing = Input.back().name;
  Value.inner_class_present = true;
  Value.inner_name = "Value";
  Value.inner_access = {"private", "static"};
  auto &Target = Input[0].methods[0];
  const std::string Applied = Nested
      ? "Lfixture/Carrier<Lfixture/Carrier<Lfixture/Core$Value;>;>;"
      : "Lfixture/Carrier<Lfixture/Core$Value;>;";
  Target.generic_signature = "(" + Applied + "JI)V";
  if (Context) {
    Class C;
    C.name = "Lfixture/Core$Context;";
    C.superclass = "Ljava/lang/Object;";
    C.enclosing = Input.back().name;
    C.inner_class_present = true;
    C.inner_name = "Context";
    C.inner_access = {"private", "static"};
    C.interfaces = {"Lfixture/ContextContract;"};
    C.generic_signature = "Ljava/lang/Object;Lfixture/ContextContract<Ljava/lang/Object;>;";
    C.source_id = Input[0].source_id;
    Target.reference.parameters.insert(Target.reference.parameters.begin(), C.name);
    Target.generic_signature = "(" + C.name + Applied + "JI)V";
    Class Contract;
    Contract.name = C.interfaces.front();
    Contract.access = {"public", "interface", "abstract"};
    Contract.superclass = "Ljava/lang/Object;";
    Contract.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
    Contract.external = SDK;
    Contract.source_id = Input[0].source_id;
    // Finish references into the vector before appending declarations.
    Target.registers = Target.incomingWords();
    auto &Caller = Input[0].methods[1];
    Caller.reference.parameters = Target.reference.parameters;
    Caller.reference.parameters.push_back(Input[1].name);
    Caller.registers = High ? 304 : 12;
    const unsigned Self = Caller.registers - Caller.incomingWords();
    auto &Call = Caller.instructions[0];
    Call.reference = Target.reference;
    Call.opcode = "invoke-direct/range";
    Call.registers.clear();
    for (unsigned W = 0; W < Target.incomingWords(); ++W)
      Call.registers.push_back(Self + W);
    Input.push_back(C);
    Input.push_back(Contract);
  }
  return Input;
}
} // namespace

TEST(MobileDalvikSignature,
     PrivateSiblingConstructorsKeepCompleteOwnedLexicalContracts) {
  for (bool SDK : {false, true})
    for (bool High : {false, true})
      for (bool Checked : {false, true})
        for (bool Context : {false, true})
          for (bool Nested : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, High, Checked, Context, Nested, Route}));
              auto Input = privateSiblingConstructorClasses(SDK, High, Checked,
                                                            Context, Nested);
              const auto Caller = Input[0].methods[1].reference;
              const auto Target = Input[0].methods[0].reference;
              const auto Signature = Input[0].methods[0].generic_signature;
              if (Route == 2) {
                std::reverse(Input.begin(), Input.end());
                for (auto &C : Input)
                  std::reverse(C.methods.begin(), C.methods.end());
              }
              EXPECT_NO_THROW(([&] {
                ClassMap Classes;
                for (const auto &C : Input)
                  Classes.emplace(C.name, C);
                Budget B;
                if (Route)
                  Classes = linkClasses(Input, B);
                const auto Plans = validateGenericSignatures(Classes, B);
                ASSERT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
                EXPECT_FALSE(Plans.methods.contains(Caller));
                if (Checked)
                  EXPECT_TRUE(Plans.constructor_exception_contracts.contains(Caller));
                EXPECT_EQ(Plans.constructor_calls.at({Caller, Target}).parameters.size(),
                          Target.parameters.size());
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
                const auto &Methods = Classes.at(Target.owner).methods;
                const auto Retained = std::find_if(Methods.begin(), Methods.end(),
                    [&](const auto &M) { return M.reference == Target; });
                ASSERT_NE(Retained, Methods.end());
                EXPECT_EQ(Retained->generic_signature, Signature);
              })());
            }
}

TEST(MobileDalvikSignature,
     PrivateSiblingConstructorsRejectForeignOrIncompleteLexicalScopes) {
  for (unsigned Mutation = 0; Mutation < 19; ++Mutation)
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = privateSiblingConstructorClasses(true, true, true, true, true);
      auto &Member = Input[0];
      auto &Value = Input[3];
      auto &Context = Input[Input.size() - 2];
      auto &Outer = Input[Input.size() - 3];
      if (Mutation == 0)
        Value.external = true;
      else if (Mutation == 1)
        Context.external = true;
      else if (Mutation == 2)
        Outer.external = true;
      else if (Mutation == 3)
        Value.enclosing = "Lfixture/Foreign;";
      else if (Mutation == 4)
        Context.enclosing = "Lfixture/Foreign;";
      else if (Mutation == 5)
        Value.inner_access.erase("private");
      else if (Mutation == 6)
        Value.inner_access.erase("static");
      else if (Mutation == 7)
        Context.inner_name.reset();
      else if (Mutation == 8)
        Value.inner_class_present = false;
      else if (Mutation == 9)
        Member.enclosing = "Lfixture/Foreign;";
      else if (Mutation == 10)
        Input.erase(Input.end() - 3);
      else if (Mutation == 11) {
        auto &S = *Member.methods[0].generic_signature;
        const auto At = S.find("Lfixture/Core$Value;");
        S.replace(At, std::string("Lfixture/Core$Value;").size(), "TMissing;");
      } else if (Mutation == 12)
        Value.generic_signature = "<U:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 13)
        Context.generic_signature = "<U:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/ContextContract<Ljava/lang/Object;>;";
      else if (Mutation == 14)
        Input[2].generic_signature = "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 15)
        Member.methods[1].instructions[0].registers[4] =
            Member.methods[1].instructions[0].registers[3];
      else if (Mutation == 16)
        Member.methods[1].declared_throws.reset();
      else if (Mutation == 17)
        Member.methods[0].generic_signature = "<T:Ljava/lang/Object;>" +
                                              *Member.methods[0].generic_signature;
      else {
        Member.inner_access.erase("private");
        Member.inner_access.insert("public");
      }
      if (Route == 2) {
        std::reverse(Input.begin(), Input.end());
        for (auto &C : Input)
          std::reverse(C.methods.begin(), C.methods.end());
      }
      Budget B;
      ClassMap Classes;
      for (const auto &C : Input)
        Classes.emplace(C.name, C);
      EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> seededStarPropertyClasses(unsigned SDK, unsigned Role,
                                             bool Range, bool Dropped,
                                             bool Alpha, bool PropertyClass) {
  auto Input =
      anchoredSeedNominalClasses(SDK & 1, Role == 1, Range, Dropped, Alpha);
  auto &Property = Input[2];
  Property.external = SDK & 2;
  Property.generic_signature =
      "<E:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;";
  if (PropertyClass)
    Property.access = {"public", "abstract"};
  Class Context;
  Context.name = "Ljava/lang/String;";
  Context.superclass = "Ljava/lang/Object;";
  Context.access = {"public", "final"};
  Context.external = true;
  auto Signature = [&](const std::string &Formal, bool Caller) {
    return "<" + Formal + ":Ljava/lang/Object;>(" +
           (Caller ? "Lfixture/Provider;" : "") + "Ljava/lang/String;T" +
           Formal + ";Lfixture/Token<T" + Formal + ";*>;)Lfixture/Provider;";
  };
  auto &Target = Input[0].methods[1];
  Target.reference.parameters = {Context.name, "Ljava/lang/Object;",
                                 Property.name};
  Target.generic_signature = Signature(Alpha ? "X" : "T", false);
  if (Role == 2)
    Target.access.insert("static");
  if (!Input[0].external && Role != 1) {
    Target.registers = Role == 2 ? 3 : 4;
    if (Role == 2) {
      auto Zero = Target.instructions[0];
      Zero.opcode = "const/4";
      Zero.literal = int64_t(0);
      Target.instructions[0].pc = 1;
      Target.instructions.insert(Target.instructions.begin(), std::move(Zero));
      Target.code_end = 2;
    }
  }
  auto &Call = Input[1].methods[1];
  Call.reference.parameters = {Input[0].name, Context.name,
                               "Ljava/lang/Object;", Property.name};
  Call.generic_signature = Signature(Alpha ? "Y" : "R", true);
  Call.registers = Range ? 305 : 6;
  const unsigned Receiver = Call.registers - 4;
  auto &Invoke = Call.instructions[0];
  Invoke.opcode = Role == 2
                      ? (Range ? "invoke-static/range" : "invoke-static")
                      : (Range ? "invoke-virtual/range" : "invoke-virtual");
  Invoke.reference = Target.reference;
  Invoke.registers = {Receiver + 1, Receiver + 2, Receiver + 3};
  if (Role != 2)
    Invoke.registers.insert(Invoke.registers.begin(), Receiver);
  if (Dropped)
    Call.instructions.back().registers = {Receiver};
  Input.push_back(std::move(Context));
  return Input;
}
} // namespace

TEST(MobileDalvikSignature, SeededStarPropertiesRetainScopeAndCheckedResults) {
  for (unsigned SDK : {0u, 1u, 2u, 3u})
    for (unsigned Role : {0u, 1u, 2u})
      for (bool Range : {false, true})
        for (bool Dropped : {false, true})
          for (bool Alpha : {false, true})
            for (bool PropertyClass : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Role, Range, Dropped, Alpha, PropertyClass, Route}));
                auto Input = seededStarPropertyClasses(
                    SDK, Role, Range, Dropped, Alpha, PropertyClass);
                const auto Target = Input[0].methods[1].reference;
                if (Route == 2) {
                  std::reverse(Input.begin(), Input.end());
                  for (auto &C : Input)
                    std::reverse(C.methods.begin(), C.methods.end());
                }
                ClassMap Classes;
                for (const auto &C : Input)
                  Classes.emplace(C.name, C);
                Budget B;
                if (Route)
                  Classes = linkClasses(Input, B);
                const auto Plans = validateGenericSignatures(Classes, B);
                ASSERT_TRUE(Plans.erased_generic_invocations.contains(Target));
                llvm::json::Object R;
                ASSERT_NO_THROW(R = recoverJava(Classes, B));
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(R.getInteger("projected_method_count").value_or(0),
                          0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
              }
}

TEST(MobileDalvikSignature, SeededStarPropertiesRejectUnprovedSlotsAndRoles) {
  for (unsigned SDK : {0u, 3u})
    for (unsigned Route : {0u, 1u, 2u})
      for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Route, Mutation}));
        auto Input =
            seededStarPropertyClasses(SDK, 1, true, false, false, false);
        auto &Target = Input[0].methods[1];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &Signature = *Target.generic_signature;
          const auto Offset = Signature.find(From);
          ASSERT_NE(Offset, std::string::npos);
          Signature.replace(Offset, From.size(), To);
        };
        if (Mutation == 0)
          Replace("Token<TT;*>", "Token<+TT;*>");
        else if (Mutation == 1)
          Replace("Token<TT;*>", "Token<-TT;*>");
        else if (Mutation == 2)
          Replace("Token<TT;*>", "Token<*TT;>");
        else if (Mutation == 3)
          Replace("Token<TT;*>", "Token<TT;+TT;>");
        else if (Mutation == 4)
          Replace("String;TT;", "String;TMissing;");
        else if (Mutation == 5) {
          Replace("<T:", "<U:Ljava/lang/Object;T:");
          Replace("String;TT;", "String;TU;");
        } else if (Mutation == 6)
          Replace("String;TT;", "String;Ljava/lang/String;");
        else if (Mutation == 7)
          Input.erase(Input.begin() + 2);
        else if (Mutation == 8)
          Input[2].generic_signature =
              "<E:Ljava/lang/Object;V:Ljava/lang/Number;>Ljava/lang/Object;";
        else if (Mutation == 9)
          Input[2].generic_signature =
              "<E:Ljava/lang/Object;>Ljava/lang/Object;";
        else if (Mutation == 10)
          Input[1].methods[1].instructions[0].registers.back() = 305;
        else if (Mutation == 11) {
          Target.access.erase("public");
          Target.access.insert("protected");
        } else if (Mutation == 12)
          Target.declared_throws = {"Lfixture/Missing;"};
        else if (Mutation == 13)
          Target.declared_throws = {"Lfixture/Provider;"};
        else if (Mutation == 14)
          Target.access.insert("varargs");
        else if (Mutation == 15)
          Target.access.insert("native");
        else if (Mutation == 16)
          Replace(")Lfixture/Provider;", ")Ljava/lang/Object;");
        else if (Mutation == 17)
          Replace("String;TT;", "String;Ljava/lang/Object;");
        else if (Mutation == 18)
          Replace("Token<TT;*>", "Token<Ljava/lang/Object;*>");
        else
          *Target.generic_signature += "^TT;";
        if (Route == 2) {
          std::reverse(Input.begin(), Input.end());
          for (auto &C : Input)
            std::reverse(C.methods.begin(), C.methods.end());
        }
        ClassMap Classes;
        for (const auto &C : Input)
          Classes.emplace(C.name, C);
        Budget B;
        EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                     std::runtime_error);
      }
}

TEST(MobileDalvikSignature, DestinationElementMustMatchItsLowerBoundAndToken) {
  const std::string Descriptor = "Lfixture/Sink;";
  for (const auto &[Bound, Token, Valid] :
       std::vector<std::tuple<std::string, std::string, bool>>{
           {"-TE;", "TE;", true},
           {"+TE;", "TE;", false},
           {"-TE;", "*", false},
           {"-TE;", "-TE;", false}}) {
    SCOPED_TRACE(::testing::PrintToString(std::tie(Bound, Token)));
    auto Input = arrayTypeTokenClasses(true, false, false, true, true);
    auto Sink = genericClass();
    Sink.name = Descriptor;
    Sink.access = {"public", "abstract", "interface"};
    Sink.external = true;
    Input.push_back(Sink);
    auto &Provider = Input[1];
    Provider.access.clear();
    auto &M = Provider.methods[0];
    M.reference.parameters = {"[Ljava/lang/Object;", Descriptor, Input[2].name};
    M.reference.returns = Descriptor;
    M.generic_signature =
        "<D::Lfixture/Sink<" + Bound +
        ">;E:Ljava/lang/Object;>([Ljava/lang/Object;TD;Lfixture/Token<" +
        Token + ">;)TD;";
    auto &Call = Input[0].methods[0];
    Call.reference.parameters = M.reference.parameters;
    Call.reference.returns = Descriptor;
    Call.instructions.front().reference = M.reference;
    Budget B;
    auto Classes = linkClasses(Input, B);
    EXPECT_EQ(validateGenericSignatures(Classes, B)
                  .erased_generic_invocations.contains(M.reference),
              Valid);
    if (Valid) {
      EXPECT_EQ(recoverJava(Classes, B).getInteger("unrecovered_method_count"),
                0);
    } else {
      EXPECT_THROW(recoverJava(Classes, B), std::runtime_error);
    }
  }
}

TEST(MobileDalvikSignature, RangeTypeTokenKeepsBothOriginalIntegerEndpoints) {
  for (const std::string End : {"I", "F"}) {
    SCOPED_TRACE(End);
    auto Input = arrayTypeTokenClasses(true, true, false, true, true);
    auto &M = Input[1].methods[0];
    auto &Call = Input[0].methods[0];
    M.reference.parameters.insert(M.reference.parameters.begin() + 2, End);
    auto &Signature = *M.generic_signature;
    Signature.insert(Signature.find(";ILfixture/Token") + 2, End);
    Call.reference.parameters = M.reference.parameters;
    ++Call.registers;
    Call.instructions.front().reference = M.reference;
    Call.instructions.front().registers = {1, 2, 3, 4};
    Budget B;
    auto Classes = linkClasses(Input, B);
    EXPECT_EQ(validateGenericSignatures(Classes, B)
                  .erased_generic_invocations.contains(M.reference),
              End == "I");
  }
}
