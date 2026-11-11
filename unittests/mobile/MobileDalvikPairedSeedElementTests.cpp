//===- MobileDalvikPairedSeedElementTests.cpp - Scoped seed binding
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify ordered binary carrier, direct seed and callback method scopes.
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
                           bool Reverse, bool Contra, bool Covar) {
  const std::string Object = "Ljava/lang/Object;",
                    A = Alpha ? "InputValue" : "A",
                    B = Alpha ? "OutputValue" : "B";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{"Lfixture/Pair;", Object,
                                  "Lfixture/Callback;"};
  auto Adapt = method({Provider.name, "adapt", Params, Object},
                      {"public", "static"}, SDK ? 0 : 4, {});
  const auto First = A + ":Ljava/lang/Object;",
             Second = B + ":Ljava/lang/Object;";
  Adapt.generic_signature =
      "<" + (Reverse ? Second + First : First + Second) + ">(Lfixture/Pair<T" +
      A + ";T" + B + ";>;T" + A + ";Lfixture/Callback<" + (Contra ? "-" : "") +
      "T" + A + ";" + (Covar ? "+" : "") + "T" + B + ";>;)T" + B + ";";
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Adapt.instructions = {Zero, op(1, "return-object", {0})};
    Adapt.code_end = 2;
  }
  Provider.methods.push_back(Adapt);
  auto Core = klass("Lfixture/Core;");
  Core.methods.push_back(
      method({Core.name, "call", Params, Object}, {"public", "static"}, 4,
             {op(0, Range ? "invoke-static/range" : "invoke-static", {1, 2, 3},
                 Adapt.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto Pair = klass("Lfixture/Pair;", CarriersSDK);
  Pair.access = {"public", "interface", "abstract"};
  Pair.generic_signature =
      "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Callback = klass("Lfixture/Callback;", CarriersSDK);
  Callback.access = {"public", "interface", "abstract"};
  Callback.generic_signature =
      "<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Provider,
          Core,
          Pair,
          Callback,
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

TEST(MobileDalvikPairedSeedElement,
     PreserveOrderedPairDirectSeedCallbackVarianceAndObjectResult) {
  for (bool SDK : {false, true})
    for (bool CarriersSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Reverse : {false, true})
            for (bool Contra : {false, true})
              for (bool Covar : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, CarriersSDK, Range, Alpha, Reverse,
                                 Contra, Covar, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = input(fixture(SDK, CarriersSDK, Range, Alpha,
                                                 Reverse, Contra, Covar),
                                         Route, B);
                    const auto Target =
                        Classes.at("Lfixture/Provider;").methods[0];
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                        Target.reference));
                    const auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    EXPECT_EQ(Classes.at(Target.reference.owner)
                                  .methods[0]
                                  .generic_signature,
                              Target.generic_signature);
                  })());
                }
}
TEST(MobileDalvikPairedSeedElement,
     RejectForeignScopesReversedRolesIncompleteCarriersFlagsErasuresAndWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 29; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, false, false, true, true);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Sig = *Target.generic_signature;
          auto At = Sig.find(From);
          ASSERT_NE(At, std::string::npos);
          Sig.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace(")TB;", ")TA;");
          break;
        case 1:
          replace("Pair<TA;TB;>", "Pair<TB;TA;>");
          break;
        case 2:
          replace(">;TA;Lfixture/Callback<", ">;TB;Lfixture/Callback<");
          break;
        case 3:
          replace("Callback<-TA;+TB;>", "Callback<-TB;+TB;>");
          break;
        case 4:
          replace("Callback<-TA;+TB;>", "Callback<-TA;+TA;>");
          break;
        case 5:
          replace("Callback<-TA;+TB;>", "Callback<+TA;+TB;>");
          break;
        case 6:
          replace("Callback<-TA;+TB;>", "Callback<-TA;-TB;>");
          break;
        case 7:
          replace("Pair<TA;TB;>", "Pair<+TA;TB;>");
          break;
        case 8:
          replace("A:Ljava/lang/Object;", "A:Ljava/lang/Number;");
          break;
        case 9:
          replace("TA;Lfixture/Callback<", "TMissing;Lfixture/Callback<");
          break;
        case 10:
          replace("B:Ljava/lang/Object;>(",
                  "B:Ljava/lang/Object;C:Ljava/lang/Object;>(");
          break;
        case 11:
          Classes[2].generic_signature.reset();
          break;
        case 12:
          Classes[3].generic_signature.reset();
          break;
        case 13:
          Classes[2].generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 14:
          Classes[3].generic_signature =
              "<X:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 15:
          Target.access.erase("public");
          Target.access.insert("private");
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
          Target.declared_throws = {"Ljava/lang/Exception;"};
          break;
        case 22:
          Caller.instructions[0].registers.pop_back();
          break;
        case 23:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 24:
          Caller.instructions[1].opcode = "move-result";
          break;
        case 25:
          replace("TA;Lfixture/Callback<",
                  "Ljava/lang/Number;Lfixture/Callback<");
          break;
        case 26:
          Classes.erase(Classes.begin() + 3);
          break;
        case 27:
          Classes[0].generic_signature =
              "<OwnerValue:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 28:
          replace("Pair<TA;TB;>", "Pair<TA;TA;>");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}
