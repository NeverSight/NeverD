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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Shadow,
                           bool Self, unsigned Context, bool Header) {
  const std::string Object = "Ljava/lang/Object;", Node = "Lfixture/Node;",
                    T = Alpha ? "MethodValue" : "T",
                    E = Shadow ? T : "ClassValue";
  auto Carrier = klass(Node, Self ? SDK : true);
  Carrier.generic_signature =
      "<" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (Header) {
    Carrier.interfaces = {"Lfixture/Tagged;"};
    *Carrier.generic_signature +=
        "Lfixture/Tagged<Lfixture/Node<T" + E + ";>;>;";
  }
  auto Provider = Self ? Carrier : klass("Lfixture/Provider;", SDK);
  if (!Self)
    Provider.generic_signature =
        "<" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  std::vector<std::string> Params{Object};
  const std::string ContextName = Context >= 2 ? "Lfixture/Context;" : Node;
  if (Context)
    Params.push_back(ContextName);
  auto Target = method({Provider.name, "make", Params, Node},
                       {"public", "static"}, SDK ? 0 : Params.size() + 1, {});
  Target.generic_signature = "<" + T + ":Ljava/lang/Object;>(T" + T + ";" +
                             (Context ? ContextName : "") + ")Lfixture/Node<T" +
                             T + ";>;";
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Target.instructions = {Zero, op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  Provider.methods.push_back(Target);
  auto Core = klass("Lfixture/Core;");
  std::vector<unsigned> Words{1};
  if (Context)
    Words.push_back(2);
  Core.methods.push_back(
      method({Core.name, "call", Params, Node}, {"public", "static"},
             Params.size() + 1,
             {op(0, Range ? "invoke-static/range" : "invoke-static", Words,
                 Target.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto Tag = klass("Lfixture/Tagged;", true);
  Tag.access = {"public", "interface", "abstract"};
  Tag.generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  std::vector<Class> Out{Provider,
                         Core,
                         Tag,
                         Root,
                         klass("Ljava/lang/Number;", true),
                         klass("Ljava/lang/String;", true)};
  if (!Self)
    Out.push_back(Carrier);
  auto Ctx = klass("Lfixture/Context;", true);
  if (Context == 3)
    Ctx.access = {"public", "interface", "abstract"};
  Out.push_back(Ctx);
  return Out;
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

TEST(MobileDalvikDirectSeedRawCarrier,
     KeepIndependentMethodSeedOptionalRawContextAndInvariantOpaqueResult) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Shadow : {false, true})
          for (bool Self : {false, true})
            for (unsigned Context : {0u, 1u, 2u, 3u})
              for (bool Header : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, Range, Alpha, Shadow, Self, Context,
                                 Header, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = input(fixture(SDK, Range, Alpha, Shadow,
                                                 Self, Context, Header),
                                         Route, B);
                    const auto Owner =
                        Self ? "Lfixture/Node;" : "Lfixture/Provider;";
                    const auto Target = Classes.at(Owner).methods[0];
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                        Target.reference));
                    const auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    EXPECT_EQ(Classes.at(Owner).methods[0].generic_signature,
                              Target.generic_signature);
                  })());
                }
}
TEST(MobileDalvikDirectSeedRawCarrier,
     RejectBorrowedScopeIncompleteCarriersErasuresVarianceRolesAndWordFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 26; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, false, false, false, true,
                               Mutation >= 24 ? 2 : 1, true);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Signature = *Target.generic_signature;
          auto At = Signature.find(From);
          ASSERT_NE(At, std::string::npos);
          Signature.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("(TT;", "(TMissing;");
          break;
        case 1:
          replace("(TT;", "(Ljava/lang/String;");
          break;
        case 2:
          replace("<T:Ljava/lang/Object;>", "<T:Ljava/lang/Number;>");
          break;
        case 3:
          replace(")Lfixture/Node<TT;>;", ")Lfixture/Node<+TT;>;");
          break;
        case 4:
          replace(")Lfixture/Node<TT;>;", ")Lfixture/Node<-TT;>;");
          break;
        case 5:
          replace(")Lfixture/Node<TT;>;", ")Lfixture/Node<TClassValue;>;");
          break;
        case 6:
          replace(";Lfixture/Node;)", ";Lfixture/Node<TT;>;)");
          break;
        case 7:
          replace(";Lfixture/Node;)", ";Ljava/lang/Object;)");
          break;
        case 8:
          replace(")Lfixture/Node<TT;>;", ")Ljava/lang/Object;");
          break;
        case 9:
          Classes[0].generic_signature =
              "<ClassValue:Ljava/lang/Number;>Ljava/lang/Object;";
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
          replace("T:Ljava/lang/Object;>",
                  "T:Ljava/lang/Object;U:Ljava/lang/Object;>");
          break;
        case 21:
          Classes[0].generic_signature.reset();
          break;
        case 22:
          replace(")Lfixture/Node<TT;>;",
                  ")Lfixture/Node<Ljava/lang/String;>;");
          break;
        case 23:
          Caller.instructions[0].opcode = "invoke-virtual";
          break;
        case 24:
          Classes.pop_back();
          break;
        case 25:
          Classes.back().generic_signature =
              "<X:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
