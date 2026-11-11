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
std::vector<Class> fixture(bool ParentSDK, bool FieldSDK, bool Range,
                           unsigned Variant) {
  const std::string Object = "Ljava/lang/Object;", Outer = "Lfixture/Outer;",
                    Inner = "Lfixture/Inner;", Marker = "Lfixture/Marker;",
                    K = Variant & 1 ? "Key" : "K",
                    V = Variant & 1 ? "Value" : "V";
  const std::string First = Variant & 2 ? V : K, Second = Variant & 2 ? K : V;
  auto Parent = klass("Lfixture/Base;", ParentSDK);
  Parent.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  MethodRef RootInit{Object, "<init>", {}, "V"};
  auto Target = method({Parent.name, "<init>", {Outer, Marker}, "V"},
                       {"public", "constructor"}, ParentSDK ? 0 : 3,
                       ParentSDK ? std::vector<Instruction>{}
                                 : std::vector<Instruction>{
                                       op(0, "invoke-direct", {0}, RootInit),
                                       op(3, "return-void")});
  Target.generic_signature =
      "(Lfixture/Outer<Lfixture/Inner<TA;>;TB;>;Lfixture/Marker;)V";
  Parent.methods.push_back(Target);
  auto Holder = klass("Lfixture/Holder;", FieldSDK);
  FieldRef Read{Holder.name, "selected", Marker};
  Holder.fields.push_back(
      {Read, {"public", "static", Variant & 4 ? "final" : "volatile"}, {}});
  auto Value = klass(Marker, FieldSDK);
  auto Pair = klass(Outer, Variant & 8);
  Pair.generic_signature =
      "<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Unary = klass(Inner, Variant & 8);
  Unary.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Child = klass("Lfixture/Child;");
  Child.superclass = Parent.name;
  Child.generic_signature = "<" + K + ":Ljava/lang/Object;" + V +
                            ":Ljava/lang/Object;>Lfixture/Base<T" + First +
                            ";T" + Second + ";>;";
  auto Caller = method({Child.name, "<init>", {Outer}, "V"},
                       {"public", "constructor"}, 5, {});
  Caller.generic_signature =
      "(Lfixture/Outer<Lfixture/Inner<T" + First + ";>;T" + Second + ";>;)V";
  Caller.instructions = {op(0, "sget-object", {0}, Read),
                         op(2, "move-object", {2, 0}),
                         op(3, "move-object", {0, 3}),
                         op(4, "move-object", {1, 4}),
                         op(5, Range ? "invoke-direct/range" : "invoke-direct",
                            {0, 1, 2}, Target.reference),
                         op(8, "return-void")};
  Caller.code_end = 9;
  Child.methods.push_back(Caller);
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(method(RootInit, {"public", "constructor"}, 0, {}));
  return {Child, Parent, Holder, Value, Pair, Unary, Root};
}
} // namespace

TEST(MobileDalvikScopedStaticFieldSuper,
     RetainStaticReadSnapshotsNestedOwnerFormalsCopiesAndExactDelegation) {
  for (bool ParentSDK : {false, true})
    for (bool FieldSDK : {false, true})
      for (bool Range : {false, true})
        for (unsigned Variant = 0; Variant < 16; ++Variant)
          for (unsigned Route : {0u, 1u, 2u}) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{ParentSDK, FieldSDK, Range, Variant, Route}));
            auto Input = fixture(ParentSDK, FieldSDK, Range, Variant);
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
            EXPECT_EQ(Plans.superclass_call_declarations.at({Caller, Target}),
                      Target);
            const auto Report = recoverJava(Classes, B);
            EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
            EXPECT_FALSE(Report.getArray("generic_signature_projections"));
            for (const auto &C : Original) {
              const auto &After = Classes.at(C.name);
              EXPECT_EQ(After.access, C.access);
              EXPECT_EQ(After.generic_signature, C.generic_signature);
              for (size_t I = 0; I < C.fields.size(); ++I) {
                EXPECT_EQ(After.fields[I].reference, C.fields[I].reference);
                EXPECT_EQ(After.fields[I].access, C.fields[I].access);
                EXPECT_EQ(After.fields[I].generic_signature,
                          C.fields[I].generic_signature);
              }
              for (size_t I = 0; I < C.methods.size(); ++I) {
                EXPECT_EQ(After.methods[I].reference, C.methods[I].reference);
                EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                EXPECT_EQ(After.methods[I].generic_signature,
                          C.methods[I].generic_signature);
              }
            }
          }
}

TEST(MobileDalvikScopedStaticFieldSuper,
     RejectUnprovedFieldDeclarationsNestedBindingsAndConstructorWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 27; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, SDK, true, 0);
      auto &Caller = Input[0].methods[0], &Target = Input[1].methods[0];
      if (Mutation == 0)
        Input[2].fields.clear();
      else if (Mutation == 1)
        Input[2].fields[0].access.erase("static");
      else if (Mutation == 2) {
        Input[2].fields[0].access.erase("public");
        Input[2].fields[0].access.insert("private");
      } else if (Mutation == 3)
        Input[2].fields[0].generic_signature = "Lfixture/Marker;";
      else if (Mutation == 4)
        Input.erase(Input.begin() + 3);
      else if (Mutation == 5)
        Input[3].generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 6) {
        Input[2].access.erase("public");
        Input[2].access.insert("private");
      } else if (Mutation == 7)
        Caller.instructions[0].registers = {4};
      else if (Mutation == 8)
        Caller.instructions[0].opcode = "sget";
      else if (Mutation == 9)
        std::get<FieldRef>(Caller.instructions[0].reference).type =
            "Ljava/lang/Object;";
      else if (Mutation == 10)
        Target.generic_signature =
            "(Lfixture/Outer<Lfixture/Inner<TB;>;TB;>;Lfixture/Marker;)V";
      else if (Mutation == 11)
        Target.generic_signature =
            "(Lfixture/Outer<Lfixture/Inner<TMissing;>;TB;>;Lfixture/Marker;)V";
      else if (Mutation == 12)
        Target.generic_signature =
            "(Lfixture/Outer<Lfixture/Inner<+TA;>;TB;>;Lfixture/Marker;)V";
      else if (Mutation == 13)
        Target.generic_signature =
            "<M:Ljava/lang/Object;>(Lfixture/Outer<Lfixture/"
            "Inner<TA;>;TB;>;Lfixture/Marker;)V";
      else if (Mutation == 14)
        Target.access.insert("varargs");
      else if (Mutation == 15) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 16)
        Target.access.insert("native");
      else if (Mutation == 17)
        Target.access.insert("static");
      else if (Mutation == 18)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/RuntimeException;"};
      else if (Mutation == 19)
        Caller.instructions[4].registers.pop_back();
      else if (Mutation == 20)
        Caller.instructions[4].opcode = "invoke-virtual";
      else if (Mutation == 21)
        Input[5].generic_signature =
            "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 22)
        Input[4].generic_signature = "<A:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 23)
        Input[0].generic_signature =
            "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Lfixture/Base;";
      else if (Mutation == 24)
        Caller.instructions[1].registers[1] = 1;
      else if (Mutation == 25) {
        auto Sibling = Target;
        Sibling.reference.parameters[0] = "Ljava/lang/Object;";
        Sibling.generic_signature =
            "<M:Ljava/lang/Object;>(TM;Lfixture/Marker;)V";
        Input[1].methods.push_back(Sibling);
      } else
        std::get<FieldRef>(Caller.instructions[0].reference).owner =
            "Lfixture/Missing;";
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
