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
std::vector<Class> fixture(bool SDK, bool CarrierSDK, bool Range, bool Alpha,
                           bool First, bool Covariant) {
  const std::string Object = "Ljava/lang/Object;", A = Alpha ? "Item" : "P",
                    B = Alpha ? "Result" : "R", Input = "Lfixture/Input;",
                    Output = "Lfixture/Output;", Mapper = "Lfixture/Mapper;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  const std::string
      Pair = "Lfixture/Input<" + std::string(Covariant ? "+" : "") + "T" + A +
             ";" + (Covariant ? "+" : "") + "T" + B + ";>;",
      Context =
          "Lfixture/Mapper<Ljava/lang/RuntimeException;Ljava/lang/Exception;>;";
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  MethodRef Selected{Provider.name, "select",
                     First ? std::vector<std::string>{Input, Mapper}
                           : std::vector<std::string>{Mapper, Input},
                     Output};
  auto Target =
      method(Selected, {"public", "static"}, SDK ? 0 : 3,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  Target.generic_signature = "<" + A + ":Ljava/lang/Object;" + B +
                             ":Ljava/lang/Object;>(" +
                             (First ? Pair + Context : Context + Pair) +
                             ")Lfixture/Output<T" + A + ";T" + B + ";>;";
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  Client.methods.push_back(method(
      {Client.name, "call", Selected.parameters, Output}, {"public", "static"},
      3,
      {op(0, Range ? "invoke-static/range" : "invoke-static", {1, 2}, Selected),
       op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto In = klass(Input, CarrierSDK);
  In.access = {"public", "abstract", "interface"};
  In.interfaces = {"Ljava/io/Serializable;"};
  In.generic_signature = "<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/lang/"
                         "Object;Ljava/io/Serializable;";
  auto Apply =
      method({Input, "apply", {Object}, Object}, {"public", "abstract"}, 0, {});
  Apply.generic_signature = "(TX;)TY;";
  Apply.declared_throws = std::vector<std::string>{"Ljava/lang/Exception;"};
  In.methods.push_back(Apply);
  auto Out = klass(Output, CarrierSDK);
  Out.access = In.access;
  Out.interfaces = {Input};
  Out.generic_signature = "<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/"
                          "lang/Object;Lfixture/Input<TX;TY;>;";
  Apply.reference.owner = Output;
  Apply.declared_throws =
      std::vector<std::string>{"Ljava/lang/RuntimeException;"};
  Out.methods.push_back(Apply);
  auto Map = klass(Mapper, CarrierSDK);
  Map.access = In.access;
  Map.generic_signature =
      "<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/lang/Object;";
  auto MapApply = method({Mapper, "apply", {Object}, Object},
                         {"public", "abstract"}, 0, {});
  MapApply.generic_signature = "(TY;)TX;";
  Map.methods.push_back(MapApply);
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Throwable = klass("Ljava/lang/Throwable;", true),
       Exception = klass("Ljava/lang/Exception;", true),
       Runtime = klass("Ljava/lang/RuntimeException;", true);
  Exception.superclass = Throwable.name;
  Runtime.superclass = Exception.name;
  auto Serializable = klass("Ljava/io/Serializable;", true);
  Serializable.access = In.access;
  return {Client, Provider,  In,        Map,     Out,
          Root,   Throwable, Exception, Runtime, Serializable};
}
} // namespace

TEST(
    MobileDalvikBinaryClosedContext,
    RetainOrderedPairSlotsClosedBinaryArgumentsOriginalScopesAndCarrierDeclarations) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool First : {false, true})
            for (bool Covariant : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, CarrierSDK, Range, Alpha, First, Covariant, Route}));
                auto Input =
                    fixture(SDK, CarrierSDK, Range, Alpha, First, Covariant);
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
                    EXPECT_EQ(After.methods[I].declared_throws,
                              C.methods[I].declared_throws);
                  }
                }
              }
}

TEST(
    MobileDalvikBinaryClosedContext,
    RejectDependentContextsVarianceMissingDeclarationsSlotReorderingAndChangedPhysicalCalls) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, true, false, true, false);
      auto &Target = Input[1].methods[0];
      auto &Signature = *Target.generic_signature;
      auto Replace = [&](const std::string &From, const std::string &To) {
        const auto I = Signature.find(From);
        ASSERT_NE(I, std::string::npos);
        Signature.replace(I, From.size(), To);
      };
      if (Mutation == 0)
        Replace("Mapper<Ljava/lang/RuntimeException;", "Mapper<TP;");
      else if (Mutation == 1)
        Replace("Mapper<Ljava/lang/RuntimeException;",
                "Mapper<+Ljava/lang/RuntimeException;");
      else if (Mutation == 2)
        Replace("Input<TP;TR;>", "Input<TR;TP;>");
      else if (Mutation == 3)
        Replace("Output<TP;TR;>", "Output<TP;TP;>");
      else if (Mutation == 4)
        Replace("Mapper<Ljava/lang/RuntimeException;", "Mapper<TMissing;");
      else if (Mutation == 5)
        Replace("Lfixture/Mapper<", "Lfixture/Input<");
      else if (Mutation == 6)
        Input.erase(Input.begin() + 3);
      else if (Mutation == 7)
        Input[3].generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 8)
        Input[3].generic_signature =
            "<X:Ljava/lang/Exception;Y:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 9) {
        Target.access.erase("public");
        Target.access.insert("private");
      } else if (Mutation == 10)
        Target.access.erase("static");
      else if (Mutation == 11)
        Target.access.insert("native");
      else if (Mutation == 12)
        Target.access.insert("abstract");
      else if (Mutation == 13)
        Target.access.insert("varargs");
      else if (Mutation == 14)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 15)
        Input[0].methods[0].instructions[0].registers.push_back(1);
      else if (Mutation == 16)
        Replace("Output<TP;TR;>", "Output<+TP;TR;>");
      else {
        Input[1].generic_signature = "<P:Ljava/lang/Object;>Ljava/lang/Object;";
        Replace("<P:Ljava/lang/Object;R:Ljava/lang/Object;>",
                "<X:Ljava/lang/Object;R:Ljava/lang/Object;>");
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
