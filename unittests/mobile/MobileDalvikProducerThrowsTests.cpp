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
std::vector<Class> fixture(bool SDK, bool Instance, bool Range, bool Alpha,
                           unsigned Throws, unsigned Context,
                           bool HandlerSDK = true) {
  const std::string T = Alpha ? "Element" : "T", Handler = "Lfixture/Handler;",
                    Object = "Ljava/lang/Object;",
                    String = "Ljava/lang/String;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{Handler};
  std::string Sig = "Lfixture/Handler<T" + T + ";>;";
  if (Context) {
    Params.push_back(String);
    Sig += String;
  }
  if (Context == 2) {
    Params.push_back("J");
    Sig += "J";
  }
  const unsigned Words = 1 + (Context ? 1 : 0) + (Context == 2 ? 2 : 0);
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method({Provider.name, "read", Params, Object}, {"public", "static"},
             SDK ? 0 : Words + (Instance ? 2 : 1),
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  if (Instance)
    Target.access.erase("static");
  Target.generic_signature =
      "<" + T + ":Ljava/lang/Object;>(" + Sig + ")T" + T + ";";
  if (Throws)
    Target.declared_throws = std::vector<std::string>{
        Throws == 1 ? "Lfixture/Failure;" : "Ljava/lang/Exception;"};
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  if (Instance)
    Params.insert(Params.begin(), Provider.name);
  const unsigned CallerWords = Words + (Instance ? 1 : 0);
  std::vector<unsigned> Args;
  for (unsigned I = 1; I <= CallerWords; ++I)
    Args.push_back(I);
  std::string Opcode = Instance ? "invoke-virtual" : "invoke-static";
  if (Range)
    Opcode += "/range";
  Client.methods.push_back(
      method({Client.name, "call", Params, Object}, {"public", "static"},
             CallerWords + 1,
             {op(0, Opcode, Args, Target.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto Callback = klass(Handler, HandlerSDK);
  Callback.access = {"public", "abstract", "interface"};
  Callback.generic_signature = "<R:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Read = method({Handler, "read", {"Ljava/io/BufferedReader;"}, Object},
                     {"public", "abstract"}, 0, {});
  Read.generic_signature = "(Ljava/io/BufferedReader;)TR;";
  Read.declared_throws = std::vector<std::string>{"Ljava/io/IOException;"};
  Callback.methods.push_back(Read);
  auto Text = klass(String, true);
  Text.access.insert("final");
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Reader = klass("Ljava/io/BufferedReader;", true);
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = Throwable.name;
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = Exception.name;
  auto Failure = klass("Lfixture/Failure;", true);
  Failure.superclass = Runtime.name;
  auto IO = klass("Ljava/io/IOException;", true);
  IO.superclass = Exception.name;
  auto Number = klass("Ljava/lang/Number;", true);
  return {Client,    Provider,  Callback, Text,    Root, Reader,
          Throwable, Exception, Runtime,  Failure, IO,   Number};
}
} // namespace

TEST(MobileDalvikProducerThrows,
     RetainSoleProducerOwnResultScopesInstanceAndStaticClosedThrows) {
  for (bool SDK : {false, true})
    for (bool Instance : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (unsigned Throws : {0u, 1u, 2u})
            for (unsigned Context : {0u, 1u, 2u})
              for (bool HandlerSDK : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, Instance, Range, Alpha, Throws, Context,
                                 HandlerSDK, Route}));
                  auto Input = fixture(SDK, Instance, Range, Alpha, Throws,
                                       Context, HandlerSDK);
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
                  EXPECT_FALSE(
                      Report.getArray("generic_signature_projections"));
                  for (const auto &C : Original) {
                    const auto &After = Classes.at(C.name);
                    EXPECT_EQ(After.access, C.access);
                    EXPECT_EQ(After.generic_signature, C.generic_signature);
                    for (size_t I = 0; I < C.methods.size(); ++I) {
                      EXPECT_EQ(After.methods[I].reference,
                                C.methods[I].reference);
                      EXPECT_EQ(After.methods[I].generic_signature,
                                C.methods[I].generic_signature);
                      EXPECT_EQ(After.methods[I].declared_throws,
                                C.methods[I].declared_throws);
                    }
                  }
                }
}

TEST(MobileDalvikProducerThrows,
     RejectForeignFormalsBrokenCarrierBoundsIncompleteExceptionsAndWrongRoles) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, true, false, 1, 0);
      auto &Target = Input[1].methods[0];
      auto &Signature = *Target.generic_signature;
      auto Replace = [&](const std::string &From, const std::string &To) {
        const auto At = Signature.find(From);
        ASSERT_NE(At, std::string::npos);
        Signature.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        Replace("Object;>(", "Object;T:Ljava/lang/Object;>(");
      else if (Mutation == 1)
        Replace("Handler<TT;>", "Handler<TMissing;>");
      else if (Mutation == 2)
        Replace(")TT;", ")TMissing;");
      else if (Mutation == 3)
        Replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
      else if (Mutation == 4)
        Input[2].generic_signature =
            "<R:Ljava/lang/Object;S:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 5)
        Input[2].generic_signature = "<R:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 6) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 7)
        Target.access.insert("varargs");
      else if (Mutation == 8)
        Target.access.insert("native");
      else if (Mutation == 9)
        Target.access.insert("abstract");
      else if (Mutation == 10)
        Target.access.insert("synthetic");
      else if (Mutation == 11)
        Target.access.insert("bridge");
      else if (Mutation == 12)
        Target.access.insert("static");
      else if (Mutation == 13)
        Input[0].methods[0].instructions[0].registers.pop_back();
      else if (Mutation == 14)
        Target.declared_throws = std::vector<std::string>{"Ljava/lang/String;"};
      else if (Mutation == 15)
        Target.declared_throws = std::vector<std::string>{"Lfixture/Missing;"};
      else if (Mutation == 16)
        Input[1].generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
      else
        Replace(")TT;", ")Ljava/lang/String;");
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}

