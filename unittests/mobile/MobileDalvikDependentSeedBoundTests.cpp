#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>

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
                           unsigned Slots, bool Reverse, bool Swap,
                           bool Throwing) {
  const std::string Object = "Ljava/lang/Object;",
                    Carrier = "Lfixture/Carrier;", Gate = "Lfixture/Gate;",
                    T = Alpha ? "Container" : "T", X = Alpha ? "Failure" : "X";
  std::vector<std::string> Elements;
  for (unsigned I = 0; I < Slots; ++I)
    Elements.push_back((Alpha ? "Element" : "E") + std::to_string(I));
  std::string Arguments = "<";
  for (unsigned I = 0; I < Slots; ++I)
    Arguments += "T" + Elements[Swap ? Slots - I - 1 : I] + ";";
  Arguments += ">;";
  std::vector<std::string> Formals;
  for (const auto &E : Elements)
    Formals.push_back(E + ":Ljava/lang/Object;");
  Formals.push_back(T + (Interface ? "::" : ":") + "Lfixture/Carrier" +
                    Arguments);
  if (Throwing)
    Formals.push_back(X + ":Ljava/lang/Throwable;");
  if (Reverse)
    std::reverse(Formals.begin(), Formals.end());
  std::string Signature = "<";
  for (const auto &F : Formals)
    Signature += F;
  Signature += ">(T" + T + ";";
  std::vector<std::string> Params{Carrier};
  if (Throwing) {
    Params.push_back(Gate);
    Signature += "Lfixture/Gate<T" + X + ";>;";
  } else {
    Params.push_back("Ljava/lang/String;");
    Params.push_back("[Ljava/lang/Object;");
    Signature += "Ljava/lang/String;[Ljava/lang/Object;";
  }
  Signature += ")T" + T + ";";
  if (Throwing)
    Signature += "^T" + X + ";";
  auto Provider = klass("Lfixture/Provider;", SDK);
  auto Target = method({Provider.name, "evaluate", Params, Carrier},
                       {"public", "static"}, SDK ? 0 : Params.size() + 1, {});
  Target.generic_signature = Signature;
  Target.declared_throws = std::vector<std::string>{
      Throwing ? "Ljava/lang/Throwable;"
               : "Ljava/lang/IllegalArgumentException;"};
  if (!Throwing)
    Target.access.insert("varargs");
  FieldRef Value{"Lfixture/Trace;", "result", Carrier};
  if (!SDK) {
    Target.instructions = {op(0, "sget-object", {0}, Value),
                           op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  Provider.methods.push_back(Target);
  auto Core = klass("Lfixture/Core;");
  unsigned Start = Range ? 32 : 2;
  std::vector<unsigned> Registers;
  for (unsigned I = 0; I < Params.size(); ++I)
    Registers.push_back(Start + I);
  Core.methods.push_back(
      method({Core.name, "call", Params, Carrier}, {"public", "static"},
             Start + Params.size(),
             {op(0, Range ? "invoke-static/range" : "invoke-static", Registers,
                 Target.reference),
              op(1, "move-result-object", {0}), op(2, "return-object", {0})}));
  auto Bound = klass(Carrier, SDK);
  if (Interface)
    Bound.access = {"public", "interface", "abstract"};
  std::string Decl = "<";
  for (unsigned I = 0; I < Slots; ++I)
    Decl += "Slot" + std::to_string(I) + ":Ljava/lang/Object;";
  Decl += ">Ljava/lang/Object;";
  Bound.generic_signature = Decl;
  auto Callback = klass(Gate, SDK);
  Callback.access = {"public", "interface", "abstract"};
  Callback.generic_signature = "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Trace = klass(Value.owner, true);
  Trace.fields.push_back({Value, {"public", "static"}, {}});
  auto Root = klass(Object, true);
  Root.superclass.reset();
  auto Exception = klass("Ljava/lang/Throwable;", true);
  auto Failure = klass("Ljava/lang/IllegalArgumentException;", true);
  Failure.superclass = "Ljava/lang/RuntimeException;";
  auto Checked = klass("Ljava/io/IOException;", true);
  Checked.superclass = "Ljava/lang/Exception;";
  auto String = klass("Ljava/lang/String;", true);
  return {Provider, Core,      Bound,   Callback, Trace,
          Root,     Exception, Failure, Checked,  String};
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
TEST(MobileDalvikDependentSeedBound,
     KeepScopedInvariantDependenciesOriginalOrderAndSeparateThrows) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Interface : {false, true})
          for (unsigned Slots : {1u, 2u})
            for (bool Reverse : {false, true})
              for (bool Swap : {false, true})
                for (bool Throwing : {false, true})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, Range, Alpha, Interface, Slots, Reverse,
                                   Swap, Throwing, Route}));
                    ASSERT_NO_THROW(([&] {
                      Budget B;
                      auto Classes =
                          input(fixture(SDK, Range, Alpha, Interface, Slots,
                                        Reverse, Swap, Throwing),
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
                      EXPECT_EQ(Classes.at(Target.reference.owner)
                                    .methods[0]
                                    .declared_throws,
                                Target.declared_throws);
                    })());
                  }
}
TEST(MobileDalvikDependentSeedBound,
     RejectOpenVariantCyclicUnusedAndMismatchedDependencies) {
  for (bool SDK : {false, true})
    for (bool Throwing : {false, true})
      for (unsigned Mutation = 0; Mutation < 16; ++Mutation)
        for (unsigned Route : {0u, 1u, 2u}) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{SDK, Throwing, Mutation, Route}));
          auto Classes =
              fixture(SDK, true, false, true, 2, false, false, Throwing);
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
            Replace("E0:Ljava/lang/Object;", "E0:Ljava/lang/Number;");
            break;
          case 1:
            Replace("Carrier<TE0;", "Carrier<+TE0;");
            break;
          case 2:
            Replace("Carrier<TE0;TE1;>", "Carrier<TE0;TE0;>");
            break;
          case 3:
            Replace("Carrier<TE0;", "Carrier<TMissing;");
            break;
          case 4:
            Replace("(TT;", "(Lfixture/Carrier;");
            break;
          case 5:
            Replace(")TT;", ")TE0;");
            break;
          case 6:
            if (Throwing)
              Target.declared_throws.reset();
            else
              Target.declared_throws =
                  std::vector<std::string>{"Ljava/io/IOException;"};
            break;
          case 7:
            Target.access = {"private", "static"};
            break;
          case 8:
            Target.access.erase("static");
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
            Caller.instructions[0].registers[0] = Caller.registers;
            break;
          case 13:
            Caller.instructions[0].registers.pop_back();
            break;
          case 14:
            Caller.instructions[1].opcode = "move-result-wide";
            break;
          case 15:
            Replace("E0:Ljava/lang/Object;", "E0:TT;");
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
std::vector<Class> dependentMapperFixture(unsigned Dependencies, bool Range,
                                          bool Alpha, bool Reorder,
                                          bool Invariant, bool Used) {
  const std::string T = Alpha ? "Input" : "T", K = Alpha ? "Key" : "K",
                    R = Alpha ? "Element" : "R", C = Alpha ? "Items" : "C",
                    M = Alpha ? "Index" : "M";
  const auto variable = [](const std::string &name) {
    return "T" + name + ";";
  };
  const std::string Mapper = "Lfixture/Mapper;",
                    Producer = "Lfixture/Producer;", Items = "Lfixture/Items;",
                    Index = "Lfixture/Index;", Result = "Lfixture/Result;";
  std::vector<std::string> formals{
      T + ":Ljava/lang/Object;", K + ":Ljava/lang/Object;",
      R + ":Ljava/lang/Object;", C + "::Lfixture/Items<" + variable(R) + ">;",
      M + "::Lfixture/Index<" + variable(K) + variable(C) + ">;"};
  if (Reorder)
    std::reverse(formals.begin(), formals.end());
  std::string signature = "<";
  for (const auto &formal : formals)
    signature += formal;
  signature += ">(";
  for (const auto &value : {K, R})
    signature += "Lfixture/Mapper<" + std::string(Invariant ? "" : "-") +
                 variable(T) + (Invariant ? "" : "+") + variable(value) + ">;";
  signature += "Lfixture/Producer<" + variable(C) + ">;Lfixture/Producer<" +
               variable(M) + ">;)Lfixture/Result<" + variable(T) +
               (Reorder ? "+Ljava/lang/Object;" : "*") + variable(M) + ">;";
  auto provider = klass("Lfixture/Provider;", Dependencies & 1);
  MethodRef target{
      provider.name, "assemble", {Mapper, Mapper, Producer, Producer}, Result};
  const FieldRef value{"Lfixture/Trace;", "result", Result};
  provider.methods.push_back(
      method(target, {"public", "static"}, provider.external ? 0 : 5,
             provider.external
                 ? std::vector<Instruction>{}
                 : std::vector<Instruction>{op(0, "sget-object", {0}, value),
                                            op(2, "return-object", {0})}));
  provider.methods[0].generic_signature = signature;
  auto caller = klass("Lfixture/Core;");
  const unsigned first = Range ? 16 : 1;
  std::vector<Instruction> code{
      op(0, Range ? "invoke-static/range" : "invoke-static",
         {first, first + 1, first + 2, first + 3}, target)};
  if (Used) {
    code.push_back(op(3, "move-result-object", {0}));
    code.push_back(op(4, "return-object", {0}));
  } else
    code.push_back(op(3, "return-void"));
  caller.methods.push_back(
      method({caller.name, "call", target.parameters, Used ? Result : "V"},
             {"public", "static"}, first + 4, std::move(code)));
  auto trace = klass(value.owner);
  Field field;
  field.reference = value;
  field.access = {"public", "static"};
  trace.fields.push_back(field);
  std::vector<Class> classes{caller, provider, trace};
  for (const auto &[name, count] :
       std::vector<std::pair<std::string, unsigned>>{
           {Mapper, 2}, {Producer, 1}, {Items, 1}, {Index, 2}, {Result, 3}}) {
    auto cls = klass(name, Dependencies & 2);
    cls.access = {"public", "abstract", "interface"};
    std::string shape = "<";
    for (unsigned slot = 0; slot < count; ++slot)
      shape += "Slot" + std::to_string(slot) + ":Ljava/lang/Object;";
    cls.generic_signature = shape + ">Ljava/lang/Object;";
    classes.push_back(std::move(cls));
  }
  // Keep nominal negative bounds complete so rejection cannot be explained
  // by a missing Object, Number, exception or marker declaration.
  auto root = klass("Ljava/lang/Object;", true);
  root.superclass.reset();
  classes.push_back(root);
  classes.push_back(klass("Ljava/lang/Number;", true));
  classes.push_back(klass("Ljava/lang/Exception;", true));
  auto marker = klass("Ljava/io/Serializable;", true);
  marker.access = {"public", "abstract", "interface"};
  classes.push_back(marker);
  return classes;
}
} // namespace

