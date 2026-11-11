#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>

using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;

namespace {
Instruction op(unsigned PC, std::string Opcode,
               std::vector<unsigned> Registers = {}, Reference Ref = {}) {
  Instruction I;
  I.pc = PC;
  I.opcode = std::move(Opcode);
  I.registers = std::move(Registers);
  I.reference = std::move(Ref);
  return I;
}
Method method(MethodRef Ref, Access Flags, unsigned Registers,
              std::vector<Instruction> Code) {
  Method M;
  M.reference = std::move(Ref);
  M.access = std::move(Flags);
  M.registers = Registers;
  M.instructions = std::move(Code);
  M.code_end = M.instructions.empty() ? 0 : M.instructions.back().pc + 1;
  return M;
}
Class klass(std::string Name, bool SDK = false) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.source_id = "owned-fixture";
  C.external = SDK;
  return C;
}
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Static,
                           bool Other, bool Marker) {
  const std::string Object = "Ljava/lang/Object;", Name = Alpha ? "Seed" : "T";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  auto Base = klass("Lfixture/Base;", SDK);
  Base.generic_signature =
      "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;";
  Base.methods.push_back(method(
      {Base.name, "<init>", {}, "V"}, {"public", "constructor"}, SDK ? 0 : 1,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{
                op(0, "invoke-direct", {0}, Root.methods[0].reference),
                op(1, "return-void")}));
  auto Provider = klass("Lfixture/Provider;", SDK);
  Provider.superclass = Base.name;
  Provider.generic_signature =
      "Lfixture/Base<Ljava/lang/String;Ljava/lang/Object;>;";
  auto Tag = klass("Lfixture/Tag;", SDK);
  Tag.access = {"public", "interface", "abstract"};
  Tag.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  if (Marker) {
    Provider.interfaces = {Tag.name};
    *Provider.generic_signature += "Lfixture/Tag<Ljava/lang/String;>;";
  }
  const std::string Result = Other ? "Lfixture/Returned;" : Provider.name;
  auto Null = op(0, "const/4", {0});
  Null.literal = int64_t(0);
  auto Target =
      method({Provider.name, "accept", {Object}, Result},
             Static ? Access{"public", "static"} : Access{"public"},
             SDK      ? 0
             : Static ? 2
                      : 3,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Null, op(1, "return-object", {0})});
  Target.generic_signature =
      "<" + Name + ":Ljava/lang/Object;>(T" + Name + ";)" + Result;
  Provider.methods.push_back(Target);
  Provider.methods.push_back(
      method({Provider.name, "<init>", {}, "V"}, {"public", "constructor"},
             SDK ? 0 : 1,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{
                       op(0, "invoke-direct", {0}, Base.methods[0].reference),
                       op(1, "return-void")}));
  auto Core = klass("Lfixture/Core;");
  Core.methods.push_back(method(
      {Core.name, "call", {Provider.name, Object}, Result},
      {"public", "static"}, 2,
      {op(0,
          std::string(Static ? "invoke-static" : "invoke-virtual") +
              (Range ? "/range" : ""),
          Static ? std::vector<unsigned>{1} : std::vector<unsigned>{0, 1},
          Target.reference),
       op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = "Ljava/lang/Throwable;";
  return {Provider,
          Core,
          Base,
          Tag,
          Root,
          klass("Ljava/lang/String;", true),
          klass("Lfixture/Returned;", true),
          klass("Ljava/lang/Number;", true),
          Exception,
          klass("Ljava/lang/Throwable;", true)};
}
ClassMap input(std::vector<Class> Classes, unsigned Route, Budget &B) {
  if (Route == 2)
    std::reverse(Classes.begin(), Classes.end());
  if (Route)
    return linkClasses(Classes, B);
  ClassMap Out;
  for (auto &C : Classes)
    Out.emplace(C.name, std::move(C));
  return Out;
}
} // namespace

TEST(MobileDalvikDirectSeedNominal,
     KeepOwnDirectSeedFixedResultClosedOwnerAndOriginalDispatch) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Static : {false, true})
          for (bool Other : {false, true})
            for (bool Marker : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Range, Alpha, Static, Other, Marker, Route}));
                ASSERT_NO_THROW(([&] {
                  Budget B;
                  auto Classes =
                      input(fixture(SDK, Range, Alpha, Static, Other, Marker),
                            Route, B);
                  const auto Target =
                      Classes.at("Lfixture/Provider;").methods[0];
                  const auto Plans = validateGenericSignatures(Classes, B);
                  EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                      Target.reference));
                  const auto R = recoverJava(Classes, B);
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                  EXPECT_EQ(Classes.at(Target.reference.owner)
                                .methods[0]
                                .generic_signature,
                            Target.generic_signature);
                })());
              }
}

TEST(MobileDalvikDirectSeedNominal,
     RejectForeignFormalsMalformedOriginalErasuresRolesAndCallWords) {
  for (bool SDK : {false, true})
    for (bool Static : {false, true})
      for (unsigned Mutation = 0; Mutation < 16; ++Mutation)
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{SDK, Static, Mutation, Route}));
          auto Classes = fixture(SDK, false, false, Static, false, true);
          auto &Provider = Classes[0];
          auto &Target = Provider.methods[0], &Caller = Classes[1].methods[0];
          switch (Mutation) {
          case 0:
            Target.generic_signature =
                "<T:Ljava/lang/Object;>(TMissing;)Lfixture/Provider;";
            break;
          case 1:
            Target.generic_signature =
                "<T:Ljava/lang/Number;>(TT;)Lfixture/Provider;";
            break;
          case 2:
            Target.generic_signature =
                "<T:Ljava/lang/Object;>(Ljava/lang/String;)Lfixture/Provider;";
            break;
          case 3:
            Target.generic_signature =
                "<T:Ljava/lang/Object;>(TT;)Ljava/lang/Object;";
            break;
          case 4:
            Provider.generic_signature =
                "<C:Ljava/lang/Object;>Lfixture/Base<TMissing;Ljava/"
                "lang/Object;>;Lfixture/Tag<Ljava/lang/String;>;";
            break;
          case 5:
            Target.access.erase("public");
            Target.access.insert("private");
            break;
          case 6:
            Target.access.insert("native");
            break;
          case 7:
            Target.access.insert("abstract");
            break;
          case 8:
            Target.access.insert("bridge");
            break;
          case 9:
            Caller.instructions[0].opcode =
                Static ? "invoke-virtual" : "invoke-static";
            break;
          case 10:
            Target.access.insert("varargs");
            break;
          case 11:
            Target.declared_throws =
                std::vector<std::string>{"Ljava/lang/Exception;"};
            break;
          case 12:
            Caller.instructions[0].registers.pop_back();
            break;
          case 13:
            Caller.instructions[0].registers.back() = 2;
            break;
          case 14:
            Caller.instructions[1].opcode = "move-result";
            break;
          case 15:
            Target.generic_signature =
                "<T:Ljava/lang/Object;>(TT;)Lfixture/Provider<TT;>;";
            break;
          }
          if (Mutation == 11) {
            const auto Before = Target;
            ASSERT_NO_THROW(([&] {
              Budget B;
              const auto Linked = input(Classes, Route, B);
              const auto Plans = validateGenericSignatures(Linked, B);
              EXPECT_TRUE(
                  Plans.erased_generic_invocations.contains(Before.reference));
              EXPECT_TRUE(validatedClosedDeclaredThrows(Before, Plans));
              const auto Report = recoverJava(Linked, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(Report.getInteger("projected_method_count").value_or(0),
                        0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              const auto &After = Linked.at(Before.reference.owner).methods[0];
              EXPECT_EQ(After.generic_signature, Before.generic_signature);
              EXPECT_EQ(After.declared_throws, Before.declared_throws);
              EXPECT_EQ(After.access, Before.access);
            })());
          } else {
            EXPECT_THROW(([&] {
                           Budget B;
                           recoverJava(input(Classes, Route, B), B);
                         }()),
                         std::runtime_error);
          }
        }
}