// Closed carrier contexts and checked producer results.
namespace {
std::vector<Class>
closedCarrierProducerFixture(bool SDK, bool CarriersSDK, bool Range, bool Alpha,
                             bool Static, bool InterfaceContext,
                             unsigned Throws, unsigned Position) {
  const std::string Object = "Ljava/lang/Object;", E = Alpha ? "Element" : "E",
                    Labels = "Lfixture/Labels;", Context = "Lfixture/Context;",
                    Producer = "Lfixture/Producer;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  Provider.access.insert("abstract");
  Provider.interfaces = {"Ljava/io/Serializable;"};
  std::vector<std::string> Params{Labels, Context, Producer};
  std::vector<std::string> Types{"Lfixture/Labels<Ljava/lang/String;>;",
                                 Context, "Lfixture/Producer<T" + E + ";>;"};
  std::rotate(Params.begin(), Params.begin() + Position, Params.end());
  std::rotate(Types.begin(), Types.begin() + Position, Types.end());
  std::string Sig = "<" + E + ":Ljava/lang/Object;>(";
  for (const auto &T : Types)
    Sig += T;
  Sig += ")T" + E + ";";
  auto Target = method({Provider.name, "read", Params, Object}, {"public"},
                       SDK ? 0 : (Static ? 4 : 5), {});
  if (Static)
    Target.access.insert("static");
  Target.generic_signature = Sig;
  if (Throws)
    Target.declared_throws = {Throws == 1 ? "Ljava/lang/RuntimeException;"
                                          : "Ljava/io/IOException;"};
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Target.instructions = {Zero, op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  Provider.methods.push_back(Target);
  auto Core = klass("Lfixture/Core;");
  if (!Static)
    Params.insert(Params.begin(), Provider.name);
  std::vector<unsigned> Args;
  for (unsigned N = 0; N < Params.size(); ++N)
    Args.push_back(N + 1);
  std::string Opcode = Static ? "invoke-static" : "invoke-virtual";
  if (Range)
    Opcode += "/range";
  Core.methods.push_back(
      method({Core.name, "call", Params, Object}, {"public", "static"},
             unsigned(Params.size() + 1),
             {op(0, Opcode, Args, Target.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  Core.methods[0].declared_throws = Target.declared_throws;
  auto Carrier = klass(Labels, CarriersSDK);
  Carrier.access = {"public", "interface", "abstract"};
  Carrier.generic_signature = "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Callback = klass(Producer, CarriersSDK);
  Callback.access = {"public", "interface", "abstract"};
  Callback.interfaces = {"Ljava/io/Serializable;"};
  Callback.generic_signature =
      "<Value:Ljava/lang/Object;>Ljava/lang/Object;Ljava/io/Serializable;";
  auto Produce = method({Producer, "produce", {Context}, Object},
                        {"public", "abstract"}, 0, {});
  Produce.generic_signature = "(Lfixture/Context;)TValue;";
  Callback.methods.push_back(Produce);
  auto Ctx = klass(Context, CarriersSDK);
  if (InterfaceContext)
    Ctx.access = {"public", "interface", "abstract"};
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Serializable = klass("Ljava/io/Serializable;", true);
  Serializable.access = {"public", "interface", "abstract"};
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = Throwable.name;
  auto IO = klass("Ljava/io/IOException;", true);
  IO.superclass = Exception.name;
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = Exception.name;
  return {Provider,
          Core,
          Carrier,
          Callback,
          Ctx,
          Root,
          Serializable,
          Throwable,
          Exception,
          IO,
          Runtime,
          klass("Ljava/lang/String;", true),
          klass("Ljava/lang/Number;", true)};
}
ClassMap closedCarrierProducerInput(std::vector<Class> Input, unsigned Route,
                                    Budget &B) {
  if (Route == 2)
    std::reverse(Input.begin(), Input.end());
  if (Route)
    return linkClasses(Input, B);
  ClassMap Out;
  for (auto &C : Input)
    Out.emplace(C.name, std::move(C));
  return Out;
}
} // namespace

TEST(MobileDalvikProducerThrows,
     ClosedContextsRetainProducerScopeOriginalDispatchAndClosedExceptions) {
  for (bool SDK : {false, true})
    for (bool CarriersSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Static : {false, true})
            for (bool InterfaceContext : {false, true})
              for (unsigned Throws : {0u, 1u, 2u})
                for (unsigned Position : {0u, 1u, 2u})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, CarriersSDK, Range, Alpha, Static,
                                   InterfaceContext, Throws, Position, Route}));
                    ASSERT_NO_THROW(([&] {
                      Budget B;
                      auto Classes = closedCarrierProducerInput(
                          closedCarrierProducerFixture(
                              SDK, CarriersSDK, Range, Alpha, Static,
                              InterfaceContext, Throws, Position),
                          Route, B);
                      const auto Original =
                          Classes.at("Lfixture/Provider;").methods[0];
                      const auto Plans = validateGenericSignatures(Classes, B);
                      EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                          Original.reference));
                      if (Throws)
                        EXPECT_TRUE(
                            validatedClosedDeclaredThrows(Original, Plans));
                      const auto R = recoverJava(Classes, B);
                      EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                      EXPECT_FALSE(R.getArray("generic_signature_projections"));
                      const auto &After =
                          Classes.at(Original.reference.owner).methods[0];
                      EXPECT_EQ(After.reference, Original.reference);
                      EXPECT_EQ(After.access, Original.access);
                      EXPECT_EQ(After.generic_signature,
                                Original.generic_signature);
                      EXPECT_EQ(After.declared_throws,
                                Original.declared_throws);
                    })());
                  }
}

