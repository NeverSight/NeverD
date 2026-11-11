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
std::vector<Class> fixture(bool SDK, bool CarrierSDK, bool Range, bool Alpha,
                           bool Reversed, bool Wide, bool Covariant) {
  const std::string Object = "Ljava/lang/Object;",
                    Carrier = "Lfixture/Carrier;", K = Alpha ? "Key" : "K",
                    V = Alpha ? "Value" : "V", C = Alpha ? "Container" : "C",
                    First = Wide ? "J" : "I", Second = Wide ? "D" : "F",
                    Variance = Covariant ? "+" : "";
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
  Init.generic_signature = "(Lfixture/Carrier<" + Variance + "T" + K + ";" +
                           Variance + "T" + V + ";>;)V";
  auto Sibling = Init;
  Sibling.reference.parameters = {"Lfixture/Supplier;"};
  Sibling.generic_signature =
      "(Lfixture/Supplier<Lfixture/Carrier<T" + K + ";T" + V + ";>;>;)V";
  Parent.methods = {Init, Sibling};
  auto Bound = klass(Carrier, CarrierSDK);
  Bound.access = {"public", "interface", "abstract"};
  Bound.generic_signature =
      "<Left:Ljava/lang/Object;Right:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Fresh = klass("Lfixture/Fresh;", SDK);
  Fresh.interfaces = {Carrier};
  Fresh.generic_signature = "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/"
                            "lang/Object;Lfixture/Carrier<TK;TV;>;";
  auto Allocation =
      method({Fresh.name, "<init>", {First, Second}, "V"},
             {"public", "constructor"}, SDK ? 0 : (Wide ? 5 : 3),
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{
                       op(0, "invoke-direct", {0}, Root.methods[0].reference),
                       op(1, "return-void")});
  auto OtherAllocation = Allocation;
  OtherAllocation.reference.parameters = {Second, First};
  Fresh.methods = {Allocation, OtherAllocation};
  auto Core = klass("Lfixture/Core;");
  Core.superclass = Parent.name;
  // The child and allocation deliberately reuse lexical formal names. Their
  // owner identities remain distinct before and after parent substitution.
  Core.generic_signature = "<K:Ljava/lang/Object;V:Ljava/lang/Object;" + C +
                           "::Lfixture/Dependent<TV;>;>Lfixture/Parent<TK;T" +
                           C + ";>;";
  auto Dependent = klass("Lfixture/Dependent;", CarrierSDK);
  Dependent.access = {"public", "abstract", "interface"};
  Dependent.generic_signature =
      "<Element:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Supplier = klass("Lfixture/Supplier;", CarrierSDK);
  Supplier.access = {"public", "abstract", "interface"};
  Supplier.generic_signature = "<Element:Ljava/lang/Object;>Ljava/lang/Object;";
  const unsigned Self = Wide ? 5 : 3, FirstWord = 1, SecondWord = Wide ? 3 : 2,
                 Incoming = Self + 1, Words = Wide ? 2 : 1;
  const unsigned FromFirst = Incoming + (Reversed ? Words : 0),
                 FromSecond = Incoming + (Reversed ? 0 : Words);
  std::vector<unsigned> Arguments{0, FirstWord};
  if (Wide)
    Arguments.push_back(FirstWord + 1);
  Arguments.push_back(SecondWord);
  if (Wide)
    Arguments.push_back(SecondWord + 1);
  auto Caller =
      method({Core.name, "<init>",
              Reversed ? std::vector<std::string>{Second, First}
                       : std::vector<std::string>{First, Second},
              "V"},
             {"public", "constructor"}, Wide ? 10 : 6,
             {op(0, Wide ? "move-wide" : "move", {FirstWord, FromFirst}),
              op(1, Wide ? "move-wide" : "move", {SecondWord, FromSecond}),
              op(2, "new-instance", {0}, Fresh.name),
              op(4, Range ? "invoke-direct/range" : "invoke-direct", Arguments,
                 Allocation.reference),
              op(7, "move-object", {Incoming, 0}),
              op(8, Range ? "invoke-direct/range" : "invoke-direct",
                 {Self, Incoming}, Init.reference),
              op(11, "return-void")});
  Core.methods = {Caller};
  return {Parent, Core, Fresh, Bound, Root, Dependent, Supplier};
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

TEST(MobileDalvikDependentCarrierAllocation,
     KeepDependentOwnerBoundsPrimitiveWordsAndSelectedCarrierOverloads) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Reversed : {false, true})
            for (bool Wide : {false, true})
              for (bool Covariant : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, CarrierSDK, Range, Alpha, Reversed, Wide,
                                 Covariant, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = input(fixture(SDK, CarrierSDK, Range, Alpha,
                                                 Reversed, Wide, Covariant),
                                         Route, B);
                    const auto Target =
                                   Classes.at("Lfixture/Parent;").methods[0],
                               Caller = Classes.at("Lfixture/Core;").methods[0];
                    const auto Header =
                        Classes.at("Lfixture/Core;").generic_signature;
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(Plans.constructor_super_prologues.contains(
                        Caller.reference));
                    EXPECT_TRUE(Plans.superclass_calls.contains(
                        {Caller.reference, Target.reference}));
                    const auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    EXPECT_EQ(Classes.at("Lfixture/Core;").generic_signature,
                              Header);
                    EXPECT_EQ(Classes.at("Lfixture/Core;")
                                  .methods[0]
                                  .generic_signature,
                              Caller.generic_signature);
                    EXPECT_EQ(Classes.at(Target.reference.owner)
                                  .methods[0]
                                  .generic_signature,
                              Target.generic_signature);
                  })());
                }
}

