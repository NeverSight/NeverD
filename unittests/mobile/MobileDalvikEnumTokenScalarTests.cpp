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
                           bool PlatformAlpha, bool Ignored) {
  const std::string Object = "Ljava/lang/Object;", Enum = "Ljava/lang/Enum;",
                    Token = "Ljava/lang/Class;", E = Alpha ? "Element" : "E",
                    P = PlatformAlpha ? "Self" : "P";
  auto Provider = klass("Lfixture/Provider;", SDK);
  if (OwnerFormal)
    Provider.generic_signature =
        "<" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  MethodRef Selected{Provider.name, "select", {Token, "I"}, Enum};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method(Selected, {"public", "static"}, SDK ? 0 : 3,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  Target.generic_signature = "<" + E + ":Ljava/lang/Enum<T" + E +
                             ";>;>(Ljava/lang/Class<T" + E + ";>;I)T" + E + ";";
  Provider.methods.push_back(Target);
  std::vector<Instruction> Code{
      op(0, Range ? "invoke-static/range" : "invoke-static", {1, 2}, Selected)};
  if (Ignored)
    Code.push_back(op(1, "return-void"));
  else {
    Code.push_back(op(1, "move-result-object", {0}));
    Code.push_back(op(2, "return-object", {0}));
  }
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(
      method({Client.name, "call", {Token, "I"}, Ignored ? "V" : Enum},
             {"public", "static"}, 3, Code));
  auto EnumClass = klass(Enum, true);
  EnumClass.access = {"public", "abstract"};
  EnumClass.generic_signature =
      "<" + P + ":Ljava/lang/Enum<T" + P + ";>;>Ljava/lang/Object;";
  auto ClassClass = klass(Token, true);
  ClassClass.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Client, Provider, EnumClass, ClassClass, Root};
}
} // namespace

TEST(MobileDalvikEnumTokenScalar,
     RetainRecursiveTokenResultScopesAndIndependentIntWords) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool OwnerFormal : {false, true})
          for (bool PlatformAlpha : {false, true})
            for (bool Ignored : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Range, Alpha, OwnerFormal, PlatformAlpha,
                               Ignored, Route}));
                auto Input = fixture(SDK, Range, Alpha, OwnerFormal,
                                     PlatformAlpha, Ignored);
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
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              }
}

TEST(MobileDalvikEnumTokenScalar,
     RejectBrokenSelfBoundsTokenVarianceContextErasureAndRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, false, false, false);
      auto &Target = Input[1].methods[0];
      auto &Caller = Input[0].methods[0];
      if (Mutation == 0)
        Target.generic_signature =
            "<E:Ljava/lang/Enum<TMissing;>;>(Ljava/lang/Class<TE;>;I)TE;";
      else if (Mutation == 1)
        Target.generic_signature =
            "<E:Ljava/lang/Enum<+TE;>;>(Ljava/lang/Class<TE;>;I)TE;";
      else if (Mutation == 2)
        Target.generic_signature =
            "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/Class<+TE;>;I)TE;";
      else if (Mutation == 3)
        Target.generic_signature =
            "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/Class<TE;>;I)TMissing;";
      else if (Mutation == 4)
        Target.generic_signature =
            "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/Class<TE;>;J)TE;";
      else if (Mutation == 5)
        Target.generic_signature = "<E:Ljava/lang/Enum<TE;>;U:Ljava/lang/"
                                   "Object;>(Ljava/lang/Class<TE;>;I)TE;";
      else if (Mutation == 6)
        Target.generic_signature =
            "<E:Ljava/lang/Object;>(Ljava/lang/Class<TE;>;I)TE;";
      else if (Mutation == 7)
        Target.access.insert("native");
      else if (Mutation == 8)
        Target.access.insert("abstract");
      else if (Mutation == 9)
        Target.access.insert("synthetic");
      else if (Mutation == 10)
        Target.access.insert("varargs");
      else if (Mutation == 11)
        Target.access.erase("static");
      else if (Mutation == 12) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 13)
        Input[1].access.insert("interface");
      else if (Mutation == 14)
        Input[2].access.erase("abstract");
      else if (Mutation == 15)
        Input[2].generic_signature =
            "<P:Ljava/lang/Enum<TMissing;>;>Ljava/lang/Object;";
      else if (Mutation == 16)
        Input[3].generic_signature =
            "<C:Ljava/lang/Object;D:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 17)
        Caller.instructions[0].registers.pop_back();
      else if (Mutation == 18)
        Caller.instructions[0].opcode = "invoke-virtual";
      else
        Target.declared_throws = {"Ljava/lang/RuntimeException;"};
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
