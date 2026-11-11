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
                           bool Throws, unsigned Context, bool Alias,
                           bool Integer) {
  const std::string T = Alpha ? "Element" : "T",
                    Interface =
                        Alias ? "Lfixture/Store;" : "Ljava/util/Collection;",
                    Arg =
                        Integer ? "Ljava/lang/Integer;" : "Ljava/lang/String;",
                    String = "Ljava/lang/String;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{Interface};
  std::string Sig = "T" + T + ";";
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
      method({Provider.name, "read", Params, Interface}, {"public", "static"},
             SDK ? 0 : Words + (Instance ? 2 : 1),
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  if (Instance)
    Target.access.erase("static");
  Target.generic_signature = "<" + T +
                             "::" + Interface.substr(0, Interface.size() - 1) +
                             "<" + Arg + ">;>(" + Sig + ")T" + T + ";";
  if (Throws)
    Target.declared_throws = std::vector<std::string>{"Lfixture/Failure;"};
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
      method({Client.name, "call", Params, Interface}, {"public", "static"},
             CallerWords + 1,
             {op(0, Opcode, Args, Target.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto Bound = klass(Interface, true);
  Bound.access = {"public", "abstract", "interface"};
  Bound.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Text = klass(String, true);
  Text.access.insert("final");
  auto Number = klass("Ljava/lang/Number;", true);
  auto Int = klass("Ljava/lang/Integer;", true);
  Int.superclass = Number.name;
  Int.access.insert("final");
  auto Root = klass("Ljava/lang/Object;", true);
  Root.superclass.reset();
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = Throwable.name;
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = Exception.name;
  auto Failure = klass("Lfixture/Failure;", true);
  Failure.superclass = Runtime.name;
  return {Client, Provider,  Bound,     Text,    Number, Int,
          Root,   Throwable, Exception, Runtime, Failure};
}
} // namespace

TEST(
    MobileDalvikParameterizedInterfaceSeed,
    RetainClosedInvariantBoundsDirectFormalScopesOriginalErasuresAndReceivers) {
  for (bool SDK : {false, true})
    for (bool Instance : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Throws : {false, true})
            for (unsigned Context : {0u, 1u, 2u})
              for (bool Alias : {false, true})
                for (bool Integer : {false, true})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, Instance, Range, Alpha, Throws, Context,
                                   Alias, Integer, Route}));
                    auto Input = fixture(SDK, Instance, Range, Alpha, Throws,
                                         Context, Alias, Integer);
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
                    const auto &Shape = Plans.methods.at(Selected);
                    const auto &Formal = Shape.type_parameters.front();
                    const auto Bound = Formal.class_bound
                                           ? *Formal.class_bound
                                           : Formal.interface_bounds.front();
                    SCOPED_TRACE(
                        "formal=" + Formal.erasure +
                        ", bound=" + Shape.types.at(Bound).erasure +
                        ", result=" + Shape.types.at(*Shape.result).erasure +
                        ", projections=" +
                        std::to_string(Shape.source_projections.size()));
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

TEST(MobileDalvikParameterizedInterfaceSeed,
     RejectOpenBoundsChangedSlotConstraintsScopesErasuresAndUncheckedClaims) {
  for (bool SDK : {false, true})
    for (bool Instance : {false, true})
      for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Instance, Mutation}));
        auto Input = fixture(SDK, Instance, true, false, true, 0, true, false);
        auto &Target = Input[1].methods[0];
        auto &Signature = *Target.generic_signature;
        auto Replace = [&](const std::string &From, const std::string &To) {
          const auto At = Signature.find(From);
          ASSERT_NE(At, std::string::npos);
          Signature.replace(At, From.size(), To);
        };
        if (Mutation == 0)
          Replace(">(TT;)", ">(TMissing;)");
        else if (Mutation == 1)
          Replace(")TT;", ")TMissing;");
        else if (Mutation == 2)
          Replace(";>;>(", ";>;T:Ljava/lang/Object;>(");
        else if (Mutation == 3)
          Input[2].generic_signature =
              "<E:Ljava/lang/Object;F:Ljava/lang/Object;>Ljava/lang/Object;";
        else if (Mutation == 4)
          Replace("<Ljava/lang/String;>", "<Lfixture/Missing;>");
        else if (Mutation == 5)
          Input[2].generic_signature =
              "<E:Ljava/lang/Number;>Ljava/lang/Object;";
        else if (Mutation == 6)
          Input[2].access.erase("interface");
        else if (Mutation == 7) {
          Target.access.erase("public");
          Target.access.insert("private");
        } else if (Mutation == 8)
          Target.access.insert("native");
        else if (Mutation == 9)
          Target.access.insert("abstract");
        else if (Mutation == 10)
          Target.access.insert("synthetic");
        else if (Mutation == 11)
          Target.access.insert("bridge");
        else if (Mutation == 12)
          Target.access.insert("varargs");
        else if (Mutation == 13)
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
        else if (Mutation == 14)
          Input[0].methods[0].instructions[0].registers.pop_back();
        else if (Mutation == 15) {
          if (Instance)
            Target.access.insert("static");
          else
            Target.access.erase("static");
        } else if (Mutation == 16)
          Replace(")TT;", ")Ljava/lang/String;");
        else
          Replace("Store<Ljava/lang/String;>", "Store<+Ljava/lang/String;>");
        Budget B;
        if (Mutation == 13 && !Instance) {
          // A complete closed static bound now retains checked exceptions.
          // The instance declaration keeps its existing unchecked-only rule.
          auto Classes = linkClasses(Input, B);
          const auto Plans = validateGenericSignatures(Classes, B);
          EXPECT_TRUE(
              Plans.erased_generic_invocations.contains(Target.reference));
          EXPECT_TRUE(validatedClosedDeclaredThrows(Target, Plans));
          EXPECT_FALSE(validatedUncheckedDeclaredThrows(Target, Plans));
          EXPECT_EQ(
              recoverJava(Classes, B).getInteger("unrecovered_method_count"),
              0);
          EXPECT_EQ(
              Classes.at(Target.reference.owner).methods[0].declared_throws,
              Target.declared_throws);
          continue;
        }
        EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
      }
}
