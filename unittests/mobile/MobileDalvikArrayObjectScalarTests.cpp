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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool FromField,
                           bool TypedKey = false) {
  const std::string Object = "Ljava/lang/Object;",
                    Array = "[Ljava/lang/Object;", E = Alpha ? "Element" : "E";
  auto Provider = klass("Lfixture/Provider;", SDK);
  MethodRef Selected{Provider.name, "score", {Array, Object}, "I"};
  auto Value = op(0, "const", {0});
  Value.literal = int64_t(-2147483611);
  auto Target =
      method(Selected, {"public", "static"}, SDK ? 0 : 3,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Value, op(1, "return", {0})});
  Target.generic_signature = "<" + E + ":Ljava/lang/Object;>([T" + E + ";" +
                             (TypedKey ? "T" + E + ";" : "Ljava/lang/Object;") +
                             ")I";
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  Client.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  Method Caller;
  if (FromField) {
    Field Values;
    Values.reference = {Client.name, "values", Array};
    Values.access = {"public", "final"};
    Values.generic_signature = "[TT;";
    Client.fields.push_back(Values);
    auto Init = method(
        {Client.name, "<init>", {Array}, "V"}, {"public", "constructor"}, 2,
        {op(0, "invoke-direct", {0}, MethodRef{Object, "<init>", {}, "V"}),
         op(1, "iput-object", {1, 0}, Values.reference), op(2, "return-void")});
    Init.generic_signature = "([TT;)V";
    Client.methods.push_back(Init);
    Caller = method({Client.name, "index", {Object}, "I"}, {"private"}, 4,
                    {op(0, "iget-object", {0, 2}, Values.reference),
                     op(1, "move-object", {1, 3}),
                     op(2, Range ? "invoke-static/range" : "invoke-static",
                        {0, 1}, Selected),
                     op(3, "move-result", {0}), op(4, "return", {0})});
    Caller.generic_signature = "(TT;)I";
  } else
    Caller = method({Client.name, "call", {Array, Object}, "I"},
                    {"public", "static"}, 2,
                    {op(0, Range ? "invoke-static/range" : "invoke-static",
                        {0, 1}, Selected),
                     op(1, "move-result", {0}), op(2, "return", {0})});
  Client.methods.push_back(Caller);
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  return {Client, Provider, Root, klass("Ljava/lang/String;", true)};
}
} // namespace

TEST(MobileDalvikArrayObjectScalar,
     RetainMethodOwnedArraysIndependentObjectsAndOpaqueIntegers) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool FromField : {false, true})
          for (bool TypedKey : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, FromField, TypedKey, Route}));
              auto Input = fixture(SDK, Range, Alpha, FromField, TypedKey);
              const auto Original = Input;
              const auto Selected = Input[1].methods[0].reference;
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
              EXPECT_TRUE(Plans.erased_generic_invocations.contains(Selected));
              for (const auto &C : Original) {
                const auto &After = Classes.at(C.name);
                EXPECT_EQ(After.access, C.access);
                EXPECT_EQ(After.generic_signature, C.generic_signature);
                for (size_t I = 0; I < C.methods.size(); ++I) {
                  EXPECT_EQ(After.methods[I].reference, C.methods[I].reference);
                  EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                  EXPECT_EQ(After.methods[I].generic_signature,
                            C.methods[I].generic_signature);
                }
              }
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikArrayObjectScalar,
     RejectChangedArrayScopesBoundsShapesAndDeclarationRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, false);
      auto &Target = Input[1].methods[0];
      auto &Caller = Input[0].methods[0];
      if (Mutation == 0)
        Target.generic_signature =
            "<E:Ljava/lang/Object;>([TMissing;Ljava/lang/Object;)I";
      else if (Mutation == 1)
        Target.generic_signature =
            "<E:Ljava/lang/String;>([TE;Ljava/lang/Object;)I";
      else if (Mutation == 2)
        Target.generic_signature = "<E:Ljava/lang/Object;U:Ljava/lang/"
                                   "Object;>([TE;Ljava/lang/Object;)I";
      else if (Mutation == 3)
        Target.generic_signature =
            "<E:Ljava/lang/Object;>([TE;Ljava/util/List<TE;>;)I";
      else if (Mutation == 4)
        Target.generic_signature =
            "<E:Ljava/lang/Object;>([[TE;Ljava/lang/Object;)I";
      else if (Mutation == 5)
        Target.generic_signature =
            "<E:Ljava/lang/Object;>([TE;Ljava/lang/Object;)Z";
      else if (Mutation == 6)
        Target.access.insert("varargs");
      else if (Mutation == 7) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 8) {
        Target.access.erase("public");
        Target.access.insert("protected");
      } else if (Mutation == 9)
        Target.access.insert("synthetic");
      else if (Mutation == 10)
        Target.access.insert("native");
      else if (Mutation == 11)
        Target.access.insert("abstract");
      else if (Mutation == 12)
        Target.access.erase("static");
      else if (Mutation == 13)
        Input[1].access.insert("interface");
      else if (Mutation == 14)
        Input[1].access.erase("public");
      else if (Mutation == 15)
        Input[1].generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 16)
        Target.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 17)
        Target.generic_signature = "<E:Ljava/lang/Object;>([TE;TMissing;)I";
      else if (Mutation == 18)
        Caller.instructions[0].registers.pop_back();
      else
        Caller.instructions[0].opcode = "invoke-interface";
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}

