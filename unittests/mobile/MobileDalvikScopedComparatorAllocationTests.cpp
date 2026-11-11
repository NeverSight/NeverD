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
                           unsigned Variance, bool Covariant) {
  const std::string Object = "Ljava/lang/Object;", Map = "Lfixture/Map;",
                    Compare = "Lfixture/Compare;", K = Alpha ? "Key" : "K",
                    V = Alpha ? "Value" : "V",
                    ArgVariance = Variance == 0   ? ""
                                  : Variance == 1 ? "-"
                                                  : "+",
                    ParentVariance = Covariant ? "+" : "";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  const MethodRef RootInit{Object, "<init>", {}, "V"};
  Root.methods = {method(RootInit, {"public", "constructor"}, 0, {})};
  auto Parent = klass("Lfixture/Parent;", SDK);
  Parent.generic_signature =
      "<P:Ljava/lang/Object;Q:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Init = method(
      {Parent.name, "<init>", {Map}, "V"}, {"public", "constructor"},
      SDK ? 0 : 2,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{op(0, "invoke-direct", {0}, RootInit),
                                     op(1, "return-void")});
  Init.generic_signature =
      "(Lfixture/Map<" + ParentVariance + "TP;" + ParentVariance + "TQ;>;)V";
  auto Other = Init;
  Other.reference.parameters = {"I"};
  Other.generic_signature.reset();
  Parent.methods = {Init, Other};
  auto Fresh = klass("Lfixture/Fresh;", SDK);
  Fresh.interfaces = {Map};
  Fresh.generic_signature = "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/"
                            "lang/Object;Lfixture/Map<TA;TB;>;";
  auto Allocate = method(
      {Fresh.name, "<init>", {Compare}, "V"}, {"public", "constructor"},
      SDK ? 0 : 2,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{op(0, "invoke-direct", {0}, RootInit),
                                     op(1, "return-void")});
  Allocate.generic_signature = "(Lfixture/Compare<" + ArgVariance + "TA;>;)V";
  auto MapInit = Allocate;
  MapInit.reference.parameters = {Map};
  MapInit.generic_signature = "(Lfixture/Map<+TA;+TB;>;)V";
  Fresh.methods = {Allocate, MapInit};
  auto Core = klass("Lfixture/Core;");
  Core.superclass = Parent.name;
  Core.generic_signature = "<" + K + ":Ljava/lang/Object;" + V +
                           ":Ljava/lang/Object;>Lfixture/Parent<T" + K + ";T" +
                           V + ";>;";
  const unsigned Self = Range ? 2 : 1, Input = Self + 1;
  auto Caller = method({Core.name, "<init>", {Compare}, "V"},
                       {"public", "constructor"}, Self + 2, {});
  Caller.generic_signature =
      "(Lfixture/Compare<" + ArgVariance + "T" + K + ";>;)V";
  if (Range)
    Caller.instructions.push_back(op(0, "move-object", {1, Input}));
  Caller.instructions.push_back(op(1, "new-instance", {0}, Fresh.name));
  Caller.instructions.push_back(
      op(3, Range ? "invoke-direct/range" : "invoke-direct",
         {0, Range ? 1u : Input}, Allocate.reference));
  if (Range)
    Caller.instructions.push_back(op(6, "move-object", {Input, 0}));
  Caller.instructions.push_back(
      op(7, Range ? "invoke-direct/range" : "invoke-direct",
         {Self, Range ? Input : 0}, Init.reference));
  Caller.instructions.push_back(op(10, "return-void"));
  Caller.code_end = 11;
  Core.methods = {Caller};
  auto MapClass = klass(Map, CarrierSDK);
  MapClass.access = {"public", "interface", "abstract"};
  MapClass.generic_signature =
      "<L:Ljava/lang/Object;R:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Comparison = klass(Compare, CarrierSDK);
  Comparison.access = {"public", "interface", "abstract"};
  Comparison.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  auto CompareMethod = method({Compare, "compare", {Object, Object}, "I"},
                              {"public", "abstract"}, 0, {});
  CompareMethod.generic_signature = "(TT;TT;)I";
  Comparison.methods = {CompareMethod};
  return {Parent, Core, Fresh, MapClass, Comparison, Root};
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