TEST(MobileDalvikProducerThrows,
     ClosedContextsRejectUnboundProducersVarianceIncompleteTypesAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 40; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Input = closedCarrierProducerFixture(SDK, true, false, false,
                                                  false, false, 2, 0);
        auto &Target = Input[0].methods[0], &Caller = Input[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &Sig = *Target.generic_signature;
          auto At = Sig.find(From);
          ASSERT_NE(At, std::string::npos);
          Sig.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Labels<Ljava/lang/String;>", "Labels<TE;>");
          break;
        case 1:
          Replace("Labels<Ljava/lang/String;>", "Labels<+Ljava/lang/String;>");
          break;
        case 2:
          Replace("Labels<Ljava/lang/String;>", "Labels<-Ljava/lang/String;>");
          break;
        case 3:
          Replace("Labels<Ljava/lang/String;>", "Labels<*>");
          break;
        case 4:
          Replace("Labels<Ljava/lang/String;>", "Labels");
          break;
        case 5:
          Replace("Producer<TE;>", "Producer<+TE;>");
          break;
        case 6:
          Replace("Producer<TE;>", "Producer<-TE;>");
          break;
        case 7:
          Replace("Producer<TE;>", "Producer<*>");
          break;
        case 8:
          Replace("Producer<TE;>", "Producer");
          break;
        case 9:
          Replace("Producer<TE;>", "Producer<TMissing;>");
          break;
        case 10:
          Replace(")TE;", ")Ljava/lang/Object;");
          break;
        case 11:
          Replace(")TE;", ")TMissing;");
          break;
        case 12:
          Replace("E:Ljava/lang/Object;", "E:Ljava/lang/Number;");
          break;
        case 13:
          Input[3].generic_signature.reset();
          break;
        case 14:
          Input[3].access.erase("public");
          break;
        case 15:
          Input[3].generic_signature = "<Value:Ljava/lang/Number;>Ljava/lang/"
                                       "Object;Ljava/io/Serializable;";
          break;
        case 16:
          Input[2].access.erase("public");
          break;
        case 17:
          Input[2].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 18:
          Input.erase(Input.begin() + 11);
          break;
        case 19:
          Input[4].generic_signature =
              "<Unused:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 20:
          Input.erase(Input.begin() + 4);
          break;
        case 21:
          Target.declared_throws = {"Lfixture/Missing;"};
          break;
        case 22:
          Target.declared_throws = {"Ljava/lang/String;"};
          break;
        case 23:
          Target.declared_throws = {"Ljava/io/IOException;",
                                    "Ljava/io/IOException;"};
          break;
        case 24:
          Target.generic_signature = *Target.generic_signature + "^TE;";
          break;
        case 25:
          Target.access.insert("static");
          break;
        case 26:
          Target.access.insert("varargs");
          break;
        case 27:
          Target.access.insert("native");
          break;
        case 28:
          Target.access.insert("abstract");
          break;
        case 29:
          Target.access.insert("bridge");
          break;
        case 30:
          Target.access.insert("synthetic");
          break;
        case 31:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 32:
          Caller.instructions[0].registers.pop_back();
          break;
        case 33:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 34:
          Caller.instructions[1].opcode = "move-result";
          break;
        case 35:
          Replace("<E:", "<Unused:Ljava/lang/Object;E:");
          break;
        case 36:
          Input[0].access.insert("interface");
          break;
        case 37:
          Replace("Labels<Ljava/lang/String;>", "Labels<Lfixture/Producer;>");
          break;
        case 38:
          Input[0].generic_signature =
              "<E:Ljava/lang/Object;>Ljava/lang/Object;Ljava/io/Serializable;";
          break;
        case 39:
          Input[9].access.insert("interface");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(closedCarrierProducerInput(Input, Route, B),
                                   B);
                     }()),
                     std::runtime_error);
      }
}

