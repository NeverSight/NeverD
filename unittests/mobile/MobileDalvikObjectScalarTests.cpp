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
Class klass(std::string Name, bool External = false) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.external = External;
  C.source_id = "owned-fixture";
  return C;
}
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha) {
  const std::string Object = "Ljava/lang/Object;", T = Alpha ? "Element" : "T";
  auto Provider = klass("Lfixture/Provider;", SDK);
  MethodRef Ref{Provider.name, "score", {Object, Object, "Z"}, "I"};
  auto Constant = op(0, "const/16", {0});
  Constant.literal = int64_t(-37);
  auto Score =
      method(Ref, {"public", "static"}, 4, {Constant, op(1, "return", {0})});
  Score.generic_signature =
      "<" + T + ":Ljava/lang/Object;>(T" + T + ";T" + T + ";Z)I";
  if (SDK) {
    Score.instructions.clear();
    Score.registers = Score.code_end = 0;
  }
  Provider.methods.push_back(Score);
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(method(
      {Client.name, "call", Ref.parameters, "I"}, {"public", "static"}, 4,
      {op(0, Range ? "invoke-static/range" : "invoke-static", {1, 2, 3}, Ref),
       op(1, "move-result", {0}), op(2, "return", {0})}));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Provider, Client, Root};
}
} // namespace

TEST(MobileDalvikObjectScalar, PreserveOwnedPairsAndOpaqueIntegerResults) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(
              ::testing::PrintToString(std::tuple{SDK, Range, Alpha, Route}));
          auto Input = fixture(SDK, Range, Alpha);
          const auto Ref = Input[0].methods[0].reference;
          const auto Original = Input[0].methods[0].generic_signature;
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
                    Original);
          const auto R = recoverJava(Classes, B);
          EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
          EXPECT_FALSE(R.getArray("generic_signature_projections"));
          EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
        }
}

TEST(MobileDalvikObjectScalar, RejectChangedScopeContractsAccessAndWordShapes) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false);
      auto &Score = Input[0].methods[0];
      auto &Call = Input[1].methods[0];
      if (Mutation == 0)
        Score.access = {"private", "static"};
      else if (Mutation == 1)
        Score.access.erase("static");
      else if (Mutation == 2) {
        Score.access.insert("native");
        Score.instructions.clear();
        Score.registers = Score.code_end = 0;
      } else if (Mutation == 3) {
        Input[0].access.insert("abstract");
        Score.access.insert("abstract");
        Score.instructions.clear();
        Score.registers = Score.code_end = 0;
      } else if (Mutation == 4)
        Score.access.insert("synthetic");
      else if (Mutation == 5)
        Score.access.insert("varargs");
      else if (Mutation == 6)
        Score.generic_signature =
            "<T:Ljava/lang/Object;U:Ljava/lang/Object;>(TT;TT;Z)I";
      else if (Mutation == 7)
        Score.generic_signature = "<T:Ljava/lang/Number;>(TT;TT;Z)I";
      else if (Mutation == 8)
        Score.generic_signature =
            "<T:Ljava/lang/Object;>(Ljava/lang/Object;TT;Z)I";
      else if (Mutation == 9)
        Score.generic_signature =
            "<T:Ljava/lang/Object;>(TT;Ljava/lang/Object;Z)I";
      else if (Mutation == 10) {
        Score.reference.parameters[2] = "I";
        Score.generic_signature = "<T:Ljava/lang/Object;>(TT;TT;I)I";
        Call.reference.parameters[2] = "I";
      } else if (Mutation == 11) {
        Score.reference.returns = "F";
        Score.generic_signature = "<T:Ljava/lang/Object;>(TT;TT;Z)F";
        Call.reference.returns = "F";
      } else if (Mutation == 12)
        Input[0].generic_signature = "<O:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 13)
        Score.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 14)
        Call.instructions[0].registers.pop_back();
      else
        Score.generic_signature = "<T:Ljava/lang/Object;>(TT;TMissing;Z)I";
      Call.instructions[0].reference = Score.reference;
      Budget B;
      if (Mutation == 8 || Mutation == 9) {
        const auto Before = Score;
        ASSERT_NO_THROW(([&] {
          const auto Classes = linkClasses(Input, B);
          const auto Plans = validateGenericSignatures(Classes, B);
          EXPECT_TRUE(
              Plans.erased_generic_invocations.contains(Before.reference));
          EXPECT_FALSE(Plans.erased_unused_formal_invocations.contains(
              Before.reference));
          if (Before.declared_throws && !Before.declared_throws->empty())
            EXPECT_TRUE(validatedClosedDeclaredThrows(Before, Plans));
          const auto Report = recoverJava(Classes, B);
          EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
          EXPECT_EQ(Report.getInteger("projected_method_count").value_or(0), 0);
          EXPECT_FALSE(Report.getArray("generic_signature_projections"));
          const auto &After = Classes.at(Before.reference.owner).methods[0];
          EXPECT_EQ(After.reference, Before.reference);
          EXPECT_EQ(After.generic_signature, Before.generic_signature);
          EXPECT_EQ(After.declared_throws, Before.declared_throws);
          EXPECT_EQ(After.access, Before.access);
        })());
      } else {
        EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
      }
    }
}
