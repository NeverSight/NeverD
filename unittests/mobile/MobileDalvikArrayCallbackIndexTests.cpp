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
std::vector<Class> fixture(bool SDK, bool CallbackSDK, bool Range, bool Alpha,
                           bool Lower, bool Varargs, unsigned Scalar) {
  const std::string Object = "Ljava/lang/Object;", Filter = "Lfixture/Filter;",
                    T = Alpha ? "Element" : "T",
                    Context = Scalar == 2 ? "J" : "I";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{Filter};
  if (Scalar)
    Params.push_back(Context);
  Params.push_back("[Ljava/lang/Object;");
  const unsigned Words = Scalar == 2 ? 4 : Scalar ? 3 : 2;
  auto Constant = op(0, "const/16", {0});
  Constant.literal = int64_t(42);
  auto Target =
      method({Provider.name, "index", Params, "I"}, {"public", "static"},
             SDK ? 0 : Words + 1,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Constant, op(2, "return", {0})});
  if (Varargs)
    Target.access.insert("varargs");
  Target.generic_signature = "<" + T + ":Ljava/lang/Object;>(Lfixture/Filter<" +
                             (Lower ? std::string("-") : "") + "T" + T + ";>;" +
                             (Scalar ? Context : "") + "[T" + T + ";)I";
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  std::vector<unsigned> Args;
  for (unsigned I = 1; I <= Words; ++I)
    Args.push_back(I);
  Client.methods.push_back(method(
      {Client.name, "call", Params, "I"}, {"public", "static"}, Words + 1,
      {op(0, Range ? "invoke-static/range" : "invoke-static", Args,
          Target.reference),
       op(3, "move-result", {0}), op(4, "return", {0})}));
  auto Callback = klass(Filter, CallbackSDK);
  Callback.access = {"public", "abstract", "interface"};
  Callback.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Test =
      method({Filter, "test", {Object}, "Z"}, {"public", "abstract"}, 0, {});
  Test.generic_signature = "(TE;)Z";
  Callback.methods.push_back(Test);
  auto Root = klass(Object, true);
  Root.superclass.reset();
  return {Client, Provider, Callback, Root};
}
} // namespace

TEST(
    MobileDalvikArrayCallbackIndex,
    RetainFormalArrayIdentityUnaryConsumerIntegerResultsAndFixedVarargsFrames) {
  for (bool SDK : {false, true})
    for (bool CallbackSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Lower : {false, true})
            for (bool Varargs : {false, true})
              for (unsigned Scalar : {0u, 1u, 2u})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, CallbackSDK, Range, Alpha, Lower, Varargs,
                                 Scalar, Route}));
                  auto Input = fixture(SDK, CallbackSDK, Range, Alpha, Lower,
                                       Varargs, Scalar);
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
                      EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                      EXPECT_EQ(After.methods[I].generic_signature,
                                C.methods[I].generic_signature);
                    }
                  }
                }
  // A concrete Object array is independent of the callback's method formal.
  // The existing erased-input proof already accepts that distinct signature.
  for (bool SDK : {false, true}) {
    auto Input = fixture(SDK, true, true, false, false, true, 1);
    auto &Signature = *Input[1].methods[0].generic_signature;
    const auto At = Signature.find("[TT;");
    ASSERT_NE(At, std::string::npos);
    Signature.replace(At, 4, "[Ljava/lang/Object;");
    const auto Selected = Input[1].methods[0].reference;
    Budget B;
    const auto Classes = linkClasses(Input, B);
    const auto Plans = validateGenericSignatures(Classes, B);
    EXPECT_TRUE(Plans.erased_generic_invocations.contains(Selected));
    const auto Report = recoverJava(Classes, B);
    EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
  }
}

TEST(
    MobileDalvikArrayCallbackIndex,
    RejectUnprovedComponentsCallbackVarianceChangedRanksVarargsTailsAndForeignScopes) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, true, false, false, true, 1);
      auto &Target = Input[1].methods[0];
      auto &Signature = *Target.generic_signature;
      auto Replace = [&](const std::string &From, const std::string &To) {
        const auto I = Signature.find(From);
        ASSERT_NE(I, std::string::npos);
        Signature.replace(I, From.size(), To);
      };
      if (Mutation == 0)
        Replace("Filter<TT;>", "Filter<+TT;>");
      else if (Mutation == 1)
        Replace("[TT;", "[TMissing;");
      else if (Mutation == 2)
        Replace("Filter<TT;>", "Filter<TMissing;>");
      else if (Mutation == 3)
        Replace("Object;>(", "Object;U:Ljava/lang/Object;>(");
      else if (Mutation == 4)
        Signature.back() = 'Z';
      else if (Mutation == 5)
        Replace("[TT;", "[[TT;");
      else if (Mutation == 6) {
        Signature = "<T:Ljava/lang/Object;>(Lfixture/Filter<TT;>;[TT;I)I";
        std::swap(Target.reference.parameters[1],
                  Target.reference.parameters[2]);
        Input[0].methods[0].reference.parameters = Target.reference.parameters;
        std::get<MethodRef>(Input[0].methods[0].instructions[0].reference) =
            Target.reference;
      } else if (Mutation == 7)
        Input[2].generic_signature =
            "<E:Ljava/lang/Object;F:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 8)
        Input[2].generic_signature = "<E:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 9)
        Input.erase(Input.begin() + 2);
      else if (Mutation == 10) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 11)
        Target.access.erase("static");
      else if (Mutation == 12)
        Target.access.insert("native");
      else if (Mutation == 13)
        Target.access.insert("abstract");
      else if (Mutation == 14)
        Target.access.insert("synthetic");
      else if (Mutation == 15)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 16)
        Input[0].methods[0].instructions[0].registers.pop_back();
      else {
        Input[1].generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
        Replace("<T:Ljava/lang/Object;>", "<U:Ljava/lang/Object;>");
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
