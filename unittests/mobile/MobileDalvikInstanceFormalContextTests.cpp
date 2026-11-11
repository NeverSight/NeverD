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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, unsigned Throws,
                           bool SeedFirst) {
  const std::string Object = "Ljava/lang/Object;",
                    Type = "Ljava/lang/reflect/Type;",
                    E = Alpha ? "Element" : "E";
  auto Provider = klass("Lfixture/Provider;", SDK);
  MethodRef Selected{Provider.name, "convert",
                     SeedFirst ? std::vector<std::string>{Object, Type, Object}
                               : std::vector<std::string>{Type, Object, Object},
                     Object};
  auto Target = method(Selected, {"public"}, SDK ? 0 : 5,
                       SDK ? std::vector<Instruction>{}
                           : std::vector<Instruction>{op(
                                 0, "return-object", {SeedFirst ? 2u : 4u})});
  Target.generic_signature =
      "<" + E + ":Ljava/lang/Object;>(" +
      (SeedFirst ? "T" + E + ";Ljava/lang/reflect/Type;Ljava/lang/Object;"
                 : "Ljava/lang/reflect/Type;Ljava/lang/Object;T" + E + ";") +
      ")T" + E + ";";
  if (Throws)
    Target.declared_throws = Throws == 2
                                 ? std::vector<std::string>{"Lfixture/Failure;"}
                                 : std::vector<std::string>{};
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  auto Parameters = Selected.parameters;
  Parameters.insert(Parameters.begin(), Provider.name);
  Client.methods.push_back(
      method({Client.name, "call", Parameters, Object}, {"public", "static"}, 4,
             {op(0, Range ? "invoke-virtual/range" : "invoke-virtual",
                 {0, 1, 2, 3}, Selected),
              op(1, "move-result-object", {0}), op(2, "return-object", {0})}));
  auto TypeClass = klass(Type, true);
  TypeClass.access = {"public", "interface", "abstract"};
  auto Failure = klass("Lfixture/Failure;", true);
  Failure.superclass = "Ljava/lang/RuntimeException;";
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = "Ljava/lang/Exception;";
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = "Ljava/lang/Throwable;";
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Client,    Provider, TypeClass,
          Failure,   Runtime,  Exception,
          Throwable, Root,     klass("Ljava/lang/String;", true)};
}
} // namespace

TEST(MobileDalvikInstanceFormalContext,
     RetainScopedSeedsIndependentContextsExceptionsAndVirtualCalls) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (unsigned Throws : {0u, 1u, 2u})
          for (bool SeedFirst : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, Throws, SeedFirst, Route}));
              auto Input = fixture(SDK, Range, Alpha, Throws, SeedFirst);
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
                  EXPECT_EQ(After.methods[I].declared_throws,
                            C.methods[I].declared_throws);
                }
              }
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikInstanceFormalContext,
     RejectForeignFormalsChangedContextsThrowsAndDeclarationRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, 2, false);
      auto &Target = Input[1].methods[0];
      auto &Caller = Input[0].methods[0];
      if (Mutation == 0)
        Target.generic_signature = "<E:Ljava/lang/Object;>(Ljava/lang/reflect/"
                                   "Type;Ljava/lang/Object;TMissing;)TE;";
      else if (Mutation == 1)
        Target.generic_signature = "<E:Ljava/lang/Object;>(Ljava/lang/reflect/"
                                   "Type;Ljava/lang/Object;TE;)TMissing;";
      else if (Mutation == 2)
        Target.generic_signature = "<E:Ljava/lang/String;>(Ljava/lang/reflect/"
                                   "Type;Ljava/lang/Object;TE;)TE;";
      else if (Mutation == 3)
        Target.generic_signature =
            "<E:Ljava/lang/Object;U:Ljava/lang/Object;>(Ljava/lang/reflect/"
            "Type;Ljava/lang/Object;TE;)TE;";
      else if (Mutation == 4)
        Target.generic_signature =
            "<E:Ljava/lang/Object;>(TE;Ljava/lang/Object;TE;)TE;";
      else if (Mutation == 5)
        Target.generic_signature = "<E:Ljava/lang/Object;>(Ljava/lang/reflect/"
                                   "Type;Ljava/lang/Object;TE;)[TE;";
      else if (Mutation == 6)
        Target.access.insert("varargs");
      else if (Mutation == 7)
        Target.access.insert("static");
      else if (Mutation == 8)
        Target.access.insert("native");
      else if (Mutation == 9)
        Target.access.insert("abstract");
      else if (Mutation == 10)
        Target.access.insert("synthetic");
      else if (Mutation == 11) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 12)
        Input[1].access.insert("interface");
      else if (Mutation == 13)
        Input[1].generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 14)
        Target.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 15)
        Target.declared_throws = {"Ljava/lang/String;"};
      else if (Mutation == 16)
        Target.declared_throws = {"Lfixture/MissingFailure;"};
      else if (Mutation == 17)
        Caller.instructions[0].registers.pop_back();
      else if (Mutation == 18)
        Caller.instructions[0].opcode = "invoke-static";
      else
        Target.generic_signature =
            "<E:Ljava/lang/Object;>(Ljava/lang/reflect/Type;Ljava/lang/"
            "Object;TE;)Ljava/lang/Object;";
      Budget B;
      if (Mutation == 19) {
        const auto Before = Target;
        ASSERT_NO_THROW(([&] {
          const auto Classes = linkClasses(Input, B);
          const auto Plans = validateGenericSignatures(Classes, B);
          EXPECT_TRUE(
              Plans.erased_generic_invocations.contains(Before.reference));
          EXPECT_FALSE(Plans.erased_unused_formal_invocations.contains(
              Before.reference));
          if (Before.declared_throws && !Before.declared_throws->empty())
            EXPECT_TRUE(validatedClosedDeclaredThrows(Before, Plans));
          const auto Report = recoverJava(Classes, B);
          EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
          EXPECT_EQ(Report.getInteger("projected_method_count").value_or(0), 0);
          EXPECT_FALSE(Report.getArray("generic_signature_projections"));
          const auto &After = Classes.at(Before.reference.owner).methods[0];
          EXPECT_EQ(After.reference, Before.reference);
          EXPECT_EQ(After.generic_signature, Before.generic_signature);
          EXPECT_EQ(After.declared_throws, Before.declared_throws);
          EXPECT_EQ(After.access, Before.access);
        })());
      } else {
        EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
      }
    }
}
