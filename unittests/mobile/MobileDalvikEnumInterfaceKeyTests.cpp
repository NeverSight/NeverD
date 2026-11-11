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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool PlatformAlpha,
                           bool Abstract, bool Fallback) {
  const std::string Object = "Ljava/lang/Object;", Enum = "Ljava/lang/Enum;",
                    Token = "Ljava/lang/Class;", E = Alpha ? "Element" : "E",
                    K = Alpha ? "Key" : "K", P = PlatformAlpha ? "Self" : "E";
  auto Source = klass("Lfixture/Source;", SDK);
  Source.access = {"public", "abstract", "interface"};
  Source.generic_signature = "<" + K + ":Ljava/lang/Object;>Ljava/lang/Object;";
  std::vector<std::string> Params{Token, Object};
  if (Fallback)
    Params.push_back(Enum);
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method({Source.name, "select", Params, Enum}, {"public"},
             SDK || Abstract ? 0 : Params.size() + 2,
             SDK || Abstract
                 ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  if (Abstract)
    Target.access.insert("abstract");
  Target.generic_signature = "<" + E + ":Ljava/lang/Enum<T" + E +
                             ";>;>(Ljava/lang/Class<T" + E + ";>;T" + K + ";" +
                             (Fallback ? "T" + E + ";" : "") + ")T" + E + ";";
  Source.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  Params.insert(Params.begin(), Source.name);
  std::vector<unsigned> Args;
  for (unsigned I = 1; I <= Params.size(); ++I)
    Args.push_back(I);
  Client.methods.push_back(
      method({Client.name, "call", Params, Enum}, {"public", "static"},
             Params.size() + 1,
             {op(0, Range ? "invoke-interface/range" : "invoke-interface", Args,
                 Target.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  // Mirror a typed interface caller rather than an already erased raw receiver.
  // The caller's formals have their own scope, independent of Source's key.
  Client.methods.back().generic_signature =
      "<Key:Ljava/lang/Object;Result:Ljava/lang/Enum<TResult;>;>(Lfixture/"
      "Source<TKey;>;Ljava/lang/Class<TResult;>;TKey;" +
      std::string(Fallback ? "TResult;" : "") + ")TResult;";
  auto EnumClass = klass(Enum, true);
  EnumClass.access = {"public", "abstract"};
  EnumClass.generic_signature =
      "<" + P + ":Ljava/lang/Enum<T" + P + ";>;>Ljava/lang/Object;";
  auto ClassClass = klass(Token, true);
  ClassClass.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Client, Source, EnumClass, ClassClass, Root};
}
} // namespace

TEST(MobileDalvikEnumInterfaceKey,
     RetainSeparateInterfaceAndRecursiveMethodScopesAbstractDefaultsAndFrames) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool PlatformAlpha : {false, true})
          for (bool Abstract : {false, true})
            for (bool Fallback : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Range, Alpha, PlatformAlpha, Abstract,
                               Fallback, Route}));
                auto Input = fixture(SDK, Range, Alpha, PlatformAlpha, Abstract,
                                     Fallback);
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
                  }
                }
              }
}

TEST(MobileDalvikEnumInterfaceKey,
     RejectBrokenKeyAndSelfScopesChangedErasureRolesAndIncompleteDeclarations) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 24; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, false, true, true);
      auto &Source = Input[1];
      auto &Target = Source.methods[0];
      auto &Signature = *Target.generic_signature;
      auto Replace = [&](const std::string &From, const std::string &To) {
        const auto At = Signature.find(From);
        ASSERT_NE(At, std::string::npos);
        Signature.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        Replace("Enum<TE;>", "Enum<TMissing;>");
      else if (Mutation == 1)
        Replace("Enum<TE;>", "Enum<+TE;>");
      else if (Mutation == 2)
        Replace("Class<TE;>", "Class<+TE;>");
      else if (Mutation == 3)
        Replace(";TK;", ";TE;");
      else if (Mutation == 4)
        Replace(";TK;", ";TMissing;");
      else if (Mutation == 5)
        Source.generic_signature = "<K:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 6)
        Source.generic_signature.reset();
      else if (Mutation == 7)
        Replace(";>;>(", ";>;U:Ljava/lang/Object;>(");
      else if (Mutation == 8)
        Replace(")TE;", ")TK;");
      else if (Mutation == 9)
        Input.erase(Input.begin() + 2);
      else if (Mutation == 10)
        Input[2].generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 11)
        Input.erase(Input.begin() + 3);
      else if (Mutation == 12)
        Input[3].generic_signature = "<C:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 13)
        Source.access.erase("public");
      else if (Mutation == 14)
        Target.access.insert("static");
      else if (Mutation == 15)
        Target.access.insert("native");
      else if (Mutation == 16)
        Target.access.insert("bridge");
      else if (Mutation == 17)
        Target.access.insert("synthetic");
      else if (Mutation == 18)
        Target.access.insert("varargs");
      else if (Mutation == 19) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 20)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 21)
        Source.access.erase("interface");
      else if (Mutation == 22)
        Input[0].methods[0].instructions[0].registers.pop_back();
      else
        Replace(";TK;", ";[TK;");
      Budget B;
      if (Mutation == 2 || Mutation == 7 || Mutation == 10 || Mutation == 17 ||
          (SDK && Mutation == 18)) {
        // A raw interface invocation can use the existing independent erasure
        // path. It must not receive this recursive enum binding receipt.
        const auto Selected = Target.reference;
        const auto Classes = linkClasses(Input, B);
        const auto Plans = validateGenericSignatures(Classes, B);
        EXPECT_FALSE(Plans.erased_generic_invocations.contains(Selected));
      } else
        EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
