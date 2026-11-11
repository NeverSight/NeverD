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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Flag = true) {
  const std::string Object = "Ljava/lang/Object;",
                    Comparable = "Ljava/lang/Comparable;",
                    E = Alpha ? "Element" : "E";
  auto Bound = klass(Comparable, true);
  Bound.access = {"public", "interface", "abstract"};
  Bound.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Compare = method({Comparable, "compareTo", {Object}, "I"},
                        {"public", "abstract"}, 0, {});
  Compare.generic_signature = "(TC;)I";
  Bound.methods.push_back(Compare);
  auto Provider = klass("Lfixture/Provider;", SDK);
  auto Parameters = Flag ? std::vector<std::string>{Comparable, Comparable, "Z"}
                         : std::vector<std::string>{Comparable, Comparable};
  auto Constant = op(0, "const/16", {0});
  Constant.literal = int64_t(-37);
  auto Score =
      method({Provider.name, "score", Parameters, "I"}, {"public", "static"},
             Flag ? 4 : 3, {Constant, op(1, "return", {0})});
  Score.generic_signature = "<" + E + "::Ljava/lang/Comparable<-T" + E +
                            ";>;>(T" + E + ";T" + E + ";" + (Flag ? "Z" : "") +
                            ")I";
  if (SDK) {
    Score.instructions.clear();
    Score.registers = Score.code_end = 0;
  }
  Provider.methods.push_back(Score);
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(method(
      {Client.name, "call", Parameters, "I"}, {"public", "static"},
      Flag ? 4 : 3,
      {op(0, Range ? "invoke-static/range" : "invoke-static",
          Flag ? std::vector<unsigned>{1, 2, 3} : std::vector<unsigned>{1, 2},
          Score.reference),
       op(1, "move-result", {0}), op(2, "return", {0})}));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Provider, Client, Bound, Root};
}
} // namespace

TEST(MobileDalvikBoundedPair,
     RetainRecursiveSuperSelfPairsAndIndependentBooleanContext) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Flag : {false, true})
          for (unsigned Route : {0u, 1u, 2u}) {
            if (SDK && !Flag)
              continue;
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{SDK, Range, Alpha, Flag, Route}));
            auto Input = fixture(SDK, Range, Alpha, Flag);
            const auto Ref = Input[0].methods[0].reference;
            const auto Signature = Input[0].methods[0].generic_signature;
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
            EXPECT_TRUE(Plans.erased_generic_invocations.contains(Ref));
            EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature,
                      Signature);
            const auto R = recoverJava(Classes, B);
            EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
            EXPECT_FALSE(R.getArray("generic_signature_projections"));
          }
}

TEST(MobileDalvikBoundedPair,
     RejectChangedBoundsScopesFlagsResultsAndDeclarations) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false);
      auto &Score = Input[0].methods[0];
      auto &Call = Input[1].methods[0];
      const std::string Prefix = "<E::Ljava/lang/Comparable<-TE;>;>";
      if (Mutation == 0)
        Score.generic_signature = "<E::Ljava/lang/Comparable<+TE;>;>(TE;TE;Z)I";
      else if (Mutation == 1)
        Score.generic_signature = "<E::Ljava/lang/Comparable<TE;>;>(TE;TE;Z)I";
      else if (Mutation == 2)
        Score.generic_signature = "<E::Ljava/lang/Comparable<*>;>(TE;TE;Z)I";
      else if (Mutation == 3)
        Score.generic_signature = Prefix + "(TE;TMissing;Z)I";
      else if (Mutation == 4)
        Score.generic_signature =
            "<E::Ljava/lang/Comparable<-TE;>;U:Ljava/lang/Object;>(TE;TE;Z)I";
      else if (Mutation == 5) {
        auto Extra = Input[2].methods[0];
        Extra.reference.name = "other";
        Input[2].methods.push_back(Extra);
      } else if (Mutation == 6)
        Input[2].fields.push_back(Field{{Input[2].name, "extra", "I"},
                                        {"public", "static", "final"}});
      else if (Mutation == 7)
        Score.generic_signature = Prefix + "(TE;Ljava/lang/Object;Z)I";
      else if (Mutation == 8) {
        Score.reference.returns = "F";
        Call.reference.returns = "F";
        Score.generic_signature = Prefix + "(TE;TE;Z)F";
        Call.instructions[0].reference = Score.reference;
      } else if (Mutation == 9) {
        Score.reference.parameters[2] = "I";
        Call.reference.parameters[2] = "I";
        Score.generic_signature = Prefix + "(TE;TE;I)I";
        Call.instructions[0].reference = Score.reference;
      } else if (Mutation == 10)
        Score.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 11)
        Score.access.insert("native");
      else if (Mutation == 12)
        Score.access.insert("synthetic");
      else if (Mutation == 13)
        Score.access.insert("varargs");
      else if (Mutation == 14)
        Score.access = {"private", "static"};
      else if (Mutation == 15)
        Score.access.erase("static");
      else if (Mutation == 16)
        Input[0].generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 17)
        Call.instructions[0].registers.pop_back();
      else if (Mutation == 18)
        Input[2].methods[0].generic_signature = "(Ljava/lang/Object;)I";
      else
        Score.generic_signature =
            "<E::Ljava/lang/Comparable<-TOther;>;>(TE;TE;Z)I";
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