TEST(MobileDalvikDependentSeedBound,
     MapperFactoriesRetainChainedInterfaceInputs) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Reorder : {false, true})
          for (bool Invariant : {false, true})
            for (bool Used : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{Dependencies, Range, Alpha, Reorder, Invariant,
                               Used, Route}));
                auto classes = dependentMapperFixture(
                    Dependencies, Range, Alpha, Reorder, Invariant, Used);
                const auto original = classes[1].methods[0];
                Budget B;
                const auto linked = input(classes, Route, B);
                const auto plans = validateGenericSignatures(linked, B);
                EXPECT_TRUE(plans.erased_generic_invocations.contains(
                    original.reference));
                const auto report = recoverJava(linked, B);
                EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(report.getArray("generic_signature_projections"));
                const auto &after =
                    linked.at(original.reference.owner).methods[0];
                EXPECT_EQ(after.generic_signature, original.generic_signature);
                EXPECT_EQ(after.access, original.access);
                EXPECT_EQ(after.declared_throws, original.declared_throws);
              }
}

TEST(MobileDalvikDependentSeedBound,
     MapperFactoriesRejectBrokenChainsDeclarationsAndFrames) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Mutation = 0; Mutation < 28; ++Mutation)
      for (unsigned Route = 1; Route < 3; ++Route) {
        SCOPED_TRACE(::testing::PrintToString(
            std::tuple{Dependencies, Mutation, Route}));
        auto classes = dependentMapperFixture(Dependencies, true, false, false,
                                              false, true);
        auto &provider = classes[1];
        auto &target = provider.methods[0];
        auto &caller = classes[0].methods[0];
        auto replace = [&](const std::string &from, const std::string &to) {
          auto &text = *target.generic_signature;
          const auto at = text.find(from);
          ASSERT_NE(at, std::string::npos);
          text.replace(at, from.size(), to);
        };
        switch (Mutation) {
        case 0:
          replace("R:Ljava/lang/Object;", "R:Ljava/lang/Number;");
          break;
        case 1:
          replace("Items<TR;>", "Items<+TR;>");
          break;
        case 2:
          replace("Index<TK;TC;>", "Index<TC;TK;>");
          break;
        case 3:
          replace("Index<TK;TC;>", "Index<TK;TR;>");
          break;
        case 4:
          replace("Items<TR;>", "Items<TC;>");
          break;
        case 5:
          replace("Items<TR;>", "Items<TMissing;>");
          break;
        case 6:
          replace("Producer<TC;>", "Producer<+TC;>");
          break;
        case 7:
          replace("Producer<TM;>", "Producer<TC;>");
          break;
        case 8:
          replace("Mapper<-TT;+TR;>", "Mapper<-TT;+TK;>");
          break;
        case 9:
          replace("Mapper<-TT;+TK;>", "Mapper<+TT;+TK;>");
          break;
        case 10:
          replace("Result<TT;*TM;>", "Result<TT;*TC;>");
          break;
        case 11:
          replace("Result<TT;*TM;>", "Result<TT;TR;TM;>");
          break;
        case 12:
          classes.erase(classes.begin() + 5);
          break;
        case 13:
          classes[6].access.erase("interface");
          break;
        case 14:
          classes[5].access.erase("public");
          classes[5].name = "Lhidden/Items;";
          replace("Lfixture/Items<", "Lhidden/Items<");
          break;
        case 15:
          classes[6].generic_signature =
              "<A:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 16:
          classes[3].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 17:
          target.access.erase("static");
          break;
        case 18:
          target.access = {"private", "static"};
          break;
        case 19:
          target.access.insert("native");
          break;
        case 20:
          target.access.insert("synthetic");
          break;
        case 21:
          target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 22:
          provider.generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 23:
          caller.instructions[0].registers.pop_back();
          break;
        case 24:
          caller.instructions[0].registers[3] = caller.registers;
          break;
        case 25:
          caller.instructions[1].opcode = "move-result-wide";
          break;
        case 26:
          replace("C::Lfixture/Items", "C:Ljava/lang/Object;:Lfixture/Items");
          break;
        case 27:
          replace("M::Lfixture/Index<TK;TC;>;",
                  "M::Lfixture/Index<TK;TC;>;:Ljava/io/Serializable;");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> producedCollectorFixture(unsigned Dependencies, bool Range,
                                            bool Alpha, bool Reorder,
                                            bool Used) {
  auto classes =
      dependentMapperFixture(Dependencies, Range, Alpha, Reorder, false, Used);
  const std::string element = Alpha ? "Element" : "T";
  const std::string carrier = Alpha ? "Container" : "C";
  const auto variable = [](const std::string &name) {
    return "T" + name + ";";
  };
  const auto first = element + ":Ljava/lang/Object;";
  const auto second = carrier + "::Lfixture/Items<" + variable(element) + ">;";
  auto &target = classes[1].methods[0];
  target.reference.parameters = {"Lfixture/Producer;"};
  target.registers = classes[1].external ? 0 : 2;
  target.generic_signature = "<" + (Reorder ? second + first : first + second) +
                             ">(Lfixture/Producer<" + variable(carrier) +
                             ">;)Lfixture/Result<" + variable(element) +
                             (Reorder ? "+Ljava/lang/Object;" : "*") +
                             variable(carrier) + ">;";
  auto &caller = classes[0].methods[0];
  caller.reference.parameters = target.reference.parameters;
  caller.registers = Range ? 17 : 2;
  caller.instructions[0].registers = {Range ? 16u : 1u};
  caller.instructions[0].reference = target.reference;
  return classes;
}
} // namespace

TEST(MobileDalvikDependentSeedBound,
     ProducedCollectorsRetainDependentInterfaceResults) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Reorder : {false, true})
          for (bool Used : {false, true})
            for (unsigned Route = 0; Route < 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(std::tuple{
                  Dependencies, Range, Alpha, Reorder, Used, Route}));
              auto classes = producedCollectorFixture(Dependencies, Range,
                                                      Alpha, Reorder, Used);
              const auto original = classes[1].methods[0];
              Budget B;
              const auto linked = input(classes, Route, B);
              const auto plans = validateGenericSignatures(linked, B);
              EXPECT_TRUE(plans.erased_generic_invocations.contains(
                  original.reference));
              const auto report = recoverJava(linked, B);
              EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(report.getArray("generic_signature_projections"));
              const auto &after =
                  linked.at(original.reference.owner).methods[0];
              EXPECT_EQ(after.generic_signature, original.generic_signature);
              EXPECT_EQ(after.access, original.access);
              EXPECT_EQ(after.declared_throws, original.declared_throws);
            }
}

