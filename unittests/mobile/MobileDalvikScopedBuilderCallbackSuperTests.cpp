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
std::vector<Class> fixture(bool ParentSDK, bool BuilderSDK, bool CarrierSDK,
                           bool Range, bool Alpha, bool Synthetic,
                           bool Overload, unsigned Casts) {
  const std::string Object = "Ljava/lang/Object;", Map = "Lfixture/Map;",
                    Function = "Lfixture/Function;",
                    Marker = "Lfixture/Marker;", K = Alpha ? "ChildKey" : "K",
                    V = Alpha ? "ChildValue" : "V";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  const MethodRef RootInit{Object, "<init>", {}, "V"};
  Root.methods = {method(RootInit, {"public", "constructor"}, 0, {})};
  auto Parent = klass("Lfixture/Parent;", ParentSDK);
  Parent.generic_signature =
      "<P:Ljava/lang/Object;Q:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Init = method({Parent.name, "<init>", {Map, Function}, "V"},
                     {"public", "constructor"}, ParentSDK ? 0 : 3,
                     ParentSDK ? std::vector<Instruction>{}
                               : std::vector<Instruction>{
                                     op(0, "invoke-direct", {0}, RootInit),
                                     op(1, "return-void")});
  Init.generic_signature =
      "(Lfixture/Map<TP;TQ;>;Lfixture/Function<Ljava/lang/Object;TP;>;)V";
  Parent.methods = {Init};
  if (Overload) {
    auto Other = Init;
    Other.reference.parameters[1] = "I";
    Other.generic_signature = "(Lfixture/Map<TP;TQ;>;I)V";
    Parent.methods.push_back(Other);
  }
  auto Builder = klass("Lfixture/Builder;", BuilderSDK);
  Builder.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Build = method({Builder.name, "build", {}, Map}, {"public"},
                      BuilderSDK ? 0 : 2, {});
  Build.generic_signature = "()Lfixture/Map<TA;TB;>;";
  if (!BuilderSDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Build.instructions = {Zero, op(1, "return-object", {0})};
    Build.code_end = 2;
  }
  auto BuilderInit = method(
      {Builder.name, "<init>", {Map}, "V"}, {"public", "constructor"},
      BuilderSDK ? 0 : 2,
      BuilderSDK
          ? std::vector<Instruction>{}
          : std::vector<Instruction>{op(0, "invoke-direct", {0}, RootInit),
                                     op(1, "return-void")});
  BuilderInit.generic_signature = "(Lfixture/Map<TA;TB;>;)V";
  Builder.methods = {Build, BuilderInit};
  auto Adapter = klass("Lfixture/Adapter;");
  Adapter.access = {"public", "final"};
  Adapter.interfaces = {Function, Marker};
  auto Allocate =
      method({Adapter.name, "<init>", {}, "V"}, {"public", "constructor"}, 1,
             {op(0, "invoke-direct", {0}, RootInit), op(1, "return-void")});
  if (Synthetic) {
    Adapter.access.insert("synthetic");
    Allocate.access.insert("synthetic");
  }
  Adapter.methods = {Allocate,
                     method({Adapter.name, "apply", {Object}, Object},
                            {"public"}, 2, {op(0, "return-object", {1})})};
  auto Core = klass("Lfixture/Core;");
  Core.superclass = Parent.name;
  Core.generic_signature = "<" + K + ":Ljava/lang/Object;" + V +
                           ":Ljava/lang/Object;>Lfixture/Parent<T" + K + ";T" +
                           V + ";>;";
  const unsigned Self = Range ? 4 : 2, Input = Self + 1, Result = Range ? 1 : 0,
                 Callback = Range ? 2 : 1;
  auto Caller = method({Core.name, "<init>", {Builder.name}, "V"},
                       {"constructor"}, Self + 2,
                       {op(0, Range ? "invoke-virtual/range" : "invoke-virtual",
                           {Input}, Build.reference),
                        op(3, "move-result-object", {Result}),
                        op(4, "new-instance", {Callback}, Adapter.name),
                        op(6, Range ? "invoke-direct/range" : "invoke-direct",
                           {Callback}, Allocate.reference)});
  Caller.generic_signature = "(Lfixture/Builder<T" + K + ";T" + V + ";>;)V";
  if (Casts == 2)
    Caller.instructions.push_back(op(8, "check-cast", {Callback}, Object));
  if (Casts)
    Caller.instructions.push_back(op(9, "check-cast", {Callback}, Marker));
  Caller.instructions.push_back(op(11, "check-cast", {Callback}, Function));
  if (Range)
    Caller.instructions.push_back(op(12, "move-object", {0, Self}));
  Caller.instructions.push_back(
      op(13, Range ? "invoke-direct/range" : "invoke-direct",
         {Range ? 0u : Self, Result, Callback}, Init.reference));
  Caller.instructions.push_back(op(16, "return-void"));
  Caller.code_end = 17;
  Core.methods = {Caller};
  auto MapClass = klass(Map, CarrierSDK);
  MapClass.access = {"public", "interface", "abstract"};
  MapClass.generic_signature =
      "<L:Ljava/lang/Object;R:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Fn = MapClass;
  Fn.name = Function;
  auto Apply = method({Function, "apply", {Object}, Object},
                      {"public", "abstract"}, 0, {});
  Apply.generic_signature = "(TL;)TR;";
  Fn.methods = {Apply};
  auto Tag = klass(Marker, CarrierSDK);
  Tag.access = {"public", "interface", "abstract"};
  return {Parent, Core, Builder, Adapter, MapClass, Fn, Tag, Root};
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