// Direct method seeds retain independent primitive values and word widths.
namespace {
std::vector<Class> primitiveSeedFixture(bool SDK, bool Range, bool Static,
                                        bool Other, unsigned Profile,
                                        unsigned Position, unsigned Depth) {
  auto Classes = fixture(SDK, Range, Position != 0, Static, Other, true);
  auto &Provider = Classes[0];
  const std::string Name = Position ? "Seed" : "T";
  const std::vector<std::vector<std::string>> Profiles{
      {"Z", "Z"}, {"J", "Z"}, {"D", "F"}};
  auto Parameters = Profiles.at(Profile);
  Parameters.insert(Parameters.begin() + Position, "Ljava/lang/Object;");
  auto &Target = Provider.methods[0];
  Target.reference.parameters = Parameters;
  std::string Signature = "<" + Name + ":Ljava/lang/Object;>(";
  unsigned Words = 0;
  for (unsigned I = 0; I < Parameters.size(); ++I) {
    Signature += I == Position ? "T" + Name + ";" : Parameters[I];
    Words += Parameters[I] == "J" || Parameters[I] == "D" ? 2 : 1;
  }
  Target.generic_signature = Signature + ")" + Target.reference.returns;
  Target.registers = SDK ? 0 : Words + (Static ? 1 : 2);
  auto &Caller = Classes[1].methods[0];
  Caller.reference.parameters = {Provider.name};
  Caller.reference.parameters.insert(Caller.reference.parameters.end(),
                                     Parameters.begin(), Parameters.end());
  Caller.registers = Words + 1;
  Caller.instructions[0].reference = Target.reference;
  Caller.instructions[0].registers.clear();
  for (unsigned I = Static ? 1 : 0; I < Caller.registers; ++I)
    Caller.instructions[0].registers.push_back(I);
  // Capture these values before appending ancestors invalidates references.
  auto Super = Provider.superclass;
  auto Header = Provider.generic_signature;
  auto Interfaces = Provider.interfaces;
  for (unsigned I = 0; I < Depth; ++I) {
    auto Parent = klass("Lfixture/Ancestor" + std::to_string(I) + ";", SDK);
    Parent.superclass = Super;
    Parent.generic_signature = Header;
    Parent.interfaces = Interfaces;
    Parent.methods.push_back(method(
        {Parent.name, "<init>", {}, "V"}, {"public", "constructor"},
        SDK ? 0 : 1,
        SDK ? std::vector<Instruction>{}
            : std::vector<Instruction>{op(0, "invoke-direct", {0},
                                          MethodRef{*Super, "<init>", {}, "V"}),
                                       op(1, "return-void")}));
    Super = Parent.name;
    Header.reset();
    Interfaces.clear();
    Classes.push_back(std::move(Parent));
  }
  if (Depth) {
    Classes[0].superclass = Super;
    Classes[0].generic_signature.reset();
    Classes[0].interfaces.clear();
    if (!SDK)
      Classes[0].methods[1].instructions[0].reference =
          MethodRef{*Super, "<init>", {}, "V"};
  }
  return Classes;
}
} // namespace

TEST(MobileDalvikDirectSeedNominal,
     PrimitiveInputsRetainOwnSeedClosedAncestorsAndOriginalCallWords) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Static : {false, true})
        for (bool Other : {false, true})
          for (unsigned Profile : {0u, 1u, 2u})
            for (unsigned Position : {0u, 1u, 2u})
              for (unsigned Depth : {0u, 2u})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, Range, Static, Other, Profile, Position,
                                 Depth, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes =
                        input(primitiveSeedFixture(SDK, Range, Static, Other,
                                                   Profile, Position, Depth),
                              Route, B);
                    const auto Target =
                        Classes.at("Lfixture/Provider;").methods[0];
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                        Target.reference));
                    const auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    EXPECT_EQ(Classes.at(Target.reference.owner)
                                  .methods[0]
                                  .generic_signature,
                              Target.generic_signature);
                  })());
                }
}

