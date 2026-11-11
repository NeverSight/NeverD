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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Varargs,
                           unsigned Rank, bool Wide, bool Wrapper,
                           bool Throws) {
  const std::string Object = "Ljava/lang/Object;",
                    Array = "[Ljava/lang/Object;",
                    String = "Ljava/lang/String;", T = Alpha ? "Element" : "T",
                    A = Alpha ? "Caller" : "A",
                    Meta = std::string(Rank, '[') + Object;
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{Array, String};
  if (Wide)
    Params.push_back("J");
  Params.push_back(Meta);
  auto Target = method({Provider.name, "transfer", Params, Array},
                       {"public", "static"}, SDK ? 0 : (Wide ? 7 : 5), {});
  if (Varargs)
    Target.access.insert("varargs");
  Target.generic_signature = "<" + T + ":Ljava/lang/Object;>([T" + T + ";" +
                             String + (Wide ? "J" : "") + Meta + ")[T" + T +
                             ";";
  if (Throws)
    Target.declared_throws =
        std::vector<std::string>{"Ljava/lang/IllegalArgumentException;"};
  FieldRef Value{"Lfixture/Trace;", "result", Array};
  if (!SDK) {
    Target.instructions = {op(0, "sget-object", {0}, Value),
                           op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  Provider.methods.push_back(Target);
  auto Core = klass("Lfixture/Core;");
  Method Caller;
  if (Wrapper) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    auto Text = op(2, "const-string", {1});
    Text.literal = std::string("owned-context");
    Caller =
        method({Core.name, "call", {Array}, Array}, {"public", "static"}, 3,
               {Zero, op(1, "new-array", {0, 0}, Meta), Text,
                op(3, "invoke-static", {2, 1, 0}, Target.reference),
                op(4, "move-result-object", {0}), op(5, "return-object", {0})});
    Caller.generic_signature =
        "<" + A + ":Ljava/lang/Object;>([T" + A + ";)[T" + A + ";";
  } else {
    unsigned Start = Range ? 32 : 2, Words = Wide ? 5 : 3;
    std::vector<unsigned> Registers;
    for (unsigned I = 0; I < Words; ++I)
      Registers.push_back(Start + I);
    Caller = method(
        {Core.name, "call", Params, Array}, {"public", "static"}, Start + Words,
        {op(0, Range ? "invoke-static/range" : "invoke-static", Registers,
            Target.reference),
         op(1, "move-result-object", {0}), op(2, "return-object", {0})});
    Caller.generic_signature = "<" + A + ":Ljava/lang/Object;>([T" + A + ";" +
                               String + (Wide ? "J" : "") + Meta + ")[T" + A +
                               ";";
  }
  if (Throws)
    Caller.declared_throws = Target.declared_throws;
  Core.methods.push_back(Caller);
  auto Trace = klass(Value.owner, true);
  Trace.fields.push_back({Value, {"public", "static"}, {}});
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Text = klass(String, true);
  auto Failure = klass("Ljava/lang/IllegalArgumentException;", true);
  Failure.superclass = "Ljava/lang/RuntimeException;";
  auto Checked = klass("Ljava/io/IOException;", true);
  Checked.superclass = "Ljava/lang/Exception;";
  return {Provider, Core, Trace, Root, Text, Failure, Checked};
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
TEST(MobileDalvikArrayTransferContext,
     KeepOneScopedArrayOpaqueResultClosedContextsAndFixedVarargs) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Varargs : {false, true})
          for (unsigned Rank : {1u, 2u})
            for (bool Wide : {false, true})
              for (bool Wrapper : {false, true})
                for (bool Throws : {false, true})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    if (Wrapper && Wide)
                      continue;
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, Range, Alpha, Varargs, Rank, Wide,
                                   Wrapper, Throws, Route}));
                    ASSERT_NO_THROW(([&] {
                      Budget B;
                      auto Classes = input(fixture(SDK, Range, Alpha, Varargs,
                                                   Rank, Wide, Wrapper, Throws),
                                           Route, B);
                      const auto Target =
                          Classes.at("Lfixture/Provider;").methods[0];
                      const auto Plans = validateGenericSignatures(Classes, B);
                      EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                          Target.reference));
                      const auto Report = recoverJava(Classes, B);
                      EXPECT_EQ(Report.getInteger("unrecovered_method_count"),
                                0);
                      EXPECT_FALSE(
                          Report.getArray("generic_signature_projections"));
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
TEST(MobileDalvikArrayTransferContext,
     RejectUnprovedComponentsContextsExceptionsFlagsAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, true, 1, false, false, true);
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
          Replace("([TT;", "([TUnknown;");
          break;
        case 2:
          Replace(")[TT;", ")[TUnknown;");
          break;
        case 3:
          Replace(")[TT;", ")[[TT;");
          break;
        case 4:
          Replace("Ljava/lang/String;", "Ljava/lang/Object;");
          break;
        case 5:
          Replace("[Ljava/lang/Object;", "[TT;");
          break;
        case 6:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/io/IOException;"};
          break;
        case 7:
          Target.access.erase("static");
          break;
        case 8:
          Target.access = {"private", "static", "varargs"};
          break;
        case 9:
          Target.access.insert("native");
          break;
        case 10:
          Target.access.insert("synthetic");
          break;
        case 11:
          Provider.generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 12:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 13:
          Caller.instructions[0].registers.pop_back();
          break;
        case 14:
          Caller.instructions[1].opcode = "move-result-wide";
          break;
        case 15:
          Replace("Ljava/lang/String;", "Lfixture/Missing;");
          Target.reference.parameters[1] = "Lfixture/Missing;";
          Caller.reference.parameters[1] = "Lfixture/Missing;";
          Caller.generic_signature.reset();
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

namespace {
std::vector<Class> instanceArrayFixture(bool SDK, bool Range, bool Alpha,
                                        bool Inherited, bool Shadow,
                                        bool GenericCaller, bool Synchronized) {
  const std::string Object = "Ljava/lang/Object;",
                    Array = "[Ljava/lang/Object;",
                    StringArray = "[Ljava/lang/String;",
                    T = Alpha ? "Element" : "T", E = Shadow ? T : "Owner";
  auto Provider = klass("Lfixture/Provider;", SDK);
  Provider.generic_signature =
      "<" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Target = method({Provider.name, "transfer", {Array}, Array}, {"public"},
                       SDK ? 0 : 3, {});
  if (Synchronized)
    Target.access.insert("synchronized");
  Target.generic_signature =
      "<" + T + ":Ljava/lang/Object;>([T" + T + ";)[T" + T + ";";
  FieldRef Value{"Lfixture/Trace;", "result", Array};
  if (!SDK) {
    Target.instructions = {op(0, "sget-object", {0}, Value),
                           op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  Provider.methods.push_back(Target);
  const MethodRef RootInit{Object, "<init>", {}, "V"};
  auto Init = method({Provider.name, "<init>", {}, "V"},
                     {"public", "constructor"}, SDK ? 0 : 1, {});
  if (!SDK) {
    Init.instructions = {op(0, "invoke-direct", {0}, RootInit),
                         op(1, "return-void")};
    Init.code_end = 2;
  }
  Provider.methods.push_back(Init);
  auto Derived = klass("Lfixture/Derived;", SDK);
  Derived.superclass = Provider.name;
  Derived.generic_signature = "<D:Ljava/lang/Object;>Lfixture/Provider<TD;>;";
  auto Core = klass("Lfixture/Core;");
  const auto Receiver = Inherited ? Derived.name : Provider.name;
  auto Ref = Target.reference;
  Ref.owner = Receiver;
  const unsigned Start = Range ? 32 : 1;
  auto Caller = method({Core.name,
                        "call",
                        {Receiver, GenericCaller ? Array : StringArray},
                        GenericCaller ? Array : StringArray},
                       {"public", "static"}, Start + 2,
                       {op(0, Range ? "invoke-virtual/range" : "invoke-virtual",
                           {Start, Start + 1}, Ref),
                        op(1, "move-result-object", {0})});
  if (GenericCaller)
    Caller.generic_signature = "<A:Ljava/lang/Object;>(" +
                               Receiver.substr(0, Receiver.size() - 1) +
                               "<*>;[TA;)[TA;";
  else
    Caller.instructions.push_back(op(2, "check-cast", {0}, StringArray));
  Caller.instructions.push_back(op(3, "return-object", {0}));
  Caller.code_end = 4;
  Core.methods.push_back(Caller);
  auto Trace = klass(Value.owner, true);
  Trace.fields.push_back({Value, {"public", "static"}, {}});
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(method(RootInit, {"public", "constructor"}, 0, {}));
  auto Text = klass("Ljava/lang/String;", true);
  auto Checked = klass("Ljava/io/IOException;", true);
  Checked.superclass = "Ljava/lang/Exception;";
  return {Provider, Core, Derived, Trace, Root, Text, Checked};
}
} // namespace

TEST(MobileDalvikArrayTransferContext,
     PreserveIndependentInstanceArrayFormalsAndInheritedDispatch) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Inherited : {false, true})
          for (bool Shadow : {false, true})
            for (bool GenericCaller : {false, true})
              for (bool Synchronized : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, Range, Alpha, Inherited, Shadow,
                                 GenericCaller, Synchronized, Route}));
                  auto Original =
                      instanceArrayFixture(SDK, Range, Alpha, Inherited, Shadow,
                                           GenericCaller, Synchronized);
                  const auto Target = Original[0].methods[0];
                  EXPECT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = input(Original, Route, B);
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                        Target.reference));
                    const auto Report = recoverJava(Classes, B);
                    EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(
                        Report.getArray("generic_signature_projections"));
                    EXPECT_EQ(Classes.at(Target.reference.owner)
                                  .methods[0]
                                  .generic_signature,
                              Target.generic_signature);
                    EXPECT_EQ(
                        Classes.at(Target.reference.owner).methods[0].access,
                        Target.access);
                  })());
                }
}
TEST(MobileDalvikArrayTransferContext,
     RejectUnprovedInstanceArrayScopeAndInvocationFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 18; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes =
            instanceArrayFixture(SDK, true, false, true, false, false, true);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
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
          Replace("([TT;", "([TUnknown;");
          break;
        case 2:
          Replace(")[TT;", ")[TUnknown;");
          break;
        case 3:
          Replace("([TT;", "([TOwner;");
          break;
        case 4:
          Replace(")[TT;", ")[TOwner;");
          break;
        case 5:
          Replace(")[TT;", ")[[TT;");
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
          Target.access.insert("synthetic");
          break;
        case 10:
          Target.access.insert("varargs");
          break;
        case 11:
          Target.access.insert("static");
          break;
        case 12:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 13:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/io/IOException;"};
          break;
        case 14:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 15:
          Caller.instructions[0].registers.pop_back();
          break;
        case 16:
          Caller.instructions[1].opcode = "move-result-wide";
          break;
        case 17:
          Classes[0].access.insert("interface");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> arrayStarContextFixture(bool SDK, bool ContextSDK,
                                           bool Interface, bool Alpha,
                                           bool Range, bool Upper, bool Tail,
                                           unsigned Width) {
  auto Classes = fixture(SDK, Range, Alpha, false, 1, false, false, false);
  const std::string T = Alpha ? "Element" : "T", A = Alpha ? "Caller" : "A",
                    Object = "Ljava/lang/Object;",
                    Array = "[Ljava/lang/Object;", Token = "Lfixture/Token;",
                    Scalar = Width == 0   ? "I"
                             : Width == 1 ? "J"
                                          : "D";
  const std::string Slot = Upper ? "+Ljava/lang/Object;" : "*";
  auto &Target = Classes[0].methods[0];
  Target.reference.parameters = {Array, Scalar, Token};
  if (Tail) {
    Target.reference.parameters.push_back(Array);
    Target.access.insert("varargs");
  }
  Target.generic_signature = "<" + T + ":Ljava/lang/Object;>([T" + T + ";" +
                             Scalar + "Lfixture/Token<" + Slot + ">;" +
                             (Tail ? Array : "") + ")[T" + T + ";";
  const unsigned Words = (Width == 0 ? 3 : 4) + (Tail ? 1 : 0);
  Target.registers = SDK ? 0 : Words + 1;
  auto &Caller = Classes[1].methods[0];
  Caller.reference.parameters = Target.reference.parameters;
  Caller.generic_signature = "<" + A + ":Ljava/lang/Object;>([T" + A + ";" +
                             Scalar + "Lfixture/Token<" + Slot + ">;" +
                             (Tail ? Array : "") + ")[T" + A + ";";
  const unsigned Start = Range ? 32 : 1;
  std::vector<unsigned> Registers;
  for (unsigned I = 0; I < Words; ++I)
    Registers.push_back(Start + I);
  Caller.registers = Start + Words;
  Caller.instructions = {op(0, Range ? "invoke-static/range" : "invoke-static",
                            Registers, Target.reference),
                         op(3, "move-result-object", {0}),
                         op(4, "return-object", {0})};
  Caller.code_end = 5;
  auto Context = klass(Token, ContextSDK);
  Context.generic_signature =
      "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (Interface)
    Context.access = {"public", "interface", "abstract"};
  Classes.push_back(Context);
  Classes.push_back(klass("Ljava/lang/Number;", true));
  return Classes;
}
} // namespace