// Instance producer results with fixed reifiable varargs arrays.
namespace {
std::vector<Class> instanceProducerVarargsFixture(bool SDK, bool CarriersSDK,
                                                  bool Range, bool Alpha,
                                                  bool Varargs, unsigned Throws,
                                                  unsigned Array) {
  auto Input = closedCarrierProducerFixture(SDK, CarriersSDK, Range, Alpha,
                                            false, false, Throws, 1);
  auto &Target = Input[0].methods[0], &Caller = Input[1].methods[0];
  const std::string E = Alpha ? "Element" : "E";
  const std::vector<std::string> Erasures{"[Ljava/lang/String;",
                                          "[[Lfixture/Context;", "[J",
                                          "[Lfixture/Producer;"};
  const std::vector<std::string> Types{"[Ljava/lang/String;",
                                       "[[Lfixture/Context;", "[J",
                                       "[Lfixture/Producer<*>;"};
  Target.reference.parameters.back() = Erasures[Array];
  Target.generic_signature = "<" + E +
                             ":Ljava/lang/Object;>(Lfixture/Context;"
                             "Lfixture/Producer<T" +
                             E + ";>;" + Types[Array] + ")T" + E + ";";
  if (Varargs)
    Target.access.insert("varargs");
  Caller.reference.parameters.back() = Erasures[Array];
  Caller.instructions[0].reference = Target.reference;
  return Input;
}
} // namespace

TEST(MobileDalvikProducerThrows,
     InstanceVarargsRetainReifiableArraysProducerScopeDispatchAndClosedThrows) {
  for (bool SDK : {false, true})
    for (bool CarriersSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Varargs : {false, true})
            for (unsigned Throws : {0u, 1u, 2u})
              for (unsigned Array : {0u, 1u, 2u, 3u})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, CarriersSDK, Range, Alpha, Varargs,
                                 Throws, Array, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = closedCarrierProducerInput(
                        instanceProducerVarargsFixture(SDK, CarriersSDK, Range,
                                                       Alpha, Varargs, Throws,
                                                       Array),
                        Route, B);
                    const auto Original =
                        Classes.at("Lfixture/Provider;").methods[0];
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                        Original.reference));
                    if (Throws)
                      EXPECT_TRUE(
                          validatedClosedDeclaredThrows(Original, Plans));
                    const auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    const auto &After =
                        Classes.at(Original.reference.owner).methods[0];
                    EXPECT_EQ(After.reference, Original.reference);
                    EXPECT_EQ(After.access, Original.access);
                    EXPECT_EQ(After.generic_signature,
                              Original.generic_signature);
                    EXPECT_EQ(After.declared_throws, Original.declared_throws);
                  })());
                }
}

TEST(MobileDalvikProducerThrows,
     InstanceVarargsPreserveOpaqueArraysAndRejectDependentContractsAndFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 37; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Input =
            instanceProducerVarargsFixture(SDK, true, false, false, true, 2, 0);
        auto &Target = Input[0].methods[0], &Caller = Input[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &Sig = *Target.generic_signature;
          auto At = Sig.find(From);
          ASSERT_NE(At, std::string::npos);
          Sig.replace(At, From.size(), To);
        };
        auto Tail = [&](const std::string &Signature,
                        const std::string &Descriptor) {
          Replace("[Ljava/lang/String;", Signature);
          Target.reference.parameters.back() = Descriptor;
          Caller.reference.parameters.back() = Descriptor;
          Caller.instructions[0].reference = Target.reference;
        };
        switch (Mutation) {
        case 0:
          Tail("Ljava/lang/String;", "Ljava/lang/String;");
          break;
        case 1:
          Tail("[Lfixture/Producer<TE;>;", "[Lfixture/Producer;");
          break;
        case 2:
          Tail("[TE;", "[Ljava/lang/Object;");
          break;
        case 3:
          Tail("[Lfixture/Missing;", "[Lfixture/Missing;");
          break;
        case 4:
          Tail("[V", "[V");
          break;
        case 5:
          Input.erase(Input.begin() + 11);
          break;
        case 6:
          Input.erase(Input.begin() + 4);
          break;
        case 7:
          Input[3].generic_signature.reset();
          break;
        case 8:
          Input[3].access.erase("public");
          break;
        case 9:
          Input[3].generic_signature = "<Value:Ljava/lang/Number;>Ljava/lang/"
                                       "Object;Ljava/io/Serializable;";
          break;
        case 10:
          Input[3].generic_signature =
              "<Value:Ljava/lang/Object;Other:Ljava/lang/Object;>Ljava/lang/"
              "Object;Ljava/io/Serializable;";
          break;
        case 11:
          Replace("Producer<TE;>", "Producer<+TE;>");
          break;
        case 12:
          Replace("Producer<TE;>", "Producer<-TE;>");
          break;
        case 13:
          Replace("Producer<TE;>", "Producer<*>");
          break;
        case 14:
          Replace("Producer<TE;>", "Producer");
          break;
        case 15:
          Replace("Producer<TE;>", "Producer<TMissing;>");
          break;
        case 16:
          Replace(")TE;", ")Ljava/lang/Object;");
          break;
        case 17:
          Replace("E:Ljava/lang/Object;", "E:Ljava/lang/Number;");
          break;
        case 18:
          Replace("<E:", "<Unused:Ljava/lang/Object;E:");
          break;
        case 19:
          Target.declared_throws = {"Lfixture/Missing;"};
          break;
        case 20:
          Target.declared_throws = {"Ljava/lang/String;"};
          break;
        case 21:
          Target.declared_throws = {"Ljava/io/IOException;",
                                    "Ljava/io/IOException;"};
          break;
        case 22:
          Target.generic_signature = *Target.generic_signature + "^TE;";
          break;
        case 23:
          Target.access.insert("static");
          break;
        case 24:
          Target.access.insert("native");
          break;
        case 25:
          Target.access.insert("abstract");
          break;
        case 26:
          Target.access.insert("bridge");
          break;
        case 27:
          Target.access.insert("synthetic");
          break;
        case 28:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 29:
          Input[0].generic_signature =
              "<E:Ljava/lang/Object;>Ljava/lang/Object;Ljava/io/Serializable;";
          break;
        case 30:
          Input[0].access.insert("interface");
          break;
        case 31:
          Caller.instructions[0].registers.pop_back();
          break;
        case 32:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 33:
          Caller.instructions[0].registers[0] = 2;
          break;
        case 34:
          Caller.instructions[1].opcode = "move-result";
          break;
        case 35:
          Input[9].access.insert("interface");
          break;
        case 36:
          Input[4].generic_signature =
              "<Unused:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        }
        if (Mutation == 3 || Mutation == 5) {
          ASSERT_NO_THROW(([&] {
            Budget B;
            auto Classes = closedCarrierProducerInput(Input, Route, B);
            const auto Original = Classes.at("Lfixture/Provider;").methods[0];
            const auto Plans = validateGenericSignatures(Classes, B);
            EXPECT_TRUE(
                Plans.erased_generic_invocations.contains(Original.reference));
            EXPECT_TRUE(validatedClosedDeclaredThrows(Original, Plans));
            const auto R = recoverJava(Classes, B);
            EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
            EXPECT_FALSE(R.getArray("generic_signature_projections"));
            const auto &After = Classes.at(Original.reference.owner).methods[0];
            EXPECT_EQ(After.reference, Original.reference);
            EXPECT_EQ(After.generic_signature, Original.generic_signature);
            EXPECT_EQ(After.declared_throws, Original.declared_throws);
            EXPECT_EQ(After.access, Original.access);
          })());
        } else {
          EXPECT_THROW(([&] {
                         Budget B;
                         recoverJava(
                             closedCarrierProducerInput(Input, Route, B), B);
                       }()),
                       std::runtime_error);
        }
      }
}