TEST(MobileDalvikDirectSeedNominal,
     PrimitiveSeedScopeErasuresRolesExceptionsAndWideWordsRemainValidated) {
  for (bool SDK : {false, true})
    for (bool Static : {false, true})
      for (unsigned Mutation = 0; Mutation < 24; ++Mutation)
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{SDK, Static, Mutation, Route}));
          auto Classes =
              primitiveSeedFixture(SDK, true, Static, false, 1, 1, 2);
          auto &Provider = Classes[0];
          auto &Target = Provider.methods[0], &Caller = Classes[1].methods[0];
          switch (Mutation) {
          case 0:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;>(JTMissing;Z)Lfixture/Provider;";
            break;
          case 1:
            Target.generic_signature =
                "<Seed:Ljava/lang/Number;>(JTSeed;Z)Lfixture/Provider;";
            break;
          case 2:
            Target.generic_signature = "<Seed:Ljava/lang/Object;>(JLjava/lang/"
                                       "String;Z)Lfixture/Provider;";
            break;
          case 3:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;>(JTSeed;Z)Ljava/lang/Object;";
            break;
          case 4:
            Provider.generic_signature = "Lfixture/Ancestor1<TMissing;>;";
            break;
          case 5:
            Target.access.erase("public");
            Target.access.insert("private");
            break;
          case 6:
            Target.access.insert("native");
            break;
          case 7:
            Target.access.insert("abstract");
            break;
          case 8:
            Target.access.insert("bridge");
            break;
          case 9:
            Caller.instructions[0].opcode =
                Static ? "invoke-virtual/range" : "invoke-static/range";
            break;
          case 10:
            Target.access.insert("varargs");
            break;
          case 11:
            Target.declared_throws =
                std::vector<std::string>{"Ljava/lang/Exception;"};
            break;
          case 12:
            Caller.instructions[0].registers.pop_back();
            break;
          case 13:
            Caller.instructions[0].registers.back() = Caller.registers;
            break;
          case 14:
            Caller.instructions[1].opcode = "move-result";
            break;
          case 15:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;>(JTSeed;Z)Lfixture/Provider<TSeed;>;";
            break;
          case 16:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;>(DTSeed;Z)Lfixture/Provider;";
            break;
          case 17:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;>(JTSeed;I)Lfixture/Provider;";
            break;
          case 18:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;>(TSeed;JZ)Lfixture/Provider;";
            break;
          case 19:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;U:Ljava/lang/Object;>"
                "(JTSeed;Z)Lfixture/Provider;";
            break;
          case 20:
            Target.access.insert("synthetic");
            break;
          case 21:
            Provider.access.erase("public");
            break;
          case 22:
            Target.declared_throws =
                std::vector<std::string>{"Ljava/lang/String;"};
            break;
          case 23:
            Caller.instructions[0].registers[Static ? 1 : 2] = 3;
            break;
          }
          if (Mutation == 11) {
            const auto Before = Target;
            ASSERT_NO_THROW(([&] {
              Budget B;
              const auto Linked = input(Classes, Route, B);
              const auto Plans = validateGenericSignatures(Linked, B);
              EXPECT_TRUE(
                  Plans.erased_generic_invocations.contains(Before.reference));
              EXPECT_TRUE(validatedClosedDeclaredThrows(Before, Plans));
              const auto Report = recoverJava(Linked, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(Report.getInteger("projected_method_count").value_or(0),
                        0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              const auto &After = Linked.at(Before.reference.owner).methods[0];
              EXPECT_EQ(After.generic_signature, Before.generic_signature);
              EXPECT_EQ(After.declared_throws, Before.declared_throws);
              EXPECT_EQ(After.access, Before.access);
            })());
          } else {
            EXPECT_THROW(([&] {
                           Budget B;
                           recoverJava(input(Classes, Route, B), B);
                         }()),
                         std::runtime_error);
          }
        }
}

// Method-owned seeds preserve every original covariant class bridge slot.
namespace {
std::vector<Class> methodSeedBridgeFixture(bool SDK, bool Range, bool Alpha,
                                           bool Scalars, unsigned Depth) {
  auto Classes = Scalars
                     ? primitiveSeedFixture(SDK, Range, false, false, 0, 0, 0)
                     : fixture(SDK, Range, Alpha, false, false, true);
  const auto Parameters = Classes[0].methods[0].reference.parameters;
  const unsigned Words = static_cast<unsigned>(Parameters.size());
  const std::string Tail = Scalars ? "ZZ" : "";
  std::vector<std::string> Ancestors{Classes[0].name};
  for (unsigned I = 0; I < Depth; ++I) {
    const bool External = SDK && I + 1 != Depth;
    auto Child =
        klass(I + 1 == Depth ? "Lfixture/Leaf;" : "Lfixture/Middle;", External);
    Child.superclass = Ancestors.back();
    const std::string Formal = Alpha ? (I ? "Narrow" : "ChildSeed") : "Z";
    const MethodRef Parent{Ancestors.back(), "accept", Parameters,
                           Ancestors.back()};
    const MethodRef Target{Child.name, "accept", Parameters, Child.name};
    std::vector<unsigned> Inputs;
    for (unsigned R = 1; R <= Words + 1; ++R)
      Inputs.push_back(R);
    const auto Suffix = Range ? "/range" : "";
    auto Primary = method(
        Target, {"public"}, External ? 0 : Words + 2,
        External
            ? std::vector<Instruction>{}
            : std::vector<Instruction>{
                  op(0, std::string("invoke-super") + Suffix, Inputs, Parent),
                  op(3, "move-result-object", {0}),
                  op(4, "check-cast", {0}, Child.name),
                  op(6, "return-object", {0})});
    Primary.generic_signature = "<" + Formal + ":Ljava/lang/Object;>(T" +
                                Formal + ";" + Tail + ")" + Child.name;
    Child.methods.push_back(Primary);
    Child.methods.push_back(method(
        {Child.name, "<init>", {}, "V"}, {"public", "constructor"},
        External ? 0 : 1,
        External ? std::vector<Instruction>{}
                 : std::vector<Instruction>{
                       op(0, "invoke-direct", {0},
                          MethodRef{Ancestors.back(), "<init>", {}, "V"}),
                       op(1, "return-void")}));
    for (auto &R : Inputs)
      --R;
    for (const auto &Ancestor : Ancestors)
      Child.methods.push_back(
          method({Child.name, "accept", Parameters, Ancestor},
                 {"public", "bridge", "synthetic"}, External ? 0 : Words + 1,
                 External ? std::vector<Instruction>{}
                          : std::vector<Instruction>{
                                op(0, std::string("invoke-virtual") + Suffix,
                                   Inputs, Target),
                                op(3, "move-result-object", {1}),
                                op(4, "return-object", {1})}));
    Ancestors.push_back(Child.name);
    Classes.push_back(std::move(Child));
  }
  auto &Caller = Classes[1].methods[0];
  Caller.reference.parameters[0] = "Lfixture/Leaf;";
  Caller.reference.returns = "Lfixture/Leaf;";
  Caller.instructions[0].reference = Classes.back().methods[0].reference;
  return Classes;
}
} // namespace

