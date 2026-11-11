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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Interface,
                           bool Copy) {
  const std::string Object = "Ljava/lang/Object;", E = Alpha ? "Element" : "E";
  auto Base = klass("Lfixture/Base;", SDK);
  Base.generic_signature = "<B:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Read =
      method({Base.name, "measure", {}, "I"}, {"public"}, SDK ? 0 : 2, {});
  auto Constant = op(0, "const/16", {0});
  Constant.literal = int64_t(-37);
  if (!SDK) {
    Read.instructions = {Constant, op(1, "return", {0})};
    Read.code_end = 2;
  }
  if (Interface) {
    Base.access = {"public", "abstract", "interface"};
    Read.access = {"public", "abstract"};
    Read.instructions.clear();
    Read.registers = Read.code_end = 0;
  }
  Base.methods = {Read};
  auto Slot = klass("Lfixture/Slot;", SDK);
  Slot.generic_signature = "<S:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Core = klass("Lfixture/Core;");
  Core.generic_signature = "<" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  MethodRef Selected{Core.name, "<init>", {Base.name, "I"}, "V"};
  MethodRef RootInit{Object, "<init>", {}, "V"};
  auto Target =
      method(Selected, {"public", "constructor"}, 3,
             {op(0, "invoke-direct", {0}, RootInit), op(3, "return-void")});
  Target.generic_signature = "(Lfixture/Base<Lfixture/Slot<T" + E + ";>;>;I)V";
  std::vector<Instruction> Code{op(0, "move-object", {0, 5}),
                                op(1, "move-object", {1, 6})};
  if (Copy)
    Code.push_back(op(2, "move-object", {3, 1}));
  Code.push_back(
      op(3,
         Interface ? (Range ? "invoke-interface/range" : "invoke-interface")
                   : (Range ? "invoke-virtual/range" : "invoke-virtual"),
         {Copy ? 3u : 1u}, Read.reference));
  Code.push_back(op(6, "move-result", {2}));
  Code.push_back(op(7, Range ? "invoke-direct/range" : "invoke-direct",
                    {0, 1, 2}, Selected));
  Code.push_back(op(10, "return-void"));
  auto Caller = method({Core.name, "<init>", {Base.name}, "V"},
                       {"public", "constructor"}, 7, Code);
  Caller.generic_signature = "(Lfixture/Base<Lfixture/Slot<T" + E + ";>;>;)V";
  Core.methods = {Caller, Target};
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods = {method(RootInit, {"public", "constructor"}, 0, {})};
  return {Core, Base, Slot, Root};
}
} // namespace

TEST(MobileDalvikScopedIntegerGetterDelegation,
     PreserveOriginalDynamicIntegerSnapshotsAndNestedScopedInputs) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Interface : {false, true})
          for (bool Copy : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, Interface, Copy, Route}));
              auto Input = fixture(SDK, Range, Alpha, Interface, Copy);
              const auto Original = Input;
              const auto Caller = Input[0].methods[0].reference,
                         Target = Input[0].methods[1].reference;
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
              EXPECT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
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
TEST(MobileDalvikScopedIntegerGetterDelegation,
     RejectUnprovedGetterDeclarationsScopesResultKindsAndInvocationWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 25; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, true, false);
      auto &Caller = Input[0].methods[0];
      auto &Target = Input[0].methods[1];
      auto &Getter = Input[1].methods[0];
      if (Mutation == 0) {
        Getter.access.erase("public");
        Getter.access.insert("private");
      } else if (Mutation == 1)
        Getter.access.insert("static");
      else if (Mutation == 2)
        Getter.access.insert("native");
      else if (Mutation == 3)
        Getter.access.insert("synthetic");
      else if (Mutation == 4)
        Getter.access.insert("bridge");
      else if (Mutation == 5)
        Getter.access.insert("varargs");
      else if (Mutation == 6)
        Getter.generic_signature = "<Extra:Ljava/lang/Object;>()I";
      else if (Mutation == 7)
        Getter.declared_throws =
            std::vector<std::string>{"Ljava/lang/RuntimeException;"};
      else if (Mutation == 8)
        Caller.instructions[2].opcode = "invoke-virtual";
      else if (Mutation == 9)
        Caller.instructions[2].registers.clear();
      else if (Mutation == 10)
        Caller.instructions[2].registers[0] = 0;
      else if (Mutation == 11)
        Caller.instructions[3].opcode = "move-result-object";
      else if (Mutation == 12)
        Caller.instructions[3].opcode = "move-result-wide";
      else if (Mutation == 13)
        Caller.instructions[3].registers[0] = 5;
      else if (Mutation == 14)
        Caller.instructions[4].registers.pop_back();
      else if (Mutation == 15)
        Caller.generic_signature =
            "(Lfixture/Base<Lfixture/Slot<TMissing;>;>;)V";
      else if (Mutation == 16)
        Target.generic_signature =
            "(Lfixture/Base<Lfixture/Slot<TMissing;>;>;I)V";
      else if (Mutation == 17)
        Input[2].generic_signature =
            "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 18)
        Input[1].generic_signature = "<B:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 19) {
        Getter.reference.returns = "J";
        std::get<MethodRef>(Caller.instructions[2].reference).returns = "J";
      } else if (Mutation == 20) {
        Target.reference.parameters[1] = "F";
        Target.generic_signature = "(Lfixture/Base<Lfixture/Slot<TE;>;>;F)V";
        std::get<MethodRef>(Caller.instructions[4].reference).parameters[1] =
            "F";
      } else if (Mutation == 21)
        Target.generic_signature =
            "<X:Ljava/lang/Object;>(Lfixture/Base<Lfixture/Slot<TE;>;>;I)V";
      else if (Mutation == 22)
        Target.access.insert("varargs");
      else if (Mutation == 23)
        Input[1].methods.clear();
      else {
        Caller.instructions[3].reference = std::string("Lfixture/Base;");
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