TEST(MobileDalvikDependentSeedBound,
     ProducedCollectorsRejectLostBoundsScopesAndPhysicalFrames) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Mutation = 0; Mutation < 27; ++Mutation)
      for (unsigned Route = 1; Route < 3; ++Route) {
        SCOPED_TRACE(::testing::PrintToString(
            std::tuple{Dependencies, Mutation, Route}));
        auto classes =
            producedCollectorFixture(Dependencies, true, false, false, true);
        auto &target = classes[1].methods[0];
        auto &caller = classes[0].methods[0];
        auto replace = [&](const std::string &from, const std::string &to) {
          auto &text = *target.generic_signature;
          const auto at = text.find(from);
          ASSERT_NE(at, std::string::npos);
          text.replace(at, from.size(), to);
        };
        switch (Mutation) {
        case 0:
          replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
          break;
        case 1:
          replace("Items<TT;>", "Items<+TT;>");
          break;
        case 2:
          replace("Items<TT;>", "Items<TMissing;>");
          break;
        case 3:
          replace("Producer<TC;>", "Producer<+TC;>");
          break;
        case 4:
          replace("Result<TT;*TC;>", "Result<TT;*TT;>");
          break;
        case 5:
          replace("Result<TT;*TC;>", "Result<TC;*TC;>");
          break;
        case 6:
          replace("Result<TT;*TC;>", "Result<TT;TT;TC;>");
          break;
        case 7:
          replace("Result<TT;*TC;>", "Result<TT;TC;>");
          break;
        case 8:
          replace("Items<TT;>", "Items<TT;TT;>");
          break;
        case 9:
          target.reference.parameters[0] = "Lfixture/Mapper;";
          caller.reference.parameters = target.reference.parameters;
          caller.instructions[0].reference = target.reference;
          break;
        case 10:
          classes.erase(classes.begin() + 5);
          break;
        case 11:
          classes[5].name = "Lhidden/Items;";
          classes[5].access.erase("public");
          replace("Lfixture/Items<", "Lhidden/Items<");
          break;
        case 12:
          classes[4].generic_signature =
              "<A:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 13:
          classes[7].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 14:
          replace("C::Lfixture/Items", "C:Lfixture/Items");
          break;
        case 15:
          replace("C::Lfixture/Items<TT;>;",
                  "C::Lfixture/Items<TT;>;:Ljava/io/Serializable;");
          break;
        case 16:
          target.access.erase("static");
          break;
        case 17:
          target.access = {"private", "static"};
          break;
        case 18:
          target.access.insert("synthetic");
          break;
        case 19:
          target.access.insert("varargs");
          break;
        case 20:
          target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 21:
          caller.instructions[0].registers.clear();
          break;
        case 22:
          caller.instructions[0].registers[0] = caller.registers;
          break;
        case 23:
          caller.instructions[1].opcode = "move-result-wide";
          break;
        case 24:
          replace("Producer<TC;>", "Producer<TT;>");
          break;
        case 25:
          replace(">(Lfixture/Producer",
                  "X:Ljava/lang/Object;>(Lfixture/Producer");
          break;
        case 26:
          classes[1].generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> groupedCollectorFixture(unsigned Dependencies, bool Range,
                                           bool Alpha, bool Reorder,
                                           bool Invariant, bool Used) {
  auto classes =
      dependentMapperFixture(Dependencies, Range, false, false, false, Used);
  const std::string T = Alpha ? "Input" : "T", K = Alpha ? "Key" : "K",
                    D = Alpha ? "Value" : "D", A = Alpha ? "State" : "A",
                    M = Alpha ? "Index" : "M";
  const auto variable = [](const std::string &name) {
    return "T" + name + ";";
  };
  std::vector<std::string> formals{
      T + ":Ljava/lang/Object;", K + ":Ljava/lang/Object;",
      D + ":Ljava/lang/Object;", A + ":Ljava/lang/Object;",
      M + "::Lfixture/Index<" + variable(K) + variable(D) + ">;"};
  if (Reorder)
    std::reverse(formals.begin(), formals.end());
  std::string signature = "<";
  for (const auto &formal : formals)
    signature += formal;
  signature += ">(Lfixture/Mapper<" + std::string(Invariant ? "" : "-") +
               variable(T) + (Invariant ? "" : "+") + variable(K) +
               ">;Lfixture/Producer<" + variable(M) + ">;Lfixture/Result<" +
               (Invariant ? "" : "-") + variable(T) + variable(A) +
               variable(D) + ">;)Lfixture/Result<" + variable(T) +
               (Reorder ? "+Ljava/lang/Object;" : "*") + variable(M) + ">;";
  auto &target = classes[1].methods[0];
  target.reference.parameters = {"Lfixture/Mapper;", "Lfixture/Producer;",
                                 "Lfixture/Result;"};
  target.generic_signature = signature;
  target.registers = classes[1].external ? 0 : 4;
  auto &caller = classes[0].methods[0];
  const unsigned first = Range ? 16 : 1;
  caller.reference.parameters = target.reference.parameters;
  caller.registers = first + 3;
  caller.instructions[0].reference = target.reference;
  caller.instructions[0].registers = {first, first + 1, first + 2};
  return classes;
}
} // namespace

TEST(MobileDalvikDependentSeedBound,
     GroupedCollectorsRetainIndependentStateAndDependentResults) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Reorder : {false, true})
          for (bool Invariant : {false, true})
            for (bool Used : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{Dependencies, Range, Alpha, Reorder, Invariant,
                               Used, Route}));
                auto classes = groupedCollectorFixture(
                    Dependencies, Range, Alpha, Reorder, Invariant, Used);
                const auto original = classes[1].methods[0];
                Budget B;
                const auto linked = input(classes, Route, B);
                const auto plans = validateGenericSignatures(linked, B);
                EXPECT_TRUE(plans.erased_generic_invocations.contains(
                    original.reference));
                const auto report = recoverJava(linked, B);
                EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(report.getArray("generic_signature_projections"));
                const auto &after =
                    linked.at(original.reference.owner).methods[0];
                EXPECT_EQ(after.generic_signature, original.generic_signature);
                EXPECT_EQ(after.access, original.access);
                EXPECT_EQ(after.declared_throws, original.declared_throws);
              }
}

