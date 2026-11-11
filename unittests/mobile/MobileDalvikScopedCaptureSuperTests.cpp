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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Synthetic,
                           bool Variadic) {
  const std::string Object = "Ljava/lang/Object;", T = Alpha ? "Element" : "T";
  auto Parent = klass("Lfixture/Parent;", SDK);
  Parent.generic_signature = "<P:Ljava/lang/Object;>Ljava/lang/Object;";
  MethodRef Selected{Parent.name, "<init>", {"Z", "Lfixture/Callback;"}, "V"};
  auto Sink = method(
      Selected, {"public", "constructor"}, SDK ? 0 : 3,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{op(0, "invoke-direct", {0},
                                        MethodRef{Object, "<init>", {}, "V"}),
                                     op(1, "return-void")});
  Sink.generic_signature = "(ZLfixture/Callback<-TP;>;)V";
  Parent.methods.push_back(Sink);
  auto Proxy = klass("Lfixture/Proxy;");
  Proxy.access.insert("final");
  Proxy.interfaces = {"Lfixture/Callback;"};
  auto Init = method(
      {Proxy.name, "<init>", {"[Ljava/lang/String;", "Lfixture/Token;"}, "V"},
      {"public", "constructor"}, 3,
      {op(0, "invoke-direct", {0}, MethodRef{Object, "<init>", {}, "V"}),
       op(1, "return-void")});
  if (Synthetic) {
    Proxy.access.insert("synthetic");
    Init.access.insert("synthetic");
  }
  Proxy.methods.push_back(Init);
  auto Child = klass("Lfixture/Child;");
  Child.superclass = Parent.name;
  Child.generic_signature =
      "<" + T + ":Ljava/lang/Object;>Lfixture/Parent<T" + T + ";>;";
  auto Caller = method(
      {Child.name,
       "<init>",
       {"Z", "Lfixture/Token;", "[Ljava/lang/String;"},
       "V"},
      {"public", "constructor"}, 8,
      {op(0, "move-object", {0, 4}), op(1, "move-object", {2, 7}),
       op(2, "move-object", {3, 6}), op(3, "new-instance", {1}, Proxy.name),
       op(4, Range ? "invoke-direct/range" : "invoke-direct", {1, 2, 3},
          Init.reference),
       op(5, "move-object", {2, 1}), op(6, "move", {1, 5}),
       op(7, Range ? "invoke-direct/range" : "invoke-direct", {0, 1, 2},
          Selected),
       op(8, "return-void")});
  Caller.generic_signature =
      "(ZLfixture/Token<T" + T + ";>;[Ljava/lang/String;)V";
  if (Variadic)
    Caller.access.insert("varargs");
  Child.methods.push_back(Caller);
  auto Token = klass("Lfixture/Token;", true);
  Token.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Callback = klass("Lfixture/Callback;", true);
  Callback.access = {"public", "interface", "abstract"};
  Callback.generic_signature = "<K:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  return {Child, Parent,   Proxy,
          Token, Callback, klass("Ljava/lang/String;", true),
          Root};
}
} // namespace

TEST(MobileDalvikScopedCaptureSuper,
     RetainScopedNominalCapturesAndCompleteParentContracts) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Synthetic : {false, true})
          for (bool Variadic : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, Synthetic, Variadic, Route}));
              auto Input = fixture(SDK, Range, Alpha, Synthetic, Variadic);
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
                EXPECT_EQ(After.generic_signature, C.generic_signature);
                EXPECT_EQ(After.access, C.access);
                for (size_t I = 0; I < C.methods.size(); ++I) {
                  EXPECT_EQ(After.methods[I].reference, C.methods[I].reference);
                  EXPECT_EQ(After.methods[I].generic_signature,
                            C.methods[I].generic_signature);
                  EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                }
              }
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikScopedCaptureSuper,
     RejectChangedScopesCapturesInitializersAndOverloads) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, true, true);
      auto &Caller = Input[0].methods[0], &Parent = Input[1].methods[0],
           &Init = Input[2].methods[0];
      if (Mutation == 0)
        Caller.generic_signature =
            "(ZLfixture/Token<+TT;>;[Ljava/lang/String;)V";
      else if (Mutation == 1)
        Caller.generic_signature =
            "<T:Ljava/lang/Object;>(ZLfixture/Token<TT;>;[Ljava/lang/String;)V";
      else if (Mutation == 2)
        Caller.generic_signature =
            "(ZLfixture/Token<TMissing;>;[Ljava/lang/String;)V";
      else if (Mutation == 3)
        Input[3].generic_signature = "<C:Ljava/lang/String;>Ljava/lang/Object;";
      else if (Mutation == 4)
        Init.generic_signature =
            "<U:Ljava/lang/Object;>([Ljava/lang/String;Lfixture/Token;)V";
      else if (Mutation == 5)
        Caller.instructions[4].registers.pop_back();
      else if (Mutation == 6)
        Caller.instructions[2].registers[1] = 7;
      else if (Mutation == 7)
        Caller.instructions[1].registers[1] = 6;
      else if (Mutation == 8)
        Caller.instructions[2].opcode = "move";
      else if (Mutation == 9)
        Input[2].access.insert("abstract");
      else if (Mutation == 10)
        Input[2].interfaces.clear();
      else if (Mutation == 11)
        Input[2].generic_signature =
            "<X:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Callback;";
      else if (Mutation == 12) {
        auto Sibling = Parent;
        Sibling.reference.parameters[1] = "Ljava/lang/Object;";
        Sibling.generic_signature = "(ZLjava/lang/Object;)V";
        Input[1].methods.push_back(Sibling);
      } else if (Mutation == 13)
        Parent.generic_signature = "(ZLfixture/Callback<-TMissing;>;)V";
      else if (Mutation == 14)
        Parent.generic_signature =
            "<U:Ljava/lang/Object;>(ZLfixture/Callback<-TP;>;)V";
      else if (Mutation == 15)
        Init.access.insert("private");
      else if (Mutation == 16)
        Init.access.insert("static");
      else if (Mutation == 17)
        Init.access.insert("native");
      else if (Mutation == 18)
        Init.access.insert("varargs");
      else
        Caller.instructions[3].registers[0] = 4;
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
