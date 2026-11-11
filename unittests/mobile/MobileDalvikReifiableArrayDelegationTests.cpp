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
std::vector<Class> fixture(bool SDK, bool Range, bool CallerVarargs,
                           bool TargetVarargs, bool Alpha, unsigned Slots,
                           unsigned Rank, bool Parent) {
  const std::string Core = "Lfixture/Core;", Source = "Lfixture/Input;",
                    Context = "Lfixture/Context;",
                    Token = Slots == 1 ? "Lfixture/Token;" : "Lfixture/Pair;",
                    Object = "Ljava/lang/Object;";
  const std::string Array = std::string(Rank, '[') + Token,
                    GenericArray = std::string(Rank, '[') +
                                   Token.substr(0, Token.size() - 1) + "<" +
                                   std::string(Slots, '*') + ">;";
  auto Owner = klass(Core);
  if (Parent) {
    Owner.superclass = "Lfixture/Parent;";
    Owner.generic_signature = "Lfixture/Parent;";
  }
  MethodRef Selected{Core, "<init>", {Context, Array}, "V"};
  auto Target = method(Selected, {"public", "constructor"}, 3,
                       {op(0, "invoke-direct", {0},
                           MethodRef{*Owner.superclass, "<init>", {}, "V"}),
                        op(1, "return-void")});
  Target.generic_signature = "(" + Context + GenericArray + ")V";
  if (TargetVarargs)
    Target.access.insert("varargs");
  MethodRef Convert{Source, "convert", {}, Context};
  std::vector<Instruction> Code;
  if (Range)
    Code = {op(0, "move-object", {0, 4}),
            op(1, "invoke-virtual/range", {5}, Convert),
            op(2, "move-result-object", {1}),
            op(3, "move-object", {2, 6}),
            op(4, "invoke-direct/range", {0, 1, 2}, Selected),
            op(5, "return-void")};
  else
    Code = {op(0, "invoke-virtual", {2}, Convert),
            op(1, "move-result-object", {0}),
            op(2, "invoke-direct", {1, 0, 3}, Selected), op(3, "return-void")};
  auto Caller = method({Core, "<init>", {Source, Array}, "V"},
                       {"public", "constructor"}, Range ? 7 : 4, Code);
  Caller.generic_signature = "(" + Source + GenericArray + ")V";
  if (CallerVarargs)
    Caller.access.insert("varargs");
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Sibling = method({Core, "<init>", {"Ljava/lang/String;", Array}, "V"},
                        {"public", "constructor"}, 4,
                        {Zero, op(1, "invoke-direct", {1, 0, 3}, Selected),
                         op(2, "return-void")});
  Sibling.generic_signature = "(Ljava/lang/String;" + GenericArray + ")V";
  if (TargetVarargs)
    Sibling.access.insert("varargs");
  Owner.methods = {Caller, Target, Sibling};
  auto Input = klass(Source, SDK);
  Input.methods.push_back(method(
      Convert, {"public"}, SDK ? 0 : 2,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{Zero, op(1, "return-object", {0})}));
  auto Kind = klass(Token, SDK);
  Kind.generic_signature =
      "<" + std::string(Alpha ? "Element" : "T") + ":Ljava/lang/Object;" +
      (Slots == 2 ? "Other:Ljava/lang/Object;" : "") + ">Ljava/lang/Object;";
  auto Path = klass(Context, true);
  Path.access = {"public", "interface", "abstract"};
  auto String = klass("Ljava/lang/String;", true);
  String.access.insert("final");
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  auto Base = klass("Lfixture/Parent;", true);
  Base.methods.push_back(
      method({Base.name, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  return {Owner, Input, Kind, Path, String, Root, Base};
}
} // namespace

TEST(MobileDalvikReifiableArrayDelegation,
     RetainClosedWildcardArrayRanksOriginalVirtualResultsAndFixedVarargsViews) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool CallerVarargs : {false, true})
        for (bool TargetVarargs : {false, true})
          for (bool Alpha : {false, true})
            for (unsigned Slots : {1u, 2u})
              for (unsigned Rank : {1u, 2u})
                for (bool Parent : {false, true})
                  for (bool Signature : {false, true})
                    for (unsigned Route : {0u, 1u, 2u}) {
                      if (!Signature && TargetVarargs)
                        continue;
                      SCOPED_TRACE(::testing::PrintToString(std::tuple{
                          SDK, Range, CallerVarargs, TargetVarargs, Alpha,
                          Slots, Rank, Parent, Signature, Route}));
                      auto Input =
                          fixture(SDK, Range, CallerVarargs, TargetVarargs,
                                  Alpha, Slots, Rank, Parent);
                      if (!Signature)
                        Input[0].methods[0].generic_signature.reset();
                      const auto Original = Input;
                      const auto From = Input[0].methods[0].reference,
                                 To = Input[0].methods[1].reference;
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
                      EXPECT_TRUE(Plans.constructor_calls.contains({From, To}));
                      EXPECT_NO_THROW({
                        const auto Report = recoverJava(Classes, B);
                        EXPECT_EQ(Report.getInteger("unrecovered_method_count"),
                                  0);
                        EXPECT_FALSE(
                            Report.getArray("generic_signature_projections"));
                      });
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
                        }
                      }
                    }
}

TEST(MobileDalvikReifiableArrayDelegation,
     RejectUnboundOrVariantElementsChangedErasuresAndBrokenConstructorFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, false, true, true, false, 1, 1, false);
      auto &Caller = Input[0].methods[0], &Target = Input[0].methods[1],
           &Sibling = Input[0].methods[2];
      auto Replace = [&](Method &M, const std::string &From,
                         const std::string &To) {
        auto At = M.generic_signature->find(From);
        ASSERT_NE(At, std::string::npos);
        M.generic_signature->replace(At, From.size(), To);
      };
      if (Mutation == 0)
        Replace(Caller, "Token<*>", "Token<TMissing;>");
      else if (Mutation == 1)
        Replace(Target, "Token<*>", "Token<+Ljava/lang/String;>");
      else if (Mutation == 2)
        Replace(Target, ")V", ")Ljava/lang/Object;");
      else if (Mutation == 3)
        Caller.reference.parameters.back() = "[[Lfixture/Token;";
      else if (Mutation == 4)
        Replace(Target, "Lfixture/Context;", "Ljava/lang/String;");
      else if (Mutation == 5)
        Input[2].generic_signature =
            "<T:Ljava/lang/Object;Other:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 6)
        Target.access.insert("native");
      else if (Mutation == 7)
        Target.access.insert("static");
      else if (Mutation == 8)
        Caller.instructions[2].registers.erase(
            Caller.instructions[2].registers.begin());
      else if (Mutation == 9)
        Caller.instructions[0].opcode = "invoke-static";
      else if (Mutation == 10)
        *Target.generic_signature =
            "<T:Ljava/lang/Object;T:Ljava/lang/Object;>" +
            *Target.generic_signature;
      else
        Replace(Sibling, "Token<*>", "Token<TMissing;>");
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