TEST(MobileDalvikDependentSeedBound,
     GroupedCollectorsRejectLostRolesBoundsAndPhysicalFrames) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Mutation = 0; Mutation < 32; ++Mutation)
      for (unsigned Route = 1; Route < 3; ++Route) {
        SCOPED_TRACE(::testing::PrintToString(
            std::tuple{Dependencies, Mutation, Route}));
        auto classes = groupedCollectorFixture(Dependencies, true, false, false,
                                               false, true);
        auto &provider = classes[1];
        auto &target = provider.methods[0];
        auto &caller = classes[0].methods[0];
        auto replace = [&](const std::string &from, const std::string &to) {
          auto &text = *target.generic_signature;
          const auto at = text.find(from);
          ASSERT_NE(at, std::string::npos);
          text.replace(at, from.size(), to);
        };
        switch (Mutation) {
        case 0:
          replace("D:Ljava/lang/Object;", "D:Ljava/lang/Number;");
          break;
        case 1:
          replace("A:Ljava/lang/Object;", "A:Ljava/lang/Number;");
          break;
        case 2:
          replace("Index<TK;TD;>", "Index<TK;+TD;>");
          break;
        case 3:
          replace("Index<TK;TD;>", "Index<TK;TMissing;>");
          break;
        case 4:
          replace("Producer<TM;>", "Producer<+TM;>");
          break;
        case 5:
          replace("Producer<TM;>", "Producer<TD;>");
          break;
        case 6:
          replace("Mapper<-TT;+TK;>", "Mapper<+TT;+TK;>");
          break;
        case 7:
          replace("Mapper<-TT;+TK;>", "Mapper<-TT;+TD;>");
          break;
        case 8:
          replace("Result<-TT;TA;TD;>", "Result<-TK;TA;TD;>");
          break;
        case 9:
          replace("Result<-TT;TA;TD;>", "Result<+TT;TA;TD;>");
          break;
        case 10:
          replace("Result<-TT;TA;TD;>", "Result<-TT;*TD;>");
          break;
        case 11:
          replace("Result<-TT;TA;TD;>", "Result<-TT;TA;TK;>");
          break;
        case 12:
          replace("Result<TT;*TM;>", "Result<TT;*TD;>");
          break;
        case 13:
          replace("Result<TT;*TM;>", "Result<TT;TA;TM;>");
          break;
        case 14:
          classes.erase(classes.begin() + 6);
          break;
        case 15:
          classes[6].generic_signature =
              "<Slot:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 16:
          classes[6].name = "Lhidden/Index;";
          classes[6].access.erase("public");
          replace("Lfixture/Index<", "Lhidden/Index<");
          break;
        case 17:
          classes[7].generic_signature =
              "<X:Ljava/lang/Object;Y:Ljava/lang/Number;Z:Ljava/lang/"
              "Object;>Ljava/lang/Object;";
          break;
        case 18:
          target.access.erase("static");
          break;
        case 19:
          target.access = {"private", "static"};
          break;
        case 20:
          target.access.insert("native");
          break;
        case 21:
          target.access.insert("synthetic");
          break;
        case 22:
          target.access.insert("varargs");
          break;
        case 23:
          target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 24:
          provider.generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 25:
          caller.instructions[0].registers.pop_back();
          break;
        case 26:
          caller.instructions[0].registers[2] = caller.registers;
          break;
        case 27:
          caller.instructions[1].opcode = "move-result-wide";
          break;
        case 28:
          replace("M::Lfixture/Index", "M:Ljava/lang/Object;:Lfixture/Index");
          break;
        case 29:
          replace("M::Lfixture/Index<TK;TD;>;",
                  "M::Lfixture/Index<TK;TD;>;:Ljava/io/Serializable;");
          break;
        case 30:
          replace(">(Lfixture/Mapper", "X:Ljava/lang/Object;>(Lfixture/Mapper");
          break;
        case 31:
          replace("Lfixture/Result<-TT;TA;TD;>", "Lfixture/Producer<TM;>");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> dependentOperatorFixture(unsigned Dependencies, bool Range,
                                            bool Alpha, bool Reorder,
                                            bool KeyInput, bool Used) {
  auto classes =
      dependentMapperFixture(Dependencies, Range, false, false, false, Used);
  const std::string K = Alpha ? "Key" : "K", V = Alpha ? "Value" : "V",
                    M = Alpha ? "Bounded" : "M";
  const auto variable = [](const std::string &name) {
    return "T" + name + ";";
  };
  std::vector<std::string> formals{
      K + ":Ljava/lang/Object;", V + ":Ljava/lang/Object;",
      M + "::Lfixture/Index<" + variable(K) + variable(V) + ">;"};
  if (Reorder)
    std::reverse(formals.begin(), formals.end());
  std::string signature = "<";
  for (const auto &formal : formals)
    signature += formal;
  signature += ">(Lfixture/Result<" + variable(KeyInput ? K : V) +
               ">;)Lfixture/Result<" + variable(M) + ">;";
  classes[7].generic_signature = "<Slot:Ljava/lang/Object;>Ljava/lang/Object;";
  auto &target = classes[1].methods[0];
  target.reference.parameters = {"Lfixture/Result;"};
  target.generic_signature = signature;
  target.registers = classes[1].external ? 0 : 2;
  auto &caller = classes[0].methods[0];
  caller.reference.parameters = target.reference.parameters;
  caller.registers = Range ? 17 : 2;
  caller.instructions[0].reference = target.reference;
  caller.instructions[0].registers = {Range ? 16u : 1u};
  return classes;
}
} // namespace

TEST(MobileDalvikDependentSeedBound,
     UnaryOperatorsRetainCompleteDependentResultElements) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Reorder : {false, true})
          for (bool KeyInput : {false, true})
            for (bool Used : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{Dependencies, Range, Alpha, Reorder, KeyInput,
                               Used, Route}));
                auto classes = dependentOperatorFixture(
                    Dependencies, Range, Alpha, Reorder, KeyInput, Used);
                const auto original = classes[1].methods[0];
                Budget B;
                const auto linked = input(classes, Route, B);
                const auto plans = validateGenericSignatures(linked, B);
                EXPECT_TRUE(plans.erased_generic_invocations.contains(
                    original.reference));
                const auto report = recoverJava(linked, B);
                EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(report.getArray("generic_signature_projections"));
                const auto &after =
                    linked.at(original.reference.owner).methods[0];
                EXPECT_EQ(after.generic_signature, original.generic_signature);
                EXPECT_EQ(after.access, original.access);
                EXPECT_EQ(after.declared_throws, original.declared_throws);
              }
}

