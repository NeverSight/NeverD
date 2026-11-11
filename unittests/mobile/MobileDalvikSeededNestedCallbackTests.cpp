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
                           bool Reverse, bool Interface, bool Closed) {
  const std::string Object = "Ljava/lang/Object;", Outer = "Lfixture/Outer;",
                    SinkName = "Lfixture/Sink;", Context = "Lfixture/Context;",
                    A = Alpha ? "InputValue" : "A",
                    B = Alpha ? "SeedValue" : "B";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{Outer, Object, Context, SinkName};
  auto Adapt = method({Provider.name, "adapt", Params, Outer},
                      {"public", "static"}, SDK ? 0 : 5, {});
  const auto First = A + ":Ljava/lang/Object;",
             Second = B + ":Ljava/lang/Object;";
  Adapt.generic_signature =
      "<" + (Reverse ? Second + First : First + Second) + ">(Lfixture/Outer<T" +
      A + ";>;T" + B + ";" + Context + "Lfixture/Sink<T" + A + ";T" + B +
      ";>;)Lfixture/Outer<Lfixture/Target<T" + B + ";>;>;";
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Adapt.instructions = {Zero, op(1, "return-object", {0})};
    Adapt.code_end = 2;
  }
  Provider.methods.push_back(Adapt);
  auto Core = klass("Lfixture/Core;");
  Core.methods.push_back(
      method({Core.name, "call", Params, Outer}, {"public", "static"}, 5,
             {op(0, Range ? "invoke-static/range" : "invoke-static",
                 {1, 2, 3, 4}, Adapt.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto Carrier = klass(Outer, CarriersSDK);
  Carrier.access = {"public", "interface", "abstract"};
  Carrier.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Sink = klass(SinkName, CarriersSDK);
  Sink.access = {"public", "interface", "abstract"};
  Sink.generic_signature =
      "<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Accept = method({SinkName, "accept", {Object, "Lfixture/Target;"}, "V"},
                       {"public", "abstract"}, 0, {});
  Accept.generic_signature = "(TX;Lfixture/Target<TY;>;)V";
  Sink.methods.push_back(Accept);
  auto Target = klass("Lfixture/Target;", CarriersSDK);
  Target.generic_signature = "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
  if (Closed) {
    Target.superclass = "Lfixture/Base;";
    Target.generic_signature = "<Value:Ljava/lang/Object;>Lfixture/Base<Ljava/"
                               "lang/String;Ljava/lang/Object;>;";
  }
  auto Ctx = klass(Context, true);
  if (Interface)
    Ctx.access = {"public", "interface", "abstract"};
  auto Base = klass("Lfixture/Base;", true);
  Base.generic_signature =
      "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  Base.methods.push_back(
      method({Base.name, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  if (Closed)
    Target.methods.push_back(method(
        {Target.name, "<init>", {}, "V"}, {"public", "constructor"},
        CarriersSDK ? 0 : 1,
        CarriersSDK ? std::vector<Instruction>{}
                    : std::vector<Instruction>{op(0, "invoke-direct", {0},
                                                  Base.methods[0].reference),
                                               op(1, "return-void")}));
  return {Provider,
          Core,
          Carrier,
          Sink,
          Target,
          Ctx,
          Base,
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

TEST(
    MobileDalvikSeededNestedCallback,
    KeepIndependentOrderedRolesFixedContextCompleteNestedResultAndOriginalCalls) {
  for (bool SDK : {false, true})
    for (bool CarriersSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Reverse : {false, true})
            for (bool Interface : {false, true})
              for (bool Closed : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, CarriersSDK, Range, Alpha, Reverse,
                                 Interface, Closed, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = input(fixture(SDK, CarriersSDK, Range, Alpha,
                                                 Reverse, Interface, Closed),
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
TEST(MobileDalvikSeededNestedCallback,
     RejectForeignRolesIncompleteDeclarationsVarianceErasuresFlagsAndWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 26; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, false, false, false, true);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Sig = *Target.generic_signature;
          auto At = Sig.find(From);
          ASSERT_NE(At, std::string::npos);
          Sig.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("Target<TB;>", "Target<TA;>");
          break;
        case 1:
          replace("Sink<TA;TB;>", "Sink<TB;TA;>");
          break;
        case 2:
          replace("Outer<TA;>", "Outer<+TA;>");
          break;
        case 3:
          replace(")Lfixture/Outer<Lfixture/Target",
                  ")Lfixture/Outer<+Lfixture/Target");
          break;
        case 4:
          replace("Target<TB;>", "Target<-TB;>");
          break;
        case 5:
          replace(">;TB;Lfixture/Context;", ">;TMissing;Lfixture/Context;");
          break;
        case 6:
          replace("B:Ljava/lang/Object;>", "B:Ljava/lang/Number;>");
          break;
        case 7:
          Classes[3].generic_signature =
              "<X:Ljava/lang/Object;Y:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 8:
          Classes[4].generic_signature =
              "<Value:Ljava/lang/Number;>Lfixture/Base<Ljava/lang/String;Ljava/"
              "lang/Object;>;";
          break;
        case 9:
          Classes[5].generic_signature =
              "<X:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 10:
          Classes.erase(Classes.begin() + 5);
          break;
        case 11:
          Classes[3].generic_signature.reset();
          break;
        case 12:
          replace("Sink<TA;TB;>", "Sink<+TA;TB;>");
          break;
        case 13:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 14:
          Target.access.insert("native");
          break;
        case 15:
          Target.access.insert("abstract");
          break;
        case 16:
          Target.access.insert("bridge");
          break;
        case 17:
          Target.access.insert("synthetic");
          break;
        case 18:
          Target.access.insert("varargs");
          break;
        case 19:
          Target.declared_throws = {"Ljava/lang/Exception;"};
          break;
        case 20:
          Caller.instructions[0].registers.pop_back();
          break;
        case 21:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 22:
          Caller.instructions[1].opcode = "move-result";
          break;
        case 23:
          replace(">;TB;Lfixture/Context;",
                  ">;Ljava/lang/String;Lfixture/Context;");
          break;
        case 24:
          Classes[2].generic_signature =
              "<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 25:
          replace(
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;>",
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;C:Ljava/lang/Object;>");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
