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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Varargs,
                           bool Ignored, unsigned Throws) {
  const std::string Object = "Ljava/lang/Object;",
                    Text = "Ljava/lang/CharSequence;",
                    String = "Ljava/lang/String;",
                    Array = "[Ljava/lang/Object;", E = Alpha ? "Element" : "E";
  auto Provider = klass("Lfixture/Provider;", SDK);
  MethodRef Selected{Provider.name, "select", {Text, String, Array}, Text};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method(Selected, {"public", "static"}, SDK ? 0 : 4,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  if (Varargs)
    Target.access.insert("varargs");
  Target.generic_signature = "<" + E + "::Ljava/lang/CharSequence;>(T" + E +
                             ";Ljava/lang/String;[Ljava/lang/Object;)T" + E +
                             ";";
  if (Throws == 1)
    Target.declared_throws = std::vector<std::string>{};
  else if (Throws > 1)
    Target.declared_throws = {Throws == 2
                                  ? "Ljava/lang/IllegalArgumentException;"
                                  : "Ljava/lang/RuntimeException;"};
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  std::vector<Instruction> Code{op(
      0, Range ? "invoke-static/range" : "invoke-static", {1, 2, 3}, Selected)};
  if (Ignored)
    Code.push_back(op(1, "return-void"));
  else {
    Code.push_back(op(1, "move-result-object", {0}));
    Code.push_back(op(2, "return-object", {0}));
  }
  Client.methods.push_back(
      method({Client.name, "call", Selected.parameters, Ignored ? "V" : Text},
             {"public", "static"}, 4, Code));
  auto Interface = klass(Text, true);
  Interface.access = {"public", "interface", "abstract"};
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = Throwable.name;
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = Exception.name;
  auto Illegal = klass("Ljava/lang/IllegalArgumentException;", true);
  Illegal.superclass = Runtime.name;
  return {Client,    Provider, Interface, klass(String, true), Root, Throwable,
          Exception, Runtime,  Illegal};
}
} // namespace

TEST(MobileDalvikInterfaceSeedVarargs,
     RetainInterfaceSeedResultsFixedArraysScopesAndUncheckedThrows) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Varargs : {false, true})
          for (bool Ignored : {false, true})
            for (unsigned Throws : {0u, 1u, 2u, 3u})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Range, Alpha, Varargs, Ignored, Throws, Route}));
                auto Input =
                    fixture(SDK, Range, Alpha, Varargs, Ignored, Throws);
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
                EXPECT_TRUE(
                    Plans.erased_generic_invocations.contains(Selected));
                if (Throws > 1)
                  EXPECT_TRUE(validatedUncheckedDeclaredThrows(
                      Classes.at(Selected.owner).methods[0], Plans));
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
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              }
}

TEST(MobileDalvikInterfaceSeedVarargs,
     RejectBorrowedFormalsErasureChangesCheckedThrowsAndInvalidRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 22; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, true, false, 2);
      auto &Target = Input[1].methods[0];
      auto &Caller = Input[0].methods[0];
      if (Mutation == 0)
        Target.generic_signature =
            "<E::Ljava/lang/CharSequence;>(TMissing;Ljava/lang/String;[Ljava/"
            "lang/Object;)TE;";
      else if (Mutation == 1)
        Target.generic_signature = "<E::Ljava/lang/CharSequence;>(TE;Ljava/"
                                   "lang/String;[Ljava/lang/Object;)TMissing;";
      else if (Mutation == 2)
        Target.generic_signature = "<E:Ljava/lang/Object;>(TE;Ljava/lang/"
                                   "String;[Ljava/lang/Object;)TE;";
      else if (Mutation == 3)
        Target.generic_signature =
            "<E::Ljava/lang/CharSequence;:Ljava/io/Serializable;>(TE;Ljava/"
            "lang/String;[Ljava/lang/Object;)TE;";
      else if (Mutation == 4)
        Input[1].generic_signature =
            "<Outer:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 5) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 6)
        Target.access.erase("static");
      else if (Mutation == 7)
        Target.access.insert("abstract");
      else if (Mutation == 8)
        Target.access.insert("native");
      else if (Mutation == 9)
        Target.access.insert("synthetic");
      else if (Mutation == 10)
        Target.access.insert("bridge");
      else if (Mutation == 11)
        Target.generic_signature = "<E::Ljava/lang/CharSequence;>(TE;Ljava/"
                                   "lang/Object;[Ljava/lang/Object;)TE;";
      else if (Mutation == 12)
        Target.generic_signature = "<E::Ljava/lang/CharSequence;>(TE;Ljava/"
                                   "lang/String;[Ljava/lang/String;)TE;";
      else if (Mutation == 13)
        Target.generic_signature =
            "<E::Ljava/lang/CharSequence;>(TE;Ljava/lang/String;[TE;)TE;";
      else if (Mutation == 14)
        Target.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 15)
        Target.declared_throws = {"Lfixture/MissingFailure;"};
      else if (Mutation == 16)
        Input[2].generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 17)
        Input[2].access.erase("interface");
      else if (Mutation == 18)
        Caller.instructions[0].registers.pop_back();
      else if (Mutation == 19)
        Caller.instructions[0].opcode = "invoke-virtual/range";
      else if (Mutation == 20) {
        Input[1].generic_signature =
            "<Outer::Ljava/lang/CharSequence;>Ljava/lang/Object;";
        Target.generic_signature = "<E::Ljava/lang/CharSequence;>(TOuter;Ljava/"
                                   "lang/String;[Ljava/lang/Object;)TE;";
      } else
        Target.generic_signature =
            "<E::Ljava/lang/CharSequence;>(TE;Ljava/lang/String;[Ljava/lang/"
            "Object;)TE;^TMissing;";
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