// Closed context slots share the nominal declaration and bound authority.
namespace {
std::vector<Class> closedContextProducerFixture(
    bool SDK, bool CarriersSDK, bool Range, bool Alpha, bool Static,
    bool InterfaceContext, unsigned Throws, unsigned Position, unsigned Slots) {
  auto Input =
      closedCarrierProducerFixture(SDK, CarriersSDK, Range, Alpha, Static,
                                   InterfaceContext, Throws, Position);
  const std::vector<std::string> Leaves{
      "Ljava/lang/String;", "Lfixture/Context;", "Ljava/lang/Number;"};
  std::string Closed = "Lfixture/Labels<", Header = "<";
  for (unsigned I = 0; I < Slots; ++I) {
    Closed += Leaves[I];
    Header +=
        (Alpha ? "Slot" : "V") + std::to_string(I) + ":Ljava/lang/Object;";
  }
  Closed += ">;";
  Header += ">Ljava/lang/Object;";
  auto &Sig = *Input[0].methods[0].generic_signature;
  const std::string Before = "Lfixture/Labels<Ljava/lang/String;>;";
  Sig.replace(Sig.find(Before), Before.size(), Closed);
  Input[2].generic_signature = Header;
  return Input;
}
} // namespace

TEST(MobileDalvikProducerThrows,
     ClosedContextSlotsRetainDeclaredArityScopesDispatchAndCheckedExceptions) {
  for (bool SDK : {false, true})
    for (bool CarriersSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Static : {false, true})
            for (bool InterfaceContext : {false, true})
              for (unsigned Throws : {0u, 1u, 2u})
                for (unsigned Position : {0u, 1u, 2u})
                  for (unsigned Slots : {1u, 2u, 3u})
                    for (unsigned Route : {0u, 1u, 2u}) {
                      SCOPED_TRACE(::testing::PrintToString(std::tuple{
                          SDK, CarriersSDK, Range, Alpha, Static,
                          InterfaceContext, Throws, Position, Slots, Route}));
                      ASSERT_NO_THROW(([&] {
                        Budget B;
                        auto Classes = closedCarrierProducerInput(
                            closedContextProducerFixture(
                                SDK, CarriersSDK, Range, Alpha, Static,
                                InterfaceContext, Throws, Position, Slots),
                            Route, B);
                        const auto Original =
                            Classes.at("Lfixture/Provider;").methods[0];
                        const auto Plans =
                            validateGenericSignatures(Classes, B);
                        EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                            Original.reference));
                        if (Throws)
                          EXPECT_TRUE(
                              validatedClosedDeclaredThrows(Original, Plans));
                        const auto R = recoverJava(Classes, B);
                        EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                        EXPECT_FALSE(
                            R.getArray("generic_signature_projections"));
                        const auto &After =
                            Classes.at(Original.reference.owner).methods[0];
                        EXPECT_EQ(After.reference, Original.reference);
                        EXPECT_EQ(After.access, Original.access);
                        EXPECT_EQ(After.generic_signature,
                                  Original.generic_signature);
                        EXPECT_EQ(After.declared_throws,
                                  Original.declared_throws);
                      })());
                    }
}

