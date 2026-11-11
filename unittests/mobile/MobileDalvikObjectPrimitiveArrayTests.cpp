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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, char Kind) {
  const std::string Object = "Ljava/lang/Object;",
                    Array = "[" + std::string(1, Kind),
                    E = Alpha ? "Element" : "E";
  auto Provider = klass("Lfixture/Provider;", SDK);
  MethodRef Selected{Provider.name, "pack", {Object}, Array};
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method(Selected, {"public", "static"}, SDK ? 0 : 2,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  Target.generic_signature =
      "<" + E + ":Ljava/lang/Object;>(T" + E + ";)" + Array;
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(method(
      {Client.name, "call", {Object}, Array}, {"public", "static"}, 1,
      {op(0, Range ? "invoke-static/range" : "invoke-static", {0}, Selected),
       op(1, "move-result-object", {0}), op(2, "return-object", {0})}));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Client, Provider, Root, klass("Ljava/lang/String;", true)};
}
} // namespace

TEST(MobileDalvikObjectPrimitiveArray,
     RetainMethodOwnedObjectsAndEveryFixedPrimitiveArrayDescriptor) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (char Kind : std::string("ZBCSIJFD"))
          for (unsigned Route : {0u, 1u, 2u}) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{SDK, Range, Alpha, Kind, Route}));
            auto Input = fixture(SDK, Range, Alpha, Kind);
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
            const auto Report = recoverJava(Classes, B);
            EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
            EXPECT_FALSE(Report.getArray("generic_signature_projections"));
          }
}

TEST(MobileDalvikObjectPrimitiveArray,
     RejectUnprovedInputScopesResultDescriptorsAndDeclarationRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, 'B');
      auto &Target = Input[1].methods[0];
      auto &Caller = Input[0].methods[0];
      if (Mutation == 0)
        Target.generic_signature = "<E:Ljava/lang/Object;>(TMissing;)[B";
      else if (Mutation == 1)
        Target.generic_signature = "<E:Ljava/lang/String;>(TE;)[B";
      else if (Mutation == 2)
        Target.generic_signature =
            "<E:Ljava/lang/Object;U:Ljava/lang/Object;>(TE;)[B";
      else if (Mutation == 3)
        Target.generic_signature = "<E:Ljava/lang/Object;>(TE;)[TE;";
      else if (Mutation == 4)
        Target.generic_signature = "<E:Ljava/lang/Object;>(TE;)[[B";
      else if (Mutation == 5)
        Target.generic_signature = "<E:Ljava/lang/Object;>(TE;)[I";
      else if (Mutation == 6)
        Target.generic_signature =
            "<E:Ljava/lang/Object;>(Ljava/lang/Object;)[B";
      else if (Mutation == 7)
        Target.access.insert("varargs");
      else if (Mutation == 8)
        Target.access.insert("native");
      else if (Mutation == 9)
        Target.access.insert("abstract");
      else if (Mutation == 10)
        Target.access.insert("synthetic");
      else if (Mutation == 11)
        Target.access.erase("static");
      else if (Mutation == 12) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 13)
        Input[1].access.insert("interface");
      else if (Mutation == 14)
        Input[1].generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 15)
        Target.declared_throws = {"Ljava/lang/RuntimeException;"};
      else if (Mutation == 16)
        Caller.instructions[0].registers.clear();
      else if (Mutation == 17)
        Caller.instructions[0].opcode = "invoke-virtual";
      else if (Mutation == 18)
        Caller.instructions[1].opcode = "move-result";
      else
        Target.generic_signature = "<E:Ljava/lang/Object;>(TE;)V";
      Budget B;
      if (Mutation == 6) {
        // An unused method formal already needs no argument binding.
        const auto Original = Target;
        const auto Classes = linkClasses(Input, B);
        EXPECT_EQ(
            recoverJava(Classes, B).getInteger("unrecovered_method_count"), 0);
        const auto &After = Classes.at(Original.reference.owner).methods[0];
        EXPECT_EQ(After.reference, Original.reference);
        EXPECT_EQ(After.generic_signature, Original.generic_signature);
        EXPECT_EQ(After.access, Original.access);
        continue;
      }
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
