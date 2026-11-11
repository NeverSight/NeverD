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
std::vector<Class> fixture(bool FactorySDK, bool ParentSDK, bool Range,
                           bool Alpha, bool Synthetic) {
  const std::string Object = "Ljava/lang/Object;",
                    String = "Ljava/lang/String;",
                    Callback = "Lfixture/Callback;",
                    T = Alpha ? "Element" : "T";
  auto Factory = klass("Lfixture/Provider;", FactorySDK);
  MethodRef Wrap{Factory.name, "wrap", {Object}, String};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  Factory.methods.push_back(method(
      Wrap, {"public", "static"}, FactorySDK ? 0 : 2,
      FactorySDK
          ? std::vector<Instruction>{}
          : std::vector<Instruction>{Zero, op(1, "return-object", {0})}));
  auto Parent = klass("Lfixture/Base;", ParentSDK);
  Parent.generic_signature = "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  MethodRef Super{
      Parent.name, "<init>", {Object, Object, Callback, "Z", "Z"}, "V"};
  MethodRef RootInit{Object, "<init>", {}, "V"};
  auto Init = method(Super, {"public"}, ParentSDK ? 0 : 6,
                     ParentSDK ? std::vector<Instruction>{}
                               : std::vector<Instruction>{
                                     op(0, "invoke-direct", {0}, RootInit),
                                     op(3, "return-void")});
  Init.generic_signature =
      "(T" + T + ";T" + T + ";Lfixture/Callback<T" + T + ";>;ZZ)V";
  Parent.methods.push_back(Init);
  auto Adapter = klass("Lfixture/Adapter;");
  Adapter.access.insert("final");
  Adapter.interfaces = {Callback};
  MethodRef Capture{Adapter.name, "<init>", {Object, Object, "I"}, "V"};
  auto Captured =
      method(Capture, {"public"}, 4,
             {op(0, "invoke-direct", {0}, RootInit), op(3, "return-void")});
  if (Synthetic) {
    Adapter.access.insert("synthetic");
    Captured.access.insert("synthetic");
  }
  Adapter.methods.push_back(Captured);
  Adapter.methods.push_back(
      method({Adapter.name, "step", {Object, Object, "I"}, Object}, {"public"},
             4, {op(0, "return-object", {1})}));
  auto Child = klass("Lfixture/Child;");
  Child.superclass = Parent.name;
  Child.generic_signature = "Lfixture/Base<Ljava/lang/String;>;";
  Child.methods.push_back(method(
      {Child.name, "<init>", {Object, Object, Object, "I", "Z", "Z"}, "V"},
      {"public"}, 13,
      {op(0, Range ? "invoke-static/range" : "invoke-static", {7}, Wrap),
       op(3, "move-result-object", {1}),
       op(4, Range ? "invoke-static/range" : "invoke-static", {8}, Wrap),
       op(7, "move-result-object", {2}),
       op(8, "new-instance", {3}, Adapter.name),
       op(10, "invoke-direct", {3, 7, 9, 10}, Capture),
       op(13, "move-object", {0, 6}), op(14, "move", {4, 11}),
       op(15, "move", {5, 12}),
       op(16, Range ? "invoke-direct/range" : "invoke-direct",
          {0, 1, 2, 3, 4, 5}, Super),
       op(19, "return-void")}));
  auto Interface = klass(Callback, true);
  Interface.access = {"public", "interface", "abstract"};
  Interface.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Step = method({Callback, "step", {Object, Object, "I"}, Object},
                     {"public", "abstract"}, 0, {});
  Step.generic_signature = "(TC;TC;I)TC;";
  Interface.methods.push_back(Step);
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(method(RootInit, {"public"}, 0, {}));
  auto Strings = klass(String, true);
  Strings.access.insert("final");
  auto Number = klass("Ljava/lang/Number;", true);
  Number.access.insert("abstract");
  return {Child, Parent, Factory, Adapter, Interface, Root, Strings, Number};
}
} // namespace

