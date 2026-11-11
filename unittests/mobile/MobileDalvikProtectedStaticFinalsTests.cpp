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
std::vector<Class> fixture(bool SDK, bool Range, unsigned Count,
                           unsigned Kind) {
  const std::string Core = "Lfixture/Core;", Provider = "Lfixture/Provider;",
                    Object = "Ljava/lang/Object;",
                    Type = Kind == 0   ? Object
                           : Kind == 1 ? "I"
                           : Kind == 2 ? "J"
                           : Kind == 3 ? "D"
                                       : "[Ljava/lang/String;";
  const bool Reference = Type.starts_with('L') || Type.starts_with('[');
  const bool Wide = Type == "J" || Type == "D";
  auto Owner = klass(Core);
  std::vector<Instruction> Code;
  MethodRef Read{Provider, "read", {"I"}, Type};
  for (unsigned N = 0; N < Count; ++N) {
    Field F;
    F.reference = {Core, "field" + std::to_string(N), Type};
    F.access = {"public", "static", "final"};
    Owner.fields.push_back(F);
    auto Index = op(Code.size(), "const/4", {0});
    Index.literal = int64_t(N + 1);
    Code.push_back(Index);
    Code.push_back(op(Code.size(),
                      Range ? "invoke-static/range" : "invoke-static", {0},
                      Read));
    Code.push_back(op(Code.size(),
                      Reference ? "move-result-object"
                      : Wide    ? "move-result-wide"
                                : "move-result",
                      {1}));
    Code.push_back(op(Code.size(),
                      Reference ? "sput-object"
                      : Wide    ? "sput-wide"
                                : "sput",
                      {1}, F.reference));
  }
  const auto Return = Code.size();
  Code.push_back(op(Code.size(), "return-void"));
  const auto Handler = Code.size();
  Code.push_back(op(Code.size(), "move-exception", {3}));
  Code.push_back(op(Code.size(), "new-instance", {0},
                    std::string("Ljava/lang/IllegalStateException;")));
  auto Message = op(Code.size(), "const-string", {1});
  Message.literal = std::string("owned static failure");
  Code.push_back(Message);
  Code.push_back(op(Code.size(), "invoke-direct", {0, 1, 3},
                    MethodRef{"Ljava/lang/IllegalStateException;",
                              "<init>",
                              {"Ljava/lang/String;", "Ljava/lang/Throwable;"},
                              "V"}));
  Code.push_back(op(Code.size(), "throw", {0}));
  auto Clinit =
      method({Core, "<clinit>", {}, "V"}, {"static", "constructor"}, 4, Code);
  Clinit.tries = {{Range ? 0u : 1u,
                   uint32_t(Return),
                   {{std::string("Ljava/lang/NoSuchMethodException;"),
                     uint32_t(Handler)}}}};
  Owner.methods = {Clinit};
  auto Zero = op(0, Wide ? "const-wide/16" : "const/4", {0});
  Zero.literal = int64_t(0);
  auto Fetch =
      method(Read, {"public", "static"}, SDK ? 0 : 3,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1,
                                                     Reference ? "return-object"
                                                     : Wide    ? "return-wide"
                                                               : "return",
                                                     {0})});
  Fetch.declared_throws =
      std::vector<std::string>{"Ljava/lang/NoSuchMethodException;"};
  auto Source = klass(Provider, SDK);
  Source.methods = {Fetch};
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto String = klass("Ljava/lang/String;", true);
  String.access.insert("final");
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = Throwable.name;
  auto Reflective = klass("Ljava/lang/ReflectiveOperationException;", true);
  Reflective.superclass = Exception.name;
  auto Missing = klass("Ljava/lang/NoSuchMethodException;", true);
  Missing.superclass = Reflective.name;
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = Exception.name;
  auto Illegal = klass("Ljava/lang/IllegalStateException;", true);
  Illegal.superclass = Runtime.name;
  Illegal.methods.push_back(
      method({Illegal.name,
              "<init>",
              {"Ljava/lang/String;", "Ljava/lang/Throwable;"},
              "V"},
             {"public", "constructor"}, 0, {}));
  return {Owner,     Source,     Root,    String,  Throwable,
          Exception, Reflective, Missing, Runtime, Illegal};
}
} // namespace

TEST(MobileDalvikProtectedStaticFinals,
     RetainTypedThrowTailsEveryProtectedWriteAndOriginalStaticFieldMetadata) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (unsigned Count : {1u, 3u, 6u})
        for (unsigned Kind : {0u, 1u, 2u, 3u, 4u})
          for (unsigned Route : {0u, 1u, 2u}) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{SDK, Range, Count, Kind, Route}));
            auto Input = fixture(SDK, Range, Count, Kind);
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
            EXPECT_NO_THROW({
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              EXPECT_FALSE(
                  Report.getBoolean("requires_java_rebuild_postprocessing")
                      .value_or(false));
            });
            for (const auto &C : Original) {
              const auto &After = Classes.at(C.name);
              EXPECT_EQ(After.access, C.access);
              EXPECT_EQ(After.fields.size(), C.fields.size());
              for (size_t I = 0; I < C.fields.size(); ++I) {
                EXPECT_EQ(After.fields[I].reference, C.fields[I].reference);
                EXPECT_EQ(After.fields[I].access, C.fields[I].access);
              }
              for (size_t I = 0; I < C.methods.size(); ++I) {
                EXPECT_EQ(After.methods[I].reference, C.methods[I].reference);
                EXPECT_EQ(After.methods[I].access, C.methods[I].access);
              }
            }
          }
}

TEST(
    MobileDalvikProtectedStaticFinals,
    RejectRepeatedStoresReturningOrReenteringHandlersAndMalformedStaticFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, false, 3, 0);
      auto &M = Input[0].methods[0];
      auto &Code = M.instructions;
      if (Mutation == 0)
        Code[7].reference = Input[0].fields[0].reference;
      else if (Mutation == 1) {
        Code.back().opcode = "return-void";
        Code.back().registers.clear();
      } else if (Mutation == 2) {
        Code.back().opcode = "goto";
        Code.back().registers.clear();
        Code.back().target = 3;
      } else if (Mutation == 3)
        std::get<FieldRef>(Code[3].reference).owner = "Lfixture/Provider;";
      else if (Mutation == 4)
        Code[3].registers.push_back(2);
      else if (Mutation == 5)
        ++M.tries[0].handlers[0].target;
      else if (Mutation == 6)
        Code[Code.size() - 2].registers.pop_back();
      else if (Mutation == 7)
        M.tries[0].end = M.code_end + 1;
      else if (Mutation == 8) {
        Code[3].opcode = "iput-object";
        Code[3].registers.push_back(0);
      } else {
        Code[12].opcode = "return-object";
        Code[12].registers = {1};
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
