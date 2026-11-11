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
                           bool Reverse, bool Interface) {
  const std::string Object = "Ljava/lang/Object;", Pair = "Lfixture/Pair;",
                    K = Alpha ? "Left" : "K", V = Alpha ? "Right" : "V";
  const std::string Input = "Lfixture/Pair<T" + (Reverse ? V : K) + ";T" +
                            (Reverse ? K : V) + ";>;",
                    Output = "Lfixture/Pair<T" + (Reverse ? K : V) + ";T" +
                             (Reverse ? V : K) + ";>;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  auto Target = method({Provider.name, "reverse", {Pair}, Pair},
                       {"public", "static"}, SDK ? 0 : 2, {});
  Target.generic_signature = "<" + K + ":Ljava/lang/Object;" + V +
                             ":Ljava/lang/Object;>(" + Input + ")" + Output;
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Target.instructions = {Zero, op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  Provider.methods.push_back(Target);
  auto Core = klass("Lfixture/Core;");
  Core.methods.push_back(
      method({Core.name, "call", {Pair}, Pair}, {"public", "static"}, 2,
             {op(0, Range ? "invoke-static/range" : "invoke-static", {1},
                 Target.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto Carrier = klass(Pair, CarrierSDK);
  Carrier.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  if (Interface)
    Carrier.access = {"public", "interface", "abstract"};
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Provider,
          Core,
          Carrier,
          Root,
          klass("Ljava/lang/Number;", true),
          klass("Ljava/lang/String;", true)};
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

TEST(MobileDalvikReversedCarrierSlots,
     KeepTwoIndependentInvariantSlotsOriginalCarrierAndOpaqueResult) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Reverse : {false, true})
            for (bool Interface : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, CarrierSDK, Range, Alpha, Reverse, Interface, Route}));
                ASSERT_NO_THROW(([&] {
                  Budget B;
                  auto Classes = input(fixture(SDK, CarrierSDK, Range, Alpha,
                                               Reverse, Interface),
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
TEST(
    MobileDalvikReversedCarrierSlots,
    RejectForeignDuplicateVariantSlotsIncompleteDeclarationsErasuresRolesAndWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 24; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, false, false, true);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Sig = *Target.generic_signature;
          auto At = Sig.find(From);
          ASSERT_NE(At, std::string::npos);
          Sig.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("Pair<TV;TK;>", "Pair<TMissing;TK;>");
          break;
        case 1:
          replace("Pair<TV;TK;>", "Pair<TK;TK;>");
          break;
        case 2:
          replace("(Lfixture/Pair<TK;TV;>", "(Lfixture/Pair<+TK;TV;>");
          break;
        case 3:
          replace(")Lfixture/Pair<TV;TK;>", ")Lfixture/Pair<TV;+TK;>");
          break;
        case 4:
          replace("V:Ljava/lang/Object;>", "V:Ljava/lang/Number;>");
          break;
        case 5:
          Classes[2].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 6:
          Classes[2].generic_signature =
              "<A:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 7:
          Classes.erase(Classes.begin() + 2);
          break;
        case 8:
          replace(")Lfixture/Pair<TV;TK;>;", ")Ljava/lang/Object;");
          break;
        case 9:
          replace("(Lfixture/Pair<TK;TV;>;)", "(Ljava/lang/Object;)");
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
          Caller.instructions[0].registers.clear();
          break;
        case 18:
          Caller.instructions[0].registers[0] = Caller.registers;
          break;
        case 19:
          Caller.instructions[1].opcode = "move-result";
          break;
        case 20:
          replace(
              "<K:Ljava/lang/Object;V:Ljava/lang/Object;>",
              "<K:Ljava/lang/Object;V:Ljava/lang/Object;U:Ljava/lang/Object;>");
          break;
        case 21:
          Classes[0].generic_signature =
              "<E:Ljava/lang/Object;>Ljava/lang/Object;";
          replace("Pair<TV;TK;>", "Pair<TE;TK;>");
          break;
        case 22:
          replace(")Lfixture/Pair<TV;TK;>",
                  ")Lfixture/Pair<TV;Ljava/lang/String;>");
          break;
        case 23:
          Caller.instructions[0].opcode = "invoke-virtual";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