TEST(MobileDalvikDependentCarrierAllocation,
     RejectUnprovedDependentBoundsWordsScopesAndConstructorDeclarations) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 33; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, false, false, true, true);
        auto &Parent = Classes[0], &Core = Classes[1], &Fresh = Classes[2];
        auto &Target = Parent.methods[0], &Caller = Core.methods[0],
             &Allocation = Fresh.methods[0];
        switch (Mutation) {
        case 0:
          Caller.instructions[3].registers[2] = 4;
          break;
        case 1:
          Caller.instructions[0].registers[1] = 8;
          break;
        case 2:
          Caller.instructions[1].registers[1] = 6;
          break;
        case 3:
          Caller.instructions[1].registers[0] = 2;
          break;
        case 4:
          Caller.instructions[3].registers.pop_back();
          break;
        case 5:
          Caller.instructions[2].registers[0] = 5;
          break;
        case 6:
          Caller.instructions[5].registers[0] = 0;
          break;
        case 7:
          Target.generic_signature = "(Lfixture/Carrier<-TK;+TV;>;)V";
          break;
        case 8:
          Target.generic_signature = "(Lfixture/Carrier<*+TV;>;)V";
          break;
        case 9:
          Target.generic_signature = "(Lfixture/Carrier<+TMissing;+TV;>;)V";
          break;
        case 10:
          Target.generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/Carrier<+TK;+TV;>;)V";
          break;
        case 11:
          Allocation.access = {"private", "constructor"};
          break;
        case 12:
          Allocation.access.insert("varargs");
          break;
        case 13:
          Allocation.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 14:
          Allocation.generic_signature = "<T:Ljava/lang/Object;>(JD)V";
          break;
        case 15:
          Fresh.access.insert("abstract");
          break;
        case 16:
          Fresh.interfaces.clear();
          Fresh.generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 17:
          Core.generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Parent<*>;";
          break;
        case 18:
          Core.generic_signature = "<K:Ljava/lang/Object;V:Ljava/lang/"
                                   "Object;>Lfixture/Parent<TMissing;TV;>;";
          break;
        case 19:
          Parent.methods[1].generic_signature = "<X:Ljava/lang/Object;>(I)V";
          break;
        case 20:
          Target.access.insert("varargs");
          break;
        case 21:
          Fresh.methods[1].generic_signature = "<X:Ljava/lang/Object;>(DJ)V";
          break;
        case 22:
          Core.generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Object;C::Lfixture/"
              "Dependent<TMissing;>;>Lfixture/Parent<TK;TC;>;";
          break;
        case 23:
          Core.generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Object;C::Lfixture/"
              "Dependent<+TV;>;>Lfixture/Parent<TK;TC;>;";
          break;
        case 24:
          Core.generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Object;C::Lfixture/"
              "Dependent<TC;>;>Lfixture/Parent<TK;TC;>;";
          break;
        case 25:
          Classes[5].access.erase("interface");
          break;
        case 26:
          Classes[5].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 27:
          Classes[5].generic_signature =
              "<A:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 28:
          Classes.erase(Classes.begin() + 5);
          break;
        case 29:
          Core.generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Object;C:Ljava/lang/"
              "Object;:Lfixture/Dependent<TV;>;>Lfixture/Parent<TK;TC;>;";
          break;
        case 30:
          Core.generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Object;C:Lfixture/"
              "Dependent<TV;>;>Lfixture/Parent<TK;TC;>;";
          break;
        case 31:
          Core.generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Number;C::Lfixture/"
              "Dependent<TV;>;>Lfixture/Parent<TK;TC;>;";
          Classes.push_back(klass("Ljava/lang/Number;", true));
          break;
        case 32:
          Core.generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Object;C::Lfixture/"
              "Dependent<Ljava/lang/String;>;>Lfixture/Parent<TK;TC;>;";
          Classes.push_back(klass("Ljava/lang/String;", true));
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       auto C = input(Classes, Route, B);
                       recoverJava(C, B);
                     })(),
                     std::runtime_error);
      }
}
