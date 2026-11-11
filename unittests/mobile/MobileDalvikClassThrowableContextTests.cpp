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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool SuperInput,
                           bool Invariant, bool Interface) {
  const std::string Object = "Ljava/lang/Object;",
                    Throwable = "Ljava/lang/Throwable;",
                    Input = "Lfixture/Input;", Carrier = "Lfixture/Factory;",
                    Stored = Alpha ? "Value" : "T",
                    Failure = Alpha ? "Problem" : "X";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  auto Provider = klass("Lfixture/Provider;", SDK);
  Provider.generic_signature =
      "<" + Stored + ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Null = op(0, "const/4", {0});
  Null.literal = int64_t(0);
  auto Target = method(
      {Provider.name, "get", {Carrier, Input}, Object}, {"public"}, SDK ? 0 : 4,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{Null, op(1, "return-object", {0})});
  Target.generic_signature =
      "<" + Failure + ":Ljava/lang/Throwable;>(Lfixture/Factory<" +
      (SuperInput ? std::string("-") : std::string()) + Input +
      (Invariant ? std::string() : std::string("+")) + "T" + Failure + ";>;" +
      Input + ")T" + Stored + ";^T" + Failure + ";";
  Target.declared_throws = std::vector<std::string>{Throwable};
  Provider.methods.push_back(Target);
  Provider.methods.push_back(
      method({Provider.name, "<init>", {}, "V"}, {"public", "constructor"},
             SDK ? 0 : 1,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{
                       op(0, "invoke-direct", {0}, Root.methods[0].reference),
                       op(1, "return-void")}));
  auto Core = klass("Lfixture/Core;");
  Core.generic_signature = "<Stored:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Caller =
      method({Core.name, "call", {Provider.name, Carrier, Input}, Object},
             {"public"}, 5,
             {op(0, Range ? "invoke-virtual/range" : "invoke-virtual",
                 {2, 3, 4}, Target.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})});
  Caller.generic_signature = "(Lfixture/Provider<TStored;>;Lfixture/Factory<" +
                             Input + "Ljava/lang/RuntimeException;>;" + Input +
                             ")TStored;";
  Core.methods.push_back(Caller);
  auto Factory = klass(Carrier, SDK);
  Factory.access = {"public", "interface", "abstract"};
  Factory.generic_signature =
      "<Arg:Ljava/lang/Object;Result:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Apply = method({Carrier, "apply", {Object}, Object},
                      {"public", "abstract"}, 0, {});
  Apply.generic_signature = "(TArg;)TResult;";
  Factory.methods.push_back(Apply);
  auto Key = klass(Input, true);
  if (Interface)
    Key.access = {"public", "interface", "abstract"};
  auto Thrown = klass(Throwable, true),
       Exception = klass("Ljava/lang/Exception;", true),
       Runtime = klass("Ljava/lang/RuntimeException;", true);
  Exception.superclass = Throwable;
  Runtime.superclass = Exception.name;
  return {Provider,  Core,    Factory,
          Key,       Root,    Thrown,
          Exception, Runtime, klass("Lfixture/Other;", true)};
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

TEST(MobileDalvikClassThrowableContext,
     KeepReceiverClassResultIndependentMethodThrowAndCompleteBinaryProducer) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool SuperInput : {false, true})
          for (bool Invariant : {false, true})
            for (bool Interface : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Range, Alpha, SuperInput, Invariant,
                               Interface, Route}));
                ASSERT_NO_THROW(([&] {
                  Budget B;
                  auto Classes = input(fixture(SDK, Range, Alpha, SuperInput,
                                               Invariant, Interface),
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

TEST(MobileDalvikClassThrowableContext,
     RejectScopeBorrowingBadBoundsIncompleteCarriersThrowsAndWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, false, false, false, false, false);
        auto &Provider = Classes[0];
        auto &Target = Provider.methods[0], &Caller = Classes[1].methods[0];
        switch (Mutation) {
        case 0:
          Target.generic_signature =
              "<X:Ljava/lang/Throwable;>(Lfixture/Factory<Lfixture/"
              "Input;+TMissing;>;Lfixture/Input;)TT;^TX;";
          break;
        case 1:
          Target.generic_signature =
              "<X:Ljava/lang/Throwable;>(Lfixture/Factory<Lfixture/"
              "Input;+TX;>;Lfixture/Input;)TT;^TT;";
          break;
        case 2:
          Target.generic_signature =
              "<X:Ljava/lang/Throwable;>(Lfixture/Factory<Lfixture/"
              "Input;+TX;>;Lfixture/Input;)TX;^TX;";
          break;
        case 3:
          Target.generic_signature =
              "<X:Ljava/lang/Throwable;>(Lfixture/Factory<Lfixture/"
              "Input;+TX;>;Lfixture/Input;)TMissing;^TX;";
          break;
        case 4:
          Provider.generic_signature =
              "<T:Ljava/lang/Throwable;>Ljava/lang/Object;";
          break;
        case 5:
          Target.generic_signature =
              "<X:Ljava/lang/Exception;>(Lfixture/Factory<Lfixture/"
              "Input;+TX;>;Lfixture/Input;)TT;^TX;";
          break;
        case 6:
          Target.generic_signature = "<X:Ljava/lang/Throwable;>(Lfixture/"
                                     "Factory;Lfixture/Input;)TT;^TX;";
          break;
        case 7:
          Target.generic_signature =
              "<X:Ljava/lang/Throwable;>(Lfixture/Factory<Lfixture/"
              "Input;-TX;>;Lfixture/Input;)TT;^TX;";
          break;
        case 8:
          Target.generic_signature =
              "<X:Ljava/lang/Throwable;>(Lfixture/Factory<Lfixture/"
              "Other;+TX;>;Lfixture/Input;)TT;^TX;";
          break;
        case 9:
          Target.generic_signature =
              "<X:Ljava/lang/Throwable;>(Lfixture/Factory<Lfixture/"
              "Input;+TT;>;Lfixture/Input;)TT;^TX;";
          break;
        case 10:
          Target.access.insert("static");
          break;
        case 11:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 12:
          Target.access.insert("native");
          break;
        case 13:
          Target.access.insert("abstract");
          break;
        case 14:
          Target.access.insert("varargs");
          break;
        case 15:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 16:
          Caller.instructions[0].registers.pop_back();
          break;
        case 17:
          Caller.instructions[0].registers[0] = 3;
          break;
        case 18:
          Caller.instructions[0].registers.back() = 5;
          break;
        case 19:
          Caller.instructions[1].opcode = "move-result";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
