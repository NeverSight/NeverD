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
std::vector<Class> fixture(bool SDK, bool Range, unsigned Variant) {
  const std::string Object = "Ljava/lang/Object;", Outer = "Lfixture/Outer;",
                    Pair = "Lfixture/Pair;",
                    Comparable = "Ljava/lang/Comparable;",
                    A = Variant & 1 ? "Element" : "A",
                    B = Variant & 1 ? "Ordered" : "B";
  auto Provider = klass("Lfixture/Provider;", SDK);
  if (Variant & 16)
    Provider.generic_signature = "<" + A + ":Ljava/lang/Object;" + B +
                                 ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (Variant % 3 == 0)
    Provider.access = {"public", "abstract", "interface"};
  MethodRef Selected{Provider.name, "factory", {}, Outer};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method(Selected, {"public", "static"}, SDK ? 0 : 1,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  const auto Plain = A + ":Ljava/lang/Object;";
  const auto Recursive = B + "::Ljava/lang/Comparable<-T" + B + ";>;";
  const auto Slots =
      Variant & 4 ? "T" + B + ";T" + A + ";" : "T" + A + ";T" + B + ";";
  Target.generic_signature =
      "<" + (Variant & 2 ? Recursive + Plain : Plain + Recursive) +
      ">()Lfixture/Outer<Lfixture/Pair<" + Slots + ">;>;";
  Provider.methods.push_back(Target);
  auto Unary = klass(Outer, Variant & 8);
  Unary.generic_signature = "<Item:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Binary = klass(Pair, Variant & 8);
  Binary.generic_signature =
      "<Left:Ljava/lang/Object;Right:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Bound = klass(Comparable, true);
  Bound.access = {"public", "abstract", "interface"};
  Bound.generic_signature = "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Compare = method({Comparable, "compareTo", {Object}, "I"},
                        {"public", "abstract"}, 0, {});
  Compare.generic_signature = "(TValue;)I";
  Bound.methods.push_back(Compare);
  auto Client = klass("Lfixture/Client;");
  std::vector<Instruction> Code{
      op(0, Range ? "invoke-static/range" : "invoke-static", {}, Selected)};
  const bool Ignored = Variant % 4 == 1;
  if (Ignored)
    Code.push_back(op(3, "return-void"));
  else {
    Code.push_back(op(3, "move-result-object", {0}));
    Code.push_back(op(4, "return-object", {0}));
  }
  Client.methods.push_back(
      method({Client.name, "call", {}, Ignored ? "V" : Outer},
             {"public", "static"}, 1, Code));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Client, Provider, Bound, Root, Unary, Binary};
}
} // namespace

TEST(MobileDalvikComparablePairFactory,
     RetainNestedIndependentFormalsRecursiveBoundsAndOriginalCalls) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (unsigned Variant = 0; Variant < 32; ++Variant)
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(
              ::testing::PrintToString(std::tuple{SDK, Range, Variant, Route}));
          auto Input = fixture(SDK, Range, Variant);
          const auto Original = Input;
          const auto Selected = Input[1].methods[0].reference;
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
          EXPECT_TRUE(Plans.erased_generic_invocations.contains(Selected));
          EXPECT_FALSE(
              Plans.erased_predicate_receiver_invocations.contains(Selected));
          const auto Report = recoverJava(Classes, B);
          EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
          EXPECT_FALSE(Report.getArray("generic_signature_projections"));
          for (const auto &C : Original) {
            const auto &After = Classes.at(C.name);
            EXPECT_EQ(After.access, C.access);
            EXPECT_EQ(After.generic_signature, C.generic_signature);
            for (size_t I = 0; I < C.methods.size(); ++I) {
              EXPECT_EQ(After.methods[I].reference, C.methods[I].reference);
              EXPECT_EQ(After.methods[I].access, C.methods[I].access);
              EXPECT_EQ(After.methods[I].generic_signature,
                        C.methods[I].generic_signature);
            }
          }
        }
}

TEST(MobileDalvikComparablePairFactory,
     RejectUnprovedNestedSlotsRecursiveBoundsAndOriginalInvocationRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 30; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, 16);
      auto &Target = Input[1].methods[0];
      auto &Signature = *Target.generic_signature;
      auto Replace = [&](const std::string &Before, const std::string &After) {
        const auto At = Signature.find(Before);
        ASSERT_NE(At, std::string::npos);
        Signature.replace(At, Before.size(), After);
      };
      if (Mutation == 0)
        Replace("Outer<Lfixture/Pair", "Outer<+Lfixture/Pair");
      else if (Mutation == 1)
        Replace("Pair<TA;TB;>", "Pair<+TA;TB;>");
      else if (Mutation == 2)
        Replace("Pair<TA;TB;>", "Pair<TA;-TB;>");
      else if (Mutation == 3)
        Replace("Pair<TA;TB;>", "Pair<TA;TA;>");
      else if (Mutation == 4)
        Replace("Pair<TA;TB;>", "Pair<TB;TB;>");
      else if (Mutation == 5)
        Replace("Pair<TA;TB;>", "Pair<TMissing;TB;>");
      else if (Mutation == 6)
        Replace("Comparable<-TB;>", "Comparable<-TA;>");
      else if (Mutation == 7)
        Replace("Comparable<-TB;>", "Comparable<+TB;>");
      else if (Mutation == 8)
        Replace("A:Ljava/lang/Object;", "A:Ljava/lang/Number;");
      else if (Mutation == 9)
        Replace(";>;>()", ";>;Extra:Ljava/lang/Object;>()");
      else if (Mutation == 10)
        Input.erase(Input.begin() + 4);
      else if (Mutation == 11)
        Input.pop_back();
      else if (Mutation == 12)
        Input[4].generic_signature =
            "<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 13)
        Input[5].generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 14)
        Input[4].generic_signature = "<X:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 15)
        Input[5].generic_signature =
            "<X:Ljava/lang/Object;Y:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 16)
        Input[2].methods[0].generic_signature = "(TMissing;)I";
      else if (Mutation == 17)
        Input[2].methods[0].reference.name = "unprovedCompare";
      else if (Mutation == 18)
        Input[2].access.erase("interface");
      else if (Mutation == 19) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 20)
        Target.access.erase("static");
      else if (Mutation == 21)
        Target.access.insert("native");
      else if (Mutation == 22)
        Target.access.insert("abstract");
      else if (Mutation == 23)
        Target.access.insert("bridge");
      else if (Mutation == 24)
        Target.access.insert("synthetic");
      else if (Mutation == 25)
        Target.access.insert("varargs");
      else if (Mutation == 26)
        Input[0].methods[0].instructions[0].registers = {0};
      else if (Mutation == 27)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/RuntimeException;"};
      else if (Mutation == 28)
        Replace(">()Lfixture/Outer<Lfixture/Pair<TA;TB;>;>;",
                ">()Lfixture/Pair<TA;TB;>;");
      else
        Input[0].methods[0].instructions[0].opcode = "invoke-virtual";
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
