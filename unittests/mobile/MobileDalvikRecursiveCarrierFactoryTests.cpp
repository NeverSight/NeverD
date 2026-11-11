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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool OwnerFormal,
                           bool BoundAlpha, bool Ignored,
                           bool PlatformOwner = false) {
  const std::string Object = "Ljava/lang/Object;",
                    Comparable = "Ljava/lang/Comparable;",
                    Comparator = "Ljava/util/Comparator;",
                    T = Alpha ? "Element" : "T", P = BoundAlpha ? "Item" : "T";
  auto Provider = klass(PlatformOwner ? Comparator : "Lfixture/Provider;", SDK);
  if (OwnerFormal || PlatformOwner)
    Provider.generic_signature =
        "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (PlatformOwner)
    Provider.access = {"public", "abstract", "interface"};
  MethodRef Selected{
      Provider.name, PlatformOwner ? "reverseOrder" : "select", {}, Comparator};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method(Selected, {"public", "static"}, SDK ? 0 : 1,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  Target.generic_signature = "<" + T + "::Ljava/lang/Comparable<-T" + T +
                             ";>;>()Ljava/util/Comparator<T" + T + ";>;";
  Provider.methods.push_back(Target);
  auto API = klass(Comparator, true);
  API.access = {"public", "abstract", "interface"};
  API.generic_signature = "<" + P + ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Compare = method({Comparator, "compare", {Object, Object}, "I"},
                        {"public", "abstract"}, 0, {});
  Compare.generic_signature = "(T" + P + ";T" + P + ";)I";
  API.methods.push_back(Compare);
  if (PlatformOwner) {
    Provider.generic_signature = API.generic_signature;
    Provider.methods.push_back(Compare);
  }
  auto Bound = klass(Comparable, true);
  Bound.access = {"public", "abstract", "interface"};
  Bound.generic_signature = "<" + P + ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto CompareTo = method({Comparable, "compareTo", {Object}, "I"},
                          {"public", "abstract"}, 0, {});
  CompareTo.generic_signature = "(T" + P + ";)I";
  Bound.methods.push_back(CompareTo);
  auto Client = klass("Lfixture/Client;");
  std::vector<Instruction> Code{
      op(0, Range ? "invoke-static/range" : "invoke-static", {}, Selected)};
  if (Ignored)
    Code.push_back(op(3, "return-void"));
  else {
    Code.push_back(op(3, "move-result-object", {0}));
    Code.push_back(op(4, "return-object", {0}));
  }
  Client.methods.push_back(
      method({Client.name, "call", {}, Ignored ? "V" : Comparator},
             {"public", "static"}, 1, Code));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  if (PlatformOwner)
    return {Client, Provider, Bound, Root};
  return {Client, Provider, Bound, Root, API};
}
} // namespace

TEST(MobileDalvikRecursiveCarrierFactory,
     RetainNullaryRecursiveBoundsInvariantResultsOriginalScopesAndDispatch) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool OwnerFormal : {false, true})
          for (bool BoundAlpha : {false, true})
            for (bool Ignored : {false, true})
              for (bool PlatformOwner : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  if (PlatformOwner && !SDK)
                    continue;
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, Range, Alpha, OwnerFormal, BoundAlpha,
                                 Ignored, PlatformOwner, Route}));
                  auto Input = fixture(SDK, Range, Alpha, OwnerFormal,
                                       BoundAlpha, Ignored, PlatformOwner);
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
                  EXPECT_TRUE(
                      Plans.erased_generic_invocations.contains(Selected));
                  EXPECT_FALSE(
                      Plans.erased_predicate_receiver_invocations.contains(
                          Selected));
                  const auto Report = recoverJava(Classes, B);
                  EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(
                      Report.getArray("generic_signature_projections"));
                  for (const auto &C : Original) {
                    const auto &After = Classes.at(C.name);
                    EXPECT_EQ(After.access, C.access);
                    EXPECT_EQ(After.generic_signature, C.generic_signature);
                    for (size_t I = 0; I < C.methods.size(); ++I) {
                      EXPECT_EQ(After.methods[I].reference,
                                C.methods[I].reference);
                      EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                      EXPECT_EQ(After.methods[I].generic_signature,
                                C.methods[I].generic_signature);
                    }
                  }
                }
  // The separate Object-bound nullary factory proof remains supported.
  for (bool SDK : {false, true}) {
    auto Input = fixture(SDK, true, false, true, false, false);
    Input[1].methods[0].generic_signature =
        "<T:Ljava/lang/Object;>()Ljava/util/Comparator<TT;>;";
    Budget B;
    const auto Classes = linkClasses(Input, B);
    const auto Plans = validateGenericSignatures(Classes, B);
    EXPECT_TRUE(Plans.erased_generic_invocations.contains(
        Input[1].methods[0].reference));
    const auto Report = recoverJava(Classes, B);
    EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
    EXPECT_FALSE(Report.getArray("generic_signature_projections"));
  }
}

TEST(MobileDalvikRecursiveCarrierFactory,
     RejectBorrowedFormalsChangedSelfBoundsCarrierSlotsAndInvocationRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 25; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, true, false, false);
      auto &Target = Input[1].methods[0];
      auto &Signature = *Target.generic_signature;
      if (Mutation == 0)
        Signature.replace(Signature.find("Comparable<-TT;>"), 16,
                          "Comparable<+TT;>");
      else if (Mutation == 1)
        Signature.replace(Signature.find("Comparator<TT;>"), 15,
                          "Comparator<+TT;>");
      else if (Mutation == 2)
        Signature.replace(Signature.find("Comparator<TT;>"), 15,
                          "Comparator<TMissing;>");
      else if (Mutation == 3)
        Signature = "<T::Ljava/lang/Comparable<-TT;>;U:Ljava/lang/"
                    "Object;>()Ljava/util/Comparator<TT;>;";
      else if (Mutation == 4)
        Signature =
            "<T::Ljava/lang/Comparable<-TT;>;>()Ljava/lang/Comparable<TT;>;";
      else if (Mutation == 5)
        Target.access.insert("bridge");
      else if (Mutation == 6)
        Signature = "<T::Ljava/lang/Comparable<-TT;>;>()TT;";
      else if (Mutation == 7)
        Input.pop_back();
      else if (Mutation == 8)
        Input[4].generic_signature =
            "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 9)
        Input[4].generic_signature = "<T:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 10)
        Input[2].methods[0].generic_signature = "(TMissing;)I";
      else if (Mutation == 11)
        Input[2].methods[0].reference.name = "unprovedCompare";
      else if (Mutation == 12)
        Input[2].access.erase("interface");
      else if (Mutation == 13) {
        Input[4].access.erase("public");
        Input[4].access.insert("private");
      } else if (Mutation == 14) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 15)
        Target.access.erase("static");
      else if (Mutation == 16)
        Target.access.insert("native");
      else if (Mutation == 17)
        Target.access.insert("abstract");
      else if (Mutation == 18)
        Target.access.insert("synthetic");
      else if (Mutation == 19)
        Target.access.insert("varargs");
      else if (Mutation == 20)
        Input[0].methods[0].instructions[0].opcode = "invoke-virtual";
      else if (Mutation == 21)
        Input[0].methods[0].instructions[0].registers = {0};
      else if (Mutation == 22)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/RuntimeException;"};
      else if (Mutation == 23)
        Signature =
            "<U::Ljava/lang/Comparable<-TU;>;>()Ljava/util/Comparator<TT;>;";
      else
        Signature =
            "<T::Ljava/lang/Comparable<*>;>()Ljava/util/Comparator<TT;>;";
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