TEST(MobileDalvikScopedComparatorAllocation,
     KeepExactIncomingCarrierRawInitializerAndUnusedAllocationFormal) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (unsigned Variance : {0u, 1u, 2u})
            for (bool Covariant : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, CarrierSDK, Range, Alpha, Variance,
                               Covariant, Route}));
                ASSERT_NO_THROW(([&] {
                  Budget B;
                  auto C = input(fixture(SDK, CarrierSDK, Range, Alpha,
                                         Variance, Covariant),
                                 Route, B);
                  const auto Caller = C.at("Lfixture/Core;").methods[0],
                             Target = C.at("Lfixture/Parent;").methods[0];
                  const auto Plans = validateGenericSignatures(C, B);
                  EXPECT_TRUE(Plans.superclass_calls.contains(
                      {Caller.reference, Target.reference}));
                  const auto R = recoverJava(C, B);
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_EQ(R.getInteger("minimum_java_release").value_or(8),
                            25);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                  EXPECT_EQ(
                      C.at(Caller.reference.owner).methods[0].generic_signature,
                      Caller.generic_signature);
                })());
              }
}

TEST(MobileDalvikScopedComparatorAllocation,
     RejectUnprovedInitializerScopesIncomingOriginsAndOverloads) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto C = fixture(SDK, true, false, false, 1, true);
        auto &Parent = C[0], &Core = C[1], &Fresh = C[2];
        auto &Caller = Core.methods[0], &Allocate = Fresh.methods[0];
        switch (Mutation) {
        case 0:
          Allocate.access = {"private", "constructor"};
          break;
        case 1:
          Allocate.generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/Compare<-TA;>;)V";
          break;
        case 2:
          Fresh.generic_signature = "<A:Ljava/lang/Object;B:Ljava/lang/Number;>"
                                    "Ljava/lang/Object;Lfixture/Map<TA;TB;>;";
          break;
        case 3:
          Fresh.generic_signature = "<A:Ljava/lang/Object;>Ljava/lang/"
                                    "Object;Lfixture/Map<TA;TB;>;";
          break;
        case 4:
          Caller.generic_signature = "(Lfixture/Compare<-TMissing;>;)V";
          break;
        case 5:
          Caller.generic_signature = "(Lfixture/Compare<*>;)V";
          break;
        case 6:
          Allocate.generic_signature = "(Lfixture/Compare<*>;)V";
          break;
        case 7:
          Allocate.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 8:
          Fresh.methods[1].access.insert("varargs");
          break;
        case 9:
          Fresh.methods[1].generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/Map<+TA;+TB;>;)V";
          break;
        case 10:
          Fresh.access.insert("abstract");
          break;
        case 11:
          Caller.instructions[1].registers[1] = 1;
          break;
        case 12: {
          auto Zero = op(0, "const/4", {2});
          Zero.literal = int64_t(0);
          Caller.instructions.insert(Caller.instructions.begin(), Zero);
          break;
        }
        case 13:
          Allocate.generic_signature = "(Lfixture/Compare<-TMissing;>;)V";
          break;
        case 14:
          Caller.generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/Compare<-TX;>;)V";
          break;
        case 15:
          Caller.reference.parameters = {"Lfixture/Map;"};
          Caller.generic_signature = "(Lfixture/Map<TK;TV;>;)V";
          break;
        case 16:
          Parent.methods[0].generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/Map<+TP;+TQ;>;)V";
          break;
        case 17:
          Fresh.interfaces.clear();
          Fresh.generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 18:
          Caller.instructions.insert(
              Caller.instructions.begin(),
              op(0, "check-cast", {2}, std::string("Lfixture/Compare;")));
          break;
        case 19:
          Caller.generic_signature =
              "(Lfixture/Compare<-Ljava/lang/Object;>;)V";
          break;
        }
        for (size_t At = 0; At < Caller.instructions.size(); ++At)
          Caller.instructions[At].pc = At;
        Caller.code_end = Caller.instructions.size();
        EXPECT_THROW(([&] {
                       Budget B;
                       auto Classes = input(C, Route, B);
                       recoverJava(Classes, B);
                     })(),
                     std::runtime_error);
      }
}