TEST(MobileDalvikDependentSeedBound,
     UnaryOperatorsRejectOpenElementsBoundsAndPhysicalFrames) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Mutation = 0; Mutation < 32; ++Mutation)
      for (unsigned Route = 1; Route < 3; ++Route) {
        SCOPED_TRACE(::testing::PrintToString(
            std::tuple{Dependencies, Mutation, Route}));
        auto classes = dependentOperatorFixture(Dependencies, true, false,
                                                false, false, true);
        auto &provider = classes[1];
        auto &target = provider.methods[0];
        auto &caller = classes[0].methods[0];
        auto replace = [&](const std::string &from, const std::string &to) {
          auto &text = *target.generic_signature;
          const auto at = text.find(from);
          ASSERT_NE(at, std::string::npos);
          text.replace(at, from.size(), to);
        };
        switch (Mutation) {
        case 0:
          replace("K:Ljava/lang/Object;", "K:Ljava/lang/Number;");
          break;
        case 1:
          replace("V:Ljava/lang/Object;", "V:Ljava/lang/Number;");
          break;
        case 2:
          replace("Index<TK;TV;>", "Index<TK;+TV;>");
          break;
        case 3:
          replace("Index<TK;TV;>", "Index<TK;TK;>");
          break;
        case 4:
          replace("Index<TK;TV;>", "Index<TK;TMissing;>");
          break;
        case 5:
          replace("(Lfixture/Result<TV;>", "(Lfixture/Result<TM;>");
          break;
        case 6:
          replace("(Lfixture/Result<TV;>", "(Lfixture/Result<+TV;>");
          break;
        case 7:
          replace(")Lfixture/Result<TM;>", ")Lfixture/Result<+TM;>");
          break;
        case 8:
          replace(")Lfixture/Result<TM;>", ")Lfixture/Result<TV;>");
          break;
        case 9:
          replace(")Lfixture/Result<TM;>", ")Lfixture/Result<TM;TV;>");
          break;
        case 10:
          classes.erase(classes.begin() + 6);
          break;
        case 11:
          classes[6].generic_signature =
              "<Slot:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 12:
          classes[6].access.erase("interface");
          break;
        case 13:
          classes[6].name = "Lhidden/Index;";
          classes[6].access.erase("public");
          replace("Lfixture/Index<", "Lhidden/Index<");
          break;
        case 14:
          classes[7].generic_signature =
              "<Slot:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 15:
          replace("(Lfixture/Result<TV;>", "(Lfixture/Producer<TV;>");
          target.reference.parameters[0] = "Lfixture/Producer;";
          caller.reference.parameters = target.reference.parameters;
          caller.instructions[0].reference = target.reference;
          break;
        case 16:
          target.access = {"private", "static"};
          break;
        case 17:
          target.access.erase("static");
          break;
        case 18:
          target.access.insert("native");
          break;
        case 19:
          target.access.insert("synthetic");
          break;
        case 20:
          target.access.insert("varargs");
          break;
        case 21:
          target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 22:
          provider.generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 23:
          caller.instructions[0].registers.clear();
          break;
        case 24:
          caller.instructions[0].registers[0] = caller.registers;
          break;
        case 25:
          caller.instructions[1].opcode = "move-result-wide";
          break;
        case 26:
          replace("M::Lfixture/Index", "M:Ljava/lang/Object;:Lfixture/Index");
          break;
        case 27:
          replace("M::Lfixture/Index<TK;TV;>;",
                  "M::Lfixture/Index<TK;TV;>;:Ljava/io/Serializable;");
          break;
        case 28:
          replace(">(Lfixture/Result", "X:Ljava/lang/Object;>(Lfixture/Result");
          break;
        case 29:
          replace("(Lfixture/Result<TV;>",
                  "(Lfixture/Result<Ljava/lang/Number;>");
          break;
        case 30:
          replace("Index<TK;TV;>", "Index<TK;TM;>");
          break;
        case 31:
          replace("(Lfixture/Result<TV;>", "(Lfixture/Result<*>");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> nestedProducedCollectorFixture(unsigned Dependencies,
                                                  bool Range, bool Alpha,
                                                  bool Reorder, bool NestedKey,
                                                  bool Swap, bool Used) {
  auto classes =
      dependentMapperFixture(Dependencies, Range, Alpha, Reorder, false, Used);
  const std::string key = Alpha ? "Key" : "K";
  const std::string value = Alpha ? "Value" : "V";
  const std::string carrier = Alpha ? "Container" : "M";
  const auto variable = [](const std::string &name) {
    return "T" + name + ";";
  };
  std::vector<std::string> order{key, value};
  if (Swap)
    std::reverse(order.begin(), order.end());
  std::string bound = "Lfixture/Index<";
  for (unsigned slot = 0; slot < 2; ++slot)
    bound += slot == (NestedKey ? 0u : 1u)
                 ? "Lfixture/Items<" + variable(order[slot]) + ">;"
                 : variable(order[slot]);
  bound += ">;";
  std::vector<std::string> formals{key + ":Ljava/lang/Object;",
                                   value + ":Ljava/lang/Object;",
                                   carrier + "::" + bound};
  if (Reorder)
    std::reverse(formals.begin(), formals.end());
  std::string signature = "<";
  for (const auto &formal : formals)
    signature += formal;
  signature += ">(Lfixture/Producer<" + variable(carrier) +
               ">;)Lfixture/Result<Lfixture/Index<" + variable(order[0]) +
               variable(order[1]) + ">;" +
               (Reorder ? "+Ljava/lang/Object;" : "*") + variable(carrier) +
               ">;";
  auto &target = classes[1].methods[0];
  target.reference.parameters = {"Lfixture/Producer;"};
  target.generic_signature = signature;
  target.registers = classes[1].external ? 0 : 2;
  auto &caller = classes[0].methods[0];
  caller.reference.parameters = target.reference.parameters;
  caller.registers = Range ? 17 : 2;
  caller.instructions[0].registers = {Range ? 16u : 1u};
  caller.instructions[0].reference = target.reference;
  return classes;
}
} // namespace

TEST(MobileDalvikDependentSeedBound,
     NestedProducedCollectorsRetainOrderedWrappedDependencies) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Reorder : {false, true})
          for (bool NestedKey : {false, true})
            for (bool Swap : {false, true})
              for (bool Used : {false, true})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{Dependencies, Range, Alpha, Reorder, NestedKey,
                                 Swap, Used, Route}));
                  auto classes = nestedProducedCollectorFixture(
                      Dependencies, Range, Alpha, Reorder, NestedKey, Swap,
                      Used);
                  const auto original = classes[1].methods[0];
                  Budget B;
                  const auto linked = input(classes, Route, B);
                  const auto plans = validateGenericSignatures(linked, B);
                  EXPECT_TRUE(plans.erased_generic_invocations.contains(
                      original.reference));
                  const auto report = recoverJava(linked, B);
                  EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(
                      report.getArray("generic_signature_projections"));
                  const auto &after =
                      linked.at(original.reference.owner).methods[0];
                  EXPECT_EQ(after.generic_signature,
                            original.generic_signature);
                  EXPECT_EQ(after.access, original.access);
                  EXPECT_EQ(after.declared_throws, original.declared_throws);
                }
}

