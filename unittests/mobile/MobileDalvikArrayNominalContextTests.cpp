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
                           bool Static, unsigned ResultKind, bool Aux) {
  const std::string Object = "Ljava/lang/Object;",
                    Context = "Lfixture/Context;", T = Alpha ? "Value" : "T",
                    Return = ResultKind == 0   ? "Ljava/lang/String;"
                             : ResultKind == 1 ? Context
                             : ResultKind == 2 ? Object
                                               : "Lfixture/Other;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{"[Ljava/lang/Object;", Context};
  if (Aux)
    Params.insert(Params.end(), {"Ljava/lang/String;", "Ljava/lang/String;"});
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target = method({Provider.name, "call", Params, Return},
                       Static ? Access{"public", "static"} : Access{"public"},
                       SDK ? 0 : Params.size() + (Static ? 1 : 2), {});
  Target.generic_signature =
      "<" + T + ":Ljava/lang/Object;>([T" + T + ";" + Context +
      (Aux ? "Ljava/lang/String;Ljava/lang/String;" : "") + ")" + Return;
  if (!SDK) {
    Target.instructions = {Zero, op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  Provider.methods.push_back(Target);
  auto Core = klass("Lfixture/Core;");
  std::vector<std::string> Inputs{Provider.name};
  Inputs.insert(Inputs.end(), Params.begin(), Params.end());
  std::vector<unsigned> Words;
  if (!Static)
    Words.push_back(1);
  for (unsigned I = 0; I < Params.size(); ++I)
    Words.push_back(I + 2);
  std::vector<Instruction> Code{
      op(0,
         std::string(Static ? "invoke-static" : "invoke-virtual") +
             (Range ? "/range" : ""),
         Words, Target.reference)};
  Code.push_back(op(3, "move-result-object", {0}));
  Code.push_back(op(4, "return-object", {0}));
  Core.methods.push_back(method({Core.name, "call", Inputs, Return},
                                {"public", "static"}, Inputs.size() + 1, Code));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Ctx = klass(Context, true);
  if (Interface)
    Ctx.access = {"public", "interface", "abstract"};
  return {Provider,
          Core,
          Ctx,
          Root,
          klass("Ljava/lang/String;", true),
          klass("Ljava/lang/Number;", true),
          klass("Lfixture/Other;", true)};
}
ClassMap input(std::vector<Class> Classes, unsigned Route, Budget &B) {
  if (Route == 2)
    std::reverse(Classes.begin(), Classes.end());
  if (Route)
    return linkClasses(Classes, B);
  ClassMap Out;
  for (auto &C : Classes)
    Out.emplace(C.name, std::move(C));
  return Out;
}
} // namespace

TEST(MobileDalvikArrayNominalContext,
     KeepOwnArraySeedClosedContextsOpaqueResultAndOriginalArrayView) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Interface : {false, true})
          for (bool Static : {false, true})
            for (unsigned ResultKind : {0u, 1u, 2u, 3u})
              for (bool Aux : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, Range, Alpha, Interface, Static,
                                 ResultKind, Aux, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = input(fixture(SDK, Range, Alpha, Interface,
                                                 Static, ResultKind, Aux),
                                         Route, B);
                    const auto Target =
                        Classes.at("Lfixture/Provider;").methods[0];
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                        Target.reference));
                    const auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    EXPECT_EQ(Classes.at(Target.reference.owner)
                                  .methods[0]
                                  .generic_signature,
                              Target.generic_signature);
                  })());
                }
}