TEST(MobileDalvikProducerThrows,
     ClosedContextSlotsRejectForeignVariablesArityBoundsVarianceAndFrames) {
  for (bool SDK : {false, true})
    for (bool Static : {false, true})
      for (unsigned Mutation = 0; Mutation < 44; ++Mutation)
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{SDK, Static, Mutation, Route}));
          auto Input = closedContextProducerFixture(SDK, true, false, false,
                                                    Static, false, 2, 0, 2);
          auto &Target = Input[0].methods[0], &Caller = Input[1].methods[0];
          auto Replace = [&](const std::string &From, const std::string &To) {
            auto &Sig = *Target.generic_signature;
            auto At = Sig.find(From);
            ASSERT_NE(At, std::string::npos);
            Sig.replace(At, From.size(), To);
          };
          const std::string Closed =
              "Labels<Ljava/lang/String;Lfixture/Context;>";
          switch (Mutation) {
          case 0:
            Replace(Closed, "Labels<Ljava/lang/String;TE;>");
            break;
          case 1:
            Replace(Closed, "Labels<+Ljava/lang/String;Lfixture/Context;>");
            break;
          case 2:
            Replace(Closed, "Labels<Ljava/lang/String;-Lfixture/Context;>");
            break;
          case 3:
            Replace(Closed, "Labels<*Lfixture/Context;>");
            break;
          case 4:
            Replace(Closed, "Labels");
            break;
          case 5:
            Replace("Producer<TE;>", "Producer<+TE;>");
            break;
          case 6:
            Replace("Producer<TE;>", "Producer<-TE;>");
            break;
          case 7:
            Replace("Producer<TE;>", "Producer<*>");
            break;
          case 8:
            Replace("Producer<TE;>", "Producer");
            break;
          case 9:
            Replace("Producer<TE;>", "Producer<TMissing;>");
            break;
          case 10:
            Replace(")TE;", ")TMissing;");
            break;
          case 11:
            Replace(Closed, "Labels<Ljava/lang/String;TMissing;>");
            break;
          case 12:
            Replace("E:Ljava/lang/Object;", "E:Ljava/lang/Number;");
            break;
          case 13:
            Input[3].generic_signature.reset();
            break;
          case 14:
            Input[3].access.erase("public");
            break;
          case 15:
            Input[3].generic_signature = "<Value:Ljava/lang/Number;>Ljava/lang/"
                                         "Object;Ljava/io/Serializable;";
            break;
          case 16:
            Input[2].access.erase("public");
            break;
          case 17:
            Input[2].generic_signature =
                "<V0:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 18:
            Input.erase(Input.begin() + 11);
            break;
          case 19:
            Input[4].generic_signature =
                "<Unused:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 20:
            Input.erase(Input.begin() + 4);
            break;
          case 21:
            Target.declared_throws = {"Lfixture/Missing;"};
            break;
          case 22:
            Target.declared_throws = {"Ljava/lang/String;"};
            break;
          case 23:
            Target.declared_throws = {"Ljava/io/IOException;",
                                      "Ljava/io/IOException;"};
            break;
          case 24:
            Target.generic_signature = *Target.generic_signature + "^TE;";
            break;
          case 25:
            if (Static)
              Target.access.erase("static");
            else
              Target.access.insert("static");
            break;
          case 26:
            Target.access.insert("varargs");
            break;
          case 27:
            Target.access.insert("native");
            break;
          case 28:
            Target.access.insert("abstract");
            break;
          case 29:
            Target.access.insert("bridge");
            break;
          case 30:
            Target.access.insert("synthetic");
            break;
          case 31:
            Target.access.erase("public");
            Target.access.insert("private");
            break;
          case 32:
            Caller.instructions[0].registers.pop_back();
            break;
          case 33:
            Caller.instructions[0].registers.back() = Caller.registers;
            break;
          case 34:
            Caller.instructions[1].opcode = "move-result";
            break;
          case 35:
            Replace("<E:", "<Unused:Ljava/lang/Object;E:");
            break;
          case 36:
            Input[0].access.insert("interface");
            break;
          case 37:
            Replace(Closed, "Labels<Lfixture/Producer<*>;Lfixture/Context;>");
            break;
          case 38:
            Input[0].generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/"
                                         "Object;Ljava/io/Serializable;";
            break;
          case 39:
            Input[9].access.insert("interface");
            break;
          case 40:
            Input[2].generic_signature = "<V0:Ljava/lang/Number;V1:Ljava/lang/"
                                         "Object;>Ljava/lang/Object;";
            break;
          case 41:
            Input[2].generic_signature = "<V0:Ljava/lang/Object;V1:Ljava/lang/"
                                         "Number;>Ljava/lang/Object;";
            break;
          case 42:
            Input[2].generic_signature =
                "<V0:Ljava/lang/Object;V1:Ljava/lang/Object;V2:Ljava/lang/"
                "Object;>Ljava/lang/Object;";
            break;
          case 43:
            Replace(Closed, "Labels<Ljava/lang/String;*>");
            break;
          }
          EXPECT_THROW(([&] {
                         Budget B;
                         recoverJava(
                             closedCarrierProducerInput(Input, Route, B), B);
                       }()),
                       std::runtime_error);
        }
}