TEST(MobileDalvikDependentSeedBound,
     NestedProducedCollectorsRejectLostEdgesDeclarationsAndFrames) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Mutation = 0; Mutation < 43; ++Mutation)
      for (unsigned Route = 1; Route < 3; ++Route) {
        SCOPED_TRACE(::testing::PrintToString(
            std::tuple{Dependencies, Mutation, Route}));
        auto classes = nestedProducedCollectorFixture(
            Dependencies, true, false, false, false, false, true);
        auto &target = classes[1].methods[0];
        auto &caller = classes[0].methods[0];
        auto replace = [&](const std::string &from, const std::string &to) {
          auto &text = *target.generic_signature;
          const auto at = text.find(from);
          ASSERT_NE(at, std::string::npos);
          text.replace(at, from.size(), to);
        };
        switch (Mutation) {
        case 0:
          replace("K:Ljava/lang/Object;", "K:Ljava/lang/Number;");
          break;
        case 1:
          replace("V:Ljava/lang/Object;", "V:Ljava/lang/Number;");
          break;
        case 2:
          replace("Items<TV;>", "Items<+TV;>");
          break;
        case 3:
          replace("Items<TV;>", "Items<TMissing;>");
          break;
        case 4:
          replace("Items<TV;>", "Items<TK;>");
          break;
        case 5:
          replace("Index<TK;Lfixture/Items", "Index<+TK;Lfixture/Items");
          break;
        case 6:
          replace("Index<TK;TV;>", "Index<TV;TK;>");
          break;
        case 7:
          replace("Items<TV;>", "Items<TV;TK;>");
          break;
        case 8:
          replace("Producer<TM;>", "Producer<+TM;>");
          break;
        case 9:
          replace("Producer<TM;>", "Producer<TK;>");
          break;
        case 10:
          replace("Result<Lfixture/Index", "Result<+Lfixture/Index");
          break;
        case 11:
          replace(";*TM;>", ";TK;TM;>");
          break;
        case 12:
          replace(";*TM;>", ";*+TM;>");
          break;
        case 13:
          replace(";*TM;>", ";*TK;>");
          break;
        case 14:
          replace("M::Lfixture/Index", "M:Lfixture/Index");
          break;
        case 15:
          replace(";>;>(", ";>;:Ljava/io/Serializable;>(");
          break;
        case 16:
          replace("V:Ljava/lang/Object;", "");
          break;
        case 17:
          replace(">(Lfixture/Producer",
                  "X:Ljava/lang/Object;>(Lfixture/Producer");
          break;
        case 18:
          classes.erase(classes.begin() + 5);
          break;
        case 19:
          classes.erase(classes.begin() + 6);
          break;
        case 20:
          classes[5].generic_signature =
              "<A:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 21:
          classes[6].generic_signature =
              "<A:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 22:
          classes[7].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 23:
          classes[4].generic_signature =
              "<A:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 24:
          target.reference.parameters[0] = "Lfixture/Mapper;";
          caller.reference.parameters = target.reference.parameters;
          caller.instructions[0].reference = target.reference;
          break;
        case 25:
          target.reference.returns = "Lfixture/Mapper;";
          caller.reference.returns = target.reference.returns;
          caller.instructions[0].reference = target.reference;
          break;
        case 26:
          target.access = {"private", "static"};
          break;
        case 27:
          target.access.erase("static");
          break;
        case 28:
          target.access.insert("synthetic");
          break;
        case 29:
          target.access.insert("varargs");
          break;
        case 30:
          target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 31:
          classes[1].generic_signature =
              "<K:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 32:
          caller.instructions[0].registers.clear();
          break;
        case 33:
          caller.instructions[0].registers[0] = caller.registers;
          break;
        case 34:
          caller.instructions[1].opcode = "move-result-wide";
          break;
        case 35:
          replace("M::Lfixture/Index", "M::Lfixture/Mapper");
          break;
        case 36:
          replace("Lfixture/Items<TV;>;", "TV;");
          break;
        case 37:
          replace("Index<TK;Lfixture/Items",
                  "Index<Lfixture/Items<TK;>;Lfixture/Items");
          break;
        case 38:
          replace("Index<TK;Lfixture/Items", "Index<TK;+Lfixture/Items");
          break;
        case 39:
          replace("Items<TV;>", "Items");
          break;
        case 40:
          replace("Items<TV;>", "Items<Lfixture/Items<TV;>;>");
          break;
        case 41:
          classes[6].access.erase("interface");
          break;
        case 42:
          classes[5].name = "Lhidden/Items;";
          classes[5].access.erase("public");
          replace("Lfixture/Items<", "Lhidden/Items<");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> mappedReducerFixture(unsigned Dependencies, bool Range,
                                        bool Alpha, bool Reorder,
                                        bool Invariant, unsigned Layout,
                                        bool Used) {
  auto classes = dependentMapperFixture(Dependencies, Range, Alpha, Reorder,
                                        Invariant, Used);
  const std::string element = Alpha ? "Element" : "T";
  const std::string state = Alpha ? "State" : "U";
  const auto variable = [](const std::string &name) {
    return "T" + name + ";";
  };
  std::vector<std::string> formals{element + ":Ljava/lang/Object;",
                                   state + ":Ljava/lang/Object;"};
  if (Reorder)
    std::reverse(formals.begin(), formals.end());
  std::string signature = "<" + formals[0] + formals[1] + ">(";
  std::vector<std::string> arguments{
      variable(state),
      "Lfixture/Mapper<" + std::string(Invariant ? "" : "-") +
          variable(element) + (Invariant ? "" : "+") + variable(state) + ">;",
      "Lfixture/Producer<" + variable(state) + ">;"};
  const std::vector<std::string> descriptors{
      "Ljava/lang/Object;", "Lfixture/Mapper;", "Lfixture/Producer;"};
  std::vector<unsigned> order{0, 1, 2};
  std::rotate(order.begin(), order.begin() + Layout / 2, order.end());
  if (Layout & 1)
    std::reverse(order.begin(), order.end());
  auto &target = classes[1].methods[0];
  target.reference.parameters.clear();
  for (auto slot : order) {
    target.reference.parameters.push_back(descriptors[slot]);
    signature += arguments[slot];
  }
  target.generic_signature =
      signature + ")Lfixture/Result<" + variable(element) +
      (Reorder ? "+Ljava/lang/Object;" : "*") + variable(state) + ">;";
  target.registers = classes[1].external ? 0 : 4;
  auto &caller = classes[0].methods[0];
  caller.reference.parameters = target.reference.parameters;
  const unsigned first = Range ? 16 : 1;
  caller.registers = first + 3;
  caller.instructions[0].registers = {first, first + 1, first + 2};
  caller.instructions[0].reference = target.reference;
  return classes;
}
} // namespace

TEST(MobileDalvikDependentSeedBound,
     MappedReducersRetainDistinctElementAndStateRoles) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Reorder : {false, true})
          for (bool Invariant : {false, true})
            for (unsigned Layout = 0; Layout < 6; ++Layout)
              for (bool Used : {false, true})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{Dependencies, Range, Alpha, Reorder, Invariant,
                                 Layout, Used, Route}));
                  auto classes =
                      mappedReducerFixture(Dependencies, Range, Alpha, Reorder,
                                           Invariant, Layout, Used);
                  const auto original = classes[1].methods[0];
                  Budget B;
                  const auto linked = input(classes, Route, B);
                  const auto plans = validateGenericSignatures(linked, B);
                  EXPECT_TRUE(plans.erased_generic_invocations.contains(
                      original.reference));
                  const auto report = recoverJava(linked, B);
                  EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(
                      report.getArray("generic_signature_projections"));
                  const auto &after =
                      linked.at(original.reference.owner).methods[0];
                  EXPECT_EQ(after.generic_signature,
                            original.generic_signature);
                  EXPECT_EQ(after.access, original.access);
                  EXPECT_EQ(after.declared_throws, original.declared_throws);
                }
}

