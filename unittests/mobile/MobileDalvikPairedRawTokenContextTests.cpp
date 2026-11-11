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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Reverse,
                           bool Interface, bool ContextFirst) {
  const std::string Object = "Ljava/lang/Object;", Pair = "Lfixture/Pair;",
                    Token = "Ljava/lang/Class;", K = Alpha ? "Key" : "K",
                    V = Alpha ? "Value" : "V";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Parameters =
      ContextFirst ? std::vector<std::string>{Token, Token, Token, Object}
                   : std::vector<std::string>{Token, Token, Object, Token};
  MethodRef Selected{Provider.name, "convert", Parameters, Pair};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method(Selected, {"public", "static"}, SDK ? 0 : 5,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  const std::string Tokens = "Ljava/lang/Class<T" + K +
                             ";>;Ljava/lang/Class<T" + V +
                             ";>;Ljava/lang/Object;",
                    Context = "Ljava/lang/Class<+Lfixture/Pair;>;";
  Target.generic_signature =
      "<" + K + ":Ljava/lang/Object;" + V + ":Ljava/lang/Object;>(" +
      (ContextFirst ? Context + Tokens : Tokens + Context) +
      ")Lfixture/Pair<T" + (Reverse ? V : K) + ";T" + (Reverse ? K : V) + ";>;";
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(
      method({Client.name, "call", Parameters, Pair}, {"public", "static"}, 5,
             {op(0, Range ? "invoke-static/range" : "invoke-static",
                 {1, 2, 3, 4}, Selected),
              op(1, "move-result-object", {0}), op(2, "return-object", {0})}));
  auto PairClass = klass(Pair, true);
  PairClass.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  if (Interface)
    PairClass.access = {"public", "interface", "abstract"};
  auto ClassClass = klass(Token, true);
  ClassClass.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Client, Provider, PairClass, ClassClass, Root};
}
} // namespace

TEST(MobileDalvikPairedRawTokenContext,
     RetainCompleteDistinctTokensRawResultContextsAndSlotOrder) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Reverse : {false, true})
          for (bool Interface : {false, true})
            for (bool ContextFirst : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Range, Alpha, Reverse, Interface,
                               ContextFirst, Route}));
                auto Input = fixture(SDK, Range, Alpha, Reverse, Interface,
                                     ContextFirst);
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

TEST(MobileDalvikPairedRawTokenContext,
     RejectWrongRawBoundsBorrowedSlotsAndIncompleteContracts) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 22; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, false, true, true);
      auto &Target = Input[1].methods[0];
      auto &Caller = Input[0].methods[0];
      auto Replace = [&](const std::string &From, const std::string &To) {
        auto &Text = *Target.generic_signature;
        const auto At = Text.find(From);
        ASSERT_NE(At, std::string::npos);
        Text.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        Replace("Class<+Lfixture/Pair;>", "Class<+Ljava/lang/Object;>");
      else if (Mutation == 1)
        Replace("Class<+Lfixture/Pair;>", "Class<+Lfixture/Pair<TK;TV;>;>");
      else if (Mutation == 2)
        Replace("Class<+Lfixture/Pair;>", "Class<Lfixture/Pair;>");
      else if (Mutation == 3)
        Replace("Class<TK;>", "Class<TMissing;>");
      else if (Mutation == 4)
        Replace("Class<TV;>", "Class<TK;>");
      else if (Mutation == 5)
        Replace(")Lfixture/Pair<TK;TV;>", ")Lfixture/Pair<TK;TMissing;>");
      else if (Mutation == 6)
        Replace("K:Ljava/lang/Object;", "K:Ljava/lang/Class;");
      else if (Mutation == 7)
        Replace("<K:", "<U:Ljava/lang/Object;K:");
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
        Input[3].generic_signature =
            "<C:Ljava/lang/Object;D:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 16)
        Input[2].generic_signature = "<A:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 17)
        Replace(")Lfixture/Pair<TK;TV;>", ")Lfixture/Pair<TK;TK;>");
      else if (Mutation == 18)
        Caller.instructions[0].registers.pop_back();
      else if (Mutation == 19)
        Replace(")Lfixture/Pair<TK;TV;>", ")Ljava/lang/Object;");
      else if (Mutation == 20)
        Caller.instructions[0].opcode = "invoke-virtual";
      else
        Replace("Class<+Lfixture/Pair;>", "Class<+Lfixture/Missing;>");
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
