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
std::vector<Class> fixture(unsigned ContextKind, bool SDK, bool Range,
                           bool Alpha, bool SeedFirst, bool Frame, bool Final) {
  const std::string Bound = "Ljava/lang/Appendable;", Name = "Lfixture/Core;",
                    T = Alpha ? "Buffer" : "T", Object = "Ljava/lang/Object;";
  const std::vector<std::string> Contexts{"Ljava/util/Calendar;", "I", "J",
                                          "[I", "Ljava/util/List;"};
  const std::string Context = Contexts.at(ContextKind),
                    ContextSignature =
                        ContextKind == 4 ? "Ljava/util/List<*>;" : Context;
  const unsigned ContextWords = ContextKind == 2 ? 2 : 1,
                 Words = ContextWords + 2, First = Frame ? 4 : 1,
                 Seed = SeedFirst ? First + 1 : First + 1 + ContextWords;
  auto Core = klass(Name);
  auto Target =
      method({Name, "select",
              SeedFirst ? std::vector<std::string>{Bound, Context}
                        : std::vector<std::string>{Context, Bound},
              Bound},
             {"private"}, First + Words, {op(0, "return-object", {Seed})});
  if (Final)
    Target.access.insert("final");
  Target.generic_signature = "<" + T + "::" + Bound + ">(" +
                             (SeedFirst ? "T" + T + ";" + ContextSignature
                                        : ContextSignature + "T" + T + ";") +
                             ")T" + T + ";";
  std::vector<unsigned> Registers;
  for (unsigned I = 0; I < Words; ++I)
    Registers.push_back(First + I);
  auto Parameters = Target.reference.parameters;
  Parameters.insert(Parameters.begin(), Name);
  auto Call = method(
      {Name, "call", Parameters, Bound}, {"public", "static"}, First + Words,
      {op(0, Range ? "invoke-direct/range" : "invoke-direct", Registers,
          Target.reference),
       op(3, "move-result-object", {0}), op(4, "return-object", {0})});
  Core.methods = {Target, Call};
  auto Interface = klass(Bound, SDK);
  Interface.access = {"public", "abstract", "interface"};
  auto Calendar = klass("Ljava/util/Calendar;", SDK);
  auto List = klass("Ljava/util/List;", SDK);
  List.access = {"public", "abstract", "interface"};
  List.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Core, Interface, Calendar, List, Root};
}
} // namespace

TEST(MobileDalvikPrivateInterfaceContext,
     RetainSameClassPrivateSeedPositionReifiableContextScopesAndOriginalWords) {
  for (unsigned Context : {0u, 1u, 2u, 3u, 4u})
    for (bool SDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool SeedFirst : {false, true})
            for (bool Frame : {false, true})
              for (bool Final : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{Context, SDK, Range, Alpha, SeedFirst, Frame,
                                 Final, Route}));
                  auto Input = fixture(Context, SDK, Range, Alpha, SeedFirst,
                                       Frame, Final);
                  const auto Original = Input;
                  const auto Selected = Input[0].methods[0].reference;
                  if (Route == 2) {
                    std::reverse(Input.begin(), Input.end());
                    for (auto &C : Input)
                      std::reverse(C.methods.begin(), C.methods.end());
                  }
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
                  const auto Report = recoverJava(Classes, B);
                  EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(
                      Report.getArray("generic_signature_projections"));
                  for (const auto &C : Original) {
                    const auto &After = Classes.at(C.name);
                    EXPECT_EQ(After.access, C.access);
                    EXPECT_EQ(After.generic_signature, C.generic_signature);
                    for (const auto &M : C.methods) {
                      const auto It = std::find_if(
                          After.methods.begin(), After.methods.end(),
                          [&](const Method &N) {
                            return N.reference == M.reference;
                          });
                      ASSERT_NE(It, After.methods.end());
                      EXPECT_EQ(It->access, M.access);
                      EXPECT_EQ(It->generic_signature, M.generic_signature);
                    }
                  }
                }
}

