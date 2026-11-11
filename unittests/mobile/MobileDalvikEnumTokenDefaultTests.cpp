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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Default,
                           bool OwnerFormal, bool PlatformAlpha) {
  const std::string Object = "Ljava/lang/Object;", Enum = "Ljava/lang/Enum;",
                    Token = "Ljava/lang/Class;", E = Alpha ? "Element" : "E",
                    P = PlatformAlpha ? "Self" : "P";
  auto Provider = klass("Lfixture/Provider;", SDK);
  if (OwnerFormal)
    Provider.generic_signature =
        "<" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  std::vector<std::string> Parameters{Token, Object};
  if (Default)
    Parameters.push_back(Enum);
  MethodRef Selected{Provider.name, "convert", Parameters, Enum};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method(Selected, {"public", "static"}, SDK ? 0 : Parameters.size() + 1,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  Target.generic_signature =
      "<" + E + ":Ljava/lang/Enum<T" + E + ";>;>(Ljava/lang/Class<T" + E +
      ";>;Ljava/lang/Object;" + (Default ? "T" + E + ";" : "") + ")T" + E + ";";
  Provider.methods.push_back(Target);
  std::vector<unsigned> Words;
  for (unsigned I = 1; I <= Parameters.size(); ++I)
    Words.push_back(I);
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(method(
      {Client.name, "call", Parameters, Enum}, {"public", "static"},
      Parameters.size() + 1,
      {op(0, Range ? "invoke-static/range" : "invoke-static", Words, Selected),
       op(1, "move-result-object", {0}), op(2, "return-object", {0})}));
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

TEST(MobileDalvikEnumTokenDefault,
     RetainRecursiveMethodTokenDefaultsContextsAndOpaqueEnumResults) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Default : {false, true})
          for (bool OwnerFormal : {false, true})
            for (bool PlatformAlpha : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Range, Alpha, Default, OwnerFormal,
                               PlatformAlpha, Route}));
                auto Input = fixture(SDK, Range, Alpha, Default, OwnerFormal,
                                     PlatformAlpha);
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

TEST(MobileDalvikEnumTokenDefault,
     RejectChangedSelfBoundsTokenSlotsContextsAndDeclarationRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 22; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, true, false, false);
      auto &Target = Input[1].methods[0];
      auto &Caller = Input[0].methods[0];
      if (Mutation == 0)
        Target.generic_signature = "<E:Ljava/lang/Enum<TMissing;>;>(Ljava/lang/"
                                   "Class<TE;>;Ljava/lang/Object;TE;)TE;";
      else if (Mutation == 1)
        Target.generic_signature = "<E:Ljava/lang/Enum<+TE;>;>(Ljava/lang/"
                                   "Class<TE;>;Ljava/lang/Object;TE;)TE;";
      else if (Mutation == 2)
        Target.generic_signature = "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/"
                                   "Class<+TE;>;Ljava/lang/Object;TE;)TE;";
      else if (Mutation == 3)
        Target.generic_signature = "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/"
                                   "Class<TE;>;Ljava/lang/Object;TMissing;)TE;";
      else if (Mutation == 4)
        Target.generic_signature = "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/"
                                   "Class<TE;>;Ljava/lang/Object;TE;)TMissing;";
      else if (Mutation == 5)
        Target.generic_signature =
            "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/Class<TE;>;TE;TE;)TE;";
      else if (Mutation == 6)
        Target.generic_signature = "<E:Ljava/lang/Object;>(Ljava/lang/"
                                   "Class<TE;>;Ljava/lang/Object;TE;)TE;";
      else if (Mutation == 7)
        Target.generic_signature =
            "<E:Ljava/lang/Enum<TE;>;U:Ljava/lang/Object;>(Ljava/lang/"
            "Class<TE;>;Ljava/lang/Object;TE;)TE;";
      else if (Mutation == 8)
        Target.access.insert("varargs");
      else if (Mutation == 9)
        Target.access.insert("native");
      else if (Mutation == 10)
        Target.access.insert("abstract");
      else if (Mutation == 11)
        Target.access.insert("synthetic");
      else if (Mutation == 12)
        Target.access.erase("static");
      else if (Mutation == 13) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 14)
        Input[1].access.insert("interface");
      else if (Mutation == 15)
        Input[1].access.erase("public");
      else if (Mutation == 16)
        Input[2].access.erase("abstract");
      else if (Mutation == 17)
        Input[2].generic_signature =
            "<P:Ljava/lang/Enum<TMissing;>;>Ljava/lang/Object;";
      else if (Mutation == 18)
        Input[3].generic_signature =
            "<C:Ljava/lang/Object;D:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 19)
        Caller.instructions[0].registers.pop_back();
      else if (Mutation == 20)
        Target.generic_signature =
            "<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/Class<TE;>;Ljava/lang/"
            "Object;TE;)Ljava/lang/Enum;";
      else
        Target.declared_throws = {"Ljava/lang/RuntimeException;"};
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}