TEST(MobileDalvikArrayNominalContext,
     RejectForeignElementsWrongErasuresBoundsContextsRolesAndWords) {
  for (bool SDK : {false, true})
    for (bool Static : {false, true})
      for (unsigned Mutation = 0; Mutation < 14; ++Mutation)
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{SDK, Static, Mutation, Route}));
          auto Classes = fixture(SDK, false, false, true, Static, 0, false);
          auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
          switch (Mutation) {
          case 0:
            Target.generic_signature =
                "<T:Ljava/lang/Object;>([TMissing;Lfixture/Context;)Ljava/lang/"
                "String;";
            break;
          case 1:
            Target.generic_signature =
                "<T:Ljava/lang/Object;>([Ljava/lang/String;Lfixture/"
                "Context;)Ljava/lang/String;";
            break;
          case 2:
            Target.generic_signature = "<T:Ljava/lang/Number;>([TT;Lfixture/"
                                       "Context;)Ljava/lang/String;";
            break;
          case 3:
            Target.generic_signature = "<T:Ljava/lang/Object;>([TT;Lfixture/"
                                       "Missing;)Ljava/lang/String;";
            break;
          case 4:
            Target.generic_signature = "<T:Ljava/lang/Object;>([TT;Lfixture/"
                                       "Context;)Ljava/lang/Object;";
            break;
          case 5:
            Target.access.erase("public");
            Target.access.insert("private");
            break;
          case 6:
            Target.access.insert("native");
            break;
          case 7:
            Target.access.insert("abstract");
            break;
          case 8:
            Target.access.insert("bridge");
            break;
          case 9:
            Target.access.insert("varargs");
            break;
          case 10:
            Caller.instructions[0].registers.pop_back();
            break;
          case 11:
            Caller.instructions[0].registers.back() = Caller.registers;
            break;
          case 12:
            Caller.instructions[1].opcode = "move-result";
            break;
          case 13:
            Target.generic_signature = "<T:Ljava/lang/Object;>([[TT;Lfixture/"
                                       "Context;)Ljava/lang/String;";
            break;
          }
          EXPECT_THROW(([&] {
                         Budget B;
                         recoverJava(input(Classes, Route, B), B);
                       }()),
                       std::runtime_error);
        }
}

namespace {
std::vector<Class> arrayNominalVarargsClasses(bool SDK, bool Range, bool Alpha,
                                              unsigned ContextShape,
                                              unsigned ResultKind,
                                              bool Varargs = true) {
  auto Classes = fixture(SDK, Range, Alpha, true, true, ResultKind, false);
  auto &Provider = Classes[0];
  auto &Target = Provider.methods[0];
  const std::string Formal = Alpha ? "Cell" : "Item";
  std::vector<std::string> Prefix;
  if (ContextShape == 0)
    Prefix = {"Ljava/lang/Object;", "I"};
  else if (ContextShape == 1)
    Prefix = {"Lfixture/Context;", "J"};
  else if (ContextShape == 2) {
    Prefix = {"Ljava/lang/String;", "Z"};
    Provider.superclass = "Lfixture/Other;";
  }
  std::string Signature = "<" + Formal + ":Ljava/lang/Object;>(";
  unsigned Words = 1;
  for (const auto &P : Prefix) {
    Signature += P;
    Words += P == "J" ? 2 : 1;
  }
  Signature += "[T" + Formal + ";)" + Target.reference.returns;
  Prefix.push_back("[Ljava/lang/Object;");
  Target.reference.parameters = Prefix;
  Target.generic_signature = Signature;
  Target.access = {"public", "static"};
  if (Varargs)
    Target.access.insert("varargs");
  Target.registers = SDK ? 0 : Words + 1;
  std::vector<unsigned> Arguments;
  for (unsigned I = 1; I <= Words; ++I)
    Arguments.push_back(I);
  auto &Caller = Classes[1].methods[0];
  Caller.reference.parameters = Prefix;
  Caller.registers = Words + 1;
  Caller.instructions[0] =
      op(0, Range ? "invoke-static/range" : "invoke-static", Arguments,
         Target.reference);
  return Classes;
}
} // namespace

