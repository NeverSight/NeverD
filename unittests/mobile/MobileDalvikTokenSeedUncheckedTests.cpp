//===- MobileDalvikTokenSeedUncheckedTests.cpp - Token seed throws
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify an invariant token and direct seed with proven unchecked throws.
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

std::vector<Class> fixture(bool SDK, bool CarrierSDK, bool Range, bool Alpha,
                           unsigned Rotate, unsigned Throws, bool Interface) {
  const std::string Object = "Ljava/lang/Object;",
                    T = Alpha ? "MethodValue" : "T";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{"Lfixture/Token;", "Lfixture/Context;",
                                  Object};
  std::vector<std::string> Slots{"Lfixture/Token<T" + T + ";>;",
                                 "Lfixture/Context;", "T" + T + ";"};
  std::rotate(Params.begin(), Params.begin() + Rotate, Params.end());
  std::rotate(Slots.begin(), Slots.begin() + Rotate, Slots.end());
  auto Adapt = method({Provider.name, "adapt", Params, Object},
                      {"public", "static"}, SDK ? 0 : 4, {});
  Adapt.generic_signature = "<" + T + ":Ljava/lang/Object;>(" + Slots[0] +
                            Slots[1] + Slots[2] + ")T" + T + ";";
  if (Throws)
    Adapt.declared_throws = {Throws == 1 ? "Lfixture/Failure;"
                                         : "Ljava/lang/Error;"};
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
  auto Token = klass("Lfixture/Token;", CarrierSDK);
  Token.access = {"public", "interface", "abstract"};
  Token.generic_signature = "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Context = klass("Lfixture/Context;", true);
  if (Interface)
    Context.access = {"public", "interface", "abstract"};
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = Throwable.name;
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = Exception.name;
  auto Error = klass("Ljava/lang/Error;", true);
  Error.superclass = Throwable.name;
  auto Failure = klass("Lfixture/Failure;", true);
  Failure.superclass = Runtime.name;
  return {Provider,
          Core,
          Token,
          Context,
          Failure,
          Root,
          Throwable,
          Exception,
          Runtime,
          Error,
          klass("Ljava/lang/Number;", true)};
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

TEST(MobileDalvikTokenSeedUnchecked,
     PreserveOneMethodScopeAcrossTokenSeedResultAndUncheckedExceptions) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (unsigned Rotate : {0u, 1u, 2u})
            for (unsigned Throws : {0u, 1u, 2u})
              for (bool Interface : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, CarrierSDK, Range, Alpha, Rotate, Throws,
                                 Interface, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = input(fixture(SDK, CarrierSDK, Range, Alpha,
                                                 Rotate, Throws, Interface),
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
                    EXPECT_EQ(Classes.at(Target.reference.owner)
                                  .methods[0]
                                  .declared_throws,
                              Target.declared_throws);
                  })());
                }
}
TEST(MobileDalvikTokenSeedUnchecked,
     RejectForeignScopesIncompleteTokensCheckedThrowsErasuresRolesAndWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 23; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, false, 0, 1, false);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Sig = *Target.generic_signature;
          auto At = Sig.find(From);
          ASSERT_NE(At, std::string::npos);
          Sig.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace(";TT;)", ";TMissing;)");
          break;
        case 1:
          replace("Token<TT;>", "Token<TMissing;>");
          break;
        case 2:
          replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
          break;
        case 3:
          replace("T:Ljava/lang/Object;>(",
                  "T:Ljava/lang/Object;Extra:Ljava/lang/Object;>(");
          break;
        case 4:
          replace("Token<TT;>", "Token<+TT;>");
          break;
        case 5:
          replace("Token<TT;>", "Token<-TT;>");
          break;
        case 6:
          Classes[2].generic_signature.reset();
          break;
        case 7:
          Classes[2].generic_signature =
              "<Value:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 8:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 9:
          Target.access.insert("native");
          break;
        case 10:
          Target.access.insert("abstract");
          break;
        case 11:
          Target.access.insert("bridge");
          break;
        case 12:
          Target.access.insert("synthetic");
          break;
        case 13:
          Target.access.insert("varargs");
          break;
        case 14:
          Target.declared_throws = {"Ljava/lang/Exception;"};
          break;
        case 15:
          Target.declared_throws = {"Lfixture/Missing;"};
          break;
        case 16:
          Classes[4].superclass = "Ljava/lang/Exception;";
          break;
        case 17:
          Caller.instructions[0].registers.pop_back();
          break;
        case 18:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 19:
          Caller.instructions[1].opcode = "move-result";
          break;
        case 20:
          replace("Lfixture/Context;TT;", "Ljava/lang/Number;TT;");
          break;
        case 21:
          Classes[0].generic_signature =
              "<OwnerValue:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 22:
          replace(")TT;", ")TMissing;");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}
