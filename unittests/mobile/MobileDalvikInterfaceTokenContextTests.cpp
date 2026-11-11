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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool OwnerFormal,
                           bool Ignored, unsigned Tail, unsigned Throws,
                           bool Alias, bool Varargs) {
  const std::string Object = "Ljava/lang/Object;", Token = "Ljava/lang/Class;",
                    Bound =
                        Alias ? "Lfixture/View;" : "Ljava/lang/CharSequence;",
                    Context = "Lfixture/Context;", T = Alpha ? "Result" : "T",
                    Component = Tail == 0   ? "Ljava/lang/String;"
                                : Tail == 1 ? "Ljava/lang/CharSequence;"
                                            : "Lfixture/Option;",
                    Array = "[" + Component;
  auto Provider = klass("Lfixture/Provider;", SDK);
  if (OwnerFormal)
    Provider.generic_signature =
        "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  std::vector<std::string> Parameters{Context, Token, Array};
  MethodRef Selected{Provider.name, "read", Parameters, Bound};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method(Selected, {"public", "static"}, SDK ? 0 : 4,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  if (Varargs)
    Target.access.insert("varargs");
  Target.generic_signature = "<" + T + "::" + Bound + ">(" + Context +
                             "Ljava/lang/Class<T" + T + ";>;" + Array + ")T" +
                             T + ";";
  if (Throws)
    Target.declared_throws =
        std::vector<std::string>{Throws == 1   ? "Ljava/io/IOException;"
                                 : Throws == 2 ? "Ljava/lang/RuntimeException;"
                                               : "Ljava/lang/Exception;"};
  Provider.methods.push_back(Target);
  std::vector<Instruction> Code{op(
      0, Range ? "invoke-static/range" : "invoke-static", {1, 2, 3}, Selected)};
  if (Ignored)
    Code.push_back(op(1, "return-void"));
  else {
    Code.push_back(op(1, "move-result-object", {0}));
    Code.push_back(op(2, "return-object", {0}));
  }
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(
      method({Client.name, "call", Parameters, Ignored ? "V" : Bound},
             {"public", "static"}, 4, Code));
  auto BoundClass = klass(Bound, true);
  BoundClass.access = {"public", "interface", "abstract"};
  auto ClassClass = klass(Token, true);
  ClassClass.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto ContextClass = klass(Context, true);
  auto Option = klass("Lfixture/Option;", true);
  auto Text = klass("Ljava/lang/CharSequence;", true);
  Text.access = {"public", "interface", "abstract"};
  auto String = klass("Ljava/lang/String;", true);
  String.access.insert("final");
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = Throwable.name;
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = Exception.name;
  auto IO = klass("Ljava/io/IOException;", true);
  IO.superclass = Exception.name;
  auto Result = std::vector<Class>{
      Client, Provider, BoundClass, ClassClass, ContextClass, Option, Text,
      String, Root,     Throwable,  Exception,  Runtime,      IO};
  if (!Alias)
    Result.erase(Result.begin() + 6);
  return Result;
}
} // namespace

TEST(MobileDalvikInterfaceTokenContext,
     RetainInterfaceTokenScopesNominalVarargsArraysAndClosedCheckedThrows) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool OwnerFormal : {false, true})
          for (bool Ignored : {false, true})
            for (bool Alias : {false, true})
              for (bool Varargs : {false, true})
                for (unsigned Tail : {0u, 1u, 2u})
                  for (unsigned Throws : {0u, 1u, 2u, 3u})
                    for (unsigned Route : {0u, 1u, 2u}) {
                      SCOPED_TRACE(::testing::PrintToString(
                          std::tuple{SDK, Range, Alpha, OwnerFormal, Ignored,
                                     Alias, Varargs, Tail, Throws, Route}));
                      auto Input =
                          fixture(SDK, Range, Alpha, OwnerFormal, Ignored, Tail,
                                  Throws, Alias, Varargs);
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
                      if (Throws)
                        EXPECT_TRUE(validatedClosedDeclaredThrows(
                            Classes.at(Selected.owner).methods[0], Plans));
                      const auto Report = recoverJava(Classes, B);
                      EXPECT_EQ(Report.getInteger("unrecovered_method_count"),
                                0);
                      EXPECT_FALSE(
                          Report.getArray("generic_signature_projections"));
                      for (const auto &C : Original) {
                        const auto &After = Classes.at(C.name);
                        EXPECT_EQ(After.access, C.access);
                        EXPECT_EQ(After.generic_signature, C.generic_signature);
                        for (size_t I = 0; I < C.methods.size(); ++I) {
                          EXPECT_EQ(After.methods[I].reference,
                                    C.methods[I].reference);
                          EXPECT_EQ(After.methods[I].access,
                                    C.methods[I].access);
                          EXPECT_EQ(After.methods[I].generic_signature,
                                    C.methods[I].generic_signature);
                          EXPECT_EQ(After.methods[I].declared_throws,
                                    C.methods[I].declared_throws);
                        }
                      }
                    }
}

TEST(MobileDalvikInterfaceTokenContext,
     RejectBrokenTokenScopesArrayDeclarationsExceptionsAndOriginalCallRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 24; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, false, false, 2, 1, true, true);
      auto &Target = Input[1].methods[0];
      auto &Signature = *Target.generic_signature;
      auto Replace = [&](const std::string &From, const std::string &To) {
        auto At = Signature.find(From);
        ASSERT_NE(At, std::string::npos);
        Signature.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        Replace("Class<TT;>", "Class<TMissing;>");
      else if (Mutation == 1)
        Replace(")TT;", ")TMissing;");
      else if (Mutation == 2)
        Replace(";>(", ";T:Ljava/lang/Object;>(");
      else if (Mutation == 3)
        Replace("[Lfixture/Option;", "[Ljava/lang/Object;");
      else if (Mutation == 4)
        Replace("Lfixture/Context;", "Ljava/lang/Object;");
      else if (Mutation == 5)
        Replace("Class<TT;>", "Class<+TT;>");
      else if (Mutation == 6)
        Input[2].access.erase("interface");
      else if (Mutation == 7) {
        Input[2].access.erase("public");
        Input[2].access.insert("private");
      } else if (Mutation == 8)
        Input[5].access.erase("public");
      else if (Mutation == 9)
        Input[5].generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 10)
        Target.access.insert("synthetic");
      else if (Mutation == 11)
        Target.access.insert("native");
      else if (Mutation == 12)
        Target.access.insert("abstract");
      else if (Mutation == 13)
        Target.access.insert("bridge");
      else if (Mutation == 14) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 15)
        Input[1].access.insert("interface");
      else if (Mutation == 16)
        Target.declared_throws = std::vector<std::string>{"Lfixture/Missing;"};
      else if (Mutation == 17)
        Input[12].superclass = "Lfixture/Missing;";
      else if (Mutation == 18)
        Target.access.erase("static");
      else if (Mutation == 19)
        Input[0].methods[0].instructions[0].registers.pop_back();
      else if (Mutation == 20)
        Replace(")TT;", ")Ljava/lang/String;");
      else if (Mutation == 21)
        Input[3].generic_signature =
            "<C:Ljava/lang/Object;D:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 22)
        Replace("Lfixture/View;>(",
                "Lfixture/View;:Ljava/lang/CharSequence;>(");
      else
        Replace("Class<TT;>", "Class<*>");
      Budget B;
      ClassMap Classes;
      for (const auto &C : Input)
        Classes.emplace(C.name, C);
      EXPECT_THROW((void)recoverJava(Classes, B), std::runtime_error);
    }
}
