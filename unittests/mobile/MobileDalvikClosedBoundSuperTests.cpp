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
std::vector<Class> fixture(bool SDK, bool Range, bool Copy, bool Alpha,
                           bool Package, bool ManagerFirst, bool ChildFormal,
                           bool Sibling) {
  const std::string Object = "Ljava/lang/Object;",
                    Service = "Lfixture/Service;",
                    Loader = "Ljava/lang/ClassLoader;",
                    M = Alpha ? "Manager" : "M";
  auto Parent = klass("Lfixture/Parent;", SDK);
  Parent.generic_signature = "<" + M + "::" + Service + ">Ljava/lang/Object;";
  auto Sink = method(
      {Parent.name, "<init>", {Service}, "V"}, {"protected", "constructor"},
      SDK ? 0 : 2,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{op(0, "invoke-direct", {0},
                                        MethodRef{Object, "<init>", {}, "V"}),
                                     op(1, "return-void")});
  Sink.generic_signature = "(T" + M + ";)V";
  Parent.methods.push_back(Sink);
  if (Sibling) {
    auto Other = Sink;
    Other.reference.parameters[0] = "Ljava/lang/String;";
    Other.generic_signature.reset();
    Parent.methods.push_back(Other);
  }
  auto Child = klass("Lfixture/Child;");
  Child.superclass = Parent.name;
  Child.generic_signature =
      std::string(ChildFormal ? "<T:Ljava/lang/Object;>" : "") +
      "Lfixture/Parent<" + Service + ">;";
  if (Package)
    Child.access.clear();
  std::vector<Instruction> Code;
  const unsigned Manager = ManagerFirst ? 4 : 5;
  if (Range || Copy) {
    Code.push_back(op(Code.size(), "move-object", {0, 3}));
    if (Copy)
      Code.push_back(op(Code.size(), "move-object", {2, Manager}));
    Code.push_back(op(Code.size(), "move-object", {1, Copy ? 2 : Manager}));
  }
  Code.push_back(op(Code.size(),
                    Range ? "invoke-direct/range" : "invoke-direct",
                    Range || Copy ? std::vector<unsigned>{0, 1}
                                  : std::vector<unsigned>{3, Manager},
                    Sink.reference));
  Code.push_back(op(Code.size(), "return-void"));
  Child.methods.push_back(
      method({Child.name, "<init>",
              ManagerFirst ? std::vector<std::string>{Service, Loader}
                           : std::vector<std::string>{Loader, Service},
              "V"},
             {"protected", "constructor"}, 6, Code));
  auto ServiceClass = klass(Service, true);
  ServiceClass.access = {"public", "interface", "abstract"};
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  return {Child,        Parent,
          ServiceClass, klass(Loader, true),
          Root,         klass("Ljava/lang/String;", true)};
}
} // namespace

TEST(MobileDalvikClosedBoundSuper,
     RetainClosedNominalBoundsAndIndependentDescriptorContext) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Copy : {false, true})
        for (bool Alpha : {false, true})
          for (bool Package : {false, true})
            for (bool ManagerFirst : {false, true})
              for (bool ChildFormal : {false, true})
                for (bool Sibling : {false, true})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, Range, Copy, Alpha, Package,
                                   ManagerFirst, ChildFormal, Sibling, Route}));
                    auto Input = fixture(SDK, Range, Copy, Alpha, Package,
                                         ManagerFirst, ChildFormal, Sibling);
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
                    EXPECT_TRUE(
                        Plans.superclass_calls.contains({Caller, Target}));
                    for (const auto &C : Original) {
                      const auto &After = Classes.at(C.name);
                      EXPECT_EQ(After.access, C.access);
                      EXPECT_EQ(After.generic_signature, C.generic_signature);
                      for (size_t I = 0; I < C.methods.size(); ++I) {
                        EXPECT_EQ(After.methods[I].reference,
                                  C.methods[I].reference);
                        EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                        EXPECT_EQ(After.methods[I].generic_signature,
                                  C.methods[I].generic_signature);
                      }
                    }
                    const auto Report = recoverJava(Classes, B);
                    EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(
                        Report.getArray("generic_signature_projections"));
                  }
}

TEST(MobileDalvikClosedBoundSuper,
     RejectChangedBoundsScopesDescriptorsContextsAndConstructorRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 21; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, true, false, false, false, false, false);
      auto &Caller = Input[0].methods[0], &Target = Input[1].methods[0];
      if (Mutation == 0)
        Caller.instructions[1].registers[1] = 4;
      else if (Mutation == 1)
        Caller.instructions[3].registers[0] = 1;
      else if (Mutation == 2)
        Caller.instructions[3].registers.pop_back();
      else if (Mutation == 3)
        Target.generic_signature = "<M::Lfixture/Service;>(TM;)V";
      else if (Mutation == 4)
        Target.generic_signature = "(TMissing;)V";
      else if (Mutation == 5)
        Target.generic_signature = "(Ljava/lang/Object;)V";
      else if (Mutation == 6)
        Input[1].generic_signature = "<M:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 7)
        Input[0].generic_signature = "Lfixture/Parent;";
      else if (Mutation == 8)
        Input[0].generic_signature = "Lfixture/Parent<TMissing;>;";
      else if (Mutation == 9)
        Input[0].generic_signature = "Lfixture/Parent<Ljava/lang/String;>;";
      else if (Mutation == 10)
        Target.access.insert("private");
      else if (Mutation == 11)
        Target.access.insert("static");
      else if (Mutation == 12)
        Target.access.insert("abstract");
      else if (Mutation == 13)
        Target.access.insert("native");
      else if (Mutation == 14)
        Target.access.insert("varargs");
      else if (Mutation == 15)
        Target.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 16) {
        auto Other = Target;
        Other.reference.parameters[0] = "Ljava/lang/String;";
        Other.generic_signature = "<U:Ljava/lang/Object;>(Ljava/lang/String;)V";
        Input[1].methods.push_back(Other);
      } else if (Mutation == 17)
        Caller.reference.parameters[1] = "Ljava/lang/Object;";
      else if (Mutation == 18) {
        auto Wrong = op(1, "const/4", {2});
        Wrong.literal = int64_t(1);
        Caller.instructions[1] = Wrong;
      } else if (Mutation == 19)
        --Caller.registers;
      else
        Target.generic_signature = "(TM;)I";
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
