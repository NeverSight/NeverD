//===- MobileDalvikCarrierNominalVarargsTests.cpp - Exact array tails
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify paired carrier scopes with fixed nominal contexts and array tails.
//===----------------------------------------------------------------------===//

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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Throws,
                           bool Varargs, unsigned Rank, bool Swap) {
  const std::string A = Alpha ? "Key" : "A", B = Alpha ? "Value" : "B",
                    Carrier = "Lfixture/Carrier;",
                    Context =
                        Alpha ? "Lfixture/Context;" : "Ljava/lang/String;",
                    Use = "Lfixture/Carrier<T" + A + ";T" + B + ";>;",
                    Tail = std::string(Rank, '[') + Context;
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{Swap ? Context : Carrier,
                                  Swap ? Carrier : Context, Context, Tail};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t{0};
  auto Target =
      method({Provider.name, "select", Params, Context}, {"public", "static"},
             SDK ? 0 : 5,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  Target.generic_signature =
      "<" + A + ":Ljava/lang/Object;" + B + ":Ljava/lang/Object;>(" +
      (Swap ? Context + Use : Use + Context) + Context + Tail + ")" + Context;
  if (Varargs)
    Target.access.insert("varargs");
  if (Throws)
    Target.declared_throws = std::vector<std::string>{"Lfixture/Failure;"};
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(
      method({Client.name, "call", Params, Context}, {"public", "static"}, 5,
             {op(0, Range ? "invoke-static/range" : "invoke-static",
                 {1, 2, 3, 4}, Target.reference),
              op(1, "move-result-object", {0}), op(2, "return-object", {0})}));
  auto Input = klass(Carrier, true);
  Input.generic_signature =
      "<Left:Ljava/lang/Object;Right:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Output = klass(Context, true);
  Output.access.insert("final");
  auto Root = klass("Ljava/lang/Object;", true);
  Root.superclass.reset();
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = Throwable.name;
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = Exception.name;
  auto Failure = klass("Lfixture/Failure;", true);
  Failure.superclass = Runtime.name;
  auto Number = klass("Ljava/lang/Number;", true);
  return {Client,    Provider,  Input,   Output,  Root,
          Throwable, Exception, Runtime, Failure, Number};
}
ClassMap input(std::vector<Class> Classes, unsigned Route, Budget &B) {
  if (Route == 2)
    std::reverse(Classes.begin(), Classes.end());
  if (Route)
    return linkClasses(Classes, B);
  ClassMap Out;
  for (auto &C : Classes)
    Out.emplace(C.name, std::move(C));
  return Out;
}
} // namespace
TEST(MobileDalvikCarrierNominalVarargs,
     RetainBothMethodFormalsAndExactClosedArrayContexts) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Throws : {false, true})
          for (bool Varargs : {false, true})
            for (unsigned Rank : {0u, 1u, 2u})
              for (bool Swap : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  if (Varargs && !Rank)
                    continue;
                  SCOPED_TRACE(::testing::PrintToString(std::tuple{
                      SDK, Range, Alpha, Throws, Varargs, Rank, Swap, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = input(
                        fixture(SDK, Range, Alpha, Throws, Varargs, Rank, Swap),
                        Route, B);
                    const auto Target =
                        Classes.at("Lfixture/Provider;").methods[0];
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                        Target.reference));
                    const auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    const auto &After =
                        Classes.at("Lfixture/Provider;").methods[0];
                    EXPECT_EQ(After.generic_signature,
                              Target.generic_signature);
                    EXPECT_EQ(After.access, Target.access);
                    EXPECT_EQ(After.declared_throws, Target.declared_throws);
                  })());
                }
}
TEST(MobileDalvikCarrierNominalVarargs,
     RejectIncompletePairScopesChangedErasuresAndMalformedTails) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, false, false, true, true, 1, false);
        auto &Target = Classes[1].methods[0];
        auto &Signature = *Target.generic_signature;
        auto Replace = [&](const std::string &From, const std::string &To) {
          const auto At = Signature.find(From);
          ASSERT_NE(At, std::string::npos);
          Signature.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Carrier<TA;TB;>", "Carrier<TA;TA;>");
          break;
        case 1:
          Replace("Carrier<TA;TB;>", "Carrier<+TA;TB;>");
          break;
        case 2:
          Replace("Carrier<TA;TB;>", "Carrier<*TB;>");
          break;
        case 3:
          Replace("Carrier<TA;TB;>", "Carrier<TMissing;TB;>");
          break;
        case 4:
          Replace("B:Ljava/lang/Object;>(",
                  "B:Ljava/lang/Object;Extra:Ljava/lang/Object;>(");
          break;
        case 5:
          Replace("A:Ljava/lang/Object;", "A:Ljava/lang/Number;");
          break;
        case 6:
          Replace("Carrier<TA;TB;>", "Carrier");
          break;
        case 7:
          Replace(">;Ljava/lang/String;", ">;Ljava/lang/Object;");
          break;
        case 8:
          Replace(")Ljava/lang/String;", ")Ljava/lang/Object;");
          break;
        case 9:
          Replace("[Ljava/lang/String;)", "[Ljava/lang/Object;)");
          break;
        case 10:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 11:
          Target.access.insert("native");
          break;
        case 12:
          Target.access.insert("abstract");
          break;
        case 13:
          Target.access.insert("synthetic");
          break;
        case 14:
          Target.access.insert("bridge");
          break;
        case 15:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 16:
          Target.declared_throws =
              std::vector<std::string>{"Lfixture/Missing;"};
          break;
        case 17:
          Classes[0].methods[0].instructions[0].registers.pop_back();
          break;
        case 18:
          Classes[2].generic_signature =
              "<Left:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 19:
          Target.reference.parameters.back() = "Ljava/lang/String;";
          Replace("[Ljava/lang/String;)", "Ljava/lang/String;)");
          Classes[0].methods[0].reference.parameters.back() =
              "Ljava/lang/String;";
          Classes[0].methods[0].instructions[0].reference = Target.reference;
          break;
        }
        Budget B;
        if (Mutation == 6) {
          // A raw carrier leaves both formals unused. Closed exceptions
          // retain the canonical unused-formal receipt and original slots.
          const auto Original = Target;
          const auto Linked = input(std::move(Classes), Route, B);
          const auto Plans = validateGenericSignatures(Linked, B);
          EXPECT_TRUE(Plans.erased_unused_formal_invocations.contains(
              Original.reference));
          EXPECT_TRUE(validatedClosedDeclaredThrows(Original, Plans));
          const auto R = recoverJava(Linked, B);
          EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
          EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
          EXPECT_FALSE(R.getArray("generic_signature_projections"));
          const auto &After = Linked.at(Original.reference.owner).methods[0];
          EXPECT_EQ(After.generic_signature, Original.generic_signature);
          EXPECT_EQ(After.access, Original.access);
          EXPECT_EQ(After.declared_throws, Original.declared_throws);
        } else {
          EXPECT_THROW(recoverJava(input(std::move(Classes), Route, B), B),
                       std::runtime_error);
        }
      }
}
