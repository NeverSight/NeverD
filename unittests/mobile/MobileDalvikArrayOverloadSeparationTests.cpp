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
std::vector<Class> fixture(bool SDK, bool Range, bool Variadic, unsigned Origin,
                           unsigned Kind) {
  const std::string Object = "Ljava/lang/Object;",
                    Array = Kind == 0   ? "[Ljava/lang/String;"
                            : Kind == 1 ? "[[Ljava/lang/String;"
                            : Kind == 2 ? "[B"
                                        : "[J";
  auto Base = klass("Lfixture/Base;", SDK);
  auto Child = klass("Lfixture/Child;");
  Child.superclass = Base.name;
  auto Init = method(
      {Base.name, "<init>", {Array}, "V"}, {"public", "constructor"},
      SDK ? 0 : 2,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{op(0, "invoke-direct", {0},
                                        MethodRef{Object, "<init>", {}, "V"}),
                                     op(1, "return-void")});
  if (Variadic)
    Init.access.insert("varargs");
  auto Sibling = method(
      {Base.name, "<init>", {"Ljava/util/Collection;"}, "V"},
      {"public", "constructor"}, SDK ? 0 : 2,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{op(0, "invoke-direct", {0},
                                        MethodRef{Object, "<init>", {}, "V"}),
                                     op(1, "return-void")});
  Sibling.generic_signature = "(Ljava/util/Collection<Ljava/lang/String;>;)V";
  Base.methods = {Init, Sibling};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  std::vector<Instruction> Code;
  unsigned Self, Value, Registers;
  std::vector<std::string> Params;
  std::optional<std::string> Signature;
  if (Origin == 0) {
    Params = {Array};
    Self = 0;
    Value = 1;
    Registers = 2;
  } else {
    Params = {"Ljava/util/Collection;"};
    Signature = "(Ljava/util/Collection<Ljava/lang/String;>;)V";
    Self = 1;
    Value = 0;
    Registers = 3;
    Code.push_back(Zero);
    if (Origin == 1)
      Code.push_back(op(1, "new-array", {0, 0}, Array));
  }
  Code.push_back(op(2, Range ? "invoke-direct/range" : "invoke-direct",
                    {Self, Value}, Init.reference));
  Code.push_back(op(3, "return-void"));
  if (Range && Origin) {
    Code.clear();
    auto Z = Zero;
    Z.registers = {1};
    Code.push_back(Z);
    if (Origin == 1)
      Code.push_back(op(1, "new-array", {1, 1}, Array));
    Code.push_back(op(2, "invoke-direct/range", {0, 1}, Init.reference));
    Code.push_back(op(3, "return-void"));
    Registers = 2;
  }
  auto Caller = method({Child.name, "<init>", Params, "V"},
                       {"public", "constructor"}, Registers, Code);
  Caller.generic_signature = Signature;
  Child.methods = {Caller};
  auto Collection = klass("Ljava/util/Collection;", true);
  Collection.access = {"public", "abstract", "interface"};
  Collection.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  auto String = klass("Ljava/lang/String;", true);
  String.access.insert("final");
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  return {Child, Base, Collection, String, Root};
}
} // namespace

TEST(MobileDalvikArrayOverloadSeparation,
     RetainExactArrayDescriptorsBesideUnrelatedGenericCollectionOverloads) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Variadic : {false, true})
        for (unsigned Origin : {0u, 1u, 2u})
          for (unsigned Kind : {0u, 1u, 2u, 3u})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Variadic, Origin, Kind, Route}));
              auto Input = fixture(SDK, Range, Variadic, Origin, Kind);
              const auto Original = Input;
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
              const auto &Shape =
                  Plans.methods.at(Original[1].methods[1].reference);
              SCOPED_TRACE("sibling erasure=" +
                           Shape.types.at(Shape.parameters[0]).erasure +
                           ", projections=" +
                           std::to_string(Shape.source_projections.size()));
              EXPECT_NO_THROW({
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              });
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

TEST(MobileDalvikArrayOverloadSeparation,
     RejectUnprovedSiblingErasuresArrayOriginsAndOverloadClaims) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, false, true, 1, 0);
      auto &Target = Input[1].methods[0];
      auto &Sibling = Input[1].methods[1];
      auto &Call = Input[0].methods[0];
      if (Mutation == 0)
        Sibling.generic_signature = "(Ljava/util/List<Ljava/lang/String;>;)V";
      else if (Mutation == 1)
        Sibling.generic_signature = "(Ljava/util/Collection<TMissing;>;)V";
      else if (Mutation == 2)
        Sibling.generic_signature = "(Ljava/util/Collection<V>;)V";
      else if (Mutation == 3) {
        Sibling.reference.parameters = {"Ljava/io/Serializable;"};
        Sibling.generic_signature = "<T::Ljava/io/Serializable;>(TT;)V";
        auto C = klass("Ljava/io/Serializable;", true);
        C.access = {"public", "abstract", "interface"};
        Input.push_back(C);
      } else if (Mutation == 4) {
        Sibling.reference.parameters = {"Ljava/lang/Cloneable;"};
        Sibling.generic_signature = "<T::Ljava/lang/Cloneable;>(TT;)V";
        auto C = klass("Ljava/lang/Cloneable;", true);
        C.access = {"public", "abstract", "interface"};
        Input.push_back(C);
      } else if (Mutation == 5)
        Call.instructions[2].registers.pop_back();
      else if (Mutation == 6)
        Call.instructions[2].opcode = "invoke-static";
      else if (Mutation == 7) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 8)
        Call.instructions[1].reference = "[Ljava/lang/Object;";
      else {
        Sibling.reference.parameters = {"Ljava/lang/Object;"};
        Sibling.generic_signature = "<T:Ljava/lang/Object;>(TT;)V";
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