// Interface producers retain method-owned invariant bindings and closed throws.
namespace {
std::vector<Class> interfaceTokenResultThrowsFixture(bool SDK, bool Range,
                                                     bool Alpha, bool Alias,
                                                     bool Context,
                                                     bool CallerFormal,
                                                     unsigned Throws) {
  auto Input = closedCarrierProducerFixture(SDK, true, Range, Alpha, false,
                                            false, Throws, 0);
  auto &Owner = Input[0];
  Owner.access = {"public", "interface", "abstract"};
  auto &Target = Owner.methods[0];
  Target.access = {"public", "abstract"};
  Target.registers = 0;
  Target.instructions.clear();
  Target.code_end = 0;
  Target.reference.name = "fetch";
  Target.reference.parameters = {"Ljava/lang/Class;"};
  const std::string E = Alpha ? "Element" : "E";
  std::string Params = "Ljava/lang/Class<T" + E + ";>;";
  if (Context) {
    Target.reference.parameters = {"Lfixture/Context;", "Ljava/lang/Class;",
                                   "J"};
    Params = "Lfixture/Context;" + Params + "J";
  }
  Target.generic_signature =
      "<" + E + ":Ljava/lang/Object;>(" + Params + ")T" + E + ";";
  auto Derived = klass("Lfixture/DerivedApi;", SDK);
  Derived.access = {"public", "interface", "abstract"};
  Derived.interfaces = {Owner.name};
  auto &Caller = Input[1].methods[0];
  Caller.reference.parameters = Target.reference.parameters;
  Caller.reference.parameters.insert(Caller.reference.parameters.begin(),
                                     Alias ? Derived.name : Owner.name);
  Caller.registers = Context ? 6 : 3;
  std::vector<unsigned> Args;
  for (unsigned N = 1; N < Caller.registers; ++N)
    Args.push_back(N);
  auto Reference = Target.reference;
  if (Alias)
    Reference.owner = Derived.name;
  Caller.instructions[0] =
      op(0, Range ? "invoke-interface/range" : "invoke-interface", Args,
         Reference);
  if (CallerFormal) {
    std::string Types = Alias ? Derived.name : Owner.name;
    if (Context)
      Types += "Lfixture/Context;";
    Types += "Ljava/lang/Class<TValue;>;";
    if (Context)
      Types += "J";
    Caller.generic_signature =
        "<Value:Ljava/lang/Object;>(" + Types + ")TValue;";
  }
  auto Token = klass("Ljava/lang/Class;", true);
  Token.access.insert("final");
  Token.generic_signature = "<Token:Ljava/lang/Object;>Ljava/lang/Object;";
  Input.push_back(Token);
  Input.push_back(Derived);
  return Input;
}
} // namespace

TEST(MobileDalvikProducerThrows,
     InterfaceTokensRetainScopedResultsClosedExceptionsAndInheritedDispatch) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Alias : {false, true})
          for (bool Context : {false, true})
            for (bool CallerFormal : {false, true})
              for (unsigned Throws : {0u, 1u, 2u})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, Range, Alpha, Alias, Context,
                                 CallerFormal, Throws, Route}));
                  ASSERT_NO_THROW(([&] {
                    Budget B;
                    auto Classes = closedCarrierProducerInput(
                        interfaceTokenResultThrowsFixture(SDK, Range, Alpha,
                                                          Alias, Context,
                                                          CallerFormal, Throws),
                        Route, B);
                    const auto Original =
                        Classes.at("Lfixture/Provider;").methods[0];
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(Plans.erased_generic_invocations.contains(
                        Original.reference));
                    if (Throws)
                      EXPECT_TRUE(
                          validatedClosedDeclaredThrows(Original, Plans));
                    const auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    const auto &After =
                        Classes.at(Original.reference.owner).methods[0];
                    EXPECT_EQ(After.reference, Original.reference);
                    EXPECT_EQ(After.access, Original.access);
                    EXPECT_EQ(After.generic_signature,
                              Original.generic_signature);
                    EXPECT_EQ(After.declared_throws, Original.declared_throws);
                    EXPECT_TRUE(After.instructions.empty());
                    EXPECT_EQ(After.registers, 0);
                    EXPECT_EQ(After.code_end, 0);
                  })());
                }
}

