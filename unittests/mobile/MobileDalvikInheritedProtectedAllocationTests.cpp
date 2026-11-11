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
std::vector<Class> fixture(bool SDK, bool Range, bool Final, bool Alpha,
                           bool StaticCaller, bool Deep) {
  const std::string Object = "Ljava/lang/Object;",
                    String = "Ljava/lang/String;", URI = "Ljava/net/URI;",
                    T = Alpha ? "Element" : "T";
  auto Parent = klass("Lbase/Parent;", SDK);
  Parent.access.insert("abstract");
  Parent.interfaces = {"Ljava/io/Serializable;"};
  Parent.generic_signature =
      "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;Ljava/io/Serializable;";
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target = method(
      {Parent.name, "text", {Object}, String}, {"protected"}, SDK ? 0 : 2,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  if (Final)
    Target.access.insert("final");
  Parent.methods.push_back(Target);
  auto constructor = [&](const std::string &Owner, const std::string &Base,
                         bool External) {
    return method({Owner, "<init>", {}, "V"}, {"public"}, External ? 0 : 1,
                  External ? std::vector<Instruction>{}
                           : std::vector<Instruction>{
                                 op(0, "invoke-direct", {0},
                                    MethodRef{Base, "<init>", {}, "V"}),
                                 op(3, "return-void")});
  };
  Parent.methods.push_back(constructor(Parent.name, Object, SDK));
  auto Mid = klass("Lmiddle/Step;", SDK);
  Mid.superclass = Parent.name;
  Mid.generic_signature = "<M:Ljava/lang/Object;>Lbase/Parent<TM;>;";
  Mid.methods.push_back(constructor(Mid.name, Parent.name, SDK));
  auto Child = klass("Lfixture/Child;");
  Child.superclass = Deep ? Mid.name : Parent.name;
  Child.generic_signature =
      Deep ? "Lmiddle/Step<Ljava/net/URI;>;" : "Lbase/Parent<Ljava/net/URI;>;";
  MethodRef Selected{Child.name, "text", {Object}, String};
  MethodRef Constructor{URI, "<init>", {String}, "V"};
  Access Flags{"public"};
  std::vector<std::string> Parameters{Object};
  if (StaticCaller) {
    Flags.insert("static");
    Parameters.insert(Parameters.begin(), Child.name);
  }
  Child.methods.push_back(
      method({Child.name, "make", Parameters, URI}, Flags, 3,
             {op(0, "new-instance", {0}, URI),
              op(2, Range ? "invoke-virtual/range" : "invoke-virtual", {1, 2},
                 Selected),
              op(5, "move-result-object", {2}),
              op(6, "invoke-direct", {0, 2}, Constructor),
              op(9, "return-object", {0})}));
  Child.methods.push_back(constructor(Child.name, *Child.superclass, false));
  auto UriClass = klass(URI, true);
  UriClass.access.insert("final");
  UriClass.methods.push_back(method(Constructor, {"public"}, 0, {}));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public"}, 0, {}));
  auto Serializable = klass("Ljava/io/Serializable;", true);
  Serializable.access = {"public", "interface", "abstract"};
  auto Number = klass("Ljava/lang/Number;", true);
  Number.access.insert("abstract");
  std::vector<Class> Out{Child, Parent,       UriClass, klass(String, true),
                         Root,  Serializable, Number};
  if (Deep)
    Out.push_back(Mid);
  return Out;
}
} // namespace

TEST(MobileDalvikInheritedProtectedAllocation,
     RetainExactCallerQualifierClosedParentsAndVirtualAllocationCalls) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Final : {false, true})
        for (bool Alpha : {false, true})
          for (bool StaticCaller : {false, true})
            for (bool Deep : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Range, Final, Alpha, StaticCaller, Deep, Route}));
                auto Input =
                    fixture(SDK, Range, Final, Alpha, StaticCaller, Deep);
                const auto Original = Input;
                if (Route == 2)
                  std::reverse(Input.begin(), Input.end());
                Budget B;
                ClassMap Classes;
                if (Route)
                  Classes = linkClasses(Input, B);
                else
                  for (const auto &C : Input)
                    Classes.emplace(C.name, C);
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
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
                    EXPECT_EQ(After.methods[I].declared_throws,
                              C.methods[I].declared_throws);
                  }
                }
              }
}

TEST(MobileDalvikInheritedProtectedAllocation,
     RejectUnprovedQualifierScopeEdgesDescriptorsAndReceivers) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 21; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, false, true, false);
      auto &Target = Input[1].methods[0];
      auto &Caller = Input[0].methods[0];
      if (Mutation == 0)
        Target.access.erase("protected");
      else if (Mutation == 1) {
        Target.access.erase("protected");
        Target.access.insert("private");
      } else if (Mutation == 2)
        Target.generic_signature =
            "<M:Ljava/lang/Object;>(TM;)Ljava/lang/String;";
      else if (Mutation == 3)
        Input[0].generic_signature = "Lbase/Parent;";
      else if (Mutation == 4)
        Input[0].generic_signature = "Lbase/Parent<+Ljava/net/URI;>;";
      else if (Mutation == 5)
        Input[0].generic_signature = "Lbase/Parent<TMissing;>;";
      else if (Mutation == 6)
        Input[1].generic_signature =
            "<T:Ljava/lang/Number;>Ljava/lang/Object;Ljava/io/Serializable;";
      else if (Mutation == 7)
        Input[1].access.insert("interface");
      else if (Mutation == 8) {
        Target.access.insert("bridge");
        Target.access.insert("synthetic");
      } else if (Mutation == 9)
        Target.access.insert("static");
      else if (Mutation == 10)
        Caller.instructions[1].opcode = "invoke-static/range";
      else if (Mutation == 11)
        Caller.instructions[1].registers[0] = 0;
      else if (Mutation == 12)
        Caller.instructions[1].registers.push_back(2);
      else if (Mutation == 13)
        Caller.instructions[1].registers.pop_back();
      else if (Mutation == 14)
        std::get<MethodRef>(Caller.instructions[1].reference).owner =
            Input[1].name;
      else if (Mutation == 15) {
        auto Other = Input[0];
        Other.name = "Lother/Caller;";
        for (auto &M : Other.methods)
          M.reference.owner = Other.name;
        Input.push_back(std::move(Other));
      } else if (Mutation == 16)
        Input[0].generic_signature.reset();
      else if (Mutation == 17) {
        Input[0].superclass = Input[0].name;
        Input[0].generic_signature = "Lfixture/Child;";
      } else if (Mutation == 18)
        Target.generic_signature = "(Ljava/lang/Object;)TT;";
      else if (Mutation == 19)
        Caller.instructions[0].registers[0] = 1;
      else
        Input[1].access.erase("public");
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