TEST(
    MobileDalvikPrivateInterfaceContext,
    RejectForeignPrivateCallersDependentContextsChangedBoundsAndMissingSeedRelations) {
  for (bool Linked : {false, true})
    for (unsigned Mutation = 0; Mutation < 25; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Linked, Mutation}));
      auto Input = fixture(0, true, true, false, false, false, false);
      auto &Core = Input[0];
      auto &Target = Core.methods[0];
      auto &Signature = *Target.generic_signature;
      auto &Call = Core.methods[1];
      if (Mutation == 0)
        Signature = "<T::Ljava/lang/Appendable;>(Ljava/lang/Object;TT;)TT;";
      else if (Mutation == 1)
        Signature = "<T::Ljava/lang/Appendable;>(Ljava/util/Calendar;Ljava/"
                    "lang/Appendable;)TT;";
      else if (Mutation == 2)
        Signature = "<T::Ljava/lang/Appendable;>(Ljava/util/Calendar;TT;)Ljava/"
                    "lang/Appendable;";
      else if (Mutation == 3)
        Signature = "<T::Ljava/lang/Appendable;U:Ljava/lang/Object;>(Ljava/"
                    "util/Calendar;TT;)TT;";
      else if (Mutation == 4)
        Signature = "<T::Ljava/lang/CharSequence;>(Ljava/util/Calendar;TT;)TT;";
      else if (Mutation == 5)
        Signature =
            "<T::Ljava/lang/Appendable;>(Ljava/util/Calendar;TMissing;)TT;";
      else if (Mutation == 6) {
        Input[2].generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
        Signature =
            "<T::Ljava/lang/Appendable;>(Ljava/util/Calendar<TT;>;TT;)TT;";
      } else if (Mutation == 7)
        Input[1].access.erase("public");
      else if (Mutation == 8)
        Input[1].access.erase("interface");
      else if (Mutation == 9)
        Input[1].generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 10)
        Target.access.insert("static");
      else if (Mutation == 11)
        Target.access.insert("native");
      else if (Mutation == 12)
        Target.access.insert("abstract");
      else if (Mutation == 13)
        Target.access.insert("bridge");
      else if (Mutation == 14)
        Target.access.insert("synthetic");
      else if (Mutation == 15)
        Target.access.insert("varargs");
      else if (Mutation == 16)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 17)
        Core.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 18) {
        auto Foreign = klass("Lforeign/Caller;");
        Foreign.methods = {Call};
        Foreign.methods[0].reference.owner = Foreign.name;
        Input.push_back(Foreign);
      } else if (Mutation == 19) {
        auto Child = klass("Lfixture/Child;");
        Child.superclass = Core.name;
        std::get<MethodRef>(Call.instructions[0].reference).owner = Child.name;
        Call.reference.parameters[0] = Child.name;
        Input.push_back(Child);
      } else if (Mutation == 20)
        Call.instructions[0].opcode = "invoke-virtual";
      else if (Mutation == 21)
        Call.instructions[0].registers = {1, 3, 2};
      else if (Mutation == 22) {
        Target.instructions.clear();
        Target.registers = Target.code_end = 0;
      } else if (Mutation == 23) {
        auto Foreign = klass("Lforeign/Caller;");
        Foreign.methods = {Call};
        Foreign.methods[0].reference.owner = Foreign.name;
        Core.external = true;
        for (auto &M : Core.methods) {
          M.instructions.clear();
          M.registers = M.code_end = 0;
        }
        Input.push_back(Foreign);
      } else {
        Signature = "<T::Ljava/lang/Appendable;>(TT;TT;)TT;";
        Target.reference.parameters[0] = "Ljava/lang/Appendable;";
        std::get<MethodRef>(Call.instructions[0].reference) = Target.reference;
        Call.reference.parameters[1] = "Ljava/lang/Appendable;";
      }
      Budget B;
      EXPECT_THROW(Linked ? recoverJava(linkClasses(Input, B), B)
                          : recoverJava(
                                [&] {
                                  ClassMap C;
                                  for (const auto &K : Input)
                                    C.emplace(K.name, K);
                                  return C;
                                }(),
                                B),
                   std::runtime_error);
    }
}