TEST(MobileDalvikDependentSeedBound,
     MappedReducersRejectLostMappingsBoundsAndFrames) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Mutation = 0; Mutation < 43; ++Mutation)
      for (unsigned Route = 1; Route < 3; ++Route) {
        SCOPED_TRACE(::testing::PrintToString(
            std::tuple{Dependencies, Mutation, Route}));
        auto classes = mappedReducerFixture(Dependencies, true, false, false,
                                            false, 0, true);
        auto &target = classes[1].methods[0];
        auto &caller = classes[0].methods[0];
        auto replace = [&](const std::string &from, const std::string &to) {
          auto &text = *target.generic_signature;
          const auto at = text.find(from);
          ASSERT_NE(at, std::string::npos);
          text.replace(at, from.size(), to);
        };
        switch (Mutation) {
        case 0:
          replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
          break;
        case 1:
          replace("U:Ljava/lang/Object;", "U:Ljava/lang/Number;");
          break;
        case 2:
          replace("Mapper<-TT;+TU;>", "Mapper<+TT;+TU;>");
          break;
        case 3:
          replace("Mapper<-TT;+TU;>", "Mapper<-TT;-TU;>");
          break;
        case 4:
          replace(">(TU;", ">(TT;");
          break;
        case 5:
          replace("Producer<TU;>", "Producer<TT;>");
          break;
        case 6:
          replace("Mapper<-TT;+TU;>", "Mapper<-TT;+TT;>");
          break;
        case 7:
          replace("Mapper<-TT;+TU;>", "Mapper<-TU;+TU;>");
          break;
        case 8:
          replace("Producer<TU;>", "Producer<+TU;>");
          break;
        case 9:
          replace("Result<TT;*TU;>", "Result<+TT;*TU;>");
          break;
        case 10:
          replace("Result<TT;*TU;>", "Result<TT;*+TU;>");
          break;
        case 11:
          replace("Result<TT;*TU;>", "Result<TT;TU;TU;>");
          break;
        case 12:
          replace("Result<TT;*TU;>", "Result<TT;*TT;>");
          break;
        case 13:
          replace("Result<TT;*TU;>", "Result<TU;*TU;>");
          break;
        case 14:
          replace("Mapper<-TT;+TU;>", "Mapper<-TMissing;+TU;>");
          break;
        case 15:
          replace("U:Ljava/lang/Object;", "");
          break;
        case 16:
          replace(">(TU;", "X:Ljava/lang/Object;>(TU;");
          break;
        case 17:
          replace("Mapper<-TT;+TU;>", "Mapper<-TT;>");
          break;
        case 18:
          replace("Producer<TU;>", "Producer<TT;TU;>");
          break;
        case 19:
          replace("Result<TT;*TU;>", "Result<TT;TU;>");
          break;
        case 20:
          classes.erase(classes.begin() + 3);
          break;
        case 21:
          classes.erase(classes.begin() + 4);
          break;
        case 22:
          classes.erase(classes.begin() + 7);
          break;
        case 23:
          classes[3].generic_signature =
              "<A:Ljava/lang/Number;B:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 24:
          classes[4].generic_signature =
              "<A:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 25:
          classes[7].generic_signature =
              "<A:Ljava/lang/Number;B:Ljava/lang/Object;C:Ljava/lang/"
              "Object;>Ljava/lang/Object;";
          break;
        case 26:
          classes[3].name = "Lhidden/Mapper;";
          classes[3].access.erase("public");
          replace("Lfixture/Mapper<", "Lhidden/Mapper<");
          target.reference.parameters[1] = classes[3].name;
          caller.reference.parameters = target.reference.parameters;
          caller.instructions[0].reference = target.reference;
          break;
        case 27:
          target.reference.parameters[0] = "Lfixture/Items;";
          caller.reference.parameters = target.reference.parameters;
          caller.instructions[0].reference = target.reference;
          break;
        case 28:
          target.reference.returns = "Ljava/lang/Object;";
          caller.reference.returns = target.reference.returns;
          caller.instructions[0].reference = target.reference;
          break;
        case 29:
          target.access = {"private", "static"};
          break;
        case 30:
          target.access.erase("static");
          break;
        case 31:
          target.access.insert("synthetic");
          break;
        case 32:
          target.access.insert("varargs");
          break;
        case 33:
          target.access.insert("native");
          break;
        case 34:
          target.access.insert("bridge");
          break;
        case 35:
          target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 36:
          classes[1].generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 37:
          caller.instructions[0].registers.clear();
          break;
        case 38:
          caller.instructions[0].registers[0] = caller.registers;
          break;
        case 39:
          caller.instructions[1].opcode = "move-result-wide";
          break;
        case 40:
          replace("Mapper<-TT;+TU;>", "Mapper<*+TU;>");
          break;
        case 41:
          replace("Producer<TU;>", "Producer");
          break;
        case 42:
          *target.generic_signature += "^Ljava/lang/Exception;";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> mergingCollectorFixture(unsigned Dependencies, bool Range,
                                           bool Alpha, bool Reorder,
                                           bool Invariant, bool Swap,
                                           bool Used) {
  auto classes = dependentMapperFixture(Dependencies, Range, Alpha, Reorder,
                                        Invariant, Used);
  const std::string input = Alpha ? "Input" : "T";
  const std::string first = Alpha ? "Key" : "K";
  const std::string second = Alpha ? "Value" : "V";
  const std::string outer = Alpha ? "Index" : "M";
  const auto key = Swap ? second : first;
  const auto value = Swap ? first : second;
  const auto variable = [](const std::string &name) {
    return "T" + name + ";";
  };
  std::vector<std::string> formals{
      input + ":Ljava/lang/Object;", first + ":Ljava/lang/Object;",
      second + ":Ljava/lang/Object;",
      outer + "::Lfixture/Index<" + variable(key) + variable(value) + ">;"};
  if (Reorder)
    std::reverse(formals.begin(), formals.end());
  std::string signature = "<";
  for (const auto &formal : formals)
    signature += formal;
  signature += ">(";
  for (const auto &role : {key, value})
    signature += "Lfixture/Mapper<" + std::string(Invariant ? "" : "-") +
                 variable(input) + (Invariant ? "" : "+") + variable(role) +
                 ">;";
  signature += "Lfixture/Items<" + variable(value) + ">;Lfixture/Producer<" +
               variable(outer) + ">;)Lfixture/Result<" + variable(input) +
               (Reorder ? "+Ljava/lang/Object;" : "*") + variable(outer) + ">;";
  auto &target = classes[1].methods[0];
  target.generic_signature = signature;
  target.reference.parameters = {"Lfixture/Mapper;", "Lfixture/Mapper;",
                                 "Lfixture/Items;", "Lfixture/Producer;"};
  auto &caller = classes[0].methods[0];
  caller.reference.parameters = target.reference.parameters;
  caller.instructions[0].reference = target.reference;
  return classes;
}
} // namespace

TEST(MobileDalvikDependentSeedBound,
     MergingCollectorsRetainCompleteValueAndMapRoles) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Reorder : {false, true})
          for (bool Invariant : {false, true})
            for (bool Swap : {false, true})
              for (bool Used : {false, true})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{Dependencies, Range, Alpha, Reorder, Invariant,
                                 Swap, Used, Route}));
                  auto classes =
                      mergingCollectorFixture(Dependencies, Range, Alpha,
                                              Reorder, Invariant, Swap, Used);
                  const auto original = classes[1].methods[0];
                  Budget B;
                  const auto linked = input(classes, Route, B);
                  const auto plans = validateGenericSignatures(linked, B);
                  EXPECT_TRUE(plans.erased_generic_invocations.contains(
                      original.reference));
                  const auto report = recoverJava(linked, B);
                  EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(
                      report.getArray("generic_signature_projections"));
                  const auto &after =
                      linked.at(original.reference.owner).methods[0];
                  EXPECT_EQ(after.generic_signature,
                            original.generic_signature);
                  EXPECT_EQ(after.access, original.access);
                  EXPECT_EQ(after.declared_throws, original.declared_throws);
                }
}