TEST(MobileDalvikArrayNominalContext,
     ExplicitVarargsTailRetainsOwnFormalClosedContextsAndOpaqueResult) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (unsigned ContextShape : {0u, 1u, 2u, 3u})
          for (unsigned ResultKind : {0u, 1u, 2u, 3u})
            for (bool Varargs : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Range, Alpha, ContextShape, ResultKind,
                               Varargs, Route}));
                ASSERT_NO_THROW(([&] {
                  Budget B;
                  auto Classes = input(
                      arrayNominalVarargsClasses(
                          SDK, Range, Alpha, ContextShape, ResultKind, Varargs),
                      Route, B);
                  const auto Target =
                      Classes.at("Lfixture/Provider;").methods[0];
                  const auto Plans = validateGenericSignatures(Classes, B);
                  EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                      Target.reference));
                  const auto R = recoverJava(Classes, B);
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                  EXPECT_EQ(Classes.at(Target.reference.owner)
                                .methods[0]
                                .generic_signature,
                            Target.generic_signature);
                })());
              }
}

TEST(MobileDalvikArrayNominalContext,
     ExplicitVarargsTailRejectsLostBindingsDescriptorsRolesAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = arrayNominalVarargsClasses(SDK, false, false, 0, 2);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Signature = *Target.generic_signature;
          const auto Pos = Signature.find(From);
          ASSERT_NE(Pos, std::string::npos);
          Signature.replace(Pos, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("[TItem;", "[TMissing;");
          break;
        case 1:
          replace("[TItem;", "[[TItem;");
          break;
        case 2:
          replace("[TItem;", "[Ljava/lang/String;");
          break;
        case 3:
          replace("Item:Ljava/lang/Object;", "Item:Ljava/lang/Number;");
          break;
        case 4:
          replace(";I[", ";J[");
          break;
        case 5:
          replace(">(Ljava/lang/Object;", ">(Lfixture/Missing;");
          break;
        case 6:
          replace(")Ljava/lang/Object;", ")Ljava/lang/String;");
          break;
        case 7:
          replace("<Item:", "<Unused:Ljava/lang/Object;Item:");
          break;
        case 8:
          Target.access.insert("native");
          break;
        case 9:
          Target.access.insert("abstract");
          break;
        case 10:
          Target.access.insert("bridge");
          break;
        case 11:
          Target.access.insert("synthetic");
          break;
        case 12:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 13:
          Target.access.erase("static");
          break;
        case 14:
          Classes[0].access.insert("interface");
          Classes[0].access.insert("abstract");
          break;
        case 15:
          Classes[0].generic_signature =
              "<Outer:Ljava/lang/Object;>Ljava/lang/Object;";
          replace("[TItem;", "[TOuter;");
          break;
        case 16:
          Caller.instructions[0].registers.pop_back();
          break;
        case 17:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 18:
          Caller.instructions[1].opcode = "move-result";
          break;
        case 19:
          Target.reference.parameters = {"[Ljava/lang/Object;", "I",
                                         "Ljava/lang/Object;"};
          Target.generic_signature = "<Item:Ljava/lang/Object;>([TItem;ILjava/"
                                     "lang/Object;)Ljava/lang/Object;";
          Caller.reference.parameters = Target.reference.parameters;
          Caller.instructions[0].reference = Target.reference;
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

TEST(MobileDalvikArrayNominalContext,
     PlainVarargsTailRetainsPreviouslySupportedUnusedMethodFormal) {
  for (bool SDK : {false, true})
    for (bool Varargs : {false, true})
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Varargs, Route}));
        ASSERT_NO_THROW(([&] {
          auto Classes =
              arrayNominalVarargsClasses(SDK, false, false, 0, 2, Varargs);
          auto &Signature = *Classes[0].methods[0].generic_signature;
          const auto Pos = Signature.find("[TItem;");
          ASSERT_NE(Pos, std::string::npos);
          Signature.replace(Pos, 7, "[Ljava/lang/Object;");
          const auto Reference = Classes[0].methods[0].reference;
          Budget B;
          auto Linked = input(Classes, Route, B);
          const auto Plans = validateGenericSignatures(Linked, B);
          EXPECT_TRUE(Plans.erased_generic_invocations.contains(Reference));
          const auto R = recoverJava(Linked, B);
          EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
          EXPECT_FALSE(R.getArray("generic_signature_projections"));
        })());
      }
}