TEST(MobileDalvikDirectSeedNominal,
     MethodSeedsKeepFullScopesAndAllCovariantClassBridgeSlots) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Scalars : {false, true})
          for (unsigned Depth : {1u, 2u})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, Scalars, Depth, Route}));
              ASSERT_NO_THROW(([&] {
                Budget B;
                auto Classes = input(
                    methodSeedBridgeFixture(SDK, Range, Alpha, Scalars, Depth),
                    Route, B);
                const auto Plans = validateGenericSignatures(Classes, B);
                for (const auto &[Name, Owner] : Classes) {
                  if (Owner.external)
                    continue;
                  for (const auto &Bridge : Owner.methods) {
                    if (!Bridge.access.contains("bridge"))
                      continue;
                    auto Required = Bridge.reference;
                    Required.owner = Bridge.reference.returns;
                    const auto Found = Plans.covariant_bridge_contracts.find(
                        {Bridge.reference, Required});
                    ASSERT_NE(Found, Plans.covariant_bridge_contracts.end());
                    EXPECT_EQ(Found->second.target, Owner.methods[0].reference);
                    const auto &Contract = Found->second.contract;
                    ASSERT_EQ(Contract.type_parameters.size(), 1u);
                    EXPECT_EQ(Contract.type_parameters[0].erasure,
                              "Ljava/lang/Object;");
                    EXPECT_EQ(Contract.parameters.size(),
                              Bridge.reference.parameters.size());
                    EXPECT_TRUE(Contract.source_projections.empty());
                    EXPECT_TRUE(Contract.throws_types.empty());
                  }
                }
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(Report.getInteger("projected_method_count"),
                          SDK ? Depth : Depth * (Depth + 1) / 2);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              })());
            }
}

