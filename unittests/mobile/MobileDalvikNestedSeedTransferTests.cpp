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
std::vector<Class> fixture(bool SDK, bool OuterSDK, bool InnerSDK, bool Range,
                           bool Alpha, bool Swap, bool Same, bool Closed) {
  const std::string Object = "Ljava/lang/Object;", Outer = "Lfixture/Outer;",
                    T = Alpha ? "Element" : "T";
  const std::string From = "Lfixture/Source<T" + T + ";>;",
                    To = Same ? From : "Lfixture/Target<T" + T + ";>;";
  const std::string Input = "Lfixture/Outer<" + From + ">;",
                    Output = "Lfixture/Outer<" + To + ">;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params =
      Swap ? std::vector<std::string>{Object, Outer}
           : std::vector<std::string>{Outer, Object};
  auto TargetMethod = method({Provider.name, "transfer", Params, Outer},
                             {"public", "static"}, SDK ? 0 : 3, {});
  TargetMethod.generic_signature =
      "<" + T + ":Ljava/lang/Object;>(" +
      (Swap ? "T" + T + ";" + Input : Input + "T" + T + ";") + ")" + Output;
  if (!SDK) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    TargetMethod.instructions = {Zero, op(1, "return-object", {0})};
    TargetMethod.code_end = 2;
  }
  Provider.methods.push_back(TargetMethod);
  auto Core = klass("Lfixture/Core;");
  Core.methods.push_back(
      method({Core.name, "call", Params, Outer}, {"public", "static"}, 3,
             {op(0, Range ? "invoke-static/range" : "invoke-static", {1, 2},
                 TargetMethod.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto Carrier = klass(Outer, OuterSDK);
  Carrier.access = {"public", "interface", "abstract"};
  Carrier.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Source = klass("Lfixture/Source;", InnerSDK);
  Source.generic_signature = "<A:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Target = klass("Lfixture/Target;", InnerSDK);
  Target.generic_signature = "<B:Ljava/lang/Object;>Ljava/lang/Object;";
  if (Closed) {
    Target.superclass = "Lfixture/Base;";
    Target.generic_signature = "<B:Ljava/lang/Object;>Lfixture/Base<Ljava/lang/"
                               "String;Ljava/lang/Object;>;";
  }
  auto Base = klass("Lfixture/Base;", true);
  Base.generic_signature =
      "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  Base.methods.push_back(
      method({Base.name, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  if (Closed)
    Target.methods.push_back(method(
        {Target.name, "<init>", {}, "V"}, {"public", "constructor"},
        InnerSDK ? 0 : 1,
        InnerSDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{
                       op(0, "invoke-direct", {0}, Base.methods[0].reference),
                       op(1, "return-void")}));
  return {Provider,
          Core,
          Carrier,
          Source,
          Target,
          Base,
          Root,
          klass("Ljava/lang/Number;", true),
          klass("Ljava/lang/String;", true)};
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

TEST(MobileDalvikNestedSeedTransfer,
     KeepCompleteInvariantLayersOwnSeedClosedParentsAndOpaqueResult) {
  for (bool SDK : {false, true})
    for (bool OuterSDK : {false, true})
      for (bool InnerSDK : {false, true})
        for (bool Range : {false, true})
          for (bool Alpha : {false, true})
            for (bool Swap : {false, true})
              for (bool Same : {false, true})
                for (bool Closed : {false, true})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, OuterSDK, InnerSDK, Range, Alpha, Swap,
                                   Same, Closed, Route}));
                    ASSERT_NO_THROW(([&] {
                      Budget B;
                      auto Classes =
                          input(fixture(SDK, OuterSDK, InnerSDK, Range, Alpha,
                                        Swap, Same, Closed),
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
TEST(MobileDalvikNestedSeedTransfer,
     RejectIncompleteLayersForeignScopeVarianceErasuresRolesAndWordFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 26; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes =
            fixture(SDK, true, true, false, false, false, false, true);
        auto &Target = Classes[0].methods[0], &Caller = Classes[1].methods[0];
        auto replace = [&](const std::string &From, const std::string &To) {
          auto &Sig = *Target.generic_signature;
          auto At = Sig.find(From);
          ASSERT_NE(At, std::string::npos);
          Sig.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          replace("Source<TT;>", "Source<TMissing;>");
          break;
        case 1:
          replace("(Lfixture/Outer<Lfixture/Source",
                  "(Lfixture/Outer<+Lfixture/Source");
          break;
        case 2:
          replace("Source<TT;>", "Source<+TT;>");
          break;
        case 3:
          replace(")Lfixture/Outer<Lfixture/Target",
                  ")Lfixture/Outer<+Lfixture/Target");
          break;
        case 4:
          replace("Target<TT;>", "Target<-TT;>");
          break;
        case 5:
          replace("Target<TT;>", "Target<TMissing;>");
          break;
        case 6:
          Classes[3].generic_signature =
              "<A:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 7:
          Classes[4].generic_signature =
              "<B:Ljava/lang/Number;>Lfixture/Base<Ljava/lang/String;Ljava/"
              "lang/Object;>;";
          break;
        case 8:
          Classes[2].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 9:
          Classes.erase(Classes.begin() + 3);
          break;
        case 10:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 11:
          Target.access.insert("native");
          break;
        case 12:
          Target.access.insert("abstract");
          break;
        case 13:
          Target.access.insert("bridge");
          break;
        case 14:
          Target.access.insert("synthetic");
          break;
        case 15:
          Target.access.insert("varargs");
          break;
        case 16:
          Target.declared_throws = {"Ljava/lang/Exception;"};
          break;
        case 17:
          Caller.instructions[0].registers.pop_back();
          break;
        case 18:
          Caller.instructions[0].registers.back() = Caller.registers;
          break;
        case 19:
          Caller.instructions[1].opcode = "move-result";
          break;
        case 20:
          replace(">;TT;)", ">;Ljava/lang/String;)");
          break;
        case 21:
          replace(")Lfixture/Outer<", ")Lfixture/Missing<");
          break;
        case 22:
          replace("<T:Ljava/lang/Object;>",
                  "<T:Ljava/lang/Object;U:Ljava/lang/Object;>");
          break;
        case 23:
          Caller.instructions[0].opcode = "invoke-virtual";
          break;
        case 24:
          Classes[4].generic_signature =
              "Lfixture/Base<Ljava/lang/String;Ljava/lang/Object;>;";
          break;
        case 25:
          Classes[0].generic_signature =
              "<E:Ljava/lang/Object;>Ljava/lang/Object;";
          replace(">;TT;)", ">;TE;)");
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
std::vector<Class> nestedTaskTransferClasses(bool SDK, bool Interface,
                                             bool Timed, bool Range, bool Alpha,
                                             bool Throws) {
  const std::string Object = "Ljava/lang/Object;", P = "Lfixture/Provider;",
                    In = "Lfixture/Input;", Task = "Lfixture/Task;",
                    Out = "Lfixture/Output;", Done = "Lfixture/Done;",
                    Context = "Lfixture/Context;", T = Alpha ? "Element" : "T";
  auto Provider = klass(P, SDK);
  if (Interface)
    Provider.access = {"public", "interface", "abstract"};
  std::vector<std::string> Parameters{In};
  if (Timed) {
    Parameters.push_back("J");
    Parameters.push_back(Context);
  }
  auto Target =
      method({P, "transfer", Parameters, Out},
             Interface ? Access{"public", "abstract"} : Access{"public"},
             SDK || Interface ? 0
             : Timed          ? 6
                              : 3,
             {});
  Target.generic_signature =
      "<" + T + ":Ljava/lang/Object;>(Lfixture/Input<+Lfixture/Task<T" + T +
      ";>;>;" + (Timed ? std::string("JLfixture/Context;") : std::string{}) +
      ")Lfixture/Output<Lfixture/Done<T" + T + ";>;>;";
  if (Throws)
    Target.declared_throws =
        std::vector<std::string>{"Ljava/lang/InterruptedException;"};
  if (!SDK && !Interface) {
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    Target.instructions = {Zero, op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  Provider.methods = {Target};
  auto Core = klass("Lfixture/Core;");
  auto CallerParameters = Parameters;
  CallerParameters.insert(CallerParameters.begin(), P);
  std::vector<unsigned> Arguments = Timed ? std::vector<unsigned>{1, 2, 3, 4, 5}
                                          : std::vector<unsigned>{1, 2};
  auto Caller = method(
      {Core.name, "call", CallerParameters, Out}, {"public", "static"},
      Timed ? 6 : 3,
      {op(0,
          std::string(Interface ? "invoke-interface" : "invoke-virtual") +
              (Range ? "/range" : ""),
          Arguments, Target.reference),
       op(3, "move-result-object", {0}), op(4, "return-object", {0})});
  Caller.generic_signature =
      "<Caller:Ljava/lang/Object;>(Lfixture/Provider;Lfixture/Input<+Lfixture/"
      "Task<TCaller;>;>;" +
      (Timed ? std::string("JLfixture/Context;") : std::string{}) +
      ")Lfixture/Output<Lfixture/Done<TCaller;>;>;";
  if (Throws)
    Caller.declared_throws = Target.declared_throws;
  Core.methods = {Caller};
  std::vector<Class> Result{Provider, Core};
  for (const auto &Name : {In, Task, Out, Done}) {
    auto C = klass(Name, SDK);
    C.access = {"public", "interface", "abstract"};
    C.generic_signature = "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
    Result.push_back(C);
  }
  Result.push_back(klass(Context, SDK));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  Result.push_back(Root);
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  Result.push_back(Throwable);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = Throwable.name;
  Result.push_back(Exception);
  auto Interrupted = klass("Ljava/lang/InterruptedException;", true);
  Interrupted.superclass = Exception.name;
  Result.push_back(Interrupted);
  Result.push_back(klass("Ljava/lang/Number;", true));
  return Result;
}
} // namespace

TEST(MobileDalvikNestedSeedTransfer,
     InstanceNestedTransfersKeepFourCarrierScopesAndClosedContexts) {
  for (bool SDK : {false, true})
    for (bool Interface : {false, true})
      for (bool Timed : {false, true})
        for (bool Range : {false, true})
          for (bool Alpha : {false, true})
            for (bool Throws : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Interface, Timed, Range, Alpha, Throws, Route}));
                auto Input = nestedTaskTransferClasses(SDK, Interface, Timed,
                                                       Range, Alpha, Throws);
                const auto Target = Input[0].methods[0].reference;
                Budget B;
                auto Map = input(std::move(Input), Route, B);
                const auto Plans = validateGenericSignatures(Map, B);
                EXPECT_TRUE(Plans.erased_generic_invocations.contains(Target));
                EXPECT_TRUE(
                    Plans.methods.at(Target).source_projections.empty());
                auto Result = recoverJava(Map, B);
                EXPECT_EQ(Result.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(
                    Result.getInteger("projected_method_count").value_or(0), 0);
              }
}

TEST(MobileDalvikNestedSeedTransfer,
     InstanceNestedTransfersRejectVarianceBoundsRolesAndUnboundContexts) {
  for (unsigned Mutation = 0; Mutation < 22; ++Mutation)
    for (bool SDK : {false, true})
      for (unsigned Route = 0; Route < 3; ++Route) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{Mutation, SDK, Route}));
        auto Input =
            nestedTaskTransferClasses(SDK, true, true, true, false, true);
        auto &Provider = Input[0];
        auto &Target = Provider.methods[0];
        auto &Signature = *Target.generic_signature;
        auto replace = [&](const std::string &Before,
                           const std::string &After) {
          const auto At = Signature.find(Before);
          ASSERT_NE(At, std::string::npos);
          Signature.replace(At, Before.size(), After);
        };
        if (Mutation == 0)
          replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
        else if (Mutation == 1)
          replace("Input<+", "Input<-");
        else if (Mutation == 2)
          replace("Task<TT;>", "Task<+TT;>");
        else if (Mutation == 3)
          replace("Output<Lfixture/Done", "Output<+Lfixture/Done");
        else if (Mutation == 4)
          replace("Done<TT;>", "Done<+TT;>");
        else if (Mutation == 5)
          replace("Task<TT;>", "Task<*>");
        else if (Mutation == 6)
          replace("Done<TT;>", "Done<*>");
        else if (Mutation == 7)
          replace("Task<TT;>", "Task");
        else if (Mutation == 8)
          replace("Done<TT;>", "Done");
        else if (Mutation == 9)
          Provider.generic_signature =
              "<Owner:Ljava/lang/Object;>Ljava/lang/Object;";
        else if (Mutation == 10)
          replace("Done<TT;>", "Done<TMissing;>");
        else if (Mutation == 11)
          Input[2].generic_signature.reset();
        else if (Mutation == 12)
          Input[3].generic_signature =
              "<Value:Ljava/lang/Number;>Ljava/lang/Object;";
        else if (Mutation == 13)
          Target.access.insert("static");
        else if (Mutation == 14)
          Target.access.insert("native");
        else if (Mutation == 15)
          Target.access.insert("varargs");
        else if (Mutation == 16)
          Target.access.insert("synthetic");
        else if (Mutation == 17)
          Target.reference.parameters[0] = "Lfixture/Output;";
        else if (Mutation == 18)
          replace("<T:", "<Unused:Ljava/lang/Object;T:");
        else if (Mutation == 19)
          Signature += "^TT;";
        else if (Mutation == 20)
          Input.erase(Input.begin() + 6);
        else
          Provider.access.erase("public");
        const auto TargetRef = Target.reference;
        Budget B;
        EXPECT_THROW(
            {
              auto Map = input(std::move(Input), Route, B);
              const auto Plans = validateGenericSignatures(Map, B);
              EXPECT_FALSE(
                  Plans.erased_generic_invocations.contains(TargetRef));
              recoverJava(Map, B);
            },
            std::runtime_error);
      }
}

namespace {
std::vector<Class> nestedTaskResultClasses(bool SDK, bool Interface, bool Timed,
                                           bool Range, bool Alpha,
                                           bool Throws) {
  auto Input =
      nestedTaskTransferClasses(SDK, Interface, Timed, Range, Alpha, Throws);
  auto &Target = Input[0].methods[0];
  auto &Caller = Input[1].methods[0];
  Target.reference.returns = "Ljava/lang/Object;";
  auto &Signature = *Target.generic_signature;
  Signature.replace(Signature.find(')') + 1, std::string::npos,
                    Alpha ? "TElement;" : "TT;");
  Caller.reference.returns = Target.reference.returns;
  auto &CallerSignature = *Caller.generic_signature;
  CallerSignature.replace(CallerSignature.find(')') + 1, std::string::npos,
                          "TCaller;");
  Caller.instructions[0].reference = Target.reference;
  if (Throws) {
    Target.declared_throws->push_back(
        "Ljava/util/concurrent/ExecutionException;");
    if (Timed)
      Target.declared_throws->push_back(
          "Ljava/util/concurrent/TimeoutException;");
    Caller.declared_throws = Target.declared_throws;
  }
  for (const auto &Name : {"Ljava/util/concurrent/ExecutionException;",
                           "Ljava/util/concurrent/TimeoutException;"}) {
    auto Exception = klass(Name, true);
    Exception.superclass = "Ljava/lang/Exception;";
    Input.push_back(Exception);
  }
  return Input;
}
} // namespace

TEST(MobileDalvikNestedSeedTransfer,
     NestedProducersKeepDirectOwnFormalResultsAndCheckedContexts) {
  for (bool SDK : {false, true})
    for (bool Interface : {false, true})
      for (bool Timed : {false, true})
        for (bool Range : {false, true})
          for (bool Alpha : {false, true})
            for (bool Throws : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Interface, Timed, Range, Alpha, Throws, Route}));
                auto Input = nestedTaskResultClasses(SDK, Interface, Timed,
                                                     Range, Alpha, Throws);
                const auto Target = Input[0].methods[0].reference;
                Budget B;
                auto Map = input(std::move(Input), Route, B);
                const auto Plans = validateGenericSignatures(Map, B);
                EXPECT_TRUE(Plans.erased_generic_invocations.contains(Target));
                EXPECT_TRUE(
                    Plans.methods.at(Target).source_projections.empty());
                auto Result = recoverJava(Map, B);
                EXPECT_EQ(Result.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(
                    Result.getInteger("projected_method_count").value_or(0), 0);
              }
}

TEST(MobileDalvikNestedSeedTransfer,
     NestedProducerResultsRejectLostScopesVarianceBoundsAndRoles) {
  for (unsigned Mutation = 0; Mutation < 22; ++Mutation)
    for (bool SDK : {false, true})
      for (unsigned Route = 0; Route < 3; ++Route) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{Mutation, SDK, Route}));
        auto Input =
            nestedTaskResultClasses(SDK, true, true, true, false, true);
        auto &Provider = Input[0];
        auto &Target = Provider.methods[0];
        auto &Signature = *Target.generic_signature;
        auto replace = [&](const std::string &Before,
                           const std::string &After) {
          const auto At = Signature.find(Before);
          ASSERT_NE(At, std::string::npos);
          Signature.replace(At, Before.size(), After);
        };
        if (Mutation == 0)
          replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
        else if (Mutation == 1)
          replace("Input<+", "Input<-");
        else if (Mutation == 2)
          replace("Task<TT;>", "Task<+TT;>");
        else if (Mutation == 3)
          replace("Task<TT;>", "Task<-TT;>");
        else if (Mutation == 4)
          replace("Task<TT;>", "Task<*>");
        else if (Mutation == 5)
          replace("Task<TT;>", "Task");
        else if (Mutation == 6)
          replace("Input<+Lfixture/Task<TT;>;>", "Input");
        else if (Mutation == 7)
          replace("Task<TT;>", "Task<TMissing;>");
        else if (Mutation == 8)
          replace(")TT;", ")TMissing;");
        else if (Mutation == 9)
          Provider.generic_signature =
              "<Owner:Ljava/lang/Object;>Ljava/lang/Object;";
        else if (Mutation == 10)
          replace("<T:", "<Unused:Ljava/lang/Object;T:");
        else if (Mutation == 11)
          Input[2].generic_signature.reset();
        else if (Mutation == 12)
          Input[3].generic_signature =
              "<Value:Ljava/lang/Number;>Ljava/lang/Object;";
        else if (Mutation == 13)
          Target.access.insert("static");
        else if (Mutation == 14)
          Target.access.insert("native");
        else if (Mutation == 15)
          Target.access.insert("varargs");
        else if (Mutation == 16)
          Target.access.insert("synthetic");
        else if (Mutation == 17)
          Target.reference.parameters[0] = "Lfixture/Output;";
        else if (Mutation == 18)
          Target.reference.returns = "Ljava/lang/String;";
        else if (Mutation == 19)
          Signature += "^TT;";
        else if (Mutation == 20)
          Input.erase(Input.begin() + 6);
        else
          Provider.access.erase("public");
        const auto TargetRef = Target.reference;
        Budget B;
        EXPECT_THROW(
            {
              auto Map = input(std::move(Input), Route, B);
              const auto Plans = validateGenericSignatures(Map, B);
              EXPECT_FALSE(
                  Plans.erased_generic_invocations.contains(TargetRef));
              recoverJava(Map, B);
            },
            std::runtime_error);
      }
}