TEST(MobileDalvikConstructorStaticNominal,
     RetainClosedParentFormalsStaticResultsCapturesAndOriginalScopes) {
  for (bool FactorySDK : {false, true})
    for (bool ParentSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Synthetic : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(std::tuple{
                  FactorySDK, ParentSDK, Range, Alpha, Synthetic, Route}));
              auto Input =
                  fixture(FactorySDK, ParentSDK, Range, Alpha, Synthetic);
              const auto Original = Input;
              const auto Caller = Input[0].methods[0].reference,
                         Target = Input[1].methods[0].reference;
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
              EXPECT_TRUE(Plans.superclass_calls.contains({Caller, Target}));
              for (const auto &C : Original) {
                const auto &After = Classes.at(C.name);
                EXPECT_EQ(After.access, C.access);
                EXPECT_EQ(After.generic_signature, C.generic_signature);
                for (size_t I = 0; I < C.methods.size(); ++I) {
                  EXPECT_EQ(After.methods[I].reference, C.methods[I].reference);
                  EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                  EXPECT_EQ(After.methods[I].generic_signature,
                            C.methods[I].generic_signature);
                  EXPECT_EQ(After.methods[I].declared_throws,
                            C.methods[I].declared_throws);
                }
              }
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikConstructorStaticNominal,
     RejectUnknownResultBindingsOverloadsScopesAndChangedWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, SDK, true, false, true);
      auto &Caller = Input[0].methods[0];
      auto &Target = Input[1].methods[0];
      auto &Factory = Input[2].methods[0];
      if (Mutation == 0) {
        Factory.reference.returns = "Ljava/lang/Object;";
        std::get<MethodRef>(Caller.instructions[0].reference).returns =
            Factory.reference.returns;
        std::get<MethodRef>(Caller.instructions[2].reference).returns =
            Factory.reference.returns;
      } else if (Mutation == 1)
        Factory.generic_signature =
            "<M:Ljava/lang/Object;>(TM;)Ljava/lang/String;";
      else if (Mutation == 2) {
        Factory.access.erase("public");
        Factory.access.insert("private");
      } else if (Mutation == 3)
        Factory.declared_throws = {"Ljava/lang/RuntimeException;"};
      else if (Mutation == 4)
        Input[1].generic_signature = "<T:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 5)
        Input[0].generic_signature = "Lfixture/Base;";
      else if (Mutation == 6)
        Input[0].generic_signature = "Lfixture/Base<TMissing;>;";
      else if (Mutation == 7)
        Target.generic_signature = "(TMissing;TT;Lfixture/Callback<TT;>;ZZ)V";
      else if (Mutation == 8)
        Target.generic_signature =
            "<M:Ljava/lang/Object;>(TM;TM;Lfixture/Callback<TM;>;ZZ)V";
      else if (Mutation == 9)
        Target.access.insert("varargs");
      else if (Mutation == 10)
        Target.access.insert("native");
      else if (Mutation == 11)
        Target.access.insert("static");
      else if (Mutation == 12) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 13)
        Caller.instructions[0].registers.clear();
      else if (Mutation == 14)
        Caller.instructions[1].opcode = "move-result";
      else if (Mutation == 15)
        Input[3].generic_signature =
            "<A:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Callback<TA;>;";
      else if (Mutation == 16)
        Input[3].methods[0].generic_signature =
            "<K:Ljava/lang/Object;>(Ljava/lang/Object;Ljava/lang/Object;I)V";
      else if (Mutation == 17)
        Input[4].generic_signature = "<C:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 18)
        Caller.reference.parameters.back() = "I";
      else {
        auto Sibling = Target;
        Sibling.reference.parameters[2] = "Ljava/lang/Object;";
        Sibling.generic_signature = "(TT;TT;Ljava/lang/Object;ZZ)V";
        Input[1].methods.push_back(Sibling);
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
