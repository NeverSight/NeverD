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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha,
                           bool CallerVariadic, bool TargetVariadic,
                           bool FormalArray) {
  const std::string Object = "Ljava/lang/Object;", T = Alpha ? "Element" : "T",
                    Array = FormalArray ? "[Ljava/lang/Object;"
                                        : "[Ljava/lang/String;",
                    ArrayType =
                        FormalArray ? "[T" + T + ";" : "[Ljava/lang/String;";
  auto Token = klass("Lfixture/Token;", true);
  Token.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Marker = klass("Lfixture/Marker;", SDK);
  Marker.access = {"public", "interface", "abstract"};
  Marker.generic_signature = "<M:Ljava/lang/Object;>Ljava/lang/Object;";
  auto String = klass("Ljava/lang/String;", true);
  auto Target = klass("Lfixture/Target;");
  Target.interfaces = {Marker.name};
  Target.generic_signature =
      "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Marker<T" + T +
      ";>;";
  MethodRef Selected{Target.name, "<init>", {"Z", Token.name, Array}, "V"};
  auto Sink =
      method(Selected, {"public", "constructor"}, 4,
             {op(0, "invoke-direct", {0}, MethodRef{Object, "<init>", {}, "V"}),
              op(1, "return-void")});
  Sink.generic_signature = "(ZLfixture/Token<T" + T + ";>;" + ArrayType + ")V";
  if (TargetVariadic)
    Sink.access.insert("varargs");
  auto Zero = op(1, "const/4", {1});
  Zero.literal = int64_t(0);
  auto Caller =
      method({Target.name, "<init>", {Token.name, Array}, "V"},
             {"public", "constructor"}, 7,
             {op(0, "move-object", {0, 4}), Zero, op(2, "move-object", {2, 5}),
              op(3, "move-object", {3, 6}),
              op(4, Range ? "invoke-direct/range" : "invoke-direct",
                 {0, 1, 2, 3}, Selected),
              op(5, "return-void")});
  Caller.generic_signature = "(Lfixture/Token<T" + T + ";>;" + ArrayType + ")V";
  if (CallerVariadic)
    Caller.access.insert("varargs");
  Target.methods = {Caller, Sink};
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  return {Target, Token, Marker, String, Root};
}
} // namespace

TEST(MobileDalvikTypedVariadicConstructor,
     RetainFullScopedArgumentsAndFixedArrayOverloadViews) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool CallerVariadic : {false, true})
          for (bool TargetVariadic : {false, true})
            for (bool FormalArray : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Range, Alpha, CallerVariadic,
                               TargetVariadic, FormalArray, Route}));
                auto Input = fixture(SDK, Range, Alpha, CallerVariadic,
                                     TargetVariadic, FormalArray);
                const auto Caller = Input[0].methods[0],
                           Target = Input[0].methods[1];
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
                EXPECT_TRUE(Plans.constructor_calls.contains(
                    {Caller.reference, Target.reference}));
                const auto &Methods =
                    Classes.at(Target.reference.owner).methods;
                EXPECT_EQ(Methods[0].access, Caller.access);
                EXPECT_EQ(Methods[1].access, Target.access);
                EXPECT_EQ(Methods[0].generic_signature,
                          Caller.generic_signature);
                EXPECT_EQ(Methods[1].generic_signature,
                          Target.generic_signature);
                const auto R = recoverJava(Classes, B);
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
              }
}

TEST(MobileDalvikTypedVariadicConstructor,
     RejectBrokenArrayViewsOverloadsScopesAndWordOrigins) {
  for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Input = fixture(true, true, false, true, true, false);
    auto &Caller = Input[0].methods[0];
    auto &Target = Input[0].methods[1];
    if (Mutation == 0)
      Caller.instructions[1].literal = int64_t(2);
    else if (Mutation == 1)
      Target.generic_signature = "(ZLfixture/Token<+TT;>;[Ljava/lang/String;)V";
    else if (Mutation == 2)
      Target.generic_signature =
          "(ZLfixture/Token<Ljava/lang/String;>;[Ljava/lang/String;)V";
    else if (Mutation == 3)
      Target.generic_signature =
          "(ZLfixture/Token<TMissing;>;[Ljava/lang/String;)V";
    else if (Mutation == 4)
      Target.generic_signature =
          "<U:Ljava/lang/Object;>(ZLfixture/Token<TT;>;[Ljava/lang/String;)V";
    else if (Mutation == 5) {
      Target.reference.parameters[2] = "Ljava/lang/Object;";
      Target.generic_signature = "(ZLfixture/Token<TT;>;Ljava/lang/Object;)V";
      Caller.instructions[4].reference = Target.reference;
    } else if (Mutation == 6) {
      Caller.reference.parameters[1] = Input[1].name;
      Caller.generic_signature = "(Lfixture/Token<TT;>;Lfixture/Token<TT;>;)V";
    } else if (Mutation == 7) {
      auto Lost = op(3, "const/4", {3});
      Lost.literal = int64_t(0);
      Caller.instructions[3] = Lost;
    } else if (Mutation == 8) {
      auto Lost = op(3, "const/4", {0});
      Lost.literal = int64_t(0);
      Caller.instructions[3] = Lost;
    } else if (Mutation == 9) {
      auto Sibling = Target;
      Sibling.reference.parameters[2] = "[Ljava/lang/Object;";
      Sibling.generic_signature =
          "<U:Ljava/lang/Object;>(ZLfixture/Token<TT;>;[Ljava/lang/Object;)V";
      Input[0].methods.push_back(Sibling);
    } else if (Mutation == 10)
      Target.declared_throws = {"Ljava/lang/Exception;"};
    else if (Mutation == 11)
      Target.access.insert("static");
    else if (Mutation == 12)
      Target.access.insert("synthetic");
    else if (Mutation == 13)
      Target.access.insert("native");
    else if (Mutation == 14)
      Caller.instructions[4].registers.pop_back();
    else if (Mutation == 15)
      Caller.generic_signature =
          "(Lfixture/Token<Ljava/lang/String;>;[Ljava/lang/String;)V";
    else if (Mutation == 16)
      Input[0].generic_signature = "<Other:Ljava/lang/Object;>Ljava/lang/"
                                   "Object;Lfixture/Marker<TOther;>;";
    else
      Caller.instructions[2].registers[1] = 6;
    Budget B;
    EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
  }
}
