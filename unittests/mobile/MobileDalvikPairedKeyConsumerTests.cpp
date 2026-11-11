//===- MobileDalvikPairedKeyConsumerTests.cpp - Ordered key consumer
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify ordered binary transfer scopes with a complete unary key consumer.
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
                           bool Reverse, bool SwapPair, bool SwapInputs,
                           bool Lower, bool Producer, bool Same) {
  const std::string Object = "Ljava/lang/Object;",
                    A = Alpha ? "FirstValue" : "A",
                    B = Alpha ? "SecondValue" : "B", Key = SwapPair ? B : A,
                    Value = SwapPair ? A : B,
                    ToName = Same ? "Lfixture/From;" : "Lfixture/To;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{"Lfixture/From;", "Lfixture/Consumer;"};
  const std::string Pair = "Lfixture/From<" + std::string(Producer ? "+" : "") +
                           "T" + Key + ";" + (Producer ? "+" : "") + "T" +
                           Value + ";>;",
                    Consumer = "Lfixture/Consumer<" +
                               std::string(Lower ? "-" : "") + "T" + Key +
                               ";>;";
  if (SwapInputs)
    std::reverse(Params.begin(), Params.end());
  auto Adapt = method({Provider.name, "adapt", Params, ToName},
                      {"public", "static"}, SDK ? 0 : 3, {});
  const auto First = A + ":Ljava/lang/Object;",
             Second = B + ":Ljava/lang/Object;";
  Adapt.generic_signature =
      "<" + (Reverse ? Second + First : First + Second) + ">(" +
      (SwapInputs ? Consumer + Pair : Pair + Consumer) + ")" +
      ToName.substr(0, ToName.size() - 1) + "<T" + Key + ";T" + Value + ";>;";
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Adapt.instructions = {Zero, op(1, "return-object", {0})};
    Adapt.code_end = 2;
  }
  Provider.methods.push_back(Adapt);
  auto Core = klass("Lfixture/Core;");
  Core.methods.push_back(
      method({Core.name, "call", Params, ToName}, {"public", "static"}, 3,
             {op(0, Range ? "invoke-static/range" : "invoke-static", {1, 2},
                 Adapt.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto From = klass("Lfixture/From;", CarriersSDK);
  From.access = {"public", "interface", "abstract"};
  From.generic_signature =
      "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;";
  auto To = klass("Lfixture/To;", CarriersSDK);
  To.generic_signature = From.generic_signature;
  auto Consume = klass("Lfixture/Consumer;", CarriersSDK);
  Consume.access = {"public", "interface", "abstract"};
  Consume.generic_signature = "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Provider,
          Core,
          From,
          To,
          Consume,
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

TEST(MobileDalvikPairedKeyConsumer,
     RetainOrderedPairScopesAndExactFirstSlotConsumerAcrossDeclaredCarriers) {
  for (bool SDK : {false, true})
    for (bool CarriersSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Reverse : {false, true})
            for (bool SwapPair : {false, true})
              for (bool SwapInputs : {false, true})
                for (bool Lower : {false, true})
                  for (bool Producer : {false, true})
                    for (bool Same : {false, true})
                      for (unsigned Route : {0u, 1u, 2u}) {
                        SCOPED_TRACE(::testing::PrintToString(std::tuple{
                            SDK, CarriersSDK, Range, Alpha, Reverse, SwapPair,
                            SwapInputs, Lower, Producer, Same, Route}));
                        ASSERT_NO_THROW(([&] {
                          Budget B;
                          auto Classes =
                              input(fixture(SDK, CarriersSDK, Range, Alpha,
                                            Reverse, SwapPair, SwapInputs,
                                            Lower, Producer, Same),
                                    Route, B);
                          const auto Target =
                              Classes.at("Lfixture/Provider;").methods[0];
                          const auto Plans =
                              validateGenericSignatures(Classes, B);
                          EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                              Target.reference));
                          const auto R = recoverJava(Classes, B);
                          EXPECT_EQ(R.getInteger("unrecovered_method_count"),
                                    0);
                          EXPECT_FALSE(
                              R.getArray("generic_signature_projections"));
                          EXPECT_EQ(Classes.at(Target.reference.owner)
                                        .methods[0]
                                        .generic_signature,
                                    Target.generic_signature);
                        })());
                      }
}
TEST(MobileDalvikPairedKeyConsumer,
     RejectMissingOrReversedSlotsForeignConsumersVarianceRolesAndWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 25; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, false, false, false, false,
                               true, false, false);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Sig = *Target.generic_signature;
          auto At = Sig.find(From);
          ASSERT_NE(At, std::string::npos);
          Sig.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("From<TA;TB;>", "From<TMissing;TB;>");
          break;
        case 1:
          replace("A:Ljava/lang/Object;", "A:Ljava/lang/Number;");
          break;
        case 2:
          replace("B:Ljava/lang/Object;>(",
                  "B:Ljava/lang/Object;C:Ljava/lang/Object;>(");
          break;
        case 3:
          replace("From<TA;TB;>", "From<TA;TA;>");
          break;
        case 4:
          replace("To<TA;TB;>", "To<TB;TA;>");
          break;
        case 5:
          replace("Consumer<-TA;>", "Consumer<-TB;>");
          break;
        case 6:
          replace("Consumer<-TA;>", "Consumer<+TA;>");
          break;
        case 7:
          Classes[4].generic_signature.reset();
          break;
        case 8:
          Classes[4].generic_signature =
              "<Value:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 9:
          Classes[0].generic_signature =
              "<OwnerValue:Ljava/lang/Object;>Ljava/lang/Object;";
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
          Target.access.insert("bridge");
          break;
        case 14:
          Target.access.insert("synthetic");
          break;
        case 15:
          Target.access.insert("varargs");
          break;
        case 16:
          Target.declared_throws = {"Ljava/lang/Exception;"};
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
          replace(")Lfixture/To<TA;TB;>;", ")Ljava/lang/Object;");
          break;
        case 21:
          Classes[2].generic_signature =
              "<K:Ljava/lang/Object;V:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 22:
          Classes[3].generic_signature =
              "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 23:
          Classes.erase(Classes.begin() + 2);
          break;
        case 24:
          Classes.erase(Classes.begin() + 4);
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}
