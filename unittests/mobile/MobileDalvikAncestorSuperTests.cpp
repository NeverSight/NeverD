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
Class klass(std::string Name, std::optional<std::string> Parent) {
  Class C;
  C.name = std::move(Name);
  C.superclass = std::move(Parent);
  C.access = {"public"};
  C.source_id = "owned-fixture";
  return C;
}
std::vector<Class> fixture(bool SDK, bool Range, bool Plain, bool Alpha,
                           bool Native) {
  const std::string Object = "Ljava/lang/Object;";
  const std::string T = Alpha ? "Element" : "T";
  auto Core = klass("Lfixture/Core;", "Lfixture/Mid;");
  auto Mid = klass("Lfixture/Mid;", "Lfixture/Base;");
  auto Base = klass("Lfixture/Base;", Object);
  auto Root = klass(Object, {});
  Core.generic_signature =
      "<" + T + ":Ljava/lang/Object;>Lfixture/Mid<T" + T + ";>;";
  Mid.generic_signature = "<F:Ljava/lang/Object;>Lfixture/Base<TF;>;";
  Base.generic_signature = "<B:Ljava/lang/Object;>Ljava/lang/Object;";
  for (auto *C : {&Core, &Mid, &Base}) {
    MethodRef Init{*C->superclass, "<init>", {}, "V"};
    C->methods.push_back(
        method({C->name, "<init>", {}, "V"}, {"public", "constructor"}, 1,
               {op(0, "invoke-direct", {0}, Init), op(1, "return-void")}));
  }
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  auto Read = method({Base.name, "read", {Object}, Object}, {"protected"}, 2,
                     {op(0, "return-object", {1})});
  Read.generic_signature = "(TB;)TB;";
  Base.methods.push_back(Read);
  const MethodRef Clone{Object, "clone", {}, Object};
  Root.methods.push_back(method(Clone, {"protected", "native"}, 0, {}));
  const auto Target = Native ? Clone : Read.reference;
  auto Call = method(
      {Core.name, "run", Target.parameters, Object}, {"public"}, Native ? 2 : 3,
      {op(0, Range ? "invoke-super/range" : "invoke-super",
          Native ? std::vector<unsigned>{1} : std::vector<unsigned>{1, 2},
          Target),
       op(1, "move-result-object", {0}), op(2, "return-object", {0})});
  if (!Native)
    Call.generic_signature = "(T" + T + ";)T" + T + ";";
  Core.methods.push_back(Call);
  if (Plain) {
    for (auto *C : {&Core, &Mid, &Base}) {
      C->generic_signature.reset();
      for (auto &M : C->methods)
        M.generic_signature.reset();
    }
  }
  Root.external = true;
  Mid.external = Base.external = SDK;
  if (SDK)
    for (auto *C : {&Mid, &Base})
      for (auto &M : C->methods) {
        M.instructions.clear();
        M.registers = M.code_end = 0;
      }
  return {Core, Mid, Base, Root};
}
} // namespace

TEST(MobileDalvikAncestorSuper,
     PreserveNearestInheritedDeclarationsAndNativeRoots) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Plain : {false, true})
        for (bool Alpha : {false, true})
          for (bool Native : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Plain, Alpha, Native, Route}));
              auto Input = fixture(SDK, Range, Plain, Alpha, Native);
              const auto Caller = Input[0].methods[1].reference;
              const auto Target = std::get<MethodRef>(
                  Input[0].methods[1].instructions[0].reference);
              const auto Original =
                  Native ? Input[3].methods[1] : Input[2].methods[1];
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
              EXPECT_TRUE(Plans.superclass_calls.contains({Caller, Target}));
              ASSERT_TRUE(Plans.superclass_call_declarations.contains(
                  {Caller, Target}));
              EXPECT_EQ(Plans.superclass_call_declarations.at({Caller, Target}),
                        Original.reference);
              const auto R = recoverJava(Classes, B);
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
              const auto &Owner = Classes.at(Original.reference.owner);
              EXPECT_EQ(Owner.methods[1].generic_signature,
                        Original.generic_signature);
            }
}

TEST(MobileDalvikAncestorSuper,
     RejectShadowedUnrelatedMissingAndUnprovedReceivers) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, false, false);
      auto &Call = Input[0].methods[1];
      auto &Ref = std::get<MethodRef>(Call.instructions[0].reference);
      if (Mutation == 0)
        Ref.owner = Input[0].name;
      else if (Mutation == 1) {
        auto Other = Input[2];
        Other.name = "Lfixture/Other;";
        for (auto &M : Other.methods)
          M.reference.owner = Other.name;
        Input.push_back(Other);
        std::get<MethodRef>(Input[0].methods[1].instructions[0].reference)
            .owner = Other.name;
      } else if (Mutation == 2) {
        auto Override = Input[2].methods[1];
        Override.reference.owner = Input[1].name;
        Override.generic_signature = "(TF;)TF;";
        Input[1].methods.push_back(Override);
      } else if (Mutation == 3)
        Input[2].methods[1].access.insert("static");
      else if (Mutation == 4)
        Input[2].methods[1].access = {"private"};
      else if (Mutation == 5) {
        Input[2].access.insert("abstract");
        Input[2].methods[1].access = {"protected", "abstract"};
        Input[2].methods[1].instructions.clear();
      } else if (Mutation == 6)
        Input[1].generic_signature.reset();
      else if (Mutation == 7)
        Call.instructions[0].registers[0] = 2;
      else if (Mutation == 8)
        Call.instructions[0].registers.push_back(0);
      else if (Mutation == 9) {
        Input[2].methods[1].access.clear();
        Input[0].name = "Lother/Core;";
        for (auto &M : Input[0].methods)
          M.reference.owner = Input[0].name;
      } else if (Mutation == 10)
        Input.erase(Input.begin() + 2);
      else {
        Input[2].superclass = Input[1].name;
        Input[2].generic_signature = "<B:Ljava/lang/Object;>Lfixture/Mid<TB;>;";
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
