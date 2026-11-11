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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool IfNull,
                           bool Marker) {
  const std::string Object = "Ljava/lang/Object;",
                    Carrier = "Lfixture/Carrier;",
                    String = "Ljava/lang/String;", K = Alpha ? "Key" : "K",
                    V = Alpha ? "Value" : "V";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  auto Parent = klass("Lfixture/Parent;", SDK);
  Parent.generic_signature = "<" + K + ":Ljava/lang/Object;" + V +
                             ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Init =
      method({Parent.name, "<init>", {Carrier}, "V"}, {"public", "constructor"},
             SDK ? 0 : 2,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{
                       op(0, "invoke-direct", {0}, Root.methods[0].reference),
                       op(1, "return-void")});
  Init.generic_signature = "(Lfixture/Carrier<+T" + K + ";+T" + V + ";>;)V";
  auto Sibling = Init;
  Sibling.reference.parameters = {"I"};
  Sibling.generic_signature.reset();
  Parent.methods = {Init, Sibling};
  auto Bound = klass(Carrier, SDK);
  Bound.access = {"public", "interface", "abstract"};
  Bound.generic_signature =
      "<Left:Ljava/lang/Object;Right:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Fresh = klass("Lfixture/Fresh;", SDK);
  Fresh.interfaces = {Carrier};
  Fresh.generic_signature = "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/"
                            "lang/Object;Lfixture/Carrier<TA;TB;>;";
  Fresh.methods.push_back(method(
      {Fresh.name, "<init>", {}, "V"}, {"public", "constructor"}, SDK ? 0 : 1,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{
                op(0, "invoke-direct", {0}, Root.methods[0].reference),
                op(1, "return-void")}));
  auto Core = klass("Lfixture/Core;");
  Core.superclass = Parent.name;
  Core.generic_signature =
      "Lfixture/Parent<Ljava/lang/String;Ljava/lang/Object;>;";
  unsigned Destination = Range ? 2 : 0;
  auto Branch = op(0, IfNull ? "if-nez" : "if-eqz", {2});
  Branch.target = 8;
  auto Jump = op(7, "goto");
  Jump.target = 9;
  auto Caller = method(
      {Core.name, "<init>", {Carrier}, "V"}, {"public", "constructor"}, 3,
      {Branch, op(2, "new-instance", {Destination}, Fresh.name),
       op(4, Range ? "invoke-direct/range" : "invoke-direct", {Destination},
          Fresh.methods[0].reference),
       Jump, op(8, "move-object", {Destination, 2}),
       op(9, Range ? "invoke-direct/range" : "invoke-direct", {1, Destination},
          Init.reference),
       op(12, "return-void")});
  Caller.generic_signature =
      "(Lfixture/Carrier<Ljava/lang/String;Ljava/lang/Object;>;)V";
  Core.methods.push_back(Caller);
  auto Text = klass(String, true);
  auto Tag = klass("Lfixture/Tag;");
  Tag.access = {"public", "interface", "abstract"};
  Tag.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  if (Marker) {
    Core.interfaces = {Tag.name};
    *Core.generic_signature += "Lfixture/Tag<Ljava/lang/String;>;";
  }
  return {Parent, Core, Fresh, Bound, Root, Text, Tag};
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
TEST(MobileDalvikClosedMapSuper,
     KeepClosedParentSubstitutionsConditionalAllocationAndExactSelectedCtor) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool IfNull : {false, true})
          for (bool Marker : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, IfNull, Marker, Route}));
              ASSERT_NO_THROW(([&] {
                Budget B;
                auto Classes =
                    input(fixture(SDK, Range, Alpha, IfNull, Marker), Route, B);
                const auto Target = Classes.at("Lfixture/Parent;").methods[0],
                           Caller = Classes.at("Lfixture/Core;").methods[0];
                const auto Plans = validateGenericSignatures(Classes, B);
                EXPECT_TRUE(Plans.constructor_super_prologues.contains(
                    Caller.reference));
                EXPECT_TRUE(Plans.superclass_calls.contains(
                    {Caller.reference, Target.reference}));
                const auto R = recoverJava(Classes, B);
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
                EXPECT_EQ(Classes.at(Target.reference.owner)
                              .methods[0]
                              .generic_signature,
                          Target.generic_signature);
                EXPECT_EQ(Classes.at(Caller.reference.owner)
                              .methods[0]
                              .generic_signature,
                          Caller.generic_signature);
              })());
            }
}
TEST(MobileDalvikClosedMapSuper,
     RejectUnclosedEdgesUnsafeAllocationsCandidateFormalsAndInvalidCfgWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 18; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, false, false, true, true);
        auto &Parent = Classes[0], &Core = Classes[1], &Fresh = Classes[2];
        auto &Target = Parent.methods[0], &Caller = Core.methods[0];
        switch (Mutation) {
        case 0:
          Core.generic_signature =
              "Lfixture/Parent<*>;Lfixture/Tag<Ljava/lang/String;>;";
          break;
        case 1:
          Core.generic_signature =
              "<T:Ljava/lang/Object;>Lfixture/Parent<TT;Ljava/lang/"
              "Object;>;Lfixture/Tag<Ljava/lang/String;>;";
          break;
        case 2:
          Core.generic_signature = "Lfixture/Parent<TMissing;Ljava/lang/"
                                   "Object;>;Lfixture/Tag<Ljava/lang/String;>;";
          break;
        case 3:
          Target.generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/Carrier<+TK;+TV;>;)V";
          break;
        case 4:
          Parent.methods[1].generic_signature = "<X:Ljava/lang/Object;>(I)V";
          break;
        case 5:
          Target.access = {"private", "constructor"};
          break;
        case 6:
          Target.access.insert("varargs");
          break;
        case 7:
          Fresh.access.insert("abstract");
          break;
        case 8:
          Fresh.methods[0].access = {"private", "constructor"};
          break;
        case 9:
          Fresh.methods[0].declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 10:
          Fresh.generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
          Fresh.interfaces.clear();
          break;
        case 11:
          Caller.instructions[5].registers[0] = 0;
          break;
        case 12:
          Caller.instructions[5].registers.pop_back();
          break;
        case 13:
          Caller.instructions[0].target = 4;
          break;
        case 14:
          Caller.instructions[1].registers[0] = 1;
          break;
        case 15:
          Target.generic_signature = "(Lfixture/Carrier<+TK;+TMissing;>;)V";
          break;
        case 16:
          Core.generic_signature =
              "Lfixture/Parent<Lfixture/Carrier<Ljava/lang/String;"
              "Ljava/lang/Object;>;Ljava/lang/Object;>;"
              "Lfixture/Tag<Ljava/lang/String;>;";
          Target.generic_signature = "(TK;)V";
          break;
        case 17:
          Core.generic_signature =
              "Lfixture/Parent<Ljava/lang/String;Ljava/lang/String;>;"
              "Lfixture/Tag<Ljava/lang/String;>;";
          Parent.methods[1].reference.parameters = {"Ljava/lang/String;"};
          Parent.methods[1].generic_signature = "(TV;)V";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