TEST(MobileDalvikDirectSeedNominal,
     MethodSeedBridgesRejectLostScopesOriginalSlotsAndForwardingEffects) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 24; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = methodSeedBridgeFixture(SDK, false, false, true, 2);
        auto &Leaf = Classes.back();
        auto &Target = Leaf.methods[0];
        auto &Bridge = Leaf.methods[2];
        switch (Mutation) {
        case 0:
          Target.generic_signature =
              "<Z:Ljava/lang/Object;>(TMissing;ZZ)Lfixture/Leaf;";
          break;
        case 1:
          Target.generic_signature =
              "<Z:Ljava/lang/Number;>(TZ;ZZ)Lfixture/Leaf;";
          break;
        case 2:
          Target.generic_signature =
              "<Z:Ljava/lang/Object;>(Ljava/lang/Object;ZZ)Lfixture/Leaf;";
          break;
        case 3:
          Target.generic_signature =
              "<Z:Ljava/lang/Object;U:Ljava/lang/Object;>(TZ;ZZ)Lfixture/Leaf;";
          break;
        case 4:
          Target.generic_signature =
              "<Z:Ljava/lang/Object;>(TZ;IZ)Lfixture/Leaf;";
          break;
        case 5:
          Target.generic_signature =
              "<Z:Ljava/lang/Object;>(TZ;ZZ)Ljava/lang/Object;";
          break;
        case 6:
          Target.access.insert("static");
          break;
        case 7:
          Target.access.insert("native");
          break;
        case 8:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 9:
          Classes[0].methods[0].access.insert("final");
          break;
        case 10:
          Bridge.access.erase("synthetic");
          break;
        case 11:
          Leaf.methods.erase(Leaf.methods.begin() + 2);
          break;
        case 12:
          Bridge.instructions[0].opcode = "invoke-static";
          break;
        case 13:
          Bridge.instructions[0].registers[0] = 1;
          break;
        case 14:
          Bridge.instructions[0].registers[1] = 0;
          break;
        case 15:
          std::swap(Bridge.instructions[0].registers[2],
                    Bridge.instructions[0].registers[3]);
          break;
        case 16:
          Bridge.instructions[1].opcode = "move-result";
          break;
        case 17:
          Bridge.instructions[2].registers[0] = 0;
          break;
        case 18: {
          auto Extra = Bridge.instructions[0];
          Extra.pc = 5;
          Bridge.instructions.insert(Bridge.instructions.end() - 1, Extra);
          Bridge.instructions.back().pc = 8;
          Bridge.code_end = 9;
          break;
        }
        case 19:
          Bridge.generic_signature =
              "<Z:Ljava/lang/Object;>(TZ;ZZ)Lfixture/Provider;";
          break;
        case 20:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 21:
          Leaf.generic_signature = "Lfixture/Middle<TMissing;>;";
          break;
        case 22:
          Target.generic_signature =
              "<Z:Ljava/lang/Object;>(TZ;ZZ)Lfixture/Leaf<TZ;>;";
          break;
        case 23:
          std::get<MethodRef>(Bridge.instructions[0].reference).owner =
              "Lfixture/Middle;";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

// Unused method formals retain instance calls and closed declared exceptions.
namespace {
std::vector<Class> unusedScalarFixture(bool SDK, bool Range, bool Static,
                                       bool OwnerFormal, bool Two, bool Throws,
                                       unsigned Layout) {
  const std::string Object = "Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Base = klass("Lfixture/Base;", SDK);
  Base.generic_signature =
      "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Marker = klass("Lfixture/Marker;", SDK);
  Marker.access = {"public", "interface", "abstract"};
  Marker.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Closed = klass("Lfixture/Closed;", SDK);
  Closed.superclass = Base.name;
  Closed.interfaces = {Marker.name};
  Closed.generic_signature =
      "Lfixture/Base<Ljava/lang/String;Ljava/lang/Object;>;"
      "Lfixture/Marker<Ljava/lang/String;>;";
  auto Context = klass("Lfixture/Context;", SDK);
  Context.superclass = Closed.name;
  auto Provider = klass("Lfixture/Provider;", SDK);
  if (OwnerFormal)
    Provider.generic_signature = "<T:Ljava/lang/Number;>Ljava/lang/Object;";
  const std::string Name = Two ? "Seed" : "T";
  std::vector<std::string> Parameters{Context.name};
  std::string Result = "I";
  if (Layout == 1) {
    Parameters.insert(Parameters.end(), {"J", "Z"});
    Result = "J";
  } else if (Layout == 2) {
    Parameters.insert(Parameters.end(), {"D", "[I"});
    Result = "D";
  } else if (Layout == 3) {
    Parameters.push_back("[" + Context.name);
    Result = "[" + Context.name;
  } else if (Layout == 4) {
    Result = "V";
  }
  auto Width = [](const std::string &T) {
    return T == "J" || T == "D" ? 2u : 1u;
  };
  unsigned Count = 0;
  std::string Signature = "<" + Name + ":Ljava/lang/Object;";
  if (Two)
    Signature += "U:Ljava/lang/Object;";
  Signature += ">(";
  for (const auto &P : Parameters) {
    Count += Width(P);
    Signature += P;
  }
  Signature += ")" + Result;
  const auto Return = Result == "V"             ? "return-void"
                      : Width(Result) == 2      ? "return-wide"
                      : Result.starts_with('[') ? "return-object"
                                                : "return";
  auto Value = op(0, Width(Result) == 2 ? "const-wide" : "const/4", {0});
  Value.literal = int64_t(0);
  auto Target = method(
      {Provider.name, "read", Parameters, Result},
      Static ? Access{"public", "static"} : Access{"public"},
      SDK ? 0 : 2 + Count + (Static ? 0 : 1),
      SDK             ? std::vector<Instruction>{}
      : Result == "V" ? std::vector<Instruction>{op(0, "return-void")}
                      : std::vector<Instruction>{Value, op(1, Return, {0})});
  Target.generic_signature = Signature;
  if (Throws)
    Target.declared_throws = {"Ljava/io/IOException;"};
  Provider.methods = {Target};
  auto Core = klass("Lfixture/Core;");
  auto Inputs = Parameters;
  if (!Static)
    Inputs.insert(Inputs.begin(), Provider.name);
  std::vector<unsigned> Words;
  for (unsigned N = 0; N < Count + (Static ? 0 : 1); ++N)
    Words.push_back(N + 2);
  std::vector<Instruction> Code{
      op(0,
         std::string(Static ? "invoke-static" : "invoke-virtual") +
             (Range ? "/range" : ""),
         Words, Target.reference)};
  if (Result != "V")
    Code.push_back(op(3,
                      Width(Result) == 2        ? "move-result-wide"
                      : Result.starts_with('[') ? "move-result-object"
                                                : "move-result",
                      {0}));
  Code.push_back(
      op(4, Return,
         Result == "V" ? std::vector<unsigned>{} : std::vector<unsigned>{0}));
  auto Call = method({Core.name, "call", Inputs, Result}, {"public", "static"},
                     2 + Words.size(), Code);
  if (Throws)
    Call.declared_throws = Target.declared_throws;
  Core.methods = {Call};
  Root.methods.push_back(
      method({Root.name, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  auto Init = [&](Class &C, const Class &Parent) {
    C.methods.push_back(
        method({C.name, "<init>", {}, "V"}, {"public", "constructor"},
               C.external ? 0 : 1,
               C.external ? std::vector<Instruction>{}
                          : std::vector<Instruction>{
                                op(0, "invoke-direct", {0},
                                   MethodRef{Parent.name, "<init>", {}, "V"}),
                                op(1, "return-void")}));
  };
  Init(Base, Root);
  Init(Closed, Base);
  Init(Context, Closed);
  Init(Provider, Root);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = "Ljava/lang/Throwable;";
  auto IO = klass("Ljava/io/IOException;", true);
  IO.superclass = Exception.name;
  return {Provider,
          Core,
          Context,
          Closed,
          Base,
          Marker,
          Root,
          klass("Ljava/lang/String;", true),
          klass("Ljava/lang/Number;", true),
          IO,
          Exception,
          klass("Ljava/lang/Throwable;", true)};
}
} // namespace

TEST(MobileDalvikDirectSeedNominal,
     UnusedFormalsRetainClosedSlotsIndependentOwnerScopesAndCheckedCalls) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Static : {false, true})
        for (bool Owner : {false, true})
          for (bool Two : {false, true})
            for (bool Throws : {false, true})
              for (unsigned Layout = 0; Layout < 5; ++Layout)
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(std::tuple{
                      SDK, Range, Static, Owner, Two, Throws, Layout, Route}));
                  auto Values = unusedScalarFixture(SDK, Range, Static, Owner,
                                                    Two, Throws, Layout);
                  const auto Target = Values[0].methods[0];
                  Budget B;
                  const auto Classes = input(std::move(Values), Route, B);
                  const auto Plans = validateGenericSignatures(Classes, B);
                  ASSERT_TRUE(Plans.erased_unused_formal_invocations.contains(
                      Target.reference));
                  EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                      Target.reference));
                  if (Throws)
                    EXPECT_TRUE(validatedClosedDeclaredThrows(Target, Plans));
                  const auto R = recoverJava(Classes, B);
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_EQ(R.getInteger("projected_method_count").value_or(0),
                            0);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                  EXPECT_EQ(Classes.at(Target.reference.owner)
                                .methods[0]
                                .generic_signature,
                            Target.generic_signature);
                  EXPECT_EQ(Classes.at(Target.reference.owner)
                                .methods[0]
                                .declared_throws,
                            Target.declared_throws);
                }
}

TEST(MobileDalvikDirectSeedNominal,
     UnusedInstanceFormalsRejectBorrowedScopesInvalidThrowsRolesAndWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 24; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Values =
            unusedScalarFixture(SDK, true, false, true, false, true, 1);
        auto &Target = Values[0].methods[0];
        auto &Caller = Values[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          const auto At = S.find(From);
          ASSERT_NE(At, std::string::npos);
          S.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Lfixture/Context;", "TMissing;");
          break;
        case 1:
          Replace(")J", ")I");
          break;
        case 2:
          Replace("Lfixture/Context;", "Ljava/lang/String;");
          break;
        case 3:
          Replace("T:Ljava/lang/Object;", "T:Lfixture/Missing;");
          break;
        case 4:
          Replace("T:Ljava/lang/Object;", "T:TT;");
          break;
        case 5:
          Replace(">(", "U:Ljava/lang/Object;V:Ljava/lang/Object;>(");
          break;
        case 6:
          Replace("Lfixture/Context;", "Lfixture/Context<Ljava/lang/String;>;");
          break;
        case 7:
          *Target.generic_signature += "^TT;";
          break;
        case 8:
          Target.declared_throws = {"Ljava/lang/Object;"};
          break;
        case 9:
          Target.declared_throws = {"Lfixture/Missing;"};
          break;
        case 10:
          Target.declared_throws->push_back("Ljava/io/IOException;");
          break;
        case 11:
          Values[9].superclass = Values[9].name;
          break;
        case 12:
          Target.access.insert("abstract");
          break;
        case 13:
          Target.access.insert("bridge");
          break;
        case 14:
          Target.access = {"private"};
          break;
        case 15:
          Target.access.insert("static");
          break;
        case 16:
          std::swap(Caller.instructions[0].registers[0],
                    Caller.instructions[0].registers[1]);
          break;
        case 17:
          Caller.instructions[0].registers.pop_back();
          break;
        case 18:
          Caller.instructions[0].registers[3] = 2;
          break;
        case 19:
          Target.access.insert("native");
          break;
        case 20:
          Values[0].generic_signature = "<T:TMissing;>Ljava/lang/Object;";
          break;
        case 21:
          Values[2].generic_signature = "Lfixture/Closed<TMissing;>;";
          break;
        case 22:
          Target.access.insert("varargs");
          break;
        case 23:
          Replace("JZ)", "ZJ)");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(std::move(Values), Route, B), B);
                     })(),
                     std::runtime_error);
      }
}

