//===- MobileDalvikPairedSeedScalarTests.cpp - Scalar seed binding
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify two independent direct seeds beside primitive context words.
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

std::vector<Class> fixture(bool SDK, bool CarriersSDK, bool Range, bool Alpha,
                           bool Reverse, bool SwapSeeds, bool SwapResult,
                           const std::string &Scalar, bool Leading) {
  const std::string Object = "Ljava/lang/Object;",
                    A = Alpha ? "InputValue" : "A",
                    B = Alpha ? "OutputValue" : "B",
                    PairName = "Lfixture/Pair;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{Object, Object};
  const std::string Seed =
      std::string("T") + (SwapSeeds ? B : A) + ";T" + (SwapSeeds ? A : B) + ";";
  std::string Arguments = Seed;
  if (Leading) {
    Params.insert(Params.begin(), Scalar);
    Arguments = Scalar + Arguments;
  } else {
    Params.push_back(Scalar);
    Arguments += Scalar;
  }
  unsigned Count = Scalar == "J" || Scalar == "D" ? 4 : 3;
  auto Adapt = method({Provider.name, "make", Params, PairName},
                      {"public", "static"}, SDK ? 0 : Count + 1, {});
  const auto First = A + ":Ljava/lang/Object;",
             Second = B + ":Ljava/lang/Object;";
  Adapt.generic_signature = "<" + (Reverse ? Second + First : First + Second) +
                            ">(" + Arguments + ")Lfixture/Pair<T" +
                            (SwapResult ? B : A) + ";T" + (SwapResult ? A : B) +
                            ";>;";
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Adapt.instructions = {Zero, op(1, "return-object", {0})};
    Adapt.code_end = 2;
  }
  Provider.methods.push_back(Adapt);
  auto Core = klass("Lfixture/Core;");
  std::vector<unsigned> Words;
  for (unsigned i = 1; i <= Count; ++i)
    Words.push_back(i);
  Core.methods.push_back(method(
      {Core.name, "call", Params, PairName}, {"public", "static"}, Count + 1,
      {op(0, Range ? "invoke-static/range" : "invoke-static", Words,
          Adapt.reference),
       op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto Pair = klass(PairName, CarriersSDK);
  Pair.access = {"public", "interface", "abstract"};
  Pair.generic_signature =
      "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Provider,
          Core,
          Pair,
          Root,
          klass("Ljava/lang/Number;", true),
          klass("Ljava/lang/Exception;", true)};
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

TEST(MobileDalvikPairedSeedScalar,
     PreserveIndependentSeedAndResultScopesBesideExactScalarWords) {
  for (bool SDK : {false, true})
    for (bool CarriersSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Reverse : {false, true})
            for (bool SwapSeeds : {false, true})
              for (bool SwapResult : {false, true})
                for (const std::string Scalar :
                     {"Z", "B", "C", "S", "I", "J", "F", "D"})
                  for (bool Leading : {false, true})
                    for (unsigned Route : {0u, 1u, 2u}) {
                      SCOPED_TRACE(::testing::PrintToString(std::tuple{
                          SDK, CarriersSDK, Range, Alpha, Reverse, SwapSeeds,
                          SwapResult, Scalar, Leading, Route}));
                      ASSERT_NO_THROW(([&] {
                        Budget B;
                        auto Classes = input(
                            fixture(SDK, CarriersSDK, Range, Alpha, Reverse,
                                    SwapSeeds, SwapResult, Scalar, Leading),
                            Route, B);
                        const auto Target =
                            Classes.at("Lfixture/Provider;").methods[0];
                        const auto Plans =
                            validateGenericSignatures(Classes, B);
                        EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                            Target.reference));
                        const auto R = recoverJava(Classes, B);
                        EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                        EXPECT_FALSE(
                            R.getArray("generic_signature_projections"));
                        EXPECT_EQ(Classes.at(Target.reference.owner)
                                      .methods[0]
                                      .generic_signature,
                                  Target.generic_signature);
                      })());
                    }
}
TEST(MobileDalvikPairedSeedScalar,
     RejectMissingSeedsBorrowedScopesResultVarianceErasuresRolesAndWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 25; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, false, false, false, false,
                               Mutation == 24 ? "J" : "Z", false);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Sig = *Target.generic_signature;
          auto At = Sig.find(From);
          ASSERT_NE(At, std::string::npos);
          Sig.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("(TA;TB;", "(TA;TA;");
          break;
        case 1:
          replace("(TA;TB;", "(Ljava/lang/Object;TB;");
          break;
        case 2:
          replace("Pair<TA;TB;>", "Pair<TA;TA;>");
          break;
        case 3:
          replace("Pair<TA;TB;>", "Pair<+TA;TB;>");
          break;
        case 4:
          replace("Pair<TA;TB;>", "Pair<TA;-TB;>");
          break;
        case 5:
          replace("A:Ljava/lang/Object;", "A:Ljava/lang/Number;");
          break;
        case 6:
          replace("(TA;TB;", "(TMissing;TB;");
          break;
        case 7:
          replace("B:Ljava/lang/Object;>(",
                  "B:Ljava/lang/Object;C:Ljava/lang/Object;>(");
          break;
        case 8:
          Classes[2].generic_signature.reset();
          break;
        case 9:
          Classes[2].generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 10:
          Classes[2].generic_signature =
              "<K:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 11:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 12:
          Target.access.insert("native");
          break;
        case 13:
          Target.access.insert("abstract");
          break;
        case 14:
          Target.access.insert("bridge");
          break;
        case 15:
          Target.access.insert("synthetic");
          break;
        case 16:
          Target.access.insert("varargs");
          break;
        case 17:
          Target.declared_throws = {"Ljava/lang/Exception;"};
          break;
        case 18:
          Caller.instructions[0].registers.pop_back();
          break;
        case 19:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 20:
          Caller.instructions[1].opcode = "move-result";
          break;
        case 21:
          replace(";Z)", ";I)");
          break;
        case 22:
          replace(")Lfixture/Pair<TA;TB;>;", ")Ljava/lang/Object;");
          break;
        case 23:
          replace("(TA;TB;", "(TA;Ljava/lang/Number;");
          break;
        case 24:
          Caller.instructions[0].registers.back() = 1;
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}