TEST(MobileDalvikScopedBuilderCallbackSuper,
     KeepVirtualScopedResultRawAllocationUpcastsAndPrimitiveSeparatedCtor) {
  for (bool ParentSDK : {false, true})
    for (bool BuilderSDK : {false, true})
      for (bool CarrierSDK : {false, true})
        for (bool Range : {false, true})
          for (bool Alpha : {false, true})
            for (bool Synthetic : {false, true})
              for (bool Overload : {false, true})
                for (unsigned Casts : {0u, 1u, 2u})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{ParentSDK, BuilderSDK, CarrierSDK, Range,
                                   Alpha, Synthetic, Overload, Casts, Route}));
                    ASSERT_NO_THROW(([&] {
                      Budget B;
                      auto C = input(fixture(ParentSDK, BuilderSDK, CarrierSDK,
                                             Range, Alpha, Synthetic, Overload,
                                             Casts),
                                     Route, B);
                      const auto Caller = C.at("Lfixture/Core;").methods[0],
                                 Target = C.at("Lfixture/Parent;").methods[0];
                      const auto Plans = validateGenericSignatures(C, B);
                      EXPECT_TRUE(Plans.superclass_calls.contains(
                          {Caller.reference, Target.reference}));
                      const auto R = recoverJava(C, B);
                      EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                      EXPECT_EQ(
                          R.getInteger("minimum_java_release").value_or(8), 25);
                      EXPECT_FALSE(R.getArray("generic_signature_projections"));
                      EXPECT_EQ(C.at(Caller.reference.owner)
                                    .methods[0]
                                    .generic_signature,
                                Caller.generic_signature);
                    })());
                  }
  // A synthetic flag cannot change the overload selected by the same
  // complete casted argument types. Cover the original Object sibling
  // beside both source/SDK declarations and both initializer flag states.
  for (bool SDK : {false, true})
    for (bool Synthetic : {false, true})
      for (bool Range : {false, true})
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{SDK, Synthetic, Range, Route}));
          auto Original =
              fixture(SDK, SDK, true, Range, false, Synthetic, true, 1);
          auto &Sibling = Original[0].methods[1];
          Sibling.reference.parameters[1] = "Ljava/lang/Object;";
          Sibling.generic_signature =
              "(Lfixture/Map<TP;TQ;>;Ljava/lang/Object;)V";
          const auto Caller = Original[1].methods[0].reference,
                     Target = Original[0].methods[0].reference;
          const auto Flags = Original[3].methods[0].access;
          Budget B;
          auto Classes = input(Original, Route, B);
          const auto Plans = validateGenericSignatures(Classes, B);
          EXPECT_TRUE(Plans.superclass_calls.contains({Caller, Target}));
          const auto Report = recoverJava(Classes, B);
          EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
          EXPECT_EQ(Classes.at(Original[3].name).methods[0].access, Flags);
        }
}

TEST(MobileDalvikScopedBuilderCallbackSuper,
     RejectUnprovedCastTargetsReferenceSiblingsResultsAndAllocationScopes) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto C = fixture(SDK, SDK, true, false, false, true, true, 1);
        auto &Parent = C[0], &Core = C[1], &Builder = C[2], &Adapter = C[3];
        auto &Caller = Core.methods[0], &Build = Builder.methods[0];
        switch (Mutation) {
        case 0:
          Parent.methods[1].reference.parameters[1] = "Ljava/lang/Object;";
          Parent.methods[1].generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/Map<TP;TQ;>;TX;)V";
          break;
        case 1:
          Adapter.interfaces.pop_back();
          break;
        case 2:
          Adapter.interfaces.erase(Adapter.interfaces.begin());
          break;
        case 3:
          Caller.instructions[4].reference = std::string("Lfixture/Missing;");
          break;
        case 4:
          Adapter.generic_signature =
              "Ljava/lang/Object;Lfixture/Function<Ljava/lang/Object;Ljava/"
              "lang/Object;>;Lfixture/Marker;";
          break;
        case 5:
          Adapter.methods[0].access = {"private", "constructor"};
          break;
        case 6:
          Adapter.methods[0].declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 7:
          Adapter.methods[0].generic_signature = "<X:Ljava/lang/Object;>()V";
          break;
        case 8:
          Adapter.access.insert("abstract");
          break;
        case 9:
          Build.generic_signature = "()Lfixture/Map<TB;TA;>;";
          break;
        case 10:
          Build.generic_signature =
              "<X:Ljava/lang/Object;>()Lfixture/Map<TA;TB;>;";
          break;
        case 11:
          Build.access.insert("static");
          break;
        case 12:
          Build.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 13:
          Caller.generic_signature = "(Lfixture/Builder<TMissing;TV;>;)V";
          break;
        case 14:
          Caller.instructions[4].registers[0] = 2;
          break;
        case 15:
          Caller.instructions[6].registers[0] = 1;
          break;
        case 16:
          Caller.instructions[6].registers[1] = 1;
          break;
        case 17:
          Parent.methods[1].generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/Map<TP;TQ;>;I)V";
          break;
        case 18:
          Caller.instructions[4].target = 11;
          break;
        case 19:
          Caller.instructions[1].registers[0] = 1;
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       auto Classes = input(C, Route, B);
                       recoverJava(Classes, B);
                     })(),
                     std::runtime_error);
      }
}