namespace {
std::vector<Class> enumTokenMapFixture(bool SDK, bool CarrierSDK, bool Range,
                                       bool Alpha, bool PlatformAlpha,
                                       bool Facade, bool Interface,
                                       bool ObjectKey) {
  auto Input = fixture(SDK, Range, Alpha, false, false, PlatformAlpha);
  const std::string E = Alpha ? "Element" : "E", P = "Lfixture/Pair;",
                    Key =
                        ObjectKey ? "Ljava/lang/Object;" : "Ljava/lang/String;";
  auto &Target = Input[1].methods[0];
  Target.reference.parameters = {"Ljava/lang/Class;"};
  Target.reference.returns = P;
  Target.generic_signature = "<" + E + ":Ljava/lang/Enum<T" + E +
                             ";>;>(Ljava/lang/Class<T" + E +
                             ";>;)Lfixture/Pair<" + Key + "T" + E + ";>;";
  Target.registers = SDK ? 0 : 2;
  auto Ref = Target.reference;
  auto Alias = klass("Lfixture/Facade;", SDK);
  Alias.superclass = Input[1].name;
  if (Facade)
    Ref.owner = Alias.name;
  auto &Caller = Input[0].methods[0];
  Caller.reference.parameters = Target.reference.parameters;
  Caller.reference.returns = P;
  const unsigned Start = Range ? 32 : 1;
  Caller.registers = Start + 1;
  Caller.instructions = {
      op(0, Range ? "invoke-static/range" : "invoke-static", {Start}, Ref),
      op(3, "move-result-object", {0}), op(4, "return-object", {0})};
  Caller.code_end = 5;
  auto Pair = klass(P, CarrierSDK);
  Pair.generic_signature =
      "<Key:Ljava/lang/Object;" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (Interface)
    Pair.access = {"public", "abstract", "interface"};
  Input.push_back(Pair);
  Input.push_back(klass("Ljava/lang/String;", true));
  Input.push_back(Alias);
  Input.push_back(klass("Ljava/lang/Number;", true));
  auto Checked = klass("Ljava/io/IOException;", true);
  Checked.superclass = "Ljava/lang/Exception;";
  Input.push_back(Checked);
  return Input;
}
ClassMap enumTokenMapInput(std::vector<Class> Input, unsigned Route,
                           Budget &B) {
  if (Route == 2)
    std::reverse(Input.begin(), Input.end());
  if (Route)
    return linkClasses(Input, B);
  ClassMap Classes;
  for (auto &C : Input)
    Classes.emplace(C.name, std::move(C));
  return Classes;
}
} // namespace
TEST(MobileDalvikEnumTokenDefault,
     RecursiveEnumTokensRetainClosedHeadsAndOwnInvariantResultSlots) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool PlatformAlpha : {false, true})
            for (bool Facade : {false, true})
              for (bool Interface : {false, true})
                for (bool ObjectKey : {false, true})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, CarrierSDK, Range, Alpha, PlatformAlpha,
                                   Facade, Interface, ObjectKey, Route}));
                    EXPECT_NO_THROW(([&] {
                      Budget B;
                      auto Classes = enumTokenMapInput(
                          enumTokenMapFixture(SDK, CarrierSDK, Range, Alpha,
                                              PlatformAlpha, Facade, Interface,
                                              ObjectKey),
                          Route, B);
                      const auto Target =
                          Classes.at("Lfixture/Provider;").methods[0];
                      const auto Plans = validateGenericSignatures(Classes, B);
                      EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                          Target.reference));
                      const auto Report = recoverJava(Classes, B);
                      EXPECT_EQ(Report.getInteger("unrecovered_method_count"),
                                0);
                      EXPECT_FALSE(
                          Report.getArray("generic_signature_projections"));
                      const auto &After =
                          Classes.at(Target.reference.owner).methods[0];
                      EXPECT_EQ(After.generic_signature,
                                Target.generic_signature);
                      EXPECT_EQ(After.access, Target.access);
                    })());
                  }
}
TEST(MobileDalvikEnumTokenDefault,
     RejectChangedEnumRecursionTokensResultSlotsDeclarationsAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 30; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Input = enumTokenMapFixture(SDK, true, false, false, false, false,
                                         false, false);
        auto &Target = Input[1].methods[0], &Caller = Input[0].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          const auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Enum<TE;>", "Enum<+TE;>");
          break;
        case 1:
          Replace("Enum<TE;>", "Enum<TMissing;>");
          break;
        case 2:
          Replace("Enum<TE;>", "Enum");
          break;
        case 3:
          Replace("Class<TE;>", "Class<+TE;>");
          break;
        case 4:
          Replace("Class<TE;>", "Class");
          break;
        case 5:
          Replace("Class<TE;>", "Class<TMissing;>");
          break;
        case 6:
          Replace("Pair<Ljava/lang/String;", "Pair<TE;");
          break;
        case 7:
          Replace("String;TE;>", "String;+TE;>");
          break;
        case 8:
          Replace("String;TE;>", "String;-TE;>");
          break;
        case 9:
          Replace("String;TE;>", "String;*>");
          break;
        case 10:
          Replace("String;TE;>", "String;TMissing;>");
          break;
        case 11:
          Replace("Pair<Ljava/lang/String;TE;>", "Pair");
          break;
        case 12:
          Replace("Pair<Ljava/lang/String;TE;>",
                  "Pair<Ljava/lang/String;TE;TE;>");
          break;
        case 13:
          Input[5].generic_signature =
              "<Key:Ljava/lang/Object;E:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 14:
          Input[2].generic_signature =
              "<P:Ljava/lang/Enum<+TP;>;>Ljava/lang/Object;";
          break;
        case 15:
          Input[2].access.erase("public");
          break;
        case 16:
          Target.access.insert("native");
          break;
        case 17:
          Target.access.insert("abstract");
          break;
        case 18:
          Target.access.insert("bridge");
          break;
        case 19:
          Target.access.insert("synthetic");
          break;
        case 20:
          Target.access.insert("varargs");
          break;
        case 21:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 22:
          Target.access.erase("static");
          break;
        case 23:
          Input[1].generic_signature =
              "<Owner:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 24:
          Input[1].access.insert("interface");
          break;
        case 25:
          Target.declared_throws = {"Ljava/io/IOException;"};
          break;
        case 26:
          Caller.instructions[0].registers.clear();
          break;
        case 27:
          Caller.instructions[1].opcode = "move-result-wide";
          break;
        case 28:
          Input[1].access.erase("public");
          break;
        case 29:
          Input[5].access.erase("public");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(enumTokenMapInput(Input, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> enumCarrierKeyFixture(bool SDK, bool CarrierSDK, bool Range,
                                         bool Alpha, bool PlatformAlpha,
                                         bool Facade, bool Interface,
                                         bool Default) {
  auto Input = fixture(SDK, Range, Alpha, Default, false, PlatformAlpha);
  const std::string E = Alpha ? "Element" : "E", K = Alpha ? "Index" : "K",
                    P = "Lfixture/Pair;";
  auto &Target = Input[1].methods[0];
  Target.reference.parameters[0] = P;
  Target.generic_signature = "<" + E + ":Ljava/lang/Enum<T" + E + ";>;" + K +
                             ":Ljava/lang/Object;>(Lfixture/Pair<T" + E + ";T" +
                             K + ";>;T" + K + ";" +
                             (Default ? "T" + E + ";" : "") + ")T" + E + ";";
  auto Ref = Target.reference;
  auto Alias = klass("Lfixture/Facade;", SDK);
  Alias.superclass = Input[1].name;
  if (Facade)
    Ref.owner = Alias.name;
  auto &Caller = Input[0].methods[0];
  Caller.reference.parameters = Target.reference.parameters;
  const unsigned Start = Range ? 32 : 1;
  std::vector<unsigned> Words;
  for (unsigned I = 0; I < Target.reference.parameters.size(); ++I)
    Words.push_back(Start + I);
  Caller.registers = Start + Words.size();
  Caller.instructions = {
      op(0, Range ? "invoke-static/range" : "invoke-static", Words, Ref),
      op(3, "move-result-object", {0}), op(4, "return-object", {0})};
  Caller.code_end = 5;
  auto Pair = klass(P, CarrierSDK);
  Pair.generic_signature = "<" + E + ":Ljava/lang/Object;" + K +
                           ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (Interface)
    Pair.access = {"public", "abstract", "interface"};
  Input.push_back(Pair);
  Input.push_back(Alias);
  Input.push_back(klass("Ljava/lang/Number;", true));
  auto Checked = klass("Ljava/io/IOException;", true);
  Checked.superclass = "Ljava/lang/Exception;";
  Input.push_back(Checked);
  return Input;
}
} // namespace
TEST(MobileDalvikEnumTokenDefault,
     RecursiveEnumAndIndependentKeyFormalsRetainPairedInputsAndDefaults) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool PlatformAlpha : {false, true})
            for (bool Facade : {false, true})
              for (bool Interface : {false, true})
                for (bool Default : {false, true})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, CarrierSDK, Range, Alpha, PlatformAlpha,
                                   Facade, Interface, Default, Route}));
                    EXPECT_NO_THROW(([&] {
                      Budget B;
                      auto Classes = enumTokenMapInput(
                          enumCarrierKeyFixture(SDK, CarrierSDK, Range, Alpha,
                                                PlatformAlpha, Facade,
                                                Interface, Default),
                          Route, B);
                      const auto Target =
                          Classes.at("Lfixture/Provider;").methods[0];
                      const auto Plans = validateGenericSignatures(Classes, B);
                      EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                          Target.reference));
                      const auto Report = recoverJava(Classes, B);
                      EXPECT_EQ(Report.getInteger("unrecovered_method_count"),
                                0);
                      EXPECT_FALSE(
                          Report.getArray("generic_signature_projections"));
                      const auto &After =
                          Classes.at(Target.reference.owner).methods[0];
                      EXPECT_EQ(After.generic_signature,
                                Target.generic_signature);
                      EXPECT_EQ(After.access, Target.access);
                    })());
                  }
}
TEST(MobileDalvikEnumTokenDefault,
     RejectChangedPairedEnumBoundsKeysDefaultsCarrierDeclarationsAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 34; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Input = enumCarrierKeyFixture(SDK, true, false, false, false,
                                           false, true, true);
        auto &Target = Input[1].methods[0], &Caller = Input[0].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          const auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Enum<TE;>", "Enum<+TE;>");
          break;
        case 1:
          Replace("Enum<TE;>", "Enum<TMissing;>");
          break;
        case 2:
          Replace("Enum<TE;>", "Enum");
          break;
        case 3:
          Replace("Pair<TE;", "Pair<+TE;");
          break;
        case 4:
          Replace("Pair<TE;TK;>", "Pair");
          break;
        case 5:
          Replace("Pair<TE;", "Pair<TMissing;");
          break;
        case 6:
          Replace("TK;>;", "+TK;>;");
          break;
        case 7:
          Replace("TK;>;", "-TK;>;");
          break;
        case 8:
          Replace("TK;>;", "*>;");
          break;
        case 9:
          Replace("TK;>;", "TMissing;>;");
          break;
        case 10:
          Replace(";TK;TE;)", ";TMissing;TE;)");
          break;
        case 11:
          Replace(")TE;", ")TMissing;");
          break;
        case 12:
          Replace("TK;TE;)", "TK;TMissing;)");
          break;
        case 13:
          Input[5].generic_signature =
              "<E:Ljava/lang/Object;K:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 14:
          Input[2].generic_signature =
              "<P:Ljava/lang/Enum<+TP;>;>Ljava/lang/Object;";
          break;
        case 15:
          Input[2].access.erase("public");
          break;
        case 16:
          Target.access.insert("native");
          break;
        case 17:
          Target.access.insert("abstract");
          break;
        case 18:
          Target.access.insert("bridge");
          break;
        case 19:
          Target.access.insert("synthetic");
          break;
        case 20:
          Target.access.insert("varargs");
          break;
        case 21:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 22:
          Target.access.erase("static");
          break;
        case 23:
          Input[1].generic_signature =
              "<Owner:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 24:
          Input[1].access.insert("interface");
          break;
        case 25:
          Target.declared_throws = {"Ljava/io/IOException;"};
          break;
        case 26:
          Caller.instructions[0].registers.pop_back();
          break;
        case 27:
          Caller.instructions[1].opcode = "move-result-wide";
          break;
        case 28:
          Input[1].access.erase("public");
          break;
        case 29:
          Input[5].access.erase("public");
          break;
        case 30:
          Replace("<E:", "<Unused:Ljava/lang/Object;E:");
          break;
        case 31:
          Replace("K:Ljava/lang/Object;", "K:Ljava/lang/Number;");
          break;
        case 32:
          Replace(")TE;", ")TK;");
          break;
        case 33:
          Replace("Pair<TE;TK;>", "Pair<TK;TE;>");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(enumTokenMapInput(Input, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> enumTokenPredicateFixture(bool SDK, bool Range, bool Alpha,
                                             bool PlatformAlpha, bool Facade,
                                             bool TokenAlpha) {
  auto Input = fixture(SDK, Range, Alpha, false, false, PlatformAlpha);
  const std::string E = Alpha ? "Element" : "E",
                    Token = TokenAlpha ? E : "Token";
  auto &Target = Input[1].methods[0];
  Target.reference.parameters[1] = "Ljava/lang/String;";
  Target.reference.returns = "Z";
  Target.generic_signature = "<" + E + ":Ljava/lang/Enum<T" + E +
                             ";>;>(Ljava/lang/Class<T" + E +
                             ";>;Ljava/lang/String;)Z";
  if (!SDK)
    Target.instructions.back().opcode = "return";
  auto Ref = Target.reference;
  auto Alias = klass("Lfixture/Facade;", SDK);
  Alias.superclass = Input[1].name;
  if (Facade)
    Ref.owner = Alias.name;
  auto &Caller = Input[0].methods[0];
  Caller.reference.parameters = Target.reference.parameters;
  Caller.reference.returns = "Z";
  const unsigned Start = Range ? 32 : 1;
  Caller.registers = Start + 2;
  Caller.instructions = {op(0, Range ? "invoke-static/range" : "invoke-static",
                            {Start, Start + 1}, Ref),
                         op(3, "move-result", {0}), op(4, "return", {0})};
  Caller.code_end = 5;
  Input[3].generic_signature =
      "<" + Token + ":Ljava/lang/Object;>Ljava/lang/Object;";
  Input.push_back(klass("Ljava/lang/String;", true));
  Input.push_back(Alias);
  Input.push_back(klass("Ljava/lang/Number;", true));
  auto Checked = klass("Ljava/io/IOException;", true);
  Checked.superclass = "Ljava/lang/Exception;";
  Input.push_back(Checked);
  return Input;
}
} // namespace
TEST(MobileDalvikEnumTokenDefault,
     RecursiveEnumTokensRetainClosedStringContextsAndBooleanResults) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool PlatformAlpha : {false, true})
          for (bool Facade : {false, true})
            for (bool TokenAlpha : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Range, Alpha, PlatformAlpha, Facade,
                               TokenAlpha, Route}));
                EXPECT_NO_THROW(([&] {
                  Budget B;
                  auto Classes = enumTokenMapInput(
                      enumTokenPredicateFixture(
                          SDK, Range, Alpha, PlatformAlpha, Facade, TokenAlpha),
                      Route, B);
                  const auto Target =
                      Classes.at("Lfixture/Provider;").methods[0];
                  const auto Plans = validateGenericSignatures(Classes, B);
                  EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                      Target.reference));
                  const auto Report = recoverJava(Classes, B);
                  EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(
                      Report.getArray("generic_signature_projections"));
                  const auto &After =
                      Classes.at(Target.reference.owner).methods[0];
                  EXPECT_EQ(After.generic_signature, Target.generic_signature);
                  EXPECT_EQ(After.access, Target.access);
                })());
              }
}
TEST(MobileDalvikEnumTokenDefault,
     RejectChangedPredicateBoundsTokensContextsResultsDeclarationsAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 32; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Input =
            enumTokenPredicateFixture(SDK, false, false, false, false, false);
        auto &Target = Input[1].methods[0], &Caller = Input[0].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          const auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Enum<TE;>", "Enum<+TE;>");
          break;
        case 1:
          Replace("Enum<TE;>", "Enum<TMissing;>");
          break;
        case 2:
          Replace("Enum<TE;>", "Enum");
          break;
        case 3:
          Replace("Class<TE;>", "Class<+TE;>");
          break;
        case 4:
          Replace("Class<TE;>", "Class<-TE;>");
          break;
        case 5:
          Replace("Class<TE;>", "Class<*>");
          break;
        case 6:
          Replace("Class<TE;>", "Class");
          break;
        case 7:
          Replace("Class<TE;>", "Class<TMissing;>");
          break;
        case 8:
          Replace("Ljava/lang/String;)", "TE;)");
          break;
        case 9:
          Replace(")Z", ")I");
          break;
        case 10:
          Replace(")Z", ")TE;");
          break;
        case 11:
          Replace("<E:", "<Unused:Ljava/lang/Object;E:");
          break;
        case 12:
          Input[3].generic_signature =
              "<Token:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 13:
          Input[3].generic_signature = "<Token:Ljava/lang/Object;Other:Ljava/"
                                       "lang/Object;>Ljava/lang/Object;";
          break;
        case 14:
          Input[2].generic_signature =
              "<P:Ljava/lang/Enum<+TP;>;>Ljava/lang/Object;";
          break;
        case 15:
          Input[2].access.erase("public");
          break;
        case 16:
          Target.access.insert("native");
          break;
        case 17:
          Target.access.insert("abstract");
          break;
        case 18:
          Target.access.insert("bridge");
          break;
        case 19:
          Target.access.insert("synthetic");
          break;
        case 20:
          Target.access.insert("varargs");
          break;
        case 21:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 22:
          Target.access.erase("static");
          break;
        case 23:
          Input[1].generic_signature =
              "<Owner:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 24:
          Input[1].access.insert("interface");
          break;
        case 25:
          Target.declared_throws = {"Ljava/io/IOException;"};
          break;
        case 26:
          Caller.instructions[0].registers.pop_back();
          break;
        case 27:
          Caller.instructions[1].opcode = "move-result-object";
          break;
        case 28:
          Input[1].access.erase("public");
          break;
        case 29:
          Input[3].access.erase("public");
          break;
        case 30:
          Input[5].access.erase("public");
          break;
        case 31:
          Input[5].generic_signature =
              "<Text:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(enumTokenMapInput(Input, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}
