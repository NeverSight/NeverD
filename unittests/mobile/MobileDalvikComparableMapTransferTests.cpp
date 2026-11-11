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
                           bool Interface, bool Reverse, bool Shadow) {
  const std::string Object = "Ljava/lang/Object;", Pair = "Lfixture/Pair;",
                    T = Alpha ? "Element" : "T", K = Alpha ? "Value" : "K";
  const std::string Slots =
      Reverse ? "T" + K + ";T" + T + ";" : "T" + T + ";T" + K + ";";
  const std::string Carrier = "Lfixture/Pair<" + Slots + ">;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  if (Shadow)
    Provider.generic_signature =
        "<" + K + ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Target = method({Provider.name, "transfer", {Pair, "Z"}, Pair},
                       {"public", "static"}, SDK ? 0 : 2, {});
  Target.generic_signature = "<" + T + ":Ljava/lang/Object;" + K +
                             "::Ljava/lang/Comparable<-T" + K + ";>;>(" +
                             Carrier + "Z)" + Carrier;
  if (!SDK) {
    Target.instructions = {op(0, "return-object", {0})};
    Target.code_end = 1;
  }
  Provider.methods.push_back(Target);
  auto Core = klass("Lfixture/Core;");
  Core.methods.push_back(
      method({Core.name, "call", {Pair, "Z"}, Pair}, {"public", "static"}, 3,
             {op(0, Range ? "invoke-static/range" : "invoke-static", {1, 2},
                 Target.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto CarrierType = klass(Pair, CarrierSDK);
  CarrierType.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  if (Interface)
    CarrierType.access = {"public", "interface", "abstract"};
  auto Comparable = klass("Ljava/lang/Comparable;", true);
  Comparable.access = {"public", "interface", "abstract"};
  Comparable.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Compare = method({Comparable.name, "compareTo", {Object}, "I"},
                        {"public", "abstract"}, 0, {});
  Compare.generic_signature = "(TC;)I";
  Comparable.methods.push_back(Compare);
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Provider,   Core, CarrierType,
          Comparable, Root, klass("Ljava/lang/Number;", true)};
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

TEST(MobileDalvikComparableMapTransfer,
     KeepInvariantSlotsRecursiveMethodBoundBooleanAndOpaqueCarrier) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Interface : {false, true})
            for (bool Reverse : {false, true})
              for (bool Shadow : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, CarrierSDK, Range, Alpha, Interface,
                                 Reverse, Shadow, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = input(fixture(SDK, CarrierSDK, Range, Alpha,
                                                 Interface, Reverse, Shadow),
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

TEST(MobileDalvikComparableMapTransfer,
     RejectUnprovedBoundsSlotVarianceErasuresRolesAndWordFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 24; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, false, true, false, false);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Signature = *Target.generic_signature;
          const auto At = Signature.find(From);
          ASSERT_NE(At, std::string::npos);
          Signature.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("Comparable<-TK;>", "Comparable<+TK;>");
          break;
        case 1:
          replace("Comparable<-TK;>", "Comparable<-TT;>");
          break;
        case 2:
          replace("(Lfixture/Pair<TT;TK;>", "(Lfixture/Pair<+TT;TK;>");
          break;
        case 3:
          replace(")Lfixture/Pair<TT;TK;>", ")Lfixture/Pair<TT;+TK;>");
          break;
        case 4:
          replace(")Lfixture/Pair<TT;TK;>", ")Lfixture/Pair<TK;TT;>");
          break;
        case 5:
          replace("(Lfixture/Pair<TT;TK;>", "(Lfixture/Pair<TMissing;TK;>");
          break;
        case 6:
          Classes.erase(Classes.begin() + 2);
          break;
        case 7:
          Classes[2].generic_signature =
              "<A:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 8:
          Classes[2].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 9:
          Classes[3].methods[0].generic_signature = "(Ljava/lang/Object;)I";
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
          replace(";Z)", ";I)");
          break;
        case 21:
          replace(")Lfixture/Pair<TT;TK;>;", ")Ljava/lang/Object;");
          break;
        case 22:
          replace("T:Ljava/lang/Object;K::",
                  "T:Ljava/lang/Object;U:Ljava/lang/Object;K::");
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

namespace {
std::vector<Class> comparableArrayPredicateClasses(bool SDK, bool Range,
                                                   bool Alpha, bool Rename) {
  auto Classes = fixture(SDK, true, Range, Alpha, false, false, false);
  auto &Target = Classes[0].methods[0];
  const std::string Formal = Alpha ? "Item" : "Value";
  Target.reference = {
      Classes[0].name, "test", {"[Ljava/lang/Comparable;"}, "Z"};
  Target.generic_signature = "<" + Formal + "::Ljava/lang/Comparable<-T" +
                             Formal + ";>;>([T" + Formal + ";)Z";
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Target.registers = 2;
    Target.instructions = {Zero, op(1, "return", {0})};
    Target.code_end = 2;
  }
  auto &Caller = Classes[1].methods[0];
  Caller.reference = {Classes[1].name, "call", Target.reference.parameters,
                      "Z"};
  Caller.registers = 2;
  Caller.instructions = {op(0, Range ? "invoke-static/range" : "invoke-static",
                            {1}, Target.reference),
                         op(3, "move-result", {0}), op(4, "return", {0})};
  Caller.code_end = 5;
  if (Rename) {
    Classes[3].generic_signature =
        "<Domain:Ljava/lang/Object;>Ljava/lang/Object;";
    Classes[3].methods[0].generic_signature = "(TDomain;)I";
  }
  return Classes;
}
} // namespace

TEST(MobileDalvikComparableMapTransfer,
     ArrayPredicatesRetainOwnRecursiveElementBoundAndBooleanResult) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Rename : {false, true})
          for (unsigned Route : {0u, 1u, 2u}) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{SDK, Range, Alpha, Rename, Route}));
            ASSERT_NO_THROW(([&] {
              Budget B;
              auto Classes = input(
                  comparableArrayPredicateClasses(SDK, Range, Alpha, Rename),
                  Route, B);
              const auto Target = Classes.at("Lfixture/Provider;").methods[0];
              const auto Plans = validateGenericSignatures(Classes, B);
              EXPECT_TRUE(
                  Plans.erased_generic_invocations.contains(Target.reference));
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

TEST(MobileDalvikComparableMapTransfer,
     ArrayPredicatesRejectChangedRecursionArrayScopesDeclarationsAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 24; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes =
            comparableArrayPredicateClasses(SDK, false, false, false);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Signature = *Target.generic_signature;
          const auto Pos = Signature.find(From);
          ASSERT_NE(Pos, std::string::npos);
          Signature.replace(Pos, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("<-TValue;>", "<TValue;>");
          break;
        case 1:
          replace("<-TValue;>", "<+TValue;>");
          break;
        case 2:
          replace("<-TValue;>", "<*>");
          break;
        case 3:
          replace("<-TValue;>", "<-TMissing;>");
          break;
        case 4:
          replace("[TValue;", "[TMissing;");
          break;
        case 5:
          replace("[TValue;", "[[TValue;");
          break;
        case 6:
          replace(")Z", ")I");
          break;
        case 7:
          replace("<Value:", "<Unused:Ljava/lang/Object;Value:");
          break;
        case 8:
          Target.access.insert("native");
          break;
        case 9:
          Target.access.insert("abstract");
          break;
        case 10:
          Target.access.insert("bridge");
          break;
        case 11:
          Target.access.insert("synthetic");
          break;
        case 12:
          Target.access.insert("varargs");
          break;
        case 13:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 14:
          Target.access.erase("static");
          break;
        case 15:
          Classes[0].access.insert("interface");
          Classes[0].access.insert("abstract");
          break;
        case 16:
          Classes[0].generic_signature =
              "<Owner:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 17:
          Target.declared_throws = {"Ljava/lang/Exception;"};
          break;
        case 18:
          Classes[3].methods[0].access.insert("static");
          break;
        case 19:
          Classes[3].methods[0].generic_signature = "(TMissing;)I";
          break;
        case 20:
          Caller.instructions[0].registers.clear();
          break;
        case 21:
          Caller.instructions[0].registers[0] = Caller.registers;
          break;
        case 22:
          Caller.instructions[1].opcode = "move-result-object";
          break;
        case 23:
          replace("Value::", "Value:Ljava/lang/Object;:");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> comparableArrayResultClasses(bool SDK, bool CarrierSDK,
                                                bool Range, bool Alpha,
                                                bool Interface, bool Context) {
  auto Classes =
      fixture(SDK, CarrierSDK, Range, Alpha, Interface, false, false);
  auto &Target = Classes[0].methods[0];
  const std::string Formal = Alpha ? "Item" : "Value",
                    Carrier = Classes[2].name;
  Classes[2].generic_signature = "<Slot:Ljava/lang/Object;>Ljava/lang/Object;";
  Target.reference = {Classes[0].name,
                      "pick",
                      {"[Ljava/lang/Comparable;"},
                      "Ljava/lang/Comparable;"};
  std::string Signature = "<" + Formal + "::Ljava/lang/Comparable<-T" + Formal +
                          ";>;>([T" + Formal + ";";
  if (Context) {
    Target.reference.parameters.push_back(Carrier);
    Signature += Carrier.substr(0, Carrier.size() - 1) + "<T" + Formal + ";>;";
  }
  Target.generic_signature = Signature + ")T" + Formal + ";";
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Target.registers = Context ? 3 : 2;
    Target.instructions = {Zero, op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  auto &Caller = Classes[1].methods[0];
  Caller.reference = {Classes[1].name, "call", Target.reference.parameters,
                      Target.reference.returns};
  Caller.registers = Context ? 3 : 2;
  Caller.instructions = {
      op(0, Range ? "invoke-static/range" : "invoke-static",
         Context ? std::vector<unsigned>{1, 2} : std::vector<unsigned>{1},
         Target.reference),
      op(3, "move-result-object", {0}), op(4, "return-object", {0})};
  Caller.code_end = 5;
  return Classes;
}
} // namespace

TEST(MobileDalvikComparableMapTransfer,
     ArrayResultsRetainRecursiveSeedInvariantContextAndOwnFormal) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Interface : {false, true})
            for (bool Context : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, CarrierSDK, Range, Alpha, Interface, Context, Route}));
                ASSERT_NO_THROW(([&] {
                  Budget B;
                  auto Classes = input(
                      comparableArrayResultClasses(SDK, CarrierSDK, Range,
                                                   Alpha, Interface, Context),
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

TEST(MobileDalvikComparableMapTransfer,
     ArrayResultsRejectLostRecursionArrayResultContextScopesAndRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 26; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes =
            comparableArrayResultClasses(SDK, true, false, false, true, true);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Signature = *Target.generic_signature;
          const auto Pos = Signature.find(From);
          ASSERT_NE(Pos, std::string::npos);
          Signature.replace(Pos, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("<-TValue;>", "<TValue;>");
          break;
        case 1:
          replace("<-TValue;>", "<+TValue;>");
          break;
        case 2:
          replace("<-TValue;>", "<*>");
          break;
        case 3:
          replace("<-TValue;>", "<-TMissing;>");
          break;
        case 4:
          replace("[TValue;", "[TMissing;");
          break;
        case 5:
          replace("[TValue;", "[[TValue;");
          break;
        case 6:
          replace(")TValue;", ")TMissing;");
          break;
        case 7:
          replace("<Value:", "<Unused:Ljava/lang/Object;Value:");
          break;
        case 8:
          replace("Pair<TValue;>", "Pair");
          break;
        case 9:
          replace("Pair<TValue;>", "Pair<+TValue;>");
          break;
        case 10:
          replace("Pair<TValue;>", "Pair<-TValue;>");
          break;
        case 11:
          replace("Pair<TValue;>", "Pair<*>");
          break;
        case 12:
          replace("Pair<TValue;>", "Pair<TMissing;>");
          break;
        case 13:
          Classes[2].generic_signature =
              "<Slot:Ljava/lang/Number;>Ljava/lang/Object;";
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
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 20:
          Target.access.erase("static");
          break;
        case 21:
          Classes[0].access.insert("interface");
          Classes[0].access.insert("abstract");
          break;
        case 22:
          Classes[0].generic_signature =
              "<Owner:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 23:
          Target.declared_throws = {"Ljava/lang/Exception;"};
          break;
        case 24:
          Caller.instructions[0].registers.pop_back();
          break;
        case 25:
          Caller.instructions[1].opcode = "move-result";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> comparableCarrierResultFixture(bool SDK, bool CarrierSDK,
                                                  bool Range, bool Alpha,
                                                  bool Interface, bool Facade,
                                                  bool Covariant, bool Used) {
  auto Classes =
      fixture(SDK, CarrierSDK, Range, Alpha, Interface, false, false);
  const std::string T = Alpha ? "Element" : "T", Carrier = "Lfixture/Carrier;",
                    Comparable = "Ljava/lang/Comparable;";
  auto &Provider = Classes[0];
  auto &Target = Provider.methods[0];
  Target.reference = {Provider.name, "select", {Carrier}, Comparable};
  Target.generic_signature = "<" + T + "::Ljava/lang/Comparable<-T" + T +
                             ";>;>(Lfixture/Carrier<" + (Covariant ? "+" : "") +
                             "T" + T + ";>;)T" + T + ";";
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Target.registers = 2;
    Target.instructions = {Zero, op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  Classes[2].name = Carrier;
  Classes[2].generic_signature =
      "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Alias = klass("Lfixture/Facade;", SDK);
  Alias.superclass = Provider.name;
  auto Ref = Target.reference;
  if (Facade)
    Ref.owner = Alias.name;
  auto &Caller = Classes[1].methods[0];
  const unsigned Start = Range ? 32 : 1;
  Caller.reference.parameters = {Carrier};
  Caller.reference.returns = Used ? Comparable : "V";
  Caller.registers = Start + 1;
  Caller.instructions = {
      op(0, Range ? "invoke-static/range" : "invoke-static", {Start}, Ref)};
  if (Used) {
    Caller.instructions.push_back(op(3, "move-result-object", {0}));
    Caller.instructions.push_back(op(4, "return-object", {0}));
  } else {
    Caller.instructions.push_back(op(3, "return-void"));
  }
  Caller.code_end = Caller.instructions.back().pc + 1;
  Classes.push_back(Alias);
  auto Checked = klass("Ljava/io/IOException;", true);
  Checked.superclass = "Ljava/lang/Exception;";
  Classes.push_back(Checked);
  return Classes;
}
} // namespace
TEST(MobileDalvikComparableMapTransfer,
     RecursiveCarrierInputsRetainOriginalComparableResultErasure) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Interface : {false, true})
            for (bool Facade : {false, true})
              for (bool Covariant : {false, true})
                for (bool Used : {false, true})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, CarrierSDK, Range, Alpha, Interface,
                                   Facade, Covariant, Used, Route}));
                    EXPECT_NO_THROW(([&] {
                      Budget B;
                      auto Classes =
                          input(comparableCarrierResultFixture(
                                    SDK, CarrierSDK, Range, Alpha, Interface,
                                    Facade, Covariant, Used),
                                Route, B);
                      const auto Target =
                          Classes.at("Lfixture/Provider;").methods[0];
                      const auto Plans = validateGenericSignatures(Classes, B);
                      EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                          Target.reference));
                      const auto Report = recoverJava(Classes, B);
                      EXPECT_EQ(Report.getInteger("unrecovered_method_count"),
                                0);
                      EXPECT_FALSE(
                          Report.getArray("generic_signature_projections"));
                      const auto &After =
                          Classes.at(Target.reference.owner).methods[0];
                      EXPECT_EQ(After.generic_signature,
                                Target.generic_signature);
                      EXPECT_EQ(After.access, Target.access);
                      EXPECT_EQ(After.reference.returns,
                                "Ljava/lang/Comparable;");
                    })());
                  }
}
TEST(MobileDalvikComparableMapTransfer,
     RejectUnboundCarrierProducersRecursiveScopesRolesAndResultErasures) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 36; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = comparableCarrierResultFixture(SDK, true, true, false,
                                                      true, false, false, true);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          const auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Comparable<-TT;>", "Comparable<TT;>");
          break;
        case 1:
          Replace("Comparable<-TT;>", "Comparable<+TT;>");
          break;
        case 2:
          Replace("Comparable<-TT;>", "Comparable<*>");
          break;
        case 3:
          Replace("Comparable<-TT;>", "Comparable<-TMissing;>");
          break;
        case 4:
          Replace("<T::", "<T:Ljava/lang/Object;:");
          break;
        case 5:
          Replace("<T::", "<T:");
          break;
        case 6:
          Replace("Carrier<TT;>", "Carrier");
          break;
        case 7:
          Replace("Carrier<TT;>", "Carrier<-TT;>");
          break;
        case 8:
          Replace("Carrier<TT;>", "Carrier<*>");
          break;
        case 9:
          Replace("Carrier<TT;>", "Carrier<TMissing;>");
          break;
        case 10:
          Replace(")TT;", ")TMissing;");
          break;
        case 11:
          Replace("<T:", "<Unused:Ljava/lang/Object;T:");
          break;
        case 12:
          Replace("Carrier<TT;>", "Carrier<TT;TT;>");
          break;
        case 13:
          Classes[2].generic_signature =
              "<T:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 14:
          Classes[2].generic_signature =
              "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 15:
          Classes[2].access.erase("public");
          break;
        case 16:
          Classes[3].methods[0].reference.returns = "J";
          break;
        case 17:
          Classes[3].methods[0].generic_signature = "(Ljava/lang/Object;)I";
          break;
        case 18:
          Classes[3].access.erase("interface");
          break;
        case 19:
          Classes[3].fields.push_back(
              {{Classes[3].name, "marker", "I"}, {"public", "static"}, {}});
          break;
        case 20:
          Classes[3].methods[0].access.insert("static");
          break;
        case 21:
          Target.access.insert("native");
          break;
        case 22:
          Target.access.insert("abstract");
          break;
        case 23:
          Target.access.insert("bridge");
          break;
        case 24:
          Target.access.insert("synthetic");
          break;
        case 25:
          Target.access.insert("varargs");
          break;
        case 26:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 27:
          Target.access.erase("static");
          break;
        case 28:
          Classes[0].generic_signature =
              "<Owner:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 29:
          Target.declared_throws = {"Ljava/io/IOException;"};
          break;
        case 30:
          Caller.instructions[0].registers.clear();
          break;
        case 31:
          Caller.instructions[1].opcode = "move-result-wide";
          break;
        case 32:
          Classes[2].generic_signature.reset();
          break;
        case 33:
          Classes.erase(Classes.begin() + 3);
          break;
        case 34:
          Target.reference.returns = "Ljava/lang/Object;";
          Caller.reference.returns = Target.reference.returns;
          Caller.instructions[0].reference = Target.reference;
          break;
        case 35:
          Replace(")TT;", ")Ljava/lang/Comparable;");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}
