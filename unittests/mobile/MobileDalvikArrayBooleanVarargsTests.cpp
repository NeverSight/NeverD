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
                           bool Varargs, bool Ignored) {
  const std::string Object = "Ljava/lang/Object;", T = Alpha ? "Element" : "T";
  auto Provider = klass("Lfixture/Provider;", SDK);
  if (OwnerFormal)
    Provider.generic_signature =
        "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto One = op(0, "const/4", {0});
  One.literal = int64_t(1);
  auto Target =
      method({Provider.name, "test", {"[Ljava/lang/Object;"}, "Z"},
             {"public", "static"}, SDK ? 0 : 2,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{One, op(1, "return", {0})});
  if (Varargs)
    Target.access.insert("varargs");
  Target.generic_signature = "<" + T + ":Ljava/lang/Object;>([T" + T + ";)Z";
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  std::vector<Instruction> Code{
      op(0, Range ? "invoke-static/range" : "invoke-static", {1},
         Target.reference)};
  if (Ignored)
    Code.push_back(op(3, "return-void"));
  else {
    Code.push_back(op(3, "move-result", {0}));
    Code.push_back(op(4, "return", {0}));
  }
  Client.methods.push_back(method(
      {Client.name, "call", {"[Ljava/lang/Object;"}, Ignored ? "V" : "Z"},
      {"public", "static"}, 2, Code));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Client, Provider, Root};
}
} // namespace

TEST(MobileDalvikArrayBooleanVarargs,
     RetainMethodOwnedComponentsOriginalArraysBooleanResultsAndRegisterFrames) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool OwnerFormal : {false, true})
          for (bool Varargs : {false, true})
            for (bool Ignored : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Range, Alpha, OwnerFormal, Varargs, Ignored, Route}));
                auto Input =
                    fixture(SDK, Range, Alpha, OwnerFormal, Varargs, Ignored);
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
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
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
}

TEST(
    MobileDalvikArrayBooleanVarargs,
    RejectUnboundComponentsChangedErasureForeignScopesInvalidRolesAndMissingWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, true, true, false);
      auto &Target = Input[1].methods[0];
      auto &Signature = *Target.generic_signature;
      auto Replace = [&](const std::string &From, const std::string &To) {
        const auto At = Signature.find(From);
        ASSERT_NE(At, std::string::npos);
        Signature.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        Replace("[TT;", "[TMissing;");
      else if (Mutation == 1)
        Replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
      else if (Mutation == 2)
        Replace("Object;>(", "Object;U:Ljava/lang/Object;>(");
      else if (Mutation == 3)
        Replace("[TT;", "[[TT;");
      else if (Mutation == 4)
        Signature.back() = 'I';
      else if (Mutation == 5)
        Replace("[TT;", "[I");
      else if (Mutation == 6) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 7)
        Target.access.erase("static");
      else if (Mutation == 8)
        Target.access.insert("native");
      else if (Mutation == 9)
        Target.access.insert("abstract");
      else if (Mutation == 10)
        Target.access.insert("synthetic");
      else if (Mutation == 11)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 12)
        Input[0].methods[0].instructions[0].registers.clear();
      else if (Mutation == 13)
        Replace("<T:Ljava/lang/Object;>", "<U:Ljava/lang/Object;>");
      else if (Mutation == 14)
        Input[0].methods[0].instructions[0].opcode = "invoke-virtual";
      else
        Input[0].methods[0].instructions[0].registers = {0, 1};
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
