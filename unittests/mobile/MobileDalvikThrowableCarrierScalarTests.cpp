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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, unsigned Contexts,
                           const std::string &Result, bool Invariant,
                           const std::string &Context = "D") {
  const std::string Object = "Ljava/lang/Object;",
                    Throwable = "Ljava/lang/Throwable;",
                    Carrier = "Lfixture/Carrier;", T = Alpha ? "Failure" : "X";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params(Contexts, Context);
  Params.push_back(Carrier);
  unsigned Words = Contexts * (Context == "D" || Context == "J" ? 2 : 1) + 1;
  auto Target = method({Provider.name, "evaluate", Params, Result},
                       {"public", "static"}, SDK ? 0 : Words + 2, {});
  std::string Arguments;
  for (unsigned I = 0; I < Contexts; ++I)
    Arguments += Context;
  Target.generic_signature = "<" + T + ":" + Throwable + ">(" + Arguments +
                             "Lfixture/Carrier<" + (Invariant ? "" : "+") +
                             "T" + T + ";>;)" + Result + "^T" + T + ";";
  Target.declared_throws = std::vector<std::string>{Throwable};
  bool Wide = Result == "D" || Result == "J";
  const std::string Load = Wide            ? "sget-wide"
                           : Result == "Z" ? "sget-boolean"
                           : Result == "B" ? "sget-byte"
                           : Result == "C" ? "sget-char"
                           : Result == "S" ? "sget-short"
                                           : "sget";
  FieldRef Value{"Lfixture/Trace;", "result", Result};
  if (!SDK) {
    Target.instructions =
        Result == "V" ? std::vector<Instruction>{op(0, "return-void")}
                      : std::vector<Instruction>{
                            op(0, Load, {0}, Value),
                            op(1, Wide ? "return-wide" : "return", {0})};
    Target.code_end = Target.instructions.back().pc + 1;
  }
  Provider.methods.push_back(Target);
  auto Core = klass("Lfixture/Core;");
  unsigned Start = Range ? 32 : 2;
  std::vector<unsigned> Registers;
  for (unsigned I = 0; I < Words; ++I)
    Registers.push_back(Start + I);
  std::vector<Instruction> Code{
      op(0, Range ? "invoke-static/range" : "invoke-static", Registers,
         Target.reference)};
  if (Result == "V")
    Code.push_back(op(1, "return-void"));
  else {
    Code.push_back(op(1, Wide ? "move-result-wide" : "move-result", {0}));
    Code.push_back(op(2, Wide ? "return-wide" : "return", {0}));
  }
  Core.methods.push_back(method({Core.name, "call", Params, Result},
                                {"public", "static"}, Start + Words, Code));
  auto Input = klass(Carrier, SDK);
  Input.access = {"public", "interface", "abstract"};
  Input.generic_signature = "<Element:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Get =
      method({Carrier, "get", {}, Object}, {"public", "abstract"}, 0, {});
  Get.generic_signature = "()TElement;";
  Input.methods.push_back(Get);
  auto Trace = klass(Value.owner, true);
  if (Result != "V")
    Trace.fields.push_back({Value, {"public", "static"}, {}});
  auto Bound = klass(Throwable, true), Root = klass(Object, true);
  Root.superclass.reset();
  auto Fixed = klass("Lfixture/Context;", true);
  Fixed.access = {"public", "interface", "abstract"};
  auto String = klass("Ljava/lang/String;", true);
  return {Provider, Core, Input, Trace, Bound, Root, Fixed, String};
}
std::vector<Class> arrayFixture(bool SDK, bool Range, bool Alpha,
                                bool Invariant, bool ExceptionFirst) {
  auto Classes =
      fixture(SDK, Range, Alpha, 1, "V", Invariant, "Ljava/lang/Object;");
  const std::string Array = "[Ljava/lang/Object;", T = Alpha ? "Element" : "T",
                    X = Alpha ? "Failure" : "X";
  auto &Target = Classes[0].methods[0];
  Target.reference.parameters[0] = Array;
  Target.reference.returns = Array;
  const std::string A = T + ":Ljava/lang/Object;",
                    E = X + ":Ljava/lang/Throwable;";
  Target.generic_signature = "<" + (ExceptionFirst ? E + A : A + E) + ">([T" +
                             T + ";Lfixture/Carrier<" + (Invariant ? "" : "+") +
                             "T" + X + ";>;)[T" + T + ";^T" + X + ";";
  FieldRef Value{Classes[3].name, "result", Array};
  Classes[3].fields.push_back({Value, {"public", "static"}, {}});
  if (!SDK) {
    Target.instructions = {op(0, "sget-object", {0}, Value),
                           op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  auto &Caller = Classes[1].methods[0];
  Caller.reference.parameters[0] = Array;
  Caller.reference.returns = Array;
  Caller.generic_signature = "<Caller:Ljava/lang/Object;>([TCaller;Lfixture/"
                             "Carrier<Ljava/lang/RuntimeException;>;)[TCaller;";
  auto Call = Caller.instructions[0];
  Call.reference = Target.reference;
  Caller.instructions = {Call, op(1, "move-result-object", {0}),
                         op(2, "return-object", {0})};
  Caller.code_end = 3;
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = "Ljava/lang/Exception;";
  Classes.push_back(Runtime);
  return Classes;
}
std::vector<Class> seedFixture(bool SDK, bool Range, bool Alpha, bool Invariant,
                               bool ExceptionFirst, const std::string &Bound) {
  auto Classes = arrayFixture(SDK, Range, Alpha, Invariant, ExceptionFirst);
  const std::string T = Alpha ? "Element" : "T", X = Alpha ? "Failure" : "X";
  const bool Interface = Bound == "Ljava/lang/CharSequence;";
  const std::string A = T + (Interface ? "::" : ":") + Bound,
                    E = X + ":Ljava/lang/Throwable;";
  auto &Target = Classes[0].methods[0];
  Target.reference.parameters[0] = Bound;
  Target.reference.returns = Bound;
  Target.generic_signature = "<" + (ExceptionFirst ? E + A : A + E) + ">(T" +
                             T + ";Lfixture/Carrier<" + (Invariant ? "" : "+") +
                             "T" + X + ";>;)T" + T + ";^T" + X + ";";
  auto &Value = Classes[3].fields[0].reference;
  Value.type = Bound;
  if (!SDK)
    Target.instructions[0].reference = Value;
  auto &Caller = Classes[1].methods[0];
  Caller.reference.parameters[0] = Bound;
  Caller.reference.returns = Bound;
  Caller.generic_signature =
      "<Caller" + std::string(Interface ? "::" : ":") + Bound +
      ">(TCaller;Lfixture/Carrier<Ljava/lang/RuntimeException;>;)TCaller;";
  Caller.instructions[0].reference = Target.reference;
  if (std::none_of(Classes.begin(), Classes.end(),
                   [&](const auto &C) { return C.name == Bound; })) {
    auto C = klass(Bound, true);
    if (Interface)
      C.access = {"public", "interface", "abstract"};
    Classes.push_back(C);
  }
  return Classes;
}
ClassMap input(std::vector<Class> Classes, unsigned Route, Budget &B) {
  if (Route == 2)
    std::reverse(Classes.begin(), Classes.end());
  if (Route)
    return linkClasses(Classes, B);
  ClassMap Result;
  for (auto &C : Classes)
    Result.emplace(C.name, std::move(C));
  return Result;
}
} // namespace

TEST(MobileDalvikThrowableCarrierScalar,
     KeepScopedExceptionCarrierScalarBitsAndThrows) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (unsigned Contexts : {1u, 3u})
          for (const auto *Type : {"Z", "B", "C", "S", "I", "F", "J", "D"})
            for (bool Invariant : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                if (Contexts == 3 && !Range)
                  continue;
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Range, Alpha, Contexts, Type, Invariant, Route}));
                Budget B;
                auto Classes =
                    input(fixture(SDK, Range, Alpha, Contexts, Type, Invariant),
                          Route, B);
                auto Target = Classes.at("Lfixture/Provider;").methods[0];
                const auto Plans = validateGenericSignatures(Classes, B);
                ASSERT_TRUE(Plans.erased_generic_invocations.contains(
                    Target.reference));
                const auto &Signature = Plans.methods.at(Target.reference);
                ASSERT_EQ(Signature.throws_types.size(), 1u);
                EXPECT_EQ(Signature.types.at(Signature.throws_types[0])
                              .variable_owner,
                          Target.reference.identity());
                llvm::json::Object Report;
                ASSERT_NO_THROW(Report = recoverJava(Classes, B));
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
                EXPECT_EQ(Classes.at(Target.reference.owner)
                              .methods[0]
                              .generic_signature,
                          Target.generic_signature);
                EXPECT_EQ(Classes.at(Target.reference.owner)
                              .methods[0]
                              .declared_throws,
                          Target.declared_throws);
              }
}
TEST(MobileDalvikThrowableCarrierScalar,
     RejectUnboundThrowsWrongVarianceAndPhysicalFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 18; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, 3, "D", false);
        auto &Provider = Classes[0];
        auto &Target = Provider.methods[0], &Caller = Classes[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("^TX;", "");
          break;
        case 1:
          Replace("^TX;", "^TMissing;");
          break;
        case 2:
          Replace("<+TX;>", "<-TX;>");
          break;
        case 3:
          Replace("<+TX;>", "<*>");
          break;
        case 4:
          Replace("X:Ljava/lang/Throwable;", "X:Ljava/lang/Object;");
          break;
        case 5:
          Target.declared_throws.reset();
          break;
        case 6:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 7:
          Target.access.erase("static");
          break;
        case 8:
          Target.access = {"private", "static"};
          break;
        case 9:
          Target.access.insert("native");
          break;
        case 10:
          Target.access.insert("synthetic");
          break;
        case 11:
          Replace(")D^", ")J^");
          break;
        case 12:
          Replace("(DDD", "(TUnknown;DD");
          break;
        case 13:
          Caller.instructions[0].registers[1] =
              Caller.instructions[0].registers[0];
          break;
        case 14:
          Caller.instructions[0].registers.pop_back();
          break;
        case 15:
          Provider.generic_signature =
              "<X:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 16:
          Classes[2].generic_signature =
              "<Element:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 17:
          Caller.instructions[1].opcode = "move-result-object";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

TEST(MobileDalvikThrowableCarrierContext,
     KeepVoidNominalContextsOriginalScopesAndThrows) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (unsigned Count : {1u, 2u, 3u})
          for (const auto *Context :
               {"Ljava/lang/Object;", "Ljava/lang/String;",
                "Lfixture/Context;"})
            for (bool Invariant : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Range, Alpha, Count, Context, Invariant, Route}));
                Budget B;
                auto Classes = input(
                    fixture(SDK, Range, Alpha, Count, "V", Invariant, Context),
                    Route, B);
                const auto Target = Classes.at("Lfixture/Provider;").methods[0];
                const auto Plans = validateGenericSignatures(Classes, B);
                ASSERT_TRUE(Plans.erased_generic_invocations.contains(
                    Target.reference));
                llvm::json::Object Report;
                ASSERT_NO_THROW(Report = recoverJava(Classes, B));
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
                EXPECT_EQ(Classes.at(Target.reference.owner)
                              .methods[0]
                              .generic_signature,
                          Target.generic_signature);
                EXPECT_EQ(Classes.at(Target.reference.owner)
                              .methods[0]
                              .declared_throws,
                          Target.declared_throws);
              }
}
TEST(MobileDalvikThrowableCarrierContext,
     RejectOpenContextsVarianceThrowsAndWordDrift) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 18; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes =
            fixture(SDK, true, false, 2, "V", true, "Lfixture/Context;");
        auto &Provider = Classes[0];
        auto &Target = Provider.methods[0], &Caller = Classes[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Provider.generic_signature =
              "<X:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 1:
          Replace("<TX;>", "<*>");
          break;
        case 2:
          Replace("<TX;>", "<-TX;>");
          break;
        case 3:
          Replace("^TX;", "^TUnknown;");
          break;
        case 4:
          Target.declared_throws.reset();
          break;
        case 5:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 6:
          Target.access = {"private", "static"};
          break;
        case 7:
          Target.access.erase("static");
          break;
        case 8:
          Target.access.insert("synthetic");
          break;
        case 9:
          Target.access.insert("varargs");
          break;
        case 10:
          Replace(")V^", ")D^");
          break;
        case 11:
          Replace("Lfixture/Context;", "TUnknown;");
          break;
        case 12:
          Target.reference.parameters[0] = "Ljava/lang/Object;";
          break;
        case 13:
          Caller.instructions[0].registers[0] = Caller.registers;
          break;
        case 14:
          Classes.erase(Classes.begin() + 6);
          break;
        case 15:
          Classes[6].generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 16:
          Replace("<TX;>", "<TUnknown;>");
          break;
        case 17:
          Caller.instructions.insert(Caller.instructions.begin() + 1,
                                     op(1, "move-result-object", {0}));
          Caller.instructions.back().pc = 2;
          Caller.code_end = 3;
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

TEST(MobileDalvikThrowableArrayCarrier,
     KeepIndependentArrayAndExceptionScopesInEitherDeclarationOrder) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Invariant : {false, true})
          for (bool ExceptionFirst : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(std::tuple{
                  SDK, Range, Alpha, Invariant, ExceptionFirst, Route}));
              ASSERT_NO_THROW(([&] {
                Budget B;
                auto Classes = input(
                    arrayFixture(SDK, Range, Alpha, Invariant, ExceptionFirst),
                    Route, B);
                const auto Target = Classes.at("Lfixture/Provider;").methods[0];
                const auto Plans = validateGenericSignatures(Classes, B);
                EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                    Target.reference));
                const auto &S = Plans.methods.at(Target.reference);
                const auto &Array = S.types.at(*S.result);
                const auto &Element = S.types.at(*Array.element),
                           &Thrown = S.types.at(S.throws_types[0]);
                EXPECT_EQ(Element.variable_owner, Target.reference.identity());
                EXPECT_EQ(Thrown.variable_owner, Target.reference.identity());
                EXPECT_NE(Element.variable_name, Thrown.variable_name);
                const auto R = recoverJava(Classes, B);
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
                EXPECT_EQ(Classes.at(Target.reference.owner)
                              .methods[0]
                              .generic_signature,
                          Target.generic_signature);
                EXPECT_EQ(Classes.at(Target.reference.owner)
                              .methods[0]
                              .declared_throws,
                          Target.declared_throws);
              })());
            }
}
TEST(MobileDalvikThrowableArrayCarrier,
     RejectMergedForeignUnboundAndWrongErasureRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = arrayFixture(SDK, true, false, true, false);
        auto &Provider = Classes[0];
        auto &Target = Provider.methods[0], &Caller = Classes[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
          break;
        case 1:
          Replace(")[TT;", ")[TX;");
          break;
        case 2:
          Replace("Carrier<TX;>", "Carrier<TT;>");
          break;
        case 3:
          Replace("^TX;", "^TT;");
          break;
        case 4:
          Replace("<T:Ljava/lang/Object;",
                  "<Other:Ljava/lang/Object;T:Ljava/lang/Object;");
          break;
        case 5:
          Replace("([TT;", "([Ljava/lang/Object;");
          break;
        case 6:
          Target.declared_throws.reset();
          break;
        case 7:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/RuntimeException;"};
          break;
        case 8:
          Replace("X:Ljava/lang/Throwable;", "X:Ljava/lang/Object;");
          break;
        case 9:
          Replace("([TT;", "([TUnknown;");
          break;
        case 10:
          Target.access = {"private", "static"};
          break;
        case 11:
          Target.access.erase("static");
          break;
        case 12:
          Provider.generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 13:
          Caller.instructions[0].registers[0] = Caller.registers;
          break;
        case 14:
          Caller.instructions[1].opcode = "move-result-wide";
          break;
        case 15:
          Classes[2].generic_signature =
              "<Element:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

TEST(MobileDalvikThrowableBoundSeed,
     KeepPlainClassInterfaceAndEqualBoundScopesByOriginalThrowsSlot) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Invariant : {false, true})
          for (bool ExceptionFirst : {false, true})
            for (const auto *Bound :
                 {"Ljava/lang/Object;", "Ljava/lang/Number;",
                  "Ljava/lang/CharSequence;", "Ljava/lang/Throwable;"})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Range, Alpha, Invariant, ExceptionFirst,
                               Bound, Route}));
                ASSERT_NO_THROW(([&] {
                  Budget B;
                  auto Classes = input(seedFixture(SDK, Range, Alpha, Invariant,
                                                   ExceptionFirst, Bound),
                                       Route, B);
                  const auto Target =
                      Classes.at("Lfixture/Provider;").methods[0];
                  const auto Plans = validateGenericSignatures(Classes, B);
                  EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                      Target.reference));
                  const auto &S = Plans.methods.at(Target.reference);
                  const auto &Result = S.types.at(*S.result),
                             &Thrown = S.types.at(S.throws_types[0]);
                  EXPECT_EQ(Result.erasure, Bound);
                  EXPECT_EQ(Result.variable_owner, Target.reference.identity());
                  EXPECT_EQ(Thrown.variable_owner, Target.reference.identity());
                  EXPECT_NE(Result.variable_name, Thrown.variable_name);
                  const auto R = recoverJava(Classes, B);
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                  EXPECT_EQ(Classes.at(Target.reference.owner)
                                .methods[0]
                                .generic_signature,
                            Target.generic_signature);
                  EXPECT_EQ(Classes.at(Target.reference.owner)
                                .methods[0]
                                .declared_throws,
                            Target.declared_throws);
                })());
              }
}
TEST(MobileDalvikThrowableBoundSeed,
     RejectUnprovedIntersectionsMismatchedSeedsAndExceptionScopes) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = seedFixture(SDK, true, false, true, false,
                                   "Ljava/lang/CharSequence;");
        auto &Provider = Classes[0];
        auto &Target = Provider.methods[0], &Caller = Classes[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("T::Ljava/lang/CharSequence;", "T:Ljava/lang/Object;");
          break;
        case 1:
          Replace(")TT;", ")TX;");
          break;
        case 2:
          Replace("Carrier<TX;>", "Carrier<TT;>");
          break;
        case 3:
          Replace("^TX;", "^TT;");
          break;
        case 4:
          Replace("<T::", "<Other:Ljava/lang/Object;T::");
          break;
        case 5:
          Replace("(TT;", "(Ljava/lang/CharSequence;");
          break;
        case 6:
          Target.declared_throws.reset();
          break;
        case 7:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/RuntimeException;"};
          break;
        case 8:
          Replace("X:Ljava/lang/Throwable;", "X:Ljava/lang/Object;");
          break;
        case 9:
          Replace("(TT;", "(TUnknown;");
          break;
        case 10:
          Target.access = {"private", "static"};
          break;
        case 11:
          Target.access.erase("static");
          break;
        case 12:
          Provider.generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 13:
          Caller.instructions[0].registers[0] = Caller.registers;
          break;
        case 14:
          Caller.instructions[1].opcode = "move-result-wide";
          break;
        case 15: {
          Replace("T::Ljava/lang/CharSequence;",
                  "T::Ljava/lang/CharSequence;:Ljava/lang/Runnable;");
          auto C = klass("Ljava/lang/Runnable;", true);
          C.access = {"public", "interface", "abstract"};
          Classes.push_back(C);
          break;
        }
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