TEST(MobileDalvikDependentSeedBound,
     MergingCollectorsRejectLostDependenciesRolesAndFrames) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Mutation = 0; Mutation < 46; ++Mutation)
      for (unsigned Route = 1; Route < 3; ++Route) {
        SCOPED_TRACE(::testing::PrintToString(
            std::tuple{Dependencies, Mutation, Route}));
        auto classes = mergingCollectorFixture(Dependencies, true, false, false,
                                               false, false, true);
        auto &target = classes[1].methods[0];
        auto &caller = classes[0].methods[0];
        auto replace = [&](const std::string &from, const std::string &to) {
          auto &text = *target.generic_signature;
          const auto at = text.find(from);
          ASSERT_NE(at, std::string::npos);
          text.replace(at, from.size(), to);
        };
        switch (Mutation) {
        case 0:
          replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
          break;
        case 1:
          replace("K:Ljava/lang/Object;", "K:Ljava/lang/Number;");
          break;
        case 2:
          replace("V:Ljava/lang/Object;", "V:Ljava/lang/Number;");
          break;
        case 3:
          replace("Index<TK;TV;>", "Index<+TK;TV;>");
          break;
        case 4:
          replace("Index<TK;TV;>", "Index<TK;+TV;>");
          break;
        case 5:
          replace("M::Lfixture/Index", "M:Lfixture/Index");
          break;
        case 6:
          replace("M::Lfixture/Index<TK;TV;>;",
                  "M::Lfixture/Index<TK;TV;>;:Ljava/io/Serializable;");
          break;
        case 7:
          replace("Index<TK;TV;>", "Index<TK;TT;>");
          break;
        case 8:
          replace("Index<TK;TV;>", "Index<TV;TK;>");
          break;
        case 9:
          replace("Mapper<-TT;+TK;>", "Mapper<-TT;+TV;>");
          break;
        case 10:
          replace("Mapper<-TT;+TV;>", "Mapper<-TT;+TK;>");
          break;
        case 11:
          replace("Mapper<-TT;+TK;>", "Mapper<-TV;+TK;>");
          break;
        case 12:
          replace("Mapper<-TT;+TK;>", "Mapper<+TT;+TK;>");
          break;
        case 13:
          replace("Mapper<-TT;+TV;>", "Mapper<-TT;-TV;>");
          break;
        case 14:
          replace("Items<TV;>", "Items<TK;>");
          break;
        case 15:
          replace("Items<TV;>", "Items<+TV;>");
          break;
        case 16:
          replace("Producer<TM;>", "Producer<+TM;>");
          break;
        case 17:
          replace("Producer<TM;>", "Producer<TV;>");
          break;
        case 18:
          replace("Result<TT;*TM;>", "Result<+TT;*TM;>");
          break;
        case 19:
          replace("Result<TT;*TM;>", "Result<TT;*+TM;>");
          break;
        case 20:
          replace("Result<TT;*TM;>", "Result<TT;TV;TM;>");
          break;
        case 21:
          replace("Result<TT;*TM;>", "Result<TT;*TV;>");
          break;
        case 22:
          replace("Result<TT;*TM;>", "Result<TK;*TM;>");
          break;
        case 23:
          replace("Index<TK;TV;>", "Index<TK;TMissing;>");
          break;
        case 24:
          replace(">(Lfixture/Mapper", "X:Ljava/lang/Object;>(Lfixture/Mapper");
          break;
        case 25:
          classes[3].generic_signature =
              "<A:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 26:
          classes[5].generic_signature =
              "<A:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 27:
          classes[4].generic_signature =
              "<A:Ljava/lang/Number;>Ljava/lang/Object;";
          break;
        case 28:
          classes[6].generic_signature =
              "<A:Ljava/lang/Number;B:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 29:
          classes[7].generic_signature =
              "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 30:
          classes.erase(classes.begin() + 6);
          break;
        case 31:
          classes.erase(classes.begin() + 5);
          break;
        case 32:
          classes[6].name = "Lhidden/Index;";
          classes[6].access.erase("public");
          replace("Lfixture/Index<", "Lhidden/Index<");
          break;
        case 33:
          target.reference.parameters[2] = "Lfixture/Mapper;";
          caller.reference.parameters = target.reference.parameters;
          caller.instructions[0].reference = target.reference;
          break;
        case 34:
          target.access = {"private", "static"};
          break;
        case 35:
          target.access.erase("static");
          break;
        case 36:
          target.access.insert("synthetic");
          break;
        case 37:
          target.access.insert("varargs");
          break;
        case 38:
          target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 39:
          classes[1].generic_signature =
              "<T:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 40:
          caller.instructions[0].registers.pop_back();
          break;
        case 41:
          caller.instructions[0].registers[0] = caller.registers;
          break;
        case 42:
          caller.instructions[1].opcode = "move-result-wide";
          break;
        case 43:
          replace("Lfixture/Producer<TM;>;", "");
          target.reference.parameters.pop_back();
          target.registers = classes[1].external ? 0 : 4;
          caller.reference.parameters = target.reference.parameters;
          caller.instructions[0].reference = target.reference;
          caller.instructions[0].registers = {16, 17, 18};
          caller.registers = 19;
          break;
        case 44:
          target.access.insert("native");
          break;
        case 45:
          target.access.insert("bridge");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