TEST(MobileDalvikArrayTransferContext,
     RetainOwnArrayComponentsBesideCompleteUnboundedContexts) {
  for (bool SDK : {false, true})
    for (bool ContextSDK : {false, true})
      for (bool Interface : {false, true})
        for (bool Alpha : {false, true})
          for (bool Range : {false, true})
            for (bool Upper : {false, true})
              for (bool Tail : {false, true})
                for (unsigned Width : {0u, 1u, 2u})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, ContextSDK, Interface, Alpha, Range,
                                   Upper, Tail, Width, Route}));
                    EXPECT_NO_THROW(([&] {
                      Budget B;
                      auto Classes =
                          input(arrayStarContextFixture(SDK, ContextSDK,
                                                        Interface, Alpha, Range,
                                                        Upper, Tail, Width),
                                Route, B);
                      const auto Target =
                          Classes.at("Lfixture/Provider;").methods[0];
                      const auto Plans = validateGenericSignatures(Classes, B);
                      EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                          Target.reference));
                      const auto Report = recoverJava(Classes, B);
                      EXPECT_EQ(Report.getInteger("unrecovered_method_count"),
                                0);
                      EXPECT_FALSE(
                          Report.getArray("generic_signature_projections"));
                      EXPECT_EQ(Classes.at(Target.reference.owner)
                                    .methods[0]
                                    .generic_signature,
                                Target.generic_signature);
                      EXPECT_EQ(
                          Classes.at(Target.reference.owner).methods[0].access,
                          Target.access);
                    })());
                  }
}
TEST(MobileDalvikArrayTransferContext,
     RejectLostArrayBindingsUnprovedContextsDeclarationsAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 26; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = arrayStarContextFixture(SDK, true, true, false, false,
                                               false, false, 0);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          const auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
          break;
        case 1:
          Replace("([TT;", "([TMissing;");
          break;
        case 2:
          Replace(")[TT;", ")[TMissing;");
          break;
        case 3:
          Replace("([TT;", "([[TT;");
          break;
        case 4:
          Replace(")[TT;", ")[[TT;");
          break;
        case 5:
          Replace("Token<*>", "Token");
          break;
        case 6:
          Replace("Token<*>", "Token<+TMissing;>");
          break;
        case 7:
          Replace("Token<*>", "Token<+Ljava/lang/Number;>");
          break;
        case 8:
          Replace("Token<*>", "Token<-Ljava/lang/Object;>");
          break;
        case 9:
          Replace("<T:", "<Unused:Ljava/lang/Object;T:");
          break;
        case 10:
          Classes[7].generic_signature =
              "<T:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 11:
          Classes.erase(Classes.begin() + 7);
          break;
        case 12:
          Classes[7].enclosing = "Lfixture/Missing;";
          break;
        case 13:
          Classes[7].generic_signature = "<T:Ljava/lang/Object;Extra:Ljava/"
                                         "lang/Object;>Ljava/lang/Object;";
          break;
        case 14:
          Target.access.insert("native");
          break;
        case 15:
          Target.access.insert("abstract");
          break;
        case 16:
          Target.access.insert("bridge");
          break;
        case 17:
          Target.access.insert("synthetic");
          break;
        case 18:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 19:
          Target.access.erase("static");
          break;
        case 20:
          Classes[0].access.insert("interface");
          break;
        case 21:
          Classes[0].generic_signature =
              "<Owner:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 22:
          Target.declared_throws = {"Ljava/io/IOException;"};
          break;
        case 23:
          Caller.instructions[0].registers.pop_back();
          break;
        case 24:
          Caller.instructions[1].opcode = "move-result-wide";
          break;
        case 25:
          Target.access.insert("varargs");
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
std::vector<Class> arrayInputMappedTokenFixture(bool SDK, bool TokenInterface,
                                                bool MapperInterface,
                                                bool Alpha, bool Facade,
                                                bool Range,
                                                unsigned Permutation) {
  const std::string Object = "Ljava/lang/Object;",
                    Array = "[Ljava/lang/Object;", Token = "Lfixture/Token;",
                    Mapper = "Lfixture/Mapper;", T = Alpha ? "Consumed" : "T",
                    R = Alpha ? "Produced" : "R";
  const std::vector<std::vector<unsigned>> Orders{
      {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  const std::vector<std::string> Types{Array, Token, Mapper};
  const std::vector<std::string> Signatures{
      "[T" + T + ";", "Lfixture/Token<T" + R + ";>;",
      "Lfixture/Mapper<-T" + T + ";+T" + R + ";>;"};
  std::vector<std::string> Parameters;
  std::string Signature;
  for (auto I : Orders.at(Permutation)) {
    Parameters.push_back(Types[I]);
    Signature += Signatures[I];
  }
  auto Provider = klass("Lfixture/Provider;", SDK);
  Provider.superclass = "Lfixture/Parent;";
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method({Provider.name, "map", Parameters, Array}, {"public", "static"},
             SDK ? 0 : 4,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  const std::string Formals =
      Alpha ? R + ":Ljava/lang/Object;" + T : T + ":Ljava/lang/Object;" + R;
  Target.generic_signature =
      "<" + Formals + ":Ljava/lang/Object;>(" + Signature + ")[T" + R + ";";
  Provider.methods.push_back(Target);
  auto Core = klass("Lfixture/Core;");
  auto Ref = Target.reference;
  auto Alias = klass("Lfixture/Facade;", SDK);
  Alias.superclass = Provider.name;
  if (Facade)
    Ref.owner = Alias.name;
  const unsigned Start = Range ? 32 : 1;
  Core.methods.push_back(method(
      {Core.name, "call", Parameters, Array}, {"public", "static"}, Start + 3,
      {op(0, Range ? "invoke-static/range" : "invoke-static",
          {Start, Start + 1, Start + 2}, Ref),
       op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto TokenClass = klass(Token, SDK);
  TokenClass.generic_signature =
      "<" + R + ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (TokenInterface)
    TokenClass.access = {"public", "abstract", "interface"};
  auto MapperClass = klass(Mapper, SDK);
  MapperClass.generic_signature = "<" + T + ":Ljava/lang/Object;" + R +
                                  ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (MapperInterface)
    MapperClass.access = {"public", "abstract", "interface"};
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Checked = klass("Ljava/io/IOException;", true);
  Checked.superclass = "Ljava/lang/Exception;";
  return {Provider,
          Core,
          klass("Lfixture/Parent;", true),
          TokenClass,
          MapperClass,
          Root,
          Alias,
          klass("Ljava/lang/Number;", true),
          Checked};
}
} // namespace
TEST(MobileDalvikArrayTransferContext,
     OriginalArrayComponentsBindMapperInputsAndIndependentTokenResults) {
  for (bool SDK : {false, true})
    for (bool TokenInterface : {false, true})
      for (bool MapperInterface : {false, true})
        for (bool Alpha : {false, true})
          for (bool Facade : {false, true})
            for (bool Range : {false, true})
              for (unsigned Permutation = 0; Permutation < 6; ++Permutation)
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, TokenInterface, MapperInterface, Alpha,
                                 Facade, Range, Permutation, Route}));
                  EXPECT_NO_THROW(([&] {
                    Budget B;
                    auto Classes =
                        input(arrayInputMappedTokenFixture(
                                  SDK, TokenInterface, MapperInterface, Alpha,
                                  Facade, Range, Permutation),
                              Route, B);
                    const auto Target =
                        Classes.at("Lfixture/Provider;").methods[0];
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                        Target.reference));
                    const auto Report = recoverJava(Classes, B);
                    EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(
                        Report.getArray("generic_signature_projections"));
                    const auto &After =
                        Classes.at(Target.reference.owner).methods[0];
                    EXPECT_EQ(After.generic_signature,
                              Target.generic_signature);
                    EXPECT_EQ(After.access, Target.access);
                  })());
                }
}
TEST(MobileDalvikArrayTransferContext,
     RejectUnboundInputComponentsMapperVarianceTokensScopesAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 32; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Input = arrayInputMappedTokenFixture(SDK, true, true, false, false,
                                                  true, 0);
        auto &Target = Input[0].methods[0], &Caller = Input[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          const auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("([TT;", "([TU;");
          break;
        case 1:
          Replace("([TT;", "([TR;");
          break;
        case 2:
          Replace("([TT;", "([Ljava/lang/Object;");
          break;
        case 3:
          Replace("Token<TR;>", "Token<+TR;>");
          break;
        case 4:
          Replace("Token<TR;>", "Token<-TR;>");
          break;
        case 5:
          Replace("Token<TR;>", "Token");
          break;
        case 6:
          Replace("Token<TR;>", "Token<TT;>");
          break;
        case 7:
          Replace("Mapper<-TT;+TR;>", "Mapper<-TR;+TR;>");
          break;
        case 8:
          Replace("Mapper<-TT;+TR;>", "Mapper<+TT;+TR;>");
          break;
        case 9:
          Replace("Mapper<-TT;+TR;>", "Mapper<-TT;-TR;>");
          break;
        case 10:
          Replace("Mapper<-TT;+TR;>", "Mapper<*+TR;>");
          break;
        case 11:
          Replace(")[TR;", ")[TT;");
          break;
        case 12:
          Replace("<T:", "<Unused:Ljava/lang/Object;T:");
          break;
        case 13:
          Input[3].generic_signature =
              "<R:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 14:
          Input[4].generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 15:
          Input[4].generic_signature =
              "<T:Ljava/lang/Object;R:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 16:
          Target.access.insert("native");
          break;
        case 17:
          Target.access.insert("abstract");
          break;
        case 18:
          Target.access.insert("bridge");
          break;
        case 19:
          Target.access.insert("synthetic");
          break;
        case 20:
          Target.access.insert("varargs");
          break;
        case 21:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 22:
          Target.access.erase("static");
          break;
        case 23:
          Input[0].generic_signature =
              "<Owner:Ljava/lang/Object;>Lfixture/Parent;";
          break;
        case 24:
          Input[0].access.insert("interface");
          break;
        case 25:
          Target.declared_throws = {"Ljava/io/IOException;"};
          break;
        case 26:
          Caller.instructions[0].registers.pop_back();
          break;
        case 27:
          Caller.instructions[1].opcode = "move-result-wide";
          break;
        case 28:
          Input[0].access.erase("public");
          break;
        case 29:
          Input[3].access.erase("public");
          break;
        case 30:
          Input[4].access.erase("public");
          break;
        case 31:
          Replace("R:Ljava/lang/Object;>", "R:Ljava/lang/Number;>");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Input, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> tokenStarArrayFactoryFixture(bool SDK, bool CarrierSDK,
                                                bool TokenInterface, bool Alpha,
                                                bool Facade, bool Range,
                                                bool Varargs,
                                                unsigned ContextMode) {
  const std::string Object = "Ljava/lang/Object;", Token = "Lfixture/Token;",
                    Array = "[Lfixture/Token;", Box = "Lfixture/Box;",
                    T = Alpha ? "Entry" : "T";
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Provider = klass("Lfixture/Provider;", SDK);
  auto Target =
      method({Provider.name, "select", {Token, Array}, Box},
             {"public", "static"}, SDK ? 0 : 3,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  if (Varargs)
    Target.access.insert("varargs");
  const std::string Any = ContextMode == 1 ? "+Ljava/lang/Object;" : "*";
  const std::string Context =
      ContextMode == 2 ? Token : "Lfixture/Token<" + Any + ">;";
  Target.generic_signature = "<" + T + ":Ljava/lang/Object;>(Lfixture/Token<T" +
                             T + ";>;[" + Context + ")Lfixture/Box<T" + T +
                             ";>;";
  Provider.methods.push_back(Target);
  Provider.methods.push_back(method(
      {Provider.name, "select", {Object, "[Ljava/lang/Object;"}, Box},
      {"public", "static", "varargs"}, SDK ? 0 : 3,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{Zero, op(1, "return-object", {0})}));
  auto Alias = klass("Lfixture/Facade;", SDK);
  Alias.superclass = Provider.name;
  auto Ref = Target.reference;
  if (Facade)
    Ref.owner = Alias.name;
  auto Core = klass("Lfixture/Core;");
  const unsigned Start = Range ? 32 : 1;
  Core.methods.push_back(method(
      {Core.name, "call", {Token, Array}, Box}, {"public", "static"}, Start + 2,
      {op(0, Range ? "invoke-static/range" : "invoke-static",
          {Start, Start + 1}, Ref),
       op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto TokenClass = klass(Token, CarrierSDK), BoxClass = klass(Box, CarrierSDK);
  TokenClass.generic_signature = BoxClass.generic_signature =
      "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (TokenInterface)
    TokenClass.access = {"public", "abstract", "interface"};
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Checked = klass("Ljava/io/IOException;", true);
  Checked.superclass = "Ljava/lang/Exception;";
  return {Provider,
          Core,
          TokenClass,
          BoxClass,
          Root,
          Alias,
          klass("Ljava/lang/Number;", true),
          Checked};
}
} // namespace
TEST(MobileDalvikArrayTransferContext,
     InvariantFactoryTokensRetainIndependentStarArraysAndFixedVarargs) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool TokenInterface : {false, true})
        for (bool Alpha : {false, true})
          for (bool Facade : {false, true})
            for (bool Range : {false, true})
              for (bool Varargs : {false, true})
                for (unsigned ContextMode : {0u, 1u, 2u})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(std::tuple{
                        SDK, CarrierSDK, TokenInterface, Alpha, Facade, Range,
                        Varargs, ContextMode, Route}));
                    EXPECT_NO_THROW(([&] {
                      Budget B;
                      auto Classes =
                          input(tokenStarArrayFactoryFixture(
                                    SDK, CarrierSDK, TokenInterface, Alpha,
                                    Facade, Range, Varargs, ContextMode),
                                Route, B);
                      const auto Target =
                          Classes.at("Lfixture/Provider;").methods[0];
                      const auto Plans = validateGenericSignatures(Classes, B);
                      EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                          Target.reference));
                      const auto Report = recoverJava(Classes, B);
                      EXPECT_EQ(Report.getInteger("unrecovered_method_count"),
                                0);
                      EXPECT_FALSE(
                          Report.getArray("generic_signature_projections"));
                      const auto &After =
                          Classes.at(Target.reference.owner).methods[0];
                      EXPECT_EQ(After.generic_signature,
                                Target.generic_signature);
                      EXPECT_EQ(After.access, Target.access);
                    })());
                  }
}
TEST(MobileDalvikArrayTransferContext,
     RejectUnboundFactoryTokensArraySlotsRolesErasuresAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 36; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Input = tokenStarArrayFactoryFixture(SDK, true, true, false, false,
                                                  true, true, false);
        auto &Target = Input[0].methods[0], &Caller = Input[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &S = *Target.generic_signature;
          const auto I = S.find(From);
          ASSERT_NE(I, std::string::npos);
          S.replace(I, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Token<TT;>", "Token<TMissing;>");
          break;
        case 1:
          Replace("Token<TT;>", "Token");
          break;
        case 2:
          Replace("Token<TT;>", "Token<+TT;>");
          break;
        case 3:
          Replace("Token<TT;>", "Token<-TT;>");
          break;
        case 4:
          Replace("Token<*>", "Token<**>");
          break;
        case 5:
          Replace("Token<*>", "Token<+Ljava/lang/Number;>");
          break;
        case 6:
          Replace("Token<*>", "Token<TT;>");
          break;
        case 7:
          Replace("Token<*>", "Token<-Ljava/lang/Object;>");
          break;
        case 8:
          Replace("Box<TT;>", "Box<+TT;>");
          break;
        case 9:
          Replace("Box<TT;>", "Box<TMissing;>");
          break;
        case 10:
          Replace("Box<TT;>", "Box<TT;TT;>");
          break;
        case 11:
          Replace("<T:", "<Unused:Ljava/lang/Object;T:");
          break;
        case 12:
          Replace("<T:Ljava/lang/Object;>", "<T:Ljava/lang/Number;>");
          break;
        case 13:
          Input[2].generic_signature =
              "<T:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 14:
          Input[2].generic_signature =
              "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 15:
          Input[3].generic_signature =
              "<T:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 16:
          Input[3].generic_signature =
              "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 17:
          Input[2].access.erase("public");
          break;
        case 18:
          Input[3].access.erase("public");
          break;
        case 19:
          Target.access.insert("native");
          break;
        case 20:
          Target.access.insert("abstract");
          break;
        case 21:
          Target.access.insert("bridge");
          break;
        case 22:
          Target.access.insert("synthetic");
          break;
        case 23:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 24:
          Target.access.erase("static");
          break;
        case 25:
          Input[0].generic_signature =
              "<Owner:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 26:
          Input[0].access.insert("interface");
          break;
        case 27:
          Target.declared_throws = {"Ljava/io/IOException;"};
          break;
        case 28:
          Caller.instructions[0].registers.pop_back();
          break;
        case 29:
          Caller.instructions[1].opcode = "move-result-wide";
          break;
        case 30:
          Input[0].access.erase("public");
          break;
        case 31:
          Replace("[Lfixture/Token", "[[Lfixture/Token");
          Target.reference.parameters[1] = "[[Lfixture/Token;";
          Caller.reference.parameters[1] = Target.reference.parameters[1];
          Caller.instructions[0].reference = Target.reference;
          break;
        case 32:
          Replace("Token<TT;>", "Token<*>");
          break;
        case 33:
          Replace("Box<TT;>", "Box<-TT;>");
          break;
        case 34:
          Replace("[Lfixture/Token", "[Lfixture/Box");
          break;
        case 35:
          Input[2].generic_signature.reset();
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Input, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}