namespace {
std::vector<Class> arrayObjectContextClasses(bool SDK, bool Range, bool Alpha,
                                             unsigned Shape) {
  auto Classes = fixture(SDK, Range, Alpha, false);
  auto &Target = Classes[1].methods[0];
  const std::vector<std::vector<std::string>> Suffixes{
      {"I"}, {"J"}, {"D"}, {"I", "Z"}, {"J", "D", "Z"}};
  const auto &Suffix = Suffixes.at(Shape);
  std::string Signature =
      "<" + std::string(Alpha ? "Element" : "E") + ":Ljava/lang/Object;>([T" +
      std::string(Alpha ? "Element" : "E") + ";Ljava/lang/Object;";
  unsigned Words = 2;
  for (const auto &Type : Suffix) {
    Target.reference.parameters.push_back(Type);
    Signature += Type;
    Words += Type == "J" || Type == "D" ? 2 : 1;
  }
  Target.generic_signature = Signature + ")I";
  Target.registers = SDK ? 0 : Words + 1;
  auto &Caller = Classes[0].methods[0];
  Caller.reference.parameters = Target.reference.parameters;
  Caller.registers = Words + 1;
  std::vector<unsigned> Arguments;
  for (unsigned I = 1; I <= Words; ++I)
    Arguments.push_back(I);
  Caller.instructions[0] =
      op(0, Range ? "invoke-static/range" : "invoke-static", Arguments,
         Target.reference);
  return Classes;
}
ClassMap arrayObjectContextInput(std::vector<Class> Classes, unsigned Route,
                                 Budget &B) {
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

TEST(MobileDalvikArrayObjectScalar,
     KeepScalarContextsBesideOwnArrayAndIndependentObject) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (unsigned Shape : {0u, 1u, 2u, 3u, 4u})
          for (unsigned Route : {0u, 1u, 2u}) {
            if (Shape == 4 && !Range)
              continue;
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{SDK, Range, Alpha, Shape, Route}));
            ASSERT_NO_THROW(([&] {
              Budget B;
              auto Classes = arrayObjectContextInput(
                  arrayObjectContextClasses(SDK, Range, Alpha, Shape), Route,
                  B);
              const auto Target = Classes.at("Lfixture/Provider;").methods[0];
              const auto Plans = validateGenericSignatures(Classes, B);
              EXPECT_TRUE(
                  Plans.erased_generic_invocations.contains(Target.reference));
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

TEST(MobileDalvikArrayObjectScalar,
     RejectChangedArrayScopesScalarSuffixesRolesAndWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 21; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = arrayObjectContextClasses(SDK, false, false, 0);
        auto &Target = Classes[1].methods[0], &Caller = Classes[0].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Signature = *Target.generic_signature;
          const auto Pos = Signature.find(From);
          ASSERT_NE(Pos, std::string::npos);
          Signature.replace(Pos, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("[TE;", "[TMissing;");
          break;
        case 1:
          replace("[TE;", "[[TE;");
          break;
        case 2:
          replace("E:Ljava/lang/Object;", "E:Ljava/lang/String;");
          break;
        case 3:
          replace("<E:", "<Unused:Ljava/lang/Object;E:");
          break;
        case 4:
          replace(")I", ")Z");
          break;
        case 5:
          Target.access.insert("varargs");
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
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 11:
          Classes[1].access.insert("interface");
          Classes[1].access.insert("abstract");
          break;
        case 12:
          Classes[1].generic_signature =
              "<Outer:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 13:
          Target.declared_throws = {"Ljava/lang/Exception;"};
          break;
        case 14:
          replace(";I)", ";J)");
          break;
        case 15:
          replace(";I)", ";TMissing;)");
          break;
        case 16:
          replace(";I)", ";Ljava/lang/Object;)");
          Target.reference.parameters.back() = "Ljava/lang/Object;";
          Caller.reference.parameters = Target.reference.parameters;
          Caller.instructions[0].reference = Target.reference;
          break;
        case 17:
          Caller.instructions[0].registers.pop_back();
          break;
        case 18:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 19:
          Caller.instructions[1].opcode = "move-result-object";
          break;
        case 20:
          Caller.instructions[0].opcode = "invoke-interface";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(arrayObjectContextInput(Classes, Route, B),
                                   B);
                     }()),
                     std::runtime_error);
      }
}