// Direct method seeds and unused formals keep separate scalar receipts.
namespace {
std::vector<Class> scalarSeedFixture(bool SDK, bool Range, bool Static,
                                     const std::string &Result,
                                     unsigned Position) {
  auto Values = unusedScalarFixture(SDK, Range, Static, false, false, true, 1);
  auto &Target = Values[0].methods[0];
  auto &Caller = Values[1].methods[0];
  const std::string Name = Position ? "Value" : "T";
  std::vector<std::string> Parameters{"J", "Z"};
  Parameters.insert(Parameters.begin() + Position, "Ljava/lang/Object;");
  std::string Signature = "<" + Name + ":Ljava/lang/Object;>(";
  for (unsigned I = 0; I < Parameters.size(); ++I)
    Signature += I == Position ? "T" + Name + ";" : Parameters[I];
  Target.generic_signature = Signature + ")" + Result;
  Target.reference.parameters = Parameters;
  Target.reference.returns = Result;
  const bool Wide = Result == "J" || Result == "D";
  if (!SDK) {
    Target.instructions[0].opcode = Wide ? "const-wide" : "const/4";
    Target.instructions[1].opcode = Wide ? "return-wide" : "return";
  }
  Caller.reference.parameters = Parameters;
  if (!Static)
    Caller.reference.parameters.insert(Caller.reference.parameters.begin(),
                                       Target.reference.owner);
  Caller.reference.returns = Result;
  Caller.instructions[0].reference = Target.reference;
  Caller.instructions[1].opcode = Wide ? "move-result-wide" : "move-result";
  Caller.instructions[2].opcode = Wide ? "return-wide" : "return";
  return Values;
}
} // namespace

TEST(MobileDalvikDirectSeedNominal,
     PublicScalarSeedsKeepOwnFormalsEveryPrimitiveResultAndClosedThrows) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Static : {false, true})
        for (const std::string Result :
             {"Z", "B", "S", "C", "I", "J", "F", "D"})
          for (unsigned Position : {0u, 1u, 2u})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Static, Result, Position, Route}));
              auto Values =
                  scalarSeedFixture(SDK, Range, Static, Result, Position);
              const auto Target = Values[0].methods[0];
              Budget B;
              const auto Classes = input(std::move(Values), Route, B);
              const auto Plans = validateGenericSignatures(Classes, B);
              EXPECT_TRUE(
                  Plans.erased_generic_invocations.contains(Target.reference));
              EXPECT_FALSE(Plans.erased_unused_formal_invocations.contains(
                  Target.reference));
              EXPECT_TRUE(validatedClosedDeclaredThrows(Target, Plans));
              const auto R = recoverJava(Classes, B);
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
              EXPECT_EQ(Classes.at(Target.reference.owner)
                            .methods[0]
                            .generic_signature,
                        Target.generic_signature);
              EXPECT_EQ(
                  Classes.at(Target.reference.owner).methods[0].declared_throws,
                  Target.declared_throws);
            }
}

TEST(MobileDalvikDirectSeedNominal,
     PublicScalarSeedsRejectChangedBindingsThrowsRolesAndOriginalWords) {
  for (bool SDK : {false, true})
    for (bool Static : {false, true})
      for (unsigned Mutation = 0; Mutation < 22; ++Mutation)
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{SDK, Static, Mutation, Route}));
          auto Values = scalarSeedFixture(SDK, true, Static, "I", 1);
          auto &Target = Values[0].methods[0];
          auto &Caller = Values[1].methods[0];
          auto Replace = [&](const std::string &From, const std::string &To) {
            auto &S = *Target.generic_signature;
            const auto At = S.find(From);
            ASSERT_NE(At, std::string::npos);
            S.replace(At, From.size(), To);
          };
          switch (Mutation) {
          case 0:
            Replace("TValue;Z)", "TMissing;Z)");
            break;
          case 1:
            Replace("Value:Ljava/lang/Object;", "Value:Ljava/lang/Number;");
            break;
          case 2:
            Replace("TValue;Z)", "Ljava/lang/String;Z)");
            break;
          case 3:
            Replace(")I", ")J");
            break;
          case 4:
            Values[0].generic_signature =
                "<E:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 5:
            Values[0].generic_signature = "Lfixture/Closed<TMissing;>;";
            Values[0].superclass = "Lfixture/Closed;";
            break;
          case 6:
            Target.access.insert("native");
            break;
          case 7:
            Target.access.insert("abstract");
            break;
          case 8:
            Target.access.insert("bridge");
            break;
          case 9:
            Target.access.insert("synthetic");
            break;
          case 10:
            Target.access.insert("varargs");
            break;
          case 11:
            Caller.instructions[0].opcode =
                Static ? "invoke-virtual/range" : "invoke-static/range";
            break;
          case 12:
            Caller.instructions[0].registers.pop_back();
            break;
          case 13:
            Caller.instructions[0].registers[Static ? 1 : 2] = 2;
            break;
          case 14:
            Caller.instructions[1].opcode = "move-result-object";
            break;
          case 15:
            Target.declared_throws = {"Ljava/lang/String;"};
            break;
          case 16:
            Target.declared_throws = {"Lfixture/Missing;"};
            break;
          case 17:
            Target.declared_throws->push_back("Ljava/io/IOException;");
            break;
          case 18:
            *Target.generic_signature += "^TValue;";
            break;
          case 19:
            Replace(">(", "U:Ljava/lang/Object;>(");
            break;
          case 20:
            Replace("(JTValue;Z)", "(ZTValue;J)");
            break;
          case 21:
            Target.access.erase("public");
            Target.access.insert("private");
            break;
          }
          EXPECT_THROW(([&] {
                         Budget B;
                         recoverJava(input(std::move(Values), Route, B), B);
                       })(),
                       std::runtime_error);
        }
}