TEST(MobileDalvikProducerThrows,
     InterfaceTokensRejectIncompleteBindingsExceptionProofsRolesAndCallWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 30; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Input = interfaceTokenResultThrowsFixture(SDK, false, false, true,
                                                       false, true, 2);
        auto &Target = Input[0].methods[0], &Caller = Input[1].methods[0];
        auto Replace = [&](const std::string &From, const std::string &To) {
          auto &Sig = *Target.generic_signature;
          auto At = Sig.find(From);
          ASSERT_NE(At, std::string::npos);
          Sig.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace("Class<TE;>", "Class<+TE;>");
          break;
        case 1:
          Replace("Class<TE;>", "Class<-TE;>");
          break;
        case 2:
          Replace("Class<TE;>", "Class<*>");
          break;
        case 3:
          Replace("Class<TE;>", "Class");
          break;
        case 4:
          Replace("Class<TE;>", "Class<TMissing;>");
          break;
        case 5:
          Replace(")TE;", ")Ljava/lang/Object;");
          break;
        case 6:
          Replace("E:Ljava/lang/Object;", "E:Ljava/lang/Number;");
          break;
        case 7:
          Replace("<E:", "<Unused:Ljava/lang/Object;E:");
          break;
        case 8:
          Target.declared_throws = {"Lfixture/Missing;"};
          break;
        case 9:
          Target.declared_throws = {"Ljava/lang/String;"};
          break;
        case 10:
          Target.declared_throws = {"Ljava/io/IOException;",
                                    "Ljava/io/IOException;"};
          break;
        case 11:
          *Target.generic_signature += "^TE;";
          break;
        case 12:
          Target.access.insert("static");
          break;
        case 13:
          Target.access.insert("native");
          break;
        case 14:
          Target.access.erase("abstract");
          break;
        case 15:
          Target.access.insert("final");
          break;
        case 16:
          Target.access.insert("bridge");
          break;
        case 17:
          Target.access.insert("synthetic");
          break;
        case 18:
          Target.access.insert("varargs");
          break;
        case 19:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 20:
          Input[0].generic_signature =
              "<E:Ljava/lang/Object;>Ljava/lang/Object;Ljava/io/Serializable;";
          break;
        case 21:
          Input[0].access.erase("interface");
          break;
        case 22:
          Input[13].generic_signature =
              "<Token:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 23:
          Target.instructions = {op(0, "return-object", {0})};
          Target.registers = 1;
          Target.code_end = 1;
          break;
        case 24:
          Caller.instructions[0].registers.pop_back();
          break;
        case 25:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 26:
          Caller.instructions[0].opcode = "invoke-virtual";
          break;
        case 27:
          Caller.instructions[1].opcode = "move-result";
          break;
        case 28:
          Input[9].access.insert("interface");
          break;
        case 29:
          Input[14].interfaces = {Input[14].name};
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(closedCarrierProducerInput(Input, Route, B),
                                   B);
                     }()),
                     std::runtime_error);
      }
}

TEST(MobileDalvikProducerThrows, CheckedConstructorsBindTheirOriginalErasedOperand) {
  for (bool Factory : {false, true}) {
    SCOPED_TRACE(Factory);
    const std::string Object = "Ljava/lang/Object;";
    auto Root = klass(Object, true);
    Root.superclass.reset();
    auto Throwable = klass("Ljava/lang/Throwable;", true);
    auto Exception = klass("Ljava/lang/Exception;", true);
    Exception.superclass = Throwable.name;
    auto Failure = klass("Lfixture/Failure;", true);
    Failure.superclass = Exception.name;
    auto Box = klass("Lfixture/Box;", true);
    Box.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
    auto Element = klass("Lfixture/Element;", true);
    auto Base = klass("Lfixture/Base;", true);
    auto Parent = method({Base.name, "<init>", {Box.name}, "V"},
                         {"public", "constructor"}, 0, {});
    Parent.generic_signature = "(Lfixture/Box<Lfixture/Element;>;)V";
    if (!Factory)
      Parent.declared_throws = {Failure.name};
    Base.methods.push_back(Parent);
    auto Child = klass("Lfixture/Child;");
    Child.superclass = Base.name;
    const auto InputType = Factory ? Element.name : Box.name;
    auto Init = method({Child.name, "<init>", {InputType}, "V"},
                       {"public", "constructor"}, 2, {});
    Init.declared_throws = {Failure.name};
    if (Factory) {
      auto Build = method({Child.name, "prepare", {InputType}, Box.name},
                          {"private", "static"}, 2,
                          {op(0, "const/4", {0}), op(1, "return-object", {0})});
      Build.instructions.front().literal = int64_t(0);
      Build.declared_throws = {Failure.name};
      Init.instructions = {op(0, "invoke-static", {1}, Build.reference),
                           op(1, "move-result-object", {1})};
      Child.methods.push_back(Build);
    }
    unsigned At = Init.instructions.size();
    Init.instructions.push_back(op(At++, "invoke-direct", {0, 1}, Parent.reference));
    Init.instructions.push_back(op(At++, "return-void"));
    Init.code_end = At;
    Child.methods.push_back(Init);
    auto Input = std::vector<Class>{Child, Base, Box, Element, Root, Throwable, Exception, Failure};
    Budget B;
    auto Report = recoverJava(linkClasses(Input, B), B);
    ASSERT_EQ(Report.getInteger("unrecovered_method_count"), 0);
    const auto *Units = Report.getArray("source_units");
    ASSERT_TRUE(Units && Units->size() == 1);
    const auto Source = Units->front().getAsObject()->getString("source");
    ASSERT_TRUE(Source);
    EXPECT_NE(Source->find("super("), llvm::StringRef::npos);
    EXPECT_NE(Source->find("throws fixture.Failure"), llvm::StringRef::npos);
    if (Factory)
      EXPECT_NE(Source->find("prepare("), llvm::StringRef::npos);
    // A sibling checked exception cannot escape through the original declaration.
    auto Other = klass("Lfixture/Other;", true);
    Other.superclass = Exception.name;
    Input.push_back(Other);
    (Factory ? Input[0].methods.front() : Input[1].methods.front()).declared_throws = {Other.name};
    Budget Negative;
    EXPECT_THROW(recoverJava(linkClasses(Input, Negative), Negative), std::runtime_error);
  }
}
