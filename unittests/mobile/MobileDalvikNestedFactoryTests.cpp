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
Class klass(std::string Name) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.source_id = "owned-fixture";
  return C;
}
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Allocated,
                           bool Marker) {
  const std::string Object = "Ljava/lang/Object;", E = Alpha ? "Element" : "E";
  auto Outer = klass("Lfixture/Outer;");
  Outer.access = {"public", "interface", "abstract"};
  Outer.generic_signature = "<O:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Inner = klass("Lfixture/Inner;");
  Inner.access = {"public", "interface", "abstract"};
  Inner.generic_signature = "<I:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Pair = klass("Lfixture/Pair;");
  Pair.access = {"public", "interface", "abstract"};
  Pair.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Factory = klass("Lfixture/Factory;");
  Factory.generic_signature =
      "<" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (Marker) {
    Factory.interfaces = {Pair.name};
    *Factory.generic_signature += "Lfixture/Pair<Lfixture/Inner<T" + E +
                                  ";>;Lfixture/Factory<T" + E + ";>;>;";
  }
  const std::string Input = "Lfixture/Outer<Lfixture/Inner<T" + E + ";>;>;";
  auto Init = method(
      {Factory.name, "<init>", {Outer.name}, "V"}, {"public", "constructor"}, 2,
      {op(0, "invoke-direct", {0}, MethodRef{Object, "<init>", {}, "V"}),
       op(1, "return-void")});
  Init.generic_signature = "(" + Input + ")V";
  auto From = method({Factory.name, "from", {Outer.name}, Factory.name},
                     {"public", "static"}, 2, {});
  From.generic_signature = "<" + E + ":Ljava/lang/Object;>(" + Input +
                           ")Lfixture/Factory<T" + E + ";>;";
  if (Allocated)
    From.instructions = {op(0, "new-instance", {0}, Factory.name),
                         op(1, Range ? "invoke-direct/range" : "invoke-direct",
                            {0, 1}, Init.reference),
                         op(2, "return-object", {0})};
  else {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    From.instructions = {Zero, op(1, "return-object", {0})};
  }
  From.code_end = From.instructions.back().pc + 1;
  Factory.methods = {From, Init};
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(
      method({Client.name, "call", {Outer.name}, Factory.name},
             {"public", "static"}, 2,
             {op(0, Range ? "invoke-static/range" : "invoke-static", {1},
                 From.reference),
              op(1, "move-result-object", {0}), op(2, "return-object", {0})}));
  auto Root = klass(Object);
  Root.external = true;
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  Outer.external = Inner.external = Pair.external = true;
  if (SDK) {
    Factory.external = true;
    for (auto &M : Factory.methods) {
      M.instructions.clear();
      M.registers = M.code_end = 0;
    }
  }
  return {Factory, Client, Outer, Inner, Pair, Root};
}
} // namespace

TEST(MobileDalvikNestedFactory,
     RetainIndependentStaticScopeAndCompleteNestedSelfContracts) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Allocated : {false, true})
          for (bool Marker : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, Allocated, Marker, Route}));
              auto Input = fixture(SDK, Range, Alpha, Allocated, Marker);
              const auto Ref = Input[0].methods[0].reference,
                         Original = Input[0].methods[1].reference;
              const auto Signature = Input[0].methods[0].generic_signature;
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
              EXPECT_TRUE(Plans.erased_generic_invocations.contains(Ref));
              EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature,
                        Signature);
              const auto R = recoverJava(Classes, B);
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
              EXPECT_EQ(Classes.at(Original.owner).methods[1].reference,
                        Original);
            }
}

TEST(MobileDalvikNestedFactory,
     RejectBorrowedVariantIncompleteAndChangedDeclarations) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, false, false);
      auto &From = Input[0].methods[0];
      const std::string Prefix = "<E:Ljava/lang/Object;>",
                        InputType = "Lfixture/Outer<Lfixture/Inner<TE;>;>;",
                        Output = "Lfixture/Factory<TE;>;";
      if (Mutation == 0)
        From.generic_signature =
            Prefix + "(Lfixture/Outer<Lfixture/Inner<TMissing;>;>; )" + Output;
      else if (Mutation == 1)
        From.generic_signature =
            Prefix + "(Lfixture/Outer<Lfixture/Inner<+TE;>;>; )" + Output;
      else if (Mutation == 2)
        From.generic_signature =
            Prefix + "(Lfixture/Outer<+Lfixture/Inner<TE;>;>; )" + Output;
      else if (Mutation == 3)
        From.generic_signature =
            Prefix + "(" + InputType + ")Lfixture/Factory<Ljava/lang/Object;>;";
      else if (Mutation == 4)
        From.generic_signature = "<E:Ljava/lang/Object;U:Ljava/lang/Object;>(" +
                                 InputType + ")" + Output;
      else if (Mutation == 5)
        From.generic_signature = "(" + InputType + ")" + Output;
      else if (Mutation == 6)
        From.access = {"private", "static"};
      else if (Mutation == 7)
        From.access.erase("static");
      else if (Mutation == 8)
        From.access.insert("native");
      else if (Mutation == 9)
        From.access.insert("synthetic");
      else if (Mutation == 10)
        From.access.insert("varargs");
      else if (Mutation == 11)
        From.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 12)
        Input[3].generic_signature = "<I:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 13)
        From.generic_signature =
            Prefix + "(Lfixture/Outer<Lfixture/Inner<TE;>;TE;>; )" + Output;
      else if (Mutation == 14)
        From.generic_signature =
            Prefix + "(" + InputType + ")Lfixture/Factory<+TE;>;";
      else if (Mutation == 15)
        Input[0].generic_signature =
            "<Other:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 16)
        Input[1].methods[0].instructions[0].registers.clear();
      else
        Input[3].access = {"private", "interface", "abstract"};
      if (From.generic_signature) {
        auto &T = *From.generic_signature;
        T.erase(std::remove(T.begin(), T.end(), ' '), T.end());
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