// Fixed nominal contexts share the direct method seed proof across surfaces.
namespace {
std::vector<Class> nominalContextSeedFixture(bool SDK, bool Range, bool Static,
                                             unsigned Position, bool Scalar) {
  auto Classes =
      primitiveSeedFixture(SDK, Range, Static, false, 1, Position, 2);
  auto &Target = Classes[0].methods[0];
  auto &Caller = Classes[1].methods[0];
  const auto Context = Position == 2 ? 1u : 2u;
  Target.reference.parameters[Context] = "Ljava/lang/String;";
  const std::string Name = Position ? "Seed" : "T";
  std::string Signature = "<" + Name + ":Ljava/lang/Object;>(";
  for (unsigned I = 0; I < Target.reference.parameters.size(); ++I)
    Signature +=
        I == Position ? "T" + Name + ";" : Target.reference.parameters[I];
  if (Scalar) {
    Target.reference.returns = "I";
    if (!SDK)
      Target.instructions[1].opcode = "return";
    Caller.reference.returns = "I";
    Caller.instructions[1].opcode = "move-result";
    Caller.instructions[2].opcode = "return";
  }
  Target.generic_signature = Signature + ")" + Target.reference.returns;
  Target.declared_throws = std::vector<std::string>{"Ljava/lang/Exception;"};
  Caller.reference.parameters[Context + 1] = "Ljava/lang/String;";
  Caller.instructions[0].reference = Target.reference;
  return Classes;
}
std::vector<Class> nominalContextBridgeFixture(bool SDK, bool Range, bool Alpha,
                                               unsigned Depth) {
  auto Classes = methodSeedBridgeFixture(SDK, Range, Alpha, true, Depth);
  for (auto &Owner : Classes)
    for (auto &Member : Owner.methods) {
      if (Member.reference.name == "accept") {
        Member.reference.parameters[1] = "Ljava/lang/String;";
        if (Member.generic_signature) {
          auto &Text = *Member.generic_signature;
          const auto At = Text.find(";ZZ)");
          Text.replace(At, 4, ";Ljava/lang/String;Z)");
        }
      }
      for (auto &I : Member.instructions)
        if (auto *Ref = std::get_if<MethodRef>(&I.reference);
            Ref && Ref->name == "accept")
          Ref->parameters[1] = "Ljava/lang/String;";
    }
  Classes[1].methods[0].reference.parameters[2] = "Ljava/lang/String;";
  return Classes;
}
} // namespace

TEST(MobileDalvikDirectSeedNominal,
     FixedContextsKeepNominalAndScalarResultsClosedThrowsAndOriginalWords) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Static : {false, true})
        for (bool Scalar : {false, true})
          for (unsigned Position : {0u, 1u, 2u})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Static, Scalar, Position, Route}));
              auto Values = nominalContextSeedFixture(SDK, Range, Static,
                                                      Position, Scalar);
              const auto Target = Values[0].methods[0];
              Budget B;
              const auto Classes = input(std::move(Values), Route, B);
              const auto Plans = validateGenericSignatures(Classes, B);
              EXPECT_TRUE(
                  Plans.erased_generic_invocations.contains(Target.reference));
              EXPECT_FALSE(Plans.erased_unused_formal_invocations.contains(
                  Target.reference));
              EXPECT_TRUE(validatedClosedDeclaredThrows(Target, Plans));
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(Report.getInteger("projected_method_count").value_or(0),
                        0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              const auto &After = Classes.at(Target.reference.owner).methods[0];
              EXPECT_EQ(After.generic_signature, Target.generic_signature);
              EXPECT_EQ(After.declared_throws, Target.declared_throws);
              EXPECT_EQ(After.access, Target.access);
            }
}

TEST(MobileDalvikDirectSeedNominal,
     FixedContextSeedScopesErasuresThrowsAndPhysicalWordsRemainValidated) {
  for (bool SDK : {false, true})
    for (bool Static : {false, true})
      for (unsigned Mutation = 0; Mutation < 26; ++Mutation)
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{SDK, Static, Mutation, Route}));
          auto Classes = nominalContextSeedFixture(SDK, true, Static, 1, false);
          auto &Provider = Classes[0];
          auto &Target = Provider.methods[0], &Caller = Classes[1].methods[0];
          switch (Mutation) {
          case 0:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;>(JTMissing;Ljava/lang/"
                "String;)Lfixture/Provider;";
            break;
          case 1:
            Target.generic_signature = "<Seed:Ljava/lang/Number;>(JTSeed;Ljava/"
                                       "lang/String;)Lfixture/Provider;";
            break;
          case 2:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;>(JLjava/lang/"
                "String;Ljava/lang/String;)Lfixture/Provider;";
            break;
          case 3:
            Target.generic_signature = "<Seed:Ljava/lang/Object;>(JTSeed;Ljava/"
                                       "lang/String;)Ljava/lang/Object;";
            break;
          case 4:
            Provider.generic_signature = "Lfixture/Ancestor1<TMissing;>;";
            break;
          case 5:
            Target.access.erase("public");
            Target.access.insert("private");
            break;
          case 6:
            Target.access.insert("native");
            break;
          case 7:
            Target.access.insert("abstract");
            break;
          case 8:
            Target.access.insert("bridge");
            break;
          case 9:
            Caller.instructions[0].opcode =
                Static ? "invoke-virtual/range" : "invoke-static/range";
            break;
          case 10:
            Target.access.insert("varargs");
            break;
          case 11:
            Target.generic_signature = "<Seed:Ljava/lang/Object;>(JTSeed;Ljava/"
                                       "lang/Object;)Lfixture/Provider;";
            break;
          case 12:
            Caller.instructions[0].registers.pop_back();
            break;
          case 13:
            Caller.instructions[0].registers.back() = Caller.registers;
            break;
          case 14:
            Caller.instructions[1].opcode = "move-result";
            break;
          case 15:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;>(JTSeed;Ljava/lang/String;)Lfixture/"
                "Provider<TSeed;>;";
            break;
          case 16:
            Target.generic_signature = "<Seed:Ljava/lang/Object;>(DTSeed;Ljava/"
                                       "lang/String;)Lfixture/Provider;";
            break;
          case 17:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;>(JTSeed;I)Lfixture/Provider;";
            break;
          case 18:
            Target.generic_signature = "<Seed:Ljava/lang/Object;>(TSeed;JLjava/"
                                       "lang/String;)Lfixture/Provider;";
            break;
          case 19:
            Target.generic_signature =
                "<Seed:Ljava/lang/Object;U:Ljava/lang/Object;>"
                "(JTSeed;Ljava/lang/String;)Lfixture/Provider;";
            break;
          case 20:
            Target.access.insert("synthetic");
            break;
          case 21:
            Provider.access.erase("public");
            break;
          case 22:
            Target.declared_throws =
                std::vector<std::string>{"Ljava/lang/String;"};
            break;
          case 23:
            Caller.instructions[0].registers[Static ? 1 : 2] = 3;
            break;
          case 24:
            *Target.generic_signature += "^TSeed;";
            break;
          case 25:
            for (auto &C : Classes)
              if (C.name == "Ljava/lang/String;")
                C.generic_signature = "<C:TMissing;>Ljava/lang/Object;";
            break;
          }
          EXPECT_THROW(([&] {
                         Budget B;
                         recoverJava(input(Classes, Route, B), B);
                       }()),
                       std::runtime_error);
        }
}

TEST(MobileDalvikDirectSeedNominal,
     FixedContextMethodSeedsKeepAllOriginalCovariantBridgeSlots) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (unsigned Depth : {1u, 2u})
          for (unsigned Route : {0u, 1u, 2u}) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{SDK, Range, Alpha, Depth, Route}));
            ASSERT_NO_THROW(([&] {
              Budget B;
              auto Classes =
                  input(nominalContextBridgeFixture(SDK, Range, Alpha, Depth),
                        Route, B);
              const auto Plans = validateGenericSignatures(Classes, B);
              for (const auto &[Name, Owner] : Classes) {
                if (Owner.external)
                  continue;
                for (const auto &Bridge : Owner.methods) {
                  if (!Bridge.access.contains("bridge"))
                    continue;
                  auto Required = Bridge.reference;
                  Required.owner = Bridge.reference.returns;
                  const auto Found = Plans.covariant_bridge_contracts.find(
                      {Bridge.reference, Required});
                  ASSERT_NE(Found, Plans.covariant_bridge_contracts.end());
                  EXPECT_EQ(Found->second.target, Owner.methods[0].reference);
                  const auto &Contract = Found->second.contract;
                  ASSERT_EQ(Contract.type_parameters.size(), 1u);
                  EXPECT_EQ(Contract.type_parameters[0].erasure,
                            "Ljava/lang/Object;");
                  EXPECT_EQ(Contract.parameters.size(),
                            Bridge.reference.parameters.size());
                  EXPECT_TRUE(Contract.source_projections.empty());
                  EXPECT_TRUE(Contract.throws_types.empty());
                }
              }
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(Report.getInteger("projected_method_count"),
                        SDK ? Depth : Depth * (Depth + 1) / 2);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
            })());
          }
}

TEST(MobileDalvikDirectSeedNominal,
     FixedContextBridgesRejectLostScopesOriginalSlotsAndForwardingEffects) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 24; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = nominalContextBridgeFixture(SDK, false, false, 2);
        auto &Leaf = Classes.back();
        auto &Target = Leaf.methods[0];
        auto &Bridge = Leaf.methods[2];
        switch (Mutation) {
        case 0:
          Target.generic_signature = "<Z:Ljava/lang/Object;>(TMissing;Ljava/"
                                     "lang/String;Z)Lfixture/Leaf;";
          break;
        case 1:
          Target.generic_signature =
              "<Z:Ljava/lang/Number;>(TZ;Ljava/lang/String;Z)Lfixture/Leaf;";
          break;
        case 2:
          Target.generic_signature =
              "<Z:Ljava/lang/Object;>(Ljava/lang/Object;Ljava/lang/"
              "String;Z)Lfixture/Leaf;";
          break;
        case 3:
          Target.generic_signature =
              "<Z:Ljava/lang/Object;U:Ljava/lang/Object;>(TZ;Ljava/lang/"
              "String;Z)Lfixture/Leaf;";
          break;
        case 4:
          Target.generic_signature =
              "<Z:Ljava/lang/Object;>(TZ;IZ)Lfixture/Leaf;";
          break;
        case 5:
          Target.generic_signature = "<Z:Ljava/lang/Object;>(TZ;Ljava/lang/"
                                     "String;Z)Ljava/lang/Object;";
          break;
        case 6:
          Target.access.insert("static");
          break;
        case 7:
          Target.access.insert("native");
          break;
        case 8:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 9:
          Classes[0].methods[0].access.insert("final");
          break;
        case 10:
          Bridge.access.erase("synthetic");
          break;
        case 11:
          Leaf.methods.erase(Leaf.methods.begin() + 2);
          break;
        case 12:
          Bridge.instructions[0].opcode = "invoke-static";
          break;
        case 13:
          Bridge.instructions[0].registers[0] = 1;
          break;
        case 14:
          Bridge.instructions[0].registers[1] = 0;
          break;
        case 15:
          std::swap(Bridge.instructions[0].registers[2],
                    Bridge.instructions[0].registers[3]);
          break;
        case 16:
          Bridge.instructions[1].opcode = "move-result";
          break;
        case 17:
          Bridge.instructions[2].registers[0] = 0;
          break;
        case 18: {
          auto Extra = Bridge.instructions[0];
          Extra.pc = 5;
          Bridge.instructions.insert(Bridge.instructions.end() - 1, Extra);
          Bridge.instructions.back().pc = 8;
          Bridge.code_end = 9;
          break;
        }
        case 19:
          Bridge.generic_signature = "<Z:Ljava/lang/Object;>(TZ;Ljava/lang/"
                                     "String;Z)Lfixture/Provider;";
          break;
        case 20:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 21:
          Leaf.generic_signature = "Lfixture/Middle<TMissing;>;";
          break;
        case 22:
          Target.generic_signature = "<Z:Ljava/lang/Object;>(TZ;Ljava/lang/"
                                     "String;Z)Lfixture/Leaf<TZ;>;";
          break;
        case 23:
          std::get<MethodRef>(Bridge.instructions[0].reference).owner =
              "Lfixture/Middle;";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
